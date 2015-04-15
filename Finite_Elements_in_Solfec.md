# Summary #

Solfec includes Total Lagrangian (TL) and a novel body-co-rotational (BC) formulations of the Finite Element Method. The currently implemented first order formulation supports 4, 5, 6 and 8 node elements and the Saint Venant-Kirchhoff material model.

A distinct feature of our FEM implementation is the support for discretization of arbitrary shapes by means of a "rough background mesh". That is to say, motion of a complicated non-convex shape can be parametrized by just one (or few) finite element(s). This enables gradation of kinematic freedom in large multi-body contact problems, quite independent of the actual body shapes. One benefit of that is a more rational way of capturing of the shock waves (not possible for solely rigid models) in multibody structures. In addition the underlying algebraic implicit contact usually gets better conditioned with the increase of deformability.

# Efficiency #

For the moment Solfec is not optimized to work with large FEM meshes. Only for explicit time integration this will work well (matrices are diagonal and assembling contact equations is fast).  For implicit time integration using more than 1000 nodes will be inefficient. This will be addressed in future versions of the code.

# Example #

Here is an example of a soft non-convex brick in contact with a rigid obstacle. The shape of the body is quite complicated, with many small features. One can imagine, that discretizing it with a conforming FE mesh would be quite tedious and at the same time require a rather small time step. In a simulation involving thousands of such bodies this would be counterproductive. Hence, while maintaining the shape, we use only 8 background elements to parametrize the motion. The first video below shows deformation of the brick under the gravity load.

![http://wiki.solfec.googlecode.com/hg/soft-brick-1.gif](http://wiki.solfec.googlecode.com/hg/soft-brick-1.gif)
[watch on YouTube](http://www.youtube.com/watch?v=AOv2krrIllE)

The second video shows the same body - this time with the background mesh and the von Mises stress map. The background mesh can be generated with Solfec command:

```
mesh = ROUGH_HEX (shape, 2, 2, 2)
```

where the _shape_ is a collection of convex objects describing the actual geometry of the body. The above command simply generates a hexahedral mesh spanned along the eigen-directions of the geometry and properly containing the input shape.

![http://wiki.solfec.googlecode.com/hg/soft-brick-2.gif](http://wiki.solfec.googlecode.com/hg/soft-brick-2.gif)
[watch on YouTube](http://www.youtube.com/watch?v=FJNBYfwf-fM)