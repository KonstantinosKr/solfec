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
#include "tag.h"
#include "com.h"
#include "lis.h"
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
    if (!isfinite (b[0]+b[1]+b[2])) return -1;

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
  gs->reverse = GS_OFF;

  return gs;
}

#if MPI
/* create rank coloring using adjacency graph between processors derived from the W graph */
static int* processor_coloring (GAUSS_SEIDEL *gs, LOCDYN *ldy)
{
  int i, n, m, ncpu, rank, *color, *size, *disp, *adj;
  SET *adjcpu, *item;
  MEM setmem;
  DIAB *dia;
  OFFB *blk;
  CON *con;

  adjcpu = NULL;
  rank = ldy->dom->rank;
  ncpu = ldy->dom->ncpu;
  MEM_Init (&setmem, sizeof (SET), 128);
  ERRMEM (color = MEM_CALLOC (ncpu * sizeof (int)));
  ERRMEM (disp = malloc (sizeof (int [ncpu + 1])));
  ERRMEM (size = malloc (sizeof (int [ncpu])));

  /* collaps W adjacency into processor adjacency */
  for (dia = ldy->dia; dia; dia = dia->n)
  {
    for (blk = dia->adjext; blk; blk = blk->n)
    {
      con = (CON*) blk->dia;
      SET_Insert (&setmem, &adjcpu, (void*) (long) con->rank, NULL);
    }
  }

  n = SET_Size (adjcpu);
  MPI_Allgather (&n, 1, MPI_INT, size, 1, MPI_INT, MPI_COMM_WORLD);

  for (i = disp [0] = 0; i < ncpu - 1; i ++) disp [i+1] = disp [i] + size [i];
  for (i = 0, item = SET_First (adjcpu); item; i ++, item = SET_Next (item)) color [i] = (int) (long) item->data;

  m = disp [ncpu] = (disp [ncpu-1] + size [ncpu-1]);
  ERRMEM (adj = malloc (sizeof (int [m])));

  MPI_Allgatherv (color, n, MPI_INT, adj, size, disp, MPI_INT, MPI_COMM_WORLD); /* gather graph adjacency */

  for (i = 0; i < ncpu; i ++) color [i] = 0; /* invalidate coloring */

  for (i = 0; i < ncpu; i ++) /* simple BFS coloring */
  {
    int *j, *k;

    if (color [i] == 0) /* if not colored */
    {
      do
      {
	color [i] ++; /* start from first color */

	for (j = &adj[disp[i]], k = &adj[disp[i+1]]; j < k; j ++) /* for each adjacent vertex */
	{
	  if (color [*j] == color [i]) break; /* see whether the trial color exists in the adjacency */
	}
      }
      while (j < k); /* if so try next color */
    }
  }

#if DEBUG
#if 0
  printf ("RANK %d [%d] ADJCPU: ", rank, color [rank]);
  for (item = SET_First (adjcpu); item; item = SET_Next (item)) printf ("%d [%d] ", (int) (long) item->data, color [(int) (long) item->data]);
  printf ("\n");
#endif
  if (rank == 0 && ldy->dom->verbose)
  {
    for (m = i = 0; i < ncpu; i ++) m = MAX (m, color [i]); /* get number of colors */
    printf ("GAUSS_SEIDEL: PROCESSOR COLORS = %d\n", m);
  }
#endif

  MEM_Release (&setmem);
  free (size);
  free (disp);
  free (adj);

  return color;
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

/* receive external reactions */
static void receive_reactions (DOM *dom, COMDATA *recv, int nrecv)
{
  COMDATA *ptr;
  int i, j, *k;
  double *R;
  CON *con;

  for (i = 0, ptr = recv; i < nrecv; i ++, ptr ++)
  {
    for (j = 0, k = ptr->i, R = ptr->d; j < ptr->ints; j ++, k ++, R += 3)
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (dom->conext, (void*) (long) (*k), NULL), "Invalid constraint id");
      COPY (R, con->R);
      con->state |= CON_DONE;
    }
  }
}

