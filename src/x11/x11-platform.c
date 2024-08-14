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
 * Platform and display-handling code for X11.
 */

#include "x11-platform.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/socket.h>
#include <poll.h>
#include <assert.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <gbm.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/xproto.h>
#include <xcb/present.h>

#include "platform-utils.h"
#include "dma-buf.h"

static const char *FORCE_ENABLE_ENV = "__NV_FORCE_ENABLE_X11_EGL_PLATFORM";

#define CLIENT_EXTENSIONS_XLIB "EGL_KHR_platform_x11 EGL_EXT_platform_x11"
#define CLIENT_EXTENSIONS_XCB "EGL_EXT_platform_xcb"

static const EGLint NEED_PLATFORM_SURFACE_MAJOR = 0;
static const EGLint NEED_PLATFORM_SURFACE_MINOR = 1;
static const uint32_t NEED_DRI3_MAJOR = 1;
static const uint32_t NEED_DRI3_MINOR = 2;
static const uint32_t REQUEST_DRI3_MINOR = 4;
static const uint32_t NEED_PRESENT_MAJOR = 1;
static const uint32_t NEED_PRESENT_MINOR = 2;
static const uint32_t REQUEST_PRESENT_MINOR = 4;

static X11DisplayInstance *eplX11DisplayInstanceCreate(EplDisplay *pdpy, EGLBoolean from_init);
static void eplX11DisplayInstanceFree(X11DisplayInstance *inst);
EPL_REFCOUNT_DEFINE_TYPE_FUNCS(X11DisplayInstance, eplX11DisplayInstance, refcount, eplX11DisplayInstanceFree);

static void eplX11CleanupPlatform(EplPlatformData *plat);
static void eplX11CleanupDisplay(EplDisplay *pdpy);
static const char *eplX11QueryString(EplPlatformData *plat, EplDisplay *pdpy, EGLExtPlatformString name);
static void *eplX11GetHookFunction(EplPlatformData *plat, const char *name);
static EGLBoolean eplX11IsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs);
static EGLBoolean eplX11GetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy,
        void *native_display, const EGLAttrib *attribs,
        struct glvnd_list *existing_displays);
static EGLBoolean eplX11InitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor);
static void eplX11TerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy);
static void eplX11DestroySurface(EplDisplay *pdpy, EplSurface *surf);
static void eplX11FreeSurface(EplDisplay *pdpy, EplSurface *surf);
static EGLBoolean eplX11WaitGL(EplDisplay *pdpy, EplSurface *psurf);

static const EplHookFunc X11_HOOK_FUNCTIONS[] =
{
    { "eglChooseConfig", eplX11HookChooseConfig },
    { "eglGetConfigAttrib", eplX11HookGetConfigAttrib },
    { "eglSwapInterval", eplX11SwapInterval },
};
static const int NUM_X11_HOOK_FUNCTIONS = sizeof(X11_HOOK_FUNCTIONS) / sizeof(X11_HOOK_FUNCTIONS[0]);

static const EplImplFuncs X11_IMPL_FUNCS =
{
    .CleanupPlatform = eplX11CleanupPlatform,
    .QueryString = eplX11QueryString,
    .GetHookFunction = eplX11GetHookFunction,
    .IsSameDisplay = eplX11IsSameDisplay,
    .GetPlatformDisplay = eplX11GetPlatformDisplay,
    .CleanupDisplay = eplX11CleanupDisplay,
    .InitializeDisplay = eplX11InitializeDisplay,
    .TerminateDisplay = eplX11TerminateDisplay,
    .CreateWindowSurface = eplX11CreateWindowSurface,
    .CreatePixmapSurface = eplX11CreatePixmapSurface,
    .DestroySurface = eplX11DestroySurface,
    .FreeSurface = eplX11FreeSurface,
    .SwapBuffers = eplX11SwapBuffers,
    .WaitGL = eplX11WaitGL,
};

/**
 * True if the kernel might support DMA_BUF_IOCTL_IMPORT_SYNC_FILE and
 * DMA_BUF_IOCTL_EXPORT_SYNC_FILE.
 *
 * There's no direct way to query that support, so instead, if an ioctl fails,
 * then we set this flag to false so that we don't waste time trying again.
 */
static EGLBoolean import_sync_file_supported = EGL_TRUE;
static pthread_mutex_t import_sync_file_supported_mutex = PTHREAD_MUTEX_INITIALIZER;

