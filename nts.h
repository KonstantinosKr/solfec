/*
 * nts.h
 * Copyright (C) 2010 Tomasz Koziara (t.koziara AT gmail.com)
 * -------------------------------------------------------------------
 * projected quasi-Newton solver
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

#include "ldy.h"

#ifndef __nts__
#define __nts__

typedef struct newton NEWTON;

struct newton
{
  /* input */

  double meritval; /* value of merit function sufficient for termination */

  int maxiter; /* iterations bound */

  enum {LOCDYN_ON, LOCDYN_OFF} locdyn; /* local dynamics assembling */

  double theta; /* relaxation parameter */

  double epsilon; /* smoothing epsilon */

  int presmooth; /* presmoothing steps */

  /* output */

  double *merhist; /* merit function history */

  int iters; /* iterations count */
};

/* create solver */
NEWTON* NEWTON_Create (double meritval, int maxiter);

/* run solver */
void NEWTON_Solve (NEWTON *ns, LOCDYN *ldy);

/* write labeled state values */
void NEWTON_Write_State (NEWTON *ns, PBF *bf);

/* destroy solver */
void NEWTON_Destroy (NEWTON *bs);

#endif
