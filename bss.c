/*
 * bss.c
 * Copyright (C) 2010 Tomasz Koziara (t.koziara AT gmail.com)
 * -------------------------------------------------------------------
 * body space solver
 */

/* This file is part of Solfec.
 * Solfec is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * Solfec is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Solfec. If not, see <http://www.gnu.org/licenses/>. */

#include <complex.h>
#include <stdlib.h>
#include <float.h>

#include "bss.h"
#include "dom.h"
#include "alg.h"
#include "bla.h"
#include "lap.h"
#include "err.h"
#include "vic.h"
#include "ext/krylov/krylov.h"

#if MPI
#include "com.h"
#include "pck.h"
#endif

#define SMOOTHING_EPSILON	1E-10  /* TODO */
#define ABSTOL			1E-15  /* TODO */

typedef struct bss_con_data BSS_CON_DATA;
typedef struct bss_data BSS_DATA;
typedef struct vector VECTOR;

struct bss_con_data
{
  MX *mH; /* master H */
  int mi; /* shift to mH in global velocity space */

  MX *sH;
  int si;

  int n; /* constraint index */

  double *R, /* con->R */
	 *U, /* con->U */
         *V, /* con->dia->V */
	 *B, /* con->dia->B */
	 *W, /* con->dia->W */
	 *A, /* con->dia->A */
	  X [9],
	  Y [9],
	  T [9],
	  RE [3];

  short kind; /* con->kind */

  CON *con; /* constraint */
};

struct bss_data
{
  int nprimal, /* SUM [bod in dom->bod] { bod->dofs } */ 
      ndual; /* 3 * dom->ncon */

  BSS_CON_DATA *dat; /* constraints data of dom->ncon */

  VECTOR *b, /* right hand side (of size ndual) */
         *x, /* reactions (-||-) */
	 *y, /* increments of velocities (-||-) */
	 *z; /* increments of reactions (-||-) */

  double *r, /* global reaction (of size nprimal) */
	 *u, /* global velocity (-||-) */
          resnorm, /* |A z - b| */
	  znorm, /* |z| */
	  delta; /* regularisation = |A z - b| / |z| */

  int iters; /* linear solver iterations */

  DOM *dom; /* domain */
};

struct vector
{
  double *x;

  int n;
};

static VECTOR* newvector (int n)
{
  VECTOR *v;

  ERRMEM (v = malloc (sizeof (VECTOR)));
  ERRMEM (v->x = MEM_CALLOC (n * sizeof (double)));
  v->n = n;

  return v;
}

/* y = H' x */
static void H_trans_vector (BSS_CON_DATA *dat, int n, double *x, double *y)
{
  for (; n > 0; dat ++, n --)
  {
    double *p = &x [dat->n],
	   *q = &y [dat->mi];

    MX_Matvec (1.0, MX_Tran (dat->mH), p, 1.0, q);

    if (dat->sH)
    {
      q = &y [dat->si];

      MX_Matvec (1.0, MX_Tran (dat->sH), p, 1.0, q);
    }
  }
}

/* y = H' x */
static void H_times_vector (BSS_CON_DATA *dat, int n, double *x, double *y)
{
  for (; n > 0; dat ++, n --)
  {
    double *p = &x [dat->mi],
	   *q = &y [dat->n];

    MX_Matvec (1.0, dat->mH, p, 1.0, q);

    if (dat->sH)
    {
      p = &x [dat->si];

      MX_Matvec (1.0, dat->sH, p, 1.0, q);
    }
  }
}

/* y = alpha W x */
static void W_times_vector (double alpha, BSS_DATA *A, double *x, double *y)
{
  double *r = A->r, *u = A->u;
  BSS_CON_DATA *dat = A->dat;
  DOM *dom = A->dom;
  int n = dom->ncon;
  BODY *bod;

  SETN (r, A->nprimal, 0.0);
  H_trans_vector (dat, n, x, r);

  for (bod = dom->bod; bod; r += bod->dofs, u += bod->dofs, bod = bod->next)
  {
    BODY_Invvec (alpha, bod, r, 0.0, u);
  }

  SETN (y, A->ndual, 0.0);
  H_times_vector (dat, n, A->u, y);
}

/* GMRES interface start */
static char* CAlloc (size_t count, size_t elt_size)
{
  char *ptr;

  ERRMEM (ptr = MEM_CALLOC (count * elt_size));
  return ptr;
}