static EGLBoolean LoadProcHelper(EplPlatformData *plat, void *handle, void **ptr, const char *name)
{
    *ptr = dlsym(handle, name);
    if (*ptr == NULL)
    {
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLBoolean eplX11LoadEGLExternalPlatformCommon(int major, int minor,
        const EGLExtDriver *driver, EGLExtPlatform *extplatform,
        EGLint platform_enum)
{
    EplPlatformData *plat = NULL;
    EGLBoolean timelineSupported = EGL_TRUE;
    pfn_eglPlatformGetVersionNVX ptr_eglPlatformGetVersionNVX;

    // Before we do anything else, make sure that we've got a recent enough
    // version of libgbm.
    if (dlsym(RTLD_DEFAULT, "gbm_bo_create_with_modifiers2") == NULL)
    {
        return EGL_FALSE;
    }

    plat = eplPlatformBaseAllocate(major, minor,
        driver, extplatform, platform_enum, &X11_IMPL_FUNCS,
        sizeof(EplImplPlatform));
    if (plat == NULL)
    {
        return EGL_FALSE;
    }

    ptr_eglPlatformGetVersionNVX = driver->getProcAddress("eglPlatformGetVersionNVX");
    if (ptr_eglPlatformGetVersionNVX == NULL
            || !EGL_PLATFORM_SURFACE_INTERFACE_CHECK_VERSION(ptr_eglPlatformGetVersionNVX(),
                NEED_PLATFORM_SURFACE_MAJOR, NEED_PLATFORM_SURFACE_MINOR))
    {
        // The driver doesn't support a compatible version of the platform
        // surface interface.
        eplPlatformBaseInitFail(plat);
        return EGL_FALSE;
    }

    plat->priv->egl.QueryDisplayAttribKHR = driver->getProcAddress("eglQueryDisplayAttribKHR");
    plat->priv->egl.SwapInterval = driver->getProcAddress("eglSwapInterval");
    plat->priv->egl.QueryDmaBufFormatsEXT = driver->getProcAddress("eglQueryDmaBufFormatsEXT");
    plat->priv->egl.QueryDmaBufModifiersEXT = driver->getProcAddress("eglQueryDmaBufModifiersEXT");
    plat->priv->egl.CreateSync = driver->getProcAddress("eglCreateSync");
    plat->priv->egl.DestroySync = driver->getProcAddress("eglDestroySync");
    plat->priv->egl.WaitSync = driver->getProcAddress("eglWaitSync");
    plat->priv->egl.DupNativeFenceFDANDROID = driver->getProcAddress("eglDupNativeFenceFDANDROID");
    plat->priv->egl.Flush = driver->getProcAddress("glFlush");
    plat->priv->egl.Finish = driver->getProcAddress("glFinish");
    plat->priv->egl.PlatformImportColorBufferNVX = driver->getProcAddress("eglPlatformImportColorBufferNVX");
    plat->priv->egl.PlatformFreeColorBufferNVX = driver->getProcAddress("eglPlatformFreeColorBufferNVX");
    plat->priv->egl.PlatformCreateSurfaceNVX = driver->getProcAddress("eglPlatformCreateSurfaceNVX");
    plat->priv->egl.PlatformSetColorBuffersNVX = driver->getProcAddress("eglPlatformSetColorBuffersNVX");
    plat->priv->egl.PlatformGetConfigAttribNVX = driver->getProcAddress("eglPlatformGetConfigAttribNVX");
    plat->priv->egl.PlatformCopyColorBufferNVX = driver->getProcAddress("eglPlatformCopyColorBufferNVX");
    plat->priv->egl.PlatformAllocColorBufferNVX = driver->getProcAddress("eglPlatformAllocColorBufferNVX");
    plat->priv->egl.PlatformExportColorBufferNVX = driver->getProcAddress("eglPlatformExportColorBufferNVX");

    if (plat->priv->egl.QueryDisplayAttribKHR == NULL
            || plat->priv->egl.SwapInterval == NULL
            || plat->priv->egl.QueryDmaBufFormatsEXT == NULL
            || plat->priv->egl.QueryDmaBufModifiersEXT == NULL
            || plat->priv->egl.CreateSync == NULL
            || plat->priv->egl.DestroySync == NULL
            || plat->priv->egl.WaitSync == NULL
            || plat->priv->egl.DupNativeFenceFDANDROID == NULL
            || plat->priv->egl.Finish == NULL
            || plat->priv->egl.Flush == NULL
            || plat->priv->egl.PlatformImportColorBufferNVX == NULL
            || plat->priv->egl.PlatformFreeColorBufferNVX == NULL
            || plat->priv->egl.PlatformCreateSurfaceNVX == NULL
            || plat->priv->egl.PlatformSetColorBuffersNVX == NULL
            || plat->priv->egl.PlatformGetConfigAttribNVX == NULL
            || plat->priv->egl.PlatformCopyColorBufferNVX == NULL
            || plat->priv->egl.PlatformAllocColorBufferNVX == NULL
            || plat->priv->egl.PlatformExportColorBufferNVX == NULL)
    {
        eplPlatformBaseInitFail(plat);
        return EGL_FALSE;
    }

#define LOAD_PROC(supported, prefix, group, name) \
    supported = supported && LoadProcHelper(plat, RTLD_DEFAULT, (void **) &plat->priv->group.name, prefix #name)

    // Load the functions that we'll need for explicit sync, if they're
    // available. If we don't find these, then it's not fatal.
    LOAD_PROC(timelineSupported, "xcb_", xcb, dri3_import_syncobj);
    LOAD_PROC(timelineSupported, "xcb_", xcb, dri3_free_syncobj);
    LOAD_PROC(timelineSupported, "xcb_", xcb, present_pixmap_synced);
    LOAD_PROC(timelineSupported, "drm", drm, GetCap);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjCreate);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjDestroy);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjHandleToFD);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjFDToHandle);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjImportSyncFile);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjExportSyncFile);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjTimelineSignal);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjTimelineWait);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjTransfer);

    plat->priv->timeline_funcs_supported = timelineSupported;

#undef LOAD_PROC

    eplPlatformBaseInitFinish(plat);
    return EGL_TRUE;
}

static void eplX11CleanupPlatform(EplPlatformData *plat)
{
    // Nothing to do here yet.
}

static const char *eplX11QueryString(EplPlatformData *plat, EplDisplay *pdpy, EGLExtPlatformString name)
{
    assert(plat != NULL);

    switch (name)
    {
        case EGL_EXT_PLATFORM_PLATFORM_CLIENT_EXTENSIONS:
            if (plat->platform_enum == EGL_PLATFORM_X11_KHR)
            {
                return CLIENT_EXTENSIONS_XLIB;
            }
            else if (plat->platform_enum == EGL_PLATFORM_XCB_EXT)
            {
                return CLIENT_EXTENSIONS_XCB;
            }
            else
            {
                assert(!"Can't happen: Invalid platform enum");
                return "";
            }
        case EGL_EXT_PLATFORM_DISPLAY_EXTENSIONS:
            return "";
        default:
            return NULL;
    }
}

