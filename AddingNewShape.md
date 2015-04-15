# Introduction #

I am currently adding an ellipsoid shape to Solfec, hence it is a good occasion to describe the general coding steps needed to do that. This HOWTO will grow as I go.

# General information #

A basic functionality for each shape category  (e.g. MESH, CONVEX, SPHERE) is implemented in separate source files (e.g. msh.h and msh.c for MESH or sph.h and sph.c for SPHERE, etc.). Then there is the general shape interface, implemented in shp.h and shp.c which in most cases simply invokes the methods that work on the basic shapes.

In case of ellipsoid I simply copied the sphere header sph.h. onto eli.h and then renamed the basic functions and redefined the main shape structure. Then I copied the eli.h onto the eli.c and created an empty implementation file.

My next step is to recall where all of the SPHERE related code sits in Solfec source files. I did grep "SPHERE `*`c" and noticed that there is some stuff in:

goc.c => this is where contact detection between different combinations of geometrical objects is implemented

lng.c => this is where the Python interface of the SPHERE object is defined

rnd.c => some code for rendering the SPHERE shape

shp.c => stores the higher level interface and uses the functions from sph.h (as already mentioned)

tsi.c  => well, this is a sphere-triangle intersection code, which is something else, non needed here

Now I have also recalled the the gjk.h and gjk.c implement the proximity query for basic convex primitives and it might be useful to include there the ellipsoid to ellipsoid query together with the remaining cases (e.g. to convex, to sphere).

Also in bod.c there is a BODY\_Material function which handles a switch over all shape types. This bit will also need little editing.

So in all of these files there is some work to be done, when adding a new shape. I shall put comments about each particular change in a random order first, and the tidy up this document.

# shp.h and box.h #

OK. I have just added

```
GOBJ_ELLIP = 0x10
```

to the **gobj** enum. Now I need to go to the box.h file (according to the comment near the **gobj** enum definition), where I will need to append the list of possible combinations of the basic geometrical primitives. I will need to

```
#define AABB_ELLIP_ELLIP  0x1010
#define AABB_ELEMENT_ELLIP 0x0110
```

end so on. Well, now I can see that I should redesign and automate this in the future.

Back to the shp.h file, where I needed to add

```
SHAPE_ELLIP
```

in the enum of the **kind** member of the **shape** structure. This is it here, for the moment.

# shp.c #

Here at the very beginning we need to add a whole bunch of new function pointers that will reference the suitable ELLIP functions. E.g.

```
static copy_func copy [] = {(copy_func)MESH_Copy,
                            (copy_func)CONVEX_Copy,
                            (copy_func)SPHERE_Copy,
                            (copy_func)ELLIP_Copy};
```

etc. Then we need to feel the gaps whenever there are switch (shp->kind) commands and where the SHAPE\_ELLIP case was not handled. The compiler will complain about that.

# boc.c #

In the BODY\_Material function we simply copy the SPHERE code onto a new ELLIP code in the **switch** block:

```
    case SHAPE_ELLIP:
  
      eli = shp->data;
      if (eli->volume == volume)
        eli->mat = mat;

      break;
```

This feature may never be used, but for the moment each analytical shape can carry an additional material pointer (different from the global body material type). Above, one of course needs to declare the _eli_ pointer and include the eli.h header.

# eli.h and eli.c #

The implementation of the basic shape functionality is of course needed. See the files for more information.

# box.c #

ELLIP\_Extents pointer needs to be added to the switch in SGP\_Extents\_Update.

# gjk.h and gjk.c #

Whenever the basic shape is convex, it will be useful to extend the GJK proximity computation functions so to include the new shape. In case of the ellipsoid four new queries have been added: convex-ellipsoid, sphere-ellipsoid, ellipsoid-ellipsoid and ellipsoid-point.

In every new case most of the old code can be reused. For example the conex-sphere query can be copied onto the new convex-ellipse query. The only big modification will be implementation of a new support function computation for each new shape.

# goc.c #

Now all the possible contact and distance queries need to be handled in the **gobjcontact** and **gobjdistance** functions. A lot of tedious work, but luckily the previous SPHERE related code can be used a start.

# rnd.c #

Rendering functions need to be added. Just follow the previous SPHERE related code and do quite a lot of copying, pasting and renaming to ELLIP.

# lng.c and doc/solfec.lyx #

Finally a Python object for the ellipsoid shape needs to be added and documented. One needs to bear in mind that the object itself is not enough. One needs to suitably extend functions that handle general shape arguments (e.g. ROTATE, COPY). It is enough to follow the code related to the SPHERE object and copy it accordingly.

# Example change sets #

These change sets exemplify implementation of the ellipsoid shape:

[one](http://code.google.com/p/solfec/source/detail?r=76484f70066d990765fe710a39c5b5feedcb7ed4)
[two](http://code.google.com/p/solfec/source/detail?r=e9e2abe178e087309cb81daa72b51a3ed2a14e6e)
[three](http://code.google.com/p/solfec/source/detail?r=b474905954c7155ed13c584a1949e083e259df43)

And  [some final smoothing here.](http://code.google.com/p/solfec/source/detail?r=81ab341f2daf2ccf7067339244c6fc508994fb1e) Look into goc.c and gjk.c only.