static int Free (char *ptr)
{
  free (ptr);
  return 0;
}

static int CommInfo (BSS_DATA *A, int *my_id, int *num_procs)
{
#if MPI
  *num_procs = A->dom->ncpu;
  *my_id = A->dom->rank;
#else
  *num_procs = 1;
  *my_id = 0;
#endif
  return 0;
}

static void* CreateVector (VECTOR *a)
{
  VECTOR *v;

  ERRMEM (v = malloc (sizeof (VECTOR)));
  ERRMEM (v->x = MEM_CALLOC (a->n * sizeof (double)));
  v->n = a->n;

  return v;
}

static void* CreateVectorArray (int size, VECTOR *a)
{
  VECTOR **v;
  int i;

  ERRMEM (v = malloc (size * sizeof (VECTOR*)));
  for (i = 0; i < size; i ++)
  {
    v[i] = CreateVector (a);
  }

  return v;
}

static int DestroyVector (VECTOR *a)
{
  free (a->x);
  free (a);

  return 0;
}

static double InnerProd (VECTOR *a, VECTOR *b)
{
  double dot = 0.0, *x, *y, *z;

  for (x = a->x, z = x + a->n, y = b->x; x < z; x ++, y ++)
  {
    dot += (*x) * (*y);
  }

#if MPI
  double val = dot;
  MPI_Allreduce (&val, &dot, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
#endif

  return dot;
}

static int CopyVector (VECTOR *a, VECTOR *b)
{
  double *x, *y, *z;

  for (x = a->x, z = x + a->n, y = b->x; x < z; x ++, y ++)
  {
    (*y) = (*x);
  }

  return 0;
}

static int ClearVector (VECTOR *a)
{
  double *x, *z;

  for (x = a->x, z = x + a->n; x < z; x ++)
  {
    (*x) = 0.0;
  }

  return 0;
}

static int ScaleVector (double alpha, VECTOR *a)
{
  double *x, *z;

  for (x = a->x, z = x + a->n; x < z; x ++)
  {
    (*x) *= alpha;
  }

  return 0;
}

static int  Axpy (double alpha, VECTOR *a, VECTOR *b)
{
  double *x, *y, *z;

  for (x = a->x, z = x + a->n, y = b->x; x < z; x ++, y ++)
  {
    (*y) += alpha * (*x);
  }

  return 0;
}

static void *MatvecCreate (void *A, void *x)
{
  return NULL;
}

static int Matvec (void *matvec_data, double alpha, BSS_DATA *A, VECTOR *x, double beta, VECTOR *y)
{
  double Z [3], *Y, *X, *R, *U, *Q, *u, *q, *r;
  BSS_CON_DATA *dat = A->dat;
  DOM *dom = A->dom;
  int n = dom->ncon;

  W_times_vector (alpha * dom->step, A, x->x, A->y->x); /* dU = alpha h W dR */

  for (u = A->y->x, q = y->x, r = x->x; n > 0; dat ++, n --)
  {
    U = &u [dat->n];
    Q = &q [dat->n];
    R = &r [dat->n];

    if (dat->kind == VELODIR || dat->kind == FIXDIR)
    {
      Z [0] = R [0] * alpha;
      Z [1] = R [1] * alpha;
      Z [2] = U [2];

      U = Z;
    }
    else if (dat->kind == CONTACT)
    {
      X = dat->X;
      Y = dat->Y;

      NVMUL (X, U, Z);
      NVADDMUL (Z, Y, R, Z);  /* Z = X dU + Y *dR */

      U = Z;
    }

    ADDMUL (U, beta, Q, Q); 
  }

  if (A->delta > 0.0) Axpy (alpha * A->delta, x, y);

  return 0;
}

static int MatvecDestroy (void *matvec_data)
{
  return 0;
}

static int PrecondSetup (void *vdata, void *A, void *b, void *x)
{
  return 0;
}

static int Precond (void *vdata, BSS_DATA *A, VECTOR *b, VECTOR *x)
{
  BSS_CON_DATA *dat, *end;
  double *T, *p, *q;

  for (dat = A->dat, end = dat + A->dom->ncon; dat != end; dat ++)
  {
    p = &b->x[dat->n];
    q = &x->x[dat->n];
    T = dat->T;
    NVMUL (T, p, q);
  }

  return 0;
}
/* GMRES interface end */

/* update previous and free local velocities V, B */
static void update_V_and_B (DOM *dom)
{
  double X [6], *V, *B;
  CON *con;

  for (con = dom->con; con; con = con->next)
  {
    V = con->dia->V;
    B = con->dia->B;

    SET (V, 0);
    SET (B, 0);

#if MPI
    if (con->master->flags & BODY_PARENT) /* local parent */
#endif
    {
      BODY_Local_Velo (con->master, mshp(con), mgobj(con), con->mpnt, con->base, X, X+3);
      ADD (V, X, V);
      ADD (B, X+3, B);
    }

    if (con->slave)
    {
#if MPI
      if (con->slave->flags & BODY_PARENT) /* local slave */
#endif
      {
        BODY_Local_Velo (con->slave, sshp(con), sgobj(con), con->spnt, con->base, X, X+3);
	SUB (V, X, V); /* relative = master - slave */
	SUB (B, X+3, B);
      }
    }
  }

#if MPI
  /* in parallel, parent bodies calculate local velocities of their external constraints and send them to the constraint parents;
   * upon arrival these are added or subtracted from the constraint parents B and V members, depending on the master/slave relation */
  int nsend, nrecv, *isize, *dsize, i, *j, *k;
  COMDATA *send, *recv, *ptr;
  double *D;
  BODY *bod;
  SET *item;

  nsend = dom->ncpu;
  ERRMEM (send = MEM_CALLOC (sizeof (COMDATA [nsend])));
  ERRMEM (isize = MEM_CALLOC (sizeof (int [nsend])));
  ERRMEM (dsize = MEM_CALLOC (sizeof (int [nsend])));

  for (i = 0; i < nsend; i ++) send [i].rank = i;

  for (bod = dom->bod; bod; bod = bod->next) /* for all parents */
  {
    for (item = SET_First (bod->con); item; item = SET_Next (item))
    {
      con = item->data;
      if (con->state & CON_EXTERNAL) /* needs local velocity update */
      {
	ASSERT_DEBUG (MAP_Find (dom->conext, (void*) (long) con->id, NULL), "Invalid external constraint %s %d", CON_Kind (con), con->id);
	i = con->rank;
	ptr = &send [i];
	if (bod == con->master)
	{
	  pack_int (&isize [i], &ptr->i, &ptr->ints, -con->id);
	  BODY_Local_Velo (bod, mshp(con), mgobj(con), con->mpnt, con->base, X, X+3);
	}
	else
	{
	  pack_int (&isize [i], &ptr->i, &ptr->ints, con->id);
	  BODY_Local_Velo (bod, sshp(con), sgobj(con), con->spnt, con->base, X, X+3);
	}
        pack_doubles (&dsize [i], &ptr->d, &ptr->doubles, X, 6);
      }
    }
  }

  COMALL (MPI_COMM_WORLD, send, nsend, &recv, &nrecv);

  for (i = 0; i < nrecv; i ++)
  {
    ptr = &recv [i];
    for (j = ptr->i, k = j + ptr->ints, D = ptr->d; j < k; j ++, D += 6)
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (dom->idc, (void*) (long) ABS(*j), NULL), "Invalid constraint id: %d", ABS(*j));
      V = con->dia->V;
      B = con->dia->B;
      if ((*j) < 0) /* master */
      {
	ADD (V, D, V);
	ADD (B, D+3, B);
      }
      else /* slave */
      {
	SUB (V, D, V);
	SUB (B, D+3, B);
      }
    }
  }

  for (i = 0; i < nsend; i ++)
  {
    ptr = &send [i];
    free (ptr->i);
    free (ptr->d);
  }
  free (send);
  free (isize);
  free (dsize);
  free (recv); /* includes recv[]->i and recv[]->d memory */
