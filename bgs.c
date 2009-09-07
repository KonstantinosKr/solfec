/*
 * bgs.c
 * Copyright (C) 2007, 2009 Tomasz Koziara (t.koziara AT gmail.com)
 * -------------------------------------------------------------------
 * block gauss seidel solver
 */

#include <stdlib.h>
#include <stdio.h>
#include "alg.h"
#include "dom.h"
#include "lap.h"
#include "bgs.h"
#include "exs.h"
#include "err.h"

#if MPI
#include <pthread.h>
#include "com.h"
#include "tag.h"
#include "sol.h"
#endif

static int projected_gradient (short dynamic, double epsilon, int maxiter,
  double step, double friction, double restitution, double gap, double rho,
  double *W, double *B, double *V, double *U, double *R)
{
  double vector [3], scalar; /* auxiliary vector & scalar */
  int iter = 0; /* current iteration counter */
  double UN; /* dashed normal velocity */

  if (dynamic && gap > 0)
  {
    SET (R, 0);
    COPY (B, U);
    return 0;
  }

  do
  {
    /* store current
     * reaction */
    COPY (R, vector);

    /* update velocity */
    NVADDMUL (B, W, R, U);

    /* compute dashed normal velocity */
    if (dynamic) UN = (U[2] + restitution * MIN (V[2], 0));
    else UN = ((MAX(gap, 0)/step) + U[2]);

    /* predict new
     * reactions */
    R [0] -= rho * U[0];
    R [1] -= rho * U[1];
    R [2] -= rho * UN;
   
    /* project normal reaction
     * into its feasible domain */ 
    R [2] = MAX (0, R [2]);

    /* project tangential reaction
     * into the friction cone section */
    scalar = sqrt (R[0]*R[0]+R[1]*R[1]);
    if (scalar >= friction * R[2])
    {
      if (scalar > 0.0)
	scalar = friction * R[2] / scalar;

      R [0] *= scalar;
      R [1] *= scalar;
    }
    
    SUB (R, vector, vector); /* absolute difference */
    scalar = DOT (R, R); /* length of current solution */
    scalar = sqrt (DOT (vector, vector)/MAX (scalar, 1.0));
  }
  while (++ iter < maxiter && scalar > epsilon);

  return iter;
}

static int de_saxe_and_feng (short dynamic, double epsilon, int maxiter,
  double step, double friction, double restitution, double gap, double rho,
  double *W, double *B, double *V, double *U, double *R)
{
  double vector [3], scalar; /* auxiliary vector & scalar */
  int iter = 0; /* current iteration counter */
  double tau [3], UN, coef;

  if (dynamic && gap > 0)
  {
    SET (R, 0);
    COPY (B, U);
    return 0;
  }

  do
  {
    /* store current
     * reaction */
    COPY (R, vector);

    /* update velocity */
    NVADDMUL (B, W, R, U);

    /* compute dashed normal velocity */
    if (dynamic) UN = (U[2] + restitution * MIN (V[2], 0));
    else UN = ((MAX(gap, 0)/step) + U[2]);

    /* predict new
     * reactions */
    tau [0] = R[0] - rho * U[0];
    tau [1] = R[1] - rho * U[1];
    tau [2] = R[2] - rho * (UN + friction * sqrt(U[0]*U[0]+U[1]*U[1]));
   
    scalar = sqrt (tau[0]*tau[0]+tau[1]*tau[1]);
    if (friction * scalar < -tau [2]) { SET (R, 0.0); }
    else if (scalar <= friction * tau [2]) { COPY (tau, R); }
    else
    {
      coef = (scalar - friction * tau [2]) / (1.0 + friction*friction);
      R [0] = tau [0] - coef * (tau [0] / scalar);
      R [1] = tau [1] - coef * (tau [1] / scalar);
      R [2] = tau [2] - coef * friction;
    }

    SUB (R, vector, vector); /* absolute difference */
    scalar = DOT (R, R); /* length of current solution */
    scalar = sqrt (DOT (vector, vector)/MAX (scalar, 1.0));
  }
  while (++ iter < maxiter && scalar > epsilon);

  return iter;
}

static int semismooth_newton (short dynamic, double epsilon, int maxiter,
  double step, double friction, double restitution, double gap, double rho,
  double *W, double *B, double *V, double *U, double *R)
{
  double RES [3], UN, norm, lim, a [9], b [3], c [3], d [3], R0 [3], error;
  int divi, ipiv [3], iter;

  if (dynamic && gap > 0)
  {
    SET (R, 0);
    COPY (B, U);
    return 0;
  }

  divi = maxiter / 10;
  iter = 0;
  do
  {
    /* store current
     * reaction */
    COPY (R, R0);

    if (dynamic) UN = (U[2] + restitution * MIN (V[2], 0));
    else UN = ((MAX(gap, 0)/step) + U[2]);

    /* predict new
     * reactions */
    d [0] = R[0] - rho * U[0];
    d [1] = R[1] - rho * U[1];
    d [2] = R[2] - rho * UN;

    /* calculate residum RES = W*R + B - U */
    NVADDMUL (B, W, R, RES);
    SUB (RES, U, RES);

    if (d [2] >= 0)
    {
      norm = sqrt (d[0]*d[0]+d[1]*d[1]); /* tangential force value */
      lim = friction * MAX (0, d[2]); /* friction limit */

      if (norm >= lim) /* frictional sliping */
      {
	double F [4], /* matrix associated with the derivative of an Euclidean norm in 2D */
	       M [4], H [4],  /* auxiliary metrices & vectors */
	       delta, alfa, beta, den, len, e; /* auxiliary scalars */


	if (lim > 0.0) /* non-degenerate case */
	{
	  len = sqrt (R[0]*R[0]+R[1]*R[1]);
	  den = MAX (lim, len) * norm;
	  e = lim / norm;
	  if (len == 0.0) beta = 1.0;
	  else
	  {
	    alfa = (R[0]*d[0]+R[1]*d[1]) / (len*norm);
	    delta = MIN (len/lim, 1.0);
	    beta = (alfa < 0.0 ? 1.0 / (1.0 - alfa*delta) : 1.0); /* relaxation factor in case of direction change */
	  }

	  F [0] = (R[0]*d[0])/den;
	  F [1] = (R[1]*d[0])/den;
	  F [2] = (R[0]*d[1])/den;
	  F [3] = (R[1]*d[1])/den;

	  M [0] = e * (1.0 - F[0]);
	  M [1] = - e * F[1];
	  M [2] = - e * F[2];
	  M [3] = e * (1.0 - F[3]);

	  H [0] = 1.0 - beta * M[0];
	  H [1] = - beta * M[1];
	  H [2] = - beta * M[2];
	  H [3] = 1.0 - beta * M[3];

	  a [0] = H[0] + rho*(M[0]*W[0] + M[2]*W[1]);
	  a [1] = H[1] + rho*(M[1]*W[0] + M[3]*W[1]);
	  a [2] = W[2];
	  a [3] = H[2] + rho*(M[0]*W[3] + M[2]*W[4]);
	  a [4] = H[3] + rho*(M[1]*W[3] + M[3]*W[4]);
	  a [5] = W[5];
	  a [6] = rho*(M[0]*W[6] + M[2]*W[7]) - friction*(d[0]/norm);
	  a [7] = rho*(M[1]*W[6] + M[3]*W[7]) - friction*(d[1]/norm);
	  a [8] = W[8];

	  b [0] = friction*(d[0]/norm)*R[2] - R[0] - rho*(M[0]*RES[0] + M[2]*RES[1]);
	  b [1] = friction*(d[1]/norm)*R[2] - R[1] - rho*(M[1]*RES[0] + M[3]*RES[1]);
	  b [2] = -UN - RES[2];
	}
	else /* degenerate case => enforce homogenous tangential tractions */
	{
	  a [0] = 1.0;
	  a [1] = 0.0;
	  a [2] = W[2];
	  a [3] = 0.0;
	  a [4] = 1.0;
	  a [5] = W[5];
	  a [6] = 0.0;
	  a [7] = 0.0;
	  a [8] = W[8];

	  b [0] = -R[0] - RES[0];
	  b [1] = -R[1] - RES[1];
	  b [2] = -UN - RES[2];
	}
      }
      else /* frictional sticking */
      {
	a [0] = W[0];
	a [1] = W[1];
	a [2] = W[2];
	a [3] = W[3];
	a [4] = W[4];
	a [5] = W[5];
	a [6] = W[6]+U[0]/d[2];
	a [7] = W[7]+U[1]/d[2];
	a [8] = W[8];

	b [0] = -(1.0 + rho*U[2]/d[2])*U[0] - RES[0];
	b [1] = -(1.0 + rho*U[2]/d[2])*U[1] - RES[1];
	b [2] = -UN - RES[2];
      }
    }
    else 
    {
      a [0] = 1.0;
      a [1] = 0.0;
      a [2] = 0.0;
      a [3] = 0.0;
      a [4] = 1.0;
      a [5] = 0.0;
      a [6] = 0.0;
      a [7] = 0.0;
      a [8] = 1.0;

      b [0] = -R[0];
      b [1] = -R[1];
      b [2] = -R[2];
    }

    if (lapack_dgesv (3, 1, a, 3, ipiv, b, 3)) return -1;
    if (isnan(b[0])||isnan(b[1])||isnan(b[2])) return -1;

    NVADDMUL (RES, W, b, c);
    ADD (R, b, R);
    ADD (U, c, U);

    SUB (R, R0, R0); 
    error = DOT (R, R);
    error = sqrt (DOT (R0, R0) / MAX (error, 1.0));
    iter ++;

    if ((iter % divi) == 0)
    {
      rho *= 10.0; /* penalty scaling */
      if (isinf (rho)) return -1;
    }
  }
  while (iter < maxiter && error > epsilon);

  return iter;
}

