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

#include "refcountobj.h"

#include <stdlib.h>
#include <assert.h>

void eplRefCountInit(EplRefCount *obj)
{
    obj->refcount = 1;
}

EplRefCount *eplRefCountRef(EplRefCount *obj)
{
    if (obj != NULL)
    {
        __sync_add_and_fetch(&obj->refcount, 1);
    }
    return obj;
}

int eplRefCountUnref(EplRefCount *obj)
{
    if (obj != NULL)
    {
        unsigned int prev = __sync_fetch_and_sub(&obj->refcount, 1);
        assert(prev > 0);
        if (prev == 1)
        {
            // If we were using a mutex, then this is where we'd destroy it.
            return 1;
        }
    }
    return 0;
}

