/*
 * dom.c
 * Copyright (C) 2008, Tomasz Koziara (t.koziara AT gmail.com)
 * ---------------------------------------------------------------
 * a domain gathers bodies and constraints
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

#include <string.h>
#include <limits.h>
#include <float.h>
#include "sol.h"
#include "alg.h"
#include "msh.h"
#include "cvx.h"
#include "sph.h"
#include "set.h"
#include "dom.h"
#include "goc.h"
#include "tmr.h"
#include "pck.h"
#include "err.h"
#include "dio.h"

#if MPI
#include "put.h"
#include "com.h"
#endif

#define MEMBLK 128 /* initial memory block size for state packing */
#define CONBLK 128 /* constraints memory block size */
#define MAPBLK 128 /* map items memory block size */
#define SETBLK 128 /* set items memory block size */
#define AABB_SIZE (HASH3D+1) /* aabb timing tables size */

typedef struct private_data DATA;

struct private_data
{
  double aabb_timings [AABB_SIZE],
	 aabb_limits [AABB_SIZE+1];

  int aabb_counter;

  BOXALG aabb_algo;
};

/* create private data */
static DATA* data_create (void)
{
  double part;
  DATA *data;

  ERRMEM (data = malloc (sizeof (DATA)));

  part = 1.0 / (double) AABB_SIZE;
  data->aabb_limits [0] = 0.0;
  data->aabb_counter = 0;
  data->aabb_algo = 0;

  return data;
}

/* free private data */
static void data_destroy (DATA *data)
{
  free (data);
}

/* fastest box overlap algorithm */
static BOXALG aabb_algorithm (DOM *dom)
{
#if 0
  DATA *data = dom->data;
  double num, *tim, *lim;
  int i;

  if (data->aabb_counter < AABB_SIZE)
  {
    data->aabb_algo = data->aabb_counter ++; /* at first test all algorithms */
  }
  else /* when all have been tested */
  {
    tim = data->aabb_timings;
    lim = data->aabb_limits;

    for (i = 0; i < AABB_SIZE; i ++) lim [i+1] = lim [i] + tim [i]; /* sum up */
    for (i = 1; i <= AABB_SIZE; i ++) lim [i] /= lim [AABB_SIZE]; /* normalize */

    num = DRAND(); /* random in [0, 1] */

    for (i = 0; i < AABB_SIZE; i ++)
    {
      if (num >= lim [i] && num < lim [i+1])
      {
	data->aabb_algo = i;
	return i;
      }
    }
  }

  return data->aabb_algo;
#else
  return HYBRID; /* FIXME: all other algorithms need more tesing */
#endif
}

/* update timing related data */
static void aabb_timing (DOM *dom, double timing)
{
  DATA *data = dom->data;

  data->aabb_timings [data->aabb_algo] = timing;
}

/* calculate orthonormal
 * base storing it in column-wise manner
 * in 'loc' with 'n' as the last column */
static void localbase (double *n, double *loc)
{
  double  len,
    e [2][3] = { {1., 0., 0.},
                 {0., 1., 0.} };
  loc [6] = n [0];
  loc [7] = n [1];
  loc [8] = n [2];
  PRODUCT (e [0], n, loc);
  if ((len = LEN (loc)) < GEOMETRIC_EPSILON)
  {
    PRODUCT (e [1], n, loc);
    len = LEN (loc);
  }
  loc [0] /= len;
  loc [1] /= len;
  loc [2] /= len;
  PRODUCT (loc, n, (loc + 3));
}

/* compare two constraints */
#define CONCMP ((SET_Compare) constraint_compare)
static int constraint_compare (CON *one, CON *two)
{
  BODY *onebod [2], *twobod [2];
  SGP *onesgp [2], *twosgp [2];

  if (one->slave == NULL) /* left one-body constraint */
  {
    if (two->slave) return -1; /* one-body constraints always smaller then two body ones */
    else return (one < two ? -1 : (one == two ? 0 : 1)); /* compare them by pointer within them-selves (note that they always migrate with bodies) */
  }
  else if (two->slave == NULL) return 1; /* right one-body constraint */

  if (one->master < one->slave) /* two-body constraints remain; order pointers before comparing */
  {
    onebod [0] = one->master;
    onesgp [0] = one->msgp;
    onebod [1] = one->slave;
    onesgp [1] = one->ssgp;
  }
  else
  {
    onebod [0] = one->slave;
    onesgp [0] = one->ssgp;
    onebod [1] = one->master;
    onesgp [1] = one->msgp;
  }

  if (two->master < two->slave)
  {
    twobod [0] = two->master;
    twosgp [0] = two->msgp;
    twobod [1] = two->slave;
    twosgp [1] = two->ssgp;
  }
  else
  {
    twobod [0] = two->slave;
    twosgp [0] = two->ssgp;
    twobod [1] = two->master;
    twosgp [1] = two->msgp;
  }

  if (onebod [0] < twobod [0]) return -1; /* compare lexicographically by body pointers and sgps */
  else if (onebod [0] == twobod [0])
  {
    if (onebod [1] < twobod [1]) return -1;
    else if (onebod [1] == twobod [1])
    {
      if (onesgp [0] < twosgp [0]) return -1;
      else if (onesgp [0] == twosgp [0])
      {
	if (onesgp [1] < twosgp [1]) return -1;
	else if (onesgp [1] == twosgp [1]) return 0;
      }
    }
  }

  return 1;
}

/* insert a new constrait between two bodies */
static CON* insert (DOM *dom, BODY *master, BODY *slave, SGP *msgp, SGP *ssgp)
{
  CON *con;

  /* ensure that master pointers were passed;
   * tollerate NULL slave pointers, as this indicates a single body constraint */
  ASSERT_DEBUG (master && msgp, "At least master body pointers must be passed");

  ERRMEM (con = MEM_Alloc (&dom->conmem));
  con->master = master;
  con->slave = slave;
  con->msgp = msgp;
  con->ssgp = ssgp;

  /* insert into body constraint adjacency */
  SET_Insert (&dom->setmem, &master->con, con, CONCMP);
  if (slave) SET_Insert (&dom->setmem, &slave->con, con, CONCMP);
 
  /* insert into list */
  con->next = dom->con;
  if (dom->con) dom->con->prev = con;
  dom->con = con;
  dom->ncon ++;

#if MPI
  if (dom->noid == 0) /* if id generation is enabled */
  {
#endif

  if (SET_Size (dom->sparecid))
  {
    SET *item;
   
    item = SET_First (dom->sparecid);
    con->id = (unsigned int) (long) item->data; /* use a previously freed id */
    SET_Delete (&dom->setmem, &dom->sparecid, item->data, NULL);
  }
  else
  {
    con->id = dom->cid;

#if MPI
    ASSERT (((unsigned long long) dom->cid) + ((unsigned long long) dom->ncpu) < UINT_MAX, ERR_DOM_TOO_MANY_CONSTRAINTS);
    dom->cid += dom->ncpu; /* every ncpu number */
#else
    ASSERT (((unsigned long long) dom->cid) + ((unsigned long long) 1) < UINT_MAX, ERR_DOM_TOO_MANY_CONSTRAINTS);
    con->id = dom->cid ++;
#endif
  }

#if MPI
  }
  else con->id = dom->noid; /* assign the 'noid' as it was imported with a constraint */
#endif

  /* map by id */
  MAP_Insert (&dom->mapmem, &dom->idc, (void*) (long) con->id, con, NULL);

  return con;
}

/* insert a contact into the constraints set */
static void insert_contact (DOM *dom, BODY *master, BODY *slave, SGP *msgp, SGP *ssgp, double *mpntspa,
       double *spntspa, double *normal, double area , double gap, SURFACE_MATERIAL *mat, short paircode)
{
  CON *con;

  con = insert (dom, master, slave, msgp, ssgp); /* do not insert into LOCDYN yet, only after sparsification */
  con->kind = CONTACT;
  COPY (mpntspa, con->point);
  BODY_Ref_Point (master, msgp->shp, msgp->gobj, mpntspa, con->mpnt); /* referential image */
  BODY_Ref_Point (slave, ssgp->shp, ssgp->gobj, spntspa, con->spnt);
  localbase (normal, con->base);
  con->area = area;
  con->gap = gap;
  con->paircode = paircode;
  con->state |= SURFACE_MATERIAL_Transfer (dom->time, mat, &con->mat); /* transfer surface pair data from the database to the local variable */
  con->state |= CON_NEW;  /* mark as newly created */
}

/* does a potential contact already exists ? */
static int contact_exists (BOX *one, BOX *two)
{
  CON aux;

  aux.master = one->body;
  aux.msgp = one->sgp;
  aux.slave = two->body;
  aux.ssgp = two->sgp;

  return SET_Contains (one->body->con, &aux, CONCMP);
}

