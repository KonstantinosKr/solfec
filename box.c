/*
 * box.c
 * Copyright (C) 2008, Tomasz Koziara (t.koziara AT gmail.com)
 * --------------------------------------------------------------
 * axis aligned bounding box overlap detection
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

#include "sol.h"
#include "box.h"
#include "hyb.h"
#include "alg.h"
#include "msh.h"
#include "cvx.h"
#include "sph.h"
#include "bod.h"
#include "swp.h"
#include "hsh.h"
#include "err.h"

#define SIZE 128 /* mempool size */

/* auxiliary data */
struct auxdata
{
  SET *nobody;
  SET *nogobj;
  MEM *mapmem;
  void *data;
  BOX_Overlap_Create create;
};

/* body pair comparison */
static int bodcmp (OPR *a, OPR *b)
{
  if (a->bod1 < b->bod1) return -1;
  else if (a->bod1 == b->bod1 && a->bod2 < b->bod2) return -1;
  else if (a->bod1 == b->bod1 && a->bod2 == b->bod2) return 0;
  else return 1;
}

/* geomtric object pair comparison */
static int gobjcmp (OPR *a, OPR *b)
{
  int cmp = bodcmp (a, b);

  if (cmp == 0)
  {
    if (a->sgp1 < b->sgp1) return -1;
    else if (a->sgp1 == b->sgp1 && a->sgp2 < b->sgp2) return -1;
    else if (a->sgp1 == b->sgp1 && a->sgp2 == b->sgp2) return 0;
    else return 1;
  }
  else return cmp;
}

/* local overlap creation callback => filters our unwnated adjacency */
static void* local_create (struct auxdata *aux, BOX *one, BOX *two)
{
  BODY *onebod = one->body, *twobod = two->body;

#if MPI
  if ((onebod->flags & BODY_CHILD) && (twobod->flags & BODY_CHILD)) return NULL; /* only parent-parent and parent-child contacts are valid */
  else if (onebod->flags & BODY_CHILD)
  {
    if (SET_Contains (twobod->children, (void*) (long) onebod->rank, NULL)) /* check whether the parent's body child and child's parent are on the same processor */
    {
      if (onebod->dom->rank < onebod->rank) return NULL; /* if so, use processor ordering in order to avoid duplicated contact detection */
    }
  }
  else if (twobod->flags & BODY_CHILD)
  {
    if (SET_Contains (onebod->children, (void*) (long) twobod->rank, NULL))
    {
      if (twobod->dom->rank < twobod->rank) return NULL;
    }
  }
#endif

  /* check if these are two obstacles => no need to report overlap */
  if (onebod->kind == OBS && twobod->kind == OBS) return NULL;

  /* boxes already adjacent ? */
  if (MAP_Find (one->adj, two, NULL)) return NULL;

  /* self-contact ? <=> one->body == two->body */
  if (onebod == twobod && !(onebod->flags & BODY_DETECT_SELF_CONTACT)) return NULL;

  int id1 = onebod->id,
      id2 = twobod->id,
      no1 = one->sgp - onebod->sgp,
      no2 = two->sgp - twobod->sgp;

  OPR pair = {MIN (id1, id2), MAX (id1, id2), MIN (no1, no2), MAX (no1, no2)};

  /* an excluded body pair ? */
  if (SET_Contains (aux->nobody, &pair, (SET_Compare) bodcmp)) return NULL;

  /* an excluded object pair ? */
  if (SET_Contains (aux->nogobj, &pair, (SET_Compare) gobjcmp)) return NULL;

  /* test topological adjacency */
  switch (GOBJ_Pair_Code (one, two))
  {
    case AABB_ELEMENT_ELEMENT:
    {
      if (onebod == twobod) /* assuming only one mesh per body is possible,
			       the two elements are within the same mesh;
			       exclude topologically adjacent elements */
	if (ELEMENT_Adjacent (one->sgp->gobj, two->sgp->gobj)) return NULL;
    }
    break;
    case AABB_CONVEX_CONVEX:
    {
      if (onebod == twobod) /* exclude topologically adjacent convices */
	if (CONVEX_Adjacent (one->sgp->gobj, two->sgp->gobj)) return NULL;
    }
    break;
    case AABB_SPHERE_SPHERE:
    {
      if (onebod == twobod) /* exclude topologically adjacent spheres */
	if (SPHERE_Adjacent (one->sgp->gobj, two->sgp->gobj)) return NULL;
    }
  }
 
  /* report overlap creation and get the user pointer */
  void *user = aux->create (aux->data, one, two);

  if (user) /* if the user pointer is NULL keep re-detecting the overlap */
  {
    MAP_Insert (aux->mapmem, &one->adj, two, user, NULL); /* map overlap symmetrically */
    MAP_Insert (aux->mapmem, &two->adj, one, user, NULL);
    /* one might think that only one adjacency (say box with bigger pointer
     * stores box with smaller pointer) could be used. nevertheless, for box
     * removal one needs to release all overlaps, and then the complete adjacency
     * informaltion is necessary; thus the symmetrical mapping */
  }

  return user;
}

