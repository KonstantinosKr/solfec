# Introduction #

This example is closer to our application context: a simplified graphite core undergoes a seismic excitation. The examples involves an undamped dynamic analysis with contact states changing at every time step. Dissipation of energy corresponds to the ideally plastic impacts. The material properties were: 15E9, 0.3, 1.8E3, 0.5 for respectively the Young modulus, the Poisson ratio, the mass density and the Coulomb friction. In all of the tests a “symmetrized” version of the Gauss-Seidel solvers was employed: alternate forward and backward runs were made over the set of contact points. In all tests the Gauss-Seidel **meritval** tolerance was 1E-5 and the iterations bound was 200 (the relative reaction change **epsilon** was 10 so that it would not come into play). The tests were run on an Intel Xeon 5472 3.00GHz based cluster with Infiniband network.

The simplified graphite core model is based on a repetition of a pattern of loosely keyed together bricks and it is
meant to approximate the computational complexity of a more elaborate model. 4000 pseudo-rigid3 and 1333 rigid bodies (55998 degrees of freedom) are subject to gravity and rest on a rigid foundation. The horizontal acceleration of the foundation equals to a seismic signal. 1 second runs were computed with the time step 1E-4 in order to test the parallel scaling on 8 to 64 processors. Additionally, the 64 processor run was repeated with time steps 2E-5, 5E-4 and 1E-3 in order to exemplify the influence of the step size on the convergence of the Gauss-Seidel solver and on total runtimes.

Further details on this example can be found in  [this paper](http://wiki.solfec.googlecode.com/hg/2010-Koziara-distributed-contact-dynamics.pdf).

[Link to the input file](http://code.google.com/p/solfec/source/browse/inp/cores/simple-quarter.py)

# Videos #

The videos below show full 11 second runs computed with a lower Gauss-Seidel accuracy only for the sake of illustration (solely rigid model, 10 Gauss-Seidel iterations per time step 0.001, 3h on 32 Intel Nehalem processors of a gigabit ethernet based cluster). The top view time history of the x displacement (direction of the base excitation):

![http://wiki.solfec.googlecode.com/hg/simple-quarter-top-dx.gif](http://wiki.solfec.googlecode.com/hg/simple-quarter-top-dx.gif)
[watch on YouTube](http://www.youtube.com/watch?v=xH8mjI5HYmw)

The bottom surface time history of the x displacement, with some close-ups:

![http://wiki.solfec.googlecode.com/hg/simple-quarter-bot-dx.gif](http://wiki.solfec.googlecode.com/hg/simple-quarter-bot-dx.gif)
[watch on YouTube](http://www.youtube.com/watch?v=nS10_2UrzC8)

# Figures #

The model and a close-up of a brick pattern illustrated below.

![http://wiki.solfec.googlecode.com/hg/core1.png](http://wiki.solfec.googlecode.com/hg/core1.png)

Additionally, the picture below illustrates a selection of bricks from the core model. Although the geometry is quite simplified, it gives an idea of the complexity of a real model.

![http://wiki.solfec.googlecode.com/hg/simple-quarter.png](http://wiki.solfec.googlecode.com/hg/simple-quarter.png)

The total runtime scaling is illustrated below. As for the bridge filling example, the super-linear scaling can be explained by a not perfectly uniform distribution of the computational work across the subdomains. The situation improves for larger numbers of processors. This kind of effect will generally depend on the input geometry and the parameters of the dynamic load balancing (input command IMBALANCE\_TOLERANCE).

Because of the ongoing base excitation the contact point interactions constantly change. Hence, for the 64 processor runs with larger steps, the Gauss-Seidel solver dominates the solution time and the total runtime grows. To total runtime would also grow for increasingly smaller time steps. It is then possible to find an "optimal" time step, corresponding to a shortest total runtime.

In the figure TIMEINT stands for time integration, CONUPD stands for constraints update, CONDET stands for contact detection, LOCDYN stands for local dynamics assembling, CONSOL stands for the Gauss-Seidel constraints solution, PARBAL stands for parallel load balancing and communication not related to constraints solution. The dashed line indicates linear scaling

![http://wiki.solfec.googlecode.com/hg/core2.png](http://wiki.solfec.googlecode.com/hg/core2.png)

The Gauss-Seidel runtime scaling in the figure below is as well super-linear. This corresponds to a nonuniform balancing of the off-diagonal elements of the W operator. Clearly, for larger numbers of processors the communication time dominates the solver runtimes.

In the below figure INIT stands for initialization. RUN stands for floating point computations. COM stands for communication not related to the middle set of constraints. MCOM stands for the communication of the middle set of constraint. The dashed line indicates linear scaling.

![http://wiki.solfec.googlecode.com/hg/core3.png](http://wiki.solfec.googlecode.com/hg/core3.png)

The below figure pictures the Gauss-Seidel iterations histories for 8, 16, 32 and 64 processors with step 1E-4 (left), and the 64 processor run histories for the time steps 1E-4, 2E-4, 5E-4 and 1E-3 (right). For the 1E-4 step runs, the number of iterations until **meritval** < 1E-5 does not depend on the number of processors. This is in agreement with the fact that our Gauss-Seidel implementation is consistent, while various processor numbers correspond only to varying orderings of contact points. The bumps in the history of the 8 processor run are related to a more frequent rebalancing, which changes the ordering of the contact points. This could have been alleviated by tuning the load balancing parameters.

![http://wiki.solfec.googlecode.com/hg/core4.png](http://wiki.solfec.googlecode.com/hg/core4.png)

The last figure illustrates the constraints number histories for 8, 16, 32 and 64 processors with step 1E-4 (left), and the 64 processor run histories for the time steps 1E-4, 2E-4, 5E-4 and 1E-3 (right). There is some variability, which is understandable, taking the fact that a highly nonlinear dynamic system is integrated under varying circumstances.

![http://wiki.solfec.googlecode.com/hg/core5.png](http://wiki.solfec.googlecode.com/hg/core5.png)

# Note #

A lesson has ben learnt when working with this model. In an earlier version of the model only one rigid body was used to support the entire structure. Hence, all contact points at the base were mutually adjacent through a common kinematic interaction. This translated into many dense row entires in the local dynamics equations, which as a result made the runtimes some 10 times longer.

The change: The base body has been split into tiles, each supporting an individual fuel channel and each having prescribed the same base motion. The tiles were separated by few millimeters, which left the overall model essential the same. In this case though contact point interactions at the base decouple and the local dynamics system is much sparser and hence easier to solve.