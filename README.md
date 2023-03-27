NVIDIA EGL Platform Library Base
================================

This is a skeletal EGL platform library, with support functions to handle a lot
of the common bookkeeping that any platform library would need.

There's a lot of boilerplate and bookkeeping needed to keep track of internal
and external EGLDisplays, handle refcounting for eglInitialize and
eglTerminate, keep track of EGLSurfaces, and fudge EGLConfig attributes.

Most of that common bookkeeping is implemented in `platform-base.c`. That
common code then calls into platform-specific functions, which are declared in
`platform-impl.h`.

