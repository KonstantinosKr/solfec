/*BHEADER**********************************************************************
 * Copyright (c) 2008,  Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * This file is part of HYPRE.  See file COPYRIGHT for details.
 *
 * HYPRE is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (as published by the Free
 * Software Foundation) version 2.1 dated February 1999.
 *
 * $Revision: 2.19 $
 ***********************************************************************EHEADER*/





/******************************************************************************
 *
 * Preconditioned conjugate gradient (Omin) headers
 *
 *****************************************************************************/

#ifndef hypre_KRYLOV_PCG_HEADER
#define hypre_KRYLOV_PCG_HEADER

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

/**
 * @name Generic PCG Interface
 *
 * A general description of the interface goes here...
 *
 * @memo A generic PCG linear solver interface
 * @version 0.1
 * @author Jeffrey F. Painter
 **/
/*@{*/

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
 * hypre_PCGData and hypre_PCGFunctions
 *--------------------------------------------------------------------------*/


/**
 * @name PCG structs
 *
 * Description...
 **/
/*@{*/

/**
 * The {\tt hypre\_PCGSFunctions} object ...
 **/

typedef struct
{
   char * (*CAlloc)        ( size_t count, size_t elt_size );
   int    (*Free)          ( char *ptr );
   int    (*CommInfo)      ( void  *A, int   *my_id, int   *num_procs );
   void * (*CreateVector)  ( void *vector );
   int    (*DestroyVector) ( void *vector );
   void * (*MatvecCreate)  ( void *A, void *x );
   int    (*Matvec)        ( void *matvec_data, double alpha, void *A,
                             void *x, double beta, void *y );
   int    (*MatvecDestroy) ( void *matvec_data );
   double (*InnerProd)     ( void *x, void *y );
   int    (*CopyVector)    ( void *x, void *y );
   int    (*ClearVector)   ( void *x );
   int    (*ScaleVector)   ( double alpha, void *x );
   int    (*Axpy)          ( double alpha, void *x, void *y );

   int    (*precond)();
   int    (*precond_setup)();
} hypre_PCGFunctions;

/**
 * The {\tt hypre\_PCGData} object ...
 **/

/*
 Summary of Parameters to Control Stopping Test:
 - Standard (default) error tolerance: |delta-residual|/|right-hand-side|<tol
 where the norm is an energy norm wrt preconditioner, |r|=sqrt(<Cr,r>).
 - two_norm!=0 means: the norm is the L2 norm, |r|=sqrt(<r,r>)
 - rel_change!=0 means: if pass the other stopping criteria, also check the
 relative change in the solution x.  Pass iff this relative change is small.
 - tol = relative error tolerance, as above
 -a_tol = absolute convergence tolerance (default is 0.0)
   If one desires the convergence test to check the absolute
   convergence tolerance *only*, then set the relative convergence
   tolerance to 0.0.  (The default convergence test is  <C*r,r> <=
   max(relative_tolerance^2 * <C*b, b>, absolute_tolerance^2)
- cf_tol = convergence factor tolerance; if >0 used for special test
  for slow convergence
- stop_crit!=0 means (TO BE PHASED OUT):
  pure absolute error tolerance rather than a pure relative
  error tolerance on the residual.  Never applies if rel_change!=0 or atolf!=0.
 - atolf = absolute error tolerance factor to be used _together_ with the
 relative error tolerance, |delta-residual| / ( atolf + |right-hand-side| ) < tol
  (To BE PHASED OUT)
 - recompute_residual means: when the iteration seems to be converged, recompute the
 residual from scratch (r=b-Ax) and use this new residual to repeat the convergence test.
 This can be expensive, use this only if you have seen a problem with the regular
 residual computation.
 - recompute_residual_p means: recompute the residual from scratch (r=b-Ax)
 every "recompute_residual_p" iterations.  This can be expensive and degrade the
 convergence. Use it only if you have seen a problem with the regular residual
 computation.
*/

typedef struct
{
   double   tol;
   double   atolf;
   double   cf_tol;
   double   a_tol;
   double   rtol;
   int      max_iter;
   int      two_norm;
   int      rel_change;
   int      recompute_residual;
   int      recompute_residual_p;
   int      stop_crit;
   int      converged;

   void    *A;
   void    *p;
   void    *s;
   void    *r; /* ...contains the residual.  This is currently kept permanently.
                  If that is ever changed, it still must be kept if logging>1 */

   int      owns_matvec_data;  /* normally 1; if 0, don't delete it */
   void    *matvec_data;
   void    *precond_data;

   hypre_PCGFunctions * functions;

   /* log info (always logged) */
   int      num_iterations;
   double   rel_residual_norm;

   int     print_level; /* printing when print_level>0 */
   int     logging;  /* extra computations for logging when logging>0 */
   double  *norms;
   double  *rel_norms;

} hypre_PCGData;

