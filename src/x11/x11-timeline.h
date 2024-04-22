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

#ifndef X11_TIMELINE_H
#define X11_TIMELINE_H

/**
 * \file
 *
 * Functions for dealing with timeline sync objects.
 */

#include <EGL/egl.h>
#include <xcb/xcb.h>

#include "x11-platform.h"

typedef struct
{
    uint32_t handle;
    uint32_t xid;
    uint64_t point;
} X11Timeline;

/**
 * Creates and initializes a timeline sync object.
 *
 * This will create a timeline object, and share it with the server using DRI3.
 */
EGLBoolean eplX11TimelineInit(X11DisplayInstance *inst, X11Timeline *timeline);
void eplX11TimelineDestroy(X11DisplayInstance *inst, X11Timeline *timeline);

/**
 * Attaches a sync FD to the next timeline point.
 *
 * On a successful return, \c timeline->point will be the timeline point where
 * the sync FD was attached.
 */
EGLBoolean eplX11TimelineAttachSyncFD(X11DisplayInstance *inst, X11Timeline *timeline, int syncfd);

/**
 * Extracts a sync FD from the current timeline point.
 */
int eplX11TimelinePointToSyncFD(X11DisplayInstance *inst, X11Timeline *timeline);

#endif // X11_TIMELINE_H
