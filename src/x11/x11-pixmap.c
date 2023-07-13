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
 * Pixmap handling for X11.
 *
 * Pixmaps are a lot simpler than windows, since we only have one buffer and we
 * never need to reallocate it.
 */

#include "x11-platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/xproto.h>
#include <drm_fourcc.h>

/**
 * Data for an X11 pixmap.
 *
 * A pixmap is a lot simpler than a window, because there's only one buffer,
 * and we never have to swap or resize it. So, we just import a dma-buf for the
 * pixmap when we first create the EGLSurface, and then we don't have to change
 * anything after that.
 */
typedef struct
{
    X11DisplayInstance *inst;
    xcb_pixmap_t xpix;
    EGLPlatformColorBufferNVX buffer;
} X11Pixmap;

/**
 * Fetches the dma-buf for a Pixmap from the server and creates an
 * EGLExtColorBuffer in the driver for it.
 */
static EGLPlatformColorBufferNVX ImportPixmap(X11DisplayInstance *inst, xcb_pixmap_t xpix,
        const EplFormatInfo *fmt, uint32_t width, uint32_t height)
{
    xcb_generic_error_t *error = NULL;
    xcb_dri3_buffers_from_pixmap_cookie_t cookie;
    xcb_dri3_buffers_from_pixmap_reply_t *reply = NULL;
    EGLPlatformColorBufferNVX buffer = NULL;
    int depth = fmt->colors[0] + fmt->colors[1] + fmt->colors[2] + fmt->colors[3];

    cookie = xcb_dri3_buffers_from_pixmap(inst->conn, xpix);
    reply = xcb_dri3_buffers_from_pixmap_reply(inst->conn, cookie, &error);
    if (reply == NULL)
    {
        free(error);
        goto done;
    }

    if (xcb_dri3_buffers_from_pixmap_buffers_length(reply) != 1)
    {
        goto done;
    }

    if (reply->depth != depth)
    {
        eplSetError(inst->platform, EGL_BAD_MATCH,
                "Pixmap 0x%x has depth %d, but EGLConfig requires depth %d\n",
                xpix, reply->depth, depth);
        goto done;
    }
    if (reply->bpp != fmt->bpp)
    {
        eplSetError(inst->platform, EGL_BAD_MATCH,
                "Pixmap 0x%x has bpp %d, but EGLConfig requires bpp %d\n",
                xpix, reply->bpp, fmt->bpp);
        goto done;
    }

    buffer = inst->platform->priv->egl.PlatformImportColorBufferNVX(inst->internal_display->edpy,
            xcb_dri3_buffers_from_pixmap_buffers(reply)[0],
            width, height, fmt->fourcc,
            xcb_dri3_buffers_from_pixmap_strides(reply)[0],
            xcb_dri3_buffers_from_pixmap_offsets(reply)[0],
            reply->modifier);
    if (buffer == NULL)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to import dma-buf for pixmap");
        goto done;
    }

done:
    if (reply != NULL)
    {
        int32_t *fds = xcb_dri3_buffers_from_pixmap_buffers(reply);
        int i;
        for (i=0; i<xcb_dri3_buffers_from_pixmap_buffers_length(reply); i++)
        {
            close(fds[i]);
        }
        free(reply);
    }
    return buffer;
}

void eplX11DestroyPixmap(EplSurface *surf)
{
    X11Pixmap *ppix = (X11Pixmap *) surf->priv;
    surf->priv = NULL;
    if (ppix != NULL)
    {
        if (ppix->inst != NULL)
        {
            if (surf->internal_surface != EGL_NO_SURFACE)
            {
                ppix->inst->platform->egl.DestroySurface(ppix->inst->internal_display->edpy, surf->internal_surface);
            }
            if (ppix->buffer != NULL && !ppix->inst->platform->destroyed)
            {
                ppix->inst->platform->priv->egl.PlatformFreeColorBufferNVX(ppix->inst->internal_display->edpy, ppix->buffer);
                ppix->buffer = NULL;
            }
            eplX11DisplayInstanceUnref(ppix->inst);
        }
        free(ppix);
    }
}

EGLSurface eplX11CreatePixmapSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform)
{
    X11DisplayInstance *inst = pdpy->priv->inst;
    xcb_pixmap_t xpix = 0;
    X11Pixmap *ppix = NULL;
    const EplConfig *configInfo;
    const EplFormatInfo *fmt;
    xcb_get_geometry_cookie_t geomCookie;
    xcb_get_geometry_reply_t *geomReply = NULL;
    xcb_generic_error_t *error = NULL;
    EGLSurface esurf = EGL_NO_SURFACE;
    EGLAttrib buffers[] = { GL_BACK, 0, EGL_NONE };

    if (create_platform)
    {
        if (pdpy->platform_enum == EGL_PLATFORM_X11_KHR)
        {
            xpix = (uint32_t) *((unsigned long *) native_surface);
        }
        else
        {
            xpix = *((uint32_t *) native_surface);
        }
    }
    else
    {
        xpix = (xcb_pixmap_t) ((uintptr_t) native_surface);
    }

    configInfo = eplConfigListFind(inst->configs, config);
    if (configInfo == NULL)
    {
        eplSetError(plat, EGL_BAD_CONFIG, "Invalid EGLConfig %p", config);
        return EGL_NO_SURFACE;
    }
    if (!(configInfo->surfaceMask & EGL_PIXMAP_BIT))
    {
        eplSetError(plat, EGL_BAD_CONFIG, "EGLConfig %p does not support pixmaps", config);
        return EGL_NO_SURFACE;
    }
    fmt = eplFormatInfoLookup(configInfo->fourcc);
    assert(fmt != NULL);

    geomCookie = xcb_get_geometry(inst->conn, xpix);
    geomReply = xcb_get_geometry_reply(inst->conn, geomCookie, &error);
    if (geomReply == NULL)
    {
        eplSetError(plat, EGL_BAD_NATIVE_PIXMAP, "Invalid pixmap 0x%x", xpix);
        goto done;
    }
    if (geomReply->root != inst->xscreen->root)
    {
        eplSetError(plat, EGL_BAD_NATIVE_PIXMAP, "Pixmap 0x%x is on the wrong screen", xpix);
        goto done;
    }
    if (geomReply->width <= 0 || geomReply->height <= 0)
    {
        eplSetError(plat, EGL_BAD_NATIVE_PIXMAP, "Invalid pixmap size");
        goto done;
    }

    ppix = calloc(1, sizeof(X11Pixmap));
    if (ppix == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }
    surf->priv = (EplImplSurface *) ppix;
    ppix->inst = eplX11DisplayInstanceRef(inst);
    ppix->xpix = xpix;

    ppix->buffer = ImportPixmap(inst, xpix, fmt, geomReply->width, geomReply->height);
    if (ppix->buffer == NULL)
    {
        goto done;
    }

    buffers[1] = (EGLAttrib) ppix->buffer;
    esurf = inst->platform->priv->egl.PlatformCreateSurfaceNVX(inst->internal_display->edpy, config,
            buffers, attribs);
    if (esurf == EGL_NO_SURFACE)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to allocate EGLSurface");
        goto done;
    }

done:
    if (esurf == EGL_NO_SURFACE)
    {
        eplX11DestroyPixmap(surf);
    }
    free(geomReply);
    free(error);
    return esurf;
}