static void *eplX11GetHookFunction(EplPlatformData *plat, const char *name)
{
    return eplFindHookFunction(X11_HOOK_FUNCTIONS, NUM_X11_HOOK_FUNCTIONS, name);
}

/**
 * Parses the attributes for an eglGetPlatformDisplay call.
 *
 * This is used to handle creating a new EGLDisplay and finding a matching
 * EGLDisplay.
 *
 * This mainly exists to deal with picking whether to look for
 * EGL_PLATFORM_X11_SCREEN_KHR or EGL_PLATFORM_XCB_SCREEN_EXT for the screen
 * number.
 *
 * \param plat The EplPlatformData pointer.
 * \param platform The platform enum, which should be either EGL_PLATFORM_X11_KHR
 *      or EGL_PLATFORM_XCB_EXT.
 * \param attribs The attribute array passed to eglGetPlatformDisplay.
 * \param report_errors If true, then report any invalid attributes.
 * \param[out] ret_screen Returns the screen number, or -1 if it wasn't
 *      specified.
 * \param[out] ret_device Returns the EGL_DEVICE_EXT attribute, or
 *      EGL_NO_DEVICE if it wasn't specified.
 * \return EGL_TRUE on success, or EGL_FALSE if the attributes were invalid.
 */
static EGLBoolean ParseDisplayAttribs(EplPlatformData *plat, EGLint platform,
        const EGLAttrib *attribs, EGLBoolean report_errors,
        int *ret_screen, EGLDeviceEXT *ret_device)
{
    EGLint screenAttrib = EGL_NONE;
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    int screen = -1;

    if (platform == EGL_PLATFORM_X11_KHR)
    {
        screenAttrib = EGL_PLATFORM_X11_SCREEN_KHR;
    }
    else if (platform == EGL_PLATFORM_XCB_EXT)
    {
        screenAttrib = EGL_PLATFORM_XCB_SCREEN_EXT;
    }
    else
    {
        if (report_errors)
        {
            // This shouldn't happen: The driver shouldn't have even called
            // into this library with the wrong platform enum.
            eplSetError(plat, EGL_BAD_PARAMETER, "Unsupported platform enum 0x%04x", platform);
        }
        return EGL_FALSE;
    }

    if (attribs != NULL)
    {
        int i;
        for (i=0; attribs[i] != EGL_NONE; i += 2)
        {
            if (attribs[i] == screenAttrib)
            {
                screen = (int) attribs[i + 1];
                if (screen < 0)
                {
                    if (report_errors)
                    {
                        eplSetError(plat, EGL_BAD_PARAMETER, "Invalid screen number %d", screen);
                    }
                    return EGL_FALSE;
                }
            }
            else if (attribs[i] == EGL_DEVICE_EXT)
            {
                device = (EGLDeviceEXT) attribs[i + 1];
            }
            else
            {
                if (report_errors)
                {
                    eplSetError(plat, EGL_BAD_ATTRIBUTE, "Invalid attribute 0x%lx", (unsigned long) attribs[i]);
                }
                return EGL_FALSE;
            }
        }
    }

    if (ret_screen != NULL)
    {
        *ret_screen = screen;
    }
    if (ret_device != NULL)
    {
        *ret_device = device;
    }

    return EGL_TRUE;
}

static EGLBoolean eplX11IsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs)
{
    int screen = -1;
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;

    if (eplX11IsNativeClosed(pdpy->priv->closed_callback))
    {
        // This could happen if the application called XCloseDisplay, but then
        // a subsequent XOpenDisplay call happened to return a Display at the
        // same address. In that case, we still treat it as a different native
        // display.
        return EGL_FALSE;
    }

    if (!ParseDisplayAttribs(plat, platform, attribs, EGL_FALSE, &screen, &device))
    {
        return EGL_FALSE;
    }

    if (pdpy->priv->screen_attrib != screen)
    {
        return EGL_FALSE;
    }
    if (pdpy->priv->device_attrib != device)
    {
        return EGL_FALSE;
    }

    return EGL_TRUE;
}

/**
 * Finds an EGLDeviceEXT handle that corresponds to a given DRI device node.
 */
static EGLDeviceEXT FindDeviceForNode(EplPlatformData *plat, const char *node)
{
    EGLDeviceEXT *devices = NULL;
    EGLDeviceEXT found = EGL_NO_DEVICE_EXT;
    EGLint num = 0;
    int i;

    if (!plat->egl.QueryDevicesEXT(0, NULL, &num) || num <= 0)
    {
        return EGL_NO_DEVICE_EXT;
    }

    devices = alloca(num * sizeof(EGLDeviceEXT));
    if (!plat->egl.QueryDevicesEXT(num, devices, &num) || num <= 0)
    {
        return EGL_NO_DEVICE_EXT;
    }

    for (i=0; i<num; i++)
    {
        const char *str = plat->egl.QueryDeviceStringEXT(devices[i], EGL_EXTENSIONS);
        if (!eplFindExtension("EGL_EXT_device_drm", str))
        {
            continue;
        }

        str = plat->egl.QueryDeviceStringEXT(devices[i], EGL_DRM_DEVICE_FILE_EXT);
        if (str != NULL && strcmp(str, node) == 0)
        {
            found = devices[i];
            break;
        }
    }

    return found;
}

/**
 * Returns an EGLDeviceEXT that corresponds to a device node.
 *
 * This is used to translate the file descriptor from DRI3Open into an
 * EGLDeviceEXT.
 */