static int fixpnt (short dynamic, double *W, double *B, double *V, double *U, double *R)
{
  double A [9];

  if (dynamic)
  {
    NNCOPY (W, A);
    COPY (V, U);
    SCALE (U, -1.0);
    SUB (U, B, R);
    if (lapack_dposv ('U', 3, 1, A, 3, R, 3)) return -1;
  }
  else
  {
    NNCOPY (W, A);
    SET (U, 0.0);
    SUB (U, B, R);
    if (lapack_dposv ('U', 3, 1, A, 3, R, 3)) return -1;
  }

  return 0;
}

static int fixdir (short dynamic, double *W, double *B, double *V, double *U, double *R)
{
  if (dynamic)
  {
    R [0] = 0.0;
    R [1] = 0.0;
    R [2] = -(V[2] + B[2]) / W[8];
    U [0] =  B [0]; 
    U [1] =  B [1]; 
    U [2] = -V [2];
  }
  else
  {
    R [0] = 0.0;
    R [1] = 0.0;
    R [2] = -B[2] / W[8];
    U [0] = B [0]; 
    U [1] = B [1]; 
    U [2] = 0.0;
  }
  return 0;
}

static int velodir (double *Z, double *W, double *B, double *U, double *R)
{
  R [0] = 0.0;
  R [1] = 0.0;
  R [2] = (VELODIR(Z) - B[2]) / W[8];
  U [0] = B [0]; 
  U [1] = B [1]; 
  U [2] = VELODIR(Z);
  return 0;
}

static int riglnk (short dynamic, double epsilon, int maxiter, double step,
  double *base, double *Z, double *W, double *B, double *V, double *U, double *R)
{
  double B0 [3], b,
	 C [3],
	 D [9],
	 LRR [9],
	 LRl [3],
	 LL [16],
	 DX [4],
	 TMP [9],
	 l, len,
	 tmp,
	 error;
  int iter,
      ipiv [4];

  if (DOT (B, B) == 0.0)
  {
    SET (R, 0);
    COPY (B, U);
  }
  else if (dynamic) /* q(n+1) = q(n) + (h/2) * (u(n) + u(n+1)) */
  {
    U [0] =  B[0]; 
    U [1] =  B[1]; 
    U [2] = -V[2];
    R [0] =  0.0;
    R [1] =  0.0;
    R [2] = (U[2] - B[2])/W[8];
  }
  else /* q(n+1) = q(n) + h * u(n+1) */
  {
    NVMUL (base, B, B0);
    SCALE (B0, step);
    ADD (B0, RIGLNK_VEC(Z), B0);
    b = DOT (B0, B0) - RIGLNK_LEN(Z)*RIGLNK_LEN(Z);
    TVMUL (base, B0, TMP);
    NVMUL (W, TMP, C);
    SCALE (C, step);
    NNMUL (base, W, LL);
    TNMUL (base, LL, TMP);
    NNMUL (W, TMP, D);
    SCALE9 (D, step*step);

    /* initial R, l */
    l = 0.0;
    if (DOT (R, R) == 0.0) /* use old value if non-zero */
    {
      R [0] = 0.0;
      R [1] = 0.0;
      R [2] = epsilon;
    }

    iter = 0;
    do
    {
      len = DOT (R, R);
      tmp = 1.0 / len;
      len = sqrt (len);
      DIADIC (R, R, TMP);
      SCALE9 (TMP, tmp);
      IDENTITY (LRR);
      NNSUB (LRR, TMP, LRR);
      tmp = 1.0 / len;
      SCALE9 (LRR, tmp);
      NNADDMUL (LRR, l, D, LRR);
      NVMUL (D, R, TMP);
      ADD (C, TMP, LRl);
      COPY (R, DX);
      SCALE (DX, tmp);
      ADDMUL (DX, l, LRl, DX);
      DX [3] = b + DOT (C, R);
      DX [3] += DOT (R, TMP);

      LL[0] = LRR[0]; LL[4] = LRR[3]; LL[8]  = LRR[6]; LL[12] = LRl[0];
      LL[1] = LRR[1]; LL[5] = LRR[4]; LL[9]  = LRR[7]; LL[13] = LRl[1];
      LL[2] = LRR[2]; LL[6] = LRR[5]; LL[10] = LRR[8]; LL[14] = LRl[2];
      LL[3] = LRl[0]; LL[7] = LRl[1]; LL[11] = LRl[2]; LL[15] = 0.0;

      if (lapack_dgesv (4, 1, LL, 4, ipiv, DX, 4)) return -1;

      SUB (R, DX, R);
      l -= DX [3];

      error = sqrt (DOT(DX,DX)/DOT(R,R));

    } while (error > epsilon && ++iter < maxiter);

    NVADDMUL (B, W, R, U);
   
    return iter;
  }

  return 0;
}