/* get geometrical object kind */
static int gobj_kind (SGP *sgp)
{
  switch (sgp->shp->kind)
  {
  case SHAPE_MESH: return GOBJ_ELEMENT;
  case SHAPE_CONVEX: return GOBJ_CONVEX;
  case SHAPE_SPHERE: return GOBJ_SPHERE;
  }

  ASSERT_DEBUG (0, "Invalid shape kind in gobj_kind");

  return 0;
}

#if MPI
/* number of boxes */
static int box_count (AABB *aabb, int *ierr)
{
  *ierr = ZOLTAN_OK;

  return aabb->nin + aabb->nlst;
}

/* list of box identifiers */
static void box_list (AABB *aabb, int num_gid_entries, int num_lid_entries,
  ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR local_ids, int wgt_dim, float *obj_wgts, int *ierr)
{
  BOX *box, **aux;
  BODY *bod;
  int i;

  free (aabb->aux);
  /* realloc auxiliary table */
  ERRMEM (aabb->aux = malloc ((aabb->nin + aabb->nlst) * sizeof (BOX*)));

  /* gather inserted boxes */
  for (aux = aabb->aux, i = 0, box = aabb->in; box; aux ++, i ++, box = box->next)
  {
    bod = box->body;

    global_ids [i * num_gid_entries] = bod->id;
    global_ids [i * num_gid_entries + 1] = box->sgp - bod->sgp; /* local SGP index */

    local_ids [i * num_lid_entries] = i;

    *aux = box;
  }

  /* gather current boxes */
  for (box = aabb->lst; box; aux ++, i ++, box = box->next)
  {
    bod = box->body;

    global_ids [i * num_gid_entries] = bod->id;
    global_ids [i * num_gid_entries + 1] = box->sgp - bod->sgp;

    local_ids [i * num_lid_entries] = i;

    *aux = box;
  }

  *ierr = ZOLTAN_OK;
}

/* number of spatial dimensions */
static int dimensions (AABB *aabb, int *ierr)
{
  *ierr = ZOLTAN_OK;
  return 3;
}

/* list of body extent low points */
static void boxpoints (AABB *aabb, int num_gid_entries, int num_lid_entries, int num_obj,
  ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR local_ids, int num_dim, double *geom_vec, int *ierr)
{
  BOX **aux, *box;
  int i;

  aux = aabb->aux;

  for (i = 0; i < num_obj; i ++, geom_vec += num_dim)
  {
    box = aux [local_ids [i * num_lid_entries]];
    MID (box->extents, box->extents + 3, geom_vec);
  }

  *ierr = ZOLTAN_OK;
}

