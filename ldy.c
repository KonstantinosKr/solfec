/*
 * ldy.c
 * Copyright (C) 2008, Tomasz Koziara (t.koziara AT gmail.com)
 * ---------------------------------------------------------------
 * the local dynamic problem
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

#include "sol.h"
#include "alg.h"
#include "dom.h"
#include "ldy.h"
#include "lap.h"
#include "msh.h"
#include "err.h"

/* memory block size */
#define BLKSIZE 128

/* apply forward change of variables (nornal
 * contact forces) due to the cohesion, etc. */
static void variables_change_begin (LOCDYN *ldy)
{
  OFFB *blk;
  DIAB *dia;

  for (dia = ldy->dia; dia; dia = dia->n)
  {
    CON *con = dia->con;
    double *B = dia->B; /* free velocity will
			   be eventually modified */

    if (con->state & CON_COHESIVE) /* cohesive state */
    {
      double c = SURFACE_MATERIAL_Cohesion_Get (&con->mat) * con->area,
	     *W = dia->W,
	     *R = dia->R;

      R [2] += c;       /* R_n_new = R_n + c <=> (R_n + c) >= 0 */
      B[0] -= (W[6]*c); /* in consequnce 'W_tn * c' gets subtracted */
      B[1] -= (W[7]*c); /* ... */
      B[2] -= (W[8]*c); /* and 'W_nn * c' here */
    }

    /* off-diagonal subtractions */
    for (blk = dia->adj; blk; blk = blk->n)
    {
      CON *con = blk->dia->con;
      if (con->state & CON_COHESIVE) /* cohesive state */
      {
	double c = SURFACE_MATERIAL_Cohesion_Get (&con->mat) * con->area,
	       *W = blk->W;

	B[0] -= (W[6]*c);
	B[1] -= (W[7]*c);
	B[2] -= (W[8]*c);
      }
    }
  }
}

/* apply back change of variables (nornal
 * contact forces) due to the cohesion, etc. */
static void variables_change_end (LOCDYN *ldy)
{
  DIAB *dia;

  for (dia = ldy->dia; dia; dia = dia->n)
  {
    CON *con = dia->con;
    short state = con->state;

    if (state & CON_COHESIVE) /* cohesive state */
    {
      double c = SURFACE_MATERIAL_Cohesion_Get (&con->mat) * con->area,
	     *R = dia->R;

      R [2] -= c; /* back change */

      if ((state & CON_OPEN) || /* mode-I decohesion */
	(!(state & CON_STICK))) /* mode-II decohesion */
      {
	con->state &= ~CON_COHESIVE;
	SURFACE_MATERIAL_Cohesion_Set (&con->mat, 0.0);
      }
    }
  }
}

/* test whether two constraints are able to be adjacent */
static int adjacentable (BODY *bod, CON *one, CON *two)
{
  if (bod->kind == FEM)
  {
    if (bod->msh)
    {
      ELEMENT **e1, **f1, **e2, **f2;
      CONVEX *c1 = (bod == one->master ? mgobj(one) : sgobj(one)),
	     *c2 = (bod == two->master ? mgobj(two) : sgobj(two));

      for (e1 = c1->ele, f1 = e1 + c1->nele; e1 < f1; e1 ++)
      {
	for (e2 = c2->ele, f2 = e2 + c2->nele; e2 < f2; e2 ++)
	{
	  if (*e1 == *e2) return 1;
	  else if (ELEMENT_Adjacent (*e1, *e2)) return 1;
	}
      }

      return 0;
    }
    else
    {
      ELEMENT *e1 = (bod == one->master ? mgobj(one) : sgobj(one)),
	      *e2 = (bod == two->master ? mgobj(two) : sgobj(two));

      return ELEMENT_Adjacent (e1, e2); /* only in case of a common node W_one_two and W_two_one will be != 0 */
    }
  }

  return 1;
}

/* create local dynamics for a domain */
LOCDYN* LOCDYN_Create (DOM *dom)
{
  LOCDYN *ldy;

  ERRMEM (ldy = malloc (sizeof (LOCDYN)));
  MEM_Init (&ldy->offmem, sizeof (OFFB), BLKSIZE);
  MEM_Init (&ldy->diamem, sizeof (DIAB), BLKSIZE);
  ldy->dom = dom;
  ldy->dia = NULL;
  ldy->modified = 0;

  return ldy;
}

/* insert a 'con'straint between a pair of bodies =>
 * return the diagonal entry of the local dynamical system */
