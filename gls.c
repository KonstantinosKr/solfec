/*
 * gls.c
 * Copyright (C) 2010 Tomasz Koziara (t.koziara AT gmail.com)
 * -------------------------------------------------------------------
 * gluing nonlinear constraint solver
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

#if MPI
#include "put.h"
#endif

#include "gls.h"
#include "glu.h"
#include "alg.h"
#include "bla.h"
#include "err.h"
#include "bgs.c"

/* create solver */
GLUING* GLUING_Create (double epsilon, int maxiter)
{
  GLUING *gl;

  ERRMEM (gl = malloc (sizeof (GLUING)));
  gl->epsilon = epsilon;
  gl->maxiter = maxiter;

  return gl;
}

/* run solver */
void GLUING_Solve (GLUING *gl, LOCDYN *ldy)
{
  double error, merit, step;
  int verbose, diagiters;
  int div = 10, iters;
  short dynamic;
  char fmt [512];

  verbose = ldy->dom->verbose;

  if (verbose) sprintf (fmt, "GLUING: iteration: %%%dd  error:  %%.2e  merit:  %%.2e\n", (int)log10 (1000) + 1);

  int maxiter = 100;
  double epsilon = 1E-3,
	 meritval = 1.0E6;

  GLUE *glue = GLUE_Create (ldy);

  dynamic = ldy->dom->dynamic;
  step = ldy->dom->step;
  iters = 0;
  do
  {
    double errup = 0.0,
	   errlo = 0.0;
    OFFB *blk;
    DIAB *dia;
   
    for (dia = ldy->dia; dia; dia = dia->n)
    {
      double R0 [3],
	     B [3],
	     *R = dia->R;

      CON *con = dia->con;

      if (con->kind == GLUEPNT) continue;

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
	double *W = blk->W,
	       *R = CON(blk->dia)->R;
	NVADDMUL (B, W, R, B);
      }
#endif
      
      COPY (R, R0); /* previous reaction */

      /* solve local diagonal block problem */
      diagiters = DIAGONAL_BLOCK_Solver (GS_PROJECTED_GRADIENT, 1E-6, 100,
	         dynamic, step, con->kind, con->mat.base, con->gap, con->Z, con->base, dia, B);

      /* accumulate relative
       * error components */
      SUB (R, R0, R0);
      errup += DOT (R0, R0);
      errlo += DOT (R, R);
    }

    /* merit function value */
    merit = MERIT_Function (ldy, 1);

    /* calculate relative error */
    error = sqrt (errup) / sqrt (MAX (errlo, 1.0));

#if MPI
    error = PUT_double_max (error);
#endif

    GLUE_Solve (glue, 1E-5, 100);

#if MPI
    DOM_Update_External_Reactions (ldy->dom, 0);
#endif

    if (iters % div == 0 && verbose) printf (fmt, iters, error, merit), div *= 2;
  }
  while (++ iters < maxiter && (error > epsilon || merit > meritval));

  if (verbose) printf (fmt, iters, error, merit);

   GLUE_Destroy (glue); 
}

/* write labeled satate values */
void GLUING_Write_State (GLUING *gl, PBF *bf)
{
}

/* destroy solver */
void GLUING_Destroy (GLUING *gl)
{
  free (gl);
}
