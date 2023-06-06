# SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

platform_base = static_library('platform-base',
  [
    'config-list.c',
    'platform-base.c',
    'platform-utils.c',
    'refcountobj.c',
  ],
  dependencies: [
    dep_libdrm.partial_dependency(compile_args : true, includes : true),
    dep_threads,
    dep_eglexternal,
  ],
  gnu_symbol_visibility: 'hidden',
  install: false)
