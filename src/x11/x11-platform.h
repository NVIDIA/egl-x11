/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * \file
 *
 * Common structs for the Xlib/XCB platform library.
 *
 * Note that internally, we use XCB for everything, which allows using mostly
 * the same code for both EGL_KHR_platform_x11 and EGL_EXT_platform_xcb.
 */

#ifndef X11_PLATFORM_H
#define X11_PLATFORM_H

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <xcb/xcb.h>
#include <gbm.h>

#include "platform-impl.h"
#include "platform-utils.h"
#include "driver-platform-surface.h"
#include "config-list.h"
#include "refcountobj.h"

#ifndef EGL_EXT_platform_xcb
#define EGL_EXT_platform_xcb 1
#define EGL_PLATFORM_XCB_EXT              0x31DC
#define EGL_PLATFORM_XCB_SCREEN_EXT       0x31DE
#endif /* EGL_EXT_platform_xcb */

/**
 * Keeps track of a callback that we've registered with XESetCloseDisplay.
 *
 * These are used to check whether a native display has been closed.
 */
typedef struct _X11XlibDisplayClosedData X11XlibDisplayClosedData;

EPL_REFCOUNT_DECLARE_TYPE_FUNCS(X11XlibDisplayClosedData, eplX11XlibDisplayClosedData);

/**
 * Platform-specific stuff for X11.
 *
 * Currently, this just includes the OpenGL and EGL functions that we'll need.
 */
struct _EplImplPlatform
{
    struct
    {
        PFNEGLQUERYDISPLAYATTRIBKHRPROC QueryDisplayAttribKHR;
        PFNEGLSWAPINTERVALPROC SwapInterval;
        PFNEGLQUERYDMABUFFORMATSEXTPROC QueryDmaBufFormatsEXT;
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC QueryDmaBufModifiersEXT;
        void (* Flush) (void);
        void (* Finish) (void);

        pfn_eglPlatformImportColorBufferNVX PlatformImportColorBufferNVX;
        pfn_eglPlatformFreeColorBufferNVX PlatformFreeColorBufferNVX;
        pfn_eglPlatformCreateSurfaceNVX PlatformCreateSurfaceNVX;
        pfn_eglPlatformSetColorBuffersNVX PlatformSetColorBuffersNVX;
        pfn_eglPlatformGetConfigAttribNVX PlatformGetConfigAttribNVX;
        pfn_eglPlatformCopyColorBufferNVX PlatformCopyColorBufferNVX;
        pfn_eglPlatformAllocColorBufferNVX PlatformAllocColorBufferNVX;
        pfn_eglPlatformExportColorBufferNVX PlatformExportColorBufferNVX;
    } egl;
};

/**
 * Contains per-display data that's initialized in eglInitialize and then
 * doesn't change until eglTerminate.
 *
 * It's not safe to lock a display during the window update callback, because
 * you can end up with a deadlock.
 *
 * Thankfully, all of the data that the update callback needs is static and
 * doesn't change after initialization, so we don't actually need a mutex to
 * protect it.
 *
 * So, this struct contains all of the per-display data that the update
 * callback will need.
 *
 * This struct is also refcounted. If the app calls eglTerminate while another
 * thread still holds a reference to an EplSurface, then the X11DisplayInstance
 * struct will stick around until it finishes cleanup up the surface.
 *
 * The one exception to all of this is the xcb_connection_t itself. If the
 * app calls XCloseDisplay while another thread is trying to send or receive X
 * protocol, then it'll crash. But, if that happens, then that's very
 * definitely an app bug.
 */
typedef struct
{
    EplRefCount refcount;

    /**
     * A reference to the \c EplPlatformData that this display came from.
     *
     * This is mainly here so that we can access the driver's EGL functions
     * without going through an EplDisplay, since this EplDisplayInstance might
     * have gone through an eglTerminate and eglInitialize, or might have been
     * destroyed entirely if we're going through teardown.
     */
    EplPlatformData *platform;

    /**
     * The display connection.
     */
    xcb_connection_t *conn;

    /**
     * True if the application passed NULL for the native display, so we had to
     * open our own display connection.
     */
    EGLBoolean own_display;

    /**
     * The internal (driver) EGLDisplay.
     */
    EplInternalDisplay *internal_display;

    /**
     * The screen number that we're talking to.
     */
    int screen;

    /**
     * The xcb_screen_t struct for the screen that we're talking to.
     */
    xcb_screen_t *xscreen;

    /**
     * The GBM device for whichever GPU we're rendering on.
     */
    struct gbm_device *gbmdev;

    /**
     * The EGL device that we're rendering on.
     */
    EGLDeviceEXT device;

    /**
     * The list of EGLConfigs.
     */
    EplConfigList *configs;
} X11DisplayInstance;

/**
 * Contains all of the data we need for an EGLDisplay.
 */
struct _EplImplDisplay
{
    /**
     * A copy of what the DISPLAY environment variable was when
     * eglGetPlatformDisplay was first called.
     */
    char *display_env;

    /**
     * The screen number that the application specified in the
     * eglGetPlatformDisplay attribute list, or -1 if the app didn't specify.
     */
    int screen_attrib;

    /**
     * A pointer to the X11DisplayInstance struct, or NULL if this display isn't initialized.
     */
    X11DisplayInstance *inst;

    /**
     * A callback to keep track of whether the native display has been closed.
     */
    X11XlibDisplayClosedData *closed_callback;
};

EPL_REFCOUNT_DECLARE_TYPE_FUNCS(X11DisplayInstance, eplX11DisplayInstance);

extern const EplImplFuncs X11_IMPL_FUNCS;

/**
 * Returns the xcb_connection_t and the screen number for a native xlib
 * Display.
 *
 * This is a separate function so that we can avoid depending on Xlib in the
 * XCB platform library.
 */
xcb_connection_t *eplX11GetXCBConnection(void *native_display, int *ret_screen);

/**
 * Registers a callback for when an Xlib Display is closed.
 *
 * Note that XCB doesn't have any equivalent to XESetCloseDisplay.
 */
X11XlibDisplayClosedData *eplX11AddXlibDisplayClosedCallback(void *xlib_native_display);

/**
 * Returns true if a native display has been closed.
 *
 * Note that this only works for an Xlib Display, because XCB doesn't have any
 * equivalent to XESetCloseDisplay.
 */
EGLBoolean eplX11IsNativeClosed(X11XlibDisplayClosedData *data);

EGLBoolean eplX11LoadEGLExternalPlatformCommon(int major, int minor,
        const EGLExtDriver *driver, EGLExtPlatform *extplatform,
        EGLint platform_enum);

EGLSurface eplX11CreatePixmapSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform);

EGLSurface eplX11CreateWindowSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform);

EGLBoolean eplX11SwapBuffers(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        const EGLint *rects, EGLint n_rects);

#endif // X11_PLATFORM_H