#define hypre_PCGDataOwnsMatvecData(pcgdata)  ((pcgdata) -> owns_matvec_data)

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @name generic PCG Solver
 *
 * Description...
 **/
/*@{*/

/**
 * Description...
 *
 * @param param [IN] ...
 **/

hypre_PCGFunctions *
hypre_PCGFunctionsCreate(
   char * (*CAlloc)        ( size_t count, size_t elt_size ),
   int    (*Free)          ( char *ptr ),
   int    (*CommInfo)      ( void  *A, int   *my_id, int   *num_procs ),
   void * (*CreateVector)  ( void *vector ),
   int    (*DestroyVector) ( void *vector ),
   void * (*MatvecCreate)  ( void *A, void *x ),
   int    (*Matvec)        ( void *matvec_data, double alpha, void *A,
                             void *x, double beta, void *y ),
   int    (*MatvecDestroy) ( void *matvec_data ),
   double (*InnerProd)     ( void *x, void *y ),
   int    (*CopyVector)    ( void *x, void *y ),
   int    (*ClearVector)   ( void *x ),
   int    (*ScaleVector)   ( double alpha, void *x ),
   int    (*Axpy)          ( double alpha, void *x, void *y ),
   int    (*PrecondSetup)  ( void *vdata, void *A, void *b, void *x ),
   int    (*Precond)       ( void *vdata, void *A, void *b, void *x )
   );

/**
 * Description...
 *
 * @param param [IN] ...
 **/

void *
hypre_PCGCreate( hypre_PCGFunctions *pcg_functions );
int hypre_PCGDestroy ( void *pcg_vdata );
int hypre_PCGGetResidual ( void *pcg_vdata , void **residual );
int hypre_PCGSetup ( void *pcg_vdata , void *A , void *b , void *x );
int hypre_PCGSolve ( void *pcg_vdata , void *A , void *b , void *x );
int hypre_PCGSetTol ( void *pcg_vdata , double tol );
int hypre_PCGGetTol ( void *pcg_vdata , double *tol );
int hypre_PCGSetAbsoluteTol ( void *pcg_vdata , double a_tol );
int hypre_PCGGetAbsoluteTol ( void *pcg_vdata , double *a_tol );
int hypre_PCGSetAbsoluteTolFactor ( void *pcg_vdata , double atolf );
int hypre_PCGGetAbsoluteTolFactor ( void *pcg_vdata , double *atolf );
int hypre_PCGSetResidualTol ( void *pcg_vdata , double rtol );
int hypre_PCGGetResidualTol ( void *pcg_vdata , double *rtol );
int hypre_PCGSetConvergenceFactorTol ( void *pcg_vdata , double cf_tol );
int hypre_PCGGetConvergenceFactorTol ( void *pcg_vdata , double *cf_tol );
int hypre_PCGSetMaxIter ( void *pcg_vdata , int max_iter );
int hypre_PCGGetMaxIter ( void *pcg_vdata , int *max_iter );
int hypre_PCGSetTwoNorm ( void *pcg_vdata , int two_norm );
int hypre_PCGGetTwoNorm ( void *pcg_vdata , int *two_norm );
int hypre_PCGSetRelChange ( void *pcg_vdata , int rel_change );
int hypre_PCGGetRelChange ( void *pcg_vdata , int *rel_change );
int hypre_PCGSetRecomputeResidual ( void *pcg_vdata , int recompute_residual );
int hypre_PCGGetRecomputeResidual ( void *pcg_vdata , int *recompute_residual );
int hypre_PCGSetRecomputeResidualP ( void *pcg_vdata , int recompute_residual_p );
int hypre_PCGGetRecomputeResidualP ( void *pcg_vdata , int *recompute_residual_p );
int hypre_PCGSetStopCrit ( void *pcg_vdata , int stop_crit );
int hypre_PCGGetStopCrit ( void *pcg_vdata , int *stop_crit );
int hypre_PCGSetPrecond ( void *pcg_vdata , int (*precond )(), int (*precond_setup )(), void *precond_data );
int hypre_PCGSetPrintLevel ( void *pcg_vdata , int level );
int hypre_PCGGetPrintLevel ( void *pcg_vdata , int *level );
int hypre_PCGSetLogging ( void *pcg_vdata , int level );
int hypre_PCGGetLogging ( void *pcg_vdata , int *level );
int hypre_PCGGetNumIterations ( void *pcg_vdata , int *num_iterations );
int hypre_PCGGetConverged ( void *pcg_vdata , int *converged );
int hypre_PCGPrintLogging ( void *pcg_vdata , int myid );
int hypre_PCGGetFinalRelativeResidualNorm ( void *pcg_vdata , double *relative_residual_norm );


#ifdef __cplusplus
}
#endif

#endif