static int solver (GAUSS_SEIDEL *gs, short dynamic, double step, short kind,
  SURFACE_MATERIAL *mat, double gap, double *Z, double *base, DIAB *dia, double *B)
{
  switch (kind)
  {
  case CONTACT:
    switch (mat->model)
    {
    case SIGNORINI_COULOMB:
      switch (gs->diagsolver)
      {
      case GS_PROJECTED_GRADIENT:
	return projected_gradient (dynamic, gs->diagepsilon, gs->diagmaxiter, step, mat->friction,
			   mat->restitution, gap, dia->rho, dia->W, B, dia->V, dia->U, dia->R);
      case GS_DE_SAXE_AND_FENG:
	return de_saxe_and_feng (dynamic, gs->diagepsilon, gs->diagmaxiter, step, mat->friction,
			 mat->restitution, gap, dia->rho, dia->W, B, dia->V, dia->U, dia->R);
      case GS_SEMISMOOTH_NEWTON:
	return semismooth_newton (dynamic, gs->diagepsilon, gs->diagmaxiter, step, mat->friction,
			  mat->restitution, gap, dia->rho, dia->W, B, dia->V, dia->U, dia->R);
      }
      break;
    case SPRING_DASHPOT:
      return EXPLICIT_Spring_Dashpot_Contact (gap, mat->spring, mat->dashpot,
	                         mat->friction, dia->W, dia->B, dia->V, dia->U, dia->R);
    }
    break;
  case FIXPNT:
    return fixpnt (dynamic, dia->W, B, dia->V, dia->U, dia->R);
  case FIXDIR:
    return fixdir (dynamic, dia->W, B, dia->V, dia->U, dia->R);
  case VELODIR:
    return velodir (Z, dia->W, B, dia->U, dia->R);
  case RIGLNK:
    return riglnk (dynamic, gs->diagepsilon, gs->diagmaxiter,
	    step, base, Z, dia->W, B, dia->V, dia->U, dia->R);
  }

  return 0;
}

#if MPI
/* number of cpus in here  */
static int cpu_count (GAUSS_SEIDEL *gs, int *ierr)
{
  *ierr = ZOLTAN_OK;
  return 1;
}

/* list of cpus in here */
static void cpu_list (GAUSS_SEIDEL *gs, int num_gid_entries, int num_lid_entries,
  ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR local_ids, int wgt_dim, float *obj_wgts, int *ierr)
{
  int rank;

  MPI_Comm_rank (MPI_COMM_WORLD, &rank);
  global_ids [0] =  (rank + 1); /* ids start from 1, while ranks start from 0 */
  *ierr = ZOLTAN_OK;
}

/* size of the adjacent cpus set */
static int adjcpu_size (GAUSS_SEIDEL *gs, int num_gid_entries,
  int num_lid_entries, ZOLTAN_ID_PTR global_id, ZOLTAN_ID_PTR local_id, int *ierr)
{
  *ierr = ZOLTAN_OK;
  return SET_Size (gs->adjcpu);
}

/* list of adjacent cpus */
static void adjcpu_list (GAUSS_SEIDEL *gs, int num_gid_entries, int num_lid_entries,
  ZOLTAN_ID_PTR global_id, ZOLTAN_ID_PTR local_id, ZOLTAN_ID_PTR nbor_global_id,
  int *nbor_procs, int wgt_dim, float *ewgts, int *ierr)
{
  SET *item;
  int i;

  for (i = 0, item = SET_First (gs->adjcpu); item; i ++, item = SET_Next (item))
  {
    nbor_global_id [i] = ((int) (long) item->data) + 1;
    nbor_procs [i] = (int) (long) item->data;
  }

  *ierr = ZOLTAN_OK;
}

/* create MPI related data */
static void create_mpi (GAUSS_SEIDEL *gs)
{
  int ncpu;

  MPI_Comm_size (MPI_COMM_WORLD, &ncpu);
  MEM_Init (&gs->setmem, sizeof (SET), MAX (ncpu, 1024));
  gs->adjcpu = NULL;

  /* create Zoltan context */
  ASSERT (gs->zol = Zoltan_Create (MPI_COMM_WORLD), ERR_ZOLTAN);

  /* general parameters */
  Zoltan_Set_Param (gs->zol, "DEBUG_LEVEL", "0");
  Zoltan_Set_Param (gs->zol, "DEBUG_MEMORY", "0");
  Zoltan_Set_Param (gs->zol, "NUM_GID_ENTRIES", "1");
  Zoltan_Set_Param (gs->zol, "NUM_LID_ENTRIES", "0");

  /* callbacks */
  Zoltan_Set_Fn (gs->zol, ZOLTAN_NUM_OBJ_FN_TYPE, (void (*)()) cpu_count, gs);
  Zoltan_Set_Fn (gs->zol, ZOLTAN_OBJ_LIST_FN_TYPE, (void (*)()) cpu_list, gs);
  Zoltan_Set_Fn (gs->zol, ZOLTAN_NUM_EDGES_FN_TYPE, (void (*)()) adjcpu_size, gs);
  Zoltan_Set_Fn (gs->zol, ZOLTAN_EDGE_LIST_FN_TYPE, (void (*)()) adjcpu_list, gs);
}

/* destroy MPI related data */
static void destroy_mpi (GAUSS_SEIDEL *gs)
{
  Zoltan_Destroy (&gs->zol);
}
#endif

/* create solver */
GAUSS_SEIDEL* GAUSS_SEIDEL_Create (double epsilon, int maxiter, GSFAIL failure,
                                   double diagepsilon, int diagmaxiter, GSDIAS diagsolver,
				   void *data, GAUSS_SEIDEL_Callback callback)
{
  GAUSS_SEIDEL *gs;

  ERRMEM (gs = malloc (sizeof (GAUSS_SEIDEL)));
  gs->epsilon = epsilon;
  gs->maxiter = maxiter;
  gs->failure = failure;
  gs->diagepsilon = diagepsilon;
  gs->diagmaxiter = diagmaxiter;
  gs->diagsolver = diagsolver;
  gs->data = data;
  gs->callback = callback;
  gs->history = GS_OFF;
  gs->rerhist = NULL;
  gs->verbose = 0;
  gs->reverse = GS_OFF;
  gs->variant = GS_MID_LOOP;

#if MPI
  create_mpi (gs);
#endif

  return gs;
}

#if MPI
/* a single row Gauss-Seidel step */
static int gauss_seidel (GAUSS_SEIDEL *gs, short dynamic, double step, DIAB *dia, double *errup, double *errlo)
{
  OFFB *blk;
  int diagiters;
  double R0 [3],
	 B [3],
	 *R;

  R = dia->R;

  /* prefetch reactions */
  for (blk = dia->adj; blk; blk = blk->n)
  {
    if (blk->dia) { COPY (blk->dia->R, blk->R); }
    else { COPY (XR(blk->x)->R, blk->R); }
  }

  /* compute local free velocity */
  COPY (dia->B, B);
  for (blk = dia->adj; blk; blk = blk->n)
  {
    double *W = blk->W,
	   *R = blk->R;
    NVADDMUL (B, W, R, B);
  }

  COPY (R, R0); /* previous reaction */

  /* solve local diagonal block problem */
  if (dia->con) /* LDB_OFF */
  {
    CON *con = dia->con;
    diagiters = solver (gs, dynamic, step, con->kind, &con->mat, con->gap, con->Z, con->base, dia, B);
  }
  else diagiters = solver (gs, dynamic, step, dia->kind, &dia->mat, dia->gap, dia->Z, dia->base, dia, B);

  if (diagiters > gs->diagmaxiter || diagiters < 0)
  {
    if (diagiters < 0) gs->error = GS_DIAGONAL_FAILED;
    else gs->error = GS_DIAGONAL_DIVERGED;

    switch (gs->failure)
    {
    case GS_FAILURE_EXIT: /* delay exit until external loop is reached */
    case GS_FAILURE_CONTINUE:
      COPY (R0, R); /* use previous reaction */
      break;
    case GS_FAILURE_CALLBACK:
      gs->callback (gs->data);
      break;
    }
  }

  /* accumulate relative
   * error components */
  SUB (R, R0, R0);
  *errup += DOT (R0, R0);
  *errlo += DOT (R, R);

  return diagiters;
}

