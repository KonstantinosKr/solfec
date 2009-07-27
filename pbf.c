/*
 * pbf.c
 * Copyright (C) 2006, Tomasz Koziara (t.koziara AT gmail.com)
 * --------------------------------------------------------------
 * portable binary format (PBF)
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
#include "pbf.h"
#include "err.h"

/* memory increment */
#define CHUNK 1024

/* DAT file format:
 * ----------------
 *  [FRAME_0]
 *  [FRAME_1]
 *  ...
 *  [FRAME_N]
 * ----------
 *  FRAME_i:
 * -------------------
 *    [UNLABELED_DATA]
 *    [LABELED_0]
 *    [LABELED_1]
 *    ...
 *    [LABELED_K]
 * --------------
 */

/* IDX file format:
 * ----------------
 *  [FRAME_0]
 *  [FRAME_1]
 *  ...
 *  [FRAME_N]
 * ----------
 *  FRAME_i:
 * -------------------------------
 *  [TIME] (double) {current time}
 *  [DOFF] (unsigned int) {offest of FRAME_i in DAT file}
 *  [IDX_0] (int) {index of first label}
 *  [POS_0] (unsigned int) {position of labeled data in DAT file}
 *  [IDX_1]
 *  [POS_1]
 *  ...
 *  [IDX_K]
 *  [POS_K]
 *  [-1] (int) {end of labels marker}
 * ----------------------------------
 */

/* LAB file format:
 * ----------------
 *  [LABEL_0] (string) {first label name}
 *  [LABEL_1]
 *  ...
 *  [LABEL_M]
 * ----------
 */

/* initialise labels and time index */
static void initialise_reading (PBF *bf)
{
  int index, num, siz;
  unsigned int pos;
  PBF_LABEL *l;
  
  /* create labels table */
  num = 0; siz = CHUNK;
  ERRMEM (bf->ltab = malloc (sizeof (PBF_LABEL) * siz));
  while (! feof (bf->lab))
  {
    bf->ltab [num].name = NULL;
    if (xdr_string (&bf->x_lab, &bf->ltab [num].name, PBF_MAXSTRING))
    {
      bf->ltab [num].index = num;
      if (++ num >= siz)
      { siz += CHUNK;
	ERRMEM (bf->ltab = realloc (bf->ltab, sizeof (PBF_LABEL) * siz)); }
    }
    else break;
  }
  bf->ltab = realloc (bf->ltab, sizeof (PBF_LABEL) * num); /* shrink */
  bf->lsize = num;
  
  /* create markers table */
  num = 0; siz = CHUNK;
  ERRMEM (bf->mtab = malloc (sizeof (PBF_MARKER) * siz));
  while (! feof (bf->idx))
  {
    /* time and unlabeled data position */
    if (xdr_double (&bf->x_idx, &bf->mtab [num].time))
    {
      xdr_u_int (&bf->x_idx, &bf->mtab [num].dpos);
      bf->mtab [num].ipos = xdr_getpos (&bf->x_idx);

      /* labels */
      xdr_int (&bf->x_idx, &index);
      while (index >= 0 && (! feof (bf->idx)))
      {
	xdr_u_int (&bf->x_idx, &pos);
	xdr_int (&bf->x_idx, &index);
      }
      
      if (++ num >= siz)
      { siz += CHUNK;
	ERRMEM (bf->mtab = realloc (bf->mtab, sizeof (PBF_MARKER) * siz)); }
    }
    else break;
  }
  bf->mtab = realloc (bf->mtab, sizeof (PBF_MARKER) * num); /* shrink */
  bf->msize = num;

  /* initialise state, skip
   * time and position */
  xdr_setpos (&bf->x_idx, 0);
  xdr_double (&bf->x_idx, &bf->time); /* set current time */
  xdr_u_int (&bf->x_idx, &pos); /* must be 0 anyhow */
 
  /* read labels */ 
  xdr_int (&bf->x_idx, &index);
  while (index >= 0 && (!feof (bf->idx)))
  {
    ASSERT (index < bf->lsize, ERR_PBF_INDEX_FILE_CORRUPTED);
    l = &bf->ltab [index];

    /* map label as current one */
    MAP_Insert (&bf->mappool, &bf->labels, l->name, l,
      (MAP_Compare) strcmp);

    /* read position */
    xdr_u_int (&bf->x_idx, &l->dpos);

    /* get next label */
    xdr_int (&bf->x_idx, &index);
  }
}

