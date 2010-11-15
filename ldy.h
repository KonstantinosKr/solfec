/*
 * ldy.h
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

#include "mem.h"
#include "bod.h"
#include "sps.h"

#ifndef CONSTRAINT_TYPE
#define CONSTRAINT_TYPE
typedef struct constraint CON;
#endif

#ifndef DOMAIN_TYPE
#define DOMAIN_TYPE
typedef struct domain DOM;
#endif

#ifndef __ldy__
#define __ldy__

typedef struct offb OFFB;
typedef struct diab DIAB;
typedef struct locdyn LOCDYN;

enum solver_kind
{
  NONE_SOLVER          = 0x00,
  GAUSS_SEIDEL_SOLVER  = 0x01,
  PENALTY_SOLVER       = 0x02,
  NEWTON_SOLVER        = 0x04
};

typedef enum solver_kind SOLVER_KIND;

/* off-diagonal block */
struct offb
{
  double W [9], /* generalised inverse inertia block */
	 *SYMW; /* symmetric copy of W block */

  DIAB *dia; /* corresponding diagonal block */
  BODY *bod;
  OFFB *n;
};

/* diagonal block */
struct diab
{
  double    *R, /* average reaction => points to R[3] member of the underlying constraint */
	    *U, /* relative volocity => points to U[3] member of the underlying constraint */
	    *V, /* initial velocity => points to V[3] member of the underlying constraint */ 
	 B [3], /* free velocity */
         W [9], /* generalised inverse inertia block */
	 A [9], /* inverse of W */
	 rho;   /* scaling parameter */

  OFFB *adj; /* note that W(i,j) = W1(i,j) + W2(i,j) {owing to body 1 and 2} and W1 and W2 are SEPARATELY stored here */
  CON *con;  /* the underlying constraint (and the owner od the reaction R[3] and the velocity U [3]) */

  char rowupdate; /* row update flag */

  MX *mH, *mprod, /* master H operator and H inv(M) or inv(M) H^T product */
     *sH, *sprod; /* slave counterpart */
                  /* NOTE: left product can be applied to adjext assembly (MPI)
		           while right product is sligtly faster (serial code) */
#if MPI
  OFFB *adjext;  /* external adjacency */
#endif

  DIAB *p, *n;
};

/* local dynamics */
struct locdyn
{
  MEM offmem,
      diamem;

  DOM *dom; /* domain */
  DIAB *dia; /* list of diagonal blocks */

  short modified; /* 1 if system structure has changed; otherwise 0 */

  double free_energy; /* approximate amount of kinetic energy of local free velocity (per-processor) */
};

/* create local dynamics for a domain */
LOCDYN* LOCDYN_Create (DOM *dom);

/* insert a 'con'straint between a pair of bodies =>
 * return the diagonal entry of the local dynamical system */
DIAB* LOCDYN_Insert (LOCDYN *ldy, CON *con, BODY *one, BODY *two);

/* remove a diagonal entry from local dynamics */
void LOCDYN_Remove (LOCDYN *ldy, DIAB *dia);

/* update local dynamics => prepare for a solution */
void LOCDYN_Update_Begin (LOCDYN *ldy, SOLVER_KIND solver);

/* update local dynamics => after the solution */
void LOCDYN_Update_End (LOCDYN *ldy, SOLVER_KIND solver);

/* dump local dynamics to file */
void LOCDYN_Dump (LOCDYN *ldy, const char *path);

/* free memory */
void LOCDYN_Destroy (LOCDYN *ldy);

#endif
