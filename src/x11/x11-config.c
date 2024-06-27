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
 * Stuff for handling EGLConfigs and formats.
 */

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>

#include "x11-platform.h"
#include "config-list.h"

static int CompareFormatSupportInfo(const void *p1, const void *p2)
{
    uint32_t v1 = *((const uint32_t *) p1);
    uint32_t v2 = *((const uint32_t *) p2);
    if (v1 < v2)
    {
        return -1;
    }
    else if (v1 > v2)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static EGLBoolean InitDriverFormatModifiers(EplPlatformData *plat,
        EGLDisplay internal_display, uint32_t fourcc, X11DriverFormat *support)
{
    const EplFormatInfo *fmt = eplFormatInfoLookup(fourcc);
    EGLuint64KHR *modifiers = NULL;
    EGLBoolean *external = NULL;
    EGLint num = 0;
    EGLint i;

    if (fmt == NULL)
    {
        return EGL_FALSE;
    }

    if (!plat->priv->egl.QueryDmaBufModifiersEXT(internal_display, fmt->fourcc, 0, NULL, NULL, &num) || num <= 0)
    {
        return EGL_FALSE;
    }

    modifiers = malloc(num * (sizeof(EGLuint64KHR) + sizeof(EGLBoolean)));
    if (modifiers == EGL_FALSE)
    {
        return EGL_FALSE;
    }
    external = (EGLBoolean *) (modifiers + num);

    if (!plat->priv->egl.QueryDmaBufModifiersEXT(internal_display, fmt->fourcc, num,
                modifiers, external, &num) || num <= 0)
    {
        num = 0;
    }

    support->fourcc = fmt->fourcc;
    support->fmt = fmt;
    support->num_modifiers = 0;
    support->external_modifiers = NULL;
    support->num_external_modifiers = 0;
    support->modifiers = malloc(num * sizeof(uint64_t));
    if (support->modifiers == NULL)
    {
        free(modifiers);
        return EGL_FALSE;
    }

    // Split the modifiers into renderable and external-only.
    for (i=0; i<num; i++)
    {
        if (!external[i])
        {
            support->modifiers[support->num_modifiers++] = modifiers[i];
        }
    }

    support->external_modifiers = support->modifiers + support->num_modifiers;
    for (i=0; i<num; i++)
    {
        if (external[i])
        {
            support->external_modifiers[support->num_external_modifiers++] = modifiers[i];
        }
    }
    free(modifiers);

    if (support->num_modifiers == 0)
    {
        free(support->modifiers);
        support->modifiers = NULL;

        return EGL_FALSE;
    }

    return EGL_TRUE;
}

EGLBoolean eplX11InitDriverFormats(EplPlatformData *plat, X11DisplayInstance *inst)
{
    EGLint *formats = NULL;
    EGLint num = 0;
    EGLint i;

    if (!plat->priv->egl.QueryDmaBufFormatsEXT(inst->internal_display->edpy,
                0, NULL, &num) || num <= 0)
    {
        return EGL_FALSE;
    }

    formats = malloc(num * sizeof(EGLint));
    if (formats == NULL)
    {
        return EGL_FALSE;
    }

    if (!plat->priv->egl.QueryDmaBufFormatsEXT(inst->internal_display->edpy,
                num, formats, &num) || num <= 0)
    {
        free(formats);
        return EGL_FALSE;
    }

    inst->driver_formats = calloc(1, num * sizeof(X11DriverFormat));
    if (inst->driver_formats == NULL)
    {
        free(formats);
        return EGL_FALSE;
    }

    inst->num_driver_formats = 0;
    for (i=0; i<num; i++)
    {
        if (InitDriverFormatModifiers(plat, inst->internal_display->edpy, formats[i],
                    &inst->driver_formats[inst->num_driver_formats]))
        {
            inst->num_driver_formats++;
        }
    }
    free(formats);

    if (inst->num_driver_formats == 0)
    {
        free(inst->driver_formats);
        inst->driver_formats = NULL;
        return EGL_FALSE;
    }

    // Sort the list by fourcc code.
    qsort(inst->driver_formats, inst->num_driver_formats,
            sizeof(X11DriverFormat), CompareFormatSupportInfo);

    return EGL_TRUE;
}

void eplX11CleanupDriverFormats(X11DisplayInstance *inst)
{
    if (inst->driver_formats != NULL)
    {
        int i;
        for (i=0; i<inst->num_driver_formats; i++)
        {
            free(inst->driver_formats[i].modifiers);
        }
        free(inst->driver_formats);
        inst->driver_formats = NULL;
    }
    inst->num_driver_formats = 0;
}

X11DriverFormat *eplX11FindDriverFormat(X11DisplayInstance *inst, uint32_t fourcc)
{
    return bsearch(&fourcc, inst->driver_formats, inst->num_driver_formats,
            sizeof(X11DriverFormat), CompareFormatSupportInfo);
}

static xcb_visualid_t FindVisualForFormat(EplPlatformData *plat, xcb_connection_t *conn, xcb_screen_t *xscreen, const EplFormatInfo *fmt)
{
    xcb_depth_iterator_t depthIter;
    int depth = fmt->colors[0] + fmt->colors[1] + fmt->colors[2] + fmt->colors[3];
    uint32_t red_mask   = ((1 << fmt->colors[0]) - 1) << fmt->offset[0];
    uint32_t green_mask = ((1 << fmt->colors[1]) - 1) << fmt->offset[1];
    uint32_t blue_mask  = ((1 << fmt->colors[2]) - 1) << fmt->offset[2];

    for (depthIter = xcb_screen_allowed_depths_iterator(xscreen);
            depthIter.rem > 0;
            xcb_depth_next(&depthIter))
    {
        if (depthIter.data->depth == depth)
        {
            xcb_visualtype_iterator_t visIter = xcb_depth_visuals_iterator(depthIter.data);

            for (visIter = xcb_depth_visuals_iterator(depthIter.data);
                    visIter.rem > 0;
                    xcb_visualtype_next(&visIter))
            {
                if (visIter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR
                        && visIter.data->red_mask == red_mask
                        && visIter.data->green_mask == green_mask
                        && visIter.data->blue_mask == blue_mask)
                {
                    return visIter.data->visual_id;
                }
            }
        }
    }

    return 0;
}
static void SetupConfig(EplPlatformData *plat, X11DisplayInstance *inst, EplConfig *config)
{
    X11DriverFormat *support = NULL;
    EGLint fourcc = DRM_FORMAT_INVALID;
    xcb_visualid_t visual;

    config->surfaceMask &= ~(EGL_WINDOW_BIT | EGL_PIXMAP_BIT);

    // Query the fourcc code from the driver.
    if (plat->priv->egl.PlatformGetConfigAttribNVX(inst->internal_display->edpy,
                config->config, EGL_LINUX_DRM_FOURCC_EXT, &fourcc))
    {
        config->fourcc = (uint32_t) fourcc;
    }
    else
    {
        config->fourcc = DRM_FORMAT_INVALID;
    }

    if (config->fourcc == DRM_FORMAT_INVALID)
    {
        // Without a format, we can't do anything with this config.
        return;
    }

    support = eplX11FindDriverFormat(inst, fourcc);
    if (support == NULL)
    {
        // The driver doesn't support importing a dma-buf with this format.
        return;
    }

    // We should be able to support pixmaps with any supported format, as long
    // as they have a supported modifier.
    config->surfaceMask |= EGL_PIXMAP_BIT;

    visual = FindVisualForFormat(inst->platform, inst->conn, inst->xscreen, support->fmt);
    if (visual != 0)
    {
        config->nativeVisualID = visual;
        config->nativeVisualType = XCB_VISUAL_CLASS_TRUE_COLOR;
        config->surfaceMask |= EGL_WINDOW_BIT;
    }
    else
    {
        config->nativeVisualType = EGL_NONE;
    }
}

EGLBoolean eplX11InitConfigList(EplPlatformData *plat, X11DisplayInstance *inst)
{
    int i;

    inst->configs = eplConfigListCreate(plat, inst->internal_display->edpy);
    if (inst->configs == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Can't find any usable EGLConfigs");
        return EGL_FALSE;
    }
    for (i=0; i<inst->configs->num_configs; i++)
    {
        SetupConfig(plat, inst, &inst->configs->configs[i]);
    }
    return EGL_TRUE;
}

static EGLBoolean FilterNativePixmap(EplDisplay *pdpy, EplConfig **configs, EGLint *count, xcb_pixmap_t xpix)
{
    xcb_generic_error_t *error = NULL;
    xcb_get_geometry_cookie_t geomCookie = xcb_get_geometry(pdpy->priv->inst->conn, xpix);
    xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(pdpy->priv->inst->conn, geomCookie, &error);

    xcb_dri3_buffers_from_pixmap_cookie_t buffersCookie;
    xcb_dri3_buffers_from_pixmap_reply_t *buffers = NULL;
    int32_t *fds = NULL;

    EGLint match = 0;
    EGLint i;

    if (geom == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_NATIVE_PIXMAP, "Invalid native pixmap 0x%x", xpix);
        free(error);
        return EGL_FALSE;
    }

    if (geom->root != pdpy->priv->inst->xscreen->root)
    {
        // TODO: Should this be an error, or should it just return zero matching configs?
        eplSetError(pdpy->platform, EGL_BAD_NATIVE_PIXMAP, "Pixmap 0x%x is on a different screen", xpix);
        free(geom);
        return EGL_FALSE;
    }

    // Filter out any configs that have the wrong depth.
    match = 0;
    for (i=0; i < *count; i++)
    {
        EplConfig *config = configs[i];
        const X11DriverFormat *fmt;

        if (!(config->surfaceMask & EGL_PIXMAP_BIT))
        {
            continue;
        }

        assert(config->fourcc != DRM_FORMAT_INVALID);

        fmt = eplX11FindDriverFormat(pdpy->priv->inst, config->fourcc);
        if (fmt == NULL)
        {
            // If the driver doesn't support this format, then we should never
            // have set EGL_PIXMAP_BIT on it.
            assert(!"Can't happen -- no driver support for format");
            continue;
        }

        if (eplFormatInfoDepth(fmt->fmt) != geom->depth)
        {
            continue;
        }

        configs[match++] = config;
    }
    free(geom);

    *count = match;
    if (match == 0)
    {
        return EGL_TRUE;
    }

    // To check the BPP and format modifiers, we have to use a
    // DRI3BuffersFromPixmap request.

    buffersCookie = xcb_dri3_buffers_from_pixmap(pdpy->priv->inst->conn, xpix);
    buffers = xcb_dri3_buffers_from_pixmap_reply(pdpy->priv->inst->conn, buffersCookie, &error);
    if (buffers == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_NATIVE_PIXMAP, "Can't look up dma-buf for pixmap 0x%x\n", xpix);
        free(error);
        return EGL_FALSE;
    }

    // We don't need the file descriptors, so close them now before we do
    // anything else.
    fds = xcb_dri3_buffers_from_pixmap_buffers(buffers);
    for (i=0; i<xcb_dri3_buffers_from_pixmap_buffers_length(buffers); i++)
    {
        close(fds[i]);
    }

    if (xcb_dri3_buffers_from_pixmap_buffers_length(buffers) != 1)
    {
        // We don't support pixmaps with more than one dma-buf.
        *count = 0;
        free(buffers);
        return EGL_TRUE;
    }

    match = 0;
    for (i=0; i<*count; i++)
    {
        EplConfig *config = configs[i];
        const X11DriverFormat *fmt = eplX11FindDriverFormat(pdpy->priv->inst, config->fourcc);

        if (fmt->fmt->bpp != buffers->bpp)
        {
            continue;
        }

        // With PRIME, we can support any pixmap in the server by allocating a
        // linear pixmap and then sending a CopyArea request. Without PRIME,
        // we're limited to whatever we can render to directly.
        if (!pdpy->priv->inst->supports_prime)
        {
            EGLint j;
            EGLBoolean supported = EGL_FALSE;

            for (j=0; j<fmt->num_modifiers; j++)
            {
                if (fmt->modifiers[j] == buffers->modifier)
                {
                    supported = EGL_TRUE;
                    break;
                }
            }

            if (!supported)
            {
                continue;
            }
        }

        configs[match++] = config;
    }
    *count = match;
    free(buffers);

    return EGL_TRUE;
}

