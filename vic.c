/*
 * vic.c
 * Copyright (C) 2010 Tomasz Koziara (t.koziara AT gmail.com)
 * -------------------------------------------------------------------
 * variational inequality contact formulation
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
#include "dom.h"
#include "vic.h"
#include "alg.h"
#include "err.h"

#define DIFF_FACTOR 1E-10
#define DIFF_BASE   1E-05

/* imaginary i */
static double complex imaginary_i = 0.0;

/* real normal to friction cone */
inline static void real_n (double *S, double fri, double *n)
{
  double dot, len;

  dot = DOT2(S, S);
  len = sqrt(dot);

  if (len == 0 || len <= fri * S[2])
  {
    SET (n, 0.0);
  }
  else if (fri * len + S[2] < 0.0)
  {
    dot += S[2]*S[2];
    len = sqrt (dot);
    if (len == 0) { SET (n, 0.0); }
    else { DIV (S, len, n); }
  }
  else
  {
    dot = 1.0 / sqrt (1.0 + fri*fri);
    DIV2 (S, len, n);
    n [2] = -fri;
    SCALE (n, dot);
  }
}

/* complex normal to friction cone */
inline static void complex_n (double complex *S, double complex fri, double complex *n)
{
  double complex dot, len;

  dot = DOT2(S, S);
  len = csqrt(dot);

  if (creal (len) == 0 || creal (len) <= creal (fri * S[2]))
  {
    SET (n, 0.0 + 0.0 * imaginary_i);
  }
  else if (creal (fri * len + S[2]) < 0.0)
  {
    dot += S[2]*S[2];
    len = csqrt (dot);
    if (creal (len) == 0) { SET(n, 0.0 + 0.0 * imaginary_i); }
    DIV (S, len, n);
  }
  else
  {
    dot = 1.0 / csqrt (1.0 + fri*fri);
    DIV2 (S, len, n);
    n [2] = -fri;
    SCALE (n, dot);
  }
}

/* real normal ray to friction cone */
inline static void real_m (double fri, double *S, double eps, double *m)
{
  double n [3], fun;

  real_n (S, fri, n);
  fun = DOT (S, n);

  if (fun > 0.0 && fun < eps)
  {
    fun = ((2.0/eps) - (1.0/(eps*eps))*fun)*(fun*fun);
  }

  MUL (n, fun, m)
}

/* complex normal ray to friction cone */
inline static void complex_m (double complex fri, double complex *S, double complex eps, double complex *m)
{
  double complex n [3], fun;

  complex_n (S, fri, n);
  fun = DOT (S, n);

  if (creal (fun) > 0.0 && creal (fun) < creal (eps))
  {
    fun = ((2.0/eps) - (1.0/(eps*eps))*fun)*(fun*fun);
  }

  MUL (n, fun, m)
}

/* real F = [UT, UN + fri |UT|]' */
inline static void real_F (double res, double fri, double gap, double step, short dynamic, double epsilon, double *V, double *U, double UT, double *F)
{
  double udash;

  if (dynamic) udash = (U[2] + res * MIN (V[2], 0));
  else udash = ((MAX(gap, 0)/step) + U[2]);

  F [0] = U[0];
  F [1] = U[1];
  if (UT >= 0.0) F [2] = (udash + fri * UT);
  else F [2] = (udash + fri * sqrt (DOT2(U, U) + epsilon*epsilon));
}
 
/* complex F = [UT, UN + fri |UT|]' */
inline static void complex_F (double res, double fri, double gap, double step, short dynamic, double epsilon, double *V, double complex *U, double complex UT, double complex *F)
{
  double complex udash;

  if (dynamic) udash = (U[2] + res * MIN (V[2], 0));
  else udash = ((MAX(gap, 0)/step) + U[2]);

  F [0] = U[0];
  F [1] = U[1];
  if (creal(UT) >= 0) F [2] = (udash + fri * UT);
  else F [2] = (udash + fri * csqrt (DOT2(U, U) + epsilon*epsilon));
}

/* C(U,R) + X dU + Y dR, where C(U,R) = F(U) + m(R - F(U)) */
void VIC_Linearize (CON *con, double *U, double *R, double UT, double smoothing_epsilon, double *C, double *X, double *Y)
{
  DOM *dom = con->master->dom;
  short dynamic = dom->dynamic;
  double *V = con->V,
	  gap = con->gap,
	  fri = con->mat.base->friction,
	  res = con->mat.base->restitution,
	  step = dom->step,
	  h = DIFF_FACTOR * smoothing_epsilon > 0.0 ? smoothing_epsilon : DIFF_BASE,
	  dF [9],
          S [3],
	  F [3],
	  m [3],
	  J [9];

  double complex cU [3],
		 cS [3],
		 cF [3],
		 cm [3];
  int k;

  if (C)
  {
    real_F (res, fri, gap, step, dynamic, smoothing_epsilon, V, U, UT, F);
    SUB (R, F, S);
    real_m (fri, S, smoothing_epsilon, m);
    ADD (F, m, C);
  }

  if (X)
  {
    ASSERT_DEBUG (Y, "X needs to be accompanied by Y in VIC_Linearize");

    if (imaginary_i == 0.0)
    {
      imaginary_i = csqrt (-1); /* initialize */
    }

    for (k = 0; k < 3; k ++)
    {
      cU [0] = U[0] + 0.0 * imaginary_i;
      cU [1] = U[1] + 0.0 * imaginary_i;
      cU [2] = U[2] + 0.0 * imaginary_i;
      cU [k] += h * imaginary_i;
      complex_F (res, fri, gap, step, dynamic, smoothing_epsilon, V, cU, UT, cF);
      dF [3*k+0] = cimag (cF [0]) / h;
      dF [3*k+1] = cimag (cF [1]) / h;
      dF [3*k+2] = cimag (cF [2]) / h;

      cS [0] = S[0] + 0.0 * imaginary_i;
      cS [1] = S[1] + 0.0 * imaginary_i;
      cS [2] = S[2] + 0.0 * imaginary_i;
      cS [k] += h * imaginary_i;
      complex_m (fri, cS, smoothing_epsilon, cm);
      Y [3*k+0] = cimag (cm [0]) / h; /* Y = dm/dS */
      Y [3*k+1] = cimag (cm [1]) / h;
      Y [3*k+2] = cimag (cm [2]) / h;
    }

    IDENTITY (J);
    NNSUB (J, Y, J);
    NNMUL (J, dF, X); /* X = [I - dm/dS] dF/dU */
  }
}

/* R = project-on-friction-cone (S) */
void VIC_Project (double friction, double *S, double *R)
{
  double m [3];

  real_m (friction, S, 0, m);
  SUB (S, m, R);
}