static void receive_middle_reactions (DOM *dom, COMDATA *recv, int nrecv, MEM *setmem, SET **midupd)
{
  COMDATA *ptr;
  int i, j, *k;
  CON *con;

  for (i = 0, ptr = recv; i < nrecv; i ++, ptr ++)
  {
    for (j = 0, k = ptr->i; j < ptr->ints; j ++, k ++)
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (dom->conext, (void*) (long) (*k), NULL), "Invalid constraint id");
      SET_Insert (setmem, midupd, con, NULL);
    }
  }
}

/* a single row Gauss-Seidel step */
static int gauss_seidel (GAUSS_SEIDEL *gs, short dynamic, double step, DIAB *dia, double *errup, double *errlo)
{
  double R0 [3], B [3], *R, *W;
  int diagiters;
  OFFB *blk;
  CON *con;

  /* compute local velocity */
  COPY (dia->B, B);
  for (blk = dia->adj; blk; blk = blk->n)
  {
    W = blk->W;
    R = blk->dia->R;
    NVADDMUL (B, W, R, B);
  }
  for (blk = dia->adjext; blk; blk = blk->n)
  {
    con = (CON*) blk->dia;
    W = blk->W;
    R = con->R;
    NVADDMUL (B, W, R, B);
  }
 
  R = dia->R; 
  COPY (R, R0); /* previous reaction */

  /* solve local diagonal block problem */
  con = dia->con;
  diagiters = DIAGONAL_BLOCK_Solver (gs->diagsolver, gs->diagepsilon, gs->diagmaxiter,
	     dynamic, step, con->kind, con->mat.base, con->gap, con->Z, con->base, dia, B);

  if (diagiters > gs->diagmaxiter || diagiters < 0) /* failed */
  {
    COPY (R0, R); /* use previous reaction */
  }

  /* accumulate relative
   * error components */
  SUB (R, R0, R0);
  *errup += DOT (R0, R0);
  *errlo += DOT (R, R);

  return diagiters;
}

/* a Guss-Seidel sweep over a set of blocks */
static int gauss_seidel_sweep (SET *set, int reverse, GAUSS_SEIDEL *gs, short dynamic, double step, double *errup, double *errlo)
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

typedef struct middle_list MIDDLE_LIST;

struct middle_list
{
  DIAB *dia;
  int score;
  MIDDLE_LIST *next;
};

#define MLLE(i, j) ((i)->score <= (j)->score)
IMPLEMENT_LIST_SORT (SINGLE_LINKED, middle_list_sort, MIDDLE_LIST, prev, next, MLLE)

