# Introduction #

There are many computational codes handling constraints in the regularized way: violation of a constraint invokes a repulsive spring bringing things back into order. [A Wikipedia article](http://en.wikipedia.org/wiki/Discrete_element_method) provides summary. Non-regularized approaches, on the other hand, do not resort to correcting springs when constraints are violated. Instead, an implicit relation between the corrective force and a kinematic quantity (velocity or displacement) is used. [Another Wikipedia article](http://en.wikipedia.org/wiki/Contact_dynamics) provides a nutshell summary of the difference between the regularized and the non-regularized approaches.

Solfec implements a non-regularized formulation of constrained multi-body dynamics. Our point of departure is the Contact Dynamics method by Moreau  and Jean. The foreground goal is to deliver a user friendly, efficient and scalable implementation, bringing the non-regularized approaches somewhat closer to the engineering mainstream. The background goal concerns the means of doing that: we hope to deliver a research platform facilitating development of improved formulations and solution methods. [This paper](http://wiki.solfec.googlecode.com/hg/2010-Koziara-distributed-contact-dynamics.pdf) outlines the design of Solfec and gives some examples.

[Other codes](http://code.google.com/p/solfec/wiki/OtherCodes) as well implement the non-regularized formulation. To our knowledge Solfec is currently the only one supporting distributed memory parallelization.

# Current state #

At the current stage Solfec is fully functional, although not necessarily stable yet. It can nevertheless perform useful computations on hundreds of processors in reasonable time, if only used with sufficient care. The essential technical features of the code are:

  * Python based input language. Solfec extends Python by a set of objects and functions. It's input file is a Python code.

  * Binary, platform independent output. Currently the eXternal Data Representation (XDR) standard is used, while HDF5 in the testing phase now.

  * MPI based parallelization. Solfec employs MPI and [Zoltan](http://www.cs.sandia.gov/Zoltan/) in order to run in parallel.

  * OpenGL based graphical viewer. The viewer allows to assist in model creation as well as post-process the results. A future work is to implement a plug-in for one of the established open-source post-processors.

Perhaps more interesting are the modeling features currently implemented in Solfec:

  * Finite element based, sphere based or arbitrary convex polyhedron sum based geometry definition.

  * Automatic contact detection. Contact points are derived from volumetric overlaps between convex objects.

  * Rigid, pseudo-rigid, and finite element, co-rotational and modal kinematics. Definition of underlying kinematics is largely independent of the assumed shape, which allows to grade the kinematic freedom of input models. The finite element implementations allows to contain an arbitrary shape inside of a "rought" finite element mesh. The pseudo-rigid model on the other hand, places itself one step above the rigid model, by assuming a uniform deformation state across the whole body.

  * Explicit and linearly implicit time integrators. Solfec includes novel time integrators for rigid kinematics (e.g. an explicit scheme with small, negative energy drift). Linearly implicit integrators for deformable kinematics facilitate time integration with relatively large time steps.

  * Regularized, penalty based constraint solver. The classical spring-dashpot solver has been somewhat modified in order to bring more "implicitness" and stability (we have employed some bits of Contact Dynamics in order to use an implicit estimate of the gap function).

  * Non-regularized, block Gauss-Seidel constraint solver. Gauss-Seidel relaxation is a classical element of Contact Dynamics. Having sequential nature, it is also difficult  to implement efficiently in parallel. Nevertheless, we made our best effort to implement a scalable version of it, based on the ideas developed by Adams (originally for a multi-grid smoother).

# Ongoing and future work #

Here is a brief summary:

  * A projected quasi-Newton solver for the constraint equations. This approach is scalable due to reliance on matrix-vector product. The solver is available for testing although it might still undergo some changes over the next few months.

  * Other aspects of development concern physical issues such as energetic consistency, impact models, body cracking, etc.

  * Much testing, optimization and validation is underway.

# Efficiency #

Currently Solfec is optimized for models with thousands of bodies, each having less then ~1000 degrees of freedom. Only for explicit time integration Solfec will work well for large FEM meshes (matrices are diagonal and assembling contact equations is fast).  For implicit time integration using more than 1000 nodes will be inefficient. This will be addressed in future versions of the code.

# References #

J. J. Moreau. Numerical aspects of the sweeping process. Computer Methods in Applied Mechanics and Engineering, 177(3-4):329–349, 1999. [Link](http://linkinghub.elsevier.com/retrieve/pii/S0045782598003879)

M. Jean. The non-smooth contact dynamics method. Computer Methods in Applied Mechanics and Engineering, 177(3-4):235–257, 1999. [Link](http://linkinghub.elsevier.com/retrieve/pii/S0045782598003831)

M. F. Adams. A distributed memory unstructured Gauss-Seidel algorithm for multigrid smoothers. In Proceedings of the 2001 ACM/IEEE conference on Supercomputing, pp. 4–4, New York, 2001. [Link](http://portal.acm.org/citation.cfm?id=582038)

T. Koziara. Aspects of computational contact dynamics. PhD thesis, University of Glasgow, 2008. [Link](http://theses.gla.ac.uk/429/)

T. Koziara and N. Bićanić. A distributed memory parallel multibody Contact Dynamics code. Submitted, 2010. [Link](http://wiki.solfec.googlecode.com/hg/2010-Koziara-distributed-contact-dynamics.pdf)