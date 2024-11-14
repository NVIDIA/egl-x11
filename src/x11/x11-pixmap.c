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
#include <string.h>
#include <errno.h>
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
    uint32_t width;
    uint32_t height;
    EGLPlatformColorBufferNVX buffer;
    EGLPlatformColorBufferNVX blit_target;
    int prime_dmabuf;
    xcb_pixmap_t prime_pixmap;
} X11Pixmap;

static EGLBoolean CheckDirectSupported(X11DisplayInstance *inst, const X11DriverFormat *fmt,
        const xcb_dri3_buffers_from_pixmap_reply_t *reply)
{
    int i;

    if (xcb_dri3_buffers_from_pixmap_buffers_length(reply) != 1)
    {
        return EGL_FALSE;
    }

    for (i=0; i<fmt->num_modifiers; i++)
    {
        if (fmt->modifiers[i] == reply->modifier)
        {
            return EGL_TRUE;
        }
    }

    return EGL_FALSE;
}

static EGLPlatformColorBufferNVX AllocInternalBuffer(X11DisplayInstance *inst,
        const X11DriverFormat *fmt, uint32_t width, uint32_t height)
{
    EGLPlatformColorBufferNVX buffer = NULL;
    struct gbm_bo *gbo = NULL;
    int fd = -1;

    gbo = gbm_bo_create_with_modifiers2(inst->gbmdev,
            width, height, fmt->fourcc, fmt->modifiers, fmt->num_modifiers, 0);
    if (gbo == NULL)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to allocate internal buffer for PRIME pixmap");
        goto done;
    }

    fd = gbm_bo_get_fd(gbo);
    if (fd < 0)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to get internal dma-buf for PRIME pixmap");
        goto done;
    }

    buffer = inst->platform->priv->egl.PlatformImportColorBufferNVX(inst->internal_display->edpy,
            fd, width, height, gbm_bo_get_format(gbo),
            gbm_bo_get_stride(gbo),
            gbm_bo_get_offset(gbo, 0),
            gbm_bo_get_modifier(gbo));
    if (buffer == NULL)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to import internal dma-buf for PRIME pixmap");
        goto done;
    }

done:
    if (fd >= 0)
    {
        close(fd);
    }
    if (gbo != NULL)
    {
        gbm_bo_destroy(gbo);
    }
    return buffer;
}

static EGLBoolean AllocLinearPixmap(X11DisplayInstance *inst, EplSurface *surf,
        const X11DriverFormat *fmt, uint32_t width, uint32_t height)
{
    X11Pixmap *ppix = (X11Pixmap *) surf->priv;
    int stride = 0;
    int offset = 0;
    int fd = -1;
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error = NULL;
    EGLBoolean success = EGL_FALSE;

    assert(ppix->prime_dmabuf < 0);
    assert(ppix->blit_target == NULL);

    ppix->blit_target = inst->platform->priv->egl.PlatformAllocColorBufferNVX(inst->internal_display->edpy,
                width, height, fmt->fourcc, DRM_FORMAT_MOD_LINEAR, EGL_TRUE);
    if (ppix->blit_target == NULL)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to allocate internal buffer for linear PRIME pixmap");
        goto done;
    }

    // Export the image to a dma-buf.
    if (!inst->platform->priv->egl.PlatformExportColorBufferNVX(inst->internal_display->edpy, ppix->blit_target,
        &ppix->prime_dmabuf, NULL, NULL, NULL, &stride, &offset, NULL))
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to get internal dma-buf for linear PRIME pixmap");
        goto done;
    }
    if (ppix->prime_dmabuf < 0)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC,
                "Possible driver error: Failed to get internal dma-buf for linear PRIME pixmap");
        goto done;
    }

    // XCB will close the file descriptor after sending the request, so we have
    // to duplicate it.
    fd = dup(ppix->prime_dmabuf);
    if (fd < 0)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to dup dmabuf: %s\n", strerror(errno));
        goto done;
    }

    ppix->prime_pixmap = xcb_generate_id(inst->conn);
    cookie = xcb_dri3_pixmap_from_buffers_checked(inst->conn,
            ppix->prime_pixmap, inst->xscreen->root, 1, width, height, stride, offset,
            0, 0, 0, 0, 0, 0, eplFormatInfoDepth(fmt->fmt), fmt->fmt->bpp,
            DRM_FORMAT_MOD_LINEAR, &fd);

    error = xcb_request_check(inst->conn, cookie);
    if (error != NULL)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "DRI3PixmapFromBuffers request failed with error %d\n",
                (int) error->error_code);
        ppix->prime_pixmap = 0;
        goto done;
    }

    success = EGL_TRUE;

