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

#include "x11-timeline.h"

#include <string.h>
#include <unistd.h>
#include <assert.h>

EGLBoolean eplX11TimelineInit(X11DisplayInstance *inst, X11Timeline *timeline)
{
    int fd = -1;
    int ret;

    memset(timeline, 0, sizeof(*timeline));

    if (!inst->supports_explicit_sync)
    {
        assert(inst->supports_explicit_sync);
        return EGL_FALSE;
    }

    ret = inst->platform->priv->drm.SyncobjCreate(
            gbm_device_get_fd(inst->gbmdev),
            0, &timeline->handle);
    if (ret != 0)
    {
        return EGL_FALSE;
    }

    ret = inst->platform->priv->drm.SyncobjHandleToFD(
            gbm_device_get_fd(inst->gbmdev),
            timeline->handle, &fd);
    if (ret != 0)
    {
        inst->platform->priv->drm.SyncobjDestroy(
                gbm_device_get_fd(inst->gbmdev),
                timeline->handle);
        close(fd);
        return EGL_FALSE;
    }

    timeline->xid = xcb_generate_id(inst->conn);
    // Note that libxcb will close the file descriptor after it sends the
    // request, so we do *not* close it here.
    inst->platform->priv->xcb.dri3_import_syncobj(inst->conn,
            timeline->xid, inst->xscreen->root, fd);
    return EGL_TRUE;
}

void eplX11TimelineDestroy(X11DisplayInstance *inst, X11Timeline *timeline)
{
    // The XID is non-zero if and only if eplX11TimelineInit was successfully
    // called.
    if (timeline->xid != 0)
    {
        inst->platform->priv->xcb.dri3_free_syncobj(inst->conn, timeline->xid);
        timeline->xid = 0;

        inst->platform->priv->drm.SyncobjDestroy(
                gbm_device_get_fd(inst->gbmdev),
                timeline->handle);
        timeline->handle = 0;
    }
}

int eplX11TimelinePointToSyncFD(X11DisplayInstance *inst, X11Timeline *timeline)
{
    uint32_t tempobj = 0;
    int syncfd = -1;

    if (inst->platform->priv->drm.SyncobjCreate(
                gbm_device_get_fd(inst->gbmdev),
                0, &tempobj) != 0)
    {
        return EGL_FALSE;
    }

    if (inst->platform->priv->drm.SyncobjTransfer(gbm_device_get_fd(inst->gbmdev),
			      tempobj, 0, timeline->handle, timeline->point, 0) != 0)
    {
        goto done;
    }

    if (inst->platform->priv->drm.SyncobjExportSyncFile(gbm_device_get_fd(inst->gbmdev),
            tempobj, &syncfd) != 0)
    {
        goto done;
    }

done:
    inst->platform->priv->drm.SyncobjDestroy(
            gbm_device_get_fd(inst->gbmdev),
            tempobj);
    return syncfd;
}

EGLBoolean eplX11TimelineAttachSyncFD(X11DisplayInstance *inst, X11Timeline *timeline, int syncfd)
{
    uint32_t tempobj = 0;
    EGLBoolean success = EGL_FALSE;

    assert(syncfd >= 0);

    if (inst->platform->priv->drm.SyncobjCreate(
                gbm_device_get_fd(inst->gbmdev),
                0, &tempobj) != 0)
    {
        // TODO: Issue an EGL error here?
        return EGL_FALSE;
    }

    if (inst->platform->priv->drm.SyncobjImportSyncFile(
                gbm_device_get_fd(inst->gbmdev),
                tempobj, syncfd) != 0)
    {
        goto done;
    }

    if (inst->platform->priv->drm.SyncobjTransfer(
                gbm_device_get_fd(inst->gbmdev),
                timeline->handle, timeline->point + 1,
                tempobj, 0, 0) != 0)
    {
        goto done;
    }

    timeline->point++;
    success = EGL_TRUE;

done:
    inst->platform->priv->drm.SyncobjDestroy(
            gbm_device_get_fd(inst->gbmdev),
            tempobj);
    return success;
}
