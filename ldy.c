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
#include "err.h"

#if MPI
#include <limits.h>
#include "put.h"
#include "pck.h"
#include "com.h"
#include "tag.h"
#include "lis.h"
#endif

/* memory block size */
#define BLKSIZE 512

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
      double c = con->mat.cohesion * con->area,
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
	double c = con->mat.cohesion * con->area,
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
      double c = con->mat.cohesion * con->area,
	     *R = dia->R;

      R [2] -= c; /* back change */

      if ((state & CON_OPEN) || /* mode-I decohesion */
	(!(state & CON_STICK))) /* mode-II decohesion */
      {
	con->state &= ~CON_COHESIVE;
	con->mat.cohesion = 0.0;
      }
    }
  }
}

#if MPI
/* return next pointer and realloc send memory if needed */
inline static COMDATA* sendnext (int nsend, int *size, COMDATA **send)
{
  if (nsend >= *size)
  {
    (*size) *= 2;
    ERRMEM (*send = realloc (*send, sizeof (COMDATA [*size])));
  }

  return &(*send)[nsend];
}

/* sort off-diagonal blocks by ids */
#define LEOFFB(a, b) ((a)->id <= (b)->id)
IMPLEMENT_LIST_SORT (SINGLE_LINKED, sort_offb, OFFB, p, n, LEOFFB)

/* number of vertices in the local W graph */
static int vertex_count (LOCDYN *ldy, int *ierr)
{
  if (ierr) *ierr = ZOLTAN_OK;
  return ldy->ndiab + ldy->nins; /* balanced + newly inserted */
}

/* list of vertices in the local W graph */
static void vertex_list (LOCDYN *ldy, int num_gid_entries, int num_lid_entries,
  ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR local_ids, int wgt_dim, float *obj_wgts, int *ierr)
{
  DIAB *dia;
  int i, j;

  for (dia = ldy->diab, i = 0; dia; i ++, dia = dia->n) /* balanced vertices */
  {
    global_ids [i * num_gid_entries] = dia->id;
    local_ids [i * num_lid_entries] = UINT_MAX; /* mapped */
  }

  for (j = 0; j < ldy->nins; i ++, j ++) /* newly inserted vertices */
  {
    global_ids [i * num_gid_entries] = ldy->ins [j]->id;
    local_ids [i * num_lid_entries] = j; /* indexed */
  }

  *ierr = ZOLTAN_OK;
}

/* number of spatial dimensions */
static int dimensions (LOCDYN *ldy, int *ierr)
{
  *ierr = ZOLTAN_OK;
  return 3;
}

/* list of constraint points */
static void conpoints (LOCDYN *ldy, int num_gid_entries, int num_lid_entries, int num_obj,
  ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR local_ids, int num_dim, double *geom_vec, int *ierr)
{
  DIAB *dia;
  int i;

  for (i = 0; i < num_obj; i ++, geom_vec += 3)
  {
    ZOLTAN_ID_TYPE m = local_ids [i];

    if (m == UINT_MAX) /* mapped migrated block */
    {
      ASSERT_DEBUG_EXT (dia = MAP_Find (ldy->idbb, (void*) (long) global_ids [i], NULL), "Invalid block id");
    }
    else /* use local index */
    {
      ASSERT_DEBUG (m < (unsigned)ldy->nins, "Invalid local index");
      dia = ldy->ins [m];
    }

    COPY (dia->point, geom_vec);
  }

  *ierr = ZOLTAN_OK;
}

/* sizes of edges (compressed rows) of the local W graph */
static void edge_sizes (LOCDYN *ldy, int *num_lists, int *num_pins, int *format, int *ierr)
{
  DIAB *dia;
  OFFB *b;
  int j, n = 0;

  for (dia = ldy->diab; dia; dia = dia->n, n ++)
    for (b = dia->adj; b; b = b->n) n ++;

  for (j = 0; j < ldy->nins; j ++, n ++)
  {
    dia = ldy->ins [j];
    for (b = dia->adj; b; b = b->n) n ++;
    for (b = dia->adjext; b; b = b->n) n ++;
  }

  *num_lists = ldy->ndiab + ldy->nins;
  *num_pins =  n;
  *format = ZOLTAN_COMPRESSED_EDGE;
  *ierr = ZOLTAN_OK;
}

/* graph edges (compressed rows) of W */
static void edge_list (LOCDYN *ldy, int num_gid_entries, int num_vtx_edge, int num_pins,
  int format, ZOLTAN_ID_PTR vtxedge_GID, int *vtxedge_ptr, ZOLTAN_ID_PTR pin_GID, int *ierr)
{
  ZOLTAN_ID_PTR GID = pin_GID;
  DIAB *dia;
  OFFB *b;
  int j, n = 0;

  for (dia = ldy->diab; dia; dia = dia->n, vtxedge_GID ++, vtxedge_ptr ++)
  {
    *vtxedge_GID = dia->id;
    *vtxedge_ptr = n;
    *GID = dia->id; n ++, GID ++;

    for (b = dia->adj; b; b = b->n, n ++, GID ++) *GID = b->id;
  }

  for (j = 0; j < ldy->nins; j ++, vtxedge_GID ++, vtxedge_ptr ++)
  {
    dia = ldy->ins [j];
    *vtxedge_GID = dia->id;
    *vtxedge_ptr = n;
    *GID = dia->id; n ++, GID ++;

    for (b = dia->adj; b; b = b->n, n ++, GID ++) *GID = b->id;
    for (b = dia->adjext; b; b = b->n, n ++, GID ++) *GID = b->id;
  }

  *ierr = ZOLTAN_OK;
}

/* pack off-diagonal block ids */
static void pack_block_offids (DIAB *dia, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  OFFB *b;
  int n;

  for (n = 0, b = dia->adj; b; b = b->n) n ++; /* number of internal blocks */
  for (b = dia->adjext; b; b = b->n) n ++; /* number of external blocks */

  pack_int (isize, i, ints, n); /* total number of off-diagonal blocks */

  for (b = dia->adj; b; b = b->n) /* pack internal blocks */
    pack_int (isize, i, ints, b->id);

  for (b = dia->adjext; b; b = b->n) /* pack external blocks */
    pack_int (isize, i, ints, b->id);
}

/* unpack off-diagonal block ids */
static void unpack_block_offids (DIAB *dia, MEM *offmem, MAP *idbb, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  OFFB *b, *n, *x;
  int m;

  /* free current off-diagonal blocks */
  for (b = dia->adj; b; b = n)
  {
    n = b->n;
    MEM_Free (offmem, b);
  }
  dia->adj = NULL; /* clear list */

  m = unpack_int (ipos, i, ints); /* number of off-diagonal blocks */
  for (; m > 0; m --) /* unpack off-diagonal block ids */
  {
    ERRMEM (b = MEM_Alloc (offmem));

    b->id = unpack_int (ipos, i, ints);
    b->n = dia->adj;
    dia->adj = b;
  }

  /* remove duplicates (happen when more than two constraints exist between two bodies) */
  dia->adj = sort_offb (dia->adj);
  for (b = dia->adj; b; b = b->n)
  {
    n = b->n;
    while (n && b->id == n->id)
    {
      x = n;
      n = n->n;
      MEM_Free (offmem, x);
    }
    b->n = n;
  }
}

/* pack diagonal block */
static void pack_block (DIAB *dia, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  OFFB *b;
  int n;

  pack_doubles (dsize, d, doubles, dia->R, 3);
  pack_doubles (dsize, d, doubles, dia->U, 3);
  pack_doubles (dsize, d, doubles, dia->V, 3);
  pack_doubles (dsize, d, doubles, dia->B, 3);
  pack_doubles (dsize, d, doubles, dia->W, 9);
  pack_double  (dsize, d, doubles, dia->rho);

  pack_doubles (dsize, d, doubles, dia->Z, DOM_Z_SIZE);
  pack_doubles (dsize, d, doubles, dia->point, 3);
  pack_doubles (dsize, d, doubles, dia->base, 9);
  pack_doubles (dsize, d, doubles, dia->mpnt, 3);
  pack_double  (dsize, d, doubles, dia->gap);
  pack_int     (isize, i, ints, dia->kind);
  SURFACE_MATERIAL_Pack_Data (&dia->mat, dsize, d, doubles, isize, i, ints);

  for (n = 0, b = dia->adj; b; b = b->n) n ++; /* number of internal blocks */
  for (b = dia->adjext; b; b = b->n) n ++; /* number of external blocks */

  pack_int (isize, i, ints, n); /* total number of off-diagonal blocks */

  for (b = dia->adj; b; b = b->n) /* pack internal blocks */
  {
    pack_doubles (dsize, d, doubles, b->W, 9);
    pack_int (isize, i, ints, b->id);
  }

  for (b = dia->adjext; b; b = b->n) /* pack external  blocks */
  {
    pack_doubles (dsize, d, doubles, b->W, 9);
    pack_int (isize, i, ints, b->id);
  }
}