static EGLDeviceEXT FindDeviceForFD(EplPlatformData *plat, int fd)
{
    drmDevice *dev = NULL;
    int ret;
    EGLDeviceEXT found = EGL_NO_DEVICE_EXT;

    ret = drmGetDevice(fd, &dev);
    if (ret != 0)
    {
        return EGL_NO_DEVICE_EXT;
    }

    if ((dev->available_nodes & (1 << DRM_NODE_PRIMARY)) != 0
            && dev->nodes[DRM_NODE_PRIMARY] != NULL)
    {
        EGLBoolean isNV = EGL_FALSE;

        /*
         * Call into libdrm to figure out whether this is an NVIDIA device
         * before we call eglQueryDevicesEXT.
         *
         * Calling eglQueryDevicesEXT could require waking up the GPU, which
         * can very slow and wastes battery.
         */
        if (dev->bustype == DRM_BUS_PCI)
        {
            // For a PCI device, just look at the PCI vendor ID.
            isNV = (dev->deviceinfo.pci->vendor_id == 0x10de);
        }
        else
        {
            // Tegra GPU's are not PCI devices, so for those, we have to check
            // the driver name instead.
            drmVersion *version = drmGetVersion(fd);
            if (version != NULL)
            {
                if (version->name != NULL)
                {
                    if (strcmp(version->name, "nvidia-drm") == 0
                            || strcmp(version->name, "tegra-udrm") == 0
                            || strcmp(version->name, "tegra") == 0)
                    {
                        isNV = EGL_TRUE;
                    }
                }
                drmFreeVersion(version);
            }
        }

        if (isNV)
        {
            found = FindDeviceForNode(plat, dev->nodes[DRM_NODE_PRIMARY]);
        }
    }

    drmFreeDevice(&dev);
    return found;
}

/**
 * Finds the xcb_screen_t for a screen number.
 */
static xcb_screen_t *GetXCBScreen(xcb_connection_t *conn, int screen)
{
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    int i;

    if (screen < 0 || iter.rem < screen)
    {
        return NULL;
    }
    for (i=0; screen; i++)
    {
        xcb_screen_next(&iter);
    }
    return iter.data;
}

/**
 * Sends a DRI3Open request, and returns the file descriptor that the server
 * sends back.
 */
static int GetDRI3DeviceFD(xcb_connection_t *conn, xcb_screen_t *xscr)
{
    xcb_generic_error_t *error = NULL;
    xcb_dri3_open_cookie_t cookie = xcb_dri3_open(conn, xscr->root, 0);
    xcb_dri3_open_reply_t *reply = xcb_dri3_open_reply(conn, cookie, &error);
    int fd;

    if (reply == NULL)
    {
        free(error);
        return -1;
    }
    assert(reply->nfd == 1);

    fd = xcb_dri3_open_reply_fds(conn, reply)[0];
    free(reply);

    return fd;
}

static EGLBoolean eplX11GetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy,
        void *native_display, const EGLAttrib *attribs,
        struct glvnd_list *existing_displays)
{
    const char *env;
    X11DisplayInstance *inst;

    env = getenv("DISPLAY");
    if (env == NULL && native_display == NULL)
    {
        return EGL_FALSE;
    }

    pdpy->priv = calloc(1, sizeof(EplImplDisplay));
    if (pdpy->priv == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        return EGL_FALSE;
    }
    if (env != NULL)
    {
        pdpy->priv->display_env = strdup(env);
        if (pdpy->priv->display_env == NULL)
        {
            eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
            eplX11CleanupDisplay(pdpy);
            return EGL_FALSE;
        }
    }

    if (!ParseDisplayAttribs(plat, pdpy->platform_enum, attribs, EGL_TRUE,
                &pdpy->priv->screen_attrib, &pdpy->priv->device_attrib))
    {
        eplX11CleanupDisplay(pdpy);
        return EGL_FALSE;
    }

    env = getenv("__NV_PRIME_RENDER_OFFLOAD_PROVIDER");
    if (env != NULL)
    {
        pdpy->priv->requested_device = FindDeviceForNode(plat, env);
        pdpy->priv->enable_alt_device = EGL_TRUE;
    }
    else
    {
        env = getenv("__NV_PRIME_RENDER_OFFLOAD");
        if (env != NULL && atoi(env) != 0)
        {
            pdpy->priv->enable_alt_device = EGL_TRUE;
        }
    }

    if (pdpy->priv->requested_device == EGL_NO_DEVICE_EXT)
    {
        // If the caller specified a device, then make sure it's valid.
        if (pdpy->priv->device_attrib != EGL_NO_DEVICE_EXT)
        {
            EGLint num = 0;
            EGLDeviceEXT *devices = eplGetAllDevices(plat, &num);
            EGLBoolean valid = EGL_FALSE;
            EGLint i;

            if (devices == NULL)
            {
                eplX11CleanupDisplay(pdpy);
                return EGL_FALSE;
            }

            for (i=0; i<num; i++)
            {
                if (devices[i] == pdpy->priv->device_attrib)
                {
                    valid = EGL_TRUE;
                    break;
                }
            }
            free(devices);

            if (valid)
            {
                // The requested device is a valid NVIDIA device, so use it.
                pdpy->priv->requested_device = pdpy->priv->device_attrib;
            }
            else if (pdpy->priv->enable_alt_device)
            {
                // The requested device is not an NVIDIA device, but PRIME is
                // enabled, so we'll pick an NVIDIA device during eglInitialize.
                pdpy->priv->requested_device = EGL_NO_DEVICE_EXT;
            }
            else
            {
                // The requested device is not an NVIDIA device and PRIME is not
                // enabled. Return failure to let another driver handle it.
                eplSetError(plat, EGL_BAD_MATCH, "Unknown or non-NV device handle %p",
                        pdpy->priv->device_attrib);
                eplX11CleanupDisplay(pdpy);
                return EGL_FALSE;
            }
        }
    }

    /*
     * Ideally, we'd wait until eglInitialize to open the connection or do the
     * rest of our compatibility checks, but we have to do that now to check
     * whether we can actually support whichever server we're connecting to.
     */
    inst = eplX11DisplayInstanceCreate(pdpy, EGL_FALSE);
    if (inst == NULL)
    {
        eplX11CleanupDisplay(pdpy);
        return EGL_FALSE;
    }
    eplX11DisplayInstanceUnref(inst);