/* box overlap creation callback */
static void overlap_create (DOM *dom, BOX *one, BOX *two)
{
  double onepnt [3], twopnt [3], normal [3], gap, area;
  int state, spair [2];
  short paircode;
  SURFACE_MATERIAL *mat;

  if (contact_exists (one, two)) return;

  state = gobjcontact (
    CONTACT_DETECT, GOBJ_Pair_Code (one, two),
    one->sgp->shp, one->sgp->gobj,
    two->sgp->shp, two->sgp->gobj,
    onepnt, twopnt, normal, &gap, &area, spair);

  if (state)
  {
    if (gap <= dom->depth) dom->flags |= DOM_DEPTH_VIOLATED;

    /* set surface pair data if there was a contact */
    mat = SPSET_Find (dom->sps, spair [0], spair [1]);
  }

  switch (state)
  {
    case 1: /* first body is the master */
    {
      paircode = GOBJ_Pair_Code (one, two);
      insert_contact (dom, one->body, two->body, one->sgp, two->sgp, onepnt, twopnt, normal, area, gap, mat, paircode);
    }
    break;
    case 2: /* second body is the master */
    {
      paircode = GOBJ_Pair_Code (two, one);
      insert_contact (dom, two->body, one->body, two->sgp, one->sgp, twopnt, onepnt, normal, area, gap, mat, paircode);
    }
    break;
  }
}

/* update contact data */
static void update_contact (DOM *dom, CON *con)
{
  double mpnt [3], spnt [3], normal [3];
  int state, spair [2] = {con->mat.base->surf1,
                          con->mat.base->surf2};
  void *mgobj = mgobj(con),
       *sgobj = sgobj(con);
  SHAPE *mshp = mshp(con),
	*sshp = sshp(con);

  /* current spatial points and normal */
  BODY_Cur_Point (con->master, mshp, mgobj, con->mpnt, mpnt);
  BODY_Cur_Point (con->slave, sshp, sgobj, con->spnt, spnt);
  COPY (con->base+6, normal);

  /* update contact data => during an update 'master' and 'slave' relation does not change */
  state = gobjcontact (
    CONTACT_UPDATE, con->paircode,
    mshp, mgobj, sshp, sgobj,
    mpnt, spnt, normal, &con->gap, /* 'mpnt' and 'spnt' are updated here */
    &con->area, spair); /* surface pair might change though */

  if (state)
  {
    if (con->gap <= dom->depth) dom->flags |= DOM_DEPTH_VIOLATED;

    COPY (mpnt, con->point);
    BODY_Ref_Point (con->master, mshp, mgobj, mpnt, con->mpnt);
    BODY_Ref_Point (con->slave, sshp, sgobj, spnt, con->spnt);
    localbase (normal, con->base);
    if (state > 1) /* surface pair has changed */
    {
      SURFACE_MATERIAL *mat = SPSET_Find (dom->sps, spair [0], spair [1]); /* find new surface pair description */
      con->state |= SURFACE_MATERIAL_Transfer (dom->time, mat, &con->mat); /* transfer surface pair data from the database to the local variable */
    }
  }
#if MPI
  else if (dom->balancing == FULL_BALANCING) DOM_Remove_Constraint (dom, con);
  else dom->deletions ++;
#else
  else DOM_Remove_Constraint (dom, con);
#endif
}

/* update fixed point data */
static void update_fixpnt (DOM *dom, CON *con)
{
  BODY_Cur_Point (con->master, mshp(con), mgobj(con), con->mpnt, con->point);
}

/* update fixed direction data */
static void update_fixdir (DOM *dom, CON *con)
{
  BODY_Cur_Point (con->master, mshp(con), mgobj(con), con->mpnt, con->point);
}

/* update velocity direction data */
static void update_velodir (DOM *dom, CON *con)
{
  VELODIR (con->Z) = TMS_Value (con->tms, dom->time + dom->step);
  BODY_Cur_Point (con->master, mshp(con), mgobj(con), con->mpnt, con->point);
}

/* update rigid link data */
static void update_riglnk (DOM *dom, CON *con)
{
  double n [3],
	 m [3],
	 s [3],
	 len;

  if (con->master && con->slave)
  {
    BODY_Cur_Point (con->master, mshp(con), mgobj(con), con->mpnt, m);
    BODY_Cur_Point (con->slave, sshp(con), sgobj(con), con->spnt, s);
  }
  else /* master point to a spatial point link */
  {
    BODY_Cur_Point (con->master, mshp(con), mgobj(con), con->mpnt, m);
    COPY (con->spnt, s);
  }

  COPY (m, con->point);
  SUB (s, m, RIGLNK_VEC (con->Z));
  SUB (s, m, n);
  len = LEN (n);
  con->gap = len - RIGLNK_LEN(con->Z);
  len = 1.0 / len;
  SCALE (n, len);
  localbase (n, con->base);
}

/* tell whether the geometric objects are topologically adjacent */
static int gobj_adjacent (short paircode, void *aobj, void *bobj)
{
  switch (paircode)
  {
    case AABB_ELEMENT_ELEMENT: return ELEMENT_Adjacent (aobj, bobj);
    case AABB_CONVEX_CONVEX: return CONVEX_Adjacent (aobj, bobj);
    case AABB_SPHERE_SPHERE: return SPHERE_Adjacent (aobj, bobj);
  }

  return 0;
}

#if MPI
typedef struct domain_balancing_data DBD;

/* balancing data */
struct domain_balancing_data
{
  int rank;
  DOM *dom;
  SET *bodies;
  SET *constraints;
  SET *children;
  SET *glue;
};

/* compute body weight */
static int body_weight (BODY *bod)
{
  SET *item;
  CON *con;
  int ncon;

  for (item = SET_First (bod->con), ncon = 0; item; item = SET_Next (item))
  {
    con = item->data;
    if (con->slave) ncon ++; /* one-body constraints migrate with the body, hence they increase its weight */
  }

  return  (1 + ncon) * bod->dofs;
}

/* compute constraint weight */
static int constraint_weight (CON *con)
{
  return con->master->dofs + (con->slave ? con->slave->dofs : 0);
}

/* number of objects for balacing */
static int object_count (DOM *dom, int *ierr)
{
  *ierr = ZOLTAN_OK;
  int ncon;
  CON *con;

  for (con = dom->con, ncon = 0; con; con = con->next)
  {
    if (con->slave) ncon ++; /* only two-body constraints migrate independently */
  }

  return dom->nbod + ncon;
}

/* list of object identifiers for load balancing */
static void object_list (DOM *dom, int num_gid_entries, int num_lid_entries,
  ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR local_ids, int wgt_dim, float *obj_wgts, int *ierr)
{
  BODY *bod;
  CON *con;
  int i;
  
  for (bod = dom->bod, i = 0; bod; i ++, bod = bod->next)
  {
    global_ids [i * num_gid_entries] = bod->id;
    obj_wgts [i * wgt_dim] = body_weight (bod);
  }

  for (con = dom->con; con; con = con->next)
  {
    if (con->slave)
    {
      global_ids [i * num_gid_entries] = dom->bid + con->id;
      obj_wgts [i * wgt_dim] = constraint_weight (con);
      i ++;
    }
  }

  *ierr = ZOLTAN_OK;
}

/* number of spatial dimensions */
static int dimensions (DOM *dom, int *ierr)
{
  *ierr = ZOLTAN_OK;
  return 3;
}

/* list of object points exploited during load balancing */
static void objpoints (DOM *dom, int num_gid_entries, int num_lid_entries, int num_obj,
  ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR local_ids, int num_dim, double *geom_vec, int *ierr)
{
  unsigned int id;
  double *e, *v;
  BODY *bod;
  CON *con;
  int i;

  for (i = 0; i < num_obj; i ++)
  {
    id = global_ids [i * num_gid_entries];
    v = &geom_vec [i* num_dim];

    bod = MAP_Find (dom->idb, (void*) (long) id, NULL);

    if (bod)
    {
      e = bod->extents;
      MID (e, e+3, v);
    }
    else
    {
      ASSERT_DEBUG_EXT (con = MAP_Find (dom->idc, (void*) (long) (id - dom->bid), NULL), "Invalid constraint id");
      COPY (con->point, v);
    }
  }

  *ierr = ZOLTAN_OK;
}

/* pack constraint migrating out during */
static void pack_constraint (CON *con, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  pack_int (isize, i, ints, con->kind);

  pack_int (isize, i, ints, con->id);
  pack_int (isize, i, ints, con->master->id);
  if (con->slave) pack_int (isize, i, ints, con->slave->id);
  else pack_int (isize, i, ints, 0);

  pack_int (isize, i, ints, con->msgp - con->master->sgp);
  if (con->slave) pack_int (isize, i, ints, con->ssgp - con->slave->sgp);

  pack_doubles (dsize, d, doubles, con->mpnt, 3);
  if (con->slave) pack_doubles (dsize, d, doubles, con->spnt, 3);

  pack_doubles (dsize, d, doubles, con->R, 3);
  pack_doubles (dsize, d, doubles, con->point, 3);
  pack_doubles (dsize, d, doubles, con->base, 9);
  pack_double  (dsize, d, doubles, con->gap);

  switch ((int) con->kind)
  {
    case CONTACT:
    pack_double  (dsize, d, doubles, con->area);
    pack_int (isize, i, ints, con->paircode);
    SURFACE_MATERIAL_Pack_State (&con->mat, dsize, d, doubles, isize, i, ints);
    break;
    case VELODIR:
    TMS_Pack (con->tms, dsize, d, doubles, isize, i, ints);
    pack_doubles (dsize, d, doubles, con->Z, DOM_Z_SIZE);
    break;
    case RIGLNK:
    pack_doubles (dsize, d, doubles, con->Z, DOM_Z_SIZE);
    break;
  }

  con->state |= CON_IDLOCK; /* prevent id deletion */

  DOM_Remove_Constraint (con->master->dom, con); /* remove from the domain */
}

