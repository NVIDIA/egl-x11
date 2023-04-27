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

#ifndef CONFIG_LIST_H
#define CONFIG_LIST_H

/**
 * \file
 *
 * Functions to help deal with color formats and EGLConfigs.
 */

#include <stdint.h>
#include <EGL/egl.h>
#include <drm_fourcc.h>

#include "platform-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Contains some basic information about a fourcc format.
 *
 * This is used to match a format to things like an X11 visual.
 */
typedef struct
{
    uint32_t fourcc;
    int bpp;
    int colors[4];
    int offset[4];
} EplFormatInfo;

/**
 * Contains information about an EGLConfig.
 */
typedef struct
{
    /**
     * The EGLConfig handle.
     *
     * Note that this must be the first element in EplConfig.
     */
    EGLConfig config;

    /**
     * The fourcc format code. Currently set based on the color sizes.
     */
    uint32_t fourcc;

    /**
     * The value of EGL_SURFACE_TYPE for this config.
     *
     * This is initially set to whatever the driver hands back. The platform
     * library can then add EGL_WINDOW_BIT and EGL_PIXMAP_BIT as appropriate.
     */
    EGLint surfaceMask;

    /**
     * The value of EGL_NATIVE_VISUAL_ID. Initially set to zero.
     */
    EGLint nativeVisualID;

    /**
     * The value of EGL_NATIVE_VISUAL_TYPE.
     *
     * Initially set to EGL_NONE.
     */
    EGLint nativeVisualType;

    /**
     * The value of EGL_NATIVE_RENDERABLE.
     *
     * Initially set to EGL_FALSE.
     */
    EGLBoolean nativeRenderable;
} EplConfig;

/**
 * An array of known color formats.
 *
 * Note that this list is *not* sorted by fourcc value. It's sorted based on
 * which formats to pick if we don't know/care what the color order is.
 */
extern const EplFormatInfo FORMAT_INFO_LIST[];
extern const int FORMAT_INFO_COUNT;

typedef struct
{
    /**
     * A list of EplConfigs, sorted by the EGLConfig handle.
     */
    EplConfig *configs;
    EGLint num_configs;
} EplConfigList;

/**
 * Looks up all available EGLConfigs.
 *
 * \param edpy The internal EGLDisplay.
 * \return A new EplConfigList struct.
 */
EplConfigList *eplConfigListCreate(EplPlatformData *platform, EGLDisplay edpy);

void eplConfigListFree(EplConfigList *list);

/**
 * Looks up an EplConfig by its EGLConfig handle.
 *
 * \param list The EplConfigList to search.
 * \param config The EGLConfig handle to search for.
 * \return The matching EplConfig struct, or NULL if \p config was not found.
 */
EplConfig *eplConfigListFind(EplConfigList *list, EGLConfig config);

/**
 * A helper function for handling eglChooseConfig.
 *
 * This will fetch a list of EGLConfigs from the driver, and then filter that
 * list based on the EGL_SURFACE_TYPE, EGL_NATIVE_VISUAL_TYPE, and
 * EGL_NATIVE_RENDERABLE attributes.
 *
 * This does not filter based on EGL_MATCH_NATIVE_PIXMAP, since that requires
 * platform-specific code. Instead, if EGL_MATCH_NATIVE_PIXMAP is included in
 * \p attribs, then the value is returned in \p ret_native_pixmap. Otherwise,
 * \p ret_native_pixmap is left unchanged.
 *
 * This function returns a NULL-terminated array of EplConfig pointers, so the
 * caller can do any additional filtering as needed. You can use
 * eplConfigListReturnConfigs to copy the results to an EGLConfig array.
 *
 * \param platform The platform data.
 * \param edpy The internal EGLDisplay.
 * \param list The list to search.
 * \param attribs The attribute list, as provided by the application.
 * \param[out] ret_count Optionally returns the number of matching attributes,
 *      not including the NULL terminator.
 * \param[out] ret_native_pixmap Optionally returns the value of the
 *      EGL_MATCH_NATIVE_PIXMAP attribute. If that attribtue isn't in he list,
 *      then this is left unchanged.
 * \return A NULL-termianted array of EplConfigs, or NULL on error.
 */
EplConfig **eplConfigListChooseConfigs(EplPlatformData *platform, EGLDisplay edpy,
        EplConfigList *list, const EGLint *attribs,
        EGLint *ret_count, EGLint *ret_native_pixmap);

/**
 * Copies the EGLConfig handles from an EplConfig array to an EGLConfig array.
 *
 * This is used in conjunction with eplConfigListChooseConfigs to handle
 * eglChooseConfig.
 */
void eplConfigListReturnConfigs(EplConfig **configs, EGLint count,
        EGLConfig *ret_configs, EGLint max, EGLint *ret_count);

/**
 * A helper function for handling eglGetConfigAttrib.
 *
 * This function will fill in results for attributes that are stored in the
 * EplConfig struct.
 *
 * Currently, that includes EGL_SURFACE_TYPE, EGL_NATIVE_VISUAL_ID, and
 * EGL_NATIVE_VISUAL_TYPE, and EGL_NATIVE_RENDERABLE.
 *
 * For anything else, it will call through to the driver.
 */
EGLBoolean eplConfigListGetAttribute(EplPlatformData *platform, EGLDisplay edpy,
        EplConfigList *list, EGLConfig config, EGLint attribute, EGLint *value);

/**
 * Returns the index of an EplConfig.
 *
 * \param list The EplConfigList to search.
 * \param config The EGLConfig handle to search for.
 * \return The index of the matching EplConfig struct, or -1 if \p config was
 *      not found.
 */
EGLint eplConfigListFindIndex(EplConfigList *list, EGLConfig config);

const EplFormatInfo *eplFormatInfoLookup(uint32_t fourcc);

static inline int eplFormatInfoDepth(const EplFormatInfo *fmt)
{
    return fmt->colors[0] + fmt->colors[1] + fmt->colors[2] + fmt->colors[3];
}

#ifdef __cplusplus
}
#endif
#endif // CONFIG_LIST_H