    if (pdpy->platform_enum == EGL_PLATFORM_X11_KHR && native_display != NULL)
    {
        // Note that if this fails, then it's not necessarily fatal. We just
        // won't get a callback when the application calls XCloseDisplay, which
        // is no worse than with XCB.
        pdpy->priv->closed_callback = eplX11AddXlibDisplayClosedCallback(native_display);
    }

    return EGL_TRUE;
}

static void eplX11CleanupDisplay(EplDisplay *pdpy)
{
    if (pdpy->priv != NULL)
    {
        eplX11DisplayInstanceUnref(pdpy->priv->inst);
        eplX11XlibDisplayClosedDataUnref(pdpy->priv->closed_callback);
        free(pdpy->priv->display_env);
        free(pdpy->priv);
        pdpy->priv = NULL;
    }
}

/**
 * Checks whether the server has the necessary support that we need.
 *
 * This checks that we're using a domain socket, and checks the versions of the
 * DRI3 and Present extensions.
 */
static EGLBoolean CheckServerExtensions(X11DisplayInstance *inst)
{
    const char *env;
    struct sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    const xcb_query_extension_reply_t *extReply;
    xcb_generic_error_t *error = NULL;

    xcb_dri3_query_version_cookie_t dri3Cookie;
    xcb_dri3_query_version_reply_t *dri3Reply = NULL;
    xcb_present_query_version_cookie_t presentCookie;
    xcb_present_query_version_reply_t *presentReply = NULL;
    xcb_query_extension_reply_t *nvglxReply = NULL;
    EGLBoolean success = EGL_FALSE;

    // Check to make sure that we're using a domain socket, since we need to be
    // able to push file descriptors through it.
    if (getsockname(xcb_get_file_descriptor(inst->conn), &addr, &addrlen) != 0)
    {
        return EGL_FALSE;
    }
    if (addr.sa_family != AF_UNIX)
    {
        return EGL_FALSE;
    }

    extReply = xcb_get_extension_data(inst->conn, &xcb_dri3_id);
    if (extReply == NULL || !extReply->present)
    {
        return EGL_FALSE;
    }
    extReply = xcb_get_extension_data(inst->conn, &xcb_present_id);
    if (extReply == NULL || !extReply->present)
    {
        return EGL_FALSE;
    }

    env = getenv(FORCE_ENABLE_ENV);
    if (env == NULL || atoi(env) == 0)
    {
        /*
         * Check if the NV-GLX extension is present. If it is, then that means
         * we're talking to a normal X server running with the NVIDIA driver.
         * In that case, fail here so that the driver can use its normal X11
         * path.
         *
         * Note that if/when this replaces our existing X11 path for EGL, then
         * we could add some requests to NV-GLX to support older (pre DRI3 1.2)
         * servers or non-Linux systems.
         */
        const char NVGLX_EXTENSION_NAME[] = "NV-GLX";
        xcb_query_extension_cookie_t extCookie = xcb_query_extension(inst->conn,
                sizeof(NVGLX_EXTENSION_NAME) - 1, NVGLX_EXTENSION_NAME);
        nvglxReply = xcb_query_extension_reply(inst->conn, extCookie, &error);
        if (nvglxReply == NULL)
        {
            // XQueryExtension isn't supposed to generate any errors.
            goto done;
        }

        if (nvglxReply->present)
        {
            goto done;
        }
    }

    // TODO: Send these requests in parallel, not in sequence
    dri3Cookie = xcb_dri3_query_version(inst->conn, NEED_DRI3_MAJOR, REQUEST_DRI3_MINOR);
    dri3Reply = xcb_dri3_query_version_reply(inst->conn, dri3Cookie, &error);
    if (dri3Reply == NULL)
    {
        goto done;
    }
    if (dri3Reply->major_version != NEED_DRI3_MAJOR || dri3Reply->minor_version < NEED_DRI3_MINOR)
    {
        goto done;
    }

    presentCookie = xcb_present_query_version(inst->conn, NEED_PRESENT_MAJOR, REQUEST_PRESENT_MINOR);
    presentReply = xcb_present_query_version_reply(inst->conn, presentCookie, &error);
    if (presentReply == NULL)
    {
        goto done;
    }
    if (presentReply->major_version != NEED_PRESENT_MAJOR || presentReply->minor_version < NEED_PRESENT_MINOR)
    {
        goto done;
    }

    if (inst->platform->priv->timeline_funcs_supported
            && dri3Reply->minor_version >= 4
            && presentReply->minor_version >= 4)
    {
        /*
         * The server supports the necessary extension versions, and we've got
         * the necessary driver and library support in the client. Note that
         * we still have to send a PresentQueryCapabilities request in
         * eplX11CreateWindowSurface to check whether the server actually
         * supports timeline objects.
         */
        inst->supports_explicit_sync = EGL_TRUE;
    }

    success = EGL_TRUE;

done:
    free(nvglxReply);
    free(presentReply);
    free(dri3Reply);
    free(error);

    return success;
}