/* initialize frame after seek */
static void initialise_frame (PBF *bf, int frm)
{
  PBF_LABEL *l;
  int index;

  bf->cur = frm; /* set current frame */
  bf->time = bf->mtab [frm].time; /* and time */

  /* empty current labels set */
  MAP_Free (&bf->mappool,&bf->labels);
 
  /* seek to the frame in IDX and DAT */ 
  xdr_setpos (&bf->x_idx, bf->mtab [frm].ipos);
  xdr_setpos (&bf->x_dat, bf->mtab [frm].dpos);

  /* read labels */
  xdr_int (&bf->x_idx, &index);
  while (index >= 0 && (!feof (bf->idx)))
  {
    ASSERT (index < bf->lsize, ERR_PBF_INDEX_FILE_CORRUPTED);
    l = &bf->ltab [index];

    /* map label as current one */
    MAP_Insert (&bf->mappool, &bf->labels, l->name, l,
      (MAP_Compare) strcmp);

    /* read position */
    xdr_u_int (&bf->x_idx, &l->dpos);

    /* get next label */
    xdr_int (&bf->x_idx, &index);
  }
}

/* test before closing */
static int is_empty (FILE *f)
{
  fseek (f, 0, SEEK_END);
  return ftell (f) == 0;
}

/* copy path */
static char* copypath (char *path)
{
  char *out = NULL;
  int l;

  if ((l = path ? strlen (path) : 0))
  {
    ERRMEM (out = malloc (l + 1));
    strcpy (out, path);
  }

  return out;
}

PBF* PBF_Write (const char *path)
{
  char *txt;
  PBF *bf;

  ERRMEM (bf = malloc (sizeof (PBF)));
  ERRMEM (txt = malloc (strlen (path) + 16));

  /* openin files */
  sprintf (txt, "%s.dat", path);
  if (! (bf->dat = fopen (txt, "w"))) goto failure;
  xdrstdio_create (&bf->x_dat, bf->dat, XDR_ENCODE);
  bf->dph = copypath (txt);
  sprintf (txt, "%s.idx", path);
  if (! (bf->idx = fopen (txt, "w"))) goto failure;
  xdrstdio_create (&bf->x_idx, bf->idx, XDR_ENCODE);
  bf->iph = copypath (txt);
  sprintf (txt, "%s.lab", path);
  if (! (bf->lab = fopen (txt, "w"))) goto failure;
  xdrstdio_create (&bf->x_lab, bf->lab, XDR_ENCODE);
  bf->lph = copypath (txt);

  /* initialise the rest */
  MEM_Init (&bf->mappool, sizeof (MAP), 1024);
  MEM_Init (&bf->labpool, sizeof (PBF_LABEL), 1024);
  bf->ltab = NULL;
  bf->labels = NULL;
  bf->mtab = NULL;
  bf->mode = PBF_WRITE;
  bf->time = 0.;
  bf->lsize = 0;
  bf->msize = 0;
  bf->cur = 0;
  free (txt);
  return bf;
  
failure: 
  free (bf);
  free (txt);
  return NULL;
}

PBF* PBF_Read (const char *path)
{
  char *txt;
  PBF *bf;

  ERRMEM (bf = malloc (sizeof (PBF)));
  ERRMEM (txt = malloc (strlen (path) + 16));

  /* openin files */
  sprintf (txt, "%s.dat", path);
  if (! (bf->dat = fopen (txt, "r"))) goto failure;
  xdrstdio_create (&bf->x_dat, bf->dat, XDR_DECODE);
  bf->dph = copypath (txt);
  sprintf (txt, "%s.idx", path);
  if (! (bf->idx = fopen (txt, "r"))) goto failure;
  xdrstdio_create (&bf->x_idx, bf->idx, XDR_DECODE);
  bf->iph = copypath (txt);
  sprintf (txt, "%s.lab", path);
  if (! (bf->lab = fopen (txt, "r"))) goto failure;
  xdrstdio_create (&bf->x_lab, bf->lab, XDR_DECODE);
  bf->lph = copypath (txt);

  /* initialise the rest */
  MEM_Init (&bf->mappool, sizeof (MAP), 1024);
  MEM_Init (&bf->labpool, sizeof (PBF_LABEL), 1024);
  bf->ltab = NULL;
  bf->labels = NULL;
  bf->mtab = NULL;
  bf->mode = PBF_READ;
  bf->time = 0.;
  bf->lsize = 0;
  bf->msize = 0;
  bf->cur = 0;
  initialise_reading (bf);
  free (txt);
  return bf;
  
failure: 
  free (bf);
  free (txt);
  return NULL;
}

void PBF_Close (PBF *bf)
{
  int empty;

  /* close streams */
  xdr_destroy (&bf->x_dat);
  xdr_destroy (&bf->x_idx);
  xdr_destroy (&bf->x_lab);

  fflush (bf->dat);
  fflush (bf->idx);
  fflush (bf->lab);

  empty = is_empty (bf->dat);

  fclose (bf->dat);
  fclose (bf->idx);
  fclose (bf->lab);

  if (empty) /* remove empty files */
  {
    remove (bf->dph);
    remove (bf->iph);
    remove (bf->lph);
  }
 
  free (bf->dph);
  free (bf->iph);
  free (bf->lph);

  /* free labels & markers */ 
  if (bf->mode == PBF_READ)
  {
    int k;

    for (k = 0; k < bf->lsize; k ++)
      free (bf->ltab [k].name);

    free (bf->ltab);
    free (bf->mtab);
  }
  else
  {
    MAP *k;

    for (k = MAP_First (bf->labels);
      k; k = MAP_Next (k))
    {
      PBF_LABEL *l = (PBF_LABEL*)k->data;
      free (l->name);
    }
    MEM_Release (&bf->labpool);
  }

  MEM_Release (&bf->mappool);
  free (bf);
}