/* unpack diagonal block */
static void unpack_block (DIAB *dia, MEM *offmem, MAP *idbb, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  OFFB *b, *n, *x;
  int m;

  unpack_doubles (dpos, d, doubles, dia->R, 3);
  unpack_doubles (dpos, d, doubles, dia->U, 3);
  unpack_doubles (dpos, d, doubles, dia->V, 3);
  unpack_doubles (dpos, d, doubles, dia->B, 3);
  unpack_doubles (dpos, d, doubles, dia->W, 9);
  dia->rho = unpack_double  (dpos, d, doubles);

  unpack_doubles (dpos, d, doubles, dia->Z, DOM_Z_SIZE);
  unpack_doubles (dpos, d, doubles, dia->point, 3);
  unpack_doubles (dpos, d, doubles, dia->base, 9);
  unpack_doubles (dpos, d, doubles, dia->mpnt, 3);
  dia->gap = unpack_double  (dpos, d, doubles);
  dia->kind = unpack_int (ipos, i, ints);
  SURFACE_MATERIAL_Unpack_Data (&dia->mat, dpos, d, doubles, ipos, i, ints);

  /* free current off-diagonal blocks */
  for (b = dia->adj; b; b = n)
  {
    n = b->n;
    MEM_Free (offmem, b);
  }
  dia->adj = NULL; /* clear list */

  m = unpack_int (ipos, i, ints); /* number of off-diagonal blocks */
  for (; m > 0; m --) /* unpack off-diagonal blocks */
  {
    ERRMEM (b = MEM_Alloc (offmem));

    unpack_doubles (dpos, d, doubles, b->W, 9);
    b->id = unpack_int (ipos, i, ints);
    b->dia = MAP_Find (idbb, (void*) (long) b->id, NULL); /* might be NULL for boundary nodes of the local graph portion */
    b->n = dia->adj;
    dia->adj = b;
  }

  /* sum up and remove duplicates (happen due to multiple contacts between same two bodies)*/
  dia->adj = sort_offb (dia->adj);
  for (b = dia->adj, m = 0; b; b = b->n, m ++)
  {
    n = b->n;
    while (n && b->id == n->id)
    {
      x = n;
      n = n->n;
      NNADD (b->W, x->W, b->W); /* sum up W blocks */
      MEM_Free (offmem, x);
    }
    b->n = n;
  }

  dia->degree = m + 1; /* total number of blocks in this row */
}

/* delete balanced block */
static void delete_balanced_block (LOCDYN *ldy, DIAB *dia)
{
  OFFB *b, *n;

  MAP_Delete (&ldy->mapmem, &ldy->idbb, (void*) (long) dia->id, NULL);

  MAP_Free (&ldy->mapmem, &dia->children);
  SET_Free (&ldy->setmem, &dia->rext);

  if (dia->p) dia->p->n = dia->n;
  else ldy->diab = dia->n;
  if (dia->n) dia->n->p = dia->p;

  for (b = dia->adj; b; b = n) /* delete off-diagonal blocks */
  {
    n = b->n;
    MEM_Free (&ldy->offmem, b);
  }

  MEM_Free (&ldy->diamem, dia);

  ldy->ndiab --;
}

/* pack deletion data */
static void pack_delete (SET *del, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  /* ids of blocks to be deleted */
  pack_int (isize, i, ints, SET_Size (del));
  for (SET *item = SET_First (del); item; item = SET_Next (item))
    pack_int (isize, i, ints, (int) (long) item->data);
}

/* unpack deletion data */
static void* unpack_delete  (LOCDYN *ldy, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int ndel;

  /* delete blocks */
  ndel = unpack_int (ipos, i, ints);
  for (; ndel > 0; ndel --)
  {
    DIAB *dia;
    int id;

    id = unpack_int (ipos, i, ints);
    dia = MAP_Find (ldy->idbb, (void*) (long) id, NULL);
    if (dia) delete_balanced_block (ldy, dia); /* could be NULL if deletion followed insertion before balancing */
  }

  return NULL;
}

/* pack migration data */
static void pack_migrate (DIAB *dia, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  pack_int (isize, i, ints, dia->id);
  pack_int (isize, i, ints, dia->rank);
}

/* unpack migration data */
static void* unpack_migrate (LOCDYN *ldy, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  DIAB *dia;

  ERRMEM (dia = MEM_Alloc (&ldy->diamem));
  dia->id = unpack_int (ipos, i, ints);
  dia->rank = unpack_int (ipos, i, ints);
  dia->R = dia->REAC;

  dia->n = ldy->diab;
  if (ldy->diab) ldy->diab->p = dia;
  ldy->diab = dia;

  MAP_Insert (&ldy->mapmem, &ldy->idbb, (void*) (long) dia->id, dia, NULL);

  ldy->ndiab ++;

  return dia;
}

/* auxiliary SET and LDB pair type */
typedef struct {SET *set; LDB ldb;} SET_LDB_PAIR;

/* pack off-diagonal ids data */
static void pack_offids (SET_LDB_PAIR *pair, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  /* pack updated blocks */
  pack_int (isize, i, ints, SET_Size (pair->set));
  for (SET *item = SET_First (pair->set); item; item = SET_Next (item))
  {
    DIAB *dia = item->data;

    pack_int (isize, i, ints, dia->id);
    pack_block_offids (dia, dsize, d, doubles, isize, i, ints);
    if (pair->ldb == LDB_GEOM) pack_doubles (dsize, d, doubles, CON(dia->con)->point, 3); /* the updated coordinate will be needed during balancing */
  }
}

/* unpack off-diagonal ids data */
static void* unpack_offids (LOCDYN *ldy, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int nupd;

  nupd = unpack_int (ipos, i, ints);
  for (; nupd > 0; nupd --)
  {
    DIAB *dia;
    int id;

    id = unpack_int (ipos, i, ints);
    ASSERT_DEBUG_EXT (dia = MAP_Find (ldy->idbb, (void*) (long) id, NULL), "Invalid block id");
    unpack_block_offids (dia, &ldy->offmem, ldy->idbb, dpos, d, doubles, ipos, i, ints);
    if (ldy->ldb == LDB_GEOM) unpack_doubles (dpos, d, doubles, dia->point, 3); /* update the point for geometric balancing */
  }

  return NULL;
}

/* pack update data */
static void pack_update (SET *upd, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  /* pack updated blocks */
  pack_int (isize, i, ints, SET_Size (upd));
  for (SET *item = SET_First (upd); item; item = SET_Next (item))
  {
    DIAB *dia = item->data;

    pack_int (isize, i, ints, dia->id);
    pack_block (dia, dsize, d, doubles, isize, i, ints);
  }
}

/* unpack update data */
static void* unpack_update (LOCDYN *ldy, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  int nupd;

  nupd = unpack_int (ipos, i, ints);
  for (; nupd > 0; nupd --)
  {
    DIAB *dia;
    int id;

    id = unpack_int (ipos, i, ints);
    ASSERT_DEBUG_EXT (dia = MAP_Find (ldy->idbb, (void*) (long) id, NULL), "Invalid block id");
    unpack_block (dia, &ldy->offmem, ldy->idbb, dpos, d, doubles, ipos, i, ints);
  }

  return NULL;
}

/* copy constraint data into the block */
static void copycon (DIAB *dia)
{
  CON *con = dia->con;

  for (int i = 0; i < DOM_Z_SIZE; i ++)
    dia->Z [i] = con->Z [i];
  COPY (con->point, dia->point);
  NNCOPY (con->base, dia->base);
  COPY (con->mpnt, dia->mpnt);
  dia->gap = con->gap;
  dia->kind = con->kind;
  dia->mat = con->mat;
}

/* reset balancing approach */
static void ldb_reset (LOCDYN *ldy)
{
  /* destroy previous context if any */
  if (ldy->zol) Zoltan_Destroy (&ldy->zol);

  /* create Zoltan context */
  ASSERT (ldy->zol = Zoltan_Create (MPI_COMM_WORLD), ERR_ZOLTAN);

  /* general parameters */
  Zoltan_Set_Param (ldy->zol, "DEBUG_LEVEL", "0");
  Zoltan_Set_Param (ldy->zol, "DEBUG_MEMORY", "0");
  Zoltan_Set_Param (ldy->zol, "NUM_GID_ENTRIES", "1");
  Zoltan_Set_Param (ldy->zol, "NUM_LID_ENTRIES", "1");

  switch (ldy->ldb_new)
  {
  case LDB_GEOM:
  {
    /* load balancing parameters */
    Zoltan_Set_Param (ldy->zol, "LB_METHOD", "RCB");
    Zoltan_Set_Param (ldy->zol, "IMBALANCE_TOL", "1.2");
    Zoltan_Set_Param (ldy->zol, "AUTO_MIGRATE", "FALSE");
    Zoltan_Set_Param (ldy->zol, "RETURN_LISTS", "EXPORT");

    /* RCB parameters */
    Zoltan_Set_Param (ldy->zol, "RCB_OVERALLOC", "1.3");
    Zoltan_Set_Param (ldy->zol, "RCB_REUSE", "1");
    Zoltan_Set_Param (ldy->zol, "RCB_OUTPUT_LEVEL", "0");
    Zoltan_Set_Param (ldy->zol, "CHECK_GEOM", "1");
    Zoltan_Set_Param (ldy->zol, "KEEP_CUTS", "0");

    /* callbacks */
    Zoltan_Set_Fn (ldy->zol, ZOLTAN_NUM_OBJ_FN_TYPE, (void (*)()) vertex_count, ldy);
    Zoltan_Set_Fn (ldy->zol, ZOLTAN_OBJ_LIST_FN_TYPE, (void (*)()) vertex_list, ldy);
    Zoltan_Set_Fn (ldy->zol, ZOLTAN_NUM_GEOM_FN_TYPE, (void (*)()) dimensions, ldy);
    Zoltan_Set_Fn (ldy->zol, ZOLTAN_GEOM_MULTI_FN_TYPE, (void (*)()) conpoints, ldy);
  }
  break;
  case LDB_GRAPH:
  {
    /* load balaninc parameters */
    Zoltan_Set_Param (ldy->zol, "LB_METHOD", "HYPERGRAPH");
    Zoltan_Set_Param (ldy->zol, "HYPERGRAPH_PACKAGE", "PHG");
    Zoltan_Set_Param (ldy->zol, "AUTO_MIGRATE", "FALSE");
    Zoltan_Set_Param (ldy->zol, "RETURN_LISTS", "EXPORT");

    /* PHG parameters */
    Zoltan_Set_Param (ldy->zol, "PHG_OUTPUT_LEVEL", "0");

    /* callbacks */
    Zoltan_Set_Fn (ldy->zol, ZOLTAN_NUM_OBJ_FN_TYPE, (void (*)()) vertex_count, ldy);
    Zoltan_Set_Fn (ldy->zol, ZOLTAN_OBJ_LIST_FN_TYPE, (void (*)()) vertex_list, ldy);
    Zoltan_Set_Fn (ldy->zol, ZOLTAN_HG_SIZE_CS_FN_TYPE, (void (*)()) edge_sizes, ldy);
    Zoltan_Set_Fn (ldy->zol, ZOLTAN_HG_CS_FN_TYPE, (void (*)()) edge_list, ldy);
  }
  break;
  default: break;
  }

  /* invalidate reset flag */
  ldy->ldb = ldy->ldb_new;

  /* notify that first partitioning is needed */
  ldy->nexpdia = -1;
}

