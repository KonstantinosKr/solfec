/*
 * nts.c
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

#include "nts.h"
#include "dom.h"
#include "fem.h"
#include "alg.h"
#include "bla.h"
#include "lap.h"
#include "err.h"
#include "vic.h"
#include "mrf.h"
#include "ext/krylov/krylov.h"

#if MPI
#include "com.h"
#include "pck.h"
#include "put.h"
#endif

typedef struct newton_con_data NEWTON_CON_DATA;
typedef struct newton_data NEWTON_DATA;
typedef struct vector VECTOR;

#if MPI
typedef struct newton_com_pattern NEWTON_COM_PATTERN;
struct newton_com_pattern
{
  COMDATA *send, *recv;
  int nsend, nrecv;
  void *pattern;
};
#endif

struct newton_con_data
{
  MX *mH; /* master H */
  int mi, /* shift to mH in global velocity space */
     *mj; /* index mapping for mH converted by csc_to_dense () */

  MX *sH;
  int si,
     *sj;

  int n; /* constraint shift index */

  double *R, /* con->R */
	 *U, /* con->U */
         *V, /* con->V */
	 *B, /* con->dia->B */
	 *W, /* con->dia->W */
	 *A, /* con->dia->A */
	  X [9], /* U linearization */
	  Y [9], /* R linearization */
	  T [9], /* diagonal preconditioner */
	  UT,    /* fixed point |UT| */
	  fri,   /* friction */
	  coh;   /* cohesion */

  short kind; /* con->kind */

  CON *con; /* constraint */
};

struct newton_data
{
  BODY **bod; /* bodies with constraints */

  int nprimal, /* SUM [bod in bod[]] { bod->dofs } */ 
      ndual, /* 3 * ndat */
      ndat, /* number of active constraints */
      nbod; /* size of bod[] */

  NEWTON_CON_DATA *dat; /* active constraints data */

  VECTOR *C,  /* -C (U,R) (of size ndual) */
	 *dU, /* increments of velocities (-||-) */
	 *dR; /* increments of reactions (-||-) */

  double *u, /* body space velocity (of size nprimal) */
         *r, /* body space reaction (-||-) */
	 *a, /* auxiliary vector (of size MAX [bod in bod->dom] { bod->dofs }) */
	 *b, /* auxiliary vector (of size ndual) */
	  epsilon, /* W regularization */
	  omega,   /* projection smoothing regularization */
	  Cnorm;   /* |C| */

  int iters; /* linear solver iterations */

  DOM *dom; /* domain */

  LOCDYN *ldy; /* local dynamics */

#if MPI
  NEWTON_COM_PATTERN *x_to_ext, *ext_to_y; /* communication patterns */
  NEWTON_CON_DATA *datext; /* external active constraints data */
  int ndatext, next; /* number of them, size of 'ext' (== ndatext * 3) */
  double *ext; /* storage for R_ext and U_ext */
#endif
};

struct vector
{
  double *x;

  int n;
};

/* allocate vector */
static VECTOR* newvector (int n)
{
  VECTOR *v;

  ERRMEM (v = malloc (sizeof (VECTOR)));
  ERRMEM (v->x = MEM_CALLOC (n * sizeof (double)));
  v->n = n;

  return v;
}

/* convert a sparse matrix into a dense one */
static MX* csc_to_dense (MX *a, int **map)
{
  int n, m, *i, *k, *p, *q, *f, *g;
  double *ax, *bx;
  MX *b;

  ERRMEM (f = MEM_CALLOC (sizeof (int [a->n]))); /* nonempty column flags */
  
  for (n = 0, m = a->m, p = a->p, q = p + a->n, ax = a->x, g = f; p < q; p ++, g ++)
  {
    if (*p < *(p+1)) /* if column is nonempty */
    {
      for (i = &a->i [*p], k = &a->i [*(p+1)]; i < k; i ++, ax ++)
      {
	if (*ax != 0.0) (*g) ++; /* and it really is */
      }

      if (*g > 0) n ++; /* account for it */
    }
  }

  b = MX_Create (MXDENSE, m, n, NULL, NULL);
  ERRMEM ((*map) = MEM_CALLOC (sizeof (int [n])));

  for (n = 0, p = a->p, bx = b->x, g = f; p < q; p ++, g ++)
  {
    if (*g > 0)  /* for each nonempty column */
    {
      for (i = &a->i [*p], k = &a->i [*(p+1)], ax = &a->x [*p]; i < k; i ++, ax ++)
      {
	bx [*i] = *ax; /* write it into dense storage */
      }
      
      (*map) [n] = p - a->p; /* map dense blocks to body-dofs indices */
      bx += m, n ++; /* next dense column */
    }
  }

  MX_Destroy (a);
  free (f);

  return b;
}

/* scatter and add to sparse vector */
inline static void scatter (double *a, int *j, int n, double *q)
{
  if (j)
  {
    for (int *k = j + n; j < k; j ++, a ++) q [*j] += *a;
  }
}

/* gather values of sparse vector */
inline static double* gather (double *q, int *j, int n, double *a)
{
  if (j)
  {
    for (int *k = j + n; j < k; j ++, a ++) *a = q [*j];

    return (a-n);
  }

  return q;
}

/* y = H' x */
static void H_trans_vector (double *a, NEWTON_CON_DATA *dat, int n, double *x, double *y)
{
  for (; n > 0; dat ++, n --)
  {
    double *p = &x [dat->n], *q, *r, c;

    if (dat->mH)
    {
      q = &y [dat->mi];

      if (dat->mj) c = 0.0, r = a;
      else c = 1.0, r = q;

      MX_Matvec (1.0, MX_Tran (dat->mH), p, c, r);

      scatter (r, dat->mj, dat->mH->n, q);
    }

    if (dat->sH)
    {
      q = &y [dat->si];

      if (dat->sj) c = 0.0, r = a;
      else c = 1.0, r = q;

      MX_Matvec (1.0, MX_Tran (dat->sH), p, c, r);

      scatter (r, dat->sj, dat->sH->n, q);
    }
  }
}

