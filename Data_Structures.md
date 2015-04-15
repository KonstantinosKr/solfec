# Data structures #

![http://wiki.solfec.googlecode.com/hg/topics/ds/ds1.png](http://wiki.solfec.googlecode.com/hg/topics/ds/ds1.png)

[Link to the source code](http://code.google.com/p/solfec/source/browse/bod.h#126)

A multibody domain is partitioned among processors in order to balance the computational load. The two basic computational objects are bodies and constraints. The body structure is summarised in Table 1. A body stores its configuration q, velocity u and the inverse inertia operator M<sup>-1</sup> (a generalised counterpart in case of implicit time integration). It also stores references to all adjacent constraints. The kind property is specific to the parallel implementation.

There are three kinds of bodies:

  1. Parent bodies take part in time integration, contact detection and W assembling. They can migrate during load balancing and belong to processors whose domains contain their characteristic points (e.g. spatial mass centers). They are the bodies comprising the multibody domain (i.e. they have been defined by the user).
  1. Child bodies are copies of parent bodies on the non-parent processors whose domains are overlapped by the parents. They take part in contact detection and W assembling. They need to be updated by the parents.
  1. Dummy bodies are only used during W assembling. They are attached to constraints, whose one body is either a parent or a child and another body (neither a child nor a parent) must be brought in (and becomes a dummy).

Table 3 enumerates parent, child and dummy bodies in Figure 3. It has to be noted, that maintaining this kind of structure requires communication. That is to say, during load balancing copies of bodies need to be sent across a computer network. As the communication bandwidth is the most expensive resource during
distributed computing, our current design choice in Solfec is to maintain the complete sets of bodies on all processors, while sending only the necessary minimum of data. Hence, although each processors stores all bodies involved in the analysis, at a particular time only a fraction of them is actively used as parents, children and dummies. This, of course, is an optimization suitable in our context, where tens of thousands of relatively complex bodies need to be analysed. In case of simulations involving millions of bodies, it might be more efficient to avoid such redundancy. _The current (redundant body storage) solution will be made optional in the future versions of Solfec, while the more space efficient storage method will become a default_.

![http://wiki.solfec.googlecode.com/hg/topics/ds/ds2.png](http://wiki.solfec.googlecode.com/hg/topics/ds/ds2.png)

[Link to the source code](http://code.google.com/p/solfec/source/browse/dom.h#58)

The constraint structure is summarised in Table 2. It contains the constraint reaction R, the relative velocity U as well as the corresponding row of B and W of local dynamics (14). Similarly to R and U, B is a 3-vector while W are the non-zero 3x3 blocks of the corresponding block-row of W (the parallel domain partitioning implies then a row-partitioning of local dynamics). The master and slave pointers reference the attached bodies. The kind property is specific to the parallel implementation. There are two kinds of constraints:

  1. Internal constrains, which take part in load balancing and belong to specific processor domains. They are established between pairs of parent and child bodies (parent-parent, parent-child, child-child), both present on a given processor.
  1. External constraints, needed to compute a row of local dynamics corresponding to an internal constraint. They are established between dummy bodies and parent or child bodies.

Table 3 enumerates internal and external constraints in Figure 3. Unlike in the case of bodies, communication cost related to constraints cannot be optimized by a redundant data storage. Contact points are frequently added or removed which results in the need for constant updates of the constraints data. Nevertheless, although this is not pictured in Figure 3, the necessary data flow happens only in the vicinity of sub-domain boundaries, while large inner parts of the processors domains require no communication.

We use the Recursive Coordinate Bisection (RCB) implemented in [Zoltan](http://www.cs.sandia.gov/Zoltan/) in order to maintain the computational balance. RCB takes as an input a set of weighted points and outputs their assignments to processors. The space is partitioned along axis aligned planes, so that the resultant point clusters have well balanced total weights. Rebalancing after a change of the input is fast and incremental - consecutive partitionings are similar to each other. This helps to minimise communication.

![http://wiki.solfec.googlecode.com/hg/topics/ds/ds3.png](http://wiki.solfec.googlecode.com/hg/topics/ds/ds3.png)

![http://wiki.solfec.googlecode.com/hg/topics/ds/ds4.png](http://wiki.solfec.googlecode.com/hg/topics/ds/ds4.png)

Figure 3 illustrates a partitioned multibody domain. There are five bodies in the domain, marked from a to e. There are also four constraints numbered from 1 to 4. Bodies and constraints are associated with nine points: plus signs represent bodies while dots represent constraints. These are the input points used by the RCB algorithm. The actual locations of points representing bodies can be chosen in a variety of ways. In our case these are the mid-points of their axis aligned bounding boxes. The locations of points representing constraints are simpler to chose (e.g. contact points). Clearly, as far as load balancing is concerned, we do not discriminate between bodies and constraints. We use
a common point set and, in order to obtain a good balance, suitably adjust the weights. A quite simple approach works well in practice. Let us have a quick look at the constraint structure detailed in Table 2. A constraint con is
adjacent to at most two bodies: con.master and con.slave. In case of a singlebody constraint (e.g. prescribed displacement) only con.master is valid while con.slave = nil. In any case, the constraint weight is obtained as follows

![http://wiki.solfec.googlecode.com/hg/topics/ds/ds5.png](http://wiki.solfec.googlecode.com/hg/topics/ds/ds5.png)

where d<sub>m</sub> and d<sub>s</sub> denote dimensions of master and slave body velocities, s<sub>m</sub> and s<sub>s</sub> denote sizes of master and slave body constraint sets, and _factor_ is between 0 and 1. Above, we use the dimension of a body velocity as a basic indicator of the computational complexity. The meaning of expression (16) is easier understood when we first look at the case factor = 1. For a given constraint con, the corresponding row of local dynamics will have at most as many non-zero blocks as there are constraints adjacent to con.master and con.slave. Hence d<sub>m</sub>s<sub>m</sub>+d<sub>s</sub>s<sub>s</sub> approximates computational cost of dealing with a single row of contact dynamics. This choice produces well balanced local dynamics, but can in some cases lead to poor balancing of time integration or contact detection. By selecting factor < 1 one can improve the overall balance. Looking now at the body structure in Table 1, we can see that it contains the set of constraints adjacent to the body. We can then define the weight of a body as

![http://wiki.solfec.googlecode.com/hg/topics/ds/ds6.png](http://wiki.solfec.googlecode.com/hg/topics/ds/ds6.png)

The weight of a body equals to its configuration (tangent space) dimension plus the sum of weights of the attached single-body constraints. The last contribution corresponds to the fact, that in our implementation the single-body constraints migrate together with bodies. As a result only the bodies and the two-body constraints (e.g. contacts) migrate independently. The constraint weights are usually larger than those of bodies. This corresponds to the fact, that time integration and contact detection are much less time consuming, when compared to an implicit solution of constraint equations. Processing constraints drives the computational load. When it comes to balancing the load of contact detection, the above approach works well as long as the distribution of body shapes is sufficiently uniform in terms of their spatial complexity. This is the case in our context, as well as in many other scenarios comprising large numbers of interacting bodies.