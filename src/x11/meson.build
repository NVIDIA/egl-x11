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

dep_gbm = dependency('gbm')
dep_x11 = dependency('x11', required: get_option('xlib'))
dep_x11_xcb = dependency('x11-xcb', required: get_option('xlib'))
dep_xcb = dependency('xcb')
dep_xcb_present = dependency('xcb-present')
dep_xcb_dri3 = dependency('xcb-dri3')

enable_xlib = (get_option('xlib').allowed() and dep_x11.found() and dep_x11_xcb.found())

x11_deps = [
  dep_libdrm,
  dep_threads,
  dep_gbm,
  dep_xcb,
  dep_xcb_present,
  dep_xcb_dri3,
]

x11_common_source = [
  'x11-platform.c',
  'x11-window.c',
  'x11-pixmap.c',
]

if get_option('xcb')
  xcb_platform = shared_library('nvidia-egl-xcb',
    [
      x11_common_source,
      'x11-platform-xcb.c'
    ],
    include_directories: [ inc_base ],
    dependencies: [
      x11_deps,
      dep_eglexternal,
    ],
    link_with: [ platform_base ],
    version : '1',
    gnu_symbol_visibility: 'hidden',
    install: true)

  install_data('xcb-platform.json',
    install_dir: '@0@/egl/egl_external_platform.d'.format(get_option('datadir')))
endif

if enable_xlib
  xlib_platform = shared_library('nvidia-egl-xlib',
    [
      x11_common_source,
      'x11-platform-xlib.c',
    ],
    include_directories: [ inc_base ],
    dependencies: [
      x11_deps,
      dep_x11,
      dep_x11_xcb,
      dep_eglexternal,
    ],
    link_with: [ platform_base ],
    version : '1',
    gnu_symbol_visibility: 'hidden',
    install: true)

  install_data('xlib-platform.json',
    install_dir: '@0@/egl/egl_external_platform.d'.format(get_option('datadir')))
endif