void PBF_Time (PBF *bf, double *time)
{
  int index;
  unsigned int pos;

  if (bf->mode == PBF_WRITE)
  {
    ASSERT ((*time) >= bf->time, ERR_PBF_OUTPUT_TIME_DECREASED);
    if (xdr_getpos (&bf->x_idx) > 0) /* mark end of previous time frame */
    {
      index = -1;
      xdr_int (&bf->x_idx, &index);
    }

    /* output current data
     * position and time */
    xdr_double (&bf->x_idx, time);
    pos = xdr_getpos (&bf->x_dat);
    xdr_u_int (&bf->x_idx, &pos); /* dat position */

    /* set time */
    bf->time = *time;
  }
  else
  { *time = bf->time; }
}

int PBF_Label (PBF *bf, const char *label)
{
  PBF_LABEL *l;
  unsigned int pos;

  if (bf->mode == PBF_WRITE)
  {
    if (!(l = MAP_Find (bf->labels, (void*)label,
      (MAP_Compare) strcmp)))
    {
      /* create new label */
      l = MEM_Alloc (&bf->labpool);
      ERRMEM (l->name = malloc (strlen (label) + 1));
      strcpy (l->name, label);
      l->index = bf->lsize ++;
      MAP_Insert (&bf->mappool, &bf->labels,
	l->name, l, (MAP_Compare) strcmp);

      /* output definition */
      xdr_string (&bf->x_lab, (char**)&label, PBF_MAXSTRING);
    }

    /* record label and position
     * in the index file */
    xdr_int (&bf->x_idx, &l->index);
    pos = xdr_getpos (&bf->x_dat);
    xdr_u_int (&bf->x_idx, &pos);
  }
  else
  {
    if ((l = MAP_Find (bf->labels, (void*)label,
      (MAP_Compare) strcmp)))
    {
      /* seek to labeled data begining */
      xdr_setpos (&bf->x_dat, l->dpos);
      return 1;
    }
  }
  return 0;
}

void PBF_Char (PBF *bf, char *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (char), (xdrproc_t)xdr_char); }

void PBF_Uchar (PBF *bf, unsigned char *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (unsigned char), (xdrproc_t)xdr_u_char); }

void PBF_Short (PBF *bf, short *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (short), (xdrproc_t)xdr_short); }

void PBF_Ushort (PBF *bf, unsigned short *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (unsigned short), (xdrproc_t)xdr_u_short); }

void PBF_Int (PBF *bf, int *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (int), (xdrproc_t)xdr_int); }

void PBF_Uint (PBF *bf, unsigned int *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (unsigned int), (xdrproc_t)xdr_u_int); }

void PBF_Long (PBF *bf, long *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (long), (xdrproc_t)xdr_long); }

void PBF_Ulong (PBF *bf, unsigned long *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (unsigned long), (xdrproc_t)xdr_u_long); }

void PBF_Float (PBF *bf, float *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (float), (xdrproc_t)xdr_float); }

void PBF_Double (PBF *bf, double *value, unsigned int length)
{ xdr_vector (&bf->x_dat, (char*)value, length, sizeof (double), (xdrproc_t)xdr_double); }

void PBF_String (PBF *bf, char **value)
{ xdr_string (&bf->x_dat, value, PBF_MAXSTRING); }

void PBF_Limits (PBF *bf, double *start, double *end)
{
  if (bf->mode == PBF_READ)
  {
    *start = bf->mtab [0].time;
    *end = bf->mtab [bf->msize - 1].time;
  }
}

void PBF_Seek (PBF *bf, double time)
{
  if (bf->mode == PBF_READ)
  {
    PBF_MARKER *l, *h, *m;

    /* binary search
     * of marker */
    l = bf->mtab;
    h = l + bf->msize - 1;
    while (l <= h)
    {
      m = l + (h - l) / 2;
      if (time == m->time) break;
      else if (time < m->time) h = m - 1;
      else l = m + 1;
    }

    /* handle limit cases */
    if (h < bf->mtab) m = l;
    else if (l > (bf->mtab + bf->msize - 1)) m = h;
    initialise_frame (bf, m - bf->mtab);
  }
}

void PBF_Backward (PBF *bf, int steps)
{
  if (bf->mode == PBF_READ)
  {
    if (bf->cur - steps <  0)
      steps = 0;
    else steps = bf->cur - steps;
    initialise_frame (bf, steps);
  }
}

void PBF_Forward (PBF *bf, int steps)
{
  if (bf->mode == PBF_READ)
  {
    if (bf->cur + steps >=  bf->msize)
      steps = bf->msize - 1;
    else steps += bf->cur;
    initialise_frame (bf, steps);
  }
}