static EGLBoolean CheckServerFormatSupport(X11DisplayInstance *inst,
        EGLBoolean *ret_supports_direct, EGLBoolean *ret_supports_linear)
{
    const X11DriverFormat *fmt = NULL;
    xcb_dri3_get_supported_modifiers_cookie_t cookie;
    xcb_dri3_get_supported_modifiers_reply_t *reply = NULL;
    xcb_generic_error_t *error = NULL;
    int numScreenMods;
    const uint64_t *screenMods = NULL;
    int i, j;
    EGLBoolean found = EGL_FALSE;

    // Use XRGB8 to check for server support. With our driver, every format
    // should have the same set of modifiers, so we just need to pick something
    // that we'll always support.
    fmt = eplX11FindDriverFormat(inst, DRM_FORMAT_XRGB8888);

    cookie = xcb_dri3_get_supported_modifiers(inst->conn, inst->xscreen->root,
            eplFormatInfoDepth(fmt->fmt), fmt->fmt->bpp);
    reply = xcb_dri3_get_supported_modifiers_reply(inst->conn, cookie, &error);
    if (reply == NULL)
    {
        free(error);
        return EGL_FALSE;
    }

    numScreenMods = xcb_dri3_get_supported_modifiers_screen_modifiers_length(reply);
    screenMods = xcb_dri3_get_supported_modifiers_screen_modifiers(reply);

    *ret_supports_linear = EGL_FALSE;
    for (i=0; i<numScreenMods; i++)
    {
        if (screenMods[i] == DRM_FORMAT_MOD_LINEAR)
        {
            *ret_supports_linear = EGL_TRUE;
            break;
        }
    }

    for (i=0; i<numScreenMods && !found; i++)
    {
        for (j=0; j<fmt->num_modifiers && !found; j++)
        {
            if (screenMods[i] == fmt->modifiers[j])
            {
                found = EGL_TRUE;
            }
        }
    }
    *ret_supports_direct = found;

    free(reply);
    return EGL_TRUE;
}