#endif
}

/* create constraints data */
static BSS_CON_DATA *create_constraints_data (DOM *dom, double *x)
{
  BSS_CON_DATA *out, *dat;
  double step;
  BODY *bod;
  CON *con;
  MAP *map;
  int n;

  ERRMEM (out = MEM_CALLOC (dom->ncon * sizeof (BSS_CON_DATA)));
  step = dom->step;

  for (bod = dom->bod, map = NULL, n = 0; bod; n += bod->dofs, bod = bod->next)
  {
    MAP_Insert (NULL, &map, bod, (void*) (long) n, NULL);
  }

  for (con = dom->con, dat = out, n = 0; con; con = con->next, dat ++, n += 3, x += 3)
  {
    DIAB *dia = con->dia;
    BODY *m = con->master,
	 *s = con->slave;
    void *mgobj = mgobj(con),
	 *sgobj;
    SHAPE *mshp = mshp(con),
	  *sshp;
    double *mpnt = con->mpnt,
	   *spnt = con->spnt,
	   *base = con->base;
    MX_DENSE_PTR (W, 3, 3, dia->W);
    MX_DENSE_PTR (A, 3, 3, dia->A);
    short up = dia->W[8] == 0.0;
    MX *prod;

    if (s)
    {
      sgobj = sgobj(con);
      sshp = sshp(con);
    }

    dat->mH = BODY_Gen_To_Loc_Operator (m, mshp, mgobj, mpnt, base);
    dat->mi = (int) (long) MAP_Find (map, m, NULL);

    if (up)
    {
      prod = MX_Matmat (1.0, m->inverse, MX_Tran (dat->mH), 0.0, NULL);
      MX_Matmat (step, dat->mH, prod, 0.0, &W); /* H * inv (M) * H^T */
      MX_Destroy (prod);
    }

    if (s)
    {
      dat->sH = BODY_Gen_To_Loc_Operator (s, sshp, sgobj, spnt, base);
      dat->si = (int) (long) MAP_Find (map, s, NULL);
      MX_Scale (dat->sH, -1.0);

      if (up)
      {
	prod = MX_Matmat (1.0, s->inverse, MX_Tran (dat->sH), 0.0, NULL);
	MX_Matmat (step, dat->sH, prod, 1.0, &W); /* H * inv (M) * H^T */
	MX_Destroy (prod);
      }
    }

    if (up)
    {
      MX_Copy (&W, &A);
      MX_Inverse (&A, &A);
    }

    dat->n = n;
    dat->R = con->R;
    dat->U = con->U;
    dat->V = dia->V;
    dat->B = dia->B;
    dat->W = dia->W;
    dat->A = dia->A;
    dat->kind = con->kind;
    dat->con = con;

    COPY (dat->R, x); /* initialize with previous solution */
  }

  MAP_Free (NULL, &map);

  return out;
}

