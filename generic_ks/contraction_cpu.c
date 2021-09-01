/********************** contraction_cpu.c *********************************/
/* MIMD version 7 */

/* Taken from ks_meson_mom.c to emulate a call to QUDA */
/* CPU version of the GPU KS contraction code */

#include "generic_ks_includes.h"
#include <string.h>
#include "../include/openmp_defs.h"
#ifdef OMP
#include <omp.h>
#endif

/* Calculate FT weight factor */

#define EVEN 0x02
#define ODD 0x01
#define EVENANDODD 0x03

/*******************************************/
static complex **
create_meson_q_thread(int nt, int max_threads, int num_corr_mom){
  char myname[] = "create_meson_q_thread";
  
  complex ** meson_q_thread = (complex **)malloc(max_threads*nt*sizeof(complex *));
  
  for(int mythread=0; mythread<max_threads; mythread++) {
    for(int t = 0; t < nt; t++){
      meson_q_thread[mythread*nt+t] = (complex *)malloc(num_corr_mom*sizeof(complex));
      if(meson_q_thread[mythread*nt+t] == NULL){
	printf("%s(%d): No room for meson_q_thread array\n",myname,this_node);
      }
      for(int k=0; k<num_corr_mom; k++)
	{   
	  meson_q_thread[mythread*nt+t][k].real = 0.;
	  meson_q_thread[mythread*nt+t][k].imag = 0.;
	}
    }
  }
  
  return meson_q_thread;
}

/*******************************************/
static void
destroy_meson_q_thread(complex **meson_q_thread, int nt, int max_threads){
  if(meson_q_thread == NULL)return;
  for(int mythread=0; mythread<max_threads; mythread++) {
    for(int t = 0; t < nt; t++){
      if(meson_q_thread[mythread*nt+t] != NULL)
	free(meson_q_thread[mythread*nt+t]);
    }
  }
}

/*******************************************/
static Real
sum_meson_q(complex **meson_q, complex **meson_q_thread, int nonzero[],
	    int max_threads, int nt, int num_corr_mom){

  for(int mythread=0; mythread<max_threads; mythread++) {
    for(int t = 0; t < nt; t++)if(nonzero[t]){
      for(int k=0; k<num_corr_mom; k++)
	{
	  meson_q[t][k].real += meson_q_thread[mythread*nt+t][k].real;
	  meson_q[t][k].imag += meson_q_thread[mythread*nt+t][k].imag;
	  meson_q_thread[mythread*nt+t][k].real =
	    meson_q_thread[mythread*nt+t][k].imag = 0.; // Prevent re-add
	}
    }
  }

  Real flops = (Real)sites_on_node*8*num_corr_mom;
  return flops;
}
      
/*******************************************/
/* Calculate a single Fourier phase factor */
static complex ff(Real theta, char parity, complex tmp)
{
  complex z = {0.,0.};
  
  if(parity == EVEN){
    z.real = tmp.real*cos(theta);
    z.imag = tmp.imag*cos(theta);
  }
  else if(parity == ODD){
    z.real = -tmp.imag*sin(theta);
    z.imag =  tmp.real*sin(theta);
  }
  else if(parity == EVENANDODD){
    z.real = tmp.real*cos(theta)-tmp.imag*sin(theta);
    z.imag = tmp.imag*cos(theta)+tmp.real*sin(theta);
  }
  else{
    printf("ff(%d): bad parity %d\n", this_node, parity);
    terminate(1);
  }
  return z;
} /* ff */

/*******************************************/
/* Create a table of Fourier phases, one for each momentum for each site */

static complex *
create_ftfact(int nx, int ny, int nz, int nt, int num_corr_mom,
	      int **corr_mom, char **corr_parity, int *r0, Real *flops){

  double factx = 2.0*PI/(1.0*nx) ; 
  double facty = 2.0*PI/(1.0*ny) ; 
  double factz = 2.0*PI/(1.0*nz) ; 

  complex *ftfact = (complex *)malloc(num_corr_mom*sites_on_node*sizeof(complex));
  if(ftfact == NULL)
    {
      printf("(%d): No room for FT phases\n",this_node);
      terminate(1);
    }
  
  /* ftfact contains factors such as cos(kx*x)*sin(ky*y)*exp(ikz*z)
     with factors of cos, sin, and exp selected according to the
     requested component parity */
  
  int i;
  site *s;
  FORALLSITES_OMP(i,s,) {
    for(int k=0; k<num_corr_mom; k++)
      {
	int px = corr_mom[k][0];
	int py = corr_mom[k][1];
	int pz = corr_mom[k][2];
	
	char ex = corr_parity[k][0];
	char ey = corr_parity[k][1];
	char ez = corr_parity[k][2];

	complex tmp;
	
	tmp.real = 1.;
	tmp.imag = 0.;
	
	tmp = ff(factx*(s->x-r0[0])*px, ex, tmp);
	tmp = ff(facty*(s->y-r0[1])*py, ey, tmp);
	tmp = ff(factz*(s->z-r0[2])*pz, ez, tmp);
	
	ftfact[k+num_corr_mom*i] = tmp;
      }
  } END_LOOP_OMP;
  
  *flops += (Real)sites_on_node*18*num_corr_mom;
  return ftfact;
}
  
