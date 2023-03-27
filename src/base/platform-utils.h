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

#ifndef PLATFORM_UTILS_H
#define PLATFORM_UTILS_H

/**
 * \file
 *
 * Other utility functions that don't fit elsewhere.
 */

#include <stddef.h>
#include <pthread.h>
#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char *name;
    void *func;
} EplHookFunc;

/**
 * Looks up a function from an array of EplHookFunc structs.
 *
 * This will use a binary search, so \p funcs must be sorted by name.
 */
void *eplFindHookFunction(const EplHookFunc *funcs, size_t count, const char *name);

/**
 * Returns true if \p extension is listed in \p extensions.
 */
EGLBoolean eplFindExtension(const char *extension, const char *extensions);

/**
 * Initializes a recursive mutex.
 */
EGLBoolean eplInitRecursiveMutex(pthread_mutex_t *mutex);

/**
 * Returns the length of an attribute array.
 *
 * \param attribs An EGLAttrib array, or NULL.
 * \return The length of the array, not including the EGL_NONE at the end. This
 *      will always be a multiple of 2.
 */
EGLint eplCountAttribs(const EGLAttrib *attribs);

EGLint eplCountAttribs32(const EGLint *attribs);

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_UTILS_H