static void destroy_constraints_data (BSS_CON_DATA *dat, int n)
{
  BSS_CON_DATA *ptr = dat;

  for (; n > 0; dat ++, n --)
  {
    MX_Destroy (dat->mH);
    if (dat->sH) MX_Destroy (dat->sH);
  }

  free (ptr);
}

/* create BSS data */
static BSS_DATA *create_data (DOM *dom)
{
  BSS_DATA *A;
  BODY *bod;

  update_V_and_B (dom);

  ERRMEM (A = MEM_CALLOC (sizeof (BSS_DATA)));
  A->znorm = 1.0;
  A->dom = dom;
  for (bod = dom->bod; bod; bod = bod->next) A->nprimal += bod->dofs;
  A->ndual = dom->ncon * 3;
  A->b = newvector (A->ndual);
  A->x = newvector (A->ndual);
  A->y = newvector (A->ndual);
  A->z = newvector (A->ndual);
  ERRMEM (A->r = MEM_CALLOC (sizeof (double [A->nprimal])));
  ERRMEM (A->u = MEM_CALLOC (sizeof (double [A->nprimal])));

  A->dat = create_constraints_data (dom, A->x->x); /* A->x initialized with con->R */

  return A;
}

/* update linear system */
static void update_system (BSS_DATA *A)
{
  double *Abx = A->b->x, *Axx = A->x->x, *Ayx = A->y->x, delta = A->delta;
  DOM *dom = A->dom;
  short dynamic = dom->dynamic;
  BSS_CON_DATA *dat, *end;


  /* residual = W R + B - U */
  W_times_vector (dom->step, A, Axx, Ayx);
  for (dat = A->dat, end = dat + dom->ncon; dat != end; dat ++)
  {
    double *Q = &Ayx [dat->n], *B = dat->B, *U = dat->U, *RE = dat->RE;

    ACC (B, Q);
    SUB (Q, U, RE);
  }

  for (dat = A->dat; dat != end; dat ++)
  {
    double *U = dat->U,
	   *R = dat->R,
	   *V = dat->V,
	   *W = dat->W,
	   *T = dat->T,
	   *b = &Abx [dat->n],
	   *RE = &Ayx [dat->n];

    switch (dat->kind)
    {
    case FIXPNT:
    case GLUE:
    {
      if (dynamic)
      {
	b [0] = -V[0]-U[0]-RE[0];
	b [1] = -V[1]-U[1]-RE[1];
	b [2] = -V[2]-U[2]-RE[2];
      }
      else
      {
	b [0] = -U[0]-RE[0];
	b [1] = -U[1]-RE[1];
	b [2] = -U[2]-RE[2];
      }

      NNCOPY (W, T);
    }
    break;
    case FIXDIR:
    {
      b [0] = -R[0];
      b [1] = -R[1];
      if (dynamic) b [2] = -V[2]-U[2]-RE[2];
      else b [2] = -U[2]-RE[2];

      T [1] = T [3] = T [6] = T [7] = 0.0;
      T [0] = T [4] = 1.0;
      T [2] = W [2];
      T [5] = W [5];
      T [8] = W [8];
    }
    break;
    case VELODIR:
    {
      b [0] = -R[0];
      b [1] = -R[1];
      b [2] = VELODIR(dat->con->Z)-U[2]-RE[2];

      T [1] = T [3] = T [6] = T [7] = 0.0;
      T [0] = T [4] = 1.0;
      T [2] = W [2];
      T [5] = W [5];
      T [8] = W [8];
    }
    break;
    case RIGLNK:
    {
      /* TODO */ ASSERT (0, ERR_NOT_IMPLEMENTED);
    }
    break;
    case CONTACT:
    {
      double *X = dat->X, *Y = dat->Y, G [3];

      VIC_Linearize (dat->con, SMOOTHING_EPSILON, G, X, Y);

      NVADDMUL (G, X, RE, b);
      SCALE (b, -1.0);

      NNMUL (X, W, T);
      NNADD (T, Y, T);
    }
    break;
    }

    T [0] += delta;
    T [4] += delta;
    T [8] += delta;

    MX_DENSE_PTR (S, 3, 3, T);
    MX_Inverse (&S, &S);
  }
}