/* unpack constraint migrated in during load balancing */
static void unpack_constraint (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int kind, cid, mid, sid, n;
  BODY *master, *slave;
  SGP *msgp, *ssgp;
  CON *con;

  kind = unpack_int (ipos, i, ints);

  cid = unpack_int (ipos, i, ints);
  mid = unpack_int (ipos, i, ints);
  sid = unpack_int (ipos, i, ints);

  ASSERT_DEBUG_EXT (master = MAP_Find (dom->allbodies, (void*) (long) mid, NULL), "Invalid body id");
  if (sid) ASSERT_DEBUG_EXT (slave = MAP_Find (dom->allbodies, (void*) (long) sid, NULL), "Invalid body id"); else slave = NULL;

  n = unpack_int (ipos, i, ints);
  msgp = &master->sgp [n];

  if (slave) n = unpack_int (ipos, i, ints), ssgp = &slave->sgp [n]; else ssgp = NULL;

  dom->noid = cid; /* disable constraint ids generation and use 'noid' instead */

  con = insert (dom, master, slave, msgp, ssgp);

  dom->noid = 0; /* enable constraint ids generation */

  con->kind = kind;

  unpack_doubles (dpos, d, doubles, con->mpnt, 3);
  if (slave) unpack_doubles (dpos, d, doubles, con->spnt, 3);

  unpack_doubles (dpos, d, doubles, con->R, 3);
  unpack_doubles (dpos, d, doubles, con->point, 3);
  unpack_doubles (dpos, d, doubles, con->base, 9);
  con->gap = unpack_double  (dpos, d, doubles);

  switch (kind)
  {
    case CONTACT:
    con->area = unpack_double  (dpos, d, doubles);
    con->paircode = unpack_int (ipos, i, ints);
    SURFACE_MATERIAL_Unpack_State (dom->sps, &con->mat, dpos, d, doubles, ipos, i, ints);
    break;
    case VELODIR:
    con->tms = TMS_Unpack (dpos, d, doubles, ipos, i, ints);
    unpack_doubles (dpos, d, doubles, con->Z, DOM_Z_SIZE);
    break;
    case RIGLNK:
    unpack_doubles (dpos, d, doubles, con->Z, DOM_Z_SIZE);
    break;
  }

  con->dia = LOCDYN_Insert (dom->ldy, con, con->master, con->slave); /* insert into local dynamics */
}

/* insert a new external constraint migrated in during domain gluing */
static CON* insert_external_constraint (DOM *dom, BODY *master, BODY *slave, SGP *msgp, SGP *ssgp, unsigned int cid)
{
  CON *con;

  ERRMEM (con = MEM_Alloc (&dom->conmem));
  con->master = master;
  con->slave = slave;
  con->msgp = msgp;
  con->ssgp = ssgp;

  /* add to the body constraint adjacency */
  SET_Insert (&dom->setmem, &master->con, con, CONCMP);
  if (slave) SET_Insert (&dom->setmem, &slave->con, con, CONCMP);
 
  /* constraint identifier */
  con->id = cid;

  /* insert into external map */
  MAP_Insert (&dom->mapmem, &dom->conext, (void*) (long) cid, con, NULL);

  /* state */
  con->state |= CON_EXTERNAL;

  return con;
}

/* pack boundary constraint migrating out during domain gluing */
static void pack_boundary_constraint (CON *con, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  pack_int (isize, i, ints, con->id);
  pack_int (isize, i, ints, con->master->id);

  if (con->slave) pack_int (isize, i, ints, con->slave->id);
  else pack_int (isize, i, ints, 0);

  pack_int (isize, i, ints, con->msgp - con->master->sgp);
  if (con->slave) pack_int (isize, i, ints, con->ssgp - con->slave->sgp);

  pack_doubles (dsize, d, doubles, con->mpnt, 3);
  if (con->slave) pack_doubles (dsize, d, doubles, con->spnt, 3);

  pack_doubles (dsize, d, doubles, con->R, 3);
  pack_doubles (dsize, d, doubles, con->base, 9);
}

/* unpack external constraint migrated in during domain gluing */
static void unpack_external_constraint (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  BODY *master, *slave;
  int cid, mid, sid, n;
  SGP *msgp, *ssgp;
  CON *con;

  cid = unpack_int (ipos, i, ints);
  mid = unpack_int (ipos, i, ints);
  sid = unpack_int (ipos, i, ints);

  ASSERT_DEBUG_EXT (master = MAP_Find (dom->allbodies, (void*) (long) mid, NULL), "Invalid body id");
  if (sid) ASSERT_DEBUG_EXT (slave = MAP_Find (dom->allbodies, (void*) (long) sid, NULL), "Invalid body id"); else slave = NULL;

  if (dom->dynamic) BODY_Dynamic_Init (master);
  else BODY_Static_Init (master);

  n = unpack_int (ipos, i, ints);
  msgp = &master->sgp [n];

  if (slave)
  {
    if (dom->dynamic) BODY_Dynamic_Init (slave);
    else BODY_Static_Init (slave);

    n = unpack_int (ipos, i, ints);
    ssgp = &slave->sgp [n];
  }
  else ssgp = NULL;

  con = insert_external_constraint (dom, master, slave, msgp, ssgp, cid);

  unpack_doubles (dpos, d, doubles, con->mpnt, 3);
  if (slave) unpack_doubles (dpos, d, doubles, con->spnt, 3);

  unpack_doubles (dpos, d, doubles, con->R, 3);
  unpack_doubles (dpos, d, doubles, con->base, 9);
}

/* pack migrating out parent body */
static void pack_parent (BODY *bod, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  DOM *dom;

  /* must be parent */
  ASSERT_DEBUG (bod->flags & BODY_PARENT, "Not a parent");

  /* set domain */
  dom = bod->dom;

  /* pack id */
  pack_int (isize, i, ints, bod->id);

  /* pack state */
  BODY_Parent_Pack (bod, dsize, d, doubles, isize, i, ints);

  /* delete from label map */
  if (bod->label) MAP_Delete (&dom->mapmem, &dom->lab, bod->label, (MAP_Compare)strcmp);

  /* delete from id based map */
  MAP_Delete (&dom->mapmem, &dom->idb, (void*) (long) bod->id, NULL);

  /* remove from list */
  if (bod->prev) bod->prev->next = bod->next;
  else dom->bod = bod->next;
  if (bod->next) bod->next->prev = bod->prev;

  /* decrement */
  dom->nbod --;

  /* unmark parent */
  bod->flags &= ~BODY_PARENT;
}

/* unpack migrated in parent body */
static void unpack_parent (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  BODY *bod;
  int id;

  /* unpack id */
  id = unpack_int (ipos, i, ints);

  /* find body */
  ASSERT_DEBUG_EXT (bod = MAP_Find (dom->allbodies, (void*) (long) id, NULL), "Invalid body id");

  /* must be child or dummy */
  ASSERT_DEBUG ((bod->flags & BODY_PARENT) == 0, "Neither child nor dummy");

  /* unpack state */
  BODY_Parent_Unpack (bod, dpos, d, doubles, ipos, i, ints);

  /* if it was a child */
  if (bod->flags & BODY_CHILD)
  {
    /* unmark child */
    bod->flags &= ~BODY_CHILD;

    /* delete from children set */
    SET_Delete (&dom->setmem, &dom->children, bod, NULL);
  }

  /* insert into label map */
  if (bod->label) MAP_Insert (&dom->mapmem, &dom->lab, bod->label, bod, (MAP_Compare)strcmp);

  /* insert into id based map */
  MAP_Insert (&dom->mapmem, &dom->idb, (void*) (long) bod->id, bod, NULL);

  /* insert into list */
  bod->prev = NULL;
  bod->next = dom->bod;
  if (dom->bod) dom->bod->prev = bod;
  dom->bod = bod;

  /* increment */
  dom->nbod ++;

  /* update rank */
  bod->rank = dom->rank;

  /* mark as parent */
  bod->flags |= BODY_PARENT;
}

/* pack migrating out child body */
static void pack_child (BODY *bod, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  /* must be an exported or an existing parent */
  ASSERT_DEBUG (((bod->flags & (BODY_PARENT|BODY_CHILD)) == 0 && bod->rank != bod->dom->rank) /* just migrating out parent after being packed (hence unmarked) */
                || (bod->flags & BODY_PARENT), "Not a parent"); /* or an existing parent */

  /* pack id */
  pack_int (isize, i, ints, bod->id);

  /* pack state */
  BODY_Child_Pack (bod, dsize, d, doubles, isize, i, ints);
}

/* unpack child body */
static void unpack_child (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  BODY *bod;
  int id;

  /* unpack id */
  id = unpack_int (ipos, i, ints);

  /* find body */
  ASSERT_DEBUG_EXT (bod = MAP_Find (dom->allbodies, (void*) (long) id, NULL), "Invalid body id");

  /* must be child or dummy */
  ASSERT_DEBUG ((bod->flags & BODY_PARENT) == 0, "Neither child nor dummy");

  /* unpack state */
  BODY_Child_Unpack (bod, dpos, d, doubles, ipos, i, ints);

  /* if it was a dummy */
  if ((bod->flags & BODY_CHILD) == 0)
  {
    /* insert into children set */
    SET_Insert (&dom->setmem, &dom->children, bod, NULL);

    /* mark as child */
    bod->flags |= BODY_CHILD;
  }

  /* mark as updated */
  bod->flags |= BODY_CHILD_UPDATED;
}

