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

PUBLIC EGLBoolean loadEGLExternalPlatform(int major, int minor,
                                   const EGLExtDriver *driver,
                                   EGLExtPlatform *extplatform)
{
    return eplX11LoadEGLExternalPlatformCommon(major, minor,
            driver, extplatform, EGL_PLATFORM_XCB_EXT);
}

xcb_connection_t *eplX11GetXCBConnection(void *native_display, int *ret_screen)
{
    return NULL;
}

X11XlibDisplayClosedData *eplX11AddXlibDisplayClosedCallback(void *xlib_native_display)
{
    return NULL;
}

void eplX11XlibDisplayClosedDataUnref(X11XlibDisplayClosedData *data)
{
    // This should never be called with a non-NULL value, because we never
    // allocate a X11XlibDisplayClosedData struct.
    assert(data == NULL);
}

EGLBoolean eplX11IsNativeClosed(X11XlibDisplayClosedData *data)
{
    return EGL_FALSE;
}
