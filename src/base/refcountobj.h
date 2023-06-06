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

#ifndef REFCOUNTOBJ_H
#define REFCOUNTOBJ_H

#include <EGL/egl.h>

/**
 * \file
 *
 * Helper functions for reference-counted structures.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A simple atomic refcount struct.
 *
 * This is intended to be embedded in a larger struct.
 */
typedef struct
{
    unsigned int refcount;
} EplRefCount;

/**
 * Initializes an \c EplRefCount struct.
 *
 * This will set the refcount to 1, and (if necessary) initialize a mutex.
 */
void eplRefCountInit(EplRefCount *obj);

/**
 * Increments the refcount for an \c EplRefCount. Does nothing if \p obj is
 * NULL.
 *
 * \param obj The object to reference, or NULL.
 * \return \p obj
 */
EplRefCount *eplRefCountRef(EplRefCount *obj);

/**
 * Decrements the refcount of an \c EplRefCount. Does nothing if \p obj is
 * NULL.
 *
 * If the reference count drops to zero, then this function will clean up the
 * \c EplRefCount struct and return non-zero.
 *
 * \param obj The object to unreference.
 * \return non-zero if the reference count is now zero, and so the object
 *      should be destroyed.
 */
int eplRefCountUnref(EplRefCount *obj);

/**
 * Declares functions to reference and unreference an EplRefCount-based struct.
 *
 * \param type The name of the larger struct.
 * \param prefix The prefix of the ref and unref functions. "Ref" and "Unref"
 *      will be appended to this.
 */
#define EPL_REFCOUNT_DECLARE_TYPE_FUNCS(type, prefix) \
    type *prefix##Ref(type *obj); \
    void prefix##Unref(type *obj);

/**
 * Defines functions to reference and unreference an EplRefCount-based struct.
 *
 * \param type The name of the larger struct.
 * \param prefix The prefix of the ref and unref functions. "Ref" and "Unref"
 *      will be appended to this.
 * \param member The name of the embedded EplRefCount struct.
 * \param freefunc A function to destroy an object when its refcount reaches
 *      zero. This function should take a single \p type pointer as a parameter.
 */
#define EPL_REFCOUNT_DEFINE_TYPE_FUNCS(type, prefix, member, freefunc) \
    type *prefix##Ref(type *obj) { \
        if (obj != NULL) eplRefCountRef(&obj->member); \
        return obj; \
    } \
    void prefix##Unref(type *obj) { \
        if (obj != NULL && eplRefCountUnref(&obj->member)) { \
            freefunc(obj); \
        } \
    }

#ifdef __cplusplus
}
#endif
#endif // REFCOUNTOBJ_H
