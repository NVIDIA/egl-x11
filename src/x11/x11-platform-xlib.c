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

#include "x11-platform.h"

#include <assert.h>

#include <X11/Xlibint.h>
#include <X11/Xlib-xcb.h>

/**
 * This structure keeps track of a callback that we've registered with
 * XESetCloseDisplay.
 *
 * If the same underlying Display is used with more than one EGLDisplay, then
 * the EplDisplays will all share the same X11XlibDisplayClosedData struct.
 */
struct _X11XlibDisplayClosedData
{
    EplRefCount refcount;

    Display *xdpy;
    EGLBoolean closed;
    XExtCodes *ext_codes;

    struct glvnd_list entry;
};

static void CleanupDisplayClosedCallbacks(void) __attribute__((destructor));

static void eplX11DisplayClosedCallbackDataFree(X11XlibDisplayClosedData *callback);
static int OnXlibDisplayClosed(Display *xdpy, XExtCodes *codes);

/**
 * A linked list of all DisplayClosedCallbackData structs.
 *
 * This has to be a global array, because the callback for XESetCloseDisplay
 * doesn't have an extra parameter where we could stash an EplDisplay or
 * EplPlatformData pointer.
 *
 * That also means we need a reference count to know when to clean up this
 * list, in case loadEGLExternalPlatform gets called more than once.
 */
static struct glvnd_list display_close_callback_list = { &display_close_callback_list, &display_close_callback_list };
static pthread_mutex_t display_close_callback_list_mutex = PTHREAD_MUTEX_INITIALIZER;

EPL_REFCOUNT_DEFINE_TYPE_FUNCS(X11XlibDisplayClosedData, eplX11XlibDisplayClosedData,
        refcount, eplX11DisplayClosedCallbackDataFree);

PUBLIC EGLBoolean loadEGLExternalPlatform(int major, int minor,
                                   const EGLExtDriver *driver,
                                   EGLExtPlatform *extplatform)
{
    return eplX11LoadEGLExternalPlatformCommon(major, minor,
            driver, extplatform, EGL_PLATFORM_X11_KHR);
}

xcb_connection_t *eplX11GetXCBConnection(void *native_display, int *ret_screen)
{
    Display *xdpy = native_display;
    if (ret_screen != NULL)
    {
        *ret_screen = DefaultScreen(xdpy);
    }
    return XGetXCBConnection(xdpy);
}

static void RemoveDisplayClosedCallback(X11XlibDisplayClosedData *callback)
{
    glvnd_list_del(&callback->entry);
    if (callback->ext_codes != NULL)
    {
        XESetCloseDisplay(callback->xdpy, callback->ext_codes->extension, NULL);
        callback->ext_codes = NULL;
    }
    eplX11XlibDisplayClosedDataUnref(callback);
}

static void CleanupDisplayClosedCallbacks(void)
{
    pthread_mutex_lock(&display_close_callback_list_mutex);

    while (!glvnd_list_is_empty(&display_close_callback_list))
    {
        X11XlibDisplayClosedData *callback = glvnd_list_first_entry(&display_close_callback_list,
                X11XlibDisplayClosedData, entry);
        RemoveDisplayClosedCallback(callback);
    }

    pthread_mutex_unlock(&display_close_callback_list_mutex);
}

X11XlibDisplayClosedData *eplX11AddXlibDisplayClosedCallback(void *xlib_native_display)
{
    Display *xdpy = xlib_native_display;
    X11XlibDisplayClosedData *callback;

    pthread_mutex_lock(&display_close_callback_list_mutex);
    glvnd_list_for_each_entry(callback, &display_close_callback_list, entry)
    {
        if (callback->xdpy == xdpy)
        {
            eplX11XlibDisplayClosedDataRef(callback);
            pthread_mutex_unlock(&display_close_callback_list_mutex);
            return callback;
        }
    }

    // We haven't seen this display before, so add a new one.
    callback = malloc(sizeof(X11XlibDisplayClosedData));
    if (callback == NULL)
    {
        pthread_mutex_unlock(&display_close_callback_list_mutex);
        return NULL;
    }

    callback->ext_codes = XAddExtension(xdpy);
    if (callback->ext_codes == NULL)
    {
        pthread_mutex_unlock(&display_close_callback_list_mutex);
        free(callback);
        return NULL;
    }

    eplRefCountInit(&callback->refcount);
    callback->xdpy = xdpy;
    callback->closed = EGL_FALSE;
    XESetCloseDisplay(callback->xdpy, callback->ext_codes->extension, OnXlibDisplayClosed);

    eplX11XlibDisplayClosedDataRef(callback);
    glvnd_list_add(&callback->entry, &display_close_callback_list);

    pthread_mutex_unlock(&display_close_callback_list_mutex);

    return callback;
}

static int OnXlibDisplayClosed(Display *xdpy, XExtCodes *codes)
{
    X11XlibDisplayClosedData *callback, *callbackTmp;

    pthread_mutex_lock(&display_close_callback_list_mutex);
    glvnd_list_for_each_entry_safe(callback, callbackTmp, &display_close_callback_list, entry)
    {
        if (xdpy == callback->xdpy)
        {
            assert(codes == callback->ext_codes);
            assert(!callback->closed);
            callback->closed = EGL_TRUE;
            RemoveDisplayClosedCallback(callback);
            break;
        }
    }
    pthread_mutex_unlock(&display_close_callback_list_mutex);

    return 0;
}

static void eplX11DisplayClosedCallbackDataFree(X11XlibDisplayClosedData *callback)
{
    // We should have already unregistered the callback in OnXlibDisplayClosed
    // or eplX11CleanupPlatform.
    assert(callback->ext_codes == NULL);
    free(callback);
}

EGLBoolean eplX11IsNativeClosed(X11XlibDisplayClosedData *data)
{
    EGLBoolean ret = EGL_FALSE;

    if (data != NULL)
    {
        pthread_mutex_lock(&display_close_callback_list_mutex);
        ret = data->closed;
        pthread_mutex_unlock(&display_close_callback_list_mutex);
    }
    return ret;
}