done:
    free(error);

    return success;
}

/**
 * Fetches the dma-buf for a Pixmap from the server and creates an
 * EGLExtColorBuffer in the driver for it.
 */
static EGLBoolean ImportPixmap(X11DisplayInstance *inst, EplSurface *surf,
        xcb_pixmap_t xpix, const EplFormatInfo *fmt, uint32_t width, uint32_t height)
{
    X11Pixmap *ppix = (X11Pixmap *) surf->priv;
    const X11DriverFormat *driverFmt = eplX11FindDriverFormat(inst, fmt->fourcc);
    xcb_generic_error_t *error = NULL;
    xcb_dri3_buffers_from_pixmap_cookie_t cookie;
    xcb_dri3_buffers_from_pixmap_reply_t *reply = NULL;
    int depth = fmt->colors[0] + fmt->colors[1] + fmt->colors[2] + fmt->colors[3];
    int32_t *fds = NULL;
    EGLBoolean prime = EGL_FALSE;
    EGLBoolean success = EGL_FALSE;

    if (driverFmt == NULL)
    {
        // This should never happen: If the format isn't supported, then we
        // should have caught that when we looked up the EGLConfig.
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Internal error: Unsupported format 0x%08x\n", fmt->fourcc);
        goto done;
    }

    cookie = xcb_dri3_buffers_from_pixmap(inst->conn, xpix);
    reply = xcb_dri3_buffers_from_pixmap_reply(inst->conn, cookie, &error);
    if (reply == NULL)
    {
        free(error);
        goto done;
    }
    fds = xcb_dri3_buffers_from_pixmap_buffers(reply);

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

    if (inst->force_prime || !CheckDirectSupported(inst, driverFmt, reply))
    {
        prime = EGL_TRUE;
    }

    if (!prime)
    {
        ppix->buffer = inst->platform->priv->egl.PlatformImportColorBufferNVX(inst->internal_display->edpy,
                fds[0], width, height, fmt->fourcc,
                xcb_dri3_buffers_from_pixmap_strides(reply)[0],
                xcb_dri3_buffers_from_pixmap_offsets(reply)[0],
                reply->modifier);
        if (ppix->buffer == NULL)
        {
            eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to import dma-buf for pixmap");
            goto done;
        }
        ppix->prime_dmabuf = fds[0];
    }
    else
    {
        ppix->buffer = AllocInternalBuffer(inst, driverFmt, width, height);
        if (ppix->buffer == NULL)
        {
            goto done;
        }

        if (reply->modifier == DRM_FORMAT_MOD_LINEAR &&
                xcb_dri3_buffers_from_pixmap_buffers_length(reply) == 1)
        {
            // We need to use PRIME, but the server is using a linear buffer, so we
            // can blit to it directly.

            ppix->blit_target = inst->platform->priv->egl.PlatformImportColorBufferNVX(inst->internal_display->edpy,
                    fds[0], width, height, fmt->fourcc,
                    xcb_dri3_buffers_from_pixmap_strides(reply)[0],
                    xcb_dri3_buffers_from_pixmap_offsets(reply)[0],
                    reply->modifier);
            if (ppix->blit_target == NULL)
            {
                eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to import dma-buf for pixmap");
                goto done;
            }
            ppix->prime_dmabuf = fds[0];
        }
        else
        {
            // Otherwise, create a linear intermediate pixmap. We'll blit to that
            // pixmap, and then send a CopyArea request to blit to the caller's
            // pixmap.

            if (!AllocLinearPixmap(inst, surf, driverFmt, width, height))
            {
                goto done;
            }
        }
    }

    success = EGL_TRUE;

done:
    if (reply != NULL)
    {
        int i;
        for (i=0; i<xcb_dri3_buffers_from_pixmap_buffers_length(reply); i++)
        {
            if (fds[i] != ppix->prime_dmabuf)
            {
                close(fds[i]);
            }
        }
        free(reply);
    }
    return success;
}

