/*BHEADER**********************************************************************
 * Copyright (c) 2008,  Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * This file is part of HYPRE.  See file COPYRIGHT for details.
 *
 * HYPRE is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (as published by the Free
 * Software Foundation) version 2.1 dated February 1999.
 *
 * $Revision: 2.5 $
 ***********************************************************************EHEADER*/





/******************************************************************************
 *
 * cgnr (conjugate gradient on the normal equations A^TAx = A^Tb) functions
 *
 *****************************************************************************/

#ifndef hypre_KRYLOV_CGNR_HEADER
#define hypre_KRYLOV_CGNR_HEADER

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

/**
 * @name Generic CGNR Interface
 *
 * A general description of the interface goes here...
 *
 * @memo A generic CGNR linear solver interface
 * @version 0.1
 * @author Jeffrey F. Painter
 **/
/*@{*/

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
 * hypre_CGNRData and hypre_CGNRFunctions
 *--------------------------------------------------------------------------*/


/**
 * @name CGNR structs
 *
 * Description...
 **/
/*@{*/

/**
 * The {\tt hypre\_CGNRSFunctions} object ...
 **/

typedef struct
{
   char  *(*CAlloc)        ( size_t count, size_t elt_size );
   int    (*Free)          ( char *ptr );
   int    (*CommInfo)      ( void  *A, int   *my_id, int   *num_procs );
   void * (*CreateVector)  ( void *vector );
   int    (*DestroyVector) ( void *vector );
   void * (*MatvecCreate)  ( void *A, void *x );
   int    (*Matvec)        ( void *matvec_data, double alpha, void *A,
                             void *x, double beta, void *y );
   int    (*MatvecT)       ( void *matvec_data, double alpha, void *A,
                             void *x, double beta, void *y );
   int    (*MatvecDestroy) ( void *matvec_data );
   double (*InnerProd)     ( void *x, void *y );
   int    (*CopyVector)    ( void *x, void *y );
   int    (*ClearVector)   ( void *x );
   int    (*ScaleVector)   ( double alpha, void *x );
   int    (*Axpy)          ( double alpha, void *x, void *y );
   int    (*precond_setup) ( void *vdata, void *A, void *b, void *x );
   int    (*precond)       ( void *vdata, void *A, void *b, void *x );
   int    (*precondT)       ( void *vdata, void *A, void *b, void *x );
} hypre_CGNRFunctions;

/**
 * The {\tt hypre\_CGNRData} object ...
 **/

typedef struct
{
   double   tol;
   double   rel_residual_norm;
   int      min_iter;
   int      max_iter;
   int      stop_crit;

   void    *A;
   void    *p;
   void    *q;
   void    *r;
   void    *t;

   void    *matvec_data;
   void    *precond_data;

   hypre_CGNRFunctions * functions;

   /* log info (always logged) */
   int      num_iterations;

   /* additional log info (logged when `logging' > 0) */
   int      logging;
   double  *norms;
   char    *log_file_name;

} hypre_CGNRData;


#ifdef __cplusplus
extern "C" {
#endif


/**
 * @name generic CGNR Solver
 *
 * Description...
 **/
/*@{*/


/**
 * Description...
 *
 * @param param [IN] ...
 **/
hypre_CGNRFunctions *
hypre_CGNRFunctionsCreate(
   char  *(*CAlloc)        ( size_t count, size_t elt_size ),
   int    (*Free)          ( char *ptr ),
   int    (*CommInfo)      ( void  *A, int   *my_id, int   *num_procs ),
   void * (*CreateVector)  ( void *vector ),
   int    (*DestroyVector) ( void *vector ),
   void * (*MatvecCreate)  ( void *A, void *x ),
   int    (*Matvec)        ( void *matvec_data, double alpha, void *A,
                             void *x, double beta, void *y ),
   int    (*MatvecT)        ( void *matvec_data, double alpha, void *A,
                             void *x, double beta, void *y ),
   int    (*MatvecDestroy) ( void *matvec_data ),
   double (*InnerProd)     ( void *x, void *y ),
   int    (*CopyVector)    ( void *x, void *y ),
   int    (*ClearVector)   ( void *x ),
   int    (*ScaleVector)   ( double alpha, void *x ),
   int    (*Axpy)          ( double alpha, void *x, void *y ),
   int    (*PrecondSetup)  ( void *vdata, void *A, void *b, void *x ),
   int    (*Precond)       ( void *vdata, void *A, void *b, void *x ),
   int    (*PrecondT)       ( void *vdata, void *A, void *b, void *x )
   );

/**
 * Description...
 *
 * @param param [IN] ...
 **/

void *
hypre_CGNRCreate( hypre_CGNRFunctions *cgnr_functions );
int hypre_CGNRDestroy ( void *cgnr_vdata );
int hypre_CGNRSetup ( void *cgnr_vdata , void *A , void *b , void *x );
int hypre_CGNRSolve ( void *cgnr_vdata , void *A , void *b , void *x );
int hypre_CGNRSetTol ( void *cgnr_vdata , double tol );
int hypre_CGNRSetMinIter ( void *cgnr_vdata , int min_iter );
int hypre_CGNRSetMaxIter ( void *cgnr_vdata , int max_iter );
int hypre_CGNRSetStopCrit ( void *cgnr_vdata , int stop_crit );
int hypre_CGNRSetPrecond ( void *cgnr_vdata , int (*precond )(), int (*precondT )(), int (*precond_setup )(), void *precond_data );
int hypre_CGNRSetLogging ( void *cgnr_vdata , int logging );
int hypre_CGNRGetNumIterations ( void *cgnr_vdata , int *num_iterations );
int hypre_CGNRGetFinalRelativeResidualNorm ( void *cgnr_vdata , double *relative_residual_norm );



#ifdef __cplusplus
}
#endif

#endif