/* y = H' x */
static void H_times_vector (double *a, NEWTON_CON_DATA *dat, int n, double *x, double *y)
{
  for (; n > 0; dat ++, n --)
  {
    double *p, *q = &y [dat->n], *r;

    if (dat->mH)
    {
      p = &x [dat->mi];

      r = gather (p, dat->mj, dat->mH->n, a);

      MX_Matvec (1.0, dat->mH, r, 1.0, q);
    }

    if (dat->sH)
    {
      p = &x [dat->si];

      r = gather (p, dat->sj, dat->sH->n, a);

      MX_Matvec (1.0, dat->sH, r, 1.0, q);
    }
  }
}

#if MPI
/* create x_to_ext communication pattern */
static NEWTON_COM_PATTERN* x_to_ext_create (NEWTON_DATA *A)
{
  NEWTON_CON_DATA *dat, *end;
  NEWTON_COM_PATTERN *pat;
  COMDATA *ptr;
  SET *item;

  ERRMEM (pat = MEM_CALLOC (sizeof (NEWTON_COM_PATTERN)));

  for (dat = A->dat, end = dat + A->ndat; dat != end; dat ++)
  {
    pat->nsend += SET_Size (dat->con->ext);
  }

  ERRMEM (pat->send = MEM_CALLOC (sizeof (COMDATA [pat->nsend])));

  for (dat = A->dat, ptr = pat->send; dat != end; dat ++)
  {
    for (item = SET_First (dat->con->ext); item; item = SET_Next (item), ptr ++)
    {
      ptr->rank = (int) (long) item->data;
      ptr->i = (int*) &dat->con->id;
      ptr->doubles = 3;
      ptr->d = dat->R;
      ptr->ints = 1;
    }
  }

  pat->pattern = COMALL_Pattern (MPI_COMM_WORLD, pat->send, pat->nsend, &pat->recv, &pat->nrecv);

  return pat;
}

/* create ext_to_y communication pattern */
static NEWTON_COM_PATTERN* ext_to_y_create (NEWTON_DATA *A)
{
  NEWTON_CON_DATA *dat, *end;
  NEWTON_COM_PATTERN *pat;
  COMDATA *ptr;

  ERRMEM (pat = MEM_CALLOC (sizeof (NEWTON_COM_PATTERN)));
  pat->nsend = A->ndatext;
  ERRMEM (pat->send = MEM_CALLOC (sizeof (COMDATA [pat->nsend])));

  for (dat = A->datext, ptr = pat->send, end = dat + A->ndatext; dat != end; dat ++, ptr ++)
  {
    ptr->i = (int*) &dat->con->id;
    ptr->rank = dat->con->rank;
    ptr->doubles = 3;
    ptr->d = dat->U;
    ptr->ints = 1;
  }

  pat->pattern = COMALL_Pattern (MPI_COMM_WORLD, pat->send, pat->nsend, &pat->recv, &pat->nrecv);

  return pat;
}

/* destroy communication pattern */
static void comm_pattern_destroy (NEWTON_COM_PATTERN *pat)
{
  COMALL_Free (pat->pattern);
  free (pat->send);
  free (pat->recv);
  free (pat);
}

/* send x local reactions to xext external reaction */
static void x_to_ext (NEWTON_DATA *A, double *x, double *ext)
{
  NEWTON_COM_PATTERN *pat = A->x_to_ext;
  MAP *conext = A->dom->conext;
  NEWTON_CON_DATA *dat, *end;
  COMDATA *ptr, *qtr;
  double *a, *b, *c;
  SET *item;
  CON *con;
  int *i;

  for (dat = A->dat, ptr = pat->send, end = dat + A->ndat; dat != end; dat ++)
  {
    for (item = SET_First (dat->con->ext); item; item = SET_Next (item), ptr ++)
    {
      ptr->d = &x [dat->n];
    }
  }

  COMALL_Repeat (pat->pattern);

  for (ptr = pat->recv, qtr = ptr + pat->nrecv; ptr != qtr; ptr ++)
  {
    for (a = ptr->d, i = ptr->i, b = a + ptr->doubles; a < b; a += 3, i ++)
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (conext, (void*) (long) (*i), NULL), "Invalid ext id: %d", *i);
      c = &ext [con->num];
      ACC (a, c);
    }
  }
}

/* send yext external velocities to y local velocities */
static void ext_to_y (NEWTON_DATA *A, double *ext, double *y)
{
  NEWTON_COM_PATTERN *pat = A->ext_to_y;
  MAP *idc = A->dom->idc;
  NEWTON_CON_DATA *dat, *end;
  COMDATA *ptr, *qtr;
  double *a, *b, *c;
  CON *con;
  int *i;

  for (dat = A->datext, ptr = pat->send, end = dat + A->ndatext; dat != end; dat ++, ptr ++)
  {
    ptr->d = &ext [dat->n];
  }

  COMALL_Repeat (pat->pattern);

  for (ptr = pat->recv, qtr = ptr + pat->nrecv; ptr != qtr; ptr ++)
  {
    for (a = ptr->d, i = ptr->i, b = a + ptr->doubles; a < b; a += 3, i ++)
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (idc, (void*) (long) (*i), NULL), "Invalid con id: %d", *i);
      c = &y [con->num];
      ACC (a, c);
    }
  }
}
#endif