DIAB* LOCDYN_Insert (LOCDYN *ldy, CON *con, BODY *one, BODY *two)
{
  DIAB *dia, *nei;
  double *SYMW;
  SET *item;
  OFFB *b;
  CON *c;

  ERRMEM (dia = MEM_Alloc (&ldy->diamem));
  dia->R = con->R;
  dia->V = con->V;
  dia->B = con->B;
  dia->con = con;

  /* insert into list */
  dia->n = ldy->dia;
  if (ldy->dia)
    ldy->dia->p = dia;
  ldy->dia = dia;

  if (one && one->kind != OBS) /* obstacles do not transfer adjacency */
  {
    for (item = SET_First (one->con); item; item = SET_Next (item))
    {
      c = item->data;

      if (c != con && c->dia && /* skip the coincident or unattached yet constraint */
	  adjacentable (one, con, c)) /* skip other cases where W_ij would be zero */
      {
	nei = c->dia;

        /* allocate block and put into 'nei->adj' list */ 
	ERRMEM (b = MEM_Alloc (&ldy->offmem));
	b->dia = dia; /* adjacent with 'dia' */
	b->bod = one; /* adjacent trough body 'one' */
	SYMW = b->W, b->SYMW = NULL; /* compute when assembling */
	b->n = nei->adj; /* extend list ... */
	nei->adj = b; /* ... */

	/* allocate block and put into 'dia->adj' list */ 
	ERRMEM (b = MEM_Alloc (&ldy->offmem));
	b->dia = nei; /* adjacent with 'nei' */
	b->bod = one; /* ... trough 'one' */
	b->SYMW = SYMW; /* compy when assembling */
	b->n = dia->adj;
	dia->adj = b;
      }
    }
  }

  if (two && two->kind != OBS) /* 'one' replaced with 'two' */
  {
    for (item = SET_First (two->con); item; item = SET_Next (item))
    {
      c = item->data;

      if (c != con && c->dia && /* skip the coincident or unattached yet constraint */
	  adjacentable (two, con, c)) /* skip other cases where W_ij would be zero */
      {
	nei = c->dia;

        /* allocate block and put into 'nei->adj' list */ 
	ERRMEM (b = MEM_Alloc (&ldy->offmem));
	b->dia = dia; /* adjacent with 'dia' */
	b->bod = two; /* adjacent trough body 'two' */
	SYMW = b->W, b->SYMW = NULL; /* compute when assembling */
	b->n = nei->adj; /* extend list ... */
	nei->adj = b; /* ... */

	/* allocate block and put into 'dia->adj' list */ 
	ERRMEM (b = MEM_Alloc (&ldy->offmem));
	b->dia = nei; /* adjacent with 'nei' */
	b->bod = two; /* ... trough 'two' */
	b->SYMW = SYMW; /* compy when assembling */
	b->n = dia->adj;
	dia->adj = b;
      }
    }
  }

  /* mark as modified */
  ldy->modified = 1;

  return dia;
}

/* remove a diagonal entry from local dynamics */
void LOCDYN_Remove (LOCDYN *ldy, DIAB *dia)
{
  OFFB *b, *c, *r;

  /* destroy blocks in
   * adjacent dia items */
  for (b = dia->adj; b; b = b->n)
  {
    c = b->dia->adj;
    if (c && c->dia == dia)
    {
      b->dia->adj = c->n;
      MEM_Free (&ldy->offmem, c); 
    }
    else for (; c; c = c->n)
    {
      if (c->n &&
	  c->n->dia == dia)
      {
	r = c->n;
        c->n = c->n->n;
        MEM_Free (&ldy->offmem, r); 
	break;
      }
    }
  }

  /* destroy directly
   * adjacent blocks */
  for (b = dia->adj; b; b = c)
  {
    c = b->n;
    MEM_Free (&ldy->offmem, b);
  }

  /* remove from list */
  if (dia->p)
    dia->p->n = dia->n;
  else ldy->dia = dia->n;
  if (dia->n)
    dia->n->p = dia->p;

  /* destroy passed dia */
  MEM_Free (&ldy->diamem, dia);

  /* mark as modified */
  ldy->modified = 1;
}