X11DisplayInstance *eplX11DisplayInstanceCreate(EplDisplay *pdpy, EGLBoolean from_init)
{
    X11DisplayInstance *inst = NULL;
    int fd = -1;
    const char *gbmName = NULL;
    EGLDeviceEXT serverDevice = EGL_NO_DEVICE_EXT;
    EplInternalDisplay *internalDpy = NULL;
    EGLBoolean supportsDirect = EGL_FALSE;
    EGLBoolean supportsLinear = EGL_FALSE;

    inst = calloc(1, sizeof(X11DisplayInstance));
    if (inst == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Out of memory");
        return NULL;
    }
    eplRefCountInit(&inst->refcount);
    inst->screen = pdpy->priv->screen_attrib;
    inst->platform = eplPlatformDataRef(pdpy->platform);

    if (pdpy->native_display == NULL)
    {
        int xcbScreen = 0;
        inst->own_display = EGL_TRUE;
        inst->conn = xcb_connect(pdpy->priv->display_env, &xcbScreen);
        if (inst->conn == NULL)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS, "Can't open display connection");
            eplX11DisplayInstanceUnref(inst);
            return NULL;
        }
        if (inst->screen < 0)
        {
            inst->screen = xcbScreen;
        }
    }
    else
    {
        inst->own_display = EGL_FALSE;
        if (pdpy->platform_enum == EGL_PLATFORM_X11_KHR)
        {
            int xcbScreen = 0;
            inst->conn = eplX11GetXCBConnection(pdpy->native_display, &xcbScreen);

            if (inst->screen < 0)
            {
                inst->screen = xcbScreen;
            }
        }
        else
        {
            assert(pdpy->platform_enum == EGL_PLATFORM_XCB_EXT);
            inst->conn = (xcb_connection_t *) pdpy->native_display;
        }
    }

    if (inst->screen < 0)
    {
        // If we got here, then we're dealing with EGL_PLATFORM_XCB, and the
        // application didn't provide a screen number.
        char *host = NULL;
        int port = 0;

        assert(pdpy->platform_enum == EGL_PLATFORM_XCB_EXT);
        assert(!inst->own_display);

        if (!xcb_parse_display(pdpy->priv->display_env, &host, &port, &inst->screen) || inst->screen < 0)
        {
            inst->screen = 0;
        }
        free(host);
    }

    inst->xscreen = GetXCBScreen(inst->conn, inst->screen);
    if (inst->xscreen == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Invalid screen number");
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    if (!CheckServerExtensions(inst))
    {
        if (from_init)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS, "X server is missing required extensions");
        }
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    fd = GetDRI3DeviceFD(inst->conn, inst->xscreen);
    if (fd < 0)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Can't open DRI3 device");
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    serverDevice = FindDeviceForFD(pdpy->platform, fd);
    if (serverDevice != EGL_NO_DEVICE_EXT)
    {
        /*
         * The server is running on an NVIDIA device. NV->NV offloading doesn't
         * work, so our options are the server device or nothing.
         *
         * So, if the user/caller didn't request a specific device, or
         * requested the same device as the server device, then we're fine.
         *
         * Or, if PRIME is enabled, then we're allowed to pick a different
         * device, so we can still use the server device.
         */
        if (pdpy->priv->requested_device == EGL_NO_DEVICE_EXT
                || pdpy->priv->requested_device == serverDevice
                || pdpy->priv->enable_alt_device)
        {
            inst->device = serverDevice;
        }
        else
        {
            if (!from_init && pdpy->priv->device_attrib != EGL_NO_DEVICE_EXT)
            {
                eplSetError(pdpy->platform, EGL_BAD_MATCH, "NV -> NV offloading is not supported");
            }
            close(fd);
            eplX11DisplayInstanceUnref(inst);
            return NULL;
        }

        inst->supports_implicit_sync = EGL_FALSE;
    }
    else
    {
        /*
         * The server is not running on an NVIDIA device.
         *
         * If the user/caller requested a particular device, then use it.
         *
         * Otherwise, if PRIME is enabled, then we'll pick an arbitrary NVIDIA
         * device to use.
         *
         * Otherwise, we'll fail. If this is from eglGetPlatformDisplay, then
         * eglGetPlatformDisplay will fail and the next vendor library can try.
         */

        if (pdpy->priv->requested_device != EGL_NO_DEVICE_EXT)
        {
            // Pick whatever device the user/caller requested.
            inst->device = pdpy->priv->requested_device;
        }
        else if (pdpy->priv->enable_alt_device)
        {
            // If PRIME is enabled, then pick an NVIDIA device.
            EGLint num = 0;
            if (!pdpy->platform->egl.QueryDevicesEXT(1, &inst->device, &num) || num <= 0)
            {
                inst->device = EGL_NO_DEVICE_EXT;
            }
        }

        inst->supports_implicit_sync = EGL_TRUE;
    }

    if (inst->device == EGL_NO_DEVICE_EXT)
    {
        if (from_init)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS, "X server is not running on an NVIDIA device");
        }
        close(fd);
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    if (inst->device != serverDevice)
    {
        // If we're running on a different device than the server, then we need
        // to open the correct device node for GBM.
        const char *node;

        close(fd);

        node = pdpy->platform->egl.QueryDeviceStringEXT(inst->device, EGL_DRM_DEVICE_FILE_EXT);
        if (node == NULL)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS, "Can't find device node.");
            eplX11DisplayInstanceUnref(inst);
            return NULL;
        }

        fd = open(node, O_RDWR);
        if (fd < 0)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS, "Can't open device node %s", node);
            eplX11DisplayInstanceUnref(inst);
            return NULL;
        }

        inst->force_prime = EGL_TRUE;
    }

    inst->gbmdev = gbm_create_device(fd);
    if (inst->gbmdev == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Can't open GBM device");
        close(fd);
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    gbmName = gbm_device_get_backend_name(inst->gbmdev);
    if (gbmName == NULL || (strcmp(gbmName, "nvidia") != 0 && strcmp(gbmName, "nvidia_rm") != 0))
    {
        // This should never happen.
        eplSetError(pdpy->platform, EGL_BAD_ACCESS, "Internal error: GBM device is not an NVIDIA device");
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    internalDpy = eplGetDeviceInternalDisplay(pdpy->platform, inst->device);
    if (internalDpy == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Can't create internal EGLDisplay");
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }
    if (!eplInitializeInternalDisplay(pdpy->platform, internalDpy, NULL, NULL))
    {
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }
    inst->internal_display = eplInternalDisplayRef(internalDpy);

    if (pdpy->platform->priv->egl.PlatformCopyColorBufferNVX != NULL
            && pdpy->platform->priv->egl.PlatformAllocColorBufferNVX != NULL
            && pdpy->platform->priv->egl.PlatformExportColorBufferNVX != NULL
            && pdpy->platform->priv->egl.CreateSync != NULL
            && pdpy->platform->priv->egl.DestroySync != NULL
            && pdpy->platform->priv->egl.WaitSync != NULL
            && pdpy->platform->priv->egl.DupNativeFenceFDANDROID != NULL)
    {
        const char *extensions = pdpy->platform->egl.QueryString(internalDpy->edpy, EGL_EXTENSIONS);

        // NV -> NV offloading doesn't currently work, because with our
        // driver, the X server can't use a pitch linear buffer as a
        // pixmap.
        if (serverDevice == EGL_NO_DEVICE_EXT)
        {
            inst->supports_prime = EGL_TRUE;
        }

        if (eplFindExtension("EGL_ANDROID_native_fence_sync", extensions))
        {
            inst->supports_EGL_ANDROID_native_fence_sync = EGL_TRUE;
        }
    }

    if (!eplX11InitDriverFormats(pdpy->platform, inst))
    {
        // This should never happen. If it does, then we've got a problem in
        // the driver.
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "No supported image formats from driver");
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    if (!CheckServerFormatSupport(inst, &supportsDirect, &supportsLinear))
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Can't get a format modifier list from the X server");
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }
    if (!supportsLinear)
    {
        // If the server doesn't support pitch linear buffers, then we can't
        // use PRIME.
        inst->supports_prime = EGL_FALSE;
    }

    if (!supportsDirect)
    {
        // Note that this generally shouldn't happen unless we're using a
        // different device than the server. In any case, if the client and
        // server don't have any common format modifiers, then we'll have to
        // go through the PRIME path.
        inst->force_prime = EGL_TRUE;
    }

    if (!inst->supports_EGL_ANDROID_native_fence_sync)
    {
        // We need EGL_ANDROID_native_fence_sync to get a sync FD to plug in to
        // a timeline object. Likewise, implicit sync requires a sync FD to
        // attach to a dma-buf.
        inst->supports_explicit_sync = EGL_FALSE;
        inst->supports_implicit_sync = EGL_FALSE;
    }

    if (inst->supports_explicit_sync)
    {
        // Check if the DRM device supports timeline objects.
        uint64_t cap = 0;
        if (pdpy->platform->priv->drm.GetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0
                || cap == 0)
        {
            inst->supports_explicit_sync = EGL_FALSE;
        }
    }

    if (inst->force_prime && !inst->supports_prime)
    {
        if (from_init)
        {
            eplSetError(pdpy->platform, EGL_BAD_ALLOC, "No supported image formats from server");
        }
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    if (from_init)
    {
        if (!eplX11InitConfigList(pdpy->platform, inst))
        {
            eplX11DisplayInstanceUnref(inst);
            return NULL;
        }
    }

    return inst;
}

static void eplX11DisplayInstanceFree(X11DisplayInstance *inst)
{
    eplConfigListFree(inst->configs);
    inst->configs = NULL;

    eplX11CleanupDriverFormats(inst);

    if (inst->conn != NULL && inst->own_display)
    {
        xcb_disconnect(inst->conn);
    }
    inst->conn = NULL;

    if (inst->gbmdev != NULL)
    {
        int fd = gbm_device_get_fd(inst->gbmdev);
        gbm_device_destroy(inst->gbmdev);
        close(fd);
        inst->gbmdev = NULL;
    }

    if (inst->platform != NULL)
    {
        if (!inst->platform->destroyed)
        {
            if (inst->internal_display != NULL)
            {
                eplTerminateInternalDisplay(inst->platform, inst->internal_display);
            }
        }
        eplPlatformDataUnref(inst->platform);
    }
    eplInternalDisplayUnref(inst->internal_display);

    free(inst);
}

static EGLBoolean eplX11InitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor)
{
    assert(pdpy->priv->inst == NULL);

    if (eplX11IsNativeClosed(pdpy->priv->closed_callback))
    {
        eplSetError(plat, EGL_BAD_ACCESS, "The native display has been closed");
        return EGL_FALSE;
    }

    pdpy->priv->inst = eplX11DisplayInstanceCreate(pdpy, EGL_TRUE);
    if (pdpy->priv->inst == NULL)
    {
        return EGL_FALSE;
    }

    pdpy->internal_display = pdpy->priv->inst->internal_display->edpy;
    return EGL_TRUE;
}

