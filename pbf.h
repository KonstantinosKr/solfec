/*
 * pbf.h
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

#include <stdint.h>
#include <stdio.h>
#include <rpc/types.h>
#include <rpc/xdr.h>
#include "map.h"
#include "mem.h"

#if __MINGW32__
  #define FSEEK fseeko64
  #define FTELL ftello64
#else
  #define FSEEK fseeko
  #define FTELL ftello
#endif

#if __APPLE__
  #define xdr_uint64_t xdr_u_int64_t
#endif

#ifndef __pbf__
#define __pbf__

#define PBF_MAXSTRING 4096 /* maximal string length used */

typedef struct pbf_marker PBF_MARKER; /* file marker */
typedef struct pbf_label PBF_LABEL; /* label type */
typedef struct pbf PBF; /* file type */

/* marker */
struct pbf_marker
{
  double time; /* time moment */
  u_int ipos; /* index position */
  uint64_t dpos; /* data position */
};

/* label */
struct pbf_label
{
  char *name; /* label name */
  int index; /* unique index */
  uint64_t dpos; /* data position */
};

/* file */
struct pbf
{
  char *dph; /* data path */
  char *iph; /* index path */
  char *lph; /* label path */
  FILE *dat; /* data file */
  FILE *idx; /* index file */
  FILE *lab; /* label file */
  XDR x_dat; /* data coding context */
  XDR x_idx; /* index coding context */
  XDR x_lab; /* labels coding context */
  char *mem; /* x_dat memory (READ) */
  MEM mappool; /* map items pool */
  MEM labpool; /* labels pool */
  PBF_LABEL *ltab; /* table of labels */
  MAP *labels; /* name mapped labels */
  PBF_MARKER *mtab; /* markers */
  enum {PBF_READ, PBF_WRITE} mode; /* access mode */
  double time; /* current time (>= 0) */
  int lsize; /* free index (WRITE) or ltab size (READ) */
  unsigned int msize; /* mtab size (READ) */
  unsigned int cur; /* index of current time frame */
  PBF *next; /* list of parallel files (READ) */
};

/* open for writing */
PBF* PBF_Write (const char *path);

/* open for reading */
PBF* PBF_Read (const char *path);

/* close file */
void PBF_Close (PBF *bf);

/* flush buffers */
void PBF_Flush (PBF *bf);

/* read/write current time */
void PBF_Time (PBF *bf, double *time);

/* set current label; in read mode return
 * positive value if the label was found */
int PBF_Label (PBF *bf, const char *label);

/* read/write characters */
void PBF_Char (PBF *bf, char *value, unsigned int length);

/* read/write unsigned characters */
void PBF_Uchar (PBF *bf, unsigned char *value, unsigned int length);

/* read/write other simple types ... */
void PBF_Short (PBF *bf, short *value, unsigned int length);
void PBF_Ushort (PBF *bf, unsigned short *value, unsigned int length);
void PBF_Int (PBF *bf, int *value, unsigned int length);
void PBF_Uint (PBF *bf, unsigned int *value, unsigned int length);
void PBF_Long (PBF *bf, long *value, unsigned int length);
void PBF_Ulong (PBF *bf, unsigned long *value, unsigned int length);
void PBF_Float (PBF *bf, float *value, unsigned int length);
void PBF_Double (PBF *bf, double *value, unsigned int length);

/* read/write NULL-termined string */
void PBF_String (PBF *bf, char **value);

/* get time limits in read mode */
void PBF_Limits (PBF *bf, double *start, double *end);

/* seek to time in read mode */
void PBF_Seek (PBF *bf, double time);

/* make 'steps' backward in read mode */
void PBF_Backward (PBF *bf, unsigned int steps);

/* make 'steps' forward in read mode */
void PBF_Forward (PBF *bf, unsigned int steps);

/* get number of time instants spanned by [t0, t1] */
unsigned int PBF_Span (PBF *bf, double t0, double t1);
#endif
