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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

#include <GL/gl.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/xproto.h>
#include <xcb/present.h>

#include <xf86drm.h>

#include "x11-platform.h"
#include "x11-timeline.h"
#include "glvnd_list.h"
#include "dma-buf.h"

/**
 * This is a special flag that XWayland sets in a PresentConfigureNotify event
 * when the window gets destroyed. Note that it's not actually specified in the
 * Present spec yet.
 */
#define PRESENT_WINDOW_DESTROYED_FLAG (1 << 0)

/**
 * The maximum number of color buffers to allocate for a window.
 */
static const int MAX_COLOR_BUFFERS = 4;

/**
 * The maximum number of linear buffers for PRIME presentation.
 */
static const int MAX_PRIME_BUFFERS = 2;

/**
 * The maximum number of outstanding PresentPixmap requests that we can have
 * before we wait for one to complete in eglSwapBuffers.
 */
static const uint32_t MAX_PENDING_FRAMES = 1;

/**
 * How long to wait for a buffer release before we stop to check for window
 * events.
 */
static const int RELEASE_WAIT_TIMEOUT = 100;

/**
 * An enum to keep track of whether it's safe to reuse a buffer.
 */
typedef enum
{
    /**
     * The buffer is idle, so we can use it immediately.
     */
    BUFFER_STATUS_IDLE,

    /**
     * The buffer is in use in the server, and we have not yet received a
     * PresentIdleNotify event for it.
     */
    BUFFER_STATUS_IN_USE,

    /**
     * We've received a PresentIdleNotify event for this buffer, but we haven't
     * waited for it to actually be free yet.
     */
    BUFFER_STATUS_IDLE_NOTIFIED,
} X11BufferStatus;

/**
 * Data for each color buffer that we allocate for a window.
 *
 * This contains the EGLExtColorBuffer handle for the buffer in the driver, and
 * the XID for the corresponding Pixmap in the server.
 */
typedef struct
{
    /**
     * The GBM buffer object for this color buffer.
     */
    struct gbm_bo *gbo;

    /**
     * The handle for the color buffer in the driver.
     */
    EGLPlatformColorBufferNVX buffer;

    /**
     * Whether this buffer is still in use by the server.
     */
    X11BufferStatus status;

    /**
     * The XID for the pixmap that uses this buffer.
     */
    xcb_pixmap_t xpix;

    /**
     * The serial number of the last PresentPixmap request that used this
     * buffer.
     */
    uint32_t last_present_serial;

    /**
     * A file descriptor for the dma-buf.
     *
     * Note that currently, this is only used for PRIME buffers.
     */
    int fd;

    /**
     * A timeline sync object.
     *
     * It's possible that different buffers could go through a different
     * presentation path in the server, which could in turn cause them to be
     * released in a different order than they were presented.
     *
     * To cope with that, we give each buffer its own timeline for acquire and
     * release points.
     */
    X11Timeline timeline;

    struct glvnd_list entry;
} X11ColorBuffer;

/**
 * Data that we need to keep track of for an X window.
 */
typedef struct
{
    /// A pointer back to the owning display.
    X11DisplayInstance *inst;

    xcb_window_t xwin;

    /**
     * A mutex to protect the other fields in this struct.
     *
     * We have to be careful with this to avoid a deadlock. The driver calls
     * the window update callback while holding its window-system lock, and the
     * update callback has to take the window mutex to safely modify the other
     * fields.
     *
     * That means that we can't call into the driver while holding this mutex,
     * if there's any chance that another thread could be in the middle of the
     * update callback. If we did, then that driver call could try to take the
     * window-system lock, which would then deadlock.
     *
     * To deal with that, we rely on the fact that the driver will only call
     * the update callback for a surface if that surface is current (or about
     * to be current in an eglMakeCurrent call). That means that we can safely
     * call into the driver from eglSwapBuffers while holding this mutex,
     * because eglSwapBuffers is also only valid if an EGLSurface is current.
     */
    pthread_mutex_t mutex;

    /**
     * The capabilities of the window, as reported by PresentQueryCapabilities.
     */
    uint32_t present_capabilities;

    /**
     * If true, then we use explicit sync for this surface.
     */
    EGLBoolean use_explicit_sync;

    /**
     * The current size of the window.
     */
    EGLint width;
    EGLint height;

    /**
     * The format modifiers that we're using for this window.
     */
    uint64_t modifier;

    /**
     * True if GPU offloading is in use.
     *
     * In the normal (non-PRIME) case, we render directly to the color buffer
     * that's shared with the server.
     *
     * In the PRIME case, we render to a private back buffer, and then in
     * eglSwapBuffers, we copy the back buffer to a shared intermediate buffer,
     * which is then presented.
     */
    EGLBoolean prime;

    /**
     * The pending width and height is set in response to a window resize.
     *
     * We set this when we receive a PresentConfigureNotify event.
     *
     * If the pending size is different than the current size, then that means
     * we need to reallocate the shared color buffers for this window.
     */
    EGLint pending_width;
    EGLint pending_height;

    /**
     * True if we should check for new format modifiers for the color buffers.
     */
    EGLBoolean needs_modifier_check;

    /**
     * If this is non-zero, then ignore the update callback.
     *
     * This is used in eglSwapBuffers and during teardown.
     */
    unsigned int skip_update_callback;

    /**
     * The color buffers that we've allocated for this window.
     *
     * This is a list of X11ColorBuffer structs.
     */
    struct glvnd_list color_buffers;

    /**
     * A list of pitch linear buffers used for PRIME.
     */
    struct glvnd_list prime_buffers;

    /**
     * A pointer to the current front buffer.
     */
    X11ColorBuffer *current_front;

    /**
     * A pointer to the current back buffer.
     */
    X11ColorBuffer *current_back;

    /**
     * A pointer to the current shared buffer for PRIME.
     */
    X11ColorBuffer *current_prime;

    /**
     * The current swap interval, as set by eglSwapInterval.
     */
    EGLint swap_interval;

    /**
     * The color format that we're using for this window.
     */
    const X11DriverFormat *format;

    uint32_t present_event_id;
    uint32_t present_event_stamp;
    xcb_special_event_t *present_event;

    /**
     * The serial number of the last PresentPixmap request that we sent.
     */
    uint32_t last_present_serial;

    /**
     * The serial number of the last PresentCompleteNotify event that we
     * recieved.
     */
    uint32_t last_complete_serial;

    /**
     * The MSC value from the last PresentCompleteNotify event that we received.
     */
    uint64_t last_complete_msc;

    /**
     * Set to true if the native window was destroyed.
     *
     * This requires a fairly recent version of the X server.
     */
    EGLBoolean native_destroyed;
} X11Window;

static void FreeColorBuffer(X11DisplayInstance *inst, X11ColorBuffer *buffer)
{
    if (buffer != NULL)
    {
        if (buffer->gbo != NULL)
        {
            gbm_bo_destroy(buffer->gbo);
        }
        if (buffer->buffer != NULL)
        {
            inst->platform->priv->egl.PlatformFreeColorBufferNVX(
                    inst->internal_display->edpy, buffer->buffer);
        }
        if (buffer->xpix != 0)
        {
            if (inst->conn != NULL)
            {
                // TODO: Is it safe to call into xcb if this happens during teardown?
                xcb_free_pixmap(inst->conn, buffer->xpix);
            }
        }
        eplX11TimelineDestroy(inst, &buffer->timeline);
        if (buffer->fd >= 0)
        {
            close(buffer->fd);
        }
        free(buffer);
    }
}

/**
 * Allocates a color buffer in the driver. This does *not* create a shared
 * pixmap from the buffer.
 */
static X11ColorBuffer *AllocOneColorBuffer(X11DisplayInstance *inst,
        const EplFormatInfo *fmt, uint32_t width, uint32_t height,
        const uint64_t *modifiers, int num_modifiers,
        EGLBoolean scanout)
{
    int fd = -1;
    uint32_t flags = 0;
    X11ColorBuffer *buffer = NULL;

    assert(num_modifiers > 0);

    if (scanout)
    {
        flags |= GBM_BO_USE_SCANOUT;
    }

    buffer = calloc(1, sizeof(X11ColorBuffer));
    if (buffer == NULL)
    {
        return NULL;
    }

    glvnd_list_init(&buffer->entry);
    buffer->fd = -1;

    buffer->gbo = gbm_bo_create_with_modifiers2(inst->gbmdev,
            width, height, fmt->fourcc, modifiers, num_modifiers, flags);

    if (buffer->gbo == NULL)
    {
        goto done;
    }

    fd = gbm_bo_get_fd(buffer->gbo);
    if (fd < 0)
    {
        goto done;
    }

    buffer->buffer = inst->platform->priv->egl.PlatformImportColorBufferNVX(inst->internal_display->edpy,
            fd, width, height, gbm_bo_get_format(buffer->gbo),
            gbm_bo_get_stride(buffer->gbo),
            gbm_bo_get_offset(buffer->gbo, 0),
            gbm_bo_get_modifier(buffer->gbo));
    if (buffer->buffer == NULL)
    {
        goto done;
    }

done:
    if (fd >= 0)
    {
        close(fd);
    }
    if (buffer->buffer == NULL)
    {
        FreeColorBuffer(inst, buffer);
        return NULL;
    }

    return buffer;
}

