/*
 * cuda.h
 * Copyright (C) 2010, Tomasz Koziara (t.koziara AT gmail.com)
 * --------------------------------------------------------------
 * CUDA routines
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

#ifndef __cuda__
#define __cuda__

/* PQN solver; returns the number of iterations and writes the merit function history */
int CUDA_PQN_Solve (LOCDYN *ldy, double meritval, int maxiter, double theta, double epsilon, double *merhist);

#endif
