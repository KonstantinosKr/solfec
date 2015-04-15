# Basic time-stepping #

It will be useful to introduce some basic notions. Let us have a look at figure below.

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts1.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts1.png)

There are four bodies in the figure. Placement of each point of every body is determined by a configuration qi. Velocity of each point of every body is determined by a velocity ui. Let q and u collect configurations and velocities of all bodies. If the time history of velocity is known, the configuration time history can be computed as

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts2.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts2.png)

The velocity is determined by integrating Newton’s law

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts3.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts3.png)

where M is an inertia operator (assumed constant here), f is an out of balance force, H is a linear operator, and R collects some point forces R<sub>i</sub>. While integrating the motion of bodies, we keep track of a number of local coordinate systems (local frames). There are four of them in the figure above. Each local frame is related to a pair of points, usually belonging to two distinct bodies. An observer embedded at a local frame calculates the local relative velocity U of one of the points, viewed from the perspective of the other point. Let U collect all local velocities. Then, we can find a linear transformation H, such that

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts4.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts4.png)

In our case local frames correspond to constraints. We influence the local relative velocities by applying local forces R<sub>i</sub>. This can be collectively described by an implicit relation

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts5.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts5.png)

Hence, in order to integrate equations (1) and (2), at every instant of time we need to solve the implicit relation (4). Here is an example of a numerical approximation of such procedure

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts6.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts6.png)

where h is a discrete time step. As the time step h does not appear by M<sup>-1</sup>H<sup>T</sup>R, R should now be interpreted as an impulse. At a start we have

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts7.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts7.png)

The out of balance force

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts8.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts8.png)

incorporates both internal and external forces. The symmetric and positivedefinite inertia operator

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts9.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts9.png)

is computed once. The linear operator

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts10.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts10.png)

is computed at every time step. The number of rows of H depends on the number of constraints, while its rank is related to their linear independence. We then compute

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts11.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts11.png)

and

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts12.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts12.png)

which is symmetric and semi-positive definite. The linear transformation

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts13.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts13.png)

maps constraint reactions R into local relative velocities U = Hu<sup>t+h</sup> at time t + h. Relation (14) will be here referred to as the local dynamics. Finally

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts14.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts14.png)

where C is a nonlinear and usually nonsmooth operator. A basic Contact Dynamics algorithm can be summarised as follows:

![http://wiki.solfec.googlecode.com/hg/topics/bts/bts15.png](http://wiki.solfec.googlecode.com/hg/topics/bts/bts15.png)

It should be stressed that the above presentation exemplifies only a particular instance of Contact Dynamics. Let us refer the reader to the references below for further details.

# References #

J. J. Moreau. Unilateral Contact and Dry Friction in Finite Freedom Dynamics, volume 302 of Non-smooth Mechanics and Applications, CISM Courses and Lectures. Springer, Wien, 1988.

J. J. Moreau. Some basics of unilateral dynamics. In F. Pfeiffer and C. Glocker, editors, Unilateral Multibody Contacts. Kluwer, Dordrecht, 1999.

J. J. Moreau. Numerical aspects of the sweeping process. Computer Methods in Applied Mechanics and Engineering, 177(3-4):329–349, 1999. [Link](http://linkinghub.elsevier.com/retrieve/pii/S0045782598003879)

M. Jean. The non-smooth contact dynamics method. Computer Methods in Applied Mechanics and Engineering, 177(3-4):235–257, 1999. [Link](http://linkinghub.elsevier.com/retrieve/pii/S0045782598003831)

Bernard Brogliato. Nonsmooth Mechanics. Communications and Control Engineering. Springer Verlang, 1999.

Ch. Glocker. Set-Valued Force Laws, volume 1 of Lecture Notes in Applied and Computational Mechanics. Springer Verlang, 2001.

V. Acary and B. Brogliato. Numerical Methods for Nonsmooth Dynamical Systems, volume 35 of Lecture Notes in Applied Lecture Notes in Applied and Computational Mechanics. Springer Verlang, 2008.

R.I. Leine and N. van de Wouw. Stability and Convergence of Mechanical Systems with Unilateral Constraints, volume 36 of Lecture Notes in Applied and Computational Mechanics. Springer Verlang, 2008.

Christian Studer. Numerics of Unilateral Contacts and Friction, volume 47 of Lecture Notes in Applied and Computational Mechanics. Springer Verlang, 2009.

T. Koziara. Aspects of computational contact dynamics. PhD thesis, University of Glasgow, 2008. [Link](http://theses.gla.ac.uk/429/)