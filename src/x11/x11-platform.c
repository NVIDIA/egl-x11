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

static const char *FORCE_ENABLE_ENV = "__NV_FORCE_ENABLE_X11_EGL_PLATFORM";

#define CLIENT_EXTENSIONS_XLIB "EGL_KHR_platform_x11 EGL_EXT_platform_x11"
#define CLIENT_EXTENSIONS_XCB "EGL_EXT_platform_xcb"

static const EGLint NEED_PLATFORM_SURFACE_MAJOR = 0;
static const EGLint NEED_PLATFORM_SURFACE_MINOR = 1;
static const uint32_t NEED_DRI3_MAJOR = 1;
static const uint32_t NEED_DRI3_MINOR = 2;
static const uint32_t NEED_PRESENT_MAJOR = 1;
static const uint32_t NEED_PRESENT_MINOR = 2;

static X11DisplayInstance *eplX11DisplayInstanceCreate(EplDisplay *pdpy, EGLBoolean from_init);
static void eplX11DisplayInstanceFree(X11DisplayInstance *inst);
EPL_REFCOUNT_DEFINE_TYPE_FUNCS(X11DisplayInstance, eplX11DisplayInstance, refcount, eplX11DisplayInstanceFree);

static void eplX11CleanupDisplay(EplDisplay *pdpy);

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
    EplPlatformData *plat = eplPlatformBaseAllocate(major, minor,
        driver, extplatform, platform_enum, &X11_IMPL_FUNCS,
        sizeof(EplImplPlatform));
    EGLBoolean timelineSupported = EGL_TRUE;
    pfn_eglPlatformGetVersionNVX ptr_eglPlatformGetVersionNVX;

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

void *eplX11GetHookFunction(EplPlatformData *plat, const char *name)
{
    return NULL;
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
 * \return EGL_TRUE on success, or EGL_FALSE if the attributes were invalid.
 */
static EGLBoolean ParseDisplayAttribs(EplPlatformData *plat, EGLint platform,
        const EGLAttrib *attribs, EGLBoolean report_errors, int *ret_screen)
{
    EGLint screenAttrib = EGL_NONE;
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

    return EGL_TRUE;
}

static EGLBoolean eplX11IsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs)
{
    // We don't have any other attributes yet
    int screen = -1;

    if (eplX11IsNativeClosed(pdpy->priv->closed_callback))
    {
        // This could happen if the application called XCloseDisplay, but then
        // a subsequent XOpenDisplay call happened to return a Display at the
        // same address. In that case, we still treat it as a different native
        // display.
        return EGL_FALSE;
    }

    if (!ParseDisplayAttribs(plat, platform, attribs, EGL_FALSE, &screen))
    {
        return EGL_FALSE;
    }

    if (pdpy->priv->screen_attrib != screen)
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

    if ((dev->available_nodes & (1 << DRM_NODE_PRIMARY)) == 0
            || dev->nodes[DRM_NODE_PRIMARY] == NULL)
    {
        return EGL_NO_DEVICE_EXT;
    }

    found = FindDeviceForNode(plat, dev->nodes[DRM_NODE_PRIMARY]);
    drmFreeDevice(&dev);
    return found;
}

