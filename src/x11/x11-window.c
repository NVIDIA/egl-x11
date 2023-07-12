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
 * Window handling for X11.
 */

#include "x11-platform.h"

EGLSurface eplX11CreateWindowSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform)
{
    // Not implemented yet.
    return EGL_NO_SURFACE;
}

EGLBoolean eplX11SwapBuffers(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        const EGLint *rects, EGLint n_rects)
{
    // Not implemented yet.
    return EGL_FALSE;
}