/* perform a Guss-Seidel loop over a set of blocks */
static int gauss_seidel_loop (SET *middle, SET *midupd, int reverse, MEM *setmem, int mycolor, int *color, GAUSS_SEIDEL *gs, LOCDYN *ldy, short dynamic, double step, double *errup, double *errlo)
{
  SET *requs, *ranks, *item, *jtem;
  MIDDLE_LIST *list, *cur;
  int di, dimax;
  DIAB *dia;
  OFFB *blk;
  CON *con;
  MEM lstmem, reqmem;

  MEM_Init (&lstmem, sizeof (MIDDLE_LIST), 128);
  MEM_Init (&reqmem, sizeof (MPI_Request), 128);

  dimax = 0;

  list = NULL;

  requs = NULL;

  /* post receives first */
  for (item = SET_First (midupd); item; item = SET_Next (item))
  {
    MPI_Request *req;
    con = item->data;
    ASSERT_DEBUG ((con->state & CON_DONE) == 0 && con->dia == NULL, "Invalid external constraint");
    ERRMEM (req = MEM_Alloc (&reqmem));
    MPI_Irecv (con->R, 3, MPI_DOUBLE, con->rank, TAG_LAST+con->id, MPI_COMM_WORLD, req);
    con->dia = (DIAB*) req; /* use spare (NULL) DIAB pointer for the request */
  }

  for (item = SET_First (middle); item; item = SET_Next (item))
  {
    ERRMEM (cur = MEM_Alloc (&lstmem));
    cur->dia = item->data;
    cur->score = 0;
    for (blk = cur->dia->adjext; blk; blk = blk->n)
    {
      con = (CON*) blk->dia;
      cur->score = MIN (cur->score, -color [con->rank]);
    }
    cur->next = list;
    list = cur;
  }

  list = middle_list_sort (list);

  for (cur = list; cur; cur = cur->next)
  {
    dia = cur->dia;

    for (ranks = NULL, blk = dia->adjext; blk; blk = blk->n)
    {
      con = (CON*) blk->dia;
      if (mycolor < color [con->rank] && (con->state & CON_DONE) == 0)
      {
	MPI_Status sta;
        MPI_Wait ((MPI_Request*)con->dia, &sta);
	con->state |= CON_DONE;
      }

      SET_Insert (setmem, &ranks, (void*) (long) con->rank, NULL);
    }

    di = gauss_seidel (gs, dynamic, step, dia, errup, errlo);
    dimax = MAX (dimax, di);

    con = dia->con;
    for (jtem = SET_First (ranks); jtem; jtem = SET_Next (jtem))
    {
      MPI_Request *req;
      ERRMEM (req = MEM_Alloc (&reqmem));
      MPI_Isend (con->R, 3, MPI_DOUBLE, (int) (long) jtem->data, TAG_LAST+con->id, MPI_COMM_WORLD, req);
      SET_Insert (setmem, &requs, req, NULL);
    }

    SET_Free (setmem, &ranks);
  }

  for (item = SET_First (requs); item; item = SET_Next (item))
  {
    MPI_Status sta;
    MPI_Wait (item->data, &sta);
  }

  for (item = SET_First (midupd); item; item = SET_Next (item))
  {
    con = item->data;
    if ((con->state & CON_DONE) == 0)
    {
      MPI_Status sta;
      MPI_Wait ((MPI_Request*)con->dia, &sta);
      con->state |= CON_DONE;
    }
    con->dia = NULL;
  }

  MEM_Release (&lstmem);
  MEM_Release (&reqmem);

  return dimax;
}

/* unset all */
static void unset_all (LOCDYN *ldy)
{
  DIAB *dia;
  OFFB *blk;
  CON *con;

  for (dia = ldy->dia; dia; dia = dia->n)
  {
    for (blk = dia->adjext; blk; blk = blk->n)
    {
      con = (CON*) blk->dia;
      con->state &= ~CON_DONE;
    }
  }
}

/* test all set */
static int all_set (LOCDYN *ldy)
{
  DIAB *dia;
  OFFB *blk;
  CON *con;

  for (dia = ldy->dia; dia; dia = dia->n)
  {
    for (blk = dia->adjext; blk; blk = blk->n)
    {
      con = (CON*) blk->dia;
      if ((con->state & CON_DONE) == 0)
      {
	return 0;
      }
    }
  }

  return 1;
}