static EplInternalDisplay *MakeInternalDisplay(EplPlatformData *plat, EGLDeviceEXT edev, int fd)
{
    EGLAttrib attribs[5] = {};
    EGLDisplay handle = EGL_NO_DISPLAY;
    int num = 0;

    if (plat->extensions.display_reference)
    {
        attribs[num++] = EGL_TRACK_REFERENCES_KHR;
        attribs[num++] = EGL_TRUE;
    }
    if (fd >= 0)
    {
        attribs[num++] = EGL_DRM_MASTER_FD_EXT;
        attribs[num++] = fd;
    }
    attribs[num] = EGL_NONE;

    handle = plat->egl.GetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, edev, attribs);
    if (handle == EGL_NO_DISPLAY)
    {
        return EGL_NO_DISPLAY;
    }

    return eplLookupInternalDisplay(plat, handle);
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
                &pdpy->priv->screen_attrib))
    {
        eplX11CleanupDisplay(pdpy);
        return EGL_FALSE;
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
static EGLBoolean CheckServerExtensions(xcb_connection_t *conn)
{
    const char *env;
    struct sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    xcb_generic_error_t *error = NULL;

    xcb_dri3_query_version_cookie_t dri3Cookie;
    xcb_dri3_query_version_reply_t *dri3Reply = NULL;
    xcb_present_query_version_cookie_t presentCookie;
    xcb_present_query_version_reply_t *presentReply = NULL;

    // Check to make sure that we're using a domain socket, since we need to be
    // able to push file descriptors through it.
    if (getsockname(xcb_get_file_descriptor(conn), &addr, &addrlen) != 0)
    {
        return EGL_FALSE;
    }
    if (addr.sa_family != AF_UNIX)
    {
        return EGL_FALSE;
    }

    if (xcb_get_extension_data(conn, &xcb_dri3_id) == NULL)
    {
        return EGL_FALSE;
    }
    if (xcb_get_extension_data(conn, &xcb_present_id) == NULL)
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
        xcb_query_extension_cookie_t extCookie = xcb_query_extension(conn,
                sizeof(NVGLX_EXTENSION_NAME) - 1, NVGLX_EXTENSION_NAME);
        xcb_query_extension_reply_t *extReply = xcb_query_extension_reply(conn, extCookie, &error);
        if (extReply == NULL)
        {
            // XQueryExtension isn't supposed to generate any errors.
            free(error);
            return EGL_FALSE;
        }

        if (extReply->present)
        {
            free(extReply);
            return EGL_FALSE;
        }
        free(extReply);
    }

    // TODO: Send these requests in parallel, not in sequence
    dri3Cookie = xcb_dri3_query_version(conn, NEED_DRI3_MAJOR, NEED_DRI3_MINOR);
    dri3Reply = xcb_dri3_query_version_reply(conn, dri3Cookie, &error);
    if (dri3Reply == NULL)
    {
        free(dri3Reply);
        return EGL_FALSE;
    }
    if (dri3Reply->major_version != NEED_DRI3_MAJOR || dri3Reply->minor_version < NEED_DRI3_MINOR)
    {
        free(dri3Reply);
        return EGL_FALSE;
    }
    free(dri3Reply);

    presentCookie = xcb_present_query_version(conn, NEED_PRESENT_MAJOR, NEED_PRESENT_MINOR);
    presentReply = xcb_present_query_version_reply(conn, presentCookie, &error);
    if (presentReply == NULL)
    {
        free(presentReply);
        return EGL_FALSE;
    }
    if (presentReply->major_version != NEED_PRESENT_MAJOR || presentReply->minor_version < NEED_PRESENT_MINOR)
    {
        free(presentReply);
        return EGL_FALSE;
    }
    free(presentReply);

    return EGL_TRUE;
}

X11DisplayInstance *eplX11DisplayInstanceCreate(EplDisplay *pdpy, EGLBoolean from_init)
{
    X11DisplayInstance *inst = NULL;
    int fd = -1;
    const char *gbmName = NULL;
    EplInternalDisplay *internalDpy = NULL;

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

    if (!CheckServerExtensions(inst->conn))
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

    inst->gbmdev = gbm_create_device(fd);
    if (inst->gbmdev == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Can't open GBM device");
        close(fd);
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    gbmName = gbm_device_get_backend_name(inst->gbmdev);
    if (gbmName == NULL || strcmp(gbmName, "nvidia") != 0)
    {
        if (from_init)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS, "X server is not running on an NVIDIA device");
        }
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    inst->device = FindDeviceForFD(pdpy->platform, fd);
    if (inst->device == EGL_NO_DEVICE_EXT)
    {
        if (from_init)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS, "Can't find matching EGLDevice");
        }
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    internalDpy = MakeInternalDisplay(pdpy->platform, inst->device, fd);
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

    if (!eplX11InitDriverFormats(pdpy->platform, inst))
    {
        // This should never happen. If it does, then we've got a problem in
        // the driver.
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "No supported image formats from driver");
        eplX11DisplayInstanceUnref(inst);
        return NULL;
    }

    // TODO: Check supported formats and modifiers, and build the EGLConfig
    // list.

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
    // Nothing to do yet.
}

static void eplX11FreeSurface(EplDisplay *pdpy, EplSurface *surf)
{
    // Nothing to do yet.
}

const EplImplFuncs X11_IMPL_FUNCS =
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
};