/*******************************************/

static void
destroy_ftfact(complex *ftfact ){
  if(ftfact != NULL)
    free(ftfact);
}

/*******************************************/
/* Put this in an appropriate header */

typedef struct {
  int num_corr_mom;  /* Number of sink momenta */
  int **corr_mom;  /* List of momenta as integers */
  char **corr_parity; /* The "parity" of the FT component */
  int *r0;  /* The coordinate origin for the Fourier phases */
  Real flops; /* Return value */
  Real dtime; /* Return value */
} QudaContractArgs_t;

void qudaContract(int milc_precision,
		  int quda_precision,
		  QudaContractArgs_t *cont_args,
		  su3_vector *antiquark,  /* Color vector field (propagator) */
		  su3_vector *quark,   /* Color vector field (propagator) */
                  complex *meson_q[]  /* Resulting hadron correlator indexed by time and momentum */
		  )
{

  Real dtime = -dclock();
  char myname[] = "qudaContract";
  Real flops = 0;

  int num_corr_mom = cont_args->num_corr_mom;
  int **corr_mom = cont_args->corr_mom;
  char **corr_parity = cont_args->corr_parity;
  int *r0 = cont_args->r0;

#ifdef OMP
  /* max_threads=getenv("OMP_NUM_THREADS"); */
  int max_threads = omp_get_max_threads();
#else
  int max_threads = 1;
#endif

  /* Working space for threaded time-slice reductions */
  complex **meson_q_thread = create_meson_q_thread(nt, max_threads, num_corr_mom);

  /* Fourier factors for FT */

  complex *ftfact = create_ftfact(nx, ny, nz, nt, num_corr_mom,
				  corr_mom, corr_parity, r0, &flops );

  /* For avoiding adding unecessary zeros */
  int *nonzero = (int *)malloc(nt*sizeof(int));
  if(nonzero == NULL){
    printf("%s(%d): No room for nonzero array\n",myname,this_node);
    terminate(1);
  }
  
  for(int t = 0; t < nt; t++)nonzero[t] = 0;
  
  /* Do FT on "meson" for momentum projection - 
     Result in meson_q.  We use a dumb FT because there 
     are usually very few momenta needed. */

  int i;
  site *s;
  FORALLSITES_OMP(i,s,) {
#ifdef OMP
    int mythread=omp_get_thread_num();
#else
    int mythread=0;
#endif

    /* Color-vector inner product on site i */
    complex meson = su3_dot(antiquark+i, quark+i);

    /* The time coordinate for this site */
    int st = s->t;
    double real = meson.real;
    double imag = meson.imag;
    nonzero[st] = 1;
    st += mythread*nt;
  
    /* Each thread accumulates its own time-slice values in meson_q_thread
       Each thread works with all of the momenta */
    for(int k=0; k<num_corr_mom; k++)
      {
	complex fourier_fact = ftfact[k+num_corr_mom*i];
	
	meson_q_thread[st][k].real += 
	  real*fourier_fact.real -  
	  imag*fourier_fact.imag;
	meson_q_thread[st][k].imag += 
	  real*fourier_fact.imag +  
	  imag*fourier_fact.real;
      }
  } END_LOOP_OMP;

  flops += (Real)num_corr_mom*8*sites_on_node;

  /* sum meson_q over all the threads */
  flops += sum_meson_q(meson_q, meson_q_thread, nonzero,
		       max_threads, nt, num_corr_mom);
  
  destroy_ftfact(ftfact);
  destroy_meson_q_thread(meson_q_thread, nt, max_threads);
  
  dtime += dclock();
  cont_args->dtime = dtime;
  cont_args->flops = flops;
}
