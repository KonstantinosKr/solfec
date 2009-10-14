/*
 * goc.c
 * Copyright (C) 2008, 2009 Tomasz Koziara (t.koziara AT gmail.com)
 * -------------------------------------------------------------------
 * geometric object contact detection
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

#include <float.h>
#include "alg.h"
#include "box.h"
#include "msh.h"
#include "cvx.h"
#include "sph.h"
#include "cvi.h"
#include "gjk.h"
#include "goc.h"
#include "err.h"

#define MAGNIFY 10.0 /* contact is lost if gap >= GEOMETRIC_EPSILON * MAGNIFY */

/* line-plane intersection => intersection = point + direction * coef if 1 is returned */
inline static int lineplane (double *plane, double *point, double *direction, double *coef)
{
  double d;
 
  d = DOT (plane, direction); 

  if (fabs (d) < GEOMETRIC_EPSILON) return 0;

  *coef = - PLANE (plane, point) / d;

  return 1;
}

/* compute semi-negative penetration gap from surface triangles */
inline static double convex_convex_gap (TRI *tri, int m, double *point, double *normal)
{
  double plane [4],
         min = DBL_MAX,
         max = -min,
         coef;

  for (; m > 0; tri ++, m --)
  {
    COPY (tri->out, plane);
    plane [3] = - DOT (plane, tri->ver [0]);

    if (lineplane (plane, point, normal, &coef))
    {
      if (coef < min) min = coef;
      if (coef > max) max = coef;
    }
  }

  return (min < 0 && max > 0 ? min - max : 0);
}

/* compute semi-negative convex-sphere gap */
inline static double convex_sphere_gap (double *pla, int npla, double *center, double radius, double *normal)
{
  double plane [4],
         max = -DBL_MAX,
         coef;

  for (; npla > 0; pla += 6, npla --)
  {
    COPY (pla, plane);
    plane [3] = - DOT (plane, pla+3);

    if (lineplane (plane, center, normal, &coef))
    {
      if (coef > max) max = coef;
    }
  }

  return (max > radius ? radius - max : 0);
}

/* compute semi-negative sphere-sphere gap */
inline static double sphere_sphere_gap (double *ca, double ra, double *cb, double rb, double *normal)
{
  double x [3], d, e;

  SUB (cb, ca, x);
  d = DOT (x, normal);
  e = ra + rb;

  return (e > d ? d - e : 0);
}

/* return plane normal nearest to a given direction */
inline static double* nearest_normal (double *direction, double *pla, int n)
{
  double d, max = -DBL_MAX, *ret = pla;

  for (; n > 0; n --, pla += 6)
  {
    d = DOT (direction, pla);

    if (d > max)
    {
      max = d;
      ret = pla;
    }
  }

  return ret;
}

/* return a surface code with the input point closest to an input plane */
inline static int nearest_surface (double *pnt, double *pla, int *sur, int n)
{
  double v [3], d, min = DBL_MAX;
  int ret = sur [0];

  for (; n > 0; n --, pla += 6, sur ++)
  {
    SUB (pnt, pla+3, v);
    d = DOT (pla, v);

    if (fabs (d) < min)
    {
      min = d;
      ret = *sur;
    }
  }

  return ret;
}

/* compute average point and resultant normal; return normal variance */
inline static double point_and_normal (int negative, TRI *tri, int m,
    int *surf, double *point, double *normal, double *area, int *sout)
{
  double a, v [3], dots, max;
  TRI *t, *e;

  max = -DBL_MAX;
  SET (v, 0.0);
  SET (point, 0.0);
  SET (normal, 0.0);
  *area = 0.0;
  dots = 0.0;

  for (t = tri, e = t + m; t != e; t ++)
  {
    if ((negative && t->flg < 0) ||
	(!negative && t->flg > 0))
    {
      TRIANGLE_AREA (t->ver[0], t->ver[1], t->ver[2], a);
      SCALE (t->out, a); /* scale here and reuse there (#) */
      ADD (normal, t->out, normal); /* resultant normal */
      MID3 (t->ver[0], t->ver[1], t->ver[2], v); /* triangle midpoint */
      ADDMUL (point, a, v, point); /* integrate */
      *area += a;

      if (a > max)
      {
	(*sout) = surf [ABS (t->flg) - 1]; /* identifier of surface with maximal area */
	max = a;
      }
    }
  }
  DIV (point, *area, point); /* surface mass centere */

  for (t = tri, e = t + m; t != e; t ++) /* compute normal variance */
  {
    if ((negative && t->flg < 0) ||
	(!negative && t->flg > 0))
    {
      SUB (t->out, normal, v); /* (#) scaling reused here */
      dots += DOT (v, v);
    }
  }

  return dots;
}

