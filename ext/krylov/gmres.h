/*BHEADER**********************************************************************
 * Copyright (c) 2008,  Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * This file is part of HYPRE.  See file COPYRIGHT for details.
 *
 * HYPRE is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (as published by the Free
 * Software Foundation) version 2.1 dated February 1999.
 *
 * $Revision: 2.13 $
 ***********************************************************************EHEADER*/




/******************************************************************************
 *
 * GMRES gmres
 *
 *****************************************************************************/

#ifndef hypre_KRYLOV_GMRES_HEADER
#define hypre_KRYLOV_GMRES_HEADER

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

/**
 * @name Generic GMRES Interface
 *
 * A general description of the interface goes here...
 *
 * @memo A generic GMRES linear solver interface
 * @version 0.1
 * @author Jeffrey F. Painter
 **/
/*@{*/

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
 * hypre_GMRESData and hypre_GMRESFunctions
 *--------------------------------------------------------------------------*/


/**
 * @name GMRES structs
 *
 * Description...
 **/
/*@{*/

/**
 * The {\tt hypre\_GMRESFunctions} object ...
 **/

typedef struct
{
   char * (*CAlloc)        ( size_t count, size_t elt_size );
   int    (*Free)          ( char *ptr );
   int    (*CommInfo)      ( void  *A, int   *my_id, int   *num_procs );
   void * (*CreateVector)  ( void *vector );
   void * (*CreateVectorArray)  ( int size, void *vectors );
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

} hypre_GMRESFunctions;

/**
 * The {\tt hypre\_GMRESData} object ...
 **/


typedef struct
{
   int      k_dim;
   int      min_iter;
   int      max_iter;
   int      rel_change;
   int      stop_crit;
   int      converged;
   double   tol;
   double   cf_tol;
   double   a_tol;
   double   rel_residual_norm;

   void  *A;
   void  *r;
   void  *w;
   void  *w_2;
   void  **p;

   void    *matvec_data;
   void    *precond_data;

   hypre_GMRESFunctions * functions;

   /* log info (always logged) */
   int      num_iterations;
 
   int     print_level; /* printing when print_level>0 */
   int     logging;  /* extra computations for logging when logging>0 */
   double  *norms;
   char    *log_file_name;

} hypre_GMRESData;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name generic GMRES Solver
 *
 * Description...
 **/
/*@{*/

/**
 * Description...
 *
 * @param param [IN] ...
 **/

hypre_GMRESFunctions *
hypre_GMRESFunctionsCreate(
   char * (*CAlloc)        ( size_t count, size_t elt_size ),
   int    (*Free)          ( char *ptr ),
   int    (*CommInfo)      ( void  *A, int   *my_id, int   *num_procs ),
   void * (*CreateVector)  ( void *vector ),
   void * (*CreateVectorArray)  ( int size, void *vectors ),
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
hypre_GMRESCreate( hypre_GMRESFunctions *gmres_functions );
int hypre_GMRESDestroy ( void *gmres_vdata );
int hypre_GMRESGetResidual ( void *gmres_vdata , void **residual );
int hypre_GMRESSetup ( void *gmres_vdata , void *A , void *b , void *x );
int hypre_GMRESSolve ( void *gmres_vdata , void *A , void *b , void *x );
int hypre_GMRESSetKDim ( void *gmres_vdata , int k_dim );
int hypre_GMRESGetKDim ( void *gmres_vdata , int *k_dim );
int hypre_GMRESSetTol ( void *gmres_vdata , double tol );
int hypre_GMRESGetTol ( void *gmres_vdata , double *tol );
int hypre_GMRESSetAbsoluteTol ( void *gmres_vdata , double a_tol );
int hypre_GMRESGetAbsoluteTol ( void *gmres_vdata , double *a_tol );
int hypre_GMRESSetConvergenceFactorTol ( void *gmres_vdata , double cf_tol );
int hypre_GMRESGetConvergenceFactorTol ( void *gmres_vdata , double *cf_tol );
int hypre_GMRESSetMinIter ( void *gmres_vdata , int min_iter );
int hypre_GMRESGetMinIter ( void *gmres_vdata , int *min_iter );
int hypre_GMRESSetMaxIter ( void *gmres_vdata , int max_iter );
int hypre_GMRESGetMaxIter ( void *gmres_vdata , int *max_iter );
int hypre_GMRESSetRelChange ( void *gmres_vdata , int rel_change );
int hypre_GMRESGetRelChange ( void *gmres_vdata , int *rel_change );
int hypre_GMRESSetStopCrit ( void *gmres_vdata , int stop_crit );
int hypre_GMRESGetStopCrit ( void *gmres_vdata , int *stop_crit );
int hypre_GMRESSetPrecond ( void *gmres_vdata , int (*precond )(), int (*precond_setup )(), void *precond_data );
int hypre_GMRESSetPrintLevel ( void *gmres_vdata , int level );
int hypre_GMRESGetPrintLevel ( void *gmres_vdata , int *level );
int hypre_GMRESSetLogging ( void *gmres_vdata , int level );
int hypre_GMRESGetLogging ( void *gmres_vdata , int *level );
int hypre_GMRESGetNumIterations ( void *gmres_vdata , int *num_iterations );
int hypre_GMRESGetConverged ( void *gmres_vdata , int *converged );
int hypre_GMRESGetFinalRelativeResidualNorm ( void *gmres_vdata , double *relative_residual_norm );


#ifdef __cplusplus
}
#endif
#endif
