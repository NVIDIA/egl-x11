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
#include <xcb/dri3.h>
#include <xcb/xproto.h>
#include <xcb/present.h>
#include <gbm.h>
#include <xf86drm.h>

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

#ifndef XCB_PRESENT_CAPABILITY_SYNCOBJ
#define XCB_PRESENT_CAPABILITY_SYNCOBJ 16
#endif

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
        PFNEGLCREATESYNCPROC CreateSync;
        PFNEGLDESTROYSYNCPROC DestroySync;
        PFNEGLWAITSYNCPROC WaitSync;
        PFNEGLDUPNATIVEFENCEFDANDROIDPROC DupNativeFenceFDANDROID;
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

    struct
    {
        xcb_void_cookie_t (* dri3_import_syncobj) (xcb_connection_t *c, uint32_t syncobj, xcb_drawable_t drawable, int32_t syncobj_fd);
        xcb_void_cookie_t (* dri3_free_syncobj) (xcb_connection_t *c, uint32_t syncobj);

        xcb_void_cookie_t (* present_pixmap_synced) (xcb_connection_t *c, xcb_window_t window,
                xcb_pixmap_t pixmap, uint32_t serial,
                xcb_xfixes_region_t valid, xcb_xfixes_region_t update, int16_t x_off, int16_t y_off,
                xcb_randr_crtc_t target_crtc,
                uint32_t acquire_syncobj, uint32_t release_syncobj,
                uint64_t acquire_point, uint64_t release_point,
                uint32_t options, uint64_t target_msc, uint64_t divisor, uint64_t remainder,
                uint32_t notifies_len, const xcb_present_notify_t *notifies);
    } xcb;

    struct
    {
        int (* GetCap) (int fd, uint64_t capability, uint64_t *value);
        int (* SyncobjCreate) (int fd, uint32_t flags, uint32_t *handle);
        int (* SyncobjDestroy) (int fd, uint32_t handle);
        int (* SyncobjHandleToFD) (int fd, uint32_t handle, int *obj_fd);
        int (* SyncobjFDToHandle) (int fd, int obj_fd, uint32_t *handle);
        int (* SyncobjImportSyncFile) (int fd, uint32_t handle, int sync_file_fd);
        int (* SyncobjExportSyncFile) (int fd, uint32_t handle, int *sync_file_fd);

        int (* SyncobjTimelineSignal) (int fd, const uint32_t *handles,
                            uint64_t *points, uint32_t handle_count);
        int (* SyncobjTimelineWait) (int fd, uint32_t *handles, uint64_t *points,
                          unsigned num_handles,
                          int64_t timeout_nsec, unsigned flags,
                          uint32_t *first_signaled);
        int (* SyncobjTransfer) (int fd,
                          uint32_t dst_handle, uint64_t dst_point,
                          uint32_t src_handle, uint64_t src_point,
                          uint32_t flags);
    } drm;

    EGLBoolean timeline_funcs_supported;
};

/**
 * Keeps track of format and format modifier support in the driver.
 *
 * This is used to cache the results of eglQueryDmaBufFormatsEXT and
 * eglQueryDmaBufModifiersEXT.
 */
typedef struct
{
    // Put the fourcc code as the first element so that we can use bsearch
    // with just the fourcc code for a key.
    uint32_t fourcc;
    const EplFormatInfo *fmt;
    uint64_t *modifiers;
    int num_modifiers;
    uint64_t *external_modifiers;
    int num_external_modifiers;
} X11DriverFormat;

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
     * If true, then always use of the indirect presentation path for PRIME.
     */
    EGLBoolean force_prime;

    /**
     * If true, then we can support the PRIME presentation path.
     *
     * Note that this isn't necessarily the same as
     * \c EplImplDisplay::enable_alt_device. The \c supports_prime flag means that
     * we can use the PRIME presentation path as needed on a per-window basis,
     * even if we're not doing cross-device presentation.
     */
    EGLBoolean supports_prime;

    /**
     * If true, then the driver supports EGL_ANDROID_native_fence_sync.
     */
    EGLBoolean supports_EGL_ANDROID_native_fence_sync;

    /**
     * If true, then the server supports implicit sync semantics.
     */
    EGLBoolean supports_implicit_sync;

    /**
     * If true, then we can use the new PresentPixmapSynced request for
     * synchronization on windows that support it.
     *
     * This means that we have the necessary EGL functions from the driver,
     * the necessary functions in libxcb and libdrm, and that the server
     * supports the necessary versions of the DRI3 and Present extensions.
     *
     * This does not account for the capabilities returned by the
     * PresentQueryCapabilties request. That's checked separately for each
     * window.
     */
    EGLBoolean supports_explicit_sync;

    /**
     * The list of EGLConfigs.
     */
    EplConfigList *configs;

    /**
     * The list of formats and modifiers that the driver supports.
     */
    X11DriverFormat *driver_formats;
    int num_driver_formats;
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
     * The EGLDeviceEXT handle that was specified with an EGL_DEVICE_EXT
     * attribute.
     */
    EGLDeviceEXT device_attrib;

    /**
     * The EGLDeviceEXT handle that we should use for rendering, or
     * EGL_NO_DEVICE_EXT to pick one during eglInitialize.
     *
     * This is set based on either the EGL_DEVICE_EXT attribute or based on
     * environment variables.
     */
    EGLDeviceEXT requested_device;

    /**
     * If true, allow picking a different GPU to do rendering.
     *
     * This is set based on the __NV_PRIME_RENDER_OFFLOAD environment variable.
     *
     * If the normal device (\c requested_device if it's set, the server's
     * device otherwise) isn't usable, then the \c enable_alt_device flag tells
     * eplX11DisplayInstanceCreate to pick a different device rather than just
     * fail.
     *
     * Note that this flag doesn't mean that we will use the PRIME presentation
     * path. It's possible that we'd pick the same device as the server anyway.
     *
     * Likewise, if the application passed an EGL_DISPLAY_EXT attribute, then
     * we might end up doing cross-device presentation even if the user doesn't
     * set __NV_PRIME_RENDER_OFFLOAD.
     */
    EGLBoolean enable_alt_device;

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
 * Returns the XID for the native surface handle in one of the
 * eglCreate*Surface functions.
 *
 * \param pdpy The EplDisplay struct
 * \param native_surface The native surface handle
 * \param create_platform True if this is for one of the
 *      eglCreatePlatform*Surface functions.
 * \return The XID value, or 0 if the native handle is invalid.
 */