/* detect contact between two convex polyhedrons 'a' and 'b', where
 * va, nva, vb, nbv: vertices and vertex counts
 * pa, npa, pb, npb: 6-coord planes (normal, point) and plane counts
 * sa, nsa, sb, nsb: surface identifiers and surface planes counts
 *                   (the first nsa, nsb planes are on the surface)
 * ----------------------------------------------------------------
 * ramaining paramteres behave like in gobjcontact routine 
 */
static int detect_convex_convex (
  double *va, int nva, double *pa, int npa, int *sa, int nsa,
  double *vb, int nvb, double *pb, int npb, int *sb, int nsb,
  double onepnt [3],
  double twopnt [3],
  double normal [3],
  double *gap,
  double *area,
  int spair [2])
{
  double a, b, an [3], bn [3], ap [3], bp [3], aa, ba;
  TRI *tri;
  int m;

  *gap = gjk (va, nva, vb, nvb, onepnt, twopnt);

  if (*gap < GEOMETRIC_EPSILON)
  {
    if (!(tri = cvi (va, nva, pa, npa, vb, nvb, pb, npb, &m))) return 0;

    a = point_and_normal (0, tri, m, sa, ap, an, &aa, &spair [0]);
    b = point_and_normal (1, tri, m, sb, bp, bn, &ba, &spair [1]);

    if (a < b)
    {
      NORMALIZE (an);
      COPY (an, normal);
      COPY (ap, onepnt);
      COPY (ap, twopnt);
      *area = aa;
      *gap = convex_convex_gap (tri, m, ap, an);
      free (tri);
      return 1;
    }
    else
    {
      NORMALIZE (bn);
      COPY (bn, normal);
      COPY (bp, onepnt);
      COPY (bp, twopnt);
      *area = ba;
      *gap = convex_convex_gap (tri, m, bp, bn);
      free (tri);
      return 2;
    }
  }

  return 0;
}

/* detect contact between a convex and a sphere */
static int detect_convex_sphere (
  double *vc, int nvc, double *pc, int npc, int *sc, int nsc, /* same as above */
  double *c, double r, int s, /* center, radius, surface */
  double onepnt [3],
  double twopnt [3],
  double normal [3],
  double *gap,
  double *area,
  int spair [2])
{
  double dir [3], g, h, *nn;

  if ((g = gjk_convex_sphere (vc, nvc, c, r, onepnt, twopnt)) < GEOMETRIC_EPSILON)
  {
    for (h = r; g < GEOMETRIC_EPSILON && h > GEOMETRIC_EPSILON; h *= 0.5)
      g = gjk_convex_sphere (vc, nvc, c, h, onepnt, twopnt); /* shrink sphere and redo => find projection
								of the sphere center on the convex surface */

    SUB (c, onepnt, dir);
    NORMALIZE (dir);
    nn = nearest_normal (dir, pc, npc);
    COPY (nn, normal);

    ADDMUL (c, -r, normal, twopnt);

    spair [0] = nearest_surface (onepnt, pc, sc, nsc);
    spair [1] = s;
    *area = 1.0;
    *gap = convex_sphere_gap (pc, npc, c, r, normal);
    return 1;
  }

  return 0;
}

/* detect contact between spheres 'a' and 'b' */
static int detect_sphere_sphere (
  double *ca, double ra, int sa, /* center, radius, surface */
  double *cb, double rb, int sb,
  double onepnt [3],
  double twopnt [3],
  double normal [3],
  double *gap,
  double *area,
  int spair [2])
{
  if (((*gap) = gjk_sphere_sphere (ca, ra, cb, rb, onepnt, twopnt)) < GEOMETRIC_EPSILON)
  {
    SUB (onepnt, ca, normal);
    NORMALIZE (normal);
    spair [0] = sa;
    spair [1] = sb;
    *area = 1.0;
    *gap = sphere_sphere_gap (ca, ra, cb, rb, normal);
    return 1;
  }

  return 0;
}