/* clear external adjacency of a block */
static void clear_adjext (LOCDYN *ldy, DIAB *dia)
{
  OFFB *b, *n;

  for (b = dia->adjext; b; b = n)
  {
    n = b->n;
    MEM_Free (&ldy->offmem, b);
  }

  dia->adjext = NULL;
}

/* build external adjacency */
static void locdyn_adjext (LOCDYN *ldy)
{
  CONEXT *ext;
  SET *item;
  BODY *bod;
  DIAB *dia;
  CON *con;
  OFFB *b;

  for (dia = ldy->dia; dia; dia = dia->n) clear_adjext (ldy, dia);

  for (ext = DOM(ldy->dom)->conext; ext; ext = ext->next) /* for each external constraint in the domain */
  {
    bod = ext->bod; /* for all involved bodies (parents and children) */

    if (bod->kind == OBS) continue; /* obstacles do not trasnder adjacency */

    for (item = SET_First (bod->con); item; item = SET_Next (item))  /* for each regular constraint */
    {
      con = item->data;
      dia = con->dia;

      ERRMEM (b = MEM_Alloc (&ldy->offmem));
      b->dia = NULL; /* there is no diagonal block here */
      b->bod = bod; /* adjacent through this body */
      b->id = ext->id; /* useful when migrated */
      b->ext = ext;
      b->x = NULL;
      b->n = dia->adjext;
      dia->adjext = b;
    }
  }
}

/* balance local dynamics */
static void locdyn_balance (LOCDYN *ldy)
{
  /* reset balancing approach if needed */
  if (ldy->ldb != ldy->ldb_new) ldb_reset (ldy);

  if (ldy->ldb == LDB_OFF)
  {
    DIAB *dia;
    OFFB *b;
    int n;

    /* attach external blocks to regular ones */
    for (n = 0, dia = ldy->dia; dia; n ++, dia = dia->n)
    {
      if (dia->adjext)
      {
        if (dia->adj)
	{
	  for (b = dia->adj; b->n; b = b->n);
	  b->n = dia->adjext;
	}
	else dia->adj = dia->adjext;
      }

      /* calculate degree of the diagonal block */
      for (dia->degree = 1, b = dia->adj; b; b = b->n) dia->degree ++;
    }

    ldy->ndiab = n;
    ldy->diab = ldy->dia;
    ldy->nexpdia = 0;
  }
  else
  {
    MEM setmem;
    DIAB *dia;
    DOM *dom;
    MAP *map, /* maps ranks to sets */
	*set;
    int i, j;

    dom = ldy->dom;
    MEM_Init (&setmem, sizeof (SET), BLKSIZE);
    map = NULL;

    /* map deleted blocks to ranks */
    for (i = 0; i < ldy->ndel; i ++)
    {
      int rank = ldy->del [i]->rank;

      if (!(set = MAP_Find_Node (map, (void*) (long) rank, NULL)))
	set = MAP_Insert (&ldy->mapmem, &map, (void*) (long) rank, NULL, NULL);

      SET_Insert (&setmem, (SET**)&set->data, (void*) (long) ldy->del [i]->id, NULL);
    }

    COMOBJ *send = NULL, *recv, *ptr;
    int nsend, nrecv;
    MAP *item;

    if ((nsend = MAP_Size (map)))
    {
      ERRMEM (send = malloc (sizeof (COMOBJ [nsend])));

      for (ptr = send, item = MAP_First (map); item; item = MAP_Next (item), ptr ++)
      {
	ptr->rank = (int) (long) item->key;
	ptr->o = item->data;
      }
    }

    /* communicate deleted blocks */
    COMOBJS (MPI_COMM_WORLD, TAG_LOCDYN_DELETE, (OBJ_Pack)pack_delete, ldy, (OBJ_Unpack)unpack_delete, send, nsend, &recv, &nrecv);

    MAP_Free (&ldy->mapmem, &map);
    MEM_Release (&setmem);
    free (send);
    free (recv);

    /* prepare update of off-diagonal block ids of balanced blocks */
    for (map = NULL, dia = ldy->dia; dia; dia = dia->n)
    {
      if (!MAP_Find_Node (ldy->insmap, dia, NULL)) /* not newly inserted */
      {
	if (!(set = MAP_Find_Node (map, (void*) (long) dia->rank, NULL)))
	  set = MAP_Insert (&ldy->mapmem, &map, (void*) (long) dia->rank, NULL, NULL);

	SET_Insert (&setmem, (SET**)&set->data, dia, NULL);
      }
    }

    MEM setldb;
    send = NULL;
    MEM_Init (&setldb, sizeof (SET_LDB_PAIR), MAX (nsend, 128));
    if ((nsend = MAP_Size (map)))
    {
      ERRMEM (send = malloc (sizeof (COMOBJ [nsend])));

      for (ptr = send, item = MAP_First (map); item; item = MAP_Next (item), ptr ++)
      {
	SET_LDB_PAIR *pair;

	ERRMEM (pair = MEM_Alloc (&setldb));
	pair->set = item->data;
	pair->ldb = ldy->ldb;
	ptr->rank = (int) (long) item->key;
	ptr->o = pair;
      }
    }

    /* communicate updated off-diagonal block ids to balanced blocks */
    COMOBJS (MPI_COMM_WORLD, TAG_LOCDYN_OFFIDS, (OBJ_Pack)pack_offids, ldy, (OBJ_Unpack)unpack_offids, send, nsend, &recv, &nrecv);

    MAP_Free (&ldy->mapmem, &map);
    MEM_Release (&setldb);
    MEM_Release (&setmem);
    free (send);
    free (recv);

    /* graph balancing data */
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

    int val = vertex_count (ldy, NULL),
        repartitioned = 0,
	sum, min, avg, max;

    /* get statistics on vertex counts */
    PUT_int_stats (1, &val, &sum, &min, &avg, &max);

    /* compute inbalance ratio for boxes */
    double ratio = (double) max / (double) MAX (min, 1);

    if (ldy->nexpdia < 0 || ratio > ldy->imbalance_tolerance) /* update partitioning only if not sufficient to balance boxes */
    {
      /* balance graph comprising the old balanced blocks and the newly inserted unbalanced blocks */
      ASSERT (Zoltan_LB_Balance (ldy->zol, &changes, &num_gid_entries, &num_lid_entries,
	      &num_import, &import_global_ids, &import_local_ids, &import_procs,
	      &num_export, &export_global_ids, &export_local_ids, &export_procs) == ZOLTAN_OK, ERR_ZOLTAN);

      repartitioned = 1;
    }
    else num_export = 0;

    ldy->nexpdia = num_export; /* record for later statistics */

    map = NULL;
    send = NULL;
    if ((nsend = num_export))
    {
      ERRMEM (send = malloc (sizeof (COMOBJ [nsend])));

      for (ptr = send, i = 0; i < num_export; i ++, ptr ++)
      {
	ZOLTAN_ID_TYPE m = export_local_ids [i];

	if (m == UINT_MAX) /* mapped migrated block */
	{
	  ASSERT_DEBUG_EXT (dia = MAP_Find (ldy->idbb, (void*) (long) export_global_ids [i], NULL), "Invalid block id");

	  /* parent block will need to have updated rank of its migrated copy */
	  if (!(set = MAP_Find_Node (map, (void*) (long) dia->rank, NULL)))
	    set = MAP_Insert (&ldy->mapmem, &map, (void*) (long) dia->rank, NULL, NULL);

	  SET_Insert (&setmem, (SET**)&set->data, ptr, NULL); /* map (newrank, dia) the rank set of its parent */
	}
	else /* use local index */
	{
	  ASSERT_DEBUG (m < (unsigned)ldy->nins, "Invalid local index");
	  dia = ldy->ins [m];
	}

	ptr->rank = export_procs [i];
	ptr->o = dia;
      }
    }

    /* communicate migration data (unpacking inserts the imported blocks) */
    COMOBJS (MPI_COMM_WORLD, TAG_LOCDYN_BALANCE, (OBJ_Pack)pack_migrate, ldy, (OBJ_Unpack)unpack_migrate, send, nsend, &recv, &nrecv);

    /* communicate rank updates to parents */
    COMDATA *dsend = NULL, *drecv, *dtr;
    int dnsend, dnrecv;

    if ((dnsend = MAP_Size (map)))
    {
      ERRMEM (dsend = malloc (sizeof (COMDATA [dnsend])));

      for (dtr = dsend, item = MAP_First (map); item; item = MAP_Next (item), dtr ++)
      {
	SET *set = item->data,
	    *jtem;

	dtr->rank = (int) (long) item->key;
	dtr->ints = 2 * SET_Size (set);
	ERRMEM (dtr->i = malloc (sizeof (int [dtr->ints])));
	dtr->doubles = 0;

	for (i = 0, jtem = SET_First (set); jtem; i += 2, jtem = SET_Next (jtem))
	{
	  ptr = jtem->data;
	  dia = ptr->o;
	  dtr->i [i] = dia->id;
	  dtr->i [i + 1] = ptr->rank;
	}
      }
    }

    /* communicate rank updates */
    COM (MPI_COMM_WORLD, TAG_LOCDYN_RANKS, dsend, dnsend, &drecv, &dnrecv);

    /* update ranks of blocks of received ids */
    for (i = 0, dtr = drecv; i < dnrecv; i ++, dtr ++)
    {
      CON *con;

      for (j = 0; j < dtr->ints; j += 2)
      {
	ASSERT_DEBUG_EXT (con = MAP_Find (dom->idc, (void*) (long) dtr->i [j], NULL), "Invalid constraint id");
	con->dia->rank = dtr->i [j + 1]; /* the current rank of the child copy of this block */
      }
    }

    /* update ranks of exported local blocks */
    for (i = 0; i < num_export; i ++)
    {
      ZOLTAN_ID_TYPE m = export_local_ids [i];

      if (m < UINT_MAX) /* local index */
      {
	dia = ldy->ins [m];
	dia->rank = export_procs [i];
      }
    }

    MEM_Release (&setmem);
    MAP_Free (&ldy->mapmem, &map);
    for (i = 0; i < dnsend; i ++) free (dsend [i].i);
    free (dsend);
    free (drecv);

    /* delete migrated balanced blocks */
    for (i = 0, ptr = send; i < nsend; i ++, ptr ++)
    {
      dia = ptr->o;
      if (dia->con == NULL) /* if balanced */
	delete_balanced_block (ldy, dia);
    }

    if (repartitioned)
    {
      Zoltan_LB_Free_Data (&import_global_ids, &import_local_ids, &import_procs,
			   &export_global_ids, &export_local_ids, &export_procs);
    }

    free (send);
    free (recv);

    /* internally 'migrate' blocks from the insertion list to the balanced
     * block list (those that have not been exported away from here) */
    int dsize = 0, isize = 0;
    double *dd = NULL;
    int *ii = NULL;
    for (i = 0; i < ldy->nins; i ++)
    {
      DIAB *dia = ldy->ins [i];

      if (dia->rank == dom->rank) /* same rank => not exported */
      {
	int doubles = 0, ints = 0, dpos = 0, ipos = 0;

	/* use migration routines as a wrapper here */
	pack_migrate (dia, &dsize, &dd, &doubles, &isize, &ii, &ints);
	unpack_migrate (ldy, &dpos, dd, doubles, &ipos, ii, ints);
      }
    }

    /* update constraint coppied data of unbalanced blocks
     * and create remote update sets at the same time */
    SET *upd = NULL;
    for (map = NULL, dia = ldy->dia; dia; dia = dia->n)
    {
      copycon (dia);

      if (dia->rank == dom->rank)
      {
	SET_Insert (&setmem, &upd, dia, NULL); /* shedule for local update */
      }
      else /* schedule for remote update */
      {
	if (!(set = MAP_Find_Node (map, (void*) (long) dia->rank, NULL)))
	  set = MAP_Insert (&ldy->mapmem, &map, (void*) (long) dia->rank, NULL, NULL);

	SET_Insert (&setmem, (SET**)&set->data, dia, NULL);
      }
    }

    /* update local blocks */
    {
      int doubles = 0, ints = 0, dpos = 0, ipos = 0;

      /* use migration routines as a wrapper again */
      pack_update (upd, &dsize, &dd, &doubles, &isize, &ii, &ints);
      unpack_update (ldy, &dpos, dd, doubles, &ipos, ii, ints);
    }
    free (dd);
    free (ii);

    if ((nsend = MAP_Size (map)))
    {
      ERRMEM (send = malloc (sizeof (COMOBJ [nsend])));

      for (ptr = send, item = MAP_First (map); item; item = MAP_Next (item), ptr ++)
      {
	ptr->rank = (int) (long) item->key;
	ptr->o = item->data;
      }
    }
    else send = NULL;

    /* communicate updated blocks */
    COMOBJS (MPI_COMM_WORLD, TAG_LOCDYN_UPDATE, (OBJ_Pack)pack_update, ldy, (OBJ_Unpack)unpack_update, send, nsend, &recv, &nrecv);

    MAP_Free (&ldy->mapmem, &map);
    MEM_Release (&setmem);
    free (send);
    free (recv);
  }

  /* empty the deleted parents table and free parents */
  for (int i = 0; i < ldy->ndel; i ++) MEM_Free (&ldy->diamem, ldy->del [i]);
  ldy->ndel = 0;

  /* empty the inserted parents table */
  MAP_Free (&ldy->mapmem, &ldy->insmap);
  ldy->nins = 0;
}

