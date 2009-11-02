# simple core model
from math import sin
from math import cos
from math import sqrt

# some constants
PI = 3.14159265358979323846 
FIG9 = 1
FIG11 = 2

def gcore_brick_half (inr, outd, keyinw, keyoutw1, keyoutw3, keyh, height, hstep, type):

  faces = [4, 0, 1, 5, 4, 0,
           4, 1, 2, 6, 5, 0,
           4, 2, 3, 7, 6, 0,
           4, 3, 0, 4, 7, 0,
           4, 0, 3, 2, 1, 0,
           4, 4, 5, 6, 7, 0]


  step = 2.0 * PI / 16.0
  angle = PI / 2.0 + step / 2.0

  zero = (0, 0, 0)
  zet = (0, 0, 1)
  toph =  outd * 0.5
  outr = toph / sin (angle)
  keyoutw2 = keyoutw1 + (keyoutw3 - keyoutw1) * (toph - inr) / (toph - inr - keyh)

  a = [keyoutw1/2.0, inr, 0,
       keyoutw2/2.0, toph-keyh, 0,
      -keyoutw2/2.0, toph-keyh, 0,
      -keyoutw1/2.0, inr, 0,
       keyoutw1/2.0, inr, height,
       keyoutw2/2.0, toph-keyh, height,
      -keyoutw2/2.0, toph-keyh, height,
      -keyoutw1/2.0, inr, height]

  set = []
  list = []
  c1 = None
  c2 = None

  for i in range (8):

    cvy = CONVEX (a, faces, 0);
    ROTATE (cvy, zero, zet, i*45);

    if i == 1: c1 = cvy

    if type == FIG9:
      if i % 2 != 0: set.append (cvy)
    elif type == FIG11:
      if i % 2 == 0: set.append (cvy)

    list.append (cvy)


  b = [keyoutw2/2.0, toph-keyh, 0,
       keyoutw3/2.0, toph, 0,
       keyinw/2.0, toph, 0,
       keyinw/2.0, toph-keyh, 0,
       keyoutw2/2.0, toph-keyh, height,
       keyoutw3/2.0, toph, height,
       keyinw/2.0, toph, height,
       keyinw/2.0, toph-keyh, height]

  c = [-keyinw/2.0, toph-keyh, 0,
       -keyinw/2.0, toph, 0,
       -keyoutw3/2.0, toph, 0,
       -keyoutw2/2.0, toph-keyh, 0,
       -keyinw/2.0, toph-keyh, height,
       -keyinw/2.0, toph, height,
       -keyoutw3/2.0, toph, height,
       -keyoutw2/2.0, toph-keyh, height]


  for i in range (8):

    cvx = CONVEX (b, faces, 0)
    cvy = CONVEX (c, faces, 0)

    ROTATE (cvx, zero, zet, i*45)
    ROTATE (cvy, zero, zet, i*45)

    if i == 1: c2 = cvx

    if type == FIG9:
      if i % 2 != 0:
	set.append (cvx)
	set.append (cvy)
    elif type == FIG11:
      if i % 2 == 0:
	set.append (cvx)
	set.append (cvy)

    list.append (cvx)
    list.append (cvy)


  d = [a [9], a [10], 0,
       c [6], c [7], 0,
       outr * cos (angle), outr * sin (angle), 0,
       outr * cos (angle+step), outr * sin (angle+step), 0,
       c2.vertex(1)[0], c2.vertex(1)[1], 0,
       c1.vertex(0)[0], c1.vertex(0)[1], 0,
       a [9], a [10], height,
       c [6], c [7], height,
       outr * cos (angle), outr * sin (angle), height,
       outr * cos (angle+step), outr * sin (angle+step), height,
       c2.vertex(1)[0], c2.vertex(1)[1], height,
       c1.vertex(0)[0], c1.vertex(0)[1], height]

  g = [4, 0, 1, 7, 6, 0,
       4, 1, 2, 8, 7, 0,
       4, 2, 3, 9, 8, 0,
       4, 3, 4, 10, 9, 0,
       4, 4, 5, 11, 10, 0,
       4, 5, 0, 6, 11, 0,
       6, 0, 5, 4, 3, 2, 1, 0,
       6, 6, 7, 8, 9, 10, 11, 0]

  for i in range (8):

    cvy = CONVEX (d, g, 0)
    ROTATE (cvy, zero, zet, i*45);

    if type == FIG9: set.append (cvy)

    list.append (cvy)

  scal = (1, 1, (height - hstep) / height)

  for item in set:
    SCALE (item, scal)

  return list