/* create rank coloring using adjacency graph between
 * processors derived from the balanced W graph */
static int* processor_coloring (GAUSS_SEIDEL *gs, LOCDYN *ldy)
{
  int *coloring;
  SET **adjcpu;
  MEM *setmem;
  DIAB *dia;
  int ncpu;
  int rank;

  adjcpu = &gs->adjcpu;
  setmem = &gs->setmem;

  /* empty current set of adjacent processors */
  SET_Free (setmem, adjcpu);

  /* create new set of adjacent processors */
  for (dia = ldy->diab; dia; dia = dia->n)
  {
    for (MAP *item = MAP_First (dia->children); item; item = MAP_Next (item))
    {
      SET_Insert (setmem, adjcpu, item->key, NULL);
    }

    for (SET *item = SET_First (dia->rext); item; item = SET_Next (item))
    {
      SET_Insert (setmem, adjcpu, (void*) (long) XR(item->data)->rank, NULL);
    }
  }

  rank = DOM(ldy->dom)->rank;
  ncpu = DOM(ldy->dom)->ncpu;
  ERRMEM (coloring = calloc (ncpu, sizeof (int))); /* processor to color map */

  /* FIXME: Zoltan coloring writes on Solfec memory (randomly) */
#if 0
  int num_gid_entries,
      num_lid_entries,
      color_exp;

  ZOLTAN_ID_TYPE global_ids = (rank + 1),
		 local_ids;

  /* perform coloring */
  ASSERT (Zoltan_Color (gs->zol, &num_gid_entries, &num_lid_entries,
	  1, &global_ids, &local_ids, &color_exp) == ZOLTAN_OK, ERR_ZOLTAN);

  /* gather coloring from all processors */
  MPI_Allgather (&color_exp, 1, MPI_INT, coloring, 1, MPI_INT, MPI_COMM_WORLD);
#else
  int i, n, m, *size, *disp, *adj;
  SET *item;

  ERRMEM (size = malloc (sizeof (int [ncpu])));
  ERRMEM (disp = malloc (sizeof (int [ncpu + 1])));

  n = SET_Size (*adjcpu);
  MPI_Allgather (&n, 1, MPI_INT, size, 1, MPI_INT, MPI_COMM_WORLD);

  for (i = disp [0] = 0; i < ncpu - 1; i ++) disp [i+1] = disp [i] + size [i];
  for (i = 0, item = SET_First (*adjcpu); item; i ++, item = SET_Next (item)) coloring [i] = (int) (long) item->data;

  m = disp [ncpu] = (disp [ncpu-1] + size [ncpu-1]);
  ERRMEM (adj = malloc (sizeof (int [m])));

  MPI_Allgatherv (coloring, n, MPI_INT, adj, size, disp, MPI_INT, MPI_COMM_WORLD); /* gather graph adjacency */

  for (i = 0; i < ncpu; i ++) coloring [i] = 0; /* invalidate coloring */

  m = 1; /* free color */

  for (i = 0; i < ncpu; i ++) /* simple BFS coloring */
  {
    int *j, *k;

    if (coloring [i] == 0) coloring [i] = m ++; /* not colored => do it */

    for (j = &adj[disp[i]], k = &adj[disp[i+1]]; j < k; j ++) /* for each adjacent vertex */
    {
      if (coloring [*j] == 0) coloring [*j] = m ++; /* not colored => do it */
      else if (coloring [*j] == coloring [i]) coloring [*j] = m ++; /* conflict => do it again */
    }
  }

  free (size);
  free (disp);
  free (adj);
#endif

  return coloring;
}

/* return next pointer and realloc send memory if needed */
inline static COMDATA* sendnext (int nsend, int *size, COMDATA **send)
{
  if (nsend >= *size)
  {
    (*size) *= 2;
    ERRMEM (*send = realloc (*send, sizeof (COMDATA [*size])));
  }

  return &(*send)[nsend];
}

/* undo external reactions */
inline static void REXT_undo (LOCDYN *ldy)
{
  XR *x, *end;

  for (x = ldy->REXT, end = x + ldy->REXT_count; x < end; x ++) x->done = 0;
}

#if DEBUG
/* test whether all reactions are done */
inline static int REXT_alldone (LOCDYN *ldy)
{
  XR *x, *end;
  int alldone;

  for (alldone = 1, x = ldy->REXT, end = x + ldy->REXT_count; x < end; x ++)
    if (!x->done) { alldone = 0; break; }

  return alldone;
}
#endif

/* receive reactions */
static void REXT_recv (LOCDYN *ldy, COMDATA *recv, int nrecv)
{
  XR *REXT, *x;
  COMDATA *ptr;
  int i, j, *k;
  double *R;

  for (REXT = ldy->REXT, i = 0, ptr = recv; i < nrecv; i ++, ptr ++)
  {
    ASSERT_DEBUG (ptr->doubles / ptr->ints == 3 &&
	          ptr->doubles % ptr->ints == 0,  "Incorrect data count received");

    for (j = 0, k = ptr->i, R = ptr->d; j < ptr->ints; j ++, k ++, R += 3)
    {
      x = &REXT [*k];
      COPY (R, x->R);
      x->done = 1; /* it's done for now */
    }
  }
}

/* perform a Guss-Seidel sweep over a set of blocks */
static int gauss_seidel_sweep (SET *set, int reverse,
  GAUSS_SEIDEL *gs, short dynamic, double step, double *errup, double *errlo)
{
  SET* (*first) (SET*);
  SET* (*next) (SET*);
  int di, dimax;

  if (reverse) first = SET_Last, next = SET_Prev;
  else first = SET_First, next = SET_Next;

  dimax = 0;

  for (SET *item = first (set); item; item = next (item))
  {
    di = gauss_seidel (gs, dynamic, step, item->data, errup, errlo);
    dimax = MAX (dimax, di);
  }

  return dimax;
}

#define SMR() SOLFEC_Timer_Start (sol, "GSMRUN")
#define EMR() SOLFEC_Timer_End (sol, "GSMRUN")
#define SMC() SOLFEC_Timer_Start (sol, "GSMCOM")
#define EMC() SOLFEC_Timer_End (sol, "GSMCOM")

