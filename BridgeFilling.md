# Introduction #

This example is meant to illustrate an analysis with large relative motion: a masonry bridge is backfilled with randomly generated stones. The examples involves an undamped dynamic analysis with contact states changing at every time step. Dissipation of energy corresponds to the ideally plastic impacts. The material properties were: 15E9, 0.3, 1.8E3, 0.5 for respectively the Young modulus, the Poisson ratio, the mass density and the Coulomb friction. In all of the tests a “symmetrized” version of the Gauss-Seidel solvers was employed: alternate forward and backward runs were made over the set of contact points. In all tests the Gauss-Seidel **meritval** tolerance was 1E-5 and the iterations bound was 200 (the relative reaction change **epsilon** was 10 so that it would not come into play). The tests were run on an Intel Xeon 5472 3.00GHz based cluster with Infiniband network.

10030 rigid bodies were used to set up the geometry (60180 degrees of freedom). The backfilling stones were convex, with 8 to 64 vertices and they were randomly generated without initial interactions. 3 second runs were computed with the time step 1E-4 in order to test the parallel scaling on 8 to 64 processors. Additionally, the 64 processor run was repeated with time steps 2E-5, 5E-4 and 1E-3 in order to exemplify the influence of the step size on the convergence of the Gauss-Seidel solver and on total runtimes.

Further details on this example can be found in  [this paper](http://wiki.solfec.googlecode.com/hg/2010-Koziara-distributed-contact-dynamics.pdf).

[Link to the input file](http://code.google.com/p/solfec/source/browse/inp/bridge.py)

# Videos #

The below animation illustrates partitioning of contact points for the 8 processor run. The dynamic domain partitioning is adjusting itself to the changing geometry of the domain.

![http://wiki.solfec.googlecode.com/hg/bridge1.gif](http://wiki.solfec.googlecode.com/hg/bridge1.gif)

# Figures #

The initial and final geometry are illustrated below. The colors correspond to the assignment of bodies to processors for the 8 processor run.

![http://wiki.solfec.googlecode.com/hg/bridge1.png](http://wiki.solfec.googlecode.com/hg/bridge1.png)

The total runtime scaling is illustrated below. The example is too small to scale up to 64 processors. Because the bodies eventually come to rest, the structure of contact interactions stabilizes. This allows the 64 processor runs with larger time steps to take even less time (i.e. Gauss-Seidel can still take advantage of "hot restarts" for larger steps).

In the figure TIMEINT stands for time integration, CONUPD stands for constraints update, CONDET stands for contact detection, LOCDYN stands for local dynamics assembling, CONSOL stands for the Gauss-Seidel constraints solution, PARBAL stands for parallel load balancing and communication not related to constraints solution. The dashed line indicates linear scaling

![http://wiki.solfec.googlecode.com/hg/bridge2.png](http://wiki.solfec.googlecode.com/hg/bridge2.png)

The Gauss-Seidel runtime scaling is initially super-linear which corresponds to the fact that the geometric domain partitioning does perfectly uniformly partition the off-diagonal elements of the W operator (contact interaction matrix). This effect depends on problem geometry can be alleviated by tuning of load balancing parameters.

In the below figure INIT stands for initialization. RUN stands for floating point computations. COM stands for communication not related to the middle set of constraints. MCOM stands for the communication of the middle set of constraint. The dashed line indicates linear scaling.

![http://wiki.solfec.googlecode.com/hg/bridge3.png](http://wiki.solfec.googlecode.com/hg/bridge3.png)

The below figure pictures the Gauss-Seidel iterations histories for 8, 16, 32 and 64 processors with step 1E-4 (left), and the 64 processor run histories for the time steps 1E-4, 2E-4, 5E-4 and 1E-3 (right). Clearly for the 1E-4 step runs, the number of iterations until **meritval** < 1E-5 does not depend on the number of processors. This is in agreement with the fact that our Gauss-Seidel implementation is consistent, while various processor numbers correspond only to varying orderings of contact points.

![http://wiki.solfec.googlecode.com/hg/bridge4.png](http://wiki.solfec.googlecode.com/hg/bridge4.png)

The last figure illustrates the constraints number histories for 8, 16, 32 and 64 processors with step 1E-4 (left), and the 64 processor run histories for the time steps 1E-4, 2E-4, 5E-4 and 1E-3 (right). These are in good agreement for all cases.

![http://wiki.solfec.googlecode.com/hg/bridge5.png](http://wiki.solfec.googlecode.com/hg/bridge5.png)