/* create MPI related data */
static void create_mpi (AABB *aabb)
{
  aabb->aux = NULL;

  /* zoltan context for body partitioning */
  ASSERT (aabb->zol = Zoltan_Create (MPI_COMM_WORLD), ERR_ZOLTAN);

  /* general parameters */
  Zoltan_Set_Param (aabb->zol, "DEBUG_LEVEL", "0");
  Zoltan_Set_Param (aabb->zol, "DEBUG_MEMORY", "0");
  Zoltan_Set_Param (aabb->zol, "NUM_GID_ENTRIES", "2"); /* body id, sgp index */
  Zoltan_Set_Param (aabb->zol, "NUM_LID_ENTRIES", "1"); /* indices in aux table */
 
  /* load balancing parameters */
  Zoltan_Set_Param (aabb->zol, "LB_METHOD", "RCB");
  Zoltan_Set_Param (aabb->zol, "IMBALANCE_TOL", "1.2");
  Zoltan_Set_Param (aabb->zol, "AUTO_MIGRATE", "FALSE"); /* we shall use COMOBJS */
  Zoltan_Set_Param (aabb->zol, "RETURN_LISTS", "NONE");

  /* RCB parameters */
  Zoltan_Set_Param (aabb->zol, "RCB_OVERALLOC", "1.3");
  Zoltan_Set_Param (aabb->zol, "RCB_REUSE", "1");
  Zoltan_Set_Param (aabb->zol, "RCB_OUTPUT_LEVEL", "0");
  Zoltan_Set_Param (aabb->zol, "CHECK_GEOM", "1");
  Zoltan_Set_Param (aabb->zol, "KEEP_CUTS", "1");

  /* callbacks */
  Zoltan_Set_Fn (aabb->zol, ZOLTAN_NUM_OBJ_FN_TYPE, (void (*)()) box_count, aabb);
  Zoltan_Set_Fn (aabb->zol, ZOLTAN_OBJ_LIST_FN_TYPE, (void (*)()) box_list, aabb);
  Zoltan_Set_Fn (aabb->zol, ZOLTAN_NUM_GEOM_FN_TYPE, (void (*)()) dimensions, aabb);
  Zoltan_Set_Fn (aabb->zol, ZOLTAN_GEOM_MULTI_FN_TYPE, (void (*)()) boxpoints, aabb);
}

/* destroy MPI related data */
static void destroy_mpi (AABB *aabb)
{
  free (aabb->aux);

  Zoltan_Destroy (&aabb->zol);
}
#endif

/* algorithm name */
char* AABB_Algorithm_Name (BOXALG alg)
{
  switch (alg)
  {
  case SWEEP_HASH2D_LIST: return "SWEEP_HASH2D_LIST";
  case SWEEP_HASH2D_XYTREE: return "SWEEP_HASH2D_XYTREE";
  case SWEEP_XYTREE: return "SWEEP_XYTREE";
  case SWEEP_HASH1D_XYTREE: return "SWEEP_HASH1D_XYTREE";
  case HYBRID: return "HYBRID";
  case HASH3D: return "HASH3D";
  }

  return NULL;
}

/* create box overlap driver data */
AABB* AABB_Create (int size)
{
  AABB *aabb;

  ERRMEM (aabb = malloc (sizeof (AABB)));
  aabb->lst = aabb->in = aabb->out= NULL;
  aabb->tab = NULL;

  MEM_Init (&aabb->boxmem, sizeof (BOX), MIN (size, SIZE));
  MEM_Init (&aabb->mapmem, sizeof (MAP), MIN (size, SIZE));
  MEM_Init (&aabb->setmem, sizeof (SET), MIN (size, SIZE));
  MEM_Init (&aabb->oprmem, sizeof (OPR), MIN (size, SIZE));

  aabb->nobody = aabb->nogobj= NULL;

  aabb->nin = 0;
  aabb->nlst = 0;
  aabb->ntab = 0;
  aabb->modified = 0;
  aabb->swp = aabb->hsh = NULL;

#if MPI
  create_mpi (aabb);
#endif

  return aabb;
}

/* insert geometrical object */
BOX* AABB_Insert (AABB *aabb, BODY *body, GOBJ kind, SGP *sgp, void *data, BOX_Extents_Update update)
{
  BOX *box;

  ERRMEM (box = MEM_Alloc (&aabb->boxmem));
  box->data = data;
  box->update = update;
  box->kind = kind | GOBJ_NEW; /* mark as new */
  box->body = body;
  box->sgp = sgp;
  sgp->box = box;

  /* include into the
   * insertion list */
  box->next = aabb->in;
  if (aabb->in) aabb->in->prev= box;
  aabb->in = box;
  aabb->nin ++;

  /* set modified */
  aabb->modified = 1;

  return box;
}

/* delete an object */
void AABB_Delete (AABB *aabb, BOX *box)
{
  if (box->kind & GOBJ_NEW) /* still in the insertion list */
  {
    /* remove box from the insertion list */
    if (box->prev) box->prev->next = box->next;
    else aabb->in = box->next;
    if (box->next) box->next->prev = box->prev;
    aabb->nin --;

    /* free it here */
    MEM_Free (&aabb->boxmem, box); 
  }
  else
  {
    /* remove box from the current list */
    if (box->prev) box->prev->next = box->next;
    else aabb->lst = box->next;
    if (box->next) box->next->prev = box->prev;
    aabb->nlst --; /* decrease count */

    /* insert it into
     * the deletion list */
    box->next = aabb->out;
    aabb->out = box;
  }

  /* set modified */
  aabb->modified = 1;
}

