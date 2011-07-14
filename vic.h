/*
 * vic.h
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

#include "ldy.h"

#ifndef __vic__
#define __vic__

/* C(U,R) + X dU + Y dR, where C(U,R) = F(U) + m(R - F(U)) */
void VIC_Linearize (CON *con, double *U, double *R, double UT, double smoothing_omega, double *C, double *X, double *Y);

/* R = project-on-friction-cone (S) */
void VIC_Project (double friction, double cohesion, double *S, double *R);

#endif