/* y = alpha W x */
static void W_times_vector (NEWTON_DATA *A, double *x, double *y)
{
  double *r = A->r, *u = A->u, *a = A->a, *b = A->b, step;
  int n = A->ndat, m = A->nbod, i;
  NEWTON_CON_DATA *dat, *end;
  BODY **bod = A->bod;
  DOM *dom = A->dom;

  for (dat = A->dat, end = dat + n; dat != end; dat ++)
  {
    double *px = &x [dat->n],
	   *pb = &b [dat->n];

    COPY (px, pb);

    if (dat->kind == CONTACT && dat->fri != 0.0)
    {
      pb [2] /= dat->fri; /* friction scaling */
    }
  }

  step = dom->step;
  SETN (r, A->nprimal, 0.0);
  H_trans_vector (a, A->dat, n, b, r);

#if MPI
   SETN (A->ext, A->next, 0.0);
   x_to_ext (A, b, A->ext); /* R_ext = x */
   H_trans_vector (a, A->datext, A->ndatext, A->ext, r); /* r += H' R_ext */
#endif

  for (i = 0; i < m; r += bod [i]->dofs, u += bod [i]->dofs, i++)
  {
    BODY_Invvec (step, bod [i], r, 0.0, u);
  }

  SETN (y, A->ndual, 0.0);
  H_times_vector (a, A->dat, n, A->u, y);

#if MPI
  SETN (A->ext, A->next, 0.0);
  H_times_vector (a, A->datext, A->ndatext, A->u, A->ext); /* U_ext = H_ext u */
  ext_to_y (A, A->ext, y); /* y += U_ext */
#endif

  for (dat = A->dat; dat != end; dat ++)
  {
    double *py = &y [dat->n];

    if (dat->kind == CONTACT && dat->fri != 0.0)
    {
      py [2] /= dat->fri; /* friction scaling */
    }
  }

  blas_daxpy (A->epsilon, A->ndual, x, 1, y, 1); /* W + epsilon I */
}

/* GMSS interface start */
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

static int CommInfo (NEWTON_DATA *A, int *my_id, int *num_procs)
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