/* perform a Guss-Seidel loop over a set of blocks */
static int gauss_seidel_loop (SET *set, int reverse, int mycolor, int *color,
  GAUSS_SEIDEL *gs, LOCDYN *ldy, short dynamic, double step, double *errup, double *errlo)
{
  SET* (*first) (SET*);
  SET* (*next) (SET*);

  if (reverse) first = SET_Last, next = SET_Prev;
  else first = SET_First, next = SET_Next;

  int di, dimax;

  SOLFEC *sol = DOM(ldy->dom)->owner;
  MEM *setmem = &gs->setmem;
  SET *active = NULL,
      *updated = NULL,
      *item, *jtem;

  COMDATA *send, *recv, *ptr;
  int size, nsend, nrecv;

  size = SET_Size (set);
  size = MAX (size, 128);
  ERRMEM (send = malloc (sizeof (COMDATA [size])));

  /* copy input set into the active set */
  for (item = SET_First (set); item; item = SET_Next (item))
    SET_Insert (setmem, &active, item->data, NULL);

  int nactive, mactive;

  dimax = 0;

  do
  {
    SMR();

    for (item = first (active); item; item = next (item))
    {
      DIAB *dia = item->data;
      short adjdone = 1; /* initially, the adjacent external reactions are assumed done */

      for (jtem = SET_First (dia->rext); jtem; jtem = SET_Next (jtem))
      {
	XR *x = XR (jtem->data);
	int adjcolor = color [x->rank];

	if ((reverse && adjcolor < mycolor && !x->done) || /* a lower external reaction is undone */
	    (!reverse && adjcolor > mycolor && !x->done)) { adjdone = 0; break; } /* or a higher external reaction is undone */
      }

      if (adjdone) /* external reactions were done */
      {
	di = gauss_seidel (gs, dynamic, step, dia, errup, errlo); /* update the current block */
	dimax = MAX (dimax, di);

	SET_Insert (setmem, &updated, dia, NULL); /* schedule for sending */
      }
    }

    for (nsend = 0, ptr = send, item = SET_First (updated); item; item = SET_Next (item)) /* fill send buffer */
    {
      DIAB *dia = item->data;

      SET_Delete (setmem, &active, dia, NULL); /* no more active */

      for (MAP *ktem = MAP_First (dia->children); ktem; ktem = MAP_Next (ktem)) /* send to all children */
      {
	ptr->rank = (int) (long) ktem->key;
	ptr->ints = 1;
	ptr->doubles = 3;
	ptr->i = (int*) &ktem->data;
	ptr->d = dia->R;
	ptr = sendnext (++ nsend, &size, &send);
      }
    }

    EMR ();

    SMC ();

    COM (MPI_COMM_WORLD, TAG_GAUSS_SEIDEL_MID, send, nsend, &recv, &nrecv); /* communicate all updated blocks */

    REXT_recv (ldy, recv, nrecv); /* update external reactions */

    SET_Free (setmem, &updated); /* empty the updated blocks set */

    free (recv);

    nactive = SET_Size (active);

    MPI_Allreduce (&nactive, &mactive, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD); /* is there a nonempty active set ? */

    EMC ();
  }
  while (mactive); /* until all active set are empty */

  return dimax;
}

typedef struct gauss_seidel_thread_data GSTD;

/* mid set thread data */
struct gauss_seidel_thread_data
{
#if MPITHREADS
  pthread_t thread;

  int active,
      wait,
      done;
#endif

  SET *mid;
  GSONOFF reverse;
  int mycolor;
  int *color;
  GAUSS_SEIDEL *gs;
  LOCDYN *ldy;
  short dynamic;
  double step;
  double errup;
  double errlo;
  int dimax;
};

#if MPITHREADS
/* mid set thread execution routine; there is only one thread
 * per process hence no need for a sophisticated sheduling */
static void* gauss_seidel_thread (GSTD *data)
{
  while (data->active)
  {
    while (data->wait);

    if (data->active)
    {
      data->wait = 1;

      data->dimax = gauss_seidel_loop (data->mid, data->gs->iters % 2,
		      data->mycolor, data->color, data->gs, data->ldy,
		 data->dynamic, data->step, &data->errup, &data->errlo);

      data->done = 1;
    }
  }

  return NULL;
}
#endif

/* create gauss seidel thread */
GSTD* gauss_seidel_thread_create (SET *mid, GSONOFF reverse, int mycolor,
  int *color, GAUSS_SEIDEL *gs, LOCDYN *ldy, short dynamic, double step)
{
  GSTD *data;

  ERRMEM (data = malloc (sizeof (GSTD)));

  data->mid = mid;
  data->reverse = reverse;
  data->mycolor = mycolor;
  data->color = color;
  data->gs = gs;
  data->ldy = ldy;
  data->dynamic = dynamic;
  data->step = step;
  data->errup = 0.0;
  data->errlo = 0.0;

#if MPITHREADS
  data->active = 1;
  data->wait = 1;
  data->done = 0;
  pthread_create (&data->thread, NULL, (void* (*)()) gauss_seidel_thread, data);
#endif

  return data;
}

/* subsequent thread run */
static void gauss_seidel_thread_run (GSTD *data)
{
#if MPITHREADS
  data->errup = 0.0;
  data->errlo = 0.0;

  data->done = 0;
  data->wait = 0;
#endif
}

/* wait for completion of a subsequent run */
static int gauss_seidel_thread_wait (GSTD *data, double *errup, double *errlo)
{
#if MPITHREADS
  while (!data->done);

  *errup += data->errup;
  *errlo += data->errlo;

  return data->dimax;
#else
  return gauss_seidel_loop (data->mid, data->reverse && data->gs->iters % 2,
    data->mycolor, data->color, data->gs, data->ldy, data->dynamic,
    data->step, &data->errup, &data->errlo);
#endif
}

static void gauss_seidel_thread_destroy (GSTD *data)
{
#if MPITHREADS
  void *r;

  data->active = 0;
  data->wait = 0;

  pthread_join (data->thread, &r);
#endif

  free (data);
}

#define SI() SOLFEC_Timer_Start (sol, "GSINIT")
#define EI() SOLFEC_Timer_End (sol, "GSINIT")
#define SE() SOLFEC_Timer_Start (sol, "GSEXIT")
#define EE() SOLFEC_Timer_End (sol, "GSEXIT")
#define SR() SOLFEC_Timer_Start (sol, "GSRUN")
#define ER() SOLFEC_Timer_End (sol, "GSRUN")
#define SC() SOLFEC_Timer_Start (sol, "GSCOM")
#define EC() SOLFEC_Timer_End (sol, "GSCOM")