/* update contact between two convex polyhedrons 'a' and 'b', where
 * va, nva, vb, nbv: vertices and vertex counts
 * pa, npa, pb, npb: 6-coord planes (normal, point) and plane counts
 * sa, nsa, sb, nsb: surface identifiers and surface planes counts
 *                   (the first nsa, nsb planes are on the surface)
 * ----------------------------------------------------------------
 * ramaining paramteres behave like in gobjcontact routine 
 */
static int update_convex_convex (
  double *va, int nva, double *pa, int npa, int *sa, int nsa,
  double *vb, int nvb, double *pb, int npb, int *sb, int nsb,
  double onepnt [3],
  double twopnt [3],
  double normal [3], /* outward with restpect to the 'a' body (master) */
  double *gap,
  double *area,
  int spair [2])
{
  double a, b, an [3], bn [3], ap [3], bp [3], aa, ba;
  int s [2];
  TRI *tri;
  int m;

  *gap = gjk (va, nva, vb, nvb, onepnt, twopnt);

  if (*gap < GEOMETRIC_EPSILON)
  {
    if (!(tri = cvi (va, nva, pa, npa, vb, nvb, pb, npb, &m))) return 0;

    s [0] = spair [0];
    s [1] = spair [1];

    a = point_and_normal (0, tri, m, sa, ap, an, &aa, &spair [0]);
    b = point_and_normal (1, tri, m, sb, bp, bn, &ba, &spair [1]);

    NORMALIZE (an);
    COPY (an, normal);
    COPY (ap, onepnt);
    COPY (ap, twopnt);
    *area = aa;
    *gap = convex_convex_gap (tri, m, ap, an);
    free (tri);

    if (s [0] == spair [0] &&
	s [1] == spair [1]) return 1;
    else return 2;
  }
  else if (*gap < GEOMETRIC_EPSILON * MAGNIFY)
  {
    SUB (twopnt, onepnt, normal);
    NORMALIZE (normal);

    s [0] = spair [0];
    s [1] = spair [1];

    spair [0] = nearest_surface (onepnt, pa, sa, nsa);
    spair [1] = nearest_surface (onepnt, pb, sb, nsb);

    if (s [0] == spair [0] &&
	s [1] == spair [1]) return 1;
    else return 2;
  }

  return 0;
}

/* update contact between a convex and a sphere */
static int update_convex_sphere (
  double *vc, int nvc, double *pc, int npc, int *sc, int nsc, /* same as above */
  double *c, double r, int s, /* center, radius, surface */
  double onepnt [3],
  double twopnt [3],
  double normal [3],
  double *gap,
  double *area,
  int spair [2])
{
  double dir [3], g, h, *nn;
  int s0;

  if (((*gap) = gjk_convex_sphere (vc, nvc, c, r, onepnt, twopnt)) < GEOMETRIC_EPSILON * MAGNIFY)
  {
    for (h = r, g = *gap; g < GEOMETRIC_EPSILON * MAGNIFY && h > GEOMETRIC_EPSILON; h *= 0.5)
      g = gjk_convex_sphere (vc, nvc, c, h, onepnt, twopnt); /* shrink sphere and redo => find projection
								of the sphere center on the convex surface */
    SUB (c, onepnt, dir);
    NORMALIZE (dir);
    nn = nearest_normal (dir, pc, npc);
    COPY (nn, normal);

    ADDMUL (c, -r, normal, twopnt);

    s0 = spair [0];
    spair [0] = nearest_surface (onepnt, pc, sc, nsc);
    if ((*gap) < GEOMETRIC_EPSILON)
      *gap = convex_sphere_gap (pc, npc, c, r, normal);

    if (s0 == spair [0]) return 1;
    else return 2;
  }

  return 0;
}