/* run parallel solver */
void GAUSS_SEIDEL_Solve (GAUSS_SEIDEL *gs, LOCDYN *ldy)
{
  int div = 10, di, dimax, diagiters, mycolor, rank, *color;
  double error, step;
  short dynamic;
  char fmt [512];
  SOLFEC *solfec;
  SET *ranks;
  MEM setmem;
  DIAB *dia;
  OFFB *blk;
  CON *con;
  DOM *dom;

  SET *bottom    = NULL,
      *top       = NULL,
      *middle    = NULL,
      *internal  = NULL,
      *midupd    = NULL,
      *int1      = NULL,
      *int2      = NULL;

  int size1 = 0,
      size2 = 0,
      size3 = 0,
      size4,
      size5;

  void *bot_pattern, /* communication pattern when sending from lower to higher processors */
       *top_pattern; /* the reverse communication pattern */

  COMDATA *send_bot, *recv_bot, *ptr_bot,
	  *send_top, *recv_top, *ptr_top;

  int size_bot, nsend_bot, nrecv_bot,
      size_top, nsend_top, nrecv_top;

  dom = ldy->dom;

  if (dom->verbose) sprintf (fmt, "GAUSS_SEIDEL: iteration: %%%dd  error:  %%.2e\n", (int)log10 (gs->maxiter) + 1);

  if (gs->history) gs->rerhist = realloc (gs->rerhist, gs->maxiter * sizeof (double));

  color = processor_coloring (gs, ldy); /* color processors */
  solfec = dom->solfec;
  rank = dom->rank;
  mycolor = color [rank];

  MEM_Init (&setmem, sizeof (SET), 256);

  /* create block sets */
  for (dia = ldy->dia; dia; dia = dia->n)
  {
    int lo = 0, hi = 0;

    for (blk = dia->adjext; blk; blk = blk->n)
    {
      con = (CON*) blk->dia;
      int adjcolor = color [con->rank];

      if (adjcolor < mycolor) hi ++;
      else lo ++;
    }

    if (lo && hi) SET_Insert (&setmem, &middle, dia, NULL);
    else if (lo) SET_Insert (&setmem, &bottom, dia, NULL), size1 ++;
    else if (hi) SET_Insert (&setmem, &top, dia, NULL), size2 ++;
    else SET_Insert (&setmem, &internal, dia, NULL), size3 ++;
  }

  /* size1 + |int2| = size2 + |int1|
   * |int1| + |int2| = size3
   * -------------------------------
   * |int2| = (size3 + size2 - size1) / 2
   */

  size4 = (size3 + size2 - size1) / 2;
  size4 = MAX (0, size4);
  size5 = 0;

  /* create int1 and int2 such that: |bot| + |int2| = |top| + |int1| */
  for (SET *item = SET_First (internal); item; item = SET_Next (item))
  {
    dia = item->data;

    if (size5 < size4) SET_Insert (&setmem, &int2, dia, NULL), size5 ++; /* FIXME: degrees were used */
    else SET_Insert (&setmem, &int1, dia, NULL);
  }

#if DEBUG
  if (dom->verbose)
  {
    int sizes [5] = {SET_Size (bottom), SET_Size (middle), SET_Size (top), SET_Size (int1), SET_Size (int2)}, result [5];

    MPI_Reduce (sizes, result, 5, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) printf ("GAUSS_SEIDEL: |BOTTOM| = %d, |MIDDLE| = %d, |TOP| = %d, |INT1| = %d, |INT2| = %d\n", result [0], result [1], result [2], result [3], result [4]);
  }
#endif

  size_bot = size_top = 512;
  nsend_bot = nsend_top = 0;

  ERRMEM (send_bot = MEM_CALLOC (size_bot * sizeof (COMDATA)));
  ERRMEM (send_top = MEM_CALLOC (size_top * sizeof (COMDATA)));

  ptr_bot = send_bot;
  ptr_top = send_top;

  /* prepare bottom send buffer */
  for (SET *item = SET_First (bottom); item; item = SET_Next (item))
  {
    dia = item->data;

    for (ranks = NULL, blk = dia->adjext; blk; blk = blk->n)
    { 
      con = (CON*) blk->dia;
      SET_Insert (&setmem, &ranks, (void*) (long) con->rank, NULL);
    }

    for (SET *jtem = SET_First (ranks); jtem; jtem = SET_Next (jtem))
    {
      ptr_bot->rank = (int) (long) jtem->data;
      ptr_bot->ints = 1;
      ptr_bot->doubles = 3;
      ptr_bot->i = (int*) &dia->con->id;
      ptr_bot->d = dia->R;
      ptr_bot = sendnext (++ nsend_bot, &size_bot, &send_bot);
    }

    SET_Free (&setmem, &ranks);
  }

  /* prepare top send buffer */
  for (SET *item = SET_First (top); item; item = SET_Next (item))
  {
    dia = item->data;

    for (ranks = NULL, blk = dia->adjext; blk; blk = blk->n)
    { 
      con = (CON*) blk->dia;
      SET_Insert (&setmem, &ranks, (void*) (long) con->rank, NULL);
    }

    for (SET *jtem = SET_First (ranks); jtem; jtem = SET_Next (jtem))
    {
      ptr_top->rank = (int) (long) jtem->data;
      ptr_top->ints = 1;
      ptr_top->doubles = 3;
      ptr_top->i = (int*) &dia->con->id;
      ptr_top->d = dia->R;
      ptr_top = sendnext (++ nsend_top, &size_top, &send_top);
    }

    SET_Free (&setmem, &ranks);
  }

  bot_pattern = COM_Pattern (MPI_COMM_WORLD, TAG_GAUSS_SEIDEL_BOTTOM, send_bot, nsend_bot, &recv_bot, &nrecv_bot);
  top_pattern = COM_Pattern (MPI_COMM_WORLD, TAG_GAUSS_SEIDEL_TOP, send_top, nsend_top, &recv_top, &nrecv_top);

  /* discover which external constraints from top and bottom sets are updated by middle nodes */

  COMDATA *send, *recv, *ptr;
  int nsend, nrecv, size;

  size = 128;
  nsend = 0;

  ERRMEM (send = MEM_CALLOC (size * sizeof (COMDATA)));

  ptr = send;

  for (SET *item = SET_First (middle); item; item = SET_Next (item))
  {
    dia = item->data;

    for (ranks = NULL, blk = dia->adjext; blk; blk = blk->n)
    { 
      con = (CON*) blk->dia;
      SET_Insert (&setmem, &ranks, (void*) (long) con->rank, NULL);
    }

    for (SET *jtem = SET_First (ranks); jtem; jtem = SET_Next (jtem))
    {
      ptr->rank = (int) (long) jtem->data;
      ptr->ints = 1;
      ptr->doubles = 0;
      ptr->i = (int*) &dia->con->id;
      ptr->d = NULL;
      ptr= sendnext (++ nsend, &size, &send);
    }

    SET_Free (&setmem, &ranks);
  }

  COMALL (MPI_COMM_WORLD, send, nsend, &recv, &nrecv);

  receive_middle_reactions (dom, recv, nrecv, &setmem, &midupd);

  free (send);
  free (recv);

  dynamic = dom->dynamic;
  step = dom->step;
  gs->error = GS_OK;
  gs->iters = 0;
  dimax = 0;
  do
  {
    double errup = 0.0,
           errlo = 0.0,
	   errloc [2],
	   errsum [2];

    unset_all (ldy);

    di = gauss_seidel_sweep (top, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
    COM_Send (top_pattern);
    di = gauss_seidel_sweep (int2, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di); /* large |top| => large |int2| */
    COM_Recv (top_pattern); receive_reactions (dom, recv_top, nrecv_top);

    di = gauss_seidel_loop (middle, midupd, 0, &setmem, mycolor, color, gs, ldy, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);

    di = gauss_seidel_sweep (bottom, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
    COM_Send (bot_pattern);
    di = gauss_seidel_sweep (int1, 0, gs, dynamic, step, &errup, &errlo); dimax = MAX (dimax, di);
    COM_Recv (bot_pattern); receive_reactions (dom, recv_bot, nrecv_bot);

    ASSERT_DEBUG (all_set (ldy), "Not all external reactions were updated");

    /* sum up error */
    errloc [0] = errup, errloc [1] = errlo;
    MPI_Allreduce (errloc, errsum, 2, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    errup = errsum [0], errlo = errsum [1];

    /* calculate relative error */
    error = sqrt (errup) / sqrt (MAX (errlo, 1.0));

    if (gs->history) gs->rerhist [gs->iters] = error;

    if (rank == 0 && gs->iters % div == 0 && dom->verbose) printf (fmt, gs->iters, error), div *= 2;
  }
  while (++ gs->iters < gs->maxiter && error > gs->epsilon);

  if (rank == 0 && dom->verbose) printf (fmt, gs->iters, error);

  COM_Free (bot_pattern);
  COM_Free (top_pattern);
  MEM_Release (&setmem);
  free (send_bot);
  free (recv_bot);
  free (send_top);
  free (recv_top);
  free (color);

  /* get maximal iterations count of a diagonal block solver (this has been
   * delayed until here to minimize small communication within the loop) */
  MPI_Allreduce (&dimax, &diagiters, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

  if (diagiters > gs->diagmaxiter || diagiters < 0)
  {
    if (diagiters < 0) gs->error = GS_DIAGONAL_FAILED;
    else gs->error = GS_DIAGONAL_DIVERGED;

    switch ((int) gs->failure)
    {
    case GS_FAILURE_EXIT:
      THROW (ERR_GAUSS_SEIDEL_DIAGONAL_DIVERGED);
      break;
    case GS_FAILURE_CALLBACK:
      gs->callback (gs->data);
      break;
    }
  }
  else if (gs->iters >= gs->maxiter)
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
#else
/* run serial solver */
void GAUSS_SEIDEL_Solve (GAUSS_SEIDEL *gs, LOCDYN *ldy)
{
  int verbose, diagiters;
  double error, step;
  short dynamic;
  char fmt [512];
#if !MPI
  int div = 10;
#endif
  DIAB *end;

  verbose = ldy->dom->verbose;

  if (verbose) sprintf (fmt, "GAUSS_SEIDEL: iteration: %%%dd  error:  %%.2e\n", (int)log10 (gs->maxiter) + 1);

  if (gs->history) gs->rerhist = realloc (gs->rerhist, gs->maxiter * sizeof (double));

  if (gs->reverse && ldy->dia) for (end = ldy->dia; end->n; end = end->n); /* find last block for the backward run */
  else end = NULL;

  dynamic = ldy->dom->dynamic;
  step = ldy->dom->step;
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

      /* compute local free velocity */
      COPY (dia->B, B);
      for (blk = dia->adj; blk; blk = blk->n)
      {
	double *W = blk->W,
	       *R = blk->dia->R;
	NVADDMUL (B, W, R, B);
      }
#if MPI
      for (blk = dia->adjext; blk; blk = blk->n)
      {
	CON *con = (CON*) blk->dia;
	double *W = blk->W,
	       *R = con->R;
	NVADDMUL (B, W, R, B);
      }
#endif
      
      COPY (R, R0); /* previous reaction */

      /* solve local diagonal block problem */
      CON *con = dia->con;
      diagiters = DIAGONAL_BLOCK_Solver (gs->diagsolver, gs->diagepsilon, gs->diagmaxiter,
	         dynamic, step, con->kind, con->mat.base, con->gap, con->Z, con->base, dia, B);

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
#if !MPI
    if (gs->iters % div == 0 && verbose) printf (fmt, gs->iters, error), div *= 2;
#endif
  }
  while (++ gs->iters < gs->maxiter && error > gs->epsilon);

#if MPI
  DOM_Update_External_Reactions (ldy->dom, 0);
#else
  if (verbose) printf (fmt, gs->iters, error);
#endif

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

/* free solver */
void GAUSS_SEIDEL_Destroy (GAUSS_SEIDEL *gs)
{
  free (gs->rerhist);
  free (gs);
}

/* diagonal block solver */
int DIAGONAL_BLOCK_Solver (GSDIAS diagsolver, double diagepsilon, int diagmaxiter,
  short dynamic, double step, short kind, SURFACE_MATERIAL *mat, double gap,
  double *Z, double *base, DIAB *dia, double *B)
{
  switch (kind)
  {
  case CONTACT:
    switch (mat->model)
    {
    case SIGNORINI_COULOMB:
      switch (diagsolver)
      {
      case GS_PROJECTED_GRADIENT:
	return projected_gradient (dynamic, diagepsilon, diagmaxiter, step, mat->friction,
			   mat->restitution, gap, dia->rho, dia->W, B, dia->V, dia->U, dia->R);
      case GS_DE_SAXE_AND_FENG:
	return de_saxe_and_feng (dynamic, diagepsilon, diagmaxiter, step, mat->friction,
			 mat->restitution, gap, dia->rho, dia->W, B, dia->V, dia->U, dia->R);
      case GS_SEMISMOOTH_NEWTON:
	return semismooth_newton (dynamic, diagepsilon, diagmaxiter, step, mat->friction,
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
    return riglnk (dynamic, diagepsilon, diagmaxiter,
	    step, base, Z, dia->W, B, dia->V, dia->U, dia->R);
  }

  return 0;
}
