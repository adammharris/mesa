# Copyright 2026 The Mesa authors
# SPDX-License-Identifier: MIT
#
# Redox: the libdrm stand-in that talks to the GPU daemon's scheme, and test programs.

# libredox_drm is built in drm/ (src/meson.build enters it first: it is part of dep_libdrm).

if with_gallium_asahi
  # Shader -> AGX machine code, printed; compared with a Linux build's output.
  executable(
    'agxc_test',
    files('tests/agxc_test.c'),
    include_directories : [inc_include, inc_src, inc_asahi],
    c_args : ['-Wno-c2x-extensions'],
    link_with : [libasahi_compiler, libasahi_lib, libasahi_layout, libredox_drm],
    dependencies : [idep_nir, idep_mesautil, idep_agx_pack, dep_libdrm, idep_libagx],
    build_by_default : false,
  )

  # Mesa's own device layer (agx_device, BOs, sync objects) against a daemon.
  executable(
    'agxdev_test',
    files('tests/agxdev_test.c'),
    include_directories : [inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm,
                           include_directories('drm')],
    c_args : ['-Wno-c2x-extensions'],
    link_with : [libasahi_compiler, libasahi_lib, libasahi_layout, libredox_drm],
    dependencies : [idep_nir, idep_mesautil, idep_agx_pack, dep_libdrm, idep_libagx,
                    idep_libagx_shaders_h],
    build_by_default : false,
  )

  # The gallium driver without a frontend: clear + triangle on an offscreen target.
  executable(
    'agxtri_test',
    files('tests/agxtri_test.c'),
    include_directories : [
      inc_gallium_aux, inc_gallium, inc_include, inc_src, inc_asahi, inc_virtio_gpu,
      inc_virtio_vdrm, include_directories('drm', '../gallium/drivers/asahi'),
    ],
    c_args : ['-Wno-c2x-extensions'],
    link_with : [
      libasahi, libasahiwinsys, libasahi_compiler, libasahi_lib, libasahi_layout,
      libasahi_decode, libgallium, libgalliumvl_stub, libredox_drm,
    ],
    dependencies : [
      idep_nir, idep_mesautil, idep_agx_pack, dep_libdrm, idep_libagx,
      idep_libagx_shaders_h, idep_xmlconfig,
    ],
    build_by_default : false,
  )
endif

# The marshalling alone (no daemon): also builds on Linux by hand, see the file.
executable(
  'redox_drm_marshal_test',
  files('tests/redox_drm_marshal_test.c'),
  include_directories : [include_directories('drm'), inc_include, include_directories('../../include/drm-uapi')],
  link_with : [libredox_drm],
  build_by_default : false,
)