/* pack statistics */
static void pack_stats (DOM *dom, int rank, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  DOMSTATS *s, *e;

  pack_int (isize, i, ints, dom->nbod);
  pack_int (isize, i, ints, dom->aabb->boxnum);
  pack_int (isize, i, ints, dom->ncon);
  pack_int (isize, i, ints, MAP_Size (dom->conext));
  pack_int (isize, i, ints, dom->nspa);
  pack_int (isize, i, ints, dom->deletions);
  pack_int (isize, i, ints, dom->bytes);

  if (rank == (dom->ncpu-1)) /* last set was packed => zero current statistics record */
  {
    for (s = dom->stats, e = s + dom->nstats; s < e; s ++)
    {
      s->sum = s->max = 0;
      s->min = INT_MAX;
    }
  }
}

/* unpack statistics */
static void unpack_stats (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  DOMSTATS *s, *e;
  int val;

  for (s = dom->stats, e = s + dom->nstats; s < e; s ++)
  {
    val = unpack_int (ipos, i, ints);
    s->sum += val;
    s->min = MIN (s->min, val);
    s->max = MAX (s->max, val);
  }
}

/* create statistics */
static void stats_create (DOM *dom)
{
  dom->nstats = 7;

  ERRMEM (dom->stats = MEM_CALLOC (sizeof (DOMSTATS [dom->nstats])));
  
  dom->stats [0].name = "BODIES";
  dom->stats [1].name = "BOXES";
  dom->stats [2].name = "CONSTRAINTS";
  dom->stats [3].name = "EXTERNAL";
  dom->stats [4].name = "SPARSIFIED";
  dom->stats [5].name = "DELETIONS";
  dom->stats [6].name = "BYTES SENT";
}

/* compute statistics */
static void stats_compute (DOM *dom)
{
  DOMSTATS *s, *e;

  for (s = dom->stats, e = s + dom->nstats; s < e; s ++)
  {
    s->avg = s->sum / dom->ncpu;
  }

  dom->ratio = (double) dom->stats [5].sum / (double) (dom->stats [2].sum + 1);
}

/* destroy statistics */
static void stats_destroy (DOM *dom)
{
  free (dom->stats);
}

/* compute ranks of migrating children */
static void children_migration_begin (DOM *dom, DBD *dbd)
{
  int *procs, numprocs, i;
  BODY *bod;

  ERRMEM (procs = malloc (sizeof (int [dom->ncpu])));

  for (bod = dom->bod; bod; bod = bod->next)
  {
    /* must be a parent */
    ASSERT_DEBUG (bod->flags & BODY_PARENT, "Not a parent");

    double *e = bod->extents;

    Zoltan_LB_Box_Assign (dom->zol, e[0], e[1], e[2], e[3], e[4], e[5], procs, &numprocs);

    if (dom->balancing == FULL_BALANCING) SET_Free (&dom->setmem, &bod->children); /* empty children set */

    /* during PARTIAL_BALANCING only extend the previous set of body children by new ranks;
     * constraints, which do not migrated in this mode, maintain this way their attachment
     * to children which cannot migrate out; this could have been possible otherwise */

    for (i = 0; i < numprocs; i ++)
    {
      if (bod->rank != procs [i]) /* if this is neither current nor the new body rank */
      {
        SET_Insert (&dom->setmem, &dbd [procs [i]].children, bod, NULL); /* schedule for sending a child */
	SET_Insert (&dom->setmem, &bod->children, (void*) (long) procs [i], NULL); /* restore parent's children set */
      }
    }
  }

  free (procs);
}

/* delete migrated out children */
static void children_migration_end (DOM *dom)
{
  SET *delset, *item;

  delset = NULL;

  for (item = SET_First (dom->children); item; item = SET_Next (item))
  {
    BODY *bod = item->data;

    /* must be a child */
    ASSERT_DEBUG (bod->flags & BODY_CHILD, "Not a child");

    if ((bod->flags & BODY_CHILD_UPDATED) == 0) /* migrated out as it wasn't updated by a parent */
    {
      bod->flags &= ~BODY_CHILD; /* unmark child */
      
      SET_Insert (&dom->setmem, &delset, bod, NULL); /* schedule deletion from dom->children */
    }
    else bod->flags &= ~BODY_CHILD_UPDATED; /* invalidate update flag */
  }

  /* subtract deleted children from domain children set */
  for (item = SET_First (delset); item; item = SET_Next (item))
  {
    SET_Delete (&dom->setmem, &dom->children, item->data, NULL);
  }

  SET_Free (&dom->setmem, &delset);
}

/* pack domain balancing data */
static void domain_balancing_pack (DBD *dbd, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  SET *item;

  /* pack spare body ids */
  pack_int (isize, i, ints, SET_Size (dbd->dom->sparebid));
  for (item = SET_First (dbd->dom->sparebid); item; item = SET_Next (item))
    pack_int (isize, i, ints, (int) (long) item->data);

  /* pack exported bodies */
  pack_int (isize, i, ints, SET_Size (dbd->bodies));
  for (item = SET_First (dbd->bodies); item; item = SET_Next (item))
    pack_parent (item->data, dsize, d, doubles, isize, i, ints);

  /* pack exported children */
  pack_int (isize, i, ints, SET_Size (dbd->children));
  for (item = SET_First (dbd->children); item; item = SET_Next (item))
    pack_child (item->data, dsize, d, doubles, isize, i, ints);

  /* pack exported constraints */
  pack_int (isize, i, ints, SET_Size (dbd->constraints));
  for (item = SET_First (dbd->constraints); item; item = SET_Next (item))
    pack_constraint (item->data, dsize, d, doubles, isize, i, ints);
}

/* unpack domain balancing data */
static void* domain_balancing_unpack (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int n, j, k;

  /* unpack spare body ids */
  j = unpack_int (ipos, i, ints);
  for (n = 0; n < j; n ++)
  {
    k = unpack_int (ipos, i, ints);
    SET_Insert (&dom->setmem, &dom->sparebid, (void*) (long) k, NULL); /* creates union across all ranks */
  }

  /* unpack imported bodies */
  j = unpack_int (ipos, i, ints);
  for (n = 0; n < j; n ++)
  {
    unpack_parent (dom, dpos, d, doubles, ipos, i, ints);
  }

  /* unpack imported children */
  j = unpack_int (ipos, i, ints);
  for (n = 0; n < j; n ++)
  {
    unpack_child (dom, dpos, d, doubles, ipos, i, ints);
  }

  /* unpack imporeted constraints */
  j = unpack_int (ipos, i, ints);
  for (n = 0; n < j; n ++)
  {
    unpack_constraint (dom, dpos, d, doubles, ipos, i, ints);
  }

  return NULL;
}

/* compute migration of old boundary constraints (those remaining after constraints update) */
static void old_boundary_constraints_migration (DOM *dom, DBD *dbd)
{
  COMOBJ *send = dom->conextsend;
  SET *item;
  BODY *bod;
  CON *con;
  int i;

  /* compute migration sets */
  for (con = dom->con; con; con = con->next)
  {
    BODY *bodies [] = {con->master, con->slave};

    for (i = 0; i < 2; i ++)
    {
      bod = bodies [i];

      if (bod)
      {
	for (item = SET_First (bod->children); item; item = SET_Next (item))
	{
	  SET_Insert (&dom->setmem, (SET**)&send [(int) (long) item->data].o, con, NULL); /* schedule for sending to children */
	}
      
	if (bod->flags & BODY_CHILD)
	{
	  SET_Insert (&dom->setmem, (SET**)&send [bod->rank].o, con, NULL); /* schedule for sending to parent */
	}
      }
    }
  }

  /* set up 'glue' sets to be used during packing */
  for (i = 0, send = dom->conextsend; i < dom->ncpu; i ++, send ++) dbd [i].glue = send->o;
}

/* pack old boundary constraints */
static void old_boundary_constraints_pack (DBD *dbd, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  SET *item;

  /* pack exported boundary constraints */
  pack_int (isize, i, ints, SET_Size (dbd->glue));
  for (item = SET_First (dbd->glue); item; item = SET_Next (item))
    pack_boundary_constraint (item->data, dsize, d, doubles, isize, i, ints);
}

/* unpack old external constraints */
static void* old_external_constraints_unpack (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int n, j;

  /* unpack imporeted external constraints */
  j = unpack_int (ipos, i, ints);
  for (n = 0; n < j; n ++)
  {
    unpack_external_constraint (dom, dpos, d, doubles, ipos, i, ints);
  }

  return NULL;
}