/**
 * Allocates a linear sysmem buffer to use for PRIME.
 */
static X11ColorBuffer *AllocatePrimeBuffer(X11DisplayInstance *inst,
        uint32_t fourcc, uint32_t width, uint32_t height)
{
    X11ColorBuffer *buffer;
    struct gbm_import_fd_modifier_data gimport;
    int stride = 0;
    int offset = 0;
    EGLBoolean success = EGL_FALSE;

    buffer = calloc(1, sizeof(X11ColorBuffer));
    if (buffer == NULL)
    {
        return NULL;
    }

    glvnd_list_init(&buffer->entry);
    buffer->fd = -1;

    buffer->buffer = inst->platform->priv->egl.PlatformAllocColorBufferNVX(inst->internal_display->edpy,
                width, height, fourcc, DRM_FORMAT_MOD_LINEAR, EGL_TRUE);
    if (buffer->buffer == NULL)
    {
        goto done;
    }

    // Export the image to a dma-buf.
    if (!inst->platform->priv->egl.PlatformExportColorBufferNVX(inst->internal_display->edpy, buffer->buffer,
        &buffer->fd, NULL, NULL, NULL, &stride, &offset, NULL))
    {
        goto done;
    }

    // Import the dma-buf to a gbm_bo, so that we can use it in
    // CreateSharedPixmap.
    gimport.width = width;
    gimport.height = height;
    gimport.format = fourcc;
    gimport.num_fds = 1;
    gimport.fds[0] = buffer->fd;
    gimport.strides[0] = stride;
    gimport.offsets[0] = offset;
    gimport.modifier = DRM_FORMAT_MOD_LINEAR;

    buffer->gbo = gbm_bo_import(inst->gbmdev, GBM_BO_IMPORT_FD_MODIFIER, &gimport, 0);
    if (buffer->gbo == NULL)
    {
        goto done;
    }

    success = EGL_TRUE;

done:
    if (!success)
    {
        FreeColorBuffer(inst, buffer);
        return NULL;
    }

    return buffer;
}

static void FreeWindowBuffers(EplSurface *surf)
{
    X11Window *pwin = (X11Window *) surf->priv;

    while (!glvnd_list_is_empty(&pwin->color_buffers))
    {
        X11ColorBuffer *buffer = glvnd_list_first_entry(&pwin->color_buffers, X11ColorBuffer, entry);
        glvnd_list_del(&buffer->entry);
        FreeColorBuffer(pwin->inst, buffer);
    }
    while (!glvnd_list_is_empty(&pwin->prime_buffers))
    {
        X11ColorBuffer *buffer = glvnd_list_first_entry(&pwin->prime_buffers, X11ColorBuffer, entry);
        glvnd_list_del(&buffer->entry);
        FreeColorBuffer(pwin->inst, buffer);
    }
    pwin->current_front = NULL;
    pwin->current_back = NULL;
    pwin->current_prime = NULL;
}

static EGLBoolean AllocWindowBuffers(EplSurface *surf,
        const uint64_t *modifiers, int num_modifiers, EGLBoolean prime)
{
    X11Window *pwin = (X11Window *) surf->priv;
    uint64_t modifier;
    X11ColorBuffer *front = NULL;
    X11ColorBuffer *back = NULL;
    X11ColorBuffer *shared = NULL;
    EGLPlatformColorBufferNVX sharedBuf = NULL;
    EGLBoolean success = EGL_TRUE;

    front = AllocOneColorBuffer(pwin->inst, pwin->format->fmt, pwin->pending_width, pwin->pending_height,
            modifiers, num_modifiers, !prime);
    if (front == NULL)
    {
        goto done;
    }

    // We let the driver pick the modifier when we allocate the front buffer,
    // and then we'll just re-use that same modifier for everything after that.
    modifier = gbm_bo_get_modifier(front->gbo);

    back = AllocOneColorBuffer(pwin->inst, pwin->format->fmt, pwin->pending_width, pwin->pending_height,
            &modifier, 1, !prime);
    if (back == NULL)
    {
        goto done;
    }

    if (prime)
    {
        /*
         * For PRIME, we need to allocate one linear buffer so that we can
         * attach it as the blit target.
         */
        shared = AllocatePrimeBuffer(pwin->inst, pwin->format->fmt->fourcc, pwin->pending_width, pwin->pending_height);
        if (shared == NULL)
        {
            goto done;
        }
        sharedBuf = shared->buffer;
    }

    if (surf->internal_surface != EGL_NO_SURFACE)
    {
        EGLAttrib buffers[] =
        {
            GL_FRONT, (EGLAttrib) front->buffer,
            GL_BACK,  (EGLAttrib) back->buffer,
            EGL_PLATFORM_SURFACE_BLIT_TARGET_NVX, (EGLAttrib) sharedBuf,
            EGL_NONE
        };
        if (!pwin->inst->platform->priv->egl.PlatformSetColorBuffersNVX(pwin->inst->internal_display->edpy,
                surf->internal_surface, buffers))
        {
            goto done;
        }
    }

    FreeWindowBuffers(surf);

    glvnd_list_add(&front->entry, &pwin->color_buffers);
    glvnd_list_add(&back->entry, &pwin->color_buffers);
    if (shared != NULL)
    {
        glvnd_list_append(&shared->entry, &pwin->prime_buffers);
    }
    pwin->current_front = front;
    pwin->current_back = back;
    pwin->current_prime = shared;
    pwin->width = pwin->pending_width;
    pwin->height = pwin->pending_height;
    pwin->modifier = modifier;
    pwin->prime = prime;
    success = EGL_TRUE;

done:
    if (!success)
    {
        FreeColorBuffer(pwin->inst, front);
        FreeColorBuffer(pwin->inst, back);
        FreeColorBuffer(pwin->inst, shared);
    }

    return success;
}

/**
 * Finds the intersection between two sets of modifiers.
 *
 * This function will change \p mods to be the intersection between
 * \p client_mods and \p server_mods.
 *
 * If the intersection is empty, then \p mods will be unchanged.
 *
 * \param[out] mods Returns the intersection of modifiers. This may be the
 *      same pointer as \p client_mods to update the list in place.
 * \param client_mods The list of supported modifiers in the client.
 * \param num_client_mods The number of elements in \p client_mods.
 * \param server_mods The list of supported modifiers in the server.
 * \param num_server_mods The number of elements in \p server_mods.
 *
 * \return The number of elements in the intersection.
 */
static int GetModifierIntersection(uint64_t *mods,
        const uint64_t *client_mods, int num_client_mods,
        const uint64_t *server_mods, int num_server_mods)
{
    int count = 0;
    int i, j;

    for (i=0; i<num_client_mods; i++)
    {
        EGLBoolean found = EGL_FALSE;
        for (j=0; j<num_server_mods; j++)
        {
            if (client_mods[i] == server_mods[j])
            {
                found = EGL_TRUE;
                break;
            }
        }

        if (found)
        {
            mods[count++] = client_mods[i];
        }
    }

    return count;
}

/**
 * Finds the set of modifiers that we can use for the color buffers.
 */
