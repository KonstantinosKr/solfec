# first ellipsoid example

step = 1E-3
stop = 3

solfec = SOLFEC ('DYNAMIC', step, 'out/ellip')

GRAVITY (solfec, (0, 0, -10))

bulkmat = BULK_MATERIAL (solfec, model = 'KIRCHHOFF', young = 1E6, poisson = 0.3, density = 1E3)

surfmat = SURFACE_MATERIAL (solfec, model = 'SIGNORINI_COULOMB', friction = 0.5, restitution = 0)

n = 4

for i in range (1, n):
  for j in range (1, n):
    for k in range (1, n):
      shp = ELLIP ((i, j, k), (0.1, 0.2, 0.3), 3, 3)
      ROTATE (shp, (i, j, k), (1, 1, 1), 45)
      bod = BODY (solfec, 'RIGID', shp, bulkmat)

table = HULL ([0, 0, 0,
               0, 1, 0,
	       1, 1, 0,
	       1, 0, 0,
               0, 0, -0.1,
               0, 1, -0.1,
	       1, 1, -0.1,
	       1, 0, -0.1], 1, 1)
SCALE (table, (n, n, 1))
#table = ELLIP ((n/2, n/2, 0), (n, n, 0.2), 3, 3)
#table = SPHERE ((n/2, n/2, -n/2), n/2, 3, 3)
BODY (solfec, 'OBSTACLE', table, bulkmat)

#sv = GAUSS_SEIDEL_SOLVER (1E1, 1000, 1E-7)
sv = NEWTON_SOLVER (1E-7, delta = 1E-5)

OUTPUT (solfec, 0.01)
RUN (solfec, sv, stop)