def gcore_brick (x, y, z):

  dfac = 0.015
  outd = 0.4598
  height = 0.225
  hstep = 0.0098
  keyw = 0.0381
  keyh = 0.0381

  cvx = gcore_brick_half (0.1315, outd, keyw, 0.05156, 0.05161, keyh, height, 0.0101, FIG11)
  zero = (0, 0, 0)
  yaxis =  (0, 1, 0)
  vec = (x, y, z + height)
  ROTATE (cvx, zero, yaxis, 180)
  TRANSLATE (cvx, vec)

  vec = (x, y, z + height)
  cvy = gcore_brick_half (0.1315, outd, keyw, 0.05075, 0.05080, keyh, height, hstep, FIG9)
  TRANSLATE (cvy, vec)

  cvx.extend (cvy)

  return cvx


def gcore_base (material, solfec):

  vertices = [1, 0, 0,
              1, 1, 0,
              0, 1, 0,
              0, 0, 0,
              1, 0, 1,
              1, 1, 1,
              0, 1, 1,
              0, 0, 1]

  faces = [4, 0, 1, 5, 4, 3,
           4, 1, 2, 6, 5, 3,
           4, 2, 3, 7, 6, 3,
           4, 3, 0, 4, 7, 3,
           4, 0, 3, 2, 1, 3,
           4, 4, 5, 6, 7, 3]

  outd = 0.4598
  margin = 0.05
  thick = 0.1
  lx = outd + (margin + thick)
  ly = outd + (margin + thick)
  shape = []

  cvx = CONVEX (vertices, faces, 3)
  scl = (lx,  ly,  thick)
  vec = (-lx/2, -ly/2, -thick)
  SCALE (cvx, scl)
  TRANSLATE (cvx, vec)
  shape.append (cvx)

  BODY (solfec, 'OBSTACLE', shape, material)

def generate_scene (material, solfec):

  gcore_base (material, solfec)

  shp = gcore_brick (0, 0, 0)

  TRANSLATE (shp, (0, 0, 0.11))
  ROTATE (shp, (0, 0, 0.11), (1, 1, 1), 35)

  msh = ROUGH_HEX (shp, 2, 2, 2)

  BODY (solfec , 'FINITE_ELEMENT', shp, material, mesh=msh)
  #BODY (solfec , 'FINITE_ELEMENT', msh, material)
  #BODY (solfec , 'PSEUDO_RIGID', shp, material)

### main module ###

step = 1E-3

solfec = SOLFEC ('DYNAMIC', step, 'out/boxkite')

surfmat = SURFACE_MATERIAL (solfec, model = 'SIGNORINI_COULOMB', friction = 0.3)

bulkmat = BULK_MATERIAL (solfec, model = 'KIRCHHOFF', young = 1E5, poisson = 0.25, density = 1E3)

tms = TIME_SERIES ([0, 10, 1, 10])

GRAVITY (solfec, (0, 0, -1), 10)

generate_scene (bulkmat, solfec)

#import rpdb2; rpdb2.start_embedded_debugger('a')

gs = GAUSS_SEIDEL_SOLVER (1E-3, 1000, failure = 'EXIT', diagsolver = 'PROJECTED_GRADIENT')

OUTPUT (solfec, 20 * step)

RUN (solfec, gs, 10000 * step)