static EGLBoolean FindSupportedModifiers(X11DisplayInstance *inst,
        const X11DriverFormat *format, xcb_window_t xwin,
        uint64_t **ret_modifiers, int *ret_num_modifiers,
        EGLBoolean *ret_prime)
{
    X11DriverFormat *driverFmt;
    xcb_dri3_get_supported_modifiers_cookie_t cookie;
    xcb_dri3_get_supported_modifiers_reply_t *reply = NULL;
    xcb_generic_error_t *error = NULL;
    uint64_t *mods = NULL;
    int numMods = 0;
    EGLBoolean prime = EGL_FALSE;

    driverFmt = eplX11FindDriverFormat(inst, format->fourcc);
    if (driverFmt == NULL)
    {
        // We should have already checked this when we set up the EGLConfig
        // list.
        assert(!"Can't happen -- driver doesn't support format.");
        return EGL_FALSE;
    }

    mods = malloc(driverFmt->num_modifiers * sizeof(uint64_t));
    if (mods == NULL)
    {
        return EGL_FALSE;
    }

    if (!inst->force_prime)
    {
        cookie = xcb_dri3_get_supported_modifiers(inst->conn, xwin,
                eplFormatInfoDepth(format->fmt), format->fmt->bpp);

        reply = xcb_dri3_get_supported_modifiers_reply(inst->conn, cookie, &error);
        if (reply == NULL)
        {
            free(error);
            free(mods);
            return EGL_FALSE;
        }

        if (xcb_dri3_get_supported_modifiers_window_modifiers_length(reply) > 0)
        {
            numMods = GetModifierIntersection(mods,
                    driverFmt->modifiers, driverFmt->num_modifiers,
                    xcb_dri3_get_supported_modifiers_window_modifiers(reply),
                    xcb_dri3_get_supported_modifiers_window_modifiers_length(reply));
        }

        if (numMods == 0)
        {
            /*
             * If the window list is not empty, then we assume that any
             * modifier not in that list will require a blit, so we might as
             * well use the PRIME path and do that blit in the client.
             *
             * If the window list is empty, then that means the server doesn't
             * have a separate per-window modifier list, so look for something
             * in the screen list instead.
             *
             * Likewise, if we can't support PRIME in the client, then try to
             * find something that the server supports, even if that means
             * letting the server do a blit.
             */
            if (xcb_dri3_get_supported_modifiers_window_modifiers_length(reply) == 0
                        || !inst->supports_prime)
            {
                numMods = GetModifierIntersection(mods,
                        driverFmt->modifiers, driverFmt->num_modifiers,
                        xcb_dri3_get_supported_modifiers_screen_modifiers(reply),
                        xcb_dri3_get_supported_modifiers_screen_modifiers_length(reply));
            }
        }

        free(reply);
    }

    if (numMods > 0)
    {
        prime = EGL_FALSE;
    }
    else if (inst->supports_prime)
    {
        /*
         * If we didn't find a usable modifier, then we have to use the PRIME
         * presentation path.
         *
         * In this case, we don't care what the server supports, because the
         * color buffers will only be used by the client, and the shared pixmap
         * will use DRM_FORMAT_MOD_LINEAR.
         */
        prime = EGL_TRUE;
        memcpy(mods, driverFmt->modifiers, driverFmt->num_modifiers * sizeof(uint64_t));
        numMods = driverFmt->num_modifiers;
    }
    else
    {
        // We couldn't find any supported modifiers.
        free(mods);
        return EGL_FALSE;
    }

    *ret_modifiers = mods;
    *ret_num_modifiers = numMods;
    *ret_prime = prime;
    return EGL_TRUE;
}

void eplX11FreeWindow(EplSurface *surf)
{
    X11Window *pwin = (X11Window *) surf->priv;

    FreeWindowBuffers(surf);

    if (pwin->inst->conn != NULL && pwin->present_event != NULL)
    {
        // Unregister for events. It's possible that the window has already
        // been destroyed since the last time we checked for events, so
        // ignore any errors.
        if (!pwin->native_destroyed)
        {
            xcb_void_cookie_t cookie = xcb_present_select_input_checked(pwin->inst->conn,
                    pwin->present_event_id, pwin->xwin, 0);
            xcb_discard_reply(pwin->inst->conn, cookie.sequence);
        }
        xcb_unregister_for_special_event(pwin->inst->conn, pwin->present_event);
    }

    surf->priv = NULL;
    pthread_mutex_destroy(&pwin->mutex);
    eplX11DisplayInstanceUnref(pwin->inst);
    free(pwin);
}

static void HandlePresentEvent(EplSurface *surf, xcb_generic_event_t *xcbevt)
{
    X11Window *pwin = (X11Window *) surf->priv;
    xcb_present_generic_event_t *ge = (xcb_present_generic_event_t *) xcbevt;

    if (ge->evtype == XCB_PRESENT_CONFIGURE_NOTIFY)
    {
        xcb_present_configure_notify_event_t *evt = (xcb_present_configure_notify_event_t *) xcbevt;
        pwin->pending_width = evt->width;
        pwin->pending_height = evt->height;

        if (evt->pixmap_flags & PRESENT_WINDOW_DESTROYED_FLAG)
        {
            pwin->native_destroyed = EGL_TRUE;
        }
    }
    else if (ge->evtype == XCB_PRESENT_IDLE_NOTIFY)
    {
        // With explicit sync, we don't care about PresentIdleNotify events.
        if (!pwin->use_explicit_sync)
        {
            X11ColorBuffer *buffer;
            xcb_present_idle_notify_event_t *evt = (xcb_present_idle_notify_event_t *) xcbevt;
            struct glvnd_list *buffers = pwin->prime ? &pwin->prime_buffers : &pwin->color_buffers;

            glvnd_list_for_each_entry(buffer, buffers, entry)
            {
                if (buffer->xpix == evt->pixmap && buffer->last_present_serial == evt->serial)
                {
                    assert(buffer->status == BUFFER_STATUS_IN_USE);
                    if (buffer->status == BUFFER_STATUS_IN_USE)
                    {
                        buffer->status = BUFFER_STATUS_IDLE_NOTIFIED;
                    }
                    buffer->last_present_serial = 0;

                    /*
                     * Move the buffer to the end of the list. If we don't have any
                     * server -> client synchronization, then this ensures that
                     * we'll reuse the oldest buffers first, so we'll have the best
                     * chance that the buffer really is idle.
                     */
                    glvnd_list_del(&buffer->entry);
                    glvnd_list_append(&buffer->entry, buffers);

                    break;
                }
            }
        }
    }
    else if (ge->evtype == XCB_PRESENT_COMPLETE_NOTIFY)
    {
        xcb_present_complete_notify_event_t *evt = (xcb_present_complete_notify_event_t *) xcbevt;
        uint32_t age = pwin->last_present_serial - evt->serial;
        uint32_t pending = pwin->last_present_serial - pwin->last_complete_serial;
        if (age < pending)
        {
            pwin->last_complete_serial = evt->serial;
            pwin->last_complete_msc = evt->msc;
        }

        if (!pwin->inst->force_prime && evt->mode == XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY)
        {
            /*
             * If the server tells us that this is a suboptimal format, then we
             * should check for supported format modifiers during the next
             * swap.
             */
            pwin->needs_modifier_check = EGL_TRUE;
        }
    }
    else
    {
        // We shouldn't get here.
        assert(!"Invalid present event");
    }
}

/**
 * Checks if we need to reallocate the buffers for a window, and if so,
 * reallocates them.
 *
 * \param surf The surface to check.
 * \param allow_modifier_change If true, then allow reallocating the buffers to
 *      deal with a format modifier change.
 * \param[out] was_resized Optionally returns EGL_TRUE if the window has new
 *      buffers.
 * \return EGL_TRUE on success, or EGL_FALSE if there was an error.
 */
static EGLBoolean CheckReallocWindow(EplSurface *surf,
        EGLBoolean allow_modifier_change, EGLBoolean *was_resized)
{
    X11Window *pwin = (X11Window *) surf->priv;
    EGLBoolean need_realloc = EGL_FALSE;

    if (was_resized)
    {
        *was_resized = EGL_FALSE;
    }

    if (surf->deleted || pwin->native_destroyed)
    {
        return EGL_TRUE;
    }

    if (pwin->pending_width != pwin->width
            || pwin->pending_height != pwin->height)
    {
        need_realloc = EGL_TRUE;
    }

    if (need_realloc || (allow_modifier_change && pwin->needs_modifier_check))
    {
        uint64_t currentModifier = pwin->modifier;
        const uint64_t *mods = NULL;
        uint64_t *modsBuffer = NULL;
        int numMods = 0;
        EGLBoolean prime = EGL_FALSE;

        if (pwin->needs_modifier_check)
        {
            if (!FindSupportedModifiers(pwin->inst, pwin->format, pwin->xwin, &modsBuffer, &numMods, &prime))
            {
                return EGL_FALSE;
            }
            mods = modsBuffer;

            /*
             * Check if the current modifier is one of the supported ones.
             * If it is, then we don't need to reallocate just for the
             * modifier change.
             *
             * If we need to reallocate anyway because the window resized,
             * though, then pick new modifiers while we're at it.
             */
            if (!need_realloc && allow_modifier_change)
            {
                int i;
                need_realloc = EGL_TRUE;
                for (i=0; i<numMods; i++)
                {
                    if (pwin->modifier == mods[i])
                    {
                        need_realloc = EGL_FALSE;
                        break;
                    }
                }
            }
        }
        else
        {
            // We didn't get a notification from the server that the modifier
            // is suboptimal, so just keep the current modifier.
            mods = &currentModifier;
            prime = pwin->prime;
            numMods = 1;
        }

        if (need_realloc)
        {
            if (!AllocWindowBuffers(surf, mods, numMods, prime))
            {
                free(modsBuffer);
                return EGL_FALSE;
            }

            if (was_resized != NULL)
            {
                *was_resized = EGL_TRUE;
            }
            pwin->needs_modifier_check = EGL_FALSE;
        }
        else if (allow_modifier_change)
        {
            // If we checked for new modifiers, but we didn't need to
            // reallocate, then clear the needs_modifier_check flag so that we
            // don't end up checking again on the next frame.
            pwin->needs_modifier_check = EGL_FALSE;
        }

        free(modsBuffer);
    }

    return EGL_TRUE;
}

static void PollForWindowEvents(EplSurface *surf)
{
    X11Window *pwin = (X11Window *) surf->priv;

    while (!pwin->native_destroyed && !surf->deleted)
    {
        xcb_generic_event_t *xcbevt = xcb_poll_for_special_event(pwin->inst->conn, pwin->present_event);
        if (xcbevt == NULL)
        {
            break;
        }

        HandlePresentEvent(surf, xcbevt);
        free(xcbevt);
    }
}