/* domain balancing */
static void domain_balancing (DOM *dom)
{
  int changes,
      num_gid_entries,
      num_lid_entries,
      num_import,
      *import_procs,
      num_export,
      *export_procs;

  ZOLTAN_ID_PTR import_global_ids,
		import_local_ids,
		export_global_ids,
		export_local_ids;

  COMOBJ *send, *recv;
  unsigned int id;
  char tol [128];
  int nrecv;
  SET *item;
  BODY *bod;
  DBD *dbd;
  CON *con;
  int i;

  /* allocate balancing data storage */
  ERRMEM (dbd = MEM_CALLOC (sizeof (DBD [dom->ncpu])));

  /* only during full balancing bodies and constraints can migrate */
  if (dom->balancing == FULL_BALANCING)
  {
    /* update imbalance tolerance */
    snprintf (tol, 128, "%g", dom->imbalance_tolerance);
    Zoltan_Set_Param (dom->zol, "IMBALANCE_TOL", tol);

    /* update body partitioning */
    ASSERT (Zoltan_LB_Balance (dom->zol, &changes, &num_gid_entries, &num_lid_entries,
	    &num_import, &import_global_ids, &import_local_ids, &import_procs,
	    &num_export, &export_global_ids, &export_local_ids, &export_procs) == ZOLTAN_OK, ERR_ZOLTAN);

    for (i = 0; i < num_export; i ++) /* for each exported body */
    {
      id = export_global_ids [i * num_gid_entries]; /* get id */

      bod = MAP_Find (dom->idb, (void*) (long) id, NULL);

      if (bod)
      {
	bod->rank = export_procs [i]; /* set the new rank */

	SET_Insert (&dom->setmem, &dbd [export_procs [i]].bodies, bod, NULL); /* map this body to its export rank */

	for (item = SET_First (bod->con); item; item = SET_Next (item))
	{
	  con = item->data;

	  if (!con->slave) SET_Insert (&dom->setmem, &dbd [export_procs [i]].constraints, con, NULL); /* single-body constraints migrate with bodies */
	}
      }
      else
      {
	ASSERT_DEBUG_EXT (con = MAP_Find (dom->idc, (void*) (long) (id - dom->bid), NULL), "Invalid constraint id");

	SET_Insert (&dom->setmem, &dbd [export_procs [i]].constraints, con, NULL); /* map this constraint to its export rank */
      }
    }

    /* free Zoltan data */
    Zoltan_LB_Free_Data (&import_global_ids, &import_local_ids, &import_procs,
			 &export_global_ids, &export_local_ids, &export_procs);
  }

  ERRMEM (send = malloc (sizeof (COMOBJ [dom->ncpu])));

  for (i = 0; i < dom->ncpu; i ++)
  {
    dbd [i].rank = send [i].rank = i;
    send [i].o = &dbd [i];
    dbd [i].dom = dom;
  }

  /* compute chidren migration sets */
  children_migration_begin (dom, dbd);

  /* communication */
  dom->bytes = COMOBJSALL (MPI_COMM_WORLD, (OBJ_Pack)domain_balancing_pack, dom, (OBJ_Unpack)domain_balancing_unpack, send, dom->ncpu, &recv, &nrecv);

  /* delete migrated out children */
  children_migration_end (dom);

  /* delete bodies associated with spare ids */
  for (item = SET_First (dom->sparebid); item; item = SET_Next (item))
  {
    if ((bod = MAP_Find (dom->allbodies, item->data, NULL)))
    {
      DOM_Remove_Body (dom, bod);
      BODY_Destroy (bod);
    }
  }

#if DEBUG
  for (con = dom->con; con; con = con->next)
  {
    ASSERT_DEBUG (con->master->flags & (BODY_PARENT|BODY_CHILD), "Regular constraint attached to a dummy");
    if (con->slave) ASSERT_DEBUG (con->slave->flags & (BODY_PARENT|BODY_CHILD), "Regular constraint attached to a dummy");
  }
#endif

  if (dom->balancing == FULL_BALANCING)
  {
    /* clean */
    free (recv);

    /* compute old boundary constraints migration sets */
    old_boundary_constraints_migration (dom, dbd);

    /* after this step all bodies contain sets of all old constraints (including contacts); this way during contact detection all existing contacts will get filtered out */
    dom->bytes += COMOBJSALL (MPI_COMM_WORLD, (OBJ_Pack)old_boundary_constraints_pack, dom, (OBJ_Unpack)old_external_constraints_unpack, send, dom->ncpu, &recv, &nrecv);
  }

  /* free auxiliary sets */
  for (i = 0; i < dom->ncpu; i ++)
  {
    SET_Free (&dom->setmem, &dbd [i].bodies);
    SET_Free (&dom->setmem, &dbd [i].constraints);
    SET_Free (&dom->setmem, &dbd [i].children);
  }

  /* clean */
  free (send);
  free (recv);
  free (dbd);
}

/* compute new boundary contacts migration */
static void new_boundary_contacts_migration (DOM *dom, DBD *dbd)
{
  SET *item;
  BODY *bod;
  CON *con;
  int i;

  /* compute additional boundary sets */
  for (con = dom->con; con; con = con->next)
  {
    if (con->state & CON_NEW) /* only newly created contacts */
    {
      BODY *bodies [] = {con->master, con->slave};

      for (i = 0; i < 2; i ++)
      {
	bod = bodies [i];

	if (bod)
	{
	  for (item = SET_First (bod->children); item; item = SET_Next (item))
	  {
	    SET_Insert (&dom->setmem, (SET**)&dbd [(int) (long) item->data].glue, con, NULL); /* schedule for sending to children */
	  }

	  if (bod->flags & BODY_CHILD)
	  {
	    SET_Insert (&dom->setmem, (SET**)&dbd [bod->rank].glue, con, NULL); /* schedule for sending to parent */
	  }
	}
      }
    }
  }
}

/* pack domain gluing data */
static void domain_gluing_pack (DBD *dbd, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  SET *item;

  /* pack penetration depth flag */
  pack_int (isize, i, ints, dbd->dom->flags & DOM_DEPTH_VIOLATED);

  /* pack statistics */
  pack_stats (dbd->dom, dbd->rank, dsize, d, doubles, isize, i, ints);

  /* pack exported boundary constraints */
  pack_int (isize, i, ints, SET_Size (dbd->glue));
  for (item = SET_First (dbd->glue); item; item = SET_Next (item))
    pack_boundary_constraint (item->data, dsize, d, doubles, isize, i, ints);
}

/* unpack domain balancing data */
static void* domain_gluing_unpack (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int n, j;

  /* unpack penetration depth flag */
  n = unpack_int (ipos, i, ints); ASSERT (!n, ERR_DOM_DEPTH);

  /* unpack statistics */
  unpack_stats (dom, dpos, d, doubles, ipos, i, ints);

  /* unpack imporeted external constraints */
  j = unpack_int (ipos, i, ints);
  for (n = 0; n < j; n ++)
  {
    unpack_external_constraint (dom, dpos, d, doubles, ipos, i, ints);
  }

  return NULL;
}

/* domain gluing */
static void domain_gluing (DOM *dom)
{
  COMOBJ *send, *recv, *ptr;
  DBD *dbd, *qtr;
  SET *item;
  int nrecv;
  int i;

  ERRMEM (dbd = MEM_CALLOC (sizeof (DBD [dom->ncpu])));
  ERRMEM (send = malloc (sizeof (COMOBJ [dom->ncpu])));

  /* compute migration sets */
  new_boundary_contacts_migration (dom, dbd);

  for (i = 0; i < dom->ncpu; i ++)
  {
    dbd [i].rank = send [i].rank = i;
    send [i].o = &dbd [i];
    dbd [i].dom = dom;
  }

  /* communication */
  dom->bytes += COMOBJSALL (MPI_COMM_WORLD, (OBJ_Pack)domain_gluing_pack, dom, (OBJ_Unpack)domain_gluing_unpack, send, dom->ncpu, &recv, &nrecv);

  /* merge new glue sets with the old sets; this way the complete migration sets of all boundary
   * constraints are created; they will be of use for constraint updates during solution process */
  for (i = 0, ptr = dom->conextsend, qtr = dbd; i < dom->ncpu; i ++, ptr ++, qtr ++)
  {
    for (item = SET_First (qtr->glue); item; item = SET_Next (item))
    {
      SET_Insert (&dom->setmem, (SET**) &ptr->o, item->data, NULL);
    }

    SET_Free (&dom->setmem, &qtr->glue);
  }

  /* compute statistics */
  stats_compute (dom);

  /* clean */
  free (send);
  free (recv);
  free (dbd);
}

/* remove all external constraints */
static void clear_external_constraints (DOM *dom)
{
  COMOBJ *send;
  MAP *jtem;
  CON *con;
  int i;

  /* empty external contact send sets */
  for (i = 0, send = dom->conextsend; i < dom->ncpu; i ++, send ++) SET_Free (&dom->setmem, (SET**)&send->o);

  /* delete all external constraints */
  for (jtem = MAP_First (dom->conext); jtem; jtem = MAP_Next (jtem))
  {
    con = jtem->data;

    /* remove from the body constraint adjacency  */
    SET_Delete (&dom->setmem, &con->master->con, con, CONCMP);
    if (con->slave) SET_Delete (&dom->setmem, &con->slave->con, con, CONCMP);

    /* destroy passed data */
    MEM_Free (&dom->conmem, con);
  }

  /* free external contacts map */
  MAP_Free (&dom->mapmem, &dom->conext);
}

/* pack normal reaction components (only for contacts) of boundary constraints */
static void pack_normal_reactions (SET *set, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  SET *item;
  CON *con;

  /* pack exported boundary contacts */
  pack_int (isize, i, ints, SET_Size (set));
  for (item = SET_First (set); item; item = SET_Next (item))
  {
    con = item->data;
    pack_int (isize, i, ints, con->id);
    if (con->kind == CONTACT) pack_double (dsize, d, doubles, con->R [2]);
    else pack_doubles (dsize, d, doubles, con->R, 3);
  }
}