/* insert a body */
void AABB_Insert_Body (AABB *aabb, BODY *body)
{
  SGP *sgp, *sgpe;
  BOX *box;

  for (sgp = body->sgp, sgpe = sgp + body->nsgp; sgp < sgpe; sgp ++)
  {
    box = AABB_Insert (aabb, body, gobj_kind (sgp), sgp, sgp->shp->data, SGP_Extents_Update (sgp));
    box->update (box->data, box->sgp->gobj, box->extents); /* initial update */
  }
}

/* delete a body */
void AABB_Delete_Body (AABB *aabb, BODY *body)
{
  SGP *sgp, *sgpe;

  for (sgp = body->sgp, sgpe = sgp + body->nsgp; sgp < sgpe; sgp ++)
  {
    AABB_Delete (aabb, sgp->box);
    sgp->box = NULL;
  }
}

/* update state => detect created and released overlaps */
void AABB_Update (AABB *aabb, BOXALG alg, void *data, BOX_Overlap_Create create)
{
  struct auxdata aux = {aabb->nobody, aabb->nogobj, &aabb->mapmem, data, create};
  BOX *box, *next, *adj;
  MAP *item;

#if MPI
  if (aabb->dom->rank == 0)
#endif
  if (aabb->dom && aabb->dom->verbose) printf ("CONDET (%s) ... ", AABB_Algorithm_Name (alg)), fflush (stdout);
  
  if (aabb->dom) SOLFEC_Timer_Start (aabb->dom->solfec, "CONDET");

  /* update extents */
  for (box = aabb->lst; box; box = box->next) /* for each current box */
    box->update (box->data, box->sgp->gobj, box->extents);

  for (box = aabb->in; box; box = box->next) /* for each inserted box */
  {
    box->update (box->data, box->sgp->gobj, box->extents);
    box->kind &= ~GOBJ_NEW; /* not new any more */
  }

  /* check for released overlaps */
  for (box = aabb->out; box; box = next) /* for each deleted box */
  {
    next = box->next;

    for (item = MAP_First (box->adj); item; item = MAP_Next (item)) /* for each adjacent box */
    {
      adj = item->key;
      MAP_Delete (&aabb->mapmem, &adj->adj, box, NULL); /* remove 'box' from 'adj's adjacency */
    }

    MAP_Free (&aabb->mapmem, &box->adj); /* free adjacency */
    MEM_Free (&aabb->boxmem, box); /* free box */
  }
  aabb->out = NULL; /* list emptied */

  if (aabb->modified) /* merge insertion and curent lists, update pointer table */
  {
    BOX **b;

    if (aabb->in)
    {
      for (box = aabb->in; box->next; box = box->next); /* rewind till the last inserted one */
      box->next = aabb->lst; /* link it with the current list */
      if (aabb->lst) aabb->lst->prev = box; /* set previous link */
      aabb->lst = aabb->in; /* list appended */
      aabb->nlst += aabb->nin; /* increase number of current boxes */
      aabb->in = NULL; /* list emptied */
      aabb->nin = 0;
      aabb->ntab = aabb->nlst; /* resize the pointer table */
      free (aabb->tab);
      ERRMEM (aabb->tab = malloc (sizeof (BOX*) * aabb->ntab));
    }
    else aabb->ntab = aabb->nlst; /* deleted boxes could decrement the counter */

    for (box = aabb->lst, b = aabb->tab; box; box = box->next, b ++) *b = box; /* overwrite box pointers */
  }

  /* regardless of the current algorithm notify sweep-plane about the change */
  if (aabb->modified && aabb->swp) SWEEP_Changed (aabb->swp);

  /* the algorithm
   * specific part */
  switch (alg)
  {
    case HYBRID:
    {
      hybrid (aabb->tab, aabb->ntab, &aux, (BOX_Overlap_Create)local_create);
    }
    break;
    case HASH3D:
    {
      if (!aabb->hsh) aabb->hsh = HASH_Create (aabb->ntab);

      HASH_Do (aabb->hsh, aabb->ntab, aabb->tab,
	       &aux, (BOX_Overlap_Create)local_create); 
    }
    break;
    case SWEEP_HASH2D_LIST:
    case SWEEP_HASH1D_XYTREE:
    case SWEEP_HASH2D_XYTREE:
    case SWEEP_XYTREE:
    {
      if (!aabb->swp) aabb->swp = SWEEP_Create (aabb->ntab, (DRALG)alg);

      SWEEP_Do (aabb->swp, (DRALG)alg, aabb->ntab, aabb->tab, &aux, (BOX_Overlap_Create)local_create); 
    }
    break;
  }

  /* set unmodified */
  aabb->modified = 0;

  if (aabb->dom)
  {
    DOM_Sparsify_Contacts (aabb->dom);

    SOLFEC_Timer_End (aabb->dom->solfec, "CONDET");
  }
}