static void WindowUpdateCallback(void *param)
{
    EplSurface *surf = param;
    X11Window *pwin = (X11Window *) surf->priv;

    /*
     * Here, we lock the window mutex, but *not* the display mutex.
     *
     * We can't lock the display mutex, because we could run into a deadlock:
     * - Thread A calls an EGL function in the platform library, which locks
     *   the display. The platform library calls into the driver, which tries to
     *   take the winsys lock.
     * - Thread B calls a GL function. The driver takes the winsys lock, then
     *   calls the window update callback. The window update callback tries to
     *   take the display lock.
     *
     * We can safely access anything in X11DisplayInstance, because that never
     * changes after it's initialized, and thus we don't need a mutex for it.
     */
    pthread_mutex_lock(&pwin->mutex);

    if (pwin->skip_update_callback != 0)
    {
        pthread_mutex_unlock(&pwin->mutex);
        return;
    }

    PollForWindowEvents(surf);
    CheckReallocWindow(surf, EGL_FALSE, NULL);

    pthread_mutex_unlock(&pwin->mutex);
}

/**
 * A common helper function to send a PresentPixmap or PresentPixmapSynced
 * request.
 *
 * If explicit sync is supported, then the pixmap's current timeline point must
 * already be set up to the correct acquire fence.
 */
static void SendPresentPixmap(EplSurface *surf, X11ColorBuffer *sharedPixmap, uint32_t options)
{
    X11Window *pwin = (X11Window *) surf->priv;
    uint32_t numPending = pwin->last_present_serial - pwin->last_complete_serial;
    uint32_t targetMSC = 0;
    uint64_t divisor = 1;

    if (pwin->swap_interval <= 0)
    {
        options |= XCB_PRESENT_OPTION_ASYNC;
    }

    if (options & XCB_PRESENT_OPTION_ASYNC)
    {
        // Make sure that the server actually supports the async flag. If it
        // doesn't, then just remove it.
        if (!(pwin->present_capabilities & XCB_PRESENT_CAPABILITY_ASYNC))
        {
            options &= ~XCB_PRESENT_OPTION_ASYNC;
        }
        targetMSC = 0;
    }
    else
    {
        /*
         * Note the the semantics of PresentPixmap doesn't quite match what
         * eglSwapBuffers is supposed to do.
         *
         * With a swap interval >= 1, each image must be displayed for at least
         * that many refresh cycles (or with a compositor, for that many
         * frames) before switching to the next image.
         *
         * But, PresentPixmap only accepts an absolute value for the MSC
         * target, not one that's relative to the previous present. So, to do
         * this correctly, we'd have to set the MSC target to the previous
         * frame's MSC value plus the swap interval.
         *
         * But, we don't want to stall and wait the last PresentPixmap to
         * complete, so we don't necessarily know what the last MSC value was.
         *
         * So instead, set the MSC target based on the most recent
         * PresentCompleteNotify event that we did get, and just increment it
         * based on the number of outstanding frames.
         *
         * That should give the correct result as long as you've got a
         * relatively steady framerate:
         * - If the framerate is at least as fast as the refresh rate, then
         *   we should have one PresentPixmap request complete per refresh
         *   cycle, and so adding the number of pending frames will be correct.
         * - If the framerate is slower than the refresh rate, then the
         *   previous Present will have completed by the time we send the next
         *   one, so we actually will have the most recent MSC value from the
         *   server.
         */

        targetMSC = pwin->last_complete_msc + ((numPending + 1) * pwin->swap_interval);
    }

    pwin->last_present_serial++;

    if (pwin->use_explicit_sync)
    {
        pwin->inst->platform->priv->xcb.present_pixmap_synced(pwin->inst->conn, pwin->xwin,
                sharedPixmap->xpix, pwin->last_present_serial,
                0, 0, 0, 0, 0,
                sharedPixmap->timeline.xid, sharedPixmap->timeline.xid,
                sharedPixmap->timeline.point, sharedPixmap->timeline.point + 1,
                options, targetMSC, divisor, 0,
                0, NULL);

        sharedPixmap->timeline.point++;
    }
    else
    {
        xcb_present_pixmap(pwin->inst->conn,
                pwin->xwin,
                sharedPixmap->xpix,
                pwin->last_present_serial,
                0, 0, // No update regions
                0, 0, // No offset -- update the whole window
                0, 0, 0, // No CRTC or fences
                options, targetMSC, divisor, 0, 0, NULL);
    }

    xcb_flush(pwin->inst->conn);
    sharedPixmap->status = BUFFER_STATUS_IN_USE;
    sharedPixmap->last_present_serial = pwin->last_present_serial;
}

/**
 * Allocates a shared Pixmap for a color buffer.
 */
static EGLBoolean CreateSharedPixmap(EplSurface *psurf, X11ColorBuffer *buffer, const EplFormatInfo *fmt)
{
    X11Window *pwin = (X11Window *) psurf->priv;
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    int fd = -1;

    assert(buffer->xpix == 0);

    // Note that XCB will close the file descriptor after it sends the request,
    // so even if we already have a file descriptor, we have to duplicate it.
    if (buffer->fd >= 0)
    {
        fd = dup(buffer->fd);
    }
    else
    {
        fd = gbm_bo_get_fd(buffer->gbo);
    }

    if (fd < 0)
    {
        return EGL_FALSE;
    }

    if (pwin->use_explicit_sync && buffer->timeline.xid == 0)
    {
        // If we're able to use explicit sync, then create a timeline object.
        if (!eplX11TimelineInit(pwin->inst, &buffer->timeline))
        {
            close(fd);
            return EGL_FALSE;
        }
    }

    // Temporary hack: Send the PixmapFromBuffers request synchronously to
    // check for errors.
    buffer->xpix = xcb_generate_id(pwin->inst->conn);
    cookie = xcb_dri3_pixmap_from_buffers_checked(pwin->inst->conn, buffer->xpix,
            pwin->inst->xscreen->root, 1,
            gbm_bo_get_width(buffer->gbo),
            gbm_bo_get_height(buffer->gbo),
            gbm_bo_get_stride(buffer->gbo),
            gbm_bo_get_offset(buffer->gbo, 0),
            0, 0, 0, 0, 0, 0,
            eplFormatInfoDepth(fmt), fmt->bpp,
            gbm_bo_get_modifier(buffer->gbo), &fd);

    error = xcb_request_check(pwin->inst->conn, cookie);
    if (error != NULL)
    {
        buffer->xpix = 0;
        free(error);
        return EGL_FALSE;
    }

    return EGL_TRUE;
}

static void WindowDamageCallback(void *param, int syncfd, unsigned int flags)
{
    EplSurface *surf = param;
    X11Window *pwin = (X11Window *) surf->priv;
    X11ColorBuffer *sharedPixmap = NULL;

    pthread_mutex_lock(&pwin->mutex);

    if (pwin->skip_update_callback)
    {
        // If we're in the middle of an eglSwapBuffers or teardown, then
        // don't bother doing anything here.
        goto done;
    }

    // Check for any pending events first.
    PollForWindowEvents(surf);
    if (pwin->native_destroyed || surf->deleted)
    {
        goto done;
    }

    if (pwin->prime)
    {
        sharedPixmap = pwin->current_prime;
    }
    else
    {
        sharedPixmap = pwin->current_front;
    }

    assert(sharedPixmap != NULL);

    if (sharedPixmap->xpix == 0)
    {
        if (!CreateSharedPixmap(surf, sharedPixmap, pwin->format->fmt))
        {
            goto done;
        }
    }

    if (pwin->use_explicit_sync)
    {
        EGLBoolean syncOK = EGL_FALSE;

        if (syncfd >= 0)
        {
            if (eplX11TimelineAttachSyncFD(pwin->inst, &sharedPixmap->timeline, syncfd))
            {
                syncOK = EGL_TRUE;
            }
        }

        if (!syncOK)
        {
            uint32_t handle;
            uint64_t point;

            if (!eplX11WaitForFD(syncfd))
            {
                goto done;
            }

            // If eplX11TimelineAttachSyncFD fails or if we don't have a
            // sync FD, then just manually signal the next timeline point.
            handle = sharedPixmap->timeline.handle;
            point = sharedPixmap->timeline.point + 1;
            if (pwin->inst->platform->priv->drm.SyncobjTimelineSignal(
                    gbm_device_get_fd(pwin->inst->gbmdev),
                    &handle, &point, 1) != 0)
            {
                goto done;
            }
            sharedPixmap->timeline.point++;
        }
    }
    else
    {
        // If we don't have explicit sync, then just do a CPU wait.
        // TODO: Is there a way that we can reliably use implicit sync if
        // the server supports it?
        if (!eplX11WaitForFD(syncfd))
        {
            goto done;
        }
    }

    SendPresentPixmap(surf, sharedPixmap, XCB_PRESENT_OPTION_ASYNC | XCB_PRESENT_OPTION_COPY);

done:
    pthread_mutex_unlock(&pwin->mutex);
}