/* cummunicate reactions from balanced 
 * (migrated) to unbalanced (local) systems */
static void locdyn_gossip (LOCDYN *ldy)
{
  if (ldy->ldb == LDB_OFF)
  {
    OFFB *b, *p;
    DIAB *dia;

    /* detach external blocks from regular ones */
    for (dia = ldy->dia; dia; dia = dia->n)
    {
      if (dia->adjext)
      {
	for (p = NULL, b = dia->adj; b != dia->adjext; p = b, b = b->n);

	if (p) p->n = NULL;
	else dia->adj = NULL;
      }
    }
  }
  else
  {
    double *R;
    MEM setmem;
    DIAB *dia;
    MAP *map, /* map ranks to sets of balanced blocks */
	*set;
    DOM *dom; 
    CON *con;
    int i, j;

    dom = DOM (ldy->dom);

    MEM_Init (&setmem, sizeof (SET), BLKSIZE);

    for (map = NULL, dia = ldy->diab; dia; dia = dia->n)
    {
      if (dia->rank != dom->rank) /* send */
      {
	if (!(set = MAP_Find_Node (map, (void*) (long) dia->rank, NULL)))
	  set = MAP_Insert (&ldy->mapmem, &map, (void*) (long) dia->rank, NULL, NULL);

	SET_Insert (&setmem, (SET**)&set->data, dia, NULL); /* map balanced blocks to their parent ranks */
      }
      else /* do not send */
      {
	ASSERT_DEBUG_EXT (con = MAP_Find (dom->idc, (void*) (long) dia->id, NULL), "Invalid constraint id"); /* find constraint */
	R = con->dia->R;
	COPY (dia->R, R); /* update reaction */
      }
    }

    COMDATA *send = NULL, *recv, *ptr;
    int nsend, nrecv;
    SET *item;

    if ((nsend = MAP_Size (map))) /* the number of distinct parent ranks */
    {
      ERRMEM (send = malloc (sizeof (COMDATA [nsend])));

      for (set = MAP_First (map), ptr = send; set; set = MAP_Next (set), ptr ++)
      {
	ptr->rank = (int) (long) set->key;
	ptr->ints = SET_Size ((SET*)set->data);
	ptr->doubles = 3 * ptr->ints;
	ERRMEM (ptr->i = malloc (sizeof (int [ptr->ints]))); /* ids */
	ERRMEM (ptr->d = malloc (sizeof (double [ptr->doubles]))); /* reactions */

	for (i = 0, item = SET_First ((SET*)set->data); item; i ++, item = SET_Next (item))
	{
	  dia = item->data;

	  ptr->i [i] = dia->id;
	  COPY (dia->R, &ptr->d [3*i]);
	}
      }
    }

    /* communicate reactions */
    COM (MPI_COMM_WORLD, TAG_LOCDYN_REAC, send, nsend, &recv, &nrecv);

    for (i = 0, ptr = recv; i < nrecv; i ++, ptr ++)
    {
      for (j = 0; j < ptr->ints; j ++)
      {
	ASSERT_DEBUG_EXT (con = MAP_Find (dom->idc, (void*) (long) ptr->i[j], NULL), "Invalid constraint id"); /* find constraint */
	dia = con->dia;
	R = &ptr->d [3*j];
	COPY (R, dia->R); /* update reaction */
      }
    }

    MEM_Release (&setmem);
    MAP_Free (&ldy->mapmem, &map);
    for (i = 0, ptr = send; i < nsend; i ++, ptr ++)
    { free (ptr->i); free (ptr->d); }
    free (send);
    free (recv);
  }
}

/* append buffer with a new item */
static void append (DIAB ***buf, int *n, int *s, DIAB *dia)
{
  int i = *n;

  (*n) ++;

  if ((*n) >= (*s))
  {
    (*s) *= 2;
    ERRMEM ((*buf) = realloc (*buf, (*s) * sizeof (DIAB*)));
  }

  (*buf) [i] = dia;
}

/* create MPI related data */
static void create_mpi (LOCDYN *ldy)
{
  ERRMEM (ldy->ins = malloc (BLKSIZE * sizeof (DIAB*)));
  ldy->insmap = NULL;
  ldy->sins = BLKSIZE;
  ldy->nins = 0;

  ERRMEM (ldy->del = malloc (BLKSIZE * sizeof (DIAB*)));
  ldy->sdel = BLKSIZE;
  ldy->ndel = 0;

  MEM_Init (&ldy->mapmem, sizeof (MAP), BLKSIZE);
  MEM_Init (&ldy->setmem, sizeof (SET), BLKSIZE);

  ldy->idbb = NULL;
  ldy->diab = NULL;
  ldy->ndiab = 0;

  ldy->REXT = NULL;
  ldy->REXT_count = 0;

  ldy->zol = NULL;
  ldy->ldb = LDB_OFF;
  ldy->ldb_new = ldy->ldb;
  ldy->nexpdia = -1; /* notify that first partitioning is needed */
  ldy->imbalance_tolerance = 1.3;
}

/* destroy MPI related data */
static void destroy_mpi (LOCDYN *ldy)
{
  free (ldy->ins);
  free (ldy->del);

  if (ldy->zol) Zoltan_Destroy (&ldy->zol);
}
#endif

/* create local dynamics for a domain */
LOCDYN* LOCDYN_Create (void *dom)
{
  LOCDYN *ldy;

  ERRMEM (ldy = malloc (sizeof (LOCDYN)));
  MEM_Init (&ldy->offmem, sizeof (OFFB), BLKSIZE);
  MEM_Init (&ldy->diamem, sizeof (DIAB), BLKSIZE);
  ldy->dom = dom;
  ldy->dia = NULL;
  ldy->modified = 0;

#if MPI
  create_mpi (ldy);
#endif

  return ldy;
}