static void PixmapDamageCallback(void *param, int syncfd, unsigned int flags)
{
    EplSurface *surf = param;
    X11Pixmap *ppix = (X11Pixmap *) surf->priv;

    /*
     * Note that we should be fine even if another thread is trying to call
     * eglDestroyPixmap here.
     *
     * The driver's eglDestroyPixmap gets called before anything else in the
     * X11Pixmap struct gets cleaned up. The driver will ensure that any
     * callbacks have finished and no new callbacks can start when
     * eglDestroyPixmap returns.
     */

    if (syncfd >= 0)
    {
        /*
         * We don't have any form of explicit sync that we can use for pixmap
         * rendering, because we're not using PresentPixmap.
         *
         * If the server (and the kernel) both support it, then try to use
         * implicit sync. Otherwise, do a CPU wait so that we can at least get
         * a consistent functional result in all cases.
         */
        if (ppix->prime_dmabuf < 0 || !eplX11ImportDmaBufSyncFile(ppix->inst, ppix->prime_dmabuf, syncfd))
        {
            eplX11WaitForFD(syncfd);
        }
    }

    if (ppix->prime_pixmap != 0)
    {
        // TODO: Should we hold on to the GC that we create here? We should be
        // able to reuse it for any drawable that has the same depth.

        xcb_create_gc_value_list_t gcvalues;
        xcb_gcontext_t gc = xcb_generate_id(ppix->inst->conn);
        xcb_create_gc_aux(ppix->inst->conn, gc, ppix->xpix, 0, &gcvalues);

        xcb_copy_area(ppix->inst->conn, ppix->prime_pixmap, ppix->xpix, gc,
                0, 0, 0, 0, ppix->width, ppix->height);
        xcb_free_gc(ppix->inst->conn, gc);
    }
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
            if (ppix->buffer != NULL)
            {
                ppix->inst->platform->priv->egl.PlatformFreeColorBufferNVX(ppix->inst->internal_display->edpy, ppix->buffer);
            }
            if (ppix->blit_target != NULL)
            {
                ppix->inst->platform->priv->egl.PlatformFreeColorBufferNVX(ppix->inst->internal_display->edpy, ppix->blit_target);
            }
            if (ppix->prime_pixmap != 0 && ppix->inst->conn != NULL)
            {
                xcb_free_pixmap(ppix->inst->conn, ppix->prime_pixmap);
            }
            eplX11DisplayInstanceUnref(ppix->inst);
        }
        if (ppix->prime_dmabuf >= 0)
        {
            close(ppix->prime_dmabuf);
        }
        free(ppix);
    }
}

EGLSurface eplX11CreatePixmapSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform)
{
    X11DisplayInstance *inst = pdpy->priv->inst;
    xcb_pixmap_t xpix = eplX11GetNativeXID(pdpy, native_surface, create_platform);
    X11Pixmap *ppix = NULL;
    const EplConfig *configInfo;
    const EplFormatInfo *fmt;
    xcb_get_geometry_cookie_t geomCookie;
    xcb_get_geometry_reply_t *geomReply = NULL;
    xcb_generic_error_t *error = NULL;
    EGLSurface esurf = EGL_NO_SURFACE;
    EGLAttrib buffers[] =
    {
        GL_BACK, 0,
        EGL_PLATFORM_SURFACE_BLIT_TARGET_NVX, 0,
        EGL_PLATFORM_SURFACE_DAMAGE_CALLBACK_NVX, (EGLAttrib) PixmapDamageCallback,
        EGL_PLATFORM_SURFACE_DAMAGE_CALLBACK_PARAM_NVX, (EGLAttrib) surf,
        EGL_NONE
    };
    EGLAttrib *internalAttribs = NULL;

    if (xpix == 0)
    {
        eplSetError(plat, EGL_BAD_NATIVE_PIXMAP, "Invalid native pixmap %p\n", native_surface);
        return EGL_NO_SURFACE;
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

    internalAttribs = eplX11GetInternalSurfaceAttribs(plat, pdpy, internalAttribs);
    if (internalAttribs == NULL)
    {
        goto done;
    }

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
    ppix->width = geomReply->width;
    ppix->height = geomReply->height;
    ppix->prime_dmabuf = -1;

    if (!ImportPixmap(inst, surf, xpix, fmt, geomReply->width, geomReply->height))
    {
        goto done;
    }

    buffers[1] = (EGLAttrib) ppix->buffer;
    if (ppix->blit_target != NULL)
    {
        buffers[3] = (EGLAttrib) ppix->blit_target;
    }
    else
    {
        // If we're not using PRIME, then we don't need the damage callback.
        buffers[2] = EGL_NONE;
    }
    esurf = inst->platform->priv->egl.PlatformCreateSurfaceNVX(inst->internal_display->edpy, config,
            buffers, internalAttribs);
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
    free(internalAttribs);
    return esurf;
}