EGLSurface eplX11CreateWindowSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform)
{
    X11DisplayInstance *inst = pdpy->priv->inst;
    xcb_window_t xwin = eplX11GetNativeXID(pdpy, native_surface, create_platform);
    xcb_void_cookie_t presentSelectCookie;
    xcb_get_window_attributes_cookie_t winodwAttribCookie;
    xcb_get_window_attributes_reply_t *windowAttribReply = NULL;
    xcb_present_query_capabilities_cookie_t presentCapsCookie;
    xcb_present_query_capabilities_reply_t *presentCapsReply = NULL;
    xcb_get_geometry_cookie_t geomCookie;
    xcb_get_geometry_reply_t *geomReply = NULL;
    xcb_generic_error_t *error = NULL;
    X11Window *pwin = NULL;
    EGLSurface esurf = EGL_NO_SURFACE;
    const EplConfig *configInfo;
    const X11DriverFormat *fmt;
    uint64_t *mods = NULL;
    int numMods = 0;
    EGLBoolean prime = EGL_FALSE;
    EGLAttrib platformAttribs[15];
    EGLAttrib *internalAttribs = NULL;
    uint32_t eventMask;

    if (xwin == 0)
    {
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "Invalid native window %p\n", native_surface);
        return EGL_NO_SURFACE;
    }

    configInfo = eplConfigListFind(inst->configs, config);
    if (configInfo == NULL)
    {
        eplSetError(plat, EGL_BAD_CONFIG, "Invalid EGLConfig %p", config);
        return EGL_NO_SURFACE;
    }
    if (!(configInfo->surfaceMask & EGL_WINDOW_BIT))
    {
        eplSetError(plat, EGL_BAD_CONFIG, "EGLConfig %p does not support windows", config);
        return EGL_NO_SURFACE;
    }

    internalAttribs = eplX11GetInternalSurfaceAttribs(plat, pdpy, internalAttribs);
    if (internalAttribs == NULL)
    {
        goto done;
    }

    fmt = eplX11FindDriverFormat(inst, configInfo->fourcc);
    assert(fmt != NULL);

    pwin = calloc(1, sizeof(X11Window));
    if (pwin == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }
    if (!eplInitRecursiveMutex(&pwin->mutex))
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Can't allocate internal mutex");
        free(pwin);
        pwin = NULL;
        goto done;
    }
    glvnd_list_init(&pwin->color_buffers);
    glvnd_list_init(&pwin->prime_buffers);
    surf->priv = (EplImplSurface *) pwin;
    pwin->inst = eplX11DisplayInstanceRef(inst);
    pwin->xwin = xwin;
    pwin->format = fmt;
    pwin->modifier = DRM_FORMAT_MOD_INVALID;
    pwin->swap_interval = 1;

    if (!FindSupportedModifiers(inst, fmt, xwin, &mods, &numMods, &prime))
    {
        eplSetError(plat, EGL_BAD_CONFIG, "No matching format modifiers for window");
        goto done;
    }

    presentCapsCookie = xcb_present_query_capabilities(inst->conn, xwin);
    presentCapsReply = xcb_present_query_capabilities_reply(inst->conn, presentCapsCookie, &error);
    if (presentCapsReply == NULL)
    {
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "Failed to query present capabilities for window 0x%x", xwin);
        goto done;
    }
    pwin->present_capabilities = presentCapsReply->capabilities;
    if ((pwin->present_capabilities & XCB_PRESENT_CAPABILITY_SYNCOBJ) && inst->supports_explicit_sync)
    {
        pwin->use_explicit_sync = EGL_TRUE;
    }

    /*
     * Send the PresentSelectInput event first. If we sent the
     * XGetWindowAttributes request first, then it would be possible for the
     * window to be resized after the XGetWindowAttributes and before the
     * PresentSelectInput, and we wouldn't see the new size.
     *
     * Note that if we have explicit sync support, then we'll wait on the
     * timeline sync objects to know when a buffer frees up. Otherwise, we need
     * to keep track of PresentIdleNotify events.
     */
    eventMask = XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY | XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY;
    if (!pwin->use_explicit_sync)
    {
        eventMask |= XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY;
    }
    pwin->present_event_id = xcb_generate_id(inst->conn);
    pwin->present_event = xcb_register_for_special_xge(inst->conn,
            &xcb_present_id, pwin->present_event_id, &pwin->present_event_stamp);
    presentSelectCookie = xcb_present_select_input_checked(inst->conn,
            pwin->present_event_id, xwin, eventMask);
    error = xcb_request_check(inst->conn, presentSelectCookie);
    if (error != NULL)
    {
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "Invalid window 0x%x", xwin);
        goto done;
    }

    winodwAttribCookie = xcb_get_window_attributes(inst->conn, xwin);
    windowAttribReply = xcb_get_window_attributes_reply(inst->conn, winodwAttribCookie, &error);
    if (windowAttribReply == NULL)
    {
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "Invalid window 0x%x", xwin);
        goto done;
    }
    if ((uint32_t) configInfo->nativeVisualID != windowAttribReply->visual)
    {
        eplSetError(plat, EGL_BAD_CONFIG, "EGLConfig %p uses X visual 0x%x, but window 0x%x uses visual 0x%x",
                config, configInfo->nativeVisualID, xwin, windowAttribReply->visual);
        goto done;
    }

    geomCookie = xcb_get_geometry(inst->conn, xwin);
    geomReply = xcb_get_geometry_reply(inst->conn, geomCookie, &error);
    if (geomReply == NULL)
    {
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "Invalid window 0x%x", xwin);
        goto done;
    }
    if (geomReply->root != inst->xscreen->root)
    {
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "Window 0x%x is on the wrong screen", xwin);
        goto done;
    }

    pwin->pending_width = geomReply->width;
    pwin->pending_height = geomReply->height;

    if (!AllocWindowBuffers(surf, mods, numMods, prime))
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Can't allocate color buffers");
        goto done;
    }

    platformAttribs[0] = GL_FRONT;
    platformAttribs[1] = (EGLAttrib) pwin->current_front->buffer;
    platformAttribs[2] = GL_BACK;
    platformAttribs[3] = (EGLAttrib) pwin->current_back->buffer;
    platformAttribs[4] = EGL_PLATFORM_SURFACE_BLIT_TARGET_NVX;
    if (pwin->current_prime != NULL)
    {
        platformAttribs[5] = (EGLAttrib) pwin->current_prime->buffer;
    }
    else
    {
        platformAttribs[5] = (EGLAttrib) NULL;
    }

    platformAttribs[6] = EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_NVX;
    platformAttribs[7] = (EGLAttrib) WindowUpdateCallback;
    platformAttribs[8] = EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_PARAM_NVX;
    platformAttribs[9] = (EGLAttrib) surf;
    platformAttribs[10] = EGL_PLATFORM_SURFACE_DAMAGE_CALLBACK_NVX;
    platformAttribs[11] = (EGLAttrib) WindowDamageCallback;
    platformAttribs[12] = EGL_PLATFORM_SURFACE_DAMAGE_CALLBACK_PARAM_NVX;
    platformAttribs[13] = (EGLAttrib) surf;
    platformAttribs[14] = EGL_NONE;
    esurf = inst->platform->priv->egl.PlatformCreateSurfaceNVX(inst->internal_display->edpy,
            config, platformAttribs, internalAttribs);

done:
    if (esurf == EGL_NO_SURFACE)
    {
        eplX11FreeWindow(surf);
    }
    free(windowAttribReply);
    free(geomReply);
    free(presentCapsReply);
    free(error);
    free(mods);
    free(internalAttribs);
    return esurf;
}

void eplX11DestroyWindow(EplSurface *surf)
{
    X11Window *pwin = (X11Window *) surf->priv;
    EGLSurface internalSurf;

    assert(surf->type == EPL_SURFACE_TYPE_WINDOW);

    /*
     * Lock the surface and increment skip_update_callback. After that, if
     * another thread tries to call the update callback after this, then
     * the update callback won't try to call into the driver.
     */
    pthread_mutex_lock(&pwin->mutex);

    pwin->skip_update_callback++;
    internalSurf = surf->internal_surface;

    pthread_mutex_unlock(&pwin->mutex);

    /*
     * We have to unlock the surface before we call the driver's
     * eglDestroySurface.
     *
     * If another thread tries to call the window update callback for this
     * surface, then it will be holding a mutex in the driver, and then the
     * callback will try to lock the surface's mutex.
     *
     * If we had the surface locked here, then the driver's eglDestroySurface
     * implementation would try to take the driver's mutex, which would lead to
     * a deadlock.
     */
    if (internalSurf != EGL_NO_SURFACE)
    {
        pwin->inst->platform->egl.DestroySurface(pwin->inst->internal_display->edpy, internalSurf);
    }
}