/* unpack normal reaction components (only for contacts) of external constraints */
static void* unpack_normal_reactions (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int n, j, id;
  CON *con;

  /* unpack imporeted external contacts */
  j = unpack_int (ipos, i, ints);
  for (n = 0; n < j; n ++)
  {
    id = unpack_int (ipos, i, ints);
    ASSERT_DEBUG_EXT (con = MAP_Find (dom->conext, (void*) (long) id, NULL), "Invalid contact id");
    if (con->kind == CONTACT) con->R [2] = unpack_double (dpos, d, doubles);
    else unpack_doubles (dpos, d, doubles, con->R, 3);
  }

  return NULL;
}

/* pack boundary reactions */
static void pack_reactions (SET *set, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  SET *item;
  CON *con;

  /* pack exported boundary contacts */
  pack_int (isize, i, ints, SET_Size (set));
  for (item = SET_First (set); item; item = SET_Next (item))
  {
    con = item->data;
    pack_int (isize, i, ints, con->id);
    pack_doubles (dsize, d, doubles, con->R, 3);
  }
}

/* unpack external reactions */
static void* unpack_reactions (DOM *dom, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int n, j, id;
  CON *con;

  /* unpack imporeted external contacts */
  j = unpack_int (ipos, i, ints);
  for (n = 0; n < j; n ++)
  {
    id = unpack_int (ipos, i, ints);
    ASSERT_DEBUG_EXT (con = MAP_Find (dom->conext, (void*) (long) id, NULL), "Invalid contact id");
    unpack_doubles (dpos, d, doubles, con->R, 3);
  }

  return NULL;
}

/* create MPI related data */
static void create_mpi (DOM *dom)
{
  dom->allbodies = NULL;

  dom->children = NULL;

  dom->conext = NULL;

  MPI_Comm_rank (MPI_COMM_WORLD, &dom->rank); /* store rank */

  MPI_Comm_size (MPI_COMM_WORLD, &dom->ncpu); /* store size */

  ERRMEM (dom->conextsend = MEM_CALLOC (sizeof (COMOBJ [dom->ncpu])));

  for (int i = 0; i < dom->ncpu; i ++) dom->conextsend [i].rank = i; /* initialize send ranks */

  dom->cid = (dom->rank + 1); /* overwrite */

  dom->noid = 0; /* assign constraint ids in 'insert' routine (turned off when importing non-contacts) */

  dom->bytes = 0;

  stats_create (dom);

  dom->balancing = FULL_BALANCING;

  dom->deletions = 0;

  dom->counter = 0;

  dom->ratio = 0;

  ASSERT (dom->zol = Zoltan_Create (MPI_COMM_WORLD), ERR_ZOLTAN); /* zoltan context for body partitioning */

  dom->imbalance_tolerance = 1.3;

  /* general parameters */
  Zoltan_Set_Param (dom->zol, "DEBUG_LEVEL", "0");
  Zoltan_Set_Param (dom->zol, "DEBUG_MEMORY", "0");
  Zoltan_Set_Param (dom->zol, "NUM_GID_ENTRIES", "1");
  Zoltan_Set_Param (dom->zol, "NUM_LID_ENTRIES", "0");
  Zoltan_Set_Param (dom->zol, "OBJ_WEIGHT_DIM", "1");
 
  /* load balancing parameters */
  Zoltan_Set_Param (dom->zol, "LB_METHOD", "RCB");
  Zoltan_Set_Param (dom->zol, "IMBALANCE_TOL", "1.3");
  Zoltan_Set_Param (dom->zol, "AUTO_MIGRATE", "FALSE");
  Zoltan_Set_Param (dom->zol, "RETURN_LISTS", "EXPORT");

  /* RCB parameters */
  Zoltan_Set_Param (dom->zol, "RCB_OVERALLOC", "1.3");
  Zoltan_Set_Param (dom->zol, "RCB_REUSE", "1");
  Zoltan_Set_Param (dom->zol, "RCB_OUTPUT_LEVEL", "0");
  Zoltan_Set_Param (dom->zol, "CHECK_GEOM", "1");
  Zoltan_Set_Param (dom->zol, "KEEP_CUTS", "1");

  /* callbacks */
  Zoltan_Set_Fn (dom->zol, ZOLTAN_NUM_OBJ_FN_TYPE, (void (*)()) object_count, dom);
  Zoltan_Set_Fn (dom->zol, ZOLTAN_OBJ_LIST_FN_TYPE, (void (*)()) object_list, dom);
  Zoltan_Set_Fn (dom->zol, ZOLTAN_NUM_GEOM_FN_TYPE, (void (*)()) dimensions, dom);
  Zoltan_Set_Fn (dom->zol, ZOLTAN_GEOM_MULTI_FN_TYPE, (void (*)()) objpoints, dom);
}

/* destroy MPI related data */
static void destroy_mpi (DOM *dom)
{
  MAP *item;

  for (item = MAP_First (dom->allbodies); item; item = MAP_Next (item))
  {
    BODY_Destroy (item->data);
  }

  free (dom->conextsend);

  stats_destroy (dom);

  Zoltan_Destroy (&dom->zol);
}
#endif

/* constraint kind string */
char* CON_Kind (CON *con)
{
  switch (con->kind)
  {
  case CONTACT: return "CONTACT";
  case FIXPNT: return "FIXPNT";
  case FIXDIR: return "FIXDIR";
  case VELODIR: return "VELODIR";
  case RIGLNK: return "RIGLNK";
  }

  return NULL;
}

/* create a domain */
DOM* DOM_Create (AABB *aabb, SPSET *sps, short dynamic, double step)
{
  DOM *dom;

  ERRMEM (dom = MEM_CALLOC (sizeof (DOM)));
  dom->aabb = aabb;
  aabb->dom = dom;
  dom->sps = sps;
  dom->dynamic = (dynamic == 1 ? 1 : 0);
  dom->step = step;
  dom->time = 0.0;

  MEM_Init (&dom->conmem, sizeof (CON), CONBLK);
  MEM_Init (&dom->mapmem, sizeof (MAP), MAPBLK);
  MEM_Init (&dom->setmem, sizeof (SET), SETBLK);
  dom->sparebid = NULL;
  dom->bid = 1;
  dom->lab = NULL;
  dom->idb = NULL;
  dom->bod = NULL;
  dom->nbod = 0;
  dom->delb = NULL;
  dom->newb = NULL;
  dom->sparecid = NULL;
  dom->cid = 1;
  dom->idc= NULL;
  dom->con = NULL;
  dom->ncon = 0;
  dom->prev = dom->next = NULL;
  dom->flags = 0;
  dom->threshold = 0.01;
  dom->depth = -DBL_MAX;
  ERRMEM (dom->ldy = LOCDYN_Create (dom));

  SET (dom->gravdir, 0);
  dom->gravval = NULL;

  SET (dom->extents, -DBL_MAX);
  SET (dom->extents + 3, DBL_MAX);

  dom->data = data_create ();

  dom->verbose = 0;

#if MPI
  create_mpi (dom);
#endif

  return dom;
}

/* insert a body into the domain */
void DOM_Insert_Body (DOM *dom, BODY *bod)
{
  /* if there is a spare id */
  if (SET_Size (dom->sparebid)) 
  {
    SET *item = SET_First (dom->sparebid);
    bod->id = (unsigned int) (long) item->data; /* use it */
    SET_Delete (&dom->setmem, &dom->sparebid, item->data, NULL); /* no more spare */
  }
  else bod->id = dom->bid ++; /* or use a next id */

  /* make sure we do not run out of ids */
  ASSERT (dom->bid < UINT_MAX, ERR_DOM_TOO_MANY_BODIES);

  /* assign domain */
  bod->dom = dom;

#if MPI
  /* insert into the set of all created bodies */
  MAP_Insert (&dom->mapmem, &dom->allbodies, (void*) (long) bod->id, bod, NULL);

  /* insert every 'rank' body into this domain */
  if (bod->id % (unsigned) dom->ncpu == (unsigned) dom->rank)
  {
    /* mark as parent */
    bod->flags |= BODY_PARENT;
#endif
    /* insert into overlap engine */
    AABB_Insert_Body (dom->aabb, bod);

    /* insert into label based map */
    if (bod->label) MAP_Insert (&dom->mapmem, &dom->lab, bod->label, bod, (MAP_Compare)strcmp);

    /* insert into id based map */
    MAP_Insert (&dom->mapmem, &dom->idb, (void*) (long) bod->id, bod, NULL);

    /* insert into list */
    bod->next = dom->bod;
    if (dom->bod) dom->bod->prev = bod;
    dom->bod = bod;

    /* increment */
    dom->nbod ++;

    /* schedule insertion mark in the output */
    if (dom->time > 0) SET_Insert (&dom->setmem, &dom->newb, bod, NULL);
#if MPI
  }
#endif
}

