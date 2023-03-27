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

#include "platform-utils.h"

#include <stdlib.h>
#include <string.h>

static int HookFuncCmp(const void *key, const void *elem)
{
    return strcmp((const char *) key, ((const EplHookFunc *) elem)->name);
}

void *eplFindHookFunction(const EplHookFunc *funcs, size_t count, const char *name)
{
    const EplHookFunc *found = bsearch(name, funcs, count, sizeof(EplHookFunc), HookFuncCmp);
    if (found != NULL)
    {
        return found->func;
    }
    else
    {
        return NULL;
    }
}

EGLBoolean eplFindExtension(const char *extension, const char *extensions)
{
    const char *start;
    const char *where, *terminator;

    if (extension == NULL || extensions == NULL)
    {
        return EGL_FALSE;
    }

    start = extensions;
    for (;;) {
        where = strstr(start, extension);
        if (!where) {
            break;
        }
        terminator = where + strlen(extension);
        if (where == start || *(where - 1) == ' ') {
            if (*terminator == ' ' || *terminator == '\0') {
                return EGL_TRUE;
            }
        }
        start = terminator;
    }

    return EGL_FALSE;
}

EGLBoolean eplInitRecursiveMutex(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0)
    {
        return EGL_FALSE;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(mutex, &attr) != 0)
    {
        pthread_mutexattr_destroy(&attr);
        return EGL_FALSE;
    }

    pthread_mutexattr_destroy(&attr);
    return EGL_TRUE;
}

EGLint eplCountAttribs(const EGLAttrib *attribs)
{
    EGLint count = 0;
    if (attribs != NULL)
    {
        while (attribs[count] != EGL_NONE)
        {
            count += 2;
        }
    }
    return count;
}

EGLint eplCountAttribs32(const EGLint *attribs)
{
    EGLint count = 0;
    if (attribs != NULL)
    {
        while (attribs[count] != EGL_NONE)
        {
            count += 2;
        }
    }
    return count;
}