/**
 * Waits for at least one Present event to arrive.
 *
 * This function will unlock the surface and the display while waiting, so the
 * caller must check the EplSurface::deleted flag to check whether another
 * thread destroyed the surface in the meantime.
 */
static EGLBoolean WaitForWindowEvents(EplDisplay *pdpy, EplSurface *surf)
{
    X11Window *pwin = (X11Window *) surf->priv;
    xcb_generic_event_t *xcbevt;

    /*
     * We don't want to block other threads while we wait, so we need to
     * release our locks.
     *
     * The use counter in EplDisplay should prevent the display from
     * getting terminated out from under us, and the refcount in EplSurface
     * will ensure that the EplSurface struct itself sticks around.
     *
     * It's still possible that the surface will get destroyed by another
     * thread in the meantime, though, so the caller needs to check for that.
     */

    if (pwin->native_destroyed)
    {
        /*
         * If the X window was destroyed, then xcb_wait_for_special_event would
         * hang because it would never receive any events for that window.
         * However, a new enough X server will send a PresentConfigureNotify
         * event with a special flag set to notify the client when that
         * happens.
         *
         * Note that even though we unlock the window's mutex before calling
         * xcb_wait_for_special_event, we should still be safe from race
         * conditions. This function is only called from eglSwapBuffers, which
         * means that the window must be current. Thus, only one thread will
         * ever be here for this window.
         *
         * There is still a race condition if the X window gets destroyed
         * before the server receives the PresentPixmap request, but there's
         * not much that we can do about that.
         */
        return EGL_TRUE;
    }

    pthread_mutex_unlock(&pwin->mutex);
    eplDisplayUnlock(pdpy);

    xcbevt = xcb_wait_for_special_event(pwin->inst->conn, pwin->present_event);

    eplDisplayLock(pdpy);
    pthread_mutex_lock(&pwin->mutex);

    // Sanity check: If something called eglTerminate, then that should have
    // destroyed the surface.
    assert(pdpy->priv->inst == pwin->inst || surf->deleted);

    if (surf->deleted)
    {
        /*
         * Some other thread came along and called eglDestroySurface or
         * eglTerminate.
         */
        return EGL_TRUE;
    }

    if (xcbevt == NULL)
    {
        // Note that this only happens if the X11 connection gets killed, as
        // per XKillClient.
        eplSetError(pwin->inst->platform, EGL_BAD_ALLOC, "Failed to check window-system events.");
        pwin->native_destroyed = EGL_TRUE;
        return EGL_FALSE;
    }

    HandlePresentEvent(surf, xcbevt);
    free(xcbevt);

    // There might be more than one event, so poll, but don't wait for them.
    PollForWindowEvents(surf);

    return EGL_TRUE;
}

/**
 * Flush the command stream, and set up synchronization.
 *
 * If explicit sync is supported, then this will set up the next acquire point.
 *
 * Otherwise, this will fall back to using implicit sync if it's available, or
 * a simple glFinish if it's not.
 *
 * \param surf The window surface
 * \param buffer The shared buffer (either a render or a pitch linear buffer)
 * \return EGL_TRUE on success, or EGL_FALSE on failure.
 */
static EGLBoolean SyncRendering(EplDisplay *pdpy, EplSurface *surf, X11ColorBuffer *buffer)
{
    X11Window *pwin = (X11Window *) surf->priv;
    int syncFd = -1;
    EGLSync sync = EGL_NO_SYNC;
    EGLBoolean success = EGL_FALSE;

    if (!pwin->inst->supports_EGL_ANDROID_native_fence_sync)
    {
        // If we don't have EGL_ANDROID_native_fence_sync, then we can't do
        // anything other than a glFinish here.
        assert(!pwin->use_explicit_sync);
        pwin->inst->platform->priv->egl.Finish();
        return EGL_TRUE;
    }

    pwin->inst->platform->priv->egl.Flush();

    sync = pwin->inst->platform->priv->egl.CreateSync(pwin->inst->internal_display->edpy,
            EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC)
    {
        goto done;
    }

    syncFd = pwin->inst->platform->priv->egl.DupNativeFenceFDANDROID(pwin->inst->internal_display->edpy, sync);
    if (syncFd < 0)
    {
        goto done;
    }

    if (pwin->use_explicit_sync)
    {
        // If we support explicit sync, then always use that.
        if (eplX11TimelineAttachSyncFD(pwin->inst, &buffer->timeline, syncFd))
        {
            success = EGL_TRUE;
        }
        else
        {
            /*
             * In theory, if eplX11TimelineAttachSyncFD fails, then we could
             * fall back here to a simple glFinish and then send an
             * already-signaled acquire point with the PresentPixmapSynced
             * request.
             *
             * For now, though, just fail eglSwapBuffers in this case.
             */
            eplSetError(pwin->inst->platform, EGL_BAD_ALLOC, "Failed to attach timeline point");
        }
    }
    else if (eplX11ImportDmaBufSyncFile(pwin->inst, buffer->fd, syncFd))
    {
        success = EGL_TRUE;
    }
    else
    {
        pwin->inst->platform->priv->egl.Finish();
        success = EGL_TRUE;
    }

done:
    if (sync != EGL_NO_SYNC)
    {
        pwin->inst->platform->priv->egl.DestroySync(pwin->inst->internal_display->edpy, sync);
    }
    if (syncFd >= 0)
    {
        close(syncFd);
    }
    return success;
}

/**
 * Waits for a sync FD using eglWaitSync.
 *
 * Using eglWaitSync means that the GPU will wait for the fence, without
 * doing a CPU stall.
 *
 * \param inst The X11DisplayInstance
 * \param syncfd The sync file descriptor. This must be a regular fence.
 */
static EGLBoolean WaitForSyncFDGPU(X11DisplayInstance *inst, int syncfd)
{
    EGLBoolean success = EGL_FALSE;

    if (syncfd >= 0)
    {
        const EGLAttrib syncAttribs[] =
        {
            EGL_SYNC_NATIVE_FENCE_FD_ANDROID, syncfd,
            EGL_NONE
        };
        EGLSync sync = inst->platform->priv->egl.CreateSync(inst->internal_display->edpy,
                EGL_SYNC_NATIVE_FENCE_ANDROID, syncAttribs);
        if (sync != EGL_NO_SYNC)
        {
            success = inst->platform->priv->egl.WaitSync(inst->internal_display->edpy, sync, 0);
            inst->platform->priv->egl.DestroySync(inst->internal_display->edpy, sync);
        }
    }
    return success;
}

static EGLBoolean WaitImplicitFence(EplDisplay *pdpy, X11ColorBuffer *buffer)
{
    EGLBoolean success = EGL_FALSE;
    int fd = -1;

    assert(pdpy->priv->inst->supports_implicit_sync);

    fd = eplX11ExportDmaBufSyncFile(pdpy->priv->inst, buffer->fd);
    if (fd >= 0)
    {
        success = WaitForSyncFDGPU(pdpy->priv->inst, fd);
        close(fd);
    }

    if (success)
    {
        buffer->status = BUFFER_STATUS_IDLE;
    }

    return success;
}

/**
 * Waits or polls for a buffer to free up, using implicit sync.
 *
 * Note that we can only wait for a buffer if we've received a
 * PresentIdleNotify event. If no buffers were ready, then the caller
 * has to wait for a Present event and try again.
 *
 * \param pdpy The EplDisplay pointer.
 * \param surf The EplSurface pointer.
 * \param buffer_list The list of buffers to check.
 * \param skip If not NULL, then ignore this buffer when checking the rest.
 * \param timeout_ms The number of milliseconds to wait. Zero to poll without blocking.
 *
 * \return The number of buffers that were checked, or -1 on error.
 */
static int CheckBufferReleaseImplicit(EplDisplay *pdpy, EplSurface *surf,
        struct glvnd_list *buffer_list,
        X11ColorBuffer *skip, int timeout_ms)
{
    X11Window *pwin = (X11Window *) surf->priv;
    X11ColorBuffer *buffer;
    X11ColorBuffer **buffers;
    struct pollfd *fds;
    int count;
    int i;
    int ret, err;

    PollForWindowEvents(surf);

    count = 0;
    glvnd_list_for_each_entry(buffer, buffer_list, entry)
    {
        if (buffer != skip && buffer->status == BUFFER_STATUS_IDLE_NOTIFIED)
        {
            /*
             * If possible, extract a syncfd and wait on it using eglWaitSync,
             * instead of doing a CPU wait.
             */
            if (WaitImplicitFence(pdpy, buffer))
            {
                assert(buffer->status == BUFFER_STATUS_IDLE);
                return 1;
            }
            count++;
        }
    }

    if (count == 0)
    {
        return 0;
    }

    buffers = alloca(count * sizeof(X11ColorBuffer *));
    fds = alloca(count * sizeof(struct pollfd));

    count = 0;
    glvnd_list_for_each_entry(buffer, buffer_list, entry)
    {
        if (buffer != skip && buffer->status == BUFFER_STATUS_IDLE_NOTIFIED)
        {
            buffers[count] = buffer;
            fds[count].fd = buffer->fd;
            fds[count].events = POLLOUT;
            fds[count].revents = 0;
            count++;
        }
    }

    // Release the locks while we wait, so that we don't block
    // other threads.
    pthread_mutex_unlock(&pwin->mutex);
    eplDisplayUnlock(pdpy);

    ret = poll(fds, count, timeout_ms);
    err = errno;

    eplDisplayLock(pdpy);
    pthread_mutex_lock(&pwin->mutex);

    if (surf->deleted)
    {
        /*
         * Some other thread came along and called
         * eglDestroySurface or eglTerminate.
         */
        return count;
    }
    else if (ret > 0)
    {
        for (i=0; i<count; i++)
        {
            if (fds[i].revents & POLLOUT)
            {
                buffers[i]->status = BUFFER_STATUS_IDLE;
            }
        }
        return count;
    }
    else if (ret == 0 || err == ETIME || err == EINTR)
    {
        // Nothing freed up before the timeout, but that's not a fatal error
        // here.
        return count;
    }
    else
    {
        eplSetError(pwin->inst->platform, EGL_BAD_ALLOC, "Internal error: poll() failed: %s\n",
                strerror(err));
        return -1;
    }
}

