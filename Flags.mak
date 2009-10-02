#
# Compilation flags setup
#

ifeq ($(POSIX),yes)
  POSIX = -DPOSIX
else
  POSIX = 
endif

ifeq ($(OPENGL),yes)
  OPENGL = -DOPENGL $(GLINC)
else
  OPENGL =
  GLLIB = 
endif

ifeq ($(DEBUG),yes)
  DEBUG =  -W -Wall -Wno-unused-parameter -pedantic -g -DDEBUG
  ifeq ($(PROFILE),yes)
    PROFILE = -p
  else
    PROFILE =
  endif
  ifeq ($(NOTHROW),yes)
    NOTHROW = -DNOTHROW
  else
    NOTHROW =
  endif
  ifeq ($(MEMDEBUG),yes)
    MEMDEBUG = -DMEMDEBUG
  else
    MEMDEBUG =
  endif
  ifeq ($(GEOMDEBUG),yes)
    GEOMDEBUG = -DGEOMDEBUG
  else
    GEOMDEBUG =
  endif
else
  DEBUG =  -w -pedantic -O3 -funroll-loops
  PROFILE =
  MEMDEBUG =
  GEOMDEBUG =
  NOTHROW =
endif

ifeq ($(MPI),yes)
  ifeq ($(MPITHREADS),yes)
    MPIFLG = -DMPI -DMPITHREADS $(ZOLTANINC)
  else
    MPIFLG = -DMPI $(ZOLTANINC)
    MPILIB =
  endif
  MPILIBS = $(ZOLTANLIB)
endif
