# Copyright © 2017-2019 Intel Corporation
# SPDX-License-Identifier: MIT

inc_anv = include_directories('.')

anv_flags = [
  no_override_init_args,
  sse2_args,
]

anv_cpp_flags = []

anv_entrypoints = custom_target(
  'anv_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['anv_entrypoints.h', 'anv_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'anv',
    '--beta', with_vulkan_beta.to_string(),
    '--device-prefix', 'gfx9',
    '--device-prefix', 'gfx11',
    '--device-prefix', 'gfx12',
    '--device-prefix', 'gfx125',
    '--device-prefix', 'gfx20',
    '--device-prefix', 'gfx30',
    '--device-prefix', 'anv_doom64',
    '--device-prefix', 'anv_hitman3',
    '--device-prefix', 'anv_android',
    '--device-prefix', 'anv_rmv',
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

idep_anv_headers = declare_dependency(
  sources : [anv_entrypoints[0]],
  include_directories : inc_anv,
)

bvh_spv = []
if with_intel_vk_rt
  subdir('bvh')
  anv_flags += '-DANV_SUPPORT_RT=1'
else
  anv_flags += '-DANV_SUPPORT_RT=0'
endif

intel_icd = custom_target(
  'intel_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'intel_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir'),
                             'libvulkan_intel.so'),
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'intel_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'intel_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--lib-path', meson.current_build_dir() / 'libvulkan_intel.so',
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())
# Deprecated: replaced by VK_DRIVER_FILES above
devenv.append('VK_ICD_FILENAMES', _dev_icd.full_path())

libanv_per_hw_ver_libs = []
anv_per_hw_ver_files = files(
  'genX_blorp_exec.c',
  'genX_cmd_buffer.c',
  'genX_cmd_compute.c',
  'genX_cmd_draw.c',
  'genX_cmd_draw_generated_flush.h',
  'genX_cmd_draw_generated_indirect.h',
  'genX_cmd_video.c',
  'genX_cmd_video_enc.c',
  'genX_gfx_state.c',
  'genX_gpu_memcpy.c',
  'genX_init_state.c',
  'genX_internal_kernels.c',
  'genX_pipeline.c',
  'genX_query.c',
  'genX_simple_shader.c',
)
if with_intel_vk_rt
  anv_per_hw_ver_files += files(
    'genX_acceleration_structure.c',
  )
endif

foreach _gfx_ver : ['90', '110', '120', '125', '200', '300']
  libanv_per_hw_ver_libs += static_library(
    'anv_per_hw_ver@0@'.format(_gfx_ver),
    [anv_per_hw_ver_files, anv_entrypoints[0]] + bvh_spv,
    include_directories : [
      inc_include, inc_src, inc_intel,
    ],
    c_args : anv_flags + ['-DGFX_VERx10=@0@'.format(_gfx_ver)],
    gnu_symbol_visibility : 'hidden',
    dependencies : [
      dep_libdrm, dep_valgrind, idep_nir_headers, idep_genxml,
      idep_vulkan_util_headers, idep_vulkan_wsi_headers,
      idep_vulkan_runtime_headers, idep_mesautil,
      idep_intel_driver_ds_headers,
      idep_intel_shaders, idep_intel_blorp,
    ],
  )
endforeach

libanv_files = files(
  'i915/anv_batch_chain.c',
  'i915/anv_batch_chain.h',
  'i915/anv_device.c',
  'i915/anv_device.h',
  'i915/anv_gem.c',
  'i915/anv_gem.h',
  'i915/anv_kmd_backend.c',
  'i915/anv_queue.c',
  'i915/anv_queue.h',
  'layers/anv_android_layer.c',
  'layers/anv_doom64.c',
  'layers/anv_hitman3.c',
  'layers/anv_rmv_layer.c',
  'xe/anv_batch_chain.c',
  'xe/anv_batch_chain.h',
  'xe/anv_kmd_backend.c',
  'xe/anv_device.c',
  'xe/anv_device.h',
  'xe/anv_queue.c',
  'xe/anv_queue.h',
  'anv_allocator.c',
  'anv_android.h',
  'anv_astc_emu.c',
  'anv_batch_chain.c',
  'anv_blorp.c',
  'anv_bo_sync.c',
  'anv_buffer.c',
  'anv_buffer_view.c',
  'anv_cmd_buffer.c',
  'anv_descriptor_set.c',
  'anv_device.c',
  'anv_embedded_sampler.c',
  'anv_event.c',
  'anv_formats.c',
  'anv_genX.h',
  'anv_image.c',
  'anv_image_host_copy.c',
  'anv_image_view.c',
  'anv_internal_kernels.c',
  'anv_internal_kernels.h',
  'anv_instance.c',
  'anv_kmd_backend.c',
  'anv_kmd_backend.h',
  'anv_measure.c',
  'anv_measure.h',
  'anv_nir.h',
  'anv_nir_apply_pipeline_layout.c',
  'anv_nir_compute_push_layout.c',
  'anv_nir_lower_multiview.c',
  'anv_nir_lower_ubo_loads.c',
  'anv_nir_lower_resource_intel.c',
  'anv_nir_push_descriptor_analysis.c',
  'anv_perf.c',
  'anv_physical_device.c',
  'anv_pipeline.c',
  'anv_pipeline_cache.c',
  'anv_private.h',
  'anv_queue.c',
  'anv_rmv.c',
  'anv_rmv.h',
  'anv_sampler.c',
  'anv_slab_bo.c',
  'anv_slab_bo.h',
  'anv_sparse.c',
  'anv_util.c',
  'anv_utrace.c',
  'anv_va.c',
  'anv_video.c',
  'anv_wsi.c',
  'bvh/anv_bvh.h',
  'bvh/anv_build_interface.h',
)