/**
 * Checks for a free buffer, without any sort of server -> client sync.
 *
 * Without any server -> client synchronization, the best we can do is to
 * wait for a PresentIdleNotify event, and then hope that the buffer really is
 * idle by the time we start rendering to it again.
 */
static int CheckBufferReleaseNoSync(EplDisplay *pdpy, EplSurface *surf,
        struct glvnd_list *buffer_list, X11ColorBuffer *skip)
{
    X11ColorBuffer *buffer;
    int numPending = 0;

    PollForWindowEvents(surf);

    glvnd_list_for_each_entry(buffer, buffer_list, entry)
    {
        if (buffer != skip && buffer->status == BUFFER_STATUS_IDLE_NOTIFIED)
        {
            buffer->status = BUFFER_STATUS_IDLE;
            numPending++;
        }
    }

    return numPending;
}

/**
 * Waits for a timeline point.
 *
 * This will attempt to use eglWaitSync to let the GPU wait on the sync point,
 * but if that fails, then it'll fall back to a CPU wait.
 */
static EGLBoolean WaitTimelinePoint(X11DisplayInstance *inst, X11Timeline *timeline)
{
    int syncfd = eplX11TimelinePointToSyncFD(inst, timeline);
    EGLBoolean success = EGL_FALSE;

    if (syncfd >= 0)
    {
        success = WaitForSyncFDGPU(inst, syncfd);
    }

    if (!success)
    {
        // If using eglWaitSync failed, then just do a CPU wait on the timeline
        // point.
        uint32_t first;
        if (inst->platform->priv->drm.SyncobjTimelineWait(
                    gbm_device_get_fd(inst->gbmdev),
                    &timeline->handle, &timeline->point, 1, INT64_MAX,
                    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                    &first) == 0)
        {
            eplSetError(inst->platform, EGL_BAD_ALLOC,
                    "Internal error: drmSyncobjTimelineWait(WAIT_FOR_SUBMIT) failed: %s\n",
                    strerror(errno));
            success = EGL_TRUE;
        }
    }

    return success;
}

/**
 * Waits or polls for a buffer to free up, using explicit sync.
 *
 * Unlike implicit sync, we don't need to wait for a PresentIdleNotify event
 * before waiting on a buffer.
 *
 * \param pdpy The EplDisplay pointer.
 * \param surf The EplSurface pointer.
 * \param buffer_list The list of buffers to check.
 * \param skip If not NULL, then ignore this buffer when checking the rest.
 * \param timeout_ms The number of milliseconds to wait. Zero to poll without blocking.
 *
 * \return The number of buffers that were checked, or -1 on error.
 */
static int CheckBufferReleaseExplicit(EplDisplay *pdpy, EplSurface *surf,
        struct glvnd_list *buffer_list,
        X11ColorBuffer *skip, int timeout_ms)
{
    X11Window *pwin = (X11Window *) surf->priv;
    X11ColorBuffer *buffer;
    X11ColorBuffer **buffers;
    uint32_t *handles;
    uint64_t *points;
    int64_t timeout;
    uint32_t count;
    uint32_t first;
    int ret, err;

    count = 0;
    glvnd_list_for_each_entry(buffer, buffer_list, entry)
    {
        if (buffer != skip && buffer->status != BUFFER_STATUS_IDLE)
        {
            count++;
        }
    }

    if (count == 0)
    {
        return 0;
    }

    buffers = alloca(count * sizeof(X11ColorBuffer *));
    handles = alloca(count * sizeof(uint32_t));
    points = alloca(count * sizeof(uint64_t));

    count = 0;
    glvnd_list_for_each_entry(buffer, buffer_list, entry)
    {
        if (buffer != skip && buffer->status != BUFFER_STATUS_IDLE)
        {
            buffers[count] = buffer;
            handles[count] = buffer->timeline.handle;
            points[count] = buffer->timeline.point;
            count++;
        }
    }

    if (timeout_ms > 0)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        timeout = ((int64_t) ts.tv_sec) * 1000000000 + ts.tv_nsec;
        timeout += timeout_ms * 1000000;
    }
    else
    {
        timeout = 0;
    }

    // Release the locks while we wait, so that we don't block
    // other threads.
    pthread_mutex_unlock(&pwin->mutex);
    eplDisplayUnlock(pdpy);

    ret = pwin->inst->platform->priv->drm.SyncobjTimelineWait(
                gbm_device_get_fd(pwin->inst->gbmdev),
                handles, points, count, timeout,
                DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                &first);
    err = errno;

    eplDisplayLock(pdpy);
    pthread_mutex_lock(&pwin->mutex);

    if (surf->deleted)
    {
        /*
         * Some other thread came along and called
         * eglDestroySurface or eglTerminate.
         */
        return count;
    }
    else if (ret == 0)
    {
        assert(first < count);
        if (WaitTimelinePoint(pwin->inst, &buffers[first]->timeline))
        {
            buffers[first]->status = BUFFER_STATUS_IDLE;
            return count;
        }
        else
        {
            return -1;
        }
    }
    else if (err == ETIME || err == EINTR)
    {
        // Nothing freed up before the timeout, but that's not a fatal error
        // here.
        return count;
    }
    else
    {
        eplSetError(pwin->inst->platform, EGL_BAD_ALLOC,
                "Internal error: drmSyncobjTimelineWait(WAIT_AVAILABLE) failed: %s\n",
                strerror(err));
        return -1;
    }
}

/**
 * Returns a free buffer.
 *
 * \param pdpy The EplDisplay pointer
 * \param surf The EplSurface pointer
 * \param skip If non-NULL, then ignore this buffer even if it's free.
 * \param prime If true, then look for a free PRIME buffer. Otherwise, look for
 *      a regular color buffer.
 * \return A free X11ColorBuffer, or NULL on failure. This will also return
 *      NULL if the EGLSurface or the native window is destroyed.
 */
static X11ColorBuffer *GetFreeBuffer(EplDisplay *pdpy, EplSurface *surf,
        X11ColorBuffer *skip, EGLBoolean prime)
{
    X11Window *pwin = (X11Window *) surf->priv;
    struct glvnd_list *buffers;
    int maxBuffers;

    if (prime)
    {
        assert(pwin->prime);
        buffers = &pwin->prime_buffers;
        maxBuffers = MAX_PRIME_BUFFERS;
    }
    else
    {
        buffers = &pwin->color_buffers;
        maxBuffers = MAX_COLOR_BUFFERS;
    }

    /*
     * First, poll to see if any buffers have already freed up. Do this up
     * front so that we don't try to allocate a new buffer unnecessarily.
     */
    if (pwin->use_explicit_sync)
    {
        if (CheckBufferReleaseExplicit(pdpy, surf, buffers, skip, 0) < 0)
        {
            return NULL;
        }
    }
    else if (pwin->inst->supports_implicit_sync)
    {
        if (CheckBufferReleaseImplicit(pdpy, surf, buffers, skip, 0) < 0)
        {
            return NULL;
        }
    }
    else
    {
        if (CheckBufferReleaseNoSync(pdpy, surf, buffers, skip) < 0)
        {
            return NULL;
        }
    }

    while (!surf->deleted && !pwin->native_destroyed)
    {
        X11ColorBuffer *buffer = NULL;
        int numBuffers = 0;

        // Look to see if a buffer is already free.
        glvnd_list_for_each_entry(buffer, buffers, entry)
        {
            if (buffer->status == BUFFER_STATUS_IDLE && buffer != skip)
            {
                return buffer;
            }
            numBuffers++;
        }

        if (numBuffers < maxBuffers)
        {
            // We didn't find a free buffer, but we don't have our maximum
            // number of buffers yet, so allocate a new one.
            if (prime)
            {
                buffer = AllocatePrimeBuffer(pwin->inst, pwin->format->fourcc, pwin->width, pwin->height);
            }
            else
            {
                buffer = AllocOneColorBuffer(pwin->inst, pwin->format->fmt, pwin->width, pwin->height,
                        &pwin->modifier, 1, !pwin->prime);
            }
            if (buffer == NULL)
            {
                return NULL;
            }
            glvnd_list_add(&buffer->entry, buffers);
            return buffer;
        }

        // Otherwise, we have to wait for a buffer to free up.

        if (pwin->use_explicit_sync)
        {
            /*
             * With explicit sync, we can just wait on every buffer, even if we
             * haven't gotten a PresentIdleNotify event.
             *
             * We do still poll for window events, though, in case the native
             * window gets destroyed while we're waiting.
             */
            if (CheckBufferReleaseExplicit(pdpy, surf, buffers, skip, RELEASE_WAIT_TIMEOUT) <= 0)
            {
                return NULL;
            }

            PollForWindowEvents(surf);
        }
        else
        {
            int numChecked;

            if (pwin->inst->supports_implicit_sync)
            {
                numChecked = CheckBufferReleaseImplicit(pdpy, surf, buffers, skip, RELEASE_WAIT_TIMEOUT);
            }
            else
            {
                numChecked = CheckBufferReleaseNoSync(pdpy, surf, buffers, skip);
            }
            if (numChecked < 0)
            {
                return NULL;
            }
            else if (numChecked == 0)
            {
                /*
                 * There weren't any buffers to wait on yet, so wait for a
                 * present event.
                 *
                 * If we receieve a PresentIdleNotify event, then HandlePresentEvent
                 * will mark the corresponding buffer as ready to wait on, and then
                 * CheckBufferReleaseImplicit/NoSync will find it on the next pass
                 * through this loop.
                 */
                if (!WaitForWindowEvents(pdpy, surf))
                {
                    return NULL;
                }
            }
        }
    }

    return NULL;
}