static int Matvec (void *matvec_data, double alpha, NEWTON_DATA *A, VECTOR *x, double beta, VECTOR *y)
{
  double Z [3], *Y, *X, *R, *U, *Q, *u, *q, *r;
  NEWTON_CON_DATA *dat = A->dat;
  int n = A->ndat;

  W_times_vector (A, x->x, A->dU->x); /* dU = W dR */

  for (u = A->dU->x, r = x->x, q = y->x; n > 0; dat ++, n --)
  {
    U = &u [dat->n];
    R = &r [dat->n];
    Q = &q [dat->n];

    switch (dat->kind)
    {
    case VELODIR:
    case FIXDIR:
    case RIGLNK:
    {
      Z [0] = R [0];
      Z [1] = R [1];
      Z [2] = U [2];

      U = Z;
    }
    break;
    case CONTACT:
    {
      X = dat->X;
      Y = dat->Y;

      NVMUL (X, U, Z);
      NVADDMUL (Z, Y, R, Z);  /* Z = X dU + Y dR */

      U = Z;
    }
    break;
    }

    SCALE (Q, beta);
    ADDMUL (Q, alpha, U, Q)
  }

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

static int Precond (void *vdata, NEWTON_DATA *A, VECTOR *b, VECTOR *x)
{
  NEWTON_CON_DATA *dat, *end;
  double *T, *p, *q;

  for (dat = A->dat, end = dat + A->ndat; dat != end; dat ++)
  {
    p = &b->x[dat->n];
    q = &x->x[dat->n];
    T = dat->T;
    NVMUL (T, p, q);
  }

  return 0;
}
/* GMSS interface end */

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

  COMALL (MPI_COMM_WORLD, send, nsend, &recv, &nrecv); /* send V, B */

  for (i = 0; i < nrecv; i ++)
  {
    ptr = &recv [i];
    for (j = ptr->i, k = j + ptr->ints, D = ptr->d; j < k; j ++, D += 6)
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (dom->idc, (void*) (long) ABS(*j), NULL), "Invalid con id: %d", ABS(*j));
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
static NEWTON_CON_DATA *create_constraints_data (DOM *dom, NEWTON_DATA *A, BODY **bod, int nbod, int *ndat, double *free_energy)
{
  NEWTON_CON_DATA *out, *dat, *end;
  MAP *map, *fem;
  short dynamic;
  double step;
  CON *con;
  int i, n;

  dynamic = dom->dynamic;
  (*free_energy) = 0.0;

  ERRMEM (out = MEM_CALLOC (dom->ncon * sizeof (NEWTON_CON_DATA)));
  step = dom->step;

  for (i = 0, map = fem = NULL, n = 0; i < nbod; n += bod [i]->dofs, i ++)
  {
    MAP_Insert (NULL, &map, bod [i], (void*) (long) n, NULL); /* map bodies to DOF shifts */

    if (bod [i]->con && bod [i]->kind == FEM)
    {
      MAP_Insert (NULL, &fem, bod [i], FEM_Approx_Inverse (bod [i]), NULL); /* map approximate inverses */
    }
  }

  /* create internal constraints data */
  for (con = dom->con, dat = out, n = 0; con; con = con->next)
  {
    if (dynamic && con->kind == CONTACT && con->gap > 0.0) continue; /* skip open dynamic contacts */

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
    dia->rowupdate = dia->W[8] == 0.0; /* needed only as a preconditioner => update once */
    MX *prod, *inv;

    if (s)
    {
      sgobj = sgobj(con);
      sshp = sshp(con);
    }

#if MPI
    if (m->flags & BODY_PARENT) /* master is a child => dat->mH == NULL */
#endif
    if (m->kind != OBS)
    {
      dat->mH = BODY_Gen_To_Loc_Operator (m, mshp, mgobj, mpnt, base);
      dat->mi = (int) (long) MAP_Find (map, m, NULL);

      if (dia->rowupdate)
      {
	if (m->kind == FEM) inv = MAP_Find (fem, m, NULL); else inv = m->inverse;
	prod = MX_Matmat (1.0, inv, MX_Tran (dat->mH), 0.0, NULL);
	MX_Matmat (step, dat->mH, prod, 0.0, &W); /* H * inv (M) * H^T */
	MX_Destroy (prod);
      }

      if (m->kind == FEM)
      {
	dat->mH = csc_to_dense (dat->mH, &dat->mj);
      }
    }

#if MPI
    if (s && s->flags & BODY_PARENT) /* slave is a child => dat->sH == NULL */
#endif
    if (s && s->kind != OBS)
    {
      dat->sH = BODY_Gen_To_Loc_Operator (s, sshp, sgobj, spnt, base);
      dat->si = (int) (long) MAP_Find (map, s, NULL);
      MX_Scale (dat->sH, -1.0);

      if (dia->rowupdate)
      {
	if (s->kind == FEM) inv = MAP_Find (fem, s, NULL); else inv = s->inverse;
	prod = MX_Matmat (1.0, inv, MX_Tran (dat->sH), 0.0, NULL);
	MX_Matmat (step, dat->sH, prod, 1.0, &W); /* H * inv (M) * H^T */
	MX_Destroy (prod);
      }

      if (s->kind == FEM)
      {
	dat->sH = csc_to_dense (dat->sH, &dat->sj);
      }
    }

    dat->n = n;
    dat->R = con->R;
    dat->U = con->U;
    dat->V = con->V;
    dat->B = dia->B;
    dat->W = dia->W;
    dat->A = dia->A;
    dat->kind = con->kind;
    dat->con = con;
    if (dat->kind == CONTACT)
    {
      dat->fri = con->mat.base->friction;
      dat->coh = SURFACE_MATERIAL_Cohesion_Get (&con->mat) * con->area;
      if (dat->fri != 0.0)
      {
	con->R[2] *= dat->fri;
	con->U[2] /= dat->fri; /* friction scaling */ 
      }
    }
#if MPI
    con->num = n; /* ext_to_y */
#endif

    dat ++;
    n += 3;
  }
  (*ndat) = n / 3;

#if MPI
  COMDATA *send, *recv, *ptr, *qtr;
  int nsend, nrecv, *j, *k;
  double *W1, *W2;
  MAP *item;
  SET *jtem;

  /* mark external constraints that would require Wii updates */
  for (item = MAP_First (dom->conext); item; item = MAP_Next (item))
  {
    con = item->data;
    con->paircode = 0; /* zero before using (will store dia->rowupdate) */
  }
  for (dat = out, nsend = 0, end = out + (*ndat); dat != end; dat ++)
  {
    for (jtem = SET_First (dat->con->ext); jtem; jtem = SET_Next (jtem)) nsend ++;
  }
  ERRMEM (send = MEM_CALLOC (sizeof (COMDATA [nsend]) + 2 * sizeof (int [nsend])));
  for (dat = out, ptr = send, j = (int*) (send + nsend); dat != end; dat ++)
  {
    con = dat->con;
    for (jtem = SET_First (con->ext); jtem; jtem = SET_Next (jtem), ptr ++, j += 2)
    {
      ptr->rank = (int) (long) jtem->data;
      j [1] = con->dia->rowupdate;
      j [0] = con->id;
      ptr->ints = 2;
      ptr->i = j;
    }
  }

  COMALL (MPI_COMM_WORLD, send, nsend, &recv, &nrecv); /* send rowupdate flags */

  for (ptr = recv, qtr = ptr + nrecv; ptr != qtr; ptr ++)
  {
    for (j = ptr->i, k = j + ptr->ints; j < k; j += 2)
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (dom->conext, (void*) (long) j[0], NULL), "Invalid con id: %d", j[0]);
      con->paircode = j[1]; /* otherwise unused for external constraints */
    }
  }
  free (send);
  free (recv);

  /* create external constraints data */
  A->ndatext = MAP_Size (dom->conext);
  ERRMEM (A->datext = MEM_CALLOC (sizeof (NEWTON_CON_DATA [A->ndatext])));
  for (item = MAP_First (dom->conext), dat = A->datext, n = 0; item; item = MAP_Next (item), dat ++)
  {
    con = item->data;
    if (dynamic && con->kind == CONTACT && con->gap > 0.0) continue; /* skip open dynamic contacts */

    BODY *m = con->master,
	 *s = con->slave;
    void *mgobj = mgobj(con),
	 *sgobj;
    SHAPE *mshp = mshp(con),
	  *sshp;
    double *mpnt = con->mpnt,
	   *spnt = con->spnt,
	   *base = con->base;
    MX_DENSE_PTR (W, 3, 3, dat->T);
    MX *prod, *inv;

    if (s)
    {
      sgobj = sgobj(con);
      sshp = sshp(con);
    }

    if (m->flags & BODY_PARENT && m->kind != OBS)
    {
      dat->mH = BODY_Gen_To_Loc_Operator (m, mshp, mgobj, mpnt, base);
      dat->mi = (int) (long) MAP_Find (map, m, NULL);

      if (con->paircode)
      {
	if (m->kind == FEM) inv = MAP_Find (fem, m, NULL); else inv = m->inverse;
	prod = MX_Matmat (1.0, inv, MX_Tran (dat->mH), 0.0, NULL);
	MX_Matmat (step, dat->mH, prod, 0.0, &W); /* H * inv (M) * H^T */
	MX_Destroy (prod);
      }

      if (m->kind == FEM)
      {
	dat->mH = csc_to_dense (dat->mH, &dat->mj);
      }
    }

    if (s && s->flags & BODY_PARENT && s->kind != OBS)
    {
      dat->sH = BODY_Gen_To_Loc_Operator (s, sshp, sgobj, spnt, base);
      dat->si = (int) (long) MAP_Find (map, s, NULL);
      MX_Scale (dat->sH, -1.0);

      if (con->paircode)
      {
	if (s->kind == FEM) inv = MAP_Find (fem, s, NULL); else inv = s->inverse;
	prod = MX_Matmat (1.0, inv, MX_Tran (dat->sH), 0.0, NULL);
	MX_Matmat (step, dat->sH, prod, 1.0, &W); /* H * inv (M) * H^T */
	MX_Destroy (prod);
      }

      if (s->kind == FEM)
      {
	dat->sH = csc_to_dense (dat->sH, &dat->sj);
      }
    }

    dat->n = n;
    dat->R = con->R;
    dat->U = con->U;
    dat->V = con->V;
    dat->kind = con->kind;
    dat->con = con;
    con->num = n; /* x_to_ext */
    n += 3;
  }
  A->next = n;
  A->ndatext = n / 3;
  ERRMEM (A->ext = MEM_CALLOC (sizeof (double [n])));

  /* send remote Wii contributions to parent constraints */
  ERRMEM (send = MEM_CALLOC (sizeof (COMDATA [A->ndatext])));
  for (dat = A->datext, ptr = send, end = dat + A->ndatext; dat != end; dat ++)
  {
    con = dat->con;
    if (con->paircode)
    {
      ptr->i = (int*) &con->id;
      ptr->rank = con->rank;
      ptr->doubles = 9;
      ptr->d = dat->T;
      ptr->ints = 1;
      ptr ++;
    }
  }

  COMALL (MPI_COMM_WORLD, send, ptr - send, &recv, &nrecv); /* send Wii */

  for (ptr = recv, qtr = ptr + nrecv; ptr != qtr; ptr ++)
  {
    for (j = ptr->i, k = j + ptr->ints, W1 = ptr->d; j < k; j ++, W1 += 9)
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (dom->idc, (void*) (long) (*j), NULL), "Invalid con id: %d", *j);
      ASSERT_DEBUG (con->dia->rowupdate, "Inconsistent row update flag");
      dat = &out [con->num / 3];
      W2 = dat->W;
      NNADD (W2, W1, W2);
    }
  }
  free (send);
  free (recv);