anv_deps = [
  dep_libdrm,
  dep_valgrind,
  idep_genxml,
  idep_mesautil,
  idep_nir_headers,
  idep_vulkan_util_headers,
  idep_vulkan_runtime_headers,
  idep_vulkan_wsi_headers,
  idep_intel_shaders,
  idep_intel_blorp,
]

if with_platform_x11
  anv_deps += dep_xcb_dri3
endif

if with_platform_wayland
  anv_deps += dep_wayland_client
endif

if with_xlib_lease
  anv_deps += [dep_xlib_xrandr]
endif

if with_platform_android
  anv_deps += idep_u_gralloc
  libanv_files += files('anv_android.c')
else
  libanv_files += files('anv_android_stubs.c')
endif

anv_deps += idep_intel_driver_ds_headers

libanv_common = static_library(
  'anv_common',
  [
    libanv_files, anv_entrypoints, sha1_h,
    gen_xml_pack, intel_float64_spv_h,
  ],
  include_directories : [
    inc_include, inc_src, inc_intel,
    inc_util,
  ],
  c_args : anv_flags,
  cpp_args : anv_cpp_flags,
  gnu_symbol_visibility : 'hidden',
  dependencies : anv_deps
)

libvulkan_intel = shared_library(
  'vulkan_intel',
  [files('anv_gem.c'), anv_entrypoints[0], bvh_spv],
  include_directories : [
    inc_include, inc_src, inc_intel,
  ],
  link_whole : [libanv_common, libanv_per_hw_ver_libs],
  link_with : [
    libisl, libintel_perf,
  ],
  dependencies : [
    dep_thread, dep_dl, dep_m, anv_deps, idep_libintel_common,
    idep_nir, idep_genxml, idep_vulkan_util, idep_vulkan_wsi,
    idep_vulkan_runtime, idep_mesautil, idep_xmlconfig,
    idep_intel_driver_ds, idep_intel_dev, idep_intel_blorp,
    idep_intel_compiler_brw, idep_intel_decoder_brw,
  ],
  c_args : anv_flags,
  gnu_symbol_visibility : 'hidden',
  link_args : [vulkan_icd_link_args, ld_args_build_id, ld_args_bsymbolic, ld_args_gc_sections],
  link_depends : vulkan_icd_link_depends,
  install : true,
)

if with_symbols_check
  test(
    'anv symbols check',
    symbols_check,
    args : [
      '--lib', libvulkan_intel,
      '--symbols-file', vulkan_icd_symbols,
      symbols_check_args,
    ],
    suite : ['intel'],
  )
endif

if with_tests
  libvulkan_intel_test = static_library(
    'vulkan_intel_test',
    [files('anv_gem_stubs.c'), anv_entrypoints[0]],
    include_directories : [
      inc_include, inc_src, inc_intel,
    ],
    link_whole : libanv_common,
    link_with : [
      libanv_per_hw_ver_libs, libintel_common,
      libisl, libintel_perf,
    ],
    dependencies : [
      dep_thread, dep_dl, dep_m, anv_deps,
      idep_nir, idep_vulkan_util, idep_vulkan_wsi, idep_vulkan_runtime,
      idep_mesautil, idep_intel_dev, idep_intel_blorp,
      idep_intel_compiler_brw, idep_intel_decoder_brw,
    ],
    c_args : anv_flags,
    gnu_symbol_visibility : 'hidden',
  )

  files_anv_tests = files(
    'tests/anv_tests.cpp',

    'tests/state_pool.c',
    'tests/state_pool_free_list_only.c',
    'tests/state_pool_max_size.c',
    'tests/state_pool_no_free.c',
    'tests/state_pool_padding.c',
    'tests/block_pool_no_free.c',
    'tests/block_pool_grow_first.c',
    'tests/block_pool_max_size.c',
  )

  test(
    'anv_tests',
    executable(
      'anv_tests',
      [files_anv_tests, anv_entrypoints[0]],
      c_args : [ sse2_args ],
      link_with : libvulkan_intel_test,
      dependencies : [
        idep_gtest, dep_libdrm, dep_thread, dep_m, dep_valgrind,
        idep_vulkan_util, idep_vulkan_wsi_headers,
        idep_vulkan_runtime, idep_intel_driver_ds, idep_intel_dev,
        anv_deps,
      ],
      include_directories : [
        inc_include, inc_src, inc_intel, inc_compiler, inc_nir
      ],
    ),
    suite : ['intel'],
    protocol : 'gtest',
    is_parallel : false,
    timeout : 60,
  )
endif
