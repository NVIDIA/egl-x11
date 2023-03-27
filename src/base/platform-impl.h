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

#ifndef PLATFORM_IMPL_H
#define PLATFORM_IMPL_H

/**
 * \file
 *
 * Functions that the platform implementation must implement.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <eglexternalplatform.h>

#include "platform-base.h"

/**
 * The platform enum for eglGetPlatformDisplay (e.g., EGL_PLATFORM_X11_KHR).
 */
extern const EGLint EPLIMPL_PLATFORM_ENUM_VALUE;

/**
 * Does whatever initialization is needed when the library is loaded.
 *
 * This is called from the loadEGLExternalPlatform entrypoint. The base
 * EplPlatformData struct will already be filled in.
 */
EGLBoolean eplImplInitPlatform(EplPlatformData *plat, int major, int minor,
        const EGLExtDriver *driver, EGLExtPlatform *extplatform);

/**
 * Cleans up the platform data.
 */
void eplImplCleanupPlatform(EplPlatformData *plat);

/**
 * Handles the QueryString export.
 *
 * \param pdpy will either be a valid EplDisplay, or NULL if the caller passed
 *      in EGL_NO_DISPLAY.
 */
const char *eplImplQueryString(EplPlatformData *plat, EplDisplay *pdpy, EGLExtPlatformString name);

EGLBoolean eplImplIsValidNativeDisplay(EplPlatformData *plat, void *nativeDisplay);

/**
 * Returns the hook function for an EGL function.
 */
void *eplImplGetHookFunction(EplPlatformData *plat, const char *name);

/**
 * Returns true if two sets of display attributes should be considered
 * the same, taking into account any default values.
 *
 * If the implementation doesn't recognize an attribute, it may either
 * ignore the attribute or return false.
 */
EGLBoolean eplImplIsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs);

/**
 * Called to implement eglGetPlatformDisplay.
 *
 * The base EplDisplay struct will be filled in already, except for the
 * \c internal_display member.
 *
 * \return EGL_TRUE on success, or EGL_FALSE on failure.
 */
EGLBoolean eplImplGetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy,
        void *native_display, const EGLAttrib *attribs,
        struct glvnd_list *existing_displays);

/**
 * Cleans up any implementation data in an EplDisplay.
 *
 * If this is called during teardown, then the \c EplDisplay::platform pointer
 * may be NULL.
 */
void eplImplCleanupDisplay(EplDisplay *pdpy);

/**
 * Called to implement eglInitialize.
 */
EGLBoolean eplImplInitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor);
void eplImplTerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy);

/**
 * Creates an EGLSurface for a window.
 *
 * \param plat The EplPlatformData struct
 * \param pdpy The EplDisplay struct
 * \param psurf An EplSurface struct, with the base information filled in.
 * \param native_surface The native surface handle.
 * \param attribs The attribute list.
 * \param create_platform If this is true, then the call is from
 *      eglCreatePlatformWindowSurface. If false, it's from
 *      eglCreateWindowSurface.
 * \return The internal EGLSurface handle, or EGL_NO_SURFACE on failure.
 */
EGLSurface eplImplCreateWindowSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform);

/**
 * Creates an EGLSurface for a pixmap.
 *
 * \param plat The EplPlatformData struct
 * \param pdpy The EplDisplay struct
 * \param psurf An EplSurface struct, with the base information filled in.
 * \param native_surface The native surface handle.
 * \param attribs The attribute list.
 * \param create_platform If this is true, then the call is from
 *      eglCreatePlatformPixmapSurface. If false, it's from
 *      eglCreatePixmapSurface.
 * \return The internal EGLSurface handle, or EGL_NO_SURFACE on failure.
 */
EGLSurface eplImplCreatePixmapSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform);

/**
 * Called to handle eglDestroySurface and eglTerminate.
 *
 * Note that it's possible that the EplSurface struct itself might stick around
 * if another thread is holding a reference to it.
 *
 * This is also called if the platform library gets unloaded. In that case,
 * EplDisplay::platform will be NULL.
 */
void eplImplDestroySurface(EplDisplay *pdpy, EplSurface *psurf);

/**
 * Called when an EplSurface is about to be freed.
 *
 * At this point, it's safe to assume that no other thread is going to touch
 * the surface, so the platform must free anything that it hasn't already freed
 * in eplImplDestroySurface.
 */
void eplImplFreeSurface(EplDisplay *pdpy, EplSurface *psurf);

/**
 * Implements eglSwapBuffers and eglSwapBuffersWithDamageEXT.
 *
 * If the application calls eglSwapBuffers, then \p rects will be NULL and
 * \p n_rects will be zero.
 */
EGLBoolean eplImplSwapBuffers(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf,
        const EGLint *rects, EGLint n_rects);

#endif // PLATFORM_IMPL_H
