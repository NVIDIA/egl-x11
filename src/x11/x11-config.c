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

