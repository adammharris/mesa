# Copyright 2026 The Mesa authors
# SPDX-License-Identifier: MIT
#
# libdrm for Redox: built before everything else under src/, then part of dep_libdrm.

libredox_drm = static_library(
  'redox_drm',
  files('redox_drm.c'),
  include_directories : [include_directories('.', '../../../include/drm-uapi'), inc_include],
  gnu_symbol_visibility : 'hidden',
  pic : true,
  build_by_default : false,
)

dep_libdrm = declare_dependency(
  dependencies : dep_libdrm,
  link_with : libredox_drm,
)