/* run parallel solver */
void GAUSS_SEIDEL_Solve (GAUSS_SEIDEL *gs, LOCDYN *ldy)
{
  SOLFEC *sol = DOM(ldy->dom)->owner;
  int di, dimax, diagiters;
  double error, step;
  short dynamic;
  char fmt [512];
  MEM *setmem;
  DIAB *dia;
  int div = 10,
      mycolor,
     *color,
      rank;

  SI();

  GSONOFF reverse = gs->reverse; /* fetch these two here as a user might change them */
  GSVARIANT variant = gs->variant; /* later from a callback */

  if (gs->verbose) sprintf (fmt, "GAUSS_SEIDEL: iteration: %%%dd  error:  %%.2e\n", (int)log10 (gs->maxiter) + 1);

  if (gs->history) gs->rerhist = realloc (gs->rerhist, gs->maxiter * sizeof (double));

  LOCDYN_REXT_Update (ldy); /* update REXT related data */

  color = processor_coloring (gs, ldy); /* color processors */
  rank = DOM(ldy->dom)->rank;
  mycolor = color [rank];
  setmem = &gs->setmem;

  SET *bot  = NULL, /* blocks that only send to higher processors */
      *top  = NULL, /* blocks that only send to lower processors */
      *mid  = NULL, /* blocks that send to higher and lower processors */
      *inb  = NULL, /* internal blocks */
      *in1  = NULL, /* first subest of internal nodes (used when non-blocking communication is enabled) */
      *in2  = NULL; /* second subset of internal nodes */

  int size1 = 0,
      size2 = 0,
      size3 = 0,
      size4,
      size5;

  /* create block sets */
  for (dia = ldy->diab; dia; dia = dia->n)
  {
    short lo = 0,
	  hi = 0;

    for (MAP *item = MAP_First (dia->children); item; item = MAP_Next (item))
    {
      int adjcolor = color [(int) (long) item->key];

      if (adjcolor < mycolor) hi ++; /* sends to lower */
      else lo ++; /* send to higher */
    }

    for (SET *item = SET_First (dia->rext); item; item = SET_Next (item))
    {
      int adjcolor = color [XR(item->data)->rank];

      if (adjcolor < mycolor) hi ++; /* receive from lower */
      else lo ++; /* receive from higher */
    }

    if (lo && hi) SET_Insert (setmem, &mid, dia, NULL);
    else if (lo) SET_Insert (setmem, &bot, dia, NULL), size1 += dia->degree;
    else if (hi) SET_Insert (setmem, &top, dia, NULL), size2 += dia->degree;
    else SET_Insert (setmem, &inb, dia, NULL), size3 += dia->degree;
  }

  if (variant == GS_MID_TO_ONE)
  {
    /* in1 = inb \ adj (mid) (those in inb not adjacent to mid)
     * in2 = inb \ in1 (those in inb adjacent to inb)
     */

    for  (SET *item = SET_First (inb); item; item = SET_Next (item))
    {
      dia = item->data;
      OFFB *b;

      for (b = dia->adj; b; b = b->n)
      {
	if (SET_Contains (mid, b->dia, NULL)) break;
      }

      if (b) SET_Insert (setmem, &in2, dia, NULL);
      else SET_Insert (setmem, &in1, dia, NULL);
    }
  }
  else if (variant >=  GS_NOB_MID_LOOP) /* non-blocking */
  {
    /* size1 + |in2| = size2 + |in1|
     * |in1| + |in2| = size3
     * -------------------------------
     * |in2| = (size3 + size2 - size1) / 2
     */

    size4 = (size3 + size2 - size1) / 2;
    size4 = MAX (0, size4);
    size5 = 0;

    /* create in1 and in2 such that: |bot| + |in2| = |top| + |in1| */
    for (SET *item = SET_First (inb); item; item = SET_Next (item))
    {
      dia = item->data;

      if (size5 < size4) SET_Insert (setmem, &in2, dia, NULL), size5 += dia->degree;
      else SET_Insert (setmem, &in1, dia, NULL);
    }
  }

#if DEBUG
  if (gs->verbose)
  {
    int sizes [6] = {SET_Size (bot), SET_Size (mid), SET_Size (top), SET_Size (inb), SET_Size (in1), SET_Size (in2)},
	result [6];

    MPI_Reduce (sizes, result, 6, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    if (rank == 0)
      printf ("GAUSS_SEIDEL (%s, REVERSE %s): |BOT| = %d, |MID| = %d, |TOP| = %d, |INT| = %d, |IN1| = %d, |IN2| = %d\n",
	GAUSS_SEIDEL_Variant (gs), GAUSS_SEIDEL_Reverse (gs), result [0], result [1], result [2], result [3], result [4], result [5]);
  }
#endif

  void *bot_pattern, /* communication pattern when sending from lower to higher processors */
       *top_pattern; /* the reverse communication pattern */

  COMDATA *send_bot, *recv_bot, *ptr_bot,
	  *send_top, *recv_top, *ptr_top;

  int size_bot, nsend_bot, nrecv_bot,
      size_top, nsend_top, nrecv_top;

  size_bot = size_top = 512;
  nsend_bot = nsend_top = 0;

  ERRMEM (send_bot = calloc (size_bot, sizeof (COMDATA)));
  ERRMEM (send_top = calloc (size_top, sizeof (COMDATA)));

  ptr_bot = send_bot;
  ptr_top = send_top;

  /* prepare 'bot' send buffer */
  for (SET *item = SET_First (bot); item; item = SET_Next (item))
  {
    dia = item->data;

    for (MAP *jtem = MAP_First (dia->children); jtem; jtem = MAP_Next (jtem))
    {
      ptr_bot->rank = (int) (long) jtem->key;
      ptr_bot->ints = 1;
      ptr_bot->doubles = 3;
      ptr_bot->i = (int*) &jtem->data;
      ptr_bot->d = dia->R;
      ptr_bot = sendnext (++ nsend_bot, &size_bot, &send_bot);
    }
  }

  /* prepare 'top' send buffer */
  for (SET *item = SET_First (top); item; item = SET_Next (item))
  {
    dia = item->data;

    for (MAP *jtem = MAP_First (dia->children); jtem; jtem = MAP_Next (jtem))
    {
      ptr_top->rank = (int) (long) jtem->key;
      ptr_top->ints = 1;
      ptr_top->doubles = 3;
      ptr_top->i = (int*) &jtem->data;
      ptr_top->d = dia->R;
      ptr_top = sendnext (++ nsend_top, &size_top, &send_top);
    }
  }

  bot_pattern = COM_Pattern (MPI_COMM_WORLD, TAG_GAUSS_SEIDEL_BOT, send_bot, nsend_bot, &recv_bot, &nrecv_bot);
  top_pattern = COM_Pattern (MPI_COMM_WORLD, TAG_GAUSS_SEIDEL_TOP, send_top, nsend_top, &recv_top, &nrecv_top);

  GSTD *data = NULL;

  /* create mid set update thread */
  if (variant == GS_MID_THREAD ||
      variant == GS_NOB_MID_THREAD) data = gauss_seidel_thread_create (mid, reverse, mycolor, color, gs, ldy, dynamic, step);

  void *upattern = NULL;
  SET *umid;

  /* create union of mid blocks */
  if (variant == GS_MID_TO_ALL ||
      variant == GS_NOB_MID_TO_ALL) umid = LOCDYN_Union_Create (ldy, mid, -1, &upattern);
  else if (variant == GS_MID_TO_ONE) umid = LOCDYN_Union_Create (ldy, mid, SET_Size (in1), &upattern); /* size of blocks not adjacent to mid */
  else if (variant == GS_NOB_MID_TO_ALL) umid = LOCDYN_Union_Create (ldy, mid, ldy->ndiab, &upattern);

  dynamic = DOM(ldy->dom)->dynamic;
  step = DOM(ldy->dom)->step;
  gs->error = GS_OK;
  gs->iters = 0;
  dimax = 0;
  EI ();
  do
  {
    double errup = 0.0,
           errlo = 0.0,
	   errloc [2],
	   errsum [2];

    REXT_undo (ldy); /* undo all external reactions */

    switch (variant)
    {
    case GS_MID_LOOP:
    {
      if (reverse && gs->iters % 2)
      {
	SR(); di = gauss_seidel_sweep (inb, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
	di = gauss_seidel_sweep (bot, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER ();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	di = gauss_seidel_loop (mid, 1, mycolor, color, gs, ldy, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
	SR(); di = gauss_seidel_sweep (top, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
      }
      else
      {
	SR(); di = gauss_seidel_sweep (top, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER ();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
	di = gauss_seidel_loop (mid, 0, mycolor, color, gs, ldy, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
	SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	SR(); di = gauss_seidel_sweep (inb, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
      }
    }
    break;
    case GS_MID_THREAD:
    {
      if (reverse && gs->iters % 2)
      {
	SR(); di = gauss_seidel_sweep (bot, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	gauss_seidel_thread_run (data);
	SR(); di = gauss_seidel_sweep (inb, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER ();
	di = gauss_seidel_thread_wait (data, &errup, &errlo); dimax = MAX (dimax, di);
	SR(); di = gauss_seidel_sweep (top, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
      }
      else
      {
	SR(); di = gauss_seidel_sweep (top, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
	gauss_seidel_thread_run (data);
	SR(); di = gauss_seidel_sweep (inb, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	di = gauss_seidel_thread_wait (data, &errup, &errlo); dimax = MAX (dimax, di);
	SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
      }
    }
    break;
    case GS_MID_TO_ALL:
    {
      if (reverse && gs->iters % 2)
      {
	SR(); di = gauss_seidel_sweep (inb, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
	di = gauss_seidel_sweep (bot, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	SMC(); LOCDYN_Union_Gather (upattern); EMC ();
	SMR(); di = gauss_seidel_sweep (umid, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); EMR();
	SMC(); LOCDYN_Union_Scatter (upattern); EMC();
	SR(); di = gauss_seidel_sweep (top, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
      }
      else
      {
	SR(); di = gauss_seidel_sweep (top, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
	SMC(); LOCDYN_Union_Gather (upattern); EMC();
	SMR(); di = gauss_seidel_sweep (umid, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); EMR();
	SMC(); LOCDYN_Union_Scatter (upattern); EMC();
	SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	SR(); di = gauss_seidel_sweep (inb, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
      }
    }
    break;
    case GS_MID_TO_ONE:
    {
      if (reverse && gs->iters % 2)
      {
	SR(); di = gauss_seidel_sweep (in2, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER(); /* internal blocks adjacent to mid blocks */
	SMC(); LOCDYN_Union_Gather (upattern); EMC();
	SR(); di = gauss_seidel_sweep (in1, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER(); /* internal blocks not adjacent to mid blocls */
	SMR(); di = gauss_seidel_sweep (umid, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); EMR(); /* union of mid blocks on root */
	SMC(); LOCDYN_Union_Scatter (upattern); EMC();
	SR(); di = gauss_seidel_sweep (bot, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	SR(); di = gauss_seidel_sweep (top, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
      }
      else
      {
	SR(); di = gauss_seidel_sweep (top, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
	SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	SMC(); LOCDYN_Union_Gather (upattern); EMC();
	SMR(); di = gauss_seidel_sweep (umid, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); EMR(); /* union of mid blocks on root */
	SR(); di = gauss_seidel_sweep (in1, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER(); /* internal blocks not adjacent to mid blocls */
	SMC(); LOCDYN_Union_Scatter (upattern); EMC();
	SR(); di = gauss_seidel_sweep (in2, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER(); /* internal blocks adjacent to mid blocks */
      }
    }
    break;
    case GS_NOB_MID_LOOP:
    {
      if (reverse && gs->iters % 2)
      {
	SR(); di = gauss_seidel_sweep (bot, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Send (bot_pattern); EC();
	SR(); di = gauss_seidel_sweep (in1, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Recv (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	di = gauss_seidel_loop (mid, 1, mycolor, color, gs, ldy, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
	SR(); di = gauss_seidel_sweep (in2, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SR(); di = gauss_seidel_sweep (top, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC(); /* blocking comm, due to symmetry */
      }
      else
      {
	SR(); di = gauss_seidel_sweep (top, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Send (top_pattern); EC();
	SR(); di = gauss_seidel_sweep (in2, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Recv (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
	di = gauss_seidel_loop (mid, 0, mycolor, color, gs, ldy, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
	if (reverse) /* symmetry, but blocking comm at the end */
	{
	  SR(); di = gauss_seidel_sweep (in1, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	  SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	  SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	}
	else /* symmetry broken, but non-blocking comm used again */
	{
	  SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	  SC(); COM_Send (bot_pattern); EC();
	  SR(); di = gauss_seidel_sweep (in1, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	  SC(); COM_Recv (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	}
      }
    }
    break;
    case GS_NOB_MID_THREAD:
    {
      if (reverse && gs->iters % 2)
      {
	SR(); di = gauss_seidel_sweep (bot, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Send (bot_pattern); EC();
	SR(); di = gauss_seidel_sweep (in1, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Recv (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	gauss_seidel_thread_run (data);
	SR(); di = gauss_seidel_sweep (in2, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	di = gauss_seidel_thread_wait (data, &errup, &errlo); dimax = MAX (dimax, di); /* receive all bot to top */
	SR(); di = gauss_seidel_sweep (top, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC(); /* blocking, due to symmetry */
      }
      else
      {
	SR(); di = gauss_seidel_sweep (top, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Send (top_pattern); EC();
	SR(); di = gauss_seidel_sweep (in2, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Recv (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
	gauss_seidel_thread_run (data);
	SR(); di = gauss_seidel_sweep (in1, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	di = gauss_seidel_thread_wait (data, &errup, &errlo); dimax = MAX (dimax, di); /* receive all top to bot */
	SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC(); /* blocking, due to symmetry */
      }
    }
    break;
    case GS_NOB_MID_TO_ALL:
    case GS_NOB_MID_TO_ONE:
    {
      if (reverse && gs->iters % 2)
      {
	SR(); di = gauss_seidel_sweep (bot, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Send (bot_pattern); EC();
	SR(); di = gauss_seidel_sweep (in1, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Recv (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	SMC(); LOCDYN_Union_Gather (upattern); EMC();
	SMR(); di = gauss_seidel_sweep (umid, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); EMR();
	SMC(); LOCDYN_Union_Scatter (upattern); EMC();
	SR(); di = gauss_seidel_sweep (in2, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SR(); di = gauss_seidel_sweep (top, 1, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Repeat (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC(); /* blocking, due to symmetry */
      }
      else
      {
	SR(); di = gauss_seidel_sweep (top, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Send (top_pattern); EC();
	SR(); di = gauss_seidel_sweep (in2, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	SC(); COM_Recv (top_pattern); REXT_recv (ldy, recv_top, nrecv_top); EC();
	SMC(); LOCDYN_Union_Gather (upattern); EMC();
	SMR(); di = gauss_seidel_sweep (umid, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); EMR();
	SMC(); LOCDYN_Union_Scatter (upattern); EMC();
	if (reverse) /* symmetry: top - in1 - mid - in2 - bot */
	{
	  SR(); di = gauss_seidel_sweep (in1, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	  SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	  SC(); COM_Repeat (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC(); /* blocking, due to symmetry */
	}
	else /* break symmetry: top - in1 - mid - bot - in1 */
	{
	  SR(); di = gauss_seidel_sweep (bot, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	  SC(); COM_Send (bot_pattern); EC(); /* non-blocking comm again */
	  SR(); di = gauss_seidel_sweep (in1, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); ER();
	  SC(); COM_Recv (bot_pattern); REXT_recv (ldy, recv_bot, nrecv_bot); EC();
	}
      }
    }
    break;
    }

    ASSERT_DEBUG (REXT_alldone (ldy), "All external reactions should be done by now");

    /* get maximal iterations count of a diagonal block solver */
    MPI_Allreduce (&dimax, &diagiters, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

    if (diagiters > gs->diagmaxiter || diagiters < 0)
    {
      switch ((int) gs->failure)
      {
      case GS_FAILURE_EXIT:
	THROW (ERR_GAUSS_SEIDEL_DIAGONAL_DIVERGED);
	break;
      }
    }

    /* sum up error */
    errloc [0] = errup, errloc [1] = errlo;
    MPI_Allreduce (errloc, errsum, 2, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    errup = errsum [0], errlo = errsum [1];

    /* calculate relative error */
    error = sqrt (errup) / sqrt (MAX (errlo, 1.0));
    if (gs->history) gs->rerhist [gs->iters] = error;

    if (rank == 0 && gs->iters % div == 0 && gs->verbose) printf (fmt, gs->iters, error), div *= 2;
  }
  while (++ gs->iters < gs->maxiter && error > gs->epsilon);

  SE();

  if (rank == 0 && gs->verbose) printf (fmt, gs->iters, error);

  if (upattern) LOCDYN_Union_Destroy (upattern);
  if (data) gauss_seidel_thread_destroy (data);
  SET_Free (setmem, &bot);
  SET_Free (setmem, &mid);
  SET_Free (setmem, &top);
  SET_Free (setmem, &inb);
  SET_Free (setmem, &in1);
  SET_Free (setmem, &in2);
  COM_Free (bot_pattern);
  COM_Free (top_pattern);
  free (send_bot);
  free (recv_bot);
  free (send_top);
  free (recv_top);
  free (color);

  if (gs->iters >= gs->maxiter)
  {
    gs->error = GS_DIVERGED;

    switch (gs->failure)
    {
    case GS_FAILURE_CONTINUE:
      break;
    case GS_FAILURE_EXIT:
      THROW (ERR_GAUSS_SEIDEL_DIVERGED);
      break;
    case GS_FAILURE_CALLBACK:
      gs->callback (gs->data);
      break;
    }
  }

  EE ();
}

#else  /* ~MPI */
/* run serial solver */
void GAUSS_SEIDEL_Solve (GAUSS_SEIDEL *gs, LOCDYN *ldy)
{
  double error, step;
  int diagiters;
  short dynamic;
  char fmt [512];
  int div = 10;
  DIAB *end;

  if (gs->verbose) sprintf (fmt, "GAUSS_SEIDEL: iteration: %%%dd  error:  %%.2e\n", (int)log10 (gs->maxiter) + 1);

  if (gs->history) gs->rerhist = realloc (gs->rerhist, gs->maxiter * sizeof (double));

  if (gs->reverse && ldy->dia) for (end = ldy->dia; end->n; end = end->n); /* find last block for the backward run */
  else end = NULL;

  dynamic = DOM(ldy->dom)->dynamic;
  step = DOM(ldy->dom)->step;
  gs->error = GS_OK;
  gs->iters = 0;
  do
  {
    double errup = 0.0,
	   errlo = 0.0;
    OFFB *blk;
    DIAB *dia;
   
    for (dia = end && gs->iters % 2 ? end : ldy->dia; dia; dia = end && gs->iters % 2 ? dia->p : dia->n) /* run forward and backward alternately */
    {
      double R0 [3],
	     B [3],
	     *R = dia->R;

      /* prefetch reactions */
      for (blk = dia->adj; blk; blk = blk->n)
      {
	COPY (blk->dia->R, blk->R);
      }

      /* compute local free velocity */
      COPY (dia->B, B);
      for (blk = dia->adj; blk; blk = blk->n)
      {
	double *W = blk->W,
	       *R = blk->R;
	NVADDMUL (B, W, R, B);
      }
      
      COPY (R, R0); /* previous reaction */

      /* solve local diagonal block problem */
      CON *con = dia->con;
      diagiters = solver (gs, dynamic, step, con->kind, &con->mat, con->gap, con->Z, con->base, dia, B);

      if (diagiters > gs->diagmaxiter || diagiters < 0)
      {
	if (diagiters < 0) gs->error = GS_DIAGONAL_FAILED;
	else gs->error = GS_DIAGONAL_DIVERGED;

	switch (gs->failure)
	{
	case GS_FAILURE_CONTINUE:
	  COPY (R0, R); /* use previous reaction */
	  break;
	case GS_FAILURE_EXIT:
	  THROW (ERR_GAUSS_SEIDEL_DIAGONAL_DIVERGED);
	  break;
	case GS_FAILURE_CALLBACK:
	  gs->callback (gs->data);
	  break;
	}
      }

      /* accumulate relative
       * error components */
      SUB (R, R0, R0);
      errup += DOT (R0, R0);
      errlo += DOT (R, R);
    }

    /* calculate relative error */
    error = sqrt (errup) / sqrt (MAX (errlo, 1.0));
    if (gs->history) gs->rerhist [gs->iters] = error;

    if (gs->iters % div == 0 && gs->verbose) printf (fmt, gs->iters, error), div *= 2;
  }
  while (++ gs->iters < gs->maxiter && error > gs->epsilon);

  if (gs->verbose) printf (fmt, gs->iters, error);

  if (gs->iters >= gs->maxiter)
  {
    gs->error = GS_DIVERGED;

    switch (gs->failure)
    {
    case GS_FAILURE_CONTINUE:
      break;
    case GS_FAILURE_EXIT:
      THROW (ERR_GAUSS_SEIDEL_DIVERGED);
      break;
    case GS_FAILURE_CALLBACK:
      gs->callback (gs->data);
      break;
    }
  }
}
#endif

/* return faulure string */
char* GAUSS_SEIDEL_Failure (GAUSS_SEIDEL *gs)
{
  switch (gs->failure)
  {
  case GS_FAILURE_CONTINUE: return "FAILURE_CONTINUE";
  case GS_FAILURE_EXIT: return "FAILURE_EXIT";
  case GS_FAILURE_CALLBACK: return "FAILURE_CALLBACK";
  }

  return NULL;
}

/* return diagonal solver string */
char* GAUSS_SEIDEL_Diagsolver (GAUSS_SEIDEL *gs)
{
  switch (gs->diagsolver)
  {
  case GS_PROJECTED_GRADIENT: return "PROJECTED_GRADIENT";
  case GS_DE_SAXE_AND_FENG: return "DE_SAXE_AND_FENG";
  case GS_SEMISMOOTH_NEWTON: return "SEMISMOOTH_NEWTON";
  }

  return NULL;
}

/* return error string */
char* GAUSS_SEIDEL_Error (GAUSS_SEIDEL *gs)
{
  switch (gs->error)
  {
  case GS_OK: return "OK";
  case GS_DIVERGED: return "DIVERGED";
  case GS_DIAGONAL_DIVERGED: return "DIAGONAL_DIVERGED";
  case GS_DIAGONAL_FAILED: return "DIAGONAL_FAILED";
  }

  return NULL;
}

/* return history flag string */
char* GAUSS_SEIDEL_History (GAUSS_SEIDEL *gs)
{
  switch (gs->history)
  {
  case GS_ON: return "ON";
  case GS_OFF: return "OFF";
  }

  return NULL;
}

/* return reverse flag string */
char* GAUSS_SEIDEL_Reverse (GAUSS_SEIDEL *gs)
{
  switch (gs->reverse)
  {
  case GS_ON: return "ON";
  case GS_OFF: return "OFF";
  }

  return NULL;
}

/* return variant string */
char* GAUSS_SEIDEL_Variant (GAUSS_SEIDEL *gs)
{
  switch (gs->variant)
  {
  case GS_MID_LOOP: return "MID_LOOP";
  case GS_MID_THREAD: return "MID_THREAD";
  case GS_MID_TO_ALL: return "MID_TO_ALL";
  case GS_MID_TO_ONE: return "MID_TO_ONE";
  case GS_NOB_MID_LOOP: return "NOB_MID_LOOP";
  case GS_NOB_MID_THREAD: return "NOB_MID_THREAD";
  case GS_NOB_MID_TO_ALL: return "NOB_MID_TO_ALL";
  case GS_NOB_MID_TO_ONE: return "NOB_MID_TO_ONE";
  }

  return NULL;
}

/* free solver */
void GAUSS_SEIDEL_Destroy (GAUSS_SEIDEL *gs)
{
#if MPI
  destroy_mpi (gs);
#endif

  free (gs->rerhist);
  free (gs);
}