/* remove a body from the domain */
void DOM_Remove_Body (DOM *dom, BODY *bod)
{
  /* remove from overlap engine */
  AABB_Delete_Body (dom->aabb, bod);

  SET *con = bod->con;
  bod->con = NULL; /* DOM_Remove_Constraint will try to remove the constraint
		      from body constraints set, which is not nice if we try
		      to iterate over the set at the same time => make it empty */
 
  /* remove all body related constraints */
  for (SET *item = SET_First (con); item; item = SET_Next (item)) DOM_Remove_Constraint (dom, item->data);

  /* free constraint set */
  SET_Free (&dom->setmem, &con);

  /* delete from label based map */
  if (bod->label) MAP_Delete (&dom->mapmem, &dom->lab, bod->label, (MAP_Compare)strcmp);

  /* delete from id based map */
  MAP_Delete (&dom->mapmem, &dom->idb, (void*) (long) bod->id, NULL);

  /* remove from list */
  if (bod->prev) bod->prev->next = bod->next;
  else dom->bod = bod->next;
  if (bod->next) bod->next->prev = bod->prev;

  /* decrement */
  dom->nbod --;

  /* schedule deletion mark in the output */
  if (dom->time > 0) SET_Insert (&dom->setmem, &dom->delb, (void*) (long) bod->id, NULL);

#if MPI
  /* free children set */
  SET_Free (&dom->setmem, &bod->children);

  /* delete from the set of all created bodies */
  MAP_Delete (&dom->mapmem, &dom->allbodies, (void*) (long) bod->id, NULL);

  /* free body id => spare ids from ranks > 0 will be sent to rank 0 during balancing */
  SET_Insert (&dom->setmem, &dom->sparebid, (void*) (long) bod->id, NULL);
#endif
}

/* find labeled body */
BODY* DOM_Find_Body (DOM *dom, char *label)
{
  return MAP_Find (dom->lab, label, (MAP_Compare)strcmp);
}

/* fix a referential point of the body along all directions */
CON* DOM_Fix_Point (DOM *dom, BODY *bod, double *pnt)
{
  CON *con;
  SGP *sgp;
  int n;

  if ((n = SHAPE_Sgp (bod->sgp, bod->nsgp, pnt)) < 0) return NULL;

  sgp = &bod->sgp [n];
  con = insert (dom, bod, NULL, sgp, NULL);
  con->kind = FIXPNT;
  COPY (pnt, con->point);
  COPY (pnt, con->mpnt);
  IDENTITY (con->base);

  /* insert into local dynamics */
  con->dia = LOCDYN_Insert (dom->ldy, con, bod, NULL);

  return con;
}

/* fix a referential point of the body along the spatial direction */
CON* DOM_Fix_Direction (DOM *dom, BODY *bod, double *pnt, double *dir)
{
  CON *con;
  SGP *sgp;
  int n;

  if ((n = SHAPE_Sgp (bod->sgp, bod->nsgp, pnt)) < 0) return NULL;

  sgp = &bod->sgp [n];
  con = insert (dom, bod, NULL, sgp, NULL);
  con->kind = FIXDIR;
  COPY (pnt, con->point);
  COPY (pnt, con->mpnt);
  localbase (dir, con->base);

  /* insert into local dynamics */
  con->dia = LOCDYN_Insert (dom->ldy, con, bod, NULL);

  return con;
}

/* prescribe a velocity of the referential point along the spatial direction */
CON* DOM_Set_Velocity (DOM *dom, BODY *bod, double *pnt, double *dir, TMS *vel)
{
  CON *con;
  SGP *sgp;
  int n;

  if ((n = SHAPE_Sgp (bod->sgp, bod->nsgp, pnt)) < 0) return NULL;

  sgp = &bod->sgp [n];
  con = insert (dom, bod, NULL, sgp, NULL);
  con->kind = VELODIR;
  COPY (pnt, con->point);
  COPY (pnt, con->mpnt);
  localbase (dir, con->base);
  con->tms = vel;

  /* insert into local dynamics */
  con->dia = LOCDYN_Insert (dom->ldy, con, bod, NULL);

  return con;
}

/* insert rigid link constraint between two (referential) points of bodies; if one of the body
 * pointers is NULL then the link acts between the other body and the fixed (spatial) point */
CON* DOM_Put_Rigid_Link (DOM *dom, BODY *master, BODY *slave, double *mpnt, double *spnt)
{
  double v [3], d;
  CON *con;
  SGP *msgp, *ssgp;
  int m, s;

  if (!master)
  {
    master = slave;
    mpnt = spnt;
    slave = NULL;
  }

  ASSERT_DEBUG (master, "At least one body pointer must not be NULL");

  if (master && (m = SHAPE_Sgp (master->sgp, master->nsgp, mpnt)) < 0) return NULL;

  if (slave && (s = SHAPE_Sgp (slave->sgp, slave->nsgp, spnt)) < 0) return NULL;

  msgp = &master->sgp [m];
  if (slave) ssgp = &slave->sgp [s];
  else ssgp = NULL;

  SUB (mpnt, spnt, v);
  d = LEN (v);
  
  if (d < GEOMETRIC_EPSILON) /* no point in keeping very short links */
  {
    con = insert (dom, master, slave, msgp, ssgp);
    con->kind = FIXPNT;
    COPY (mpnt, con->point);
    COPY (mpnt, con->mpnt);
    COPY (spnt, con->spnt);
    IDENTITY (con->base);

    if (slave) AABB_Exclude_Gobj_Pair (dom->aabb, master->id, m, slave->id, s); /* no contact between this pair */
  }
  else
  {
    con = insert (dom, master, slave, msgp, ssgp);
    con->kind = RIGLNK;
    COPY (mpnt, con->point);
    COPY (mpnt, con->mpnt);
    COPY (spnt, con->spnt);
    RIGLNK_LEN (con->Z) = d; /* initial distance */
    update_riglnk (dom, con); /* initial update */
  }
  
  /* insert into local dynamics */
  con->dia = LOCDYN_Insert (dom->ldy, con, master, slave);

  return con;
}

/* remove a constraint from the domain */
void DOM_Remove_Constraint (DOM *dom, CON *con)
{
#if MPI
  if (con->kind == CONTACT && (con->state & CON_EXTERNAL))
  {
    /* remove from the body constraint adjacency  */
    SET_Delete (&dom->setmem, &con->master->con, con, CONCMP);
    if (con->slave) SET_Delete (&dom->setmem, &con->slave->con, con, CONCMP);

    /* remove from map */
    MAP_Delete (&dom->mapmem, &dom->conext, (void*) (long) con->id, NULL);

    /* destroy passed data */
    MEM_Free (&dom->conmem, con);
  }
  else
  {
#endif
  /* remove from the body constraint adjacency  */
  SET_Delete (&dom->setmem, &con->master->con, con, CONCMP);
  if (con->slave) SET_Delete (&dom->setmem, &con->slave->con, con, CONCMP);

  /* remove from id-based map */
  MAP_Delete (&dom->mapmem, &dom->idc, (void*) (long) con->id, NULL);

  /* remove from list */
  if (con->prev)
    con->prev->next = con->next;
  else dom->con = con->next;
  if (con->next)
    con->next->prev = con->prev;
  dom->ncon --;

  /* remove from local dynamics */
  if (con->dia) LOCDYN_Remove (dom->ldy, con->dia);

#if MPI
  /* free constraint id if possible */
  if (!(con->state & CON_IDLOCK))
#endif
  SET_Insert (&dom->setmem, &dom->sparecid, (void*) (long) con->id, NULL);

  if (con->kind == CONTACT) SURFACE_MATERIAL_Destroy_State (&con->mat); /* free contact material state */
  /* free velocity constraint time history */
  else if (con->kind == VELODIR) TMS_Destroy (con->tms);

  /* destroy passed data */
  MEM_Free (&dom->conmem, con);
#if MPI
  }
#endif
}

/* set simulation scene extents */
void DOM_Extents (DOM *dom, double *extents)
{
  COPY6 (extents, dom->extents);
}

/* go over contact points and remove those whose corresponding
 * areas are much smaller than those of other points related to
 * objects directly topologically adjacent in their shape definitions */
void DOM_Sparsify_Contacts (DOM *dom)
{
  double threshold = dom->threshold;
  SET *del, *itm;
  CON *con, *adj;
  MEM mem;
  int n;

  MEM_Init (&mem, sizeof (SET), SETBLK);

  for (del = NULL, con = dom->con; con; con = con->next)
  {
    if (con->kind == CONTACT && con->state & CON_NEW) /* walk over all new contacts */
    {
      SET *set [2] = {con->master->con, con->slave->con};

      for (n = 0; n < 2; n ++) for (itm = SET_First (set [n]); itm; itm = SET_Next (itm))
      {
	adj = itm->data;

	if (adj == con || adj->kind != CONTACT) continue;
	
	if (con->area < threshold * adj->area) /* check whether the area of the diagonal element is too small (this test is cheaper => let it go first) */
	{
	  if (con->master == adj->master && con->slave == adj->slave) /* identify contacts pair sharing the same pairs of bodies */
	  {
	    if (gobj_adjacent (GOBJ_Pair_Code_Ext (mkind(con), mkind(adj)), mgobj(con), mgobj (adj))) /* check whether the geometric objects are topologically adjacent */
	       SET_Insert (&mem, &del, con, NULL); /* if so schedule the current contact for deletion */
	  }
	  else if (con->master == adj->slave && con->slave == adj->master)
	  {
	    if (gobj_adjacent (GOBJ_Pair_Code_Ext (mkind(con), skind(adj)), mgobj(con), sgobj(adj)))
	      SET_Insert (&mem, &del, con, NULL);
	  }
	  else if (con->slave == adj->master && con->master == adj->slave)
	  {
	    if (gobj_adjacent (GOBJ_Pair_Code_Ext (skind(con), mkind(adj)), sgobj(con), mgobj(adj)))
	      SET_Insert (&mem, &del, con, NULL);
	  }
	  else if (con->slave == adj->slave && con->master == adj->master)
	  {
	    if (gobj_adjacent (GOBJ_Pair_Code_Ext (skind(con), skind(adj)), sgobj(con), sgobj(adj)))
	      SET_Insert (&mem, &del, con, NULL);
	  }
	}
      }
    }
  }

  /* now remove unwanted contacts */
  for (itm = SET_First (del), n = 0; itm; itm = SET_Next (itm), n ++)
  {
    con = itm->data;
    /* remove first from the box adjacency structure => otherwise box engine would try
     * to release this contact at a later point and that would cose memory corruption */
    DOM_Remove_Constraint (dom, con); /* now remove from the domain */
  }

  dom->nspa = n; /* record the number of sparsified contacts */

  /* clean up */
  MEM_Release (&mem);
}