/* updiae local dynamics => prepare for a solution */
void LOCDYN_Update_Begin (LOCDYN *ldy, UPKIND upkind)
{
  DOM *dom = ldy->dom;
  double step = dom->step;
  DIAB *dia;
  OFFB *blk;

#if MPI
  if (dom->rank == 0)
#endif
  if (dom->verbose) printf ("LOCDYN ... "), fflush (stdout);

  SOLFEC_Timer_Start (ldy->dom->solfec, "LOCDYN");

  /* calculate local velocities and
   * assmeble the force-velocity 'W' operator */
  for (dia = ldy->dia; dia; dia = dia->n)
  {
    CON *con = dia->con;
    BODY *m = con->master,
	 *s = con->slave;
    void *mgobj = mgobj(con),
	 *sgobj;
    SHAPE *mshp = mshp(con),
	  *sshp;
    double *mpnt = con->mpnt,
	   *spnt = con->spnt,
	   *base = con->base,
           X [3], Y [9];
    MX_DENSE_PTR (W, 3, 3, dia->W);
    MX_DENSE (C, 3, 3);

    if (s)
    {
      sgobj = sgobj(con);
      sshp = sshp(con);
    }

    /* diagonal block */
    dia->mH = BODY_Gen_To_Loc_Operator (m, mshp, mgobj, mpnt, base);
#if MPI
    dia->mprod = MX_Matmat (1.0, dia->mH, m->inverse, 0.0, NULL);
    MX_Matmat (1.0, dia->mprod, MX_Tran (dia->mH), 0.0, &W); /* H * inv (M) * H^T */
#else
    dia->mprod = MX_Matmat (1.0, m->inverse, MX_Tran (dia->mH), 0.0, NULL);
    MX_Matmat (1.0, dia->mH, dia->mprod, 0.0, &W); /* H * inv (M) * H^T */
#endif
    if (s)
    { 
      dia->sH = BODY_Gen_To_Loc_Operator (s, sshp, sgobj, spnt, base);
#if MPI
      dia->sprod = MX_Matmat (1.0, dia->sH, s->inverse, 0.0, NULL);
      MX_Matmat (1.0, dia->sprod, MX_Tran (dia->sH), 0.0, &C); /* H * inv (M) * H^T */
#else
      dia->sprod = MX_Matmat (1.0, s->inverse, MX_Tran (dia->sH), 0.0, NULL);
      MX_Matmat (1.0, dia->sH, dia->sprod, 0.0, &C); /* H * inv (M) * H^T */
#endif
      NNADD (W.x, C.x, W.x);
    }
    SCALE9 (W.x, step); /* W = h * ( ... ) */

    if (upkind != UPDIA) /* diagonal regularization (not needed by the explicit solver) */
    {
      NNCOPY (W.x, C.x); /* calculate regularisation parameter */
      ASSERT (lapack_dsyev ('N', 'U', 3, C.x, 3, X, Y, 9) == 0, ERR_LDY_EIGEN_DECOMP);
      dia->rho = 1.0 / X [2]; /* inverse of maximal eigenvalue */
    }
  }

  if (upkind == UPALL) /* off-diagonal blocks update only for FULL domain updates */
  {
    for (dia = ldy->dia; dia; dia = dia->n)
    {
      CON *con = dia->con;
      BODY *m = con->master,
	   *s = con->slave;

      /* off-diagonal local blocks */
      for (blk = dia->adj; blk; blk = blk->n)
      {
        if (blk->SYMW) continue; /* skip blocks pointing to their symmetric copies */

	MX *left, *right;
	DIAB *adj = blk->dia;
	CON *con = adj->con;
	BODY *bod = blk->bod;
	MX_DENSE_PTR (W, 3, 3, blk->W);
	double coef;

	ASSERT_DEBUG (bod == m || bod == s, "Off diagonal block is not connected!");
       
#if MPI
	left = (bod == m ? dia->mprod : dia->sprod);
#else
	left = (bod == m ? dia->mH : dia->sH);
#endif

	if (bod == con->master)
	{
#if MPI
	  right = adj->mH;
#else
	  right =  adj->mprod;
#endif
	  coef = (bod == s ? -step : step);
	}
	else /* blk->bod == dia->slave */
	{
#if MPI
	  right = adj->sH;
#else
	  right =  adj->sprod;
#endif
	  coef = (bod == m ? -step : step);
	}

#if MPI
	MX_Matmat (1.0, left, MX_Tran (right), 0.0, &W);
#else
	MX_Matmat (1.0, left, right, 0.0, &W);
#endif
	SCALE9 (W.x, coef);
      }
    }

    /* use symmetry */
    for (dia = ldy->dia; dia; dia = dia->n)
    {  
      for (blk = dia->adj; blk; blk = blk->n)
      {
	if (blk->SYMW)
	{
	  TNCOPY (blk->SYMW, blk->W); /* transposed copy of a symmetric block */
	}
      }
    }

    /* clean up */
    for (dia = ldy->dia; dia; dia = dia->n)
    {
      MX_Destroy (dia->mH);
      MX_Destroy (dia->mprod);
      if (dia->sH)
      {
	MX_Destroy (dia->sH);
	MX_Destroy (dia->sprod);
      }
    }
  }

  /* forward variables change */
  variables_change_begin (ldy);

  SOLFEC_Timer_End (ldy->dom->solfec, "LOCDYN");
}

/* updiae local dynamics => after the solution */
void LOCDYN_Update_End (LOCDYN *ldy)
{
  SOLFEC_Timer_Start (ldy->dom->solfec, "LOCDYN");

  /* backward variables change */
  variables_change_end (ldy);

  /* not modified */
  ldy->modified = 0;

  SOLFEC_Timer_End (ldy->dom->solfec, "LOCDYN");
}

/* free memory */
void LOCDYN_Destroy (LOCDYN *ldy)
{
  MEM_Release (&ldy->diamem);
  MEM_Release (&ldy->offmem);

  free (ldy);
}