/* update contact between spheres 'a' and 'b' */
static int update_sphere_sphere (
  double *ca, double ra, int sa, /* center, radius, surface */
  double *cb, double rb, int sb,
  double onepnt [3],
  double twopnt [3],
  double normal [3],
  double *gap,
  double *area,
  int spair [2])
{
  if (((*gap) = gjk_sphere_sphere (ca, ra, cb, rb, onepnt, twopnt)) < GEOMETRIC_EPSILON * MAGNIFY)
  {
    SUB (onepnt, ca, normal);
    NORMALIZE (normal);
    if ((*gap) < GEOMETRIC_EPSILON)
      *gap = sphere_sphere_gap (ca, ra, cb, rb, normal);
    return 1;
  }

  return 0;
}

/* initialize convex a convex representation */
inline static void convex_init (CONVEX *cvx, double **v, int *nv, double **p, int *np, int **s, int *ns)
{
  double *pla, *q, max;
  int i, j ,k;

  *v = cvx->cur;
  *nv = cvx->nver;
  *np = cvx->nfac;
  *s = cvx->surface;
  *ns = cvx->nfac;

  ERRMEM ((*p) = malloc (cvx->nfac * sizeof (double [6])));

  for (i = 0, pla = cvx->pla, q = *p; i < cvx->nfac; i ++, pla += 4, q += 6)
  {
    COPY (pla, q);

    for (max = fabs (pla [0]), k = 0, j = 1; j < 3; j ++)
    {
      if (fabs (pla [j]) > max) /* get maximal absolute normal coordinate */
      {
	max = fabs (pla [j]);
	k = j;
      }
    }

    q [3] = q [4] = q [5] = 0.0;
    q [3+k]  = -pla [3] / pla [k];
  }
}

/* finalize a convex representation */
inline static void convex_done (CONVEX *cvx, double **v, int *nv, double **p, int *np, int **s, int *ns)
{
  free (*p);
}

/* swap surface pairs */
inline static void swap (int spair [2])
{
  int tmp = spair [0];

  spair [0] = spair [1];
  spair [1] = tmp;
}

/* swap back surface pair and swap renturn value */
inline static int detect_swap (int ret, int spair [2])
{
  int tmp = spair [0];

  spair [0] = spair [1];
  spair [1] = tmp;

  return (ret == 1 ? 2 : (ret == 2 ? 1 : 0));
}

/* swap back surface pair and copy return */
inline static int update_swap (int ret, int spair [2])
{
  int tmp = spair [0];

  spair [0] = spair [1];
  spair [1] = tmp;

  return ret;
}