EGLBoolean eplX11HookChooseConfig(EGLDisplay edpy, EGLint const *attribs,
        EGLConfig *configs, EGLint configSize, EGLint *numConfig)
{
    EplDisplay *pdpy;
    EGLint matchNativePixmap = XCB_PIXMAP_NONE;
    EGLBoolean success = EGL_FALSE;
    EplConfig **found = NULL;
    EGLint count = 0;

    pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    found = eplConfigListChooseConfigs(pdpy->platform, pdpy->internal_display,
            pdpy->priv->inst->configs, attribs, &count, &matchNativePixmap);
    if (found == NULL)
    {
        goto done;
    }

    if (matchNativePixmap != XCB_PIXMAP_NONE)
    {
        if (!FilterNativePixmap(pdpy, found, &count, matchNativePixmap))
        {
            goto done;
        }
    }

    success = EGL_TRUE;

done:
    if (success)
    {
        eplConfigListReturnConfigs(found, count, configs, configSize, numConfig);
    }
    free(found);
    eplDisplayRelease(pdpy);
    return success;
}

EGLBoolean eplX11HookGetConfigAttrib(EGLDisplay edpy, EGLConfig config,
        EGLint attribute, EGLint *value)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    EGLBoolean success = EGL_TRUE;

    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    success = eplConfigListGetAttribute(pdpy->platform, pdpy->internal_display,
            pdpy->priv->inst->configs, config, attribute, value);

    eplDisplayRelease(pdpy);

    return success;
}