/* solve linear system */
static void linear_solve (BSS_DATA *A, double resdec, int maxiter)
{
  hypre_FlexGMRESFunctions *gmres_functions;
  void *gmres_vdata;
  double abstol;

  abstol = resdec * A->resnorm;

  if (abstol == 0.0) /* initially */
  {
    abstol = ABSTOL * sqrt (InnerProd (A->b, A->b));
    if (abstol == 0.0) abstol = ABSTOL;
  }

  gmres_functions = hypre_FlexGMRESFunctionsCreate (CAlloc, Free, (int (*) (void*,int*,int*)) CommInfo,
    (void* (*) (void*))CreateVector, (void* (*) (int, void*))CreateVectorArray, (int (*) (void*))DestroyVector,
    MatvecCreate, (int (*) (void*,double,void*,void*,double,void*))Matvec, MatvecDestroy,
    (double (*) (void*,void*))InnerProd, (int (*) (void*,void*))CopyVector, (int (*) (void*))ClearVector,
    (int (*) (double,void*))ScaleVector, (int (*) (double,void*,void*))Axpy,
    PrecondSetup, (int (*) (void*,void*,void*,void*))Precond);
  gmres_vdata = hypre_FlexGMRESCreate (gmres_functions);

  hypre_FlexGMRESSetTol (gmres_vdata, 0.0);
  hypre_FlexGMRESSetMinIter (gmres_vdata, 1);
  hypre_FlexGMRESSetMaxIter (gmres_vdata, maxiter);
  hypre_FlexGMRESSetAbsoluteTol (gmres_vdata, abstol);
  hypre_FlexGMRESSetup (gmres_vdata, A, A->b, A->z);
  hypre_FlexGMRESSolve (gmres_vdata, A, A->b, A->z);
  hypre_FlexGMRESGetNumIterations (gmres_vdata , &A->iters);
  hypre_FlexGMRESDestroy (gmres_vdata);

  A->delta = 0;
  Matvec (NULL, -1.0, A, A->z, 1.0, A->b);
  A->resnorm = sqrt (InnerProd (A->b, A->b)); /* residual without regularisation (delta = 0) */
  A->znorm = sqrt (InnerProd (A->z, A->z));
  A->delta = A->resnorm / A->znorm; /* sqrt (L-curve) */
}