/* detect contact */
static int detect (
    short paircode,
    SHAPE *oneshp, void *onegobj,
    SHAPE *twoshp, void *twogobj,
    double onepnt [3],
    double twopnt [3],
    double normal [3],
    double *gap,
    double *area,
    int spair [2])
{
  switch (paircode)
  {
    case AABB_ELEMENT_ELEMENT:
    {
      double va [24], pa [36],
             vb [24], pb [36];
      int nva, npa, sa [6], nsa,
	  nvb, npb, sb [6], nsb;

      nva = ELEMENT_Vertices (oneshp->data, onegobj, va);
      npa = ELEMENT_Planes (oneshp->data, onegobj, pa, sa, &nsa);
      nvb = ELEMENT_Vertices (twoshp->data, twogobj, vb);
      npb = ELEMENT_Planes (twoshp->data, twogobj, pb, sb, &nsb);

      return detect_convex_convex (va, nva, pa, npa, sa, nsa,
                                   vb, nvb, pb, npb, sb, nsb,
                                   onepnt, twopnt, normal,
				   gap, area, spair);
    }
    break;
    case AABB_CONVEX_CONVEX:
    {
      double *va, *pa,
             *vb, *pb;
      int nva, npa, *sa, nsa,
	  nvb, npb, *sb, nsb,
	  ret;

      convex_init (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);
      convex_init (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      ret = detect_convex_convex (va, nva, pa, npa, sa, nsa,
                                  vb, nvb, pb, npb, sb, nsb,
                                  onepnt, twopnt, normal,
				  gap, area, spair);

      convex_done (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);
      convex_done (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      return ret;
    }
    break;
    case AABB_SPHERE_SPHERE:
    {
      SPHERE *a = onegobj,
	     *b = twogobj;

      return detect_sphere_sphere (a->cur_center, a->cur_radius, a->surface,
                                   b->cur_center, b->cur_radius, b->surface,
                                   onepnt, twopnt, normal,
				   gap, area, spair);
    }
    break;
    case AABB_ELEMENT_CONVEX:
    {
      double va [24], pa [36],
             *vb, *pb;
      int nva, npa, sa [6], nsa,
	  nvb, npb, *sb, nsb,
	  ret;

      nva = ELEMENT_Vertices (oneshp->data, onegobj, va);
      npa = ELEMENT_Planes (oneshp->data, onegobj, pa, sa, &nsa);

      convex_init (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      ret = detect_convex_convex (va, nva, pa, npa, sa, nsa,
                                  vb, nvb, pb, npb, sb, nsb,
                                  onepnt, twopnt, normal,
				  gap, area, spair);

      convex_done (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      return ret;
    }
    break;
    case AABB_CONVEX_ELEMENT:
    {
      double *va, *pa,
             vb [24], pb [36];
      int nva, npa, *sa, nsa,
	  nvb, npb, sb [6], nsb,
	  ret;

      convex_init (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);

      nvb = ELEMENT_Vertices (twoshp->data, twogobj, vb);
      npb = ELEMENT_Planes (twoshp->data, twogobj, pb, sb, &nsb);

      ret = detect_convex_convex (va, nva, pa, npa, sa, nsa,
                                  vb, nvb, pb, npb, sb, nsb,
                                  onepnt, twopnt, normal,
				  gap, area, spair);

      convex_done (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);

      return ret;
    }
    break;
    case AABB_ELEMENT_SPHERE:
    {
      double va [24], pa [36];
      int nva, npa, sa [6], nsa;
      SPHERE *b = twogobj;

      nva = ELEMENT_Vertices (oneshp->data, onegobj, va);
      npa = ELEMENT_Planes (oneshp->data, onegobj, pa, sa, &nsa);

      return detect_convex_sphere (va, nva, pa, npa, sa, nsa,
                                   b->cur_center, b->cur_radius, b->surface,
                                   onepnt, twopnt, normal,
				   gap, area, spair);
    }
    break;
    case AABB_SPHERE_ELEMENT:
    {
      SPHERE *a = onegobj;
      double vb [24], pb [36];
      int nvb, npb, sb [6], nsb,
	  ret;

      nvb = ELEMENT_Vertices (twoshp->data, twogobj, vb);
      npb = ELEMENT_Planes (twoshp->data, twogobj, pb, sb, &nsb);

      swap (spair);

      ret = detect_convex_sphere (vb, nvb, pb, npb, sb, nsb,
                                  a->cur_center, a->cur_radius, a->surface,
                                  twopnt, onepnt, normal,
				  gap, area, spair);

      return detect_swap (ret, spair);

    }
    break;
    case AABB_CONVEX_SPHERE:
    {
      double *va, *pa;
      SPHERE *b = twogobj;
      int nva, npa, *sa, nsa,
	  ret;

      convex_init (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);

      ret = detect_convex_sphere (va, nva, pa, npa, sa, nsa,
                                  b->cur_center, b->cur_radius, b->surface,
                                  onepnt, twopnt, normal,
				  gap, area, spair);

      convex_done (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);

      return ret;
    }
    break;
    case AABB_SPHERE_CONVEX:
    {
      SPHERE *a = onegobj;
      double *vb, *pb;
      int nvb, npb, *sb, nsb,
	  ret;

      convex_init (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      swap (spair);

      ret = detect_convex_sphere (vb, nvb, pb, npb, sb, nsb,
                                  a->cur_center, a->cur_radius, a->surface,
                                  twopnt, onepnt, normal,
				  gap, area, spair);

      convex_done (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      return detect_swap (ret, spair);
    }
    break;
  }

  return 0;
}

/* update contact */
static int update (
    short paircode,
    SHAPE *oneshp, void *onegobj,
    SHAPE *twoshp, void *twogobj,
    double onepnt [3],
    double twopnt [3],
    double normal [3],
    double *gap,
    double *area,
    int spair [2])
{
  switch (paircode)
  {
    case AABB_ELEMENT_ELEMENT:
    {
      double va [24], pa [36],
             vb [24], pb [36];
      int nva, npa, sa [6], nsa,
	  nvb, npb, sb [6], nsb;

      nva = ELEMENT_Vertices (oneshp->data, onegobj, va);
      npa = ELEMENT_Planes (oneshp->data, onegobj, pa, sa, &nsa);
      nvb = ELEMENT_Vertices (twoshp->data, twogobj, vb);
      npb = ELEMENT_Planes (twoshp->data, twogobj, pb, sb, &nsb);

      return update_convex_convex (va, nva, pa, npa, sa, nsa,
                                   vb, nvb, pb, npb, sb, nsb,
                                   onepnt, twopnt, normal,
				   gap, area, spair);
    }
    break;
    case AABB_CONVEX_CONVEX:
    {
      double *va, *pa,
             *vb, *pb;
      int nva, npa, *sa, nsa,
	  nvb, npb, *sb, nsb,
	  ret;

      convex_init (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);
      convex_init (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      ret = update_convex_convex (va, nva, pa, npa, sa, nsa,
                                  vb, nvb, pb, npb, sb, nsb,
                                  onepnt, twopnt, normal,
				  gap, area, spair);

      convex_done (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);
      convex_done (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      return ret;
    }
    break;
    case AABB_SPHERE_SPHERE:
    {
      SPHERE *a = onegobj,
	     *b = twogobj;

      return update_sphere_sphere (a->cur_center, a->cur_radius, a->surface,
                                   b->cur_center, b->cur_radius, b->surface,
                                   onepnt, twopnt, normal,
				   gap, area, spair);
    }
    break;
    case AABB_ELEMENT_CONVEX:
    {
      double va [24], pa [36],
             *vb, *pb;
      int nva, npa, sa [6], nsa,
	  nvb, npb, *sb, nsb,
	  ret;

      nva = ELEMENT_Vertices (oneshp->data, onegobj, va);
      npa = ELEMENT_Planes (oneshp->data, onegobj, pa, sa, &nsa);

      convex_init (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      ret = update_convex_convex (va, nva, pa, npa, sa, nsa,
                                  vb, nvb, pb, npb, sb, nsb,
                                  onepnt, twopnt, normal,
				  gap, area, spair);

      convex_done (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      return ret;
    }
    break;
    case AABB_CONVEX_ELEMENT:
    {
      double *va, *pa,
             vb [24], pb [36];
      int nva, npa, *sa, nsa,
	  nvb, npb, sb [6], nsb,
	  ret;

      convex_init (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);

      nvb = ELEMENT_Vertices (twoshp->data, twogobj, vb);
      npb = ELEMENT_Planes (twoshp->data, twogobj, pb, sb, &nsb);

      ret = update_convex_convex (va, nva, pa, npa, sa, nsa,
                                  vb, nvb, pb, npb, sb, nsb,
                                  onepnt, twopnt, normal,
				  gap, area, spair);

      convex_done (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);

      return ret;
    }
    break;
    case AABB_ELEMENT_SPHERE:
    {
      double va [24], pa [36];
      int nva, npa, sa [6], nsa;
      SPHERE *b = twogobj;

      nva = ELEMENT_Vertices (oneshp->data, onegobj, va);
      npa = ELEMENT_Planes (oneshp->data, onegobj, pa, sa, &nsa);

      return update_convex_sphere (va, nva, pa, npa, sa, nsa,
                                   b->cur_center, b->cur_radius, b->surface,
                                   onepnt, twopnt, normal,
				   gap, area, spair);
    }
    break;
    case AABB_SPHERE_ELEMENT:
    {
      SPHERE *a = onegobj;
      double vb [24], pb [36];
      int nvb, npb, sb [6], nsb,
	  ret;

      nvb = ELEMENT_Vertices (twoshp->data, twogobj, vb);
      npb = ELEMENT_Planes (twoshp->data, twogobj, pb, sb, &nsb);

      swap (spair);

      ret = update_convex_sphere (vb, nvb, pb, npb, sb, nsb,
                                  a->cur_center, a->cur_radius, a->surface,
                                  twopnt, onepnt, normal,
				  gap, area, spair);

      return update_swap (ret, spair);

    }
    break;
    case AABB_CONVEX_SPHERE:
    {
      double *va, *pa;
      SPHERE *b = twogobj;
      int nva, npa, *sa, nsa,
	  ret;

      convex_init (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);

      ret = update_convex_sphere (va, nva, pa, npa, sa, nsa,
                                  b->cur_center, b->cur_radius, b->surface,
                                  onepnt, twopnt, normal,
				  gap, area, spair);

      convex_done (onegobj, &va, &nva, &pa, &npa, &sa, &nsa);

      return ret;
    }
    break;
    case AABB_SPHERE_CONVEX:
    {
      SPHERE *a = onegobj;
      double *vb, *pb;
      int nvb, npb, *sb, nsb,
	  ret;

      convex_init (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      swap (spair);

      ret = update_convex_sphere (vb, nvb, pb, npb, sb, nsb,
                                  a->cur_center, a->cur_radius, a->surface,
                                  twopnt, onepnt, normal,
				  gap, area, spair);

      convex_done (twogobj, &vb, &nvb, &pb, &npb, &sb, &nsb);

      return update_swap (ret, spair);
    }
    break;
  }

  return 0;
}

/* detect or update contact data
 * between two geometric objects */
int gobjcontact (
    GOCDO action, short paircode,
    SHAPE *oneshp, void *onegobj,
    SHAPE *twoshp, void *twogobj,
    double onepnt [3],
    double twopnt [3],
    double normal [3],
    double *gap,
    double *area,
    int spair [2])
{
  if (action == CONTACT_DETECT)
    return detect (paircode, oneshp, onegobj, twoshp,
      twogobj, onepnt, twopnt, normal, gap, area, spair);
  else return update (paircode, oneshp, onegobj, twoshp,
    twogobj, onepnt, twopnt, normal, gap, area, spair);
}


/* get distance between two objects (output closest point pair in p, q) */
double gobjdistance (short paircode, SGP *one, SGP *two, double *p, double *q)
{
  switch (paircode)
  {
    case AABB_ELEMENT_ELEMENT:
    {
      double va [24],
             vb [24];
      int nva,
	  nvb;

      nva = ELEMENT_Vertices (one->shp->data, one->gobj, va);
      nvb = ELEMENT_Vertices (two->shp->data, two->gobj, vb);

      return gjk (va, nva, vb, nvb, p, q);
    }
    break;
    case AABB_CONVEX_CONVEX:
    {
      CONVEX *a = one->gobj,
	     *b = two->gobj;

      return gjk (a->cur, a->nver, b->cur, b->nver, p, q);
    }
    break;
    case AABB_SPHERE_SPHERE:
    {
      SPHERE *a = one->gobj,
	     *b = two->gobj;

      return gjk_sphere_sphere (a->cur_center, a->cur_radius, b->cur_center, b->cur_radius, p, q);
    }
    break;
    case AABB_ELEMENT_CONVEX:
    {
      double va [24];
      int nva;
      CONVEX *b = two->gobj;

      nva = ELEMENT_Vertices (one->shp->data, one->gobj, va);

      return gjk (va, nva, b->cur, b->nver, p, q);
    }
    break;
    case AABB_CONVEX_ELEMENT:
    {
      CONVEX *a = one->gobj;
      double vb [24];
      int nvb;

      nvb = ELEMENT_Vertices (two->shp->data, two->gobj, vb);

      return gjk (a->cur, a->nver, vb, nvb, p, q);
    }
    break;
    case AABB_ELEMENT_SPHERE:
    {
      double va [24];
      int nva;
      SPHERE *b = two->gobj;

      nva = ELEMENT_Vertices (one->shp->data, one->gobj, va);

      return gjk_convex_sphere (va, nva, b->cur_center, b->cur_radius, p, q);
    }
    break;
    case AABB_SPHERE_ELEMENT:
    {
      SPHERE *a = one->gobj;
      double vb [24];
      int nvb;

      nvb = ELEMENT_Vertices (two->shp->data, two->gobj, vb);

      return gjk_convex_sphere (vb, nvb, a->cur_center, a->cur_radius, q, p);
    }
    break;
    case AABB_CONVEX_SPHERE:
    {
      CONVEX *a = one->gobj;
      SPHERE *b = two->gobj;

      return gjk_convex_sphere (a->cur, a->nver, b->cur_center, b->cur_radius, p, q);
    }
    break;
    case AABB_SPHERE_CONVEX:
    {
      SPHERE *a = one->gobj;
      CONVEX *b = two->gobj;

      return gjk_convex_sphere (b->cur, b->nver, a->cur_center, a->cur_radius, q, p);
    }
    break;
  }

  return 0;
}