static EGLBoolean CheckWindowDeleted(EplSurface *surf, EGLBoolean *ret_success)
{
    X11Window *pwin = (X11Window *) surf->priv;
    if (surf->deleted)
    {
        *ret_success = EGL_TRUE;
        return EGL_TRUE;
    }
    else if (pwin->native_destroyed)
    {
        *ret_success = EGL_FALSE;
        eplSetError(pwin->inst->platform, EGL_BAD_NATIVE_WINDOW, "The X11 window has been destroyed");
        return EGL_TRUE;
    }

    return EGL_FALSE;
}

EGLBoolean eplX11SwapBuffers(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *surf,
        const EGLint *rects, EGLint n_rects)
{
    X11Window *pwin = (X11Window *) surf->priv;
    X11ColorBuffer *sharedPixmap = NULL;
    uint32_t options = 0;
    EGLBoolean resized = EGL_FALSE;
    EGLBoolean ret = EGL_FALSE;

    pthread_mutex_lock(&pwin->mutex);

    // Disable the update callback, so that we don't have to worry about it
    // reallocating the color buffers while we're trying to rearrange them.
    pwin->skip_update_callback++;

    if (CheckWindowDeleted(surf, &ret))
    {
        goto done;
    }

    if (pwin->prime)
    {
        sharedPixmap = GetFreeBuffer(pdpy, surf, NULL, EGL_TRUE);
        if (CheckWindowDeleted(surf, &ret))
        {
            goto done;
        }
        if (sharedPixmap == NULL)
        {
            goto done;
        }

        // Blit from the current back buffer to the shared linear buffer.
        if (!pwin->inst->platform->priv->egl.PlatformCopyColorBufferNVX(pwin->inst->internal_display->edpy,
                    pwin->current_back->buffer, sharedPixmap->buffer))
        {
            eplSetError(plat, EGL_BAD_ALLOC, "Failed to blit back buffer");
            goto done;
        }
    }
    else
    {
        // For normal rendering, we share the back buffer directly with the
        // server.
        sharedPixmap = pwin->current_back;
    }

    if (sharedPixmap->xpix == 0)
    {
        if (!CreateSharedPixmap(surf, sharedPixmap, pwin->format->fmt))
        {
            eplSetError(plat, EGL_BAD_ALLOC, "Can't create shared pixmap");
            goto done;
        }
    }

    // Sanity check: We shouldn't have been rendering to a buffer while it's in
    // use in the server.
    assert(sharedPixmap->status == BUFFER_STATUS_IDLE);

    if (!SyncRendering(pdpy, surf, sharedPixmap))
    {
        goto done;
    }

    if (!pwin->inst->force_prime)
    {
        // If we're always using PRIME, then the shared pixmap will always be
        // DRM_FORMAT_MOD_LINEAR, so it doesn't matter whether that's optimal
        // or not.
        options |= XCB_PRESENT_OPTION_SUBOPTIMAL;
    }

    // Wait for pending frames to complete before we continue.
    while (1)
    {
        uint32_t pending = pwin->last_present_serial - pwin->last_complete_serial;
        if (pending <= MAX_PENDING_FRAMES)
        {
            break;
        }

        if (!WaitForWindowEvents(pdpy, surf))
        {
            goto done;
        }
        if (CheckWindowDeleted(surf, &ret))
        {
            goto done;
        }
    }

    SendPresentPixmap(surf, sharedPixmap, options);

    /*
     * Check if we need to reallocate the buffers to deal with a resize or new
     * format modifiers.
     *
     * If we have to reallocate the buffer, then AllocWindowBuffers will
     * attach new front and back buffers.
     */
    if (!CheckReallocWindow(surf, EGL_TRUE, &resized))
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to allocate resized buffers.");
        goto done;
    }

    if (!resized)
    {
        X11ColorBuffer *newBack = NULL;
        EGLAttrib buffers[] =
        {
            GL_BACK, 0,
            EGL_PLATFORM_SURFACE_BLIT_TARGET_NVX, 0,
            GL_FRONT, (EGLAttrib) pwin->current_back->buffer,
            EGL_NONE
        };

        if (pwin->prime)
        {
            // For PRIME, the server gets the linear buffer, so we can just swap
            // the front and back buffers.
            newBack = pwin->current_front;
        }
        else
        {
            newBack = GetFreeBuffer(pdpy, surf, pwin->current_back, EGL_FALSE);
            if (CheckWindowDeleted(surf, &ret))
            {
                goto done;
            }
            if (newBack == NULL)
            {
                goto done;
            }
        }
        buffers[1] = (EGLAttrib) newBack->buffer;

        pwin->current_front = pwin->current_back;
        pwin->current_back = newBack;
        if (pwin->prime)
        {
            pwin->current_prime = sharedPixmap;
            buffers[3] = (EGLAttrib) sharedPixmap->buffer;
        }
        ret = pwin->inst->platform->priv->egl.PlatformSetColorBuffersNVX(pwin->inst->internal_display->edpy,
                    surf->internal_surface, buffers);
        if (!ret)
        {
            // Note: This should never fail. The drawable doesn't change size
            // here, so the driver doesn't need to reallocate anything.
            eplSetError(plat, EGL_BAD_ALLOC, "Driver error: Can't assign new color buffers");
            goto done;
        }
    }

    ret = EGL_TRUE;
    assert(pwin->current_back->status == BUFFER_STATUS_IDLE);

done:
    pwin->skip_update_callback--;
    pthread_mutex_unlock(&pwin->mutex);
    return ret;
}

EGLBoolean eplX11SwapInterval(EGLDisplay edpy, EGLint interval)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    EGLSurface esurf;
    EGLBoolean ret = EGL_FALSE;

    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    esurf = pdpy->platform->egl.GetCurrentSurface(EGL_DRAW);
    if (esurf != EGL_NO_SURFACE)
    {
        EplSurface *psurf = eplSurfaceAcquire(pdpy, esurf);
        if (psurf != NULL)
        {
            if (psurf->type == EPL_SURFACE_TYPE_WINDOW)
            {
                X11Window *pwin = (X11Window *) psurf->priv;
                pwin->swap_interval = interval;
                if (pwin->swap_interval < 0)
                {
                    pwin->swap_interval = 0;
                }
            }
            eplSurfaceRelease(pdpy, psurf);
            ret = EGL_TRUE;
        }
        else
        {
            // If we don't recognize he current EGLSurface, then just pass the
            // call through to the driver.
            ret = pdpy->platform->priv->egl.SwapInterval(edpy, interval);
        }
    }
    else
    {
        eplSetError(pdpy->platform, EGL_BAD_SURFACE, "eglSwapInterval called without a current EGLSurface");
    }

    eplDisplayRelease(pdpy);

    return ret;
}

EGLBoolean eplX11WaitGLWindow(EplDisplay *pdpy, EplSurface *psurf)
{
    X11Window *pwin = (X11Window *) psurf->priv;

    while ((pwin->last_present_serial - pwin->last_complete_serial) > 0
            && !psurf->deleted && !pwin->native_destroyed)
    {
        if (!WaitForWindowEvents(pdpy, psurf))
        {
            return EGL_FALSE;
        }
    }

    return EGL_TRUE;
}
