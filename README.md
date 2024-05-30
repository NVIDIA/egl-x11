NVIDIA XLib and XCB EGL Platform Library
================================

Overview
--------

This is an EGL platform library for the NVIDIA driver to support XWayland via
xlib (using `EGL_KHR_platform_x11`) or xcb (using `EGL_EXT_platform_xcb`).

Building and Installing
-----------------------

This library depends on:
- libxcb, libxcb-present, and libxcb-dri3, version 1.17.0
- libgbm, version 21.3.0
- libdrm, version 2.4.99
- libx11 (only if building the xlib library)
- EGL headers

In addition, this library depends on a (still somewhat experimental) interface
in the NVIDIA driver, which is supported only in 560 or later series drivers.

To build and install, use Meson:

```sh
meson builddir
ninja -C builddir
ninja -C builddir install
```