/* insert a 'con'straint between a pair of bodies =>
 * return the diagonal entry of the local dynamical system */
DIAB* LOCDYN_Insert (LOCDYN *ldy, void *con, BODY *one, BODY *two)
{
  DIAB *dia, *nei;
  SET *item;
  OFFB *b;
  CON *c;

  ERRMEM (dia = MEM_Alloc (&ldy->diamem));
  dia->R = CON(con)->R;
  dia->con = con;

#if MPI
  dia->id = CON(con)->id;
  dia->rank = DOM(ldy->dom)->rank;

  /* schedule for balancing use */
  append (&ldy->ins, &ldy->nins, &ldy->sins, dia);
  MAP_Insert (&ldy->mapmem, &ldy->insmap, dia, (void*) (long) (ldy->nins-1), NULL);
#endif

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

      if (c != con && c->dia) /* skip the coincident or unattached yet constraint */
      {
	nei = c->dia;

        /* allocate block and put into 'nei->adj' list */ 
	ERRMEM (b = MEM_Alloc (&ldy->offmem));
	b->dia = dia; /* adjacent with 'dia' */
	b->bod = one; /* adjacent trough body 'one' */
	b->n = nei->adj; /* extend list ... */
	nei->adj = b; /* ... */
#if MPI
	b->id = dia->id;
#endif

	/* allocate block and put into 'dia->adj' list */ 
	ERRMEM (b = MEM_Alloc (&ldy->offmem));
	b->dia = nei; /* adjacent with 'nei' */
	b->bod = one; /* ... trough 'one' */
	b->n = dia->adj;
	dia->adj = b;
#if MPI
	b->id = nei->id;
#endif
      }
    }
  }

  if (two && two->kind != OBS) /* 'one' replaced with 'two' */
  {
    for (item = SET_First (two->con); item; item = SET_Next (item))
    {
      c = item->data;

      if (c != con && c->dia) /* skip the coincident or unattached yet constraint */
      {
	nei = c->dia;

        /* allocate block and put into 'nei->adj' list */ 
	ERRMEM (b = MEM_Alloc (&ldy->offmem));
	b->dia = dia; /* adjacent with 'dia' */
	b->bod = two; /* adjacent trough body 'two' */
	b->n = nei->adj; /* extend list ... */
	nei->adj = b; /* ... */
#if MPI
	b->id = dia->id;
#endif

	/* allocate block and put into 'dia->adj' list */ 
	ERRMEM (b = MEM_Alloc (&ldy->offmem));
	b->dia = nei; /* adjacent with 'nei' */
	b->bod = two; /* ... trough 'two' */
	b->n = dia->adj;
	dia->adj = b;
#if MPI
	b->id = nei->id;
#endif
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

#if MPI
  /* in LDB_OFF mode regular blocks might store the below sets */
  if (dia->children) MAP_Free (&ldy->mapmem, &dia->children);
  if (dia->rext) SET_Free (&ldy->setmem, &dia->rext);

  MAP *item, *jtem;

  if ((item = MAP_Find_Node (ldy->insmap, dia, NULL))) /* was inserted and now is deleted before balancing */
  {
    ldy->ins [(int) (long) item->data] = ldy->ins [-- ldy->nins]; /* replace this item withe the last one */
    ASSERT_DEBUG (jtem = MAP_Find_Node (ldy->insmap, ldy->ins [(int) (long) item->data], NULL), "Failed to find an inserted block");
    jtem->data = item->data; /* update block to index mapping */
    MAP_Delete_Node (&ldy->mapmem, &ldy->insmap, item); /* remove from map */
    MEM_Free (&ldy->diamem, dia); /* and free */
  }
  else
  {
    clear_adjext (ldy, dia); /* clear external adjacency */
    append (&ldy->del, &ldy->ndel, &ldy->sdel, dia); /* schedule for balancing use and a later deletion */
  }
#else
  /* destroy passed dia */
  MEM_Free (&ldy->diamem, dia);
#endif

  /* mark as modified */
  ldy->modified = 1;
}

/* updiae local dynamics => prepare for a solution */
void LOCDYN_Update_Begin (LOCDYN *ldy, UPKIND upkind)
{
  DOM *dom = ldy->dom;
  double step = dom->step;
  DIAB *dia;

#if MPI
  if (dom->rank == 0)
#endif
  if (dom->verbose) printf ("LOCDYN ... "), fflush (stdout);

#if MPI
  SOLFEC_Timer_Start (DOM(ldy->dom)->owner, "LOCBAL");

  locdyn_adjext (ldy);

  SOLFEC_Timer_End (DOM(ldy->dom)->owner, "LOCBAL");
#endif

  SOLFEC_Timer_Start (DOM(ldy->dom)->owner, "LOCDYN");

  /* calculate local velocities and
   * assmeble the force-velocity 'W' operator */
  for (dia = ldy->dia; dia; dia = dia->n)
  {
    CON *con = dia->con;
    BODY *m = con->master,
	 *s = con->slave;
    void *mgobj = con->mgobj,
	 *sgobj = con->sgobj;
    SHAPE *mshp = con->mshp,
	  *sshp = con->sshp;
    double *mpnt = con->mpnt,
	   *spnt = con->spnt,
	   *base = con->base,
	   *V = dia->V,
	   *B = dia->B,
           X [3], Y [9];
    MX_DENSE_PTR (W, 3, 3, dia->W);
    MX_DENSE (C, 3, 3);
    MX *mH, *sH;
    OFFB *blk;

    /* previous time step velocity */
    BODY_Local_Velo (m, PREVELO, mshp, mgobj, mpnt, base, X); /* master body pointer cannot be NULL */
    if (s) BODY_Local_Velo (s, PREVELO, sshp, sgobj, spnt, base, Y); /* might be NULL for some constraints (one body) */
    else { SET (Y, 0.0); }
    SUB (Y, X, V); /* relative = slave - master => outward master normal */

    /* local free velocity */
    BODY_Local_Velo (m, CURVELO, mshp, mgobj, mpnt, base, X);
    if (s) BODY_Local_Velo (s, CURVELO, sshp, sgobj, spnt, base, Y);
    else { SET (Y, 0.0); }
    SUB (Y, X, B);

    /* diagonal block */
    mH = BODY_Gen_To_Loc_Operator (m, mshp, mgobj, mpnt, base);
    MX_Trimat (mH, m->inverse, MX_Tran (mH), &W); /* H * inv (M) * H^T */
    if (s)
    { sH = BODY_Gen_To_Loc_Operator (s, sshp, sgobj, spnt, base);
      MX_Trimat (sH, s->inverse, MX_Tran (sH), &C); /* H * inv (M) * H^T */
      NNADD (W.x, C.x, W.x); }
    SCALE9 (W.x, step); /* W = h * ( ... ) */

    NNCOPY (W.x, C.x); /* calculate regularisation parameter */
    ASSERT (lapack_dsyev ('N', 'U', 3, C.x, 3, X, Y, 9) == 0, ERR_LDY_EIGEN_DECOMP);
    dia->rho = 1.0 / X [2]; /* inverse of maximal eigenvalue */

    /* off-diagonal blocks if requested */
    for (blk = dia->adj; upkind == UPALL && blk; blk = blk->n)
    {
      DIAB *dia = blk->dia;
      CON *con = dia->con;
      BODY *bod = blk->bod;
      MX *lH, *rH, *inv;
      MX_DENSE_PTR (W, 3, 3, blk->W);
      double coef;

      ASSERT_DEBUG (bod == m || bod == s, "Off diagonal block is not connected!");
     
      lH = (bod == m ? mH : sH); /* dia->bod is a valid body (not an obstacle)
                                   as it was inserted into the dual graph */
      inv = bod->inverse;

      if (bod == con->master)
      {
	rH =  BODY_Gen_To_Loc_Operator (bod, con->mshp, con->mgobj, con->mpnt, con->base);
	coef = (bod == s ? -step : step);
      }
      else /* blk->bod == dia->slave */
      {
	rH =  BODY_Gen_To_Loc_Operator (bod, con->sshp, con->sgobj, con->spnt, con->base);
	coef = (bod == m ? -step : step);
      }

      MX_Trimat (lH, inv, MX_Tran (rH), &W);
      SCALE9 (W.x, coef);
      MX_Destroy (rH);
    }

#if MPI
    /* off-diagonal external blocks if requested */
    for (blk = dia->adjext; upkind == UPALL && blk; blk = blk->n)
    {
      CONEXT *ext = blk->ext;
      BODY *bod = blk->bod;
      MX *lH, *rH, *inv;
      MX_DENSE_PTR (W, 3, 3, blk->W);
      double coef;

      ASSERT_DEBUG (bod == m || bod == s, "Off diagonal block is not connected!");
     
      lH = (bod == m ? mH : sH);
                               
      inv = bod->inverse;

      rH =  BODY_Gen_To_Loc_Operator (bod, ext->sgp->shp, ext->sgp->gobj, ext->point, ext->base);

      if (ext->isma) coef = (bod == s ? -step : step);
      else coef = (bod == m ? -step : step);

      MX_Trimat (lH, inv, MX_Tran (rH), &W);
      SCALE9 (W.x, coef);
      MX_Destroy (rH);
    }
#endif

    MX_Destroy (mH);
    if (s) MX_Destroy (sH);
  }

  /* forward variables change */
  variables_change_begin (ldy);

  SOLFEC_Timer_End (DOM(ldy->dom)->owner, "LOCDYN");

#if MPI
  if (dom->verbose && dom->rank == 0)
  {
    switch (ldy->ldb_new)
    {
    case LDB_GEOM: printf ("GEOM BALANCING ... "), fflush (stdout); break;
    case LDB_GRAPH: printf ("GRAPH BALANCING ... "), fflush (stdout); break;
    default: break;
    }
  }

  SOLFEC_Timer_Start (dom->owner, "LOCBAL");

  locdyn_balance (ldy);

  SOLFEC_Timer_End (dom->owner, "LOCBAL");
#endif
}

/* updiae local dynamics => after the solution */
void LOCDYN_Update_End (LOCDYN *ldy)
{
  SOLFEC_Timer_Start (DOM(ldy->dom)->owner, "LOCDYN");

  /* backward variables change */
  variables_change_end (ldy);

  /* not modified */
  ldy->modified = 0;

  SOLFEC_Timer_End (DOM(ldy->dom)->owner, "LOCDYN");

#if MPI
  SOLFEC_Timer_Start (DOM(ldy->dom)->owner, "LOCBAL");

  locdyn_gossip (ldy);

  SOLFEC_Timer_End (DOM(ldy->dom)->owner, "LOCBAL");
#endif
}

#if MPI
/* change load balancing algorithm */
void LOCDYN_Balancing (LOCDYN *ldy, LDB ldb)
{
  ldy->ldb_new = ldb;

   /* only set the above flag here and delay reset of the balancing approach
    * as this call might be invoked from a Python callback in the course */
}

/* update mapping of balanced external reactions */
void LOCDYN_REXT_Update (LOCDYN *ldy)
{
  int *local_ids,
      *global_ids,
      *size, *disp,
      ncpu, i, j, k, n;
  DIAB *dia;
  OFFB *b;

  ncpu = DOM(ldy->dom)->ncpu;

  ERRMEM (size = malloc (sizeof (int [ncpu])));
  ERRMEM (disp = malloc (sizeof (int [ncpu])));
  ERRMEM (local_ids = malloc (sizeof (int [ldy->ndiab])));

  /* gather counts of local balanced blocks into size table */
  MPI_Allgather (&ldy->ndiab, 1, MPI_INT, size, 1, MPI_INT, MPI_COMM_WORLD);

  for (i = disp [0] = 0; i < ncpu - 1; i ++) disp [i+1] = disp [i] + size [i];

  MAP *idrank = NULL; /* id to rank map */
  MAP *ididx = NULL; /* id to local REXT index map */
  MEM *mapmem = &ldy->mapmem;
  MEM *setmem = &ldy->setmem;
  MAP *item;
  XR *x;

  if ((n = (disp [ncpu-1] + size [ncpu-1])))
  {
    ERRMEM (global_ids = malloc (sizeof (int [n])));

    for (i = 0, dia = ldy->diab; dia; i ++, dia = dia->n) local_ids [i] = dia->id;

    /* gather all local balanced block ids into one global table of global_ids */
    MPI_Allgatherv (local_ids, ldy->ndiab, MPI_INT, global_ids, size, disp, MPI_INT, MPI_COMM_WORLD);

    for (k = i = 0; i < ncpu; i ++)
      for (j = 0; j < size [i]; j ++, k ++)
	MAP_Insert (mapmem, &idrank, (void*) (long) global_ids [k], (void*) (long) i, NULL);
  }

  /* clean up and preprocess REXT related data */
  for (n = 0, dia = ldy->diab; dia; dia = dia->n)
  {
    MAP_Free (mapmem, &dia->children);
    SET_Free (setmem, &dia->rext);

    for (b = dia->adj; b; b = b->n)
    {
      if (!b->dia)
      {
	if (!MAP_Find_Node (ididx, (void*) (long) b->id, NULL))
	  MAP_Insert (mapmem, &ididx, (void*) (long) b->id, (void*) (long) (n ++), NULL);
	b->x = NULL;
      }
    }
  }
  free (ldy->REXT);
  ldy->REXT = NULL;
  ldy->REXT_count = n;

  if (n)
  {
    ERRMEM (ldy->REXT = calloc (n, sizeof (XR))); /* zero reactions by the way */
    for (i = 0, x = ldy->REXT; i < n; i ++, x ++) x->rank = -1;

    /* build up REXT related data */
    for (dia = ldy->diab; dia; dia = dia->n)
    {
      for (b = dia->adj; b; b = b->n)
      {
	if (!b->dia)
	{
	  ASSERT_DEBUG_EXT (item = MAP_Find_Node (ididx, (void*) (long) b->id, NULL), "Inconsitency in id to index mapping");
	  x = &ldy->REXT [(int) (long) item->data];

	  if (x->rank < 0)
	  {
	    ASSERT_DEBUG_EXT (item = MAP_Find_Node (idrank, (void*) (long) b->id, NULL), "Inconsitency in id to rank mapping");
	    x->rank = (int) (long) item->data;
	    x->id = b->id;
	  }

	  b->x = x; /* mapped to an REXT entry */

	  SET_Insert (setmem, &dia->rext, x, NULL); /* store for fast acces */
	}
      }
    }
  }

  MAP_Free (mapmem, &ididx);
  MAP_Free (mapmem, &idrank);
  free (size);
  free (disp);
  free (local_ids);
  free (global_ids);

#if DEBUG
  for (i = 0, x = ldy->REXT; i < n; i ++, x ++)
    ASSERT_DEBUG (x->id && x->rank >= 0, "Inconsitency in mapping external reactions");
#endif

  /* now send (id, index) pairs to parent blocks pointed by their ranks in REXT;
   * this way we shall fill up the 'children' members of balanced blocks */

  COMDATA *send = NULL, *recv, *ptr;
  int ssiz, nsend, nrecv, *pair;

  if ((nsend = n))
  {
    ERRMEM (send = malloc (sizeof (COMDATA [n]) + sizeof (int [n * 2])));;
    pair = (int*) (send + n);

    for (i = 0, x = ldy->REXT, ptr = send; i < n; i ++, x ++, ptr ++, pair += 2)
    {
      ptr->rank = x->rank;
      ptr->ints = 2;
      ptr->doubles = 0;
      ptr->i = pair;
      ptr->d = NULL;
      pair [0] = x->id;
      pair [1] = (x - ldy->REXT);
    }
  }

  COM (MPI_COMM_WORLD, TAG_LOCDYN_REXT, send, nsend, &recv, &nrecv);

  for (i = 0, ptr = recv; i < nrecv; i ++, ptr ++)
  {
    for  (j = 0, pair = ptr->i; j < ptr->ints; j += 2, pair += 2)
    {
      if (ldy->ldb == LDB_OFF)
      {
	CON *con;
        ASSERT_DEBUG_EXT (con = MAP_Find (DOM(ldy->dom)->idc, (void*) (long) pair [0], NULL), "Invalid block id");
	dia = con->dia;
      }
      else
      {
        ASSERT_DEBUG_EXT (dia = MAP_Find (ldy->idbb, (void*) (long) pair [0], NULL), "Invalid block id");
      }

      MAP_Insert (mapmem, &dia->children, (void*) (long) ptr->rank, (void*) (long) pair [1], NULL); /* map child rank to the local index of its XR */
    }
  }

  free (send);
  free (recv);

  /* update REXT reactions to the latest
   * values stored in balanced blocks */

  ssiz = MAX (ldy->ndiab, 128);
  ERRMEM (send = malloc (sizeof (COMDATA [ssiz])));

  for (ptr = send, nsend = 0, dia = ldy->diab; dia; dia = dia->n)
  {
    for (item = MAP_First (dia->children); item; item = MAP_Next (item))
    {
      ptr->rank = (int) (long) item->key;
      ptr->ints = 1;
      ptr->doubles = 3;
      ptr->i = (int*) &item->data;
      ptr->d = dia->R;
      ptr = sendnext (++ nsend, &ssiz, &send);
    }
  }

  COM (MPI_COMM_WORLD, TAG_LOCDYN_REXT_INIT, send, nsend, &recv, &nrecv);

  for (ptr = recv, i = 0; i < nrecv; ptr ++, i ++)
  {
    double *R = ldy->REXT [ptr->i [0]].R;
    COPY (ptr->d, R);
  }

  free (send);
  free (recv);
}

/* pack union data */
static void pack_union (SET *set, int *dsize, double **d, int *doubles, int *isize, int **i, int *ints)
{
  pack_int (isize, i, ints, SET_Size (set));
  for (SET *item = SET_First (set); item; item = SET_Next (item))
  {
    DIAB *dia = item->data;

    if (dia->con) copycon (dia); /* copy constraint data */

    pack_block (dia, dsize, d, doubles, isize, i, ints);
    pack_int (isize, i, ints, dia->id);

    /* pack children */
    pack_int (isize, i, ints, MAP_Size (dia->children));
    for (MAP *jtem = MAP_First (dia->children); jtem; jtem = MAP_Next (jtem))
    {
      pack_int (isize, i, ints, (int) (long) jtem->key);
      pack_int (isize, i, ints, (int) (long) jtem->data);
    }
  }
}

/* unpack union data */
static void* unpack_union (LOCDYN *ldy, int *dpos, double *d, int doubles, int *ipos, int *i, int ints)
{
  SET *out;
  int ndia;

  ndia = unpack_int (ipos, i, ints);
  for (out = NULL; ndia > 0; ndia --)
  {
    DIAB *dia;
    ERRMEM (dia = MEM_Alloc (&ldy->diamem));
    dia->R = dia->REAC;
    unpack_block (dia, &ldy->offmem, NULL, dpos, d, doubles, ipos, i, ints);
    dia->id = unpack_int (ipos, i, ints);

    /* unpack children */
    int n, rnk, idx, nch = unpack_int (ipos, i, ints);
    for (n = 0; n < nch; n ++)
    {
      rnk = unpack_int (ipos, i, ints);
      idx = unpack_int (ipos, i, ints);
      MAP_Insert (&ldy->mapmem, &dia->children, (void*) (long) rnk, (void*) (long) idx, NULL);
    }

    /* inser into the output set */
    SET_Insert (&ldy->setmem, &out, dia, NULL);
  }

  return out;
}

/* MPI operator callback used to find minimal score rank */
static void min_score (int *in, int *inout, int *len, MPI_Datatype *type)
{
  int i;

  for (i = 0; i < *len; i ++)
  {
    if (*in < *inout)
    {
      inout [0] = in [0];
      inout [1] = in [1];
    }

    inout += 2;
    in += 2;
  }
}

/* id and reaction pair */
typedef struct id_r_pair ID_R_PAIR;

struct id_r_pair
{
  int id;
  double R [3];
};

/* union communication patterns and data */
typedef struct union_pattern UNION_PATTERN;

struct union_pattern
{
  int root; /* minimal score rank */

  COMOBJ *recv; /* gathers sets from not-minimal-score ranks */
  int nrecv;

  XR *REXT; /* external reactions used in OFFBs that cannot be locally mapped */
  int REXT_count;

  MPI_Datatype id_r_type; /* description of ID_R_PAIR */

  double **gather_R; /* pointers of reactions to be coppied into gather_send */
  ID_R_PAIR *gather_send;
  int gather_send_count;

  ID_R_PAIR *gather_recv;
  int *gather_recv_counts;
  int *gather_recv_disps;
  int gather_count;

  double **scatter_R; /* pointers of reactions to be coppied into scatter_send */
  ID_R_PAIR *scatter_send;
  int *scatter_send_counts;
  int *scatter_send_disps;
  int scatter_count;

  ID_R_PAIR *scatter_recv;
  int scatter_recv_count;

  LOCDYN *ldy;

  SET *uni; /* union of mid node sets (not NULL on root rank) */
};

/* return the union of 'inp' sets at the processor with smallest 'score'; return
 * communication 'pattern' used to gather and scatter reactions in the union */
SET* LOCDYN_Union_Create (LOCDYN *ldy, SET *inp, int score, void **pattern)
{
  int in [2], out [2];
  MPI_Datatype type;
  int rank, ncpu;
  MPI_Op op;

  rank = DOM(ldy->dom)->rank;
  ncpu = DOM(ldy->dom)->ncpu;

  in [0] = score;
  in [1] = rank;

  MPI_Type_contiguous (2, MPI_INT, &type);
  MPI_Type_commit (&type);
  MPI_Op_create ((MPI_User_function*)min_score, 1, &op);
  MPI_Allreduce (in, out, 1, type, op, MPI_COMM_WORLD); /* compute (min(score), rank(min(score))) in out */
  MPI_Type_free (&type);
  MPI_Op_free (&op);

  COMOBJ send, *ptr, *end;
  UNION_PATTERN *up;
  int i, nsend;
  DIAB *dia;
  SET *item;
  MAP *jtem;
  CON *con;
  OFFB *b;
  XR *x;
 
  ERRMEM (up = calloc (1, sizeof (UNION_PATTERN)));
  up->root = out [1];
  up->ldy = ldy;

  ID_R_PAIR id_r_example;
  MPI_Datatype id_r_types [3] = {MPI_INT, MPI_DOUBLE, MPI_UB};
  MPI_Aint id_r_disps [3], id_r_base, id_r_id, id_r_R;
  int id_r_lengths [3] = {1, 3, 1};

  MPI_Get_address (&id_r_example.id, &id_r_id);
  MPI_Get_address (&id_r_example, &id_r_base);
  MPI_Get_address (&id_r_example.R, &id_r_R);

  id_r_disps [0] = id_r_id - id_r_base;
  id_r_disps [1] = id_r_R - id_r_base;
  id_r_disps [2] = sizeof (id_r_example);

  /* create ID_R_PAIR type descriptor */
  MPI_Type_create_struct (3, id_r_lengths, id_r_disps, id_r_types, &up->id_r_type);
  MPI_Type_commit (&up->id_r_type);

  if (in [1] != out [1]) /* we are sending the 'inp'ut set */
  {
    send.rank = out [1]; /* all processors send their 'inp'ut sets to the one having minimal score */
    send.o = inp;
    nsend = (inp ? 1 : 0);
  }
  else nsend = 0; /* this is the minimal score processor */

  /* send and receive block sets */
  COMOBJS (MPI_COMM_WORLD, TAG_LOCDYN_UNION_INIT, (OBJ_Pack) pack_union,
           ldy, (OBJ_Unpack) unpack_union, &send, nsend, &up->recv, &up->nrecv);

  MAP *idtodia;

  /* map received diagonal blocks to their ids */
  for (idtodia = NULL, ptr = up->recv, end = ptr + up->nrecv; ptr < end; ptr ++)
  {
    for (item = SET_First (ptr->o); item; item = SET_Next (item))
    {
      dia = item->data;
      MAP_Insert (&ldy->mapmem, &idtodia, (void*) (long) dia->id, (void*) (long) dia, NULL);
    }
  }

  MAP *idtoext;

  /* map imported off-diagonal blocks to imported or existing diagonal blocks or, if not possible, to up->REXT indices */
  for (idtoext = NULL, ptr = up->recv, end = ptr + up->nrecv; ptr < end; ptr ++)
  {
    for (item = SET_First (ptr->o); item; item = SET_Next (item))
    {
      for (dia = item->data, b = dia->adj; b; b = b->n)
      {
	b->dia = MAP_Find (idtodia, (void*) (long) b->id, NULL); /* try imported blocks first */

	if (!b->dia)  /* failed => try existing blocks */
	{
	  if (ldy->ldb == LDB_OFF)
	  {
	    if ((con = MAP_Find (DOM(ldy->dom)->idc, (void*) (long) b->id, NULL))) b->dia = con->dia; /* unbalanced */
	  }
	  else b->dia = MAP_Find (ldy->idbb, (void*) (long) b->id, NULL); /* balanced */
	}

	if (!b->dia) /* still failure => request import from the parent processor */
	{
	  if (!MAP_Find_Node (idtoext, (void*) (long) b->id, NULL))
	  {
	    MAP_Insert (&ldy->mapmem, &idtoext, (void*) (long) b->id, (void*) (long) up->REXT_count, NULL);
	    up->REXT_count ++;
	  }
	}
      }
    }
  }

  /* allocate external reactions */
  if (up->REXT_count) ERRMEM (up->REXT = malloc (sizeof (XR [up->REXT_count])));
  for (i = 0, x = up->REXT; i < up->REXT_count; i ++, x ++) x->rank = -1; /* invalidate ranks */

  /* map them to off-diagonal blocks */
  for (ptr = up->recv, end = ptr + up->nrecv; ptr < end; ptr ++)
  {
    for (item = SET_First (ptr->o); item; item = SET_Next (item))
    {
      for (dia = item->data, b = dia->adj; b; b = b->n)
      {
	if (!b->dia)
	{
	  MAP *node;

	  ASSERT_DEBUG_EXT (node = MAP_Find_Node (idtoext, (void*) (long) b->id, NULL), "Inconsistent ID to rank mapping");

	  x = &up->REXT [(int) (long) node->data];
	  b->x = x;

	  if (x->rank < 0)
	  {
	    x->rank = ptr->rank;
	    x->id = (int) (long) node->key;
	  }
	}
      }
    }
  }

#if DEBUG
  for (i = 0, x = up->REXT; i < up->REXT_count; i ++, x ++)
    ASSERT_DEBUG (x->id && x->rank >= 0, "Inconsitency in mapping external reactions");
#endif

  /* send REXT (id, index) pairs to parent ranks */

  COMDATA *dsend = NULL, *drecv, *qtr;
  int j, k, nrecv, *pair;

  if ((nsend = up->REXT_count))
  {
    ERRMEM (dsend = malloc (sizeof (COMDATA [nsend]) + sizeof (int [nsend * 2])));;
    pair = (int*) (dsend + nsend);

    for (i = 0, x = up->REXT, qtr = dsend; i < nsend; i ++, x ++, qtr ++, pair += 2)
    {
      qtr->rank = x->rank;
      qtr->ints = 2;
      qtr->doubles = 0;
      qtr->i = pair;
      qtr->d = NULL;
      pair [0] = x->id;
      pair [1] = (x - up->REXT);
    }
  }

  COM (MPI_COMM_WORLD, TAG_LOCDYN_UNION_REXT, dsend, nsend, &drecv, &nrecv);

  ASSERT_DEBUG (nrecv <= 1, "Inconsistent receive during REXT setup");

  if (nrecv)
  {
    /* prepare gather send data */
    up->gather_send_count = drecv->ints / 2;
    ERRMEM (up->gather_R = malloc (up->gather_send_count * sizeof (double*)));
    ERRMEM (up->gather_send = malloc (sizeof (ID_R_PAIR [up->gather_send_count])));

    double **R = up->gather_R;
    ID_R_PAIR *IDR = up->gather_send;

    for  (j = 0, pair = drecv->i; j < drecv->ints; j += 2, pair += 2, R ++, IDR ++)
    {
      *R = NULL;

      if (ldy->ldb == LDB_OFF)
      {
        if ((con = MAP_Find (DOM(ldy->dom)->idc, (void*) (long) pair [0], NULL))) *R = con->R;
      }
      else
      {
        if ((dia = MAP_Find (ldy->idbb, (void*) (long) pair [0], NULL))) *R = dia->R;
      }

      if (*R == NULL) /* must be in ldy->REXT */
      {
	for (k = 0, x = ldy->REXT; k < ldy->REXT_count; x ++, k ++) /* linear search => TODO: binary map of ids to REXT really needed? */
	{
	  if (x->id == pair [0])
	  {
	    *R = x->R;
	    break;
	  }
	}
      }

      ASSERT_DEBUG (*R, "Inconsitent mapping of ID to reaction");
      IDR->id = pair [1];
    }
  }

  free (dsend);
  free (drecv);

  if (in [1] == out [1])
  {
    /* prepare gather receive data */
    ERRMEM (up->gather_recv_counts = malloc (sizeof (int [ncpu])));
    ERRMEM (up->gather_recv_disps = malloc (sizeof (int [ncpu])));
  }

  /* gather receive counts */
  MPI_Gather (&up->gather_send_count, 1, MPI_INT, up->gather_recv_counts, 1, MPI_INT, up->root, MPI_COMM_WORLD);

  if (in [1] == out [1])
  {
    /* compute disps */
    for (i = 0; i < ncpu; i ++)
    {
      up->gather_count += up->gather_recv_counts [i];
      if (i < (ncpu - 1)) up->gather_recv_disps [i+1] = up->gather_count;
    }
    up->gather_recv_disps [0] = 0;

    ERRMEM (up->gather_recv = malloc (sizeof (ID_R_PAIR [up->gather_count])));
  }

  ASSERT_DEBUG (up->gather_count == up->REXT_count, "Inconsistent gather count");

  /* prepare scatter send data */
  if (in [1] == out [1])
  {
    MAP **rtoid;

    ERRMEM (rtoid = calloc (ncpu, sizeof (MAP*)));

    for (ptr = up->recv, end = ptr + up->nrecv; ptr < end; ptr ++) /* for each imported block */
    {
      for (item = SET_First (ptr->o); item; item = SET_Next (item))
      {
	dia = item->data;

	MAP_Insert (&ldy->mapmem, &rtoid [ptr->rank], dia->R, (void*) (long) (-dia->id), NULL); /* negative indicates bloack id */

	for (jtem = MAP_First (dia->children); jtem; jtem = MAP_Next (jtem))
	{
	  MAP_Insert (&ldy->mapmem, &rtoid [(int) (long) jtem->key], dia->R, jtem->data, NULL); /* jtem->data >= 0 indicates REXT index */
	}
      }
    }

    for (item = SET_First (inp); item; item = SET_Next (item)) /* for each local block */
    {
      dia = item->data;

      for (jtem = MAP_First (dia->children); jtem; jtem = MAP_Next (jtem)) 
      {
	MAP_Insert (&ldy->mapmem, &rtoid [(int) (long) jtem->key], dia->R, jtem->data, NULL); /* jtem->data >= 0 indicates REXT index */
      }
    }

    ERRMEM (up->scatter_send_counts = calloc (ncpu, sizeof (int)));
    ERRMEM (up->scatter_send_disps = malloc (sizeof (int [ncpu])));

    /* compute counts and disps */
    for (i = 0; i < ncpu; i ++)
    {
      up->scatter_send_counts [i] = MAP_Size (rtoid [i]);
      up->scatter_count += up->scatter_send_counts [i];
      if (i < (ncpu - 1)) up->scatter_send_disps [i+1] = up->scatter_count;
    }
    up->scatter_send_disps [0] = 0;

    ERRMEM (up->scatter_R = malloc (up->scatter_count * sizeof (double*)));
    ERRMEM (up->scatter_send = malloc (sizeof (ID_R_PAIR [up->scatter_count])));

    double **R = up->scatter_R;
    ID_R_PAIR *IDR = up->scatter_send;

    for (i = 0; i < ncpu; i ++)
    {
      for (jtem = MAP_First (rtoid [i]); jtem; jtem = MAP_Next (jtem), R ++, IDR ++)
      {
	*R = jtem->key;
	IDR->id = (int) (long) jtem->data;
      }

      MAP_Free (&ldy->mapmem, &rtoid [i]);
    }

    free (rtoid);
  }

  /* scatter receive counts */
  MPI_Scatter (up->scatter_send_counts, 1, MPI_INT, &up->scatter_recv_count, 1, MPI_INT, up->root, MPI_COMM_WORLD);

  if (up->scatter_recv_count) ERRMEM (up->scatter_recv = malloc (sizeof (ID_R_PAIR [up->scatter_recv_count])));

  /* create return set */
  up->uni = NULL;
  if (in [1] == out [1])
  {
    for (ptr = up->recv, end = ptr + up->nrecv; ptr < end; ptr ++)
      for (item = SET_First (ptr->o); item; item = SET_Next (item))
	SET_Insert (&ldy->setmem, &up->uni, item->data, NULL);
    for (item = SET_First (inp); item; item = SET_Next (item))
      SET_Insert (&ldy->setmem, &up->uni, item->data, NULL);
  }

  /* clean up */
  MAP_Free (&ldy->mapmem, &idtodia);
  MAP_Free (&ldy->mapmem, &idtoext);

  *pattern = up;
  return up->uni;
}

/* gather reactions from other processors */
void LOCDYN_Union_Gather (void *pattern)
{
  UNION_PATTERN *up = pattern;
  ID_R_PAIR *IDR;
  XR *REXT, *x;
  double **R;
  int i;

  /* update gather send buffer */
  for (R = up->gather_R, IDR = up->gather_send, i = 0; i < up->gather_send_count; i ++, R ++, IDR ++)
  {
    COPY (*R, IDR->R);
  }

  /* gather reactions */
  MPI_Gatherv (up->gather_send, up->gather_send_count, up->id_r_type,
               up->gather_recv, up->gather_recv_counts, up->gather_recv_disps,
	       up->id_r_type, up->root, MPI_COMM_WORLD);
#if DEBUG
  for (XR *x = up->REXT, *end = x + up->REXT_count; x < end; x ++) x->done = 0;
#endif

  /* update REXT storage from gathered buffers */
  for (REXT = up->REXT, IDR = up->gather_recv, i = 0; i < up->gather_count; i ++, IDR ++)
  {
    ASSERT_DEBUG (IDR->id >= 0 && IDR->id < up->REXT_count, "REXT index out of bounds");
    x = &REXT [IDR->id];
    COPY (IDR->R, x->R);
#if DEBUG
    ASSERT_DEBUG (x->done == 0, "Double update of REXT item");
    x->done = 1;
#endif
  }

#if DEBUG
  int alldone = 1;

  for (XR *x = up->REXT, *end = x + up->REXT_count; x < end; x ++)
    if (!x->done) { alldone = 0; break; }

  ASSERT_DEBUG (alldone, "All external reactions should be done");
#endif
}

/* scatter reactions to other processors */
void LOCDYN_Union_Scatter (void *pattern)
{
  UNION_PATTERN *up = pattern;
  LOCDYN *ldy = up->ldy;
  ID_R_PAIR *IDR;
  XR *REXT, *x;
  double **R;
  DIAB *dia;
  CON *con;
  int i;

  /* update scatter send buffer */
  for (R = up->scatter_R, IDR = up->scatter_send, i = 0; i < up->scatter_count; i ++, R ++, IDR ++)
  {
    COPY (*R, IDR->R);
  }

  /* scatter reactions */
  MPI_Scatterv (up->scatter_send, up->scatter_send_counts, up->scatter_send_disps,
                up->id_r_type, up->scatter_recv, up->scatter_recv_count,
		up->id_r_type, up->root, MPI_COMM_WORLD);

  /* update local REXT storage and diagonal blocks */
  for (REXT = ldy->REXT, IDR = up->scatter_recv, i = 0; i < up->scatter_recv_count; i ++, IDR ++)
  {
    if (IDR->id >= 0) /* REXT */
    {
      ASSERT_DEBUG (IDR->id >= 0 && IDR->id < ldy->REXT_count, "REXT index out of bounds");
      x = &REXT [IDR->id];
      COPY (IDR->R, x->R);
#if DEBUG
    ASSERT_DEBUG (x->done == 0, "Double update of REXT item");
#endif
      x->done = 1;
    }
    else /* block */
    {
      double *Q = NULL;

      if (ldy->ldb == LDB_OFF)
      {
	if ((con = MAP_Find (DOM(ldy->dom)->idc, (void*) (long) (-IDR->id), NULL))) Q = con->R;
      }
      else
      {
	if ((dia = MAP_Find (ldy->idbb, (void*) (long) (-IDR->id), NULL))) Q = dia->R;
      }

      ASSERT_DEBUG (Q, "Invalid block identifier");
      COPY (IDR->R, Q);
    }
  }
}

/* release memory used by the union set */
void LOCDYN_Union_Destroy (void *pattern)
{
  UNION_PATTERN *up = pattern;
  LOCDYN *ldy = up->ldy;

  COMOBJ *ptr, *end;

  for (ptr = up->recv, end = ptr + up->nrecv; ptr < end; ptr ++) /* free received blocks data */
  {
    for (SET *item = SET_First (ptr->o); item; item = SET_Next (item))
    {
      OFFB *b, *n;
      DIAB *dia;

      for (dia = item->data, b = dia->adj; b; b = n) /* off-diagonal */
      {
	n = b->n;
	MEM_Free (&ldy->offmem, b);
      }

      MAP_Free (&ldy->mapmem, &dia->children); /* children */
      MEM_Free (&ldy->diamem, dia); /* diagonal */
    }

    SET_Free (&ldy->setmem, (SET**) ptr->o); /* the whole set */
  }

  free (up->recv);
  free (up->REXT);

  /* gather and scatter */
  free (up->gather_R);
  free (up->gather_send);
  free (up->gather_recv);
  free (up->gather_recv_counts);
  free (up->gather_recv_disps);
  free (up->scatter_R);
  free (up->scatter_send);
  free (up->scatter_send_counts);
  free (up->scatter_send_disps);
  free (up->scatter_recv);
  MPI_Type_free (&up->id_r_type);

  /* the union set */
  SET_Free (&ldy->setmem, &up->uni);

  free (up);
}
#endif

/* set an approach to the linearisation of local dynamics */
void LOCDYN_Approach (LOCDYN *ldy, LOCDYN_APPROACH approach)
{
  /* TODO */
}

/* assemble tangent operator */
void LOCDYN_Tangent (LOCDYN *ldy)
{
  /* TODO */
}

/* compute merit function */
double LOCDYN_Merit (LOCDYN *ldy)
{
  return 0.0; /* TODO */
}

/* free memory */
void LOCDYN_Destroy (LOCDYN *ldy)
{
  MEM_Release (&ldy->diamem);
  MEM_Release (&ldy->offmem);

#if MPI
  destroy_mpi (ldy);
#endif

  free (ldy);
}