#endif

  /* process Wii */
  for (dat = out, end = dat + (*ndat); dat != end; dat ++)
  {
    con = dat->con;
    DIAB *dia = con->dia;
    MX_DENSE_PTR (W, 3, 3, dia->W);
    MX_DENSE_PTR (A, 3, 3, dia->A);
    double *B = dia->B, X [3];

    if (con->dia->rowupdate)
    {
      MX_Copy (&W, &A);
      MX_Inverse (&A, &A);
    }

    NVMUL (A.x, B, X);
    (*free_energy) += DOT (X, B); /* sum up free energy */

    /* add up prescribed velocity contribution */
    if (con->kind == VELODIR)
    {
      (*free_energy) += A.x[8] * VELODIR(con->Z) * VELODIR(con->Z);
    }
  }
  (*free_energy) *= 0.5;

  MAP_Free (NULL, &map);
  for (map = MAP_First (fem); map;
       map = MAP_Next (map)) MX_Destroy (map->data);
  MAP_Free (NULL, &fem);

  return out;
}

static void destroy_constraints_data (NEWTON_CON_DATA *dat, int n)
{
  NEWTON_CON_DATA *ptr = dat;

  for (; n > 0; dat ++, n --)
  {
    if (dat->mH) MX_Destroy (dat->mH);
    if (dat->sH) MX_Destroy (dat->sH);
    free (dat->mj);
    free (dat->sj);
  }

  free (ptr);
}

/* decide whether to use local dynamics; if so assemble it */
static LOCDYN* local_dynamics (LOCDYN *ldy)
{

  /* TODO: make sure that FEM_Gen_To_Loc_Operator knows how to act
   *       in case corotational FEM and local dynamics */

  return NULL; /* TODO */
}

/* create NEWTON data */
static NEWTON_DATA *create_data (LOCDYN *ldy, NEWTON *ns)
{
  DOM *dom = ldy->dom;
  NEWTON_DATA *A;
  BODY *bod;
  int m, n;

  update_V_and_B (dom);

  ERRMEM (A = MEM_CALLOC (sizeof (NEWTON_DATA)));
  A->dom = ldy->dom;
  A->ldy = local_dynamics (ldy);
  for (bod = dom->bod, m = 0; bod; bod = bod->next)
  {
    if (bod->con) /* body with constraints */
    {
      n = bod->dofs;
      if (n > m) m = n;
      A->nprimal += n;
      A->nbod ++;
    }
  }
  ERRMEM (A->bod = malloc (A->nbod * sizeof (BODY*)));
  for (bod = dom->bod, n = 0; bod; bod = bod->next)
  {
    if (bod->con) A->bod [n ++] = bod;
  }
  A->dat = create_constraints_data (dom, A, A->bod, A->nbod, &A->ndat, &dom->ldy->free_energy);
  A->ndual = A->ndat * 3;
  A->C = newvector (A->ndual);
  A->dU = newvector (A->ndual);
  A->dR = newvector (A->ndual);
  ERRMEM (A->u = MEM_CALLOC (sizeof (double [A->nprimal])));
  ERRMEM (A->r = MEM_CALLOC (sizeof (double [A->nprimal])));
  ERRMEM (A->a = MEM_CALLOC (sizeof (double [m])));
  ERRMEM (A->b = MEM_CALLOC (sizeof (double [A->ndual])));
#if MPI
  A->x_to_ext = x_to_ext_create (A);
  A->ext_to_y = ext_to_y_create (A);
#endif

  return A;
}

/* projectio onto second order cone */
static void projection (double omega, double *Z, double *Q, double *l1, double *l2)
{
  double len = LEN2 (Z), j1, j2;

  (*l1) = Z[2] - len;
  (*l2) = Z[2] + len;

  j1 = MAX (0.0, (*l1));
  j2 = MAX (0.0, (*l2));

  if (len == 0.0)
  {
    Q [0] = 0.0;
    Q [1] = 0.0;
    Q [2] = 0.5 * (j1 + j2);
  }
  else
  {
    Q [0] = 0.5 * (-j1 * Z[0] + j2 * Z[0]) / len;
    Q [1] = 0.5 * (-j1 * Z[1] + j2 * Z[1]) / len;
    Q [2] = 0.5 * (j1 + j2);
  }
}

/* smoothing function */
inline static double g (double alpha)
{
  return 0.5 * (sqrt (alpha*alpha + 4.0) + alpha);
}

