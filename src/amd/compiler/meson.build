# Copyright © 2018 Valve Corporation
# SPDX-License-Identifier: MIT

aco_depends = files('aco_opcodes.py')

aco_opcodes_h = custom_target(
  'aco_opcodes.h',
  input : 'aco_opcodes_h.py',
  output : 'aco_opcodes.h',
  command : [prog_python, '@INPUT@'],
  capture : true,
  depend_files : aco_depends,
)

aco_opcodes_c = custom_target(
  'aco_opcodes.cpp',
  input : 'aco_opcodes_cpp.py',
  output : 'aco_opcodes.cpp',
  command : [prog_python, '@INPUT@'],
  capture : true,
  depend_files : aco_depends,
)

aco_builder_h = custom_target(
  'aco_builder.h',
  input : 'aco_builder_h.py',
  output : 'aco_builder.h',
  command : [prog_python, '@INPUT@'],
  capture : true,
  depend_files : aco_depends,
)

libaco_files = files(
  'instruction_selection/aco_instruction_selection.h',
  'instruction_selection/aco_isel_cfg.cpp',
  'instruction_selection/aco_isel_helpers.cpp',
  'instruction_selection/aco_isel_setup.cpp',
  'instruction_selection/aco_select_nir_alu.cpp',
  'instruction_selection/aco_select_nir_intrinsics.cpp',
  'instruction_selection/aco_select_nir.cpp',
  'instruction_selection/aco_select_ps_epilog.cpp',
  'instruction_selection/aco_select_ps_prolog.cpp',
  'instruction_selection/aco_select_rt_prolog.cpp',
  'instruction_selection/aco_select_trap_handler.cpp',
  'instruction_selection/aco_select_vs_prolog.cpp',
  'aco_dead_code_analysis.cpp',
  'aco_dominance.cpp',
  'aco_interface.cpp',
  'aco_interface.h',
  'aco_ir.cpp',
  'aco_ir.h',
  'aco_assembler.cpp',
  'aco_form_hard_clauses.cpp',
  'aco_insert_delay_alu.cpp',
  'aco_insert_exec_mask.cpp',
  'aco_insert_fp_mode.cpp',
  'aco_insert_NOPs.cpp',
  'aco_insert_waitcnt.cpp',
  'aco_reduce_assign.cpp',
  'aco_register_allocation.cpp',
  'aco_live_var_analysis.cpp',
  'aco_lower_branches.cpp',
  'aco_lower_phis.cpp',
  'aco_lower_subdword.cpp',
  'aco_lower_to_cssa.cpp',
  'aco_lower_to_hw_instr.cpp',
  'aco_optimizer.cpp',
  'aco_optimizer_postRA.cpp',
  'aco_opt_value_numbering.cpp',
  'aco_print_asm.cpp',
  'aco_print_ir.cpp',
  'aco_reindex_ssa.cpp',
  'aco_repair_ssa.cpp',
  'aco_scheduler.cpp',
  'aco_scheduler_ilp.cpp',
  'aco_spill.cpp',
  'aco_ssa_elimination.cpp',
  'aco_statistics.cpp',
  'aco_util.h',
  'aco_validate.cpp',
)

cpp_args_aco = cpp.get_supported_arguments(['-fno-exceptions', '-fno-rtti', '-Wimplicit-fallthrough', '-Wshadow'])

_libaco = static_library(
  'aco',
  [libaco_files, aco_opcodes_c, aco_opcodes_h, aco_builder_h],
  include_directories : [
   inc_include, inc_src, inc_amd, inc_amd_common,
  ],
  dependencies : [
    dep_llvm, dep_thread, dep_valgrind, idep_nir_headers, idep_amdgfxregs_h, idep_mesautil,
  ],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  cpp_args : [cpp_args_aco, cpp_msvc_compat_args],
)

# Also link with aco
idep_aco = declare_dependency(
  include_directories : include_directories('.'),
  link_with : _libaco,
  sources : [aco_opcodes_h, aco_builder_h],
)