/* update solution */
static void update_solution (BSS_DATA *A)
{
  double S [3], *R, *x, *z, *Azx = A->z->x, *Ayx = A->y->x, *Axx = A->x->x;
  BSS_CON_DATA *dat, *end;
  DOM *dom = A->dom;

  for (dat = A->dat, end = dat + dom->ncon; dat != end; dat ++)
  {
    x = &Axx [dat->n];
    z = &Azx [dat->n];
    R = dat->R;

    if (dat->kind == CONTACT)
    {
      ADD (x, z, S);
      VIC_Project (dat->con->mat.base->friction, S, S); /* project onto the friction cone */
      SUB (S, x, z);
    }

    ACC (z, x); /* R = R + dR */
    COPY (x, R);
  }

  /* dU = W dR, U = U + dU */
  W_times_vector (dom->step, A, Azx, Ayx);
  for (dat = A->dat; dat != end; dat ++)
  {
    double *dU = &Ayx [dat->n], *U = dat->U, *RE = dat->RE;

    ACC (dU, U);
    ACC (RE, U);
  }
}

/* compute merit function value */
static double merit_function (BSS_DATA *A)
{
  double G [3], *U, *V, dot;
  DOM *dom = A->dom;
  short dynamic = dom->dynamic;
  CON *con;

  for (dot = 0, con = dom->con; con; con = con->next)
  {
    V = con->dia->V;
    U = con->U;

    switch (con->kind)
    {
    case FIXPNT:
    case GLUE:
      if (dynamic) { ADD (U, V, G); }
      else { COPY (U, G); }
      dot += DOT (G, G);
      break;
    case FIXDIR:
      if (dynamic) G [0] = U[2] - V[2];
      else G [0] = U[2];
      dot += G[0]*G[0];
      break;
    case VELODIR:
      G [0] = VELODIR(con->Z) - U[2];
      dot += G[0]*G[0];
      break;
    case RIGLNK:
      /* TODO */ ASSERT (0, ERR_NOT_IMPLEMENTED);
      break;
    case CONTACT:
      VIC_Linearize (con, 0, G, NULL, NULL);
      dot += DOT (G, G);
      break;
    }
  }

#if MPI
  /* TODO: sum up dots in parallel */
#endif

  return sqrt (dot);
}

/* destroy BSS data */
static void destroy_data (BSS_DATA *A)
{
  destroy_constraints_data (A->dat, A->dom->ncon);
  DestroyVector (A->b);
  DestroyVector (A->x);
  DestroyVector (A->y);
  DestroyVector (A->z);
  free (A->r);
  free (A->u);
  free (A);
}

/* create solver */
BSS* BSS_Create (double meritval, int maxiter)
{
  BSS *bs;

  ERRMEM (bs = MEM_CALLOC (sizeof (BSS)));
  bs->meritval = meritval;
  bs->maxiter = maxiter;
  bs->linminiter = 5;
  bs->resdec = 0.25;
  bs->verbose = 1;

  return bs;
}

/* run solver */
void BSS_Solve (BSS *bs, LOCDYN *ldy)
{
  char fmt [512];
  double *merit;
  BSS_DATA *A;
  DOM *dom;

  sprintf (fmt, "BODY_SPACE_SOLVER: (LIN its/res: %%%dd/%%.2e) iteration: %%%dd merit: %%.2e\n",
           (int)log10 (bs->linminiter * bs->maxiter) + 1, (int)log10 (bs->maxiter) + 1);

  ERRMEM (bs->merhist = realloc (bs->merhist, bs->maxiter * sizeof (double)));
  dom = ldy->dom;
  A = create_data (dom);
  merit = &dom->merit;
  bs->iters = 0;

  do
  {
    update_system (A);

    linear_solve (A, bs->resdec, bs->linminiter + bs->iters);

    update_solution (A);

    *merit = merit_function (A);

    bs->merhist [bs->iters] = *merit;

#if MPI
    if (dom->rank == 0)
#endif
    if (dom->verbose && bs->verbose) printf (fmt, A->iters, A->resnorm, bs->iters, *merit);

  } while (++ bs->iters < bs->maxiter && *merit > bs->meritval);

  destroy_data (A);
}

/* write labeled state values */
void BSS_Write_State (BSS *bs, PBF *bf)
{
  /* TODO */
}

/* destroy solver */
void BSS_Destroy (BSS *bs)
{
  free (bs->merhist);
  free (bs);
}