/* smoothing function derivative */
inline static double dgdt (double alpha)
{
  return 0.5 * (alpha / sqrt(alpha*alpha + 4.0) + 1.0);
}

/* update linear system */
static double update_system (NEWTON_DATA *A)
{
  double *Cx = A->C->x, epsilon = A->epsilon, omega = A->omega;
  DOM *dom = A->dom;
  short dynamic = dom->dynamic;
  NEWTON_CON_DATA *dat, *end;

  for (dat = A->dat, end = dat + A->ndat; dat != end; dat ++)
  {
    double *U = dat->U,
	   *R = dat->R,
	   *V = dat->V,
	   *W = dat->W,
	   *T = dat->T,
	   *C = &Cx [dat->n];

    switch (dat->kind)
    {
    case FIXPNT:
    case GLUE:
    {
      if (dynamic)
      {
	C [0] = -V[0]-U[0];
	C [1] = -V[1]-U[1];
	C [2] = -V[2]-U[2];
      }
      else
      {
	C [0] = -U[0];
	C [1] = -U[1];
	C [2] = -U[2];
      }

      NNCOPY (W, T);
      T [0] += epsilon;
      T [4] += epsilon;
      T [8] += epsilon;
    }
    break;
    case FIXDIR:
    {
      C [0] = -R[0];
      C [1] = -R[1];
      if (dynamic) C [2] = -V[2]-U[2];
      else C [2] = -U[2];

      T [1] = T [3] = T [6] = T [7] = 0.0;
      T [0] = T [4] = 1.0;
      T [2] = W [2];
      T [5] = W [5];
      T [8] = W [8];
      T [8] += epsilon;
    }
    break;
    case VELODIR:
    {
      C [0] = -R[0];
      C [1] = -R[1];
      C [2] = VELODIR(dat->con->Z)-U[2];

      T [1] = T [3] = T [6] = T [7] = 0.0;
      T [0] = T [4] = 1.0;
      T [2] = W [2];
      T [5] = W [5];
      T [8] = W [8];
      T [8] += epsilon;
    }
    break;
    case RIGLNK:
    {
      double h = dom->step * (dynamic ? 0.5 : 1.0),
             d = RIGLNK_LEN (dat->con->Z),
	     delta;

      C [0] = -R[0];
      C [1] = -R[1];
      delta = d*d - h*h*DOT2(U,U);
      if (delta >= 0.0) C [2] = (sqrt (delta) - d)/h - U[2];
      else C[2] = -U[2];

      T [1] = T [3] = T [6] = T [7] = 0.0;
      T [0] = T [4] = 1.0;
      T [2] = W [2];
      T [5] = W [5];
      T [8] = W [8];
      T [8] += epsilon;
    }
    break;
    case CONTACT:
    {
      CON *con = dat->con;
      double *X = dat->X, *Y = dat->Y,
	     res = con->mat.base->restitution,
	     step = dom->step,
	     gap = con->gap,
	     udash;

      if (dynamic) udash = res * MIN (V[2], 0);
      else udash = (MAX(gap, 0)/step);

      if (dat->fri == 0.0)
      {
	double Z = (R[2]+dat->coh) - (U[2]+udash);

	C [0] = -R[0];
	C [1] = -R[1];
	C [2] = g(Z) - (R[2]+dat->coh);
	IDENTITY (Y);
	Y [8] = 1.0 - dgdt (Z);
	X [8] = dgdt (Z);
      }
      else
      {
	double Z [3], J [9], dot, l1, l2, a, b, c;

	SUB (R, U, Z);
	udash += dat->UT;
	Z [2] += (dat->coh - udash);
	projection (omega, Z, C, &l1, &l2);
	SUB (C, R, C);
	C [2] -= dat->coh; /* -C = proj [R_coh - F_S(U)] - R_coh */
	dot = DOT2 (Z, Z);

	if (dot == 0.0)
	{
	  IDENTITY (X);
	  X[0] = X[4] = X[8] = dgdt (Z [2] / omega);
	}
	else
	{
	  l1 /= omega;
	  l2 /= omega;
	  a = (g (l2) - g(l1)) / (l2 - l1);
	  b = 0.5 * (dgdt (l2) + dgdt (l1));
	  c = 0.5 * (dgdt (l2) - dgdt (l1));

	  X [0] = a + (b - a) * Z[0]*Z[0] / dot;
	  X [1] = (b - a) * Z[1]*Z[0] / dot;
	  X [2] = c * Z[0] / sqrt (dot);
	  X [3] = X[1];
	  X [4] = a + (b - a) * Z[1]*Z[1] / dot;
	  X [5] = c * Z[1] / sqrt (dot);;
	  X [6] = X[2];
	  X [7] = X[5];
	  X [8] = b;
	}

	IDENTITY (J);
	SUB (J, X, Y);
      }

      NNMUL (X, W, T);
      NNADDMUL (T, epsilon, X, T);
      NNADD (T, Y, T);
    }
    break;
    }

    MX_DENSE_PTR (P, 3, 3, T);
    MX_Inverse (&P, &P);
  }

  return sqrt (InnerProd (A->C, A->C));
}