/* never report overlaps betweem this pair of bodies (given by identifiers) */
void AABB_Exclude_Body_Pair (AABB *aabb, unsigned int id1, unsigned int id2)
{
  OPR *opr;
  
  ERRMEM (opr = MEM_Alloc (&aabb->oprmem));
  opr->bod1 = MIN (id1, id2);
  opr->bod2 = MAX (id1, id2);
  SET_Insert (&aabb->setmem, &aabb->nobody, opr, (SET_Compare) bodcmp);
}

/* never report overlaps betweem this pair of objects (bod1, sgp1), (bod1, sgp2) */
void AABB_Exclude_Gobj_Pair (AABB *aabb, unsigned int bod1, int sgp1, unsigned int bod2, int sgp2)
{
  OPR *opr;
  
  ERRMEM (opr = MEM_Alloc (&aabb->oprmem));
  opr->bod1 = MIN (bod1, bod2);
  opr->bod2 = MAX (bod1, bod2);
  opr->sgp1 = MIN (sgp1, sgp2);
  opr->sgp2 = MAX (sgp1, sgp2);
  SET_Insert (&aabb->setmem, &aabb->nogobj, opr, (SET_Compare) gobjcmp);
}

/* break box adjacency if boxes associated with the objects are adjacent */
void AABB_Break_Adjacency (AABB *aabb, BOX *one, BOX *two)
{
  /* mutually delete from adjacency lists */
  MAP_Delete (&aabb->mapmem, &one->adj, two, NULL);
  MAP_Delete (&aabb->mapmem, &two->adj, one, NULL);
}

/* release memory */
void AABB_Destroy (AABB *aabb)
{
  free (aabb->tab);

  MEM_Release (&aabb->boxmem);
  MEM_Release (&aabb->mapmem);
  MEM_Release (&aabb->setmem);
  MEM_Release (&aabb->oprmem);

  if (aabb->swp) SWEEP_Destroy (aabb->swp);
  if (aabb->hsh) HASH_Destroy (aabb->hsh);

#if MPI
  destroy_mpi (aabb);
#endif

  free (aabb);
}

#if MPI
/* boxes load balancing */
void AABB_Balance (AABB *aabb)
{
  int val = aabb->nin + aabb->nlst,
      sum, min, avg, max;

  /* get statistics on boxes counts */
  PUT_int_stats (1, &val, &sum, &min, &avg, &max);

  /* compute inbalance ratio for boxes */
  double ratio = (double) max / (double) MAX (min, 1);

  if (aabb->dom->time == 0.0 || ratio > aabb->dom->imbalance_tolerance) /* update partitioning only if not sufficient to balance boxes */
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

      /* update box partitioning using low points of their extents */
      ASSERT (Zoltan_LB_Balance (aabb->zol, &changes, &num_gid_entries, &num_lid_entries,
	      &num_import, &import_global_ids, &import_local_ids, &import_procs,
	      &num_export, &export_global_ids, &export_local_ids, &export_procs) == ZOLTAN_OK, ERR_ZOLTAN);

      Zoltan_LB_Free_Data (&import_global_ids, &import_local_ids, &import_procs,
			   &export_global_ids, &export_local_ids, &export_procs);
  }
}
#endif

/* get geometrical object extents update callback */
BOX_Extents_Update SGP_Extents_Update (SGP *sgp)
{
  switch (sgp->shp->kind)
  {
  case SHAPE_MESH: return (BOX_Extents_Update) ELEMENT_Extents;
  case SHAPE_CONVEX: return (BOX_Extents_Update) CONVEX_Extents;
  case SHAPE_SPHERE: return (BOX_Extents_Update) SPHERE_Extents;
  }

  ASSERT_DEBUG (0, "Invalid shape kind in SGP_Extents_Update");

  return 0;
}