/* domain update initial half-step => bodies and constraints are
 * updated and the current local dynamic problem is returned */
LOCDYN* DOM_Update_Begin (DOM *dom)
{
  double time, step;
  CON *con, *next;
  TIMING timing;
  BODY *bod;

#if MPI
  if (dom->rank == 0)
#endif
  if (dom->verbose) printf ("DOMAIN ... "), fflush (stdout);

  SOLFEC_Timer_Start (dom->solfec, "TIMINT");

  /* time and step */
  time = dom->time;
  step = dom->step;

  /* initialize bodies */
  if (time == 0.0)
  {
    if (dom->dynamic > 0)
    {
      for (bod = dom->bod; bod; bod = bod->next)
      {
	BODY_Dynamic_Init (bod); /* integration scheme is set externally */

	double h = BODY_Dynamic_Critical_Step (bod);

	if (h < step) step = 0.9 * h;
      }
    }
    else
    {
      for (bod = dom->bod; bod; bod = bod->next) BODY_Static_Init (bod);
    }

#if MPI
    dom->step = step = PUT_double_min (step);

    if (dom->rank == 0)
#else
    dom->step = step;
#endif
    printf (" (TIME STEP: %g) ", step), fflush (stdout);
  }

  /* begin time integration */
  if (dom->dynamic)
    for (bod = dom->bod; bod; bod = bod->next)
      BODY_Dynamic_Step_Begin (bod, time, step);
  else
    for (bod = dom->bod; bod; bod = bod->next)
      BODY_Static_Step_Begin (bod, time, step);

  /* update old constraints */
  for (con = dom->con; con; con = next)
  {
    next = con->next; /* contact update can delete the current iterate */

    switch (con->kind)
    {
      case CONTACT: update_contact (dom, con); break;
      case FIXPNT:  update_fixpnt  (dom, con); break;
      case FIXDIR:  update_fixdir  (dom, con); break;
      case VELODIR: update_velodir (dom, con); break;
      case RIGLNK:  update_riglnk  (dom, con); break;
    }
  }

  SOLFEC_Timer_End (dom->solfec, "TIMINT");

#if MPI
  SOLFEC_Timer_Start (dom->solfec, "PARBAL");

  domain_balancing (dom);

  SOLFEC_Timer_End (dom->solfec, "PARBAL");
#endif

  /* detect contacts */
  timerstart (&timing);

  AABB_Update (dom->aabb, aabb_algorithm (dom), dom, (BOX_Overlap_Create) overlap_create);

  aabb_timing (dom, timerend (&timing));

#if MPI
  SOLFEC_Timer_Start (dom->solfec, "PARBAL");

  domain_gluing (dom);

  SOLFEC_Timer_End (dom->solfec, "PARBAL");
#else
  ASSERT (!(dom->flags & DOM_DEPTH_VIOLATED), ERR_DOM_DEPTH);
#endif

  SOLFEC_Timer_Start (dom->solfec, "TIMINT");

  /* update new contacts */
  for (con = dom->con; con; con = con->next)
  {
    if (con->kind == CONTACT && (con->state & CON_NEW))
    {
      /* insert into local dynamics */
      con->dia = LOCDYN_Insert (dom->ldy, con, con->master, con->slave);

      /* invalidate newness */
      con->state &= ~CON_NEW;
    }
  }

  SOLFEC_Timer_End (dom->solfec, "TIMINT");

  /* output local dynamics */
  return dom->ldy;
}

/* domain update final half-step => once the local dynamic
 * problem has been solved (externally), motion of bodies
 * is updated with the help of new constraint reactions */
void DOM_Update_End (DOM *dom)
{
  double time, step, *de, *be;
  SET *del, *item;
  BODY *bod;

  SOLFEC_Timer_Start (dom->solfec, "TIMINT");

  /* time and step */
  time = dom->time;
  step = dom->step;

  /* end time integration */
  if (dom->dynamic)
    for (bod = dom->bod; bod; bod = bod->next)
      BODY_Dynamic_Step_End (bod, time, step);
  else
    for (bod = dom->bod; bod; bod = bod->next)
      BODY_Static_Step_End (bod, time, step);

  /* advance time */
  dom->time += step;
  dom->step = step;

  /* erase bodies outside of scene extents */
  for (bod = dom->bod, de = dom->extents, del = NULL; bod; bod = bod->next)
  {
    be = bod->extents;

    if (be [3] < de [0] ||
	be [4] < de [1] ||
	be [5] < de [2] ||
	be [0] > de [3] ||
	be [1] > de [4] ||
	be [2] > de [5]) SET_Insert (&dom->setmem, &del, bod, NULL); /* insert into deletion set */
  }

  for (item = SET_First (del); item; item = SET_Next (item)) /* remove bodies falling out of the scene extents */
  {
    DOM_Remove_Body (dom, item->data);
    BODY_Destroy (item->data);
  }

  SET_Free (&dom->setmem, &del); /* free up deletion set */

#if MPI
  if (dom->counter ++ > 100 || dom->ratio > 0.01)
  {
    dom->counter = 0;
    dom->deletions  = 0;
    dom->balancing = FULL_BALANCING;
    clear_external_constraints (dom); /* remove external constraints */
  }
  else dom->balancing = PARTIAL_BALANCING;
#endif

  SOLFEC_Timer_End (dom->solfec, "TIMINT");
}

#if MPI
/* send boundary reactions to their external receivers;
 * if 'normal' is > 0 only normal components are sent */
void DOM_Update_External_Reactions (DOM *dom, short normal)
{
  COMOBJ *recv;
  int nrecv;

  if (normal > 0)
  {
    dom->bytes += COMOBJSALL (MPI_COMM_WORLD, (OBJ_Pack)pack_normal_reactions, dom,
      (OBJ_Unpack)unpack_normal_reactions, dom->conextsend, dom->ncpu, &recv, &nrecv);
  }
  else
  {
    dom->bytes += COMOBJSALL (MPI_COMM_WORLD, (OBJ_Pack)pack_reactions, dom,
      (OBJ_Unpack)unpack_reactions, dom->conextsend, dom->ncpu, &recv, &nrecv);
  }

  free (recv);
}
#endif

/* write domain state */
void DOM_Write_State (DOM *dom, PBF *bf, CMP_ALG alg)
{
  int cmp = alg;

  PBF_Label (bf, "DOMCMP"); /* label domain compression (0 rank file in parallel) */
  PBF_Int (bf, &cmp, 1); /* 0 rank file as well */

  if (cmp == CMP_OFF) dom_write_state (dom, bf);
  else dom_write_state_compressed (dom, bf, alg);
}

/* read domain state */
void DOM_Read_State (DOM *dom, PBF *bf)
{
  int cmp;

  if (PBF_Label (bf, "DOMCMP")) /* perhaps some other that was outputed more frequently (DOM needs not be in every frame) */
  {
    PBF_Int (bf, &cmp, 1);

    if (cmp == CMP_OFF) dom_read_state (dom, bf);
    else dom_read_state_compressed (dom, bf);
  }
}

/* read state of an individual body */
int DOM_Read_Body (DOM *dom, PBF *bf, BODY *bod)
{
  int cmp;

  if (PBF_Label (bf, "DOMCMP"))
  {
    PBF_Int (bf, &cmp, 1);

    if (cmp == CMP_OFF) return dom_read_body (dom, bf, bod);
    else return dom_read_body_compressed (dom, bf, bod);
  }

  return 0;
}

/* read state of an individual constraint */
int DOM_Read_Constraint (DOM *dom, PBF *bf, CON *con)
{
  int cmp;

  if (PBF_Label (bf, "DOMCMP"))
  {
    PBF_Int (bf, &cmp, 1);

    if (cmp == CMP_OFF) return dom_read_constraint (dom, bf, con);
    else return dom_read_constraint_compressed (dom, bf, con);
  }

  return 0;
}

/* release memory */
void DOM_Destroy (DOM *dom)
{
  CON *con;
 
#if MPI
  destroy_mpi (dom);
#else
  BODY *bod, *next;

  for (bod = dom->bod; bod; bod = next)
  {
    next = bod->next;
    BODY_Destroy (bod);
  }
#endif

  for (con = dom->con; con; con = con->next)
  {
    if (con->kind == CONTACT) SURFACE_MATERIAL_Destroy_State (&con->mat);
    else if (con->kind == VELODIR && con->tms) TMS_Destroy (con->tms);
  }

  LOCDYN_Destroy (dom->ldy);

  MEM_Release (&dom->conmem);
  MEM_Release (&dom->setmem);
  MEM_Release (&dom->mapmem);

  if (dom->gravval) TMS_Destroy (dom->gravval);

  data_destroy (dom->data);

  free (dom);
}