/* regularized merit function */
static double merit_function (NEWTON_DATA *A, double epsilon, double omega, double theta)
{
  double merit = 0.0, *dr = A->dR->x, *du = A->dU->x;
  DOM *dom = A->dom;
  short dynamic = dom->dynamic;
  NEWTON_CON_DATA *dat, *end;

  for (dat = A->dat, end = dat + A->ndat; dat != end; dat ++)
  {
    double *U0 = dat->U,
	   *R0 = dat->R,
	   *dR = &dr [dat->n],
	   *dU = &du [dat->n],
	   *V = dat->V,
	   U [3], R [3], C [3];

    ADDMUL (R0, theta, dR, R);
    ADDMUL (U0, theta, dU, U);

    switch (dat->kind)
    {
    case FIXPNT:
    case GLUE:
    {
      if (dynamic)
      {
	C [0] = -V[0]-U[0];
	C [1] = -V[1]-U[1];
	C [2] = -V[2]-U[2];
      }
      else
      {
	C [0] = -U[0];
	C [1] = -U[1];
	C [2] = -U[2];
      }
    }
    break;
    case FIXDIR:
    {
      C [0] = -R[0];
      C [1] = -R[1];
      if (dynamic) C [2] = -V[2]-U[2];
      else C [2] = -U[2];
    }
    break;
    case VELODIR:
    {
      C [0] = -R[0];
      C [1] = -R[1];
      C [2] = VELODIR(dat->con->Z)-U[2];
    }
    break;
    case RIGLNK:
    {
      double h = dom->step * (dynamic ? 0.5 : 1.0),
             d = RIGLNK_LEN (dat->con->Z),
	     delta;

      C [0] = -R[0];
      C [1] = -R[1];
      delta = d*d - h*h*DOT2(U,U);
      if (delta >= 0.0) C [2] = (sqrt (delta) - d)/h - U[2];
      else C[2] = -U[2];
    }
    break;
    case CONTACT:
    {
      CON *con = dat->con;
      double res = con->mat.base->restitution,
	     step = dom->step,
	     gap = con->gap,
	     udash;

      if (dynamic) udash = res * MIN (V[2], 0);
      else udash = (MAX(gap, 0)/step);

      if (dat->fri == 0.0)
      {
	double Z = (R[2]+dat->coh) - (U[2]+udash);

	C [0] = -R[0];
	C [1] = -R[1];
	C [2] = g(Z) - (R[2]+dat->coh);
      }
      else
      {
	double Z [3], l1, l2;

	SUB (R, U, Z);
	udash += dat->UT;
	Z [2] += (dat->coh - udash);
	projection (omega, Z, C, &l1, &l2);
	SUB (C, R, C);
	C [2] -= dat->coh; /* -C = proj [R_coh - F_S(U)] - R_coh */
      }
    }
    break;
    }

    merit += DOT (C, C);
  }

#if MPI
  double inp = merit;
  MPI_Allreduce (&inp, &merit, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
#endif

  return sqrt (merit);
}

/* update solution */
static void update_solution (NEWTON_DATA *A, double theta)
{
  NEWTON_CON_DATA *dat, *end;
  double *dr = A->dR->x,
	 *du = A->dU->x;

  for (dat = A->dat, end = dat + A->ndat; dat != end; dat ++)
  {
    double *R = dat->R,
	   *U = dat->U,
	   *dR = &dr [dat->n],
	   *dU = &du [dat->n];

    ADDMUL (R, theta, dR, R);
    ADDMUL (U, theta, dU, U);
  }
}

/* omega function */
static double omega_func (double alpha, double delta)
{
  if (delta >= 0.5 || alpha == 0.0) return DBL_MAX;
  else return 0.5 * ABS (alpha) * sqrt (delta);
}

/* lambda function */
static double lambda_func (NEWTON_DATA *A, double threshold)
{
  NEWTON_CON_DATA *dat, *end;
  double lambda = DBL_MAX;
  DOM *dom = A->dom;
  short dynamic = dom->dynamic;

  for (dat = A->dat, end = dat + A->ndat; dat != end; dat ++)
  {
    if (dat->kind == CONTACT)
    {
      CON *con = dat->con;
      double res = con->mat.base->restitution,
	     step = dom->step,
	     gap = con->gap,
	    *R = con->R,
            *V = dat->V,
            *U = con->U,
	     udash;

      if (dynamic) udash = res * MIN (V[2], 0);
      else udash = (MAX(gap, 0)/step);

      if (dat->fri == 0.0)
      {
	double Z, l1;
       
        Z = (R[2]+dat->coh) - (U[2]+udash);
        l1 = fabs (Z);

	if (l1 > threshold && l1 < lambda) lambda = l1;
      }
      else
      {
	double Z [3], len, l1, l2;

	SUB (R, U, Z);
	udash += dat->UT;
	Z [2] += (dat->coh - udash);
	len = LEN2 (Z);
	l1 = fabs (Z [2] - len);
	l2 = fabs (Z [2] + len);

	if (l1 > threshold && l1 < lambda) lambda = l1;
	if (l2 > threshold && l2 < lambda) lambda = l2;
      }
    }
  }

  return lambda == DBL_MAX ? 0.0 : lambda;
}

/* array minimum */
static double min_func (double *a, int n)
{
  double b = a [--n];
  for (; n >= 0; n --)
    if (a [n] < b) b = a[n];
  return b;
}

/* solve linear system */
static void linear_solve (NEWTON_DATA *A, double abstol, int maxiter)
{
  hypre_FlexGMRESFunctions *gmres_functions;
  void *gmres_vdata;
  VECTOR *r;
  int ret;

  gmres_functions = hypre_FlexGMRESFunctionsCreate (CAlloc, Free, (int (*) (void*,int*,int*)) CommInfo,
    (void* (*) (void*))CreateVector, (void* (*) (int, void*))CreateVectorArray, (int (*) (void*))DestroyVector,
    MatvecCreate, (int (*) (void*,double,void*,void*,double,void*))Matvec, MatvecDestroy,
    (double (*) (void*,void*))InnerProd, (int (*) (void*,void*))CopyVector, (int (*) (void*))ClearVector,
    (int (*) (double,void*))ScaleVector, (int (*) (double,void*,void*))Axpy,
    PrecondSetup, (int (*) (void*,void*,void*,void*))Precond);
  gmres_vdata = hypre_FlexGMRESCreate (gmres_functions);

  hypre_error_flag = 0;
  hypre_FlexGMRESSetTol (gmres_vdata, 0.0);
  hypre_FlexGMRESSetMinIter (gmres_vdata, 1);
  hypre_FlexGMRESSetMaxIter (gmres_vdata, maxiter);
  hypre_FlexGMRESSetAbsoluteTol (gmres_vdata, abstol);
  hypre_FlexGMRESSetup (gmres_vdata, A, A->C, A->dR);
  ret = hypre_FlexGMRESSolve (gmres_vdata, A, A->C, A->dR);
  hypre_FlexGMRESGetNumIterations (gmres_vdata , &A->iters);
  hypre_FlexGMRESGetResidual (gmres_vdata, (void**) &r);
  hypre_FlexGMRESDestroy (gmres_vdata);
  W_times_vector (A, A->dR->x, A->dU->x);
}

/* destroy NEWTON data */
static void destroy_data (NEWTON_DATA *A)
{
  /* account for cohesion */
  for (NEWTON_CON_DATA *dat = A->dat, *end = dat + A->ndat; dat != end; dat ++)
  {
    CON *con = dat->con;

    if (con->kind == CONTACT)
    {
      if (dat->fri != 0.0)
      {
	con->R [2] /= dat->fri;
	con->U [2] *= dat->fri; /* friction scaling */
      }

      if (con->state & CON_COHESIVE)
      {
	double c = dat->coh,
	       f = dat->fri,
	       e = COHESION_EPSILON * c,
	      *R = dat->R;

	if ((R [2] + c) < e || /* mode-I decohesion */
	    LEN2 (R) + e >= f * (R [2] + c)) /* mode-II decohesion */
	{
	  con->state &= ~CON_COHESIVE;
	  SURFACE_MATERIAL_Cohesion_Set (&con->mat, 0.0);
	}
      }
    }
  }

  destroy_constraints_data (A->dat, A->ndat);
#if MPI
  destroy_constraints_data (A->datext, A->ndatext);
  comm_pattern_destroy (A->x_to_ext);
  comm_pattern_destroy (A->ext_to_y);
  free (A->ext);
#endif

  DestroyVector (A->C);
  DestroyVector (A->dU);
  DestroyVector (A->dR);

  free (A->bod);
  free (A->u);
  free (A->r);
  free (A->a);
  free (A->b);
  free (A);
}

/* create solver */
NEWTON* NEWTON_Create (double meritval, int maxiter)
{
  NEWTON *ns;

  ERRMEM (ns = MEM_CALLOC (sizeof (NEWTON)));
  ns->meritval = meritval;
  ns->maxiter = maxiter;
  ns->linmaxiter = maxiter * 10;

  return ns;
}

/* run solver */
void NEWTON_Solve (NEWTON *ns, LOCDYN *ldy)
{
  double eta, rho, sigma, ksi, eta1, kappa, kappa1, kappa2, theta, beta, tau, *merit, innmer, a [3];
  char fmt_out [512], fmt_inn [512];
  NEWTON_DATA *A;
  DOM *dom;

  sprintf (fmt_out, "NEWTON_SOLVER: OUTER iteration: %%%dd, OUTER merit: %%.2e\n",
           (int)log10 (ns->maxiter) + 1);

  sprintf (fmt_inn, "NEWTON_SOLVER: INNER iteration: %%%dd, INNER merit: %%.2e, linear iterations: %%%dd) \n",
           (int)log10 (ns->maxiter) + 1, (int)log10 (ns->linmaxiter) + 1);

  ERRMEM (ns->merhist = realloc (ns->merhist, ns->maxiter * sizeof (double)));
  A = create_data (ldy, ns);
  dom = ldy->dom;
  merit = &dom->merit;
  ns->iters = 0;
  A->epsilon = A->omega = merit_function (A, 0.0, 0.0, 0.0);
  beta = merit_function (A, A->epsilon, A->omega, 0.0);
  eta = 0.01;
  eta1 = 0.001;
  rho = 0.5;
  sigma = 0.1;
  kappa = kappa1 = 0.01;
  kappa2 = 1.0;
  ksi = 0.9;
  tau = 1E-4;

  while ((*merit = MERIT_Function (ldy, 0, 0)) > ns->meritval && ns->iters < ns->maxiter)
  {
    ns->merhist [ns->iters] = *merit;
#if MPI
    if (dom->rank == 0)
#endif
    if (dom->verbose) printf (fmt_out, ns->iters, *merit);

    ns->iters ++;

    do
    {
      theta = 1.0;

      A->Cnorm = update_system (A);

      linear_solve (A, sigma * A->Cnorm, ns->linmaxiter);

      innmer = merit_function (A, A->epsilon, A->omega, theta);

      if (innmer >= beta)
      {
	while (innmer > (1.0 - theta * rho * (1.0 - sigma)) * A->Cnorm && theta >= 1E-6)
	{
	  theta *= ksi;
          innmer = merit_function (A, A->epsilon, A->omega, theta);
	}
      }
#if MPI
      if (dom->rank == 0)
#endif
      if (theta < 1E-6) fprintf (stderr, "NEWTON_SOLVER: line search failed.\n");

      update_solution (A, theta);

      ns->merhist [ns->iters] = innmer;
#if MPI
      if (dom->rank == 0)
#endif
      if (dom->verbose) printf (fmt_inn, ns->iters, innmer, A->iters);

    } while  (innmer >= beta && ++ns->iters < ns->maxiter);

    innmer = merit_function (A, 0.0, 0.0, 0.0);

    a [0] = kappa * innmer*innmer;
    a [1] = eta1 * A->omega;
    a [2] = omega_func (lambda_func (A, tau * innmer), kappa2 * innmer);
    A->omega = min_func (a, 3);
    a [0] = kappa1 * innmer*innmer;
    a [1] = eta1 * A->epsilon;
    A->epsilon = min_func (a, 2);
    beta = eta * beta;
  }

  destroy_data (A);
}

/* write labeled state values */
void NEWTON_Write_State (NEWTON *ns, PBF *bf)
{
  PBF_Label (bf, "NTITERS");
  PBF_Int (bf, &ns->iters, 1);
}

/* destroy solver */
void NEWTON_Destroy (NEWTON *ns)
{
  free (ns->merhist);
  free (ns);
}
