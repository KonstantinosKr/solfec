# stack of cubes example (CONVEX and PSEUDO_RIGID)

N = 20

def cube (x, y, z, a, b, c, sur, vol):

  cvx = CONVEX (
	  [0, 0, 0,
	   a, 0, 0,
	   a, b, 0,
	   0, b, 0,
	   0, 0, c,
	   a, 0, c,
	   a, b, c,
	   0, b, c],
	  [4, 0, 3, 2, 1, sur,
	   4, 1, 2, 6, 5, sur,
	   4, 2, 3, 7, 6, sur,
	   4, 3, 0, 4, 7, sur,
	   4, 0, 1, 5, 4, sur,
	   4, 4, 5, 6, 7, sur], vol)

  TRANSLATE (cvx, (x, y, z))

  return cvx

def stack_of_cubes_create (numside, material, solfec):

  # create an obstacle base
  shp = cube (0, 0, -1, numside, numside, 1, 1, 1)
  BODY (solfec, 'OBSTACLE', shp, material)

  # create the remaining bricks
  for x in range (numside):
    for y in range (numside):
      for z in range (numside):
	shp = cube (x, y, z, 1, 1, 1, 2, 2)
        BODY (solfec, 'PSEUDO_RIGID', shp, material)

### main module ###

step = 0.001

solfec = SOLFEC ('DYNAMIC', step, 'out/mpi1')

CONTACT_SPARSIFY (solfec, 0.005)

surfmat = SURFACE_MATERIAL (solfec, model = 'SIGNORINI_COULOMB', friction = 0.3)

bulkmat = BULK_MATERIAL (solfec, model = 'KIRCHHOFF', young = 1E5, poisson = 0.25, density = 1E1)

GRAVITY (solfec, (0, 0, -1), 9.81)

#import rpdb2; rpdb2.start_embedded_debugger('a')

stack_of_cubes_create (N, bulkmat, solfec)

def gscallback (gs):
  print gs.error
  return 0

gs = GAUSS_SEIDEL_SOLVER (1E-3, 1000, failure = 'CONTINUE', callback = gscallback, diagsolver = 'PROJECTED_GRADIENT')

OUTPUT (solfec, 1 * step, 'FASTLZ')

RUN (solfec, gs, 10 * step)

if not VIEWER() and solfec.mode == 'READ':

  timers = ['TIMINT', 'CONDET', 'LOCDYN', 'CONSOL', 'PARBAL']
  dur = DURATION (solfec)
  th = HISTORY (solfec, timers, dur[0], dur[1])
  total = 0.0

  for i in range (0, 5):
    sum = 0.0
    for tt in th [i+1]: sum += tt
    print timers [i], 'TIME:', sum
    total += sum

  print 'TOTAL TIME:', total