uint32_t eplX11GetNativeXID(EplDisplay *pdpy, void *native_surface, EGLBoolean create_platform);

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

/**
 * Initializes the list of driver formats.
 *
 * \param plat The platform data.
 * \param inst The X11DisplayInstance to fill in.
 * \return EGL_TRUE on success, or EGL_FALSE on failure.
 */
EGLBoolean eplX11InitDriverFormats(EplPlatformData *plat, X11DisplayInstance *inst);

/**
 * Cleans up the format list that was initialized in eplX11InitDriverFormats.
 */
void eplX11CleanupDriverFormats(X11DisplayInstance *inst);

/**
 * Finds the X11DriverFormat struct for a given format.
 */
X11DriverFormat *eplX11FindDriverFormat(X11DisplayInstance *inst, uint32_t fourcc);

EGLBoolean eplX11InitConfigList(EplPlatformData *plat, X11DisplayInstance *inst);

/**
 * Returns the list of EGL attributes (not the buffers/internal attributes)
 * that should be passed to eglPlatformCreateSurfaceNVX.
 *
 * Currently, this just sets the EGL_WAYLAND_Y_INVERTED_WL flag to true, and
 * passes any other attributes through.
 *
 * \param plat The platform data
 * \param pdpy The display data
 * \param attribs The attribute list that was passed to eglCreateWindowSurface
 *      or eglCreatePixmapSurface.
 * \return The EGLAttrib array to pass to the driver, or NULL on error. The
 *      caller must free the array using free().
 */

EGLAttrib *eplX11GetInternalSurfaceAttribs(EplPlatformData *plat, EplDisplay *pdpy, const EGLAttrib *attribs);

EGLBoolean eplX11HookChooseConfig(EGLDisplay edpy, EGLint const *attribs,
        EGLConfig *configs, EGLint configSize, EGLint *numConfig);

EGLBoolean eplX11HookGetConfigAttrib(EGLDisplay edpy, EGLConfig config,
        EGLint attribute, EGLint *value);

void eplX11DestroyPixmap(EplSurface *surf);

EGLBoolean eplX11SwapInterval(EGLDisplay edpy, EGLint interval);

void eplX11DestroyWindow(EplSurface *surf);

void eplX11FreeWindow(EplSurface *surf);

EGLBoolean eplX11WaitGLWindow(EplDisplay *pdpy, EplSurface *psurf);

/**
 * A wrapper around the DMA_BUF_IOCTL_IMPORT_SYNC_FILE ioctl.
 *
 * This will check whether implicit sync is supporte, and if so, it will
 * plug a syncfd into a dma-buf.
 */
EGLBoolean eplX11ImportDmaBufSyncFile(X11DisplayInstance *inst, int dmabuf, int syncfd);
int eplX11ExportDmaBufSyncFile(X11DisplayInstance *inst, int dmabuf);

/**
 * Waits for a file descriptor to be ready using poll().
 *
 * Since this uses poll(), it can work with any arbitrary file descriptor
 * (including a syncfd or a dma-buf), but it does so with a CPU stall.
 *
 * \param syncfd The file descriptor to wait on.
 * \return EGL_TRUE on success, or EGL_FALSE on error.
 */
EGLBoolean eplX11WaitForFD(int syncfd);

#endif // X11_PLATFORM_H