static void eplX11TerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy)
{
    assert(pdpy->priv->inst != NULL);
    eplX11DisplayInstanceUnref(pdpy->priv->inst);
    pdpy->priv->inst = NULL;
}

static void eplX11DestroySurface(EplDisplay *pdpy, EplSurface *surf)
{
    if (surf->type == EPL_SURFACE_TYPE_WINDOW)
    {
        eplX11DestroyWindow(surf);
    }
    else if (surf->type == EPL_SURFACE_TYPE_PIXMAP)
    {
        eplX11DestroyPixmap(surf);
    }
    else
    {
        assert(!"Invalid surface type.");
    }
}

static void eplX11FreeSurface(EplDisplay *pdpy, EplSurface *surf)
{
    if (surf->type == EPL_SURFACE_TYPE_WINDOW)
    {
        eplX11FreeWindow(surf);
    }
}

static EGLBoolean eplX11WaitGL(EplDisplay *pdpy, EplSurface *psurf)
{
    EGLBoolean ret = EGL_TRUE;

    pdpy->platform->priv->egl.Finish();
    if (psurf != NULL && psurf->type == EPL_SURFACE_TYPE_WINDOW)
    {
        ret = eplX11WaitGLWindow(pdpy, psurf);
    }

    return ret;
}

EGLAttrib *eplX11GetInternalSurfaceAttribs(EplPlatformData *plat, EplDisplay *pdpy, const EGLAttrib *attribs)
{
    EGLAttrib *internalAttribs = NULL;
    int count = 0;
    int i;

    if (attribs != NULL)
    {
        for (count = 0; attribs[count] != EGL_NONE; i += 2)
        {
            if (attribs[count] == EGL_SURFACE_Y_INVERTED_NVX)
            {
                eplSetError(plat, EGL_BAD_ATTRIBUTE, "Invalid attribute 0x%04x\n", attribs[count]);
                return NULL;
            }
        }
    }

    internalAttribs = malloc((count + 3) * sizeof(EGLAttrib));
    if (internalAttribs == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory\n");
        return NULL;
    }

    memcpy(internalAttribs, attribs, count * sizeof(EGLAttrib));
    internalAttribs[count] = EGL_SURFACE_Y_INVERTED_NVX;
    internalAttribs[count + 1] = EGL_TRUE;
    internalAttribs[count + 2] = EGL_NONE;
    return internalAttribs;
}

static EGLBoolean eplX11CheckImportSyncFileSupported(void)
{
    EGLBoolean ret;
    pthread_mutex_lock(&import_sync_file_supported_mutex);
    ret = import_sync_file_supported;
    pthread_mutex_unlock(&import_sync_file_supported_mutex);
    return ret;
}

static void eplX11SetImportSyncFileUnsupported(void)
{
    pthread_mutex_lock(&import_sync_file_supported_mutex);
    import_sync_file_supported = EGL_FALSE;
    pthread_mutex_unlock(&import_sync_file_supported_mutex);
}

EGLBoolean eplX11ImportDmaBufSyncFile(X11DisplayInstance *inst, int dmabuf, int syncfd)
{
    EGLBoolean ret = EGL_FALSE;

    if (inst->supports_implicit_sync && eplX11CheckImportSyncFileSupported())
    {
        struct dma_buf_import_sync_file params = {};

        params.flags = DMA_BUF_SYNC_WRITE;
        params.fd = syncfd;
        if (drmIoctl(dmabuf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &params) == 0)
        {
            ret = EGL_TRUE;
        }
        else if (errno == ENOTTY || errno == EBADF || errno == ENOSYS)
        {
            eplX11SetImportSyncFileUnsupported();
        }
    }

    return ret;
}

int eplX11ExportDmaBufSyncFile(X11DisplayInstance *inst, int dmabuf)
{
    int fd = -1;

    if (inst->supports_implicit_sync && eplX11CheckImportSyncFileSupported())
    {
        struct dma_buf_export_sync_file params = {};
        params.flags = DMA_BUF_SYNC_WRITE;
        params.fd = -1;

        if (drmIoctl(dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &params) == 0)
        {
            fd = params.fd;
        }
        else if (errno == ENOTTY || errno == EBADF || errno == ENOSYS)
        {
            eplX11SetImportSyncFileUnsupported();
        }
    }

    return fd;
}

EGLBoolean eplX11WaitForFD(int syncfd)
{
    struct pollfd pfd;

    if (syncfd < 0)
    {
        return EGL_TRUE;
    }

    pfd.fd = syncfd;
    pfd.events = POLLIN;

    while (1)
    {
        int num = poll(&pfd, 1, -1);
        if (num == 1)
        {
            return EGL_TRUE;
        }
        else if (num < 0 && errno != EINTR)
        {
            return EGL_FALSE;
        }
    }
}

