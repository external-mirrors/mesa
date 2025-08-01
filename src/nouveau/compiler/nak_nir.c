/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "nak_private.h"
#include "nir_builder.h"
#include "nir_control_flow.h"
#include "nir_xfb_info.h"

#include "util/u_math.h"

#define OPT(nir, pass, ...) ({                           \
   bool this_progress = false;                           \
   NIR_PASS(this_progress, nir, pass, ##__VA_ARGS__);    \
   if (this_progress)                                    \
      progress = true;                                   \
   this_progress;                                        \
})

#define OPT_V(nir, pass, ...) NIR_PASS(_, nir, pass, ##__VA_ARGS__)

nir_def *
nak_nir_load_sysval(nir_builder *b, enum nak_sv idx,
                    enum gl_access_qualifier access)
{
   bool divergent;

   switch (idx) {
   case NAK_SV_CTAID_X:
   case NAK_SV_CTAID_Y:
   case NAK_SV_CTAID_Z:
   case NAK_SV_VIRTCFG:
   case NAK_SV_PRIM_TYPE:
   case NAK_SV_CLOCK_LO:
   case NAK_SV_CLOCK_HI:
   case NAK_SV_VARIABLE_RATE:
      divergent = false;
      break;
   default:
      divergent = true;
      break;
   }

   return nir_load_sysval_nv(b, 32, .base = idx,
                             .access = access,
                             .divergent = divergent);
}

bool
nak_nir_workgroup_has_one_subgroup(const nir_shader *nir)
{
   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_TESS_EVAL:
   case MESA_SHADER_GEOMETRY:
   case MESA_SHADER_FRAGMENT:
      UNREACHABLE("Shader stage does not have workgroups");
      break;

   case MESA_SHADER_TESS_CTRL:
      /* Tessellation only ever has one subgroup per workgroup.  The Vulkan
       * limit on the number of tessellation invocations is 32 to allow for
       * this.
       */
      return true;

   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL: {
      if (nir->info.workgroup_size_variable)
         return false;

      uint16_t wg_sz = nir->info.workgroup_size[0] *
                       nir->info.workgroup_size[1] *
                       nir->info.workgroup_size[2];

      return wg_sz <= NAK_SUBGROUP_SIZE;
   }

   default:
      UNREACHABLE("Unknown shader stage");
   }
}

static uint8_t
vectorize_filter_cb(const nir_instr *instr, const void *_data)
{
   if (instr->type != nir_instr_type_alu)
      return 0;

   const nir_alu_instr *alu = nir_instr_as_alu(instr);

   const unsigned bit_size = nir_alu_instr_is_comparison(alu)
                             ? alu->src[0].src.ssa->bit_size
                             : alu->def.bit_size;

   switch (alu->op) {
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fabs:
   case nir_op_fneg:
   case nir_op_feq:
   case nir_op_fge:
   case nir_op_flt:
   case nir_op_fneu:
   case nir_op_fmul:
   case nir_op_ffma:
   case nir_op_fsign:
   case nir_op_fsat:
   case nir_op_fmax:
   case nir_op_fmin:
      return bit_size == 16 ? 2 : 1;
   default:
      return 1;
   }
}

static uint8_t
phi_vectorize_cb(const nir_instr *instr, const void *data)
{
   nir_phi_instr *phi = nir_instr_as_phi(instr);
   unsigned bit_size = phi->def.bit_size;

   if (bit_size == 16)
      return 2;
   if (bit_size == 8)
      return 4;
   return 1;
}

static void
optimize_nir(nir_shader *nir, const struct nak_compiler *nak, bool allow_copies)
{
   bool progress;

   unsigned lower_flrp =
      (nir->options->lower_flrp16 ? 16 : 0) |
      (nir->options->lower_flrp32 ? 32 : 0) |
      (nir->options->lower_flrp64 ? 64 : 0);

   do {
      progress = false;

      /* This pass is causing problems with types used by OpenCL :
       *    https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/13955
       *
       * Running with it disabled made no difference in the resulting assembly
       * code.
       */
      if (nir->info.stage != MESA_SHADER_KERNEL)
         OPT(nir, nir_split_array_vars, nir_var_function_temp);

      OPT(nir, nir_shrink_vec_array_vars, nir_var_function_temp);
      OPT(nir, nir_opt_deref);
      if (OPT(nir, nir_opt_memcpy))
         OPT(nir, nir_split_var_copies);

      OPT(nir, nir_lower_vars_to_ssa);

      if (allow_copies) {
         /* Only run this pass in the first call to brw_nir_optimize.  Later
          * calls assume that we've lowered away any copy_deref instructions
          * and we don't want to introduce any more.
          */
         OPT(nir, nir_opt_find_array_copies);
      }
      OPT(nir, nir_opt_copy_prop_vars);
      OPT(nir, nir_opt_dead_write_vars);
      OPT(nir, nir_opt_combine_stores, nir_var_all);

      OPT(nir, nir_lower_alu_width, vectorize_filter_cb, NULL);
      OPT(nir, nir_opt_vectorize, vectorize_filter_cb, NULL);
      OPT(nir, nir_lower_phis_to_scalar, phi_vectorize_cb, NULL);
      OPT(nir, nir_lower_frexp);
      OPT(nir, nir_copy_prop);
      OPT(nir, nir_opt_dce);
      OPT(nir, nir_opt_cse);

      nir_opt_peephole_select_options peephole_select_options = {
         .limit = 0,
         .discard_ok = true,
      };
      OPT(nir, nir_opt_peephole_select, &peephole_select_options);
      OPT(nir, nir_opt_intrinsics);
      OPT(nir, nir_opt_idiv_const, 32);
      OPT(nir, nir_opt_algebraic);
      OPT(nir, nir_lower_constant_convert_alu_types);
      OPT(nir, nir_opt_constant_folding);

      if (lower_flrp != 0) {
         if (OPT(nir, nir_lower_flrp, lower_flrp, false /* always_precise */))
            OPT(nir, nir_opt_constant_folding);
         /* Nothing should rematerialize any flrps */
         lower_flrp = 0;
      }

      OPT(nir, nir_opt_dead_cf);
      if (OPT(nir, nir_opt_loop)) {
         /* If nir_opt_loop makes progress, then we need to clean things up
          * if we want any hope of nir_opt_if or nir_opt_loop_unroll to make
          * progress.
          */
         OPT(nir, nir_copy_prop);
         OPT(nir, nir_opt_dce);
      }
      OPT(nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
      OPT(nir, nir_opt_phi_to_bool);
      if (nir->options->max_unroll_iterations != 0) {
         OPT(nir, nir_opt_loop_unroll);
      }
      OPT(nir, nir_opt_remove_phis);
      OPT(nir, nir_opt_gcm, false);
      OPT(nir, nir_opt_undef);
   } while (progress);
   OPT(nir, nir_lower_undef_to_zero);

   OPT(nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
}

void
nak_optimize_nir(nir_shader *nir, const struct nak_compiler *nak)
{
   optimize_nir(nir, nak, false);
}

static unsigned
lower_bit_size_cb(const nir_instr *instr, void *data)
{
   const struct nak_compiler *nak = data;

   switch (instr->type) {
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      if (nir_op_infos[alu->op].is_conversion)
         return 0;

      const unsigned bit_size = nir_alu_instr_is_comparison(alu)
                                ? alu->src[0].src.ssa->bit_size
                                : alu->def.bit_size;

      switch (alu->op) {
      case nir_op_bit_count:
      case nir_op_ufind_msb:
      case nir_op_ifind_msb:
      case nir_op_find_lsb:
         /* These are handled specially because the destination is always
          * 32-bit and so the bit size of the instruction is given by the
          * source.
          */
         return alu->src[0].src.ssa->bit_size == 32 ? 0 : 32;

      case nir_op_fabs:
      case nir_op_fadd:
      case nir_op_fneg:
      case nir_op_feq:
      case nir_op_fge:
      case nir_op_flt:
      case nir_op_fneu:
      case nir_op_fmul:
      case nir_op_ffma:
      case nir_op_ffmaz:
      case nir_op_fsign:
      case nir_op_fsat:
      case nir_op_fceil:
      case nir_op_ffloor:
      case nir_op_fround_even:
      case nir_op_ftrunc:
         if (bit_size == 16  && nak->sm >= 70)
            return 0;
         break;

      case nir_op_fmax:
      case nir_op_fmin:
         if (bit_size == 16 && nak->sm >= 80)
            return 0;
         break;

      default:
         break;
      }

      if (bit_size >= 32)
         return 0;

      if (bit_size & (8 | 16))
         return 32;

      return 0;
   }

   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_vote_ieq:
         if (intrin->src[0].ssa->bit_size != 1 &&
             intrin->src[0].ssa->bit_size < 32)
            return 32;
         return 0;

      case nir_intrinsic_vote_feq:
      case nir_intrinsic_read_invocation:
      case nir_intrinsic_read_first_invocation:
      case nir_intrinsic_shuffle:
      case nir_intrinsic_shuffle_xor:
      case nir_intrinsic_shuffle_up:
      case nir_intrinsic_shuffle_down:
      case nir_intrinsic_quad_broadcast:
      case nir_intrinsic_quad_swap_horizontal:
      case nir_intrinsic_quad_swap_vertical:
      case nir_intrinsic_quad_swap_diagonal:
      case nir_intrinsic_reduce:
      case nir_intrinsic_inclusive_scan:
      case nir_intrinsic_exclusive_scan:
         if (intrin->src[0].ssa->bit_size < 32)
            return 32;
         return 0;

      default:
         return 0;
      }
   }

   default:
      return 0;
   }
}

void
nak_preprocess_nir(nir_shader *nir, const struct nak_compiler *nak)
{
   UNUSED bool progress = false;

   nir_validate_ssa_dominance(nir, "before nak_preprocess_nir");

   OPT(nir, nir_lower_io_vars_to_temporaries,
       nir_shader_get_entrypoint(nir),
       true /* outputs */, false /* inputs */);

   const nir_lower_tex_options tex_options = {
      .lower_txd_3d = true,
      .lower_txd_cube_map = true,
      .lower_txd_clamp = true,
      .lower_txd_shadow = true,
      .lower_txp = ~0,
      .lower_invalid_implicit_lod = true,
   };
   OPT(nir, nir_lower_tex, &tex_options);
   OPT(nir, nir_normalize_cubemap_coords);

   nir_lower_image_options image_options = {
      .lower_cube_size = true,
   };
   OPT(nir, nir_lower_image, &image_options);

   OPT(nir, nir_lower_global_vars_to_local);

   OPT(nir, nak_nir_lower_cmat, nak);

   OPT(nir, nir_split_var_copies);
   OPT(nir, nir_split_struct_vars, nir_var_function_temp);

   /* Optimize but allow copies because we haven't lowered them yet */
   optimize_nir(nir, nak, true /* allow_copies */);

   OPT(nir, nir_opt_barrier_modes);
   OPT(nir, nir_opt_acquire_release_barriers, SCOPE_QUEUE_FAMILY);

   OPT(nir, nir_lower_load_const_to_scalar);
   OPT(nir, nir_lower_var_copies);
   OPT(nir, nir_lower_system_values);
   OPT(nir, nir_lower_compute_system_values, NULL);

   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      OPT(nir, nir_lower_terminate_to_demote);
}

uint16_t
nak_varying_attr_addr(const struct nak_compiler *nak, gl_varying_slot slot)
{
   if (slot >= VARYING_SLOT_PATCH0) {
      return NAK_ATTR_PATCH_START + (slot - VARYING_SLOT_PATCH0) * 0x10;
   } else if (slot >= VARYING_SLOT_VAR0) {
      return NAK_ATTR_GENERIC_START + (slot - VARYING_SLOT_VAR0) * 0x10;
   } else {
      switch (slot) {
      case VARYING_SLOT_TESS_LEVEL_OUTER: return NAK_ATTR_TESS_LOD;
      case VARYING_SLOT_TESS_LEVEL_INNER: return NAK_ATTR_TESS_INTERRIOR;
      case VARYING_SLOT_PRIMITIVE_ID:     return NAK_ATTR_PRIMITIVE_ID;
      case VARYING_SLOT_LAYER:            return NAK_ATTR_RT_ARRAY_INDEX;
      case VARYING_SLOT_VIEWPORT:         return NAK_ATTR_VIEWPORT_INDEX;
      case VARYING_SLOT_PSIZ:             return NAK_ATTR_POINT_SIZE;
      case VARYING_SLOT_POS:              return NAK_ATTR_POSITION;
      case VARYING_SLOT_CLIP_DIST0:       return NAK_ATTR_CLIP_CULL_DIST_0;
      case VARYING_SLOT_CLIP_DIST1:       return NAK_ATTR_CLIP_CULL_DIST_4;
      case VARYING_SLOT_PRIMITIVE_SHADING_RATE:
         return nak->sm >= 86 ? NAK_ATTR_VPRS_TABLE_INDEX
                              : NAK_ATTR_VIEWPORT_INDEX;
      default: UNREACHABLE("Invalid varying slot");
      }
   }
}

static uint16_t
nak_fs_out_addr(gl_frag_result slot, uint32_t blend_idx)
{
   switch (slot) {
   case FRAG_RESULT_DEPTH:
      assert(blend_idx == 0);
      return NAK_FS_OUT_DEPTH;

   case FRAG_RESULT_STENCIL:
      UNREACHABLE("EXT_shader_stencil_export not supported");

   case FRAG_RESULT_COLOR:
      UNREACHABLE("Vulkan alway uses explicit locations");

   case FRAG_RESULT_SAMPLE_MASK:
      assert(blend_idx == 0);
      return NAK_FS_OUT_SAMPLE_MASK;

   default:
      assert(blend_idx < 2);
      return NAK_FS_OUT_COLOR((slot - FRAG_RESULT_DATA0) + blend_idx);
   }
}

uint16_t
nak_sysval_attr_addr(const struct nak_compiler *nak, gl_system_value sysval)
{
   switch (sysval) {
   case SYSTEM_VALUE_PRIMITIVE_ID:  return NAK_ATTR_PRIMITIVE_ID;
   case SYSTEM_VALUE_FRAG_COORD:    return NAK_ATTR_POSITION;
   case SYSTEM_VALUE_POINT_COORD:   return NAK_ATTR_POINT_SPRITE;
   case SYSTEM_VALUE_TESS_COORD:    return NAK_ATTR_TESS_COORD;
   case SYSTEM_VALUE_INSTANCE_ID:   return NAK_ATTR_INSTANCE_ID;
   case SYSTEM_VALUE_VERTEX_ID:     return NAK_ATTR_VERTEX_ID;
   case SYSTEM_VALUE_FRONT_FACE:    return NAK_ATTR_FRONT_FACE;
   case SYSTEM_VALUE_LAYER_ID:      return NAK_ATTR_RT_ARRAY_INDEX;
   default: UNREACHABLE("Invalid system value");
   }
}

static uint8_t
nak_sysval_sysval_idx(gl_system_value sysval)
{
   switch (sysval) {
   case SYSTEM_VALUE_SUBGROUP_INVOCATION:    return NAK_SV_LANE_ID;
   case SYSTEM_VALUE_VERTICES_IN:            return NAK_SV_PRIM_TYPE;
   case SYSTEM_VALUE_INVOCATION_ID:          return NAK_SV_INVOCATION_ID;
   case SYSTEM_VALUE_HELPER_INVOCATION:      return NAK_SV_THREAD_KILL;
   case SYSTEM_VALUE_LOCAL_INVOCATION_ID:    return NAK_SV_TID;
   case SYSTEM_VALUE_WORKGROUP_ID:           return NAK_SV_CTAID;
   case SYSTEM_VALUE_SUBGROUP_EQ_MASK:       return NAK_SV_LANEMASK_EQ;
   case SYSTEM_VALUE_SUBGROUP_LT_MASK:       return NAK_SV_LANEMASK_LT;
   case SYSTEM_VALUE_SUBGROUP_LE_MASK:       return NAK_SV_LANEMASK_LE;
   case SYSTEM_VALUE_SUBGROUP_GT_MASK:       return NAK_SV_LANEMASK_GT;
   case SYSTEM_VALUE_SUBGROUP_GE_MASK:       return NAK_SV_LANEMASK_GE;
   default: UNREACHABLE("Invalid system value");
   }
}

static bool
nak_nir_lower_system_value_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                                  void *data)
{
   const struct nak_compiler *nak = data;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *val;
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_primitive_id:
   case nir_intrinsic_load_instance_id:
   case nir_intrinsic_load_vertex_id: {
      assert(b->shader->info.stage != MESA_SHADER_VERTEX ||
             b->shader->info.stage != MESA_SHADER_TESS_CTRL ||
             b->shader->info.stage == MESA_SHADER_TESS_EVAL ||
             b->shader->info.stage == MESA_SHADER_GEOMETRY);
      const gl_system_value sysval =
         nir_system_value_from_intrinsic(intrin->intrinsic);
      const uint32_t addr = nak_sysval_attr_addr(nak, sysval);
      val = nir_ald_nv(b, 1, nir_imm_int(b, 0), nir_imm_int(b, 0),
                       .base = addr, .flags = 0,
                       .range_base = addr, .range = 4,
                       .access = ACCESS_CAN_REORDER);
      break;
   }

   case nir_intrinsic_load_patch_vertices_in: {
      val = nak_nir_load_sysval(b, NAK_SV_PRIM_TYPE, ACCESS_CAN_REORDER);
      val = nir_extract_u8(b, val, nir_imm_int(b, 1));
      break;
   }

   case nir_intrinsic_load_frag_shading_rate: {
      val = nak_nir_load_sysval(b, NAK_SV_VARIABLE_RATE, ACCESS_CAN_REORDER);

      /* X is in bits 8..16 and Y is in bits 16..24.  However, we actually
       * want the log2 of X and Y and, since we only support 1, 2, and 4, a
       * right shift by 1 is log2.  So this gives us
       *
       * x_log2 = (sv >> 9) & 3
       * y_log2 = (sv >> 17) & 3
       *
       * However, we actually want y_log2 at 0..2 and x_log2 at 2..4 so that
       * gives us
       */
      nir_def *x = nir_iand_imm(b, nir_ushr_imm(b, val, 7), 0xc);
      nir_def *y = nir_iand_imm(b, nir_ushr_imm(b, val, 17), 0x3);
      val = nir_ior(b, x, y);
      break;
   }

   case nir_intrinsic_load_subgroup_eq_mask:
   case nir_intrinsic_load_subgroup_lt_mask:
   case nir_intrinsic_load_subgroup_le_mask:
   case nir_intrinsic_load_subgroup_gt_mask:
   case nir_intrinsic_load_subgroup_ge_mask: {
      const gl_system_value sysval =
         nir_system_value_from_intrinsic(intrin->intrinsic);
      const uint32_t idx = nak_sysval_sysval_idx(sysval);
      val = nak_nir_load_sysval(b, idx, ACCESS_CAN_REORDER);

      /* Pad with 0 because all invocations above 31 are off */
      if (intrin->def.bit_size == 64) {
         val = nir_u2u32(b, val);
      } else {
         assert(intrin->def.bit_size == 32);
         val = nir_pad_vector_imm_int(b, val, 0, intrin->def.num_components);
      }
      break;
   }

   case nir_intrinsic_load_subgroup_invocation:
   case nir_intrinsic_load_helper_invocation:
   case nir_intrinsic_load_invocation_id:
   case nir_intrinsic_load_workgroup_id: {
      const gl_system_value sysval =
         nir_system_value_from_intrinsic(intrin->intrinsic);
      const uint32_t idx = nak_sysval_sysval_idx(sysval);
      nir_def *comps[3];
      assert(intrin->def.num_components <= 3);
      for (unsigned c = 0; c < intrin->def.num_components; c++) {
         comps[c] = nak_nir_load_sysval(b, idx + c, ACCESS_CAN_REORDER);
      }
      val = nir_vec(b, comps, intrin->def.num_components);
      break;
   }

   case nir_intrinsic_load_local_invocation_id: {
      nir_def *x = nak_nir_load_sysval(b, NAK_SV_TID_X,
                                       ACCESS_CAN_REORDER);
      nir_def *y = nak_nir_load_sysval(b, NAK_SV_TID_Y,
                                       ACCESS_CAN_REORDER);
      nir_def *z = nak_nir_load_sysval(b, NAK_SV_TID_Z,
                                       ACCESS_CAN_REORDER);
      if (b->shader->info.derivative_group == DERIVATIVE_GROUP_QUADS) {
         nir_def *x_lo = nir_iand_imm(b, x, 0x1);
         nir_def *y_lo = nir_ushr_imm(b, nir_iand_imm(b, x, 0x2), 1);
         nir_def *x_hi = nir_ushr_imm(b, nir_iand_imm(b, x, ~0x3), 1);
         nir_def *y_hi = nir_ishl_imm(b, y, 1);

         x = nir_ior(b, x_lo, x_hi);
         y = nir_ior(b, y_lo, y_hi);
      }

      val = nir_vec3(b, x, y, z);
      break;
   }

   case nir_intrinsic_load_num_subgroups: {
      assert(!b->shader->info.workgroup_size_variable);
      uint16_t wg_size = b->shader->info.workgroup_size[0] *
                         b->shader->info.workgroup_size[1] *
                         b->shader->info.workgroup_size[2];
      val = nir_imm_int(b, DIV_ROUND_UP(wg_size, 32));
      break;
   }

   case nir_intrinsic_load_subgroup_id:
      if (nak_nir_workgroup_has_one_subgroup(b->shader)) {
         val = nir_imm_int(b, 0);
      } else {
         assert(!b->shader->info.workgroup_size_variable);

         nir_def *tid_x = b->shader->info.workgroup_size[0] > 1
            ? nak_nir_load_sysval(b, NAK_SV_TID_X, ACCESS_CAN_REORDER)
            : nir_imm_int(b, 0);
         nir_def *tid_y = b->shader->info.workgroup_size[1] > 1
            ? nak_nir_load_sysval(b, NAK_SV_TID_Y, ACCESS_CAN_REORDER)
            : nir_imm_int(b, 0);
         nir_def *tid_z = b->shader->info.workgroup_size[2] > 1
            ? nak_nir_load_sysval(b, NAK_SV_TID_Z, ACCESS_CAN_REORDER)
            : nir_imm_int(b, 0);

         const uint16_t *wg_size = b->shader->info.workgroup_size;
         nir_def *tid =
            nir_iadd(b, tid_x,
            nir_iadd(b, nir_imul_imm(b, tid_y, wg_size[0]),
                        nir_imul_imm(b, tid_z, wg_size[0] * wg_size[1])));

         val = nir_udiv_imm(b, tid, 32);
      }
      break;

   case nir_intrinsic_is_helper_invocation: {
      /* Unlike load_helper_invocation, this one isn't re-orderable */
      val = nak_nir_load_sysval(b, NAK_SV_THREAD_KILL, 0);
      break;
   }

   case nir_intrinsic_shader_clock: {
      /* The CS2R opcode can load 64 bits worth of sysval data at a time but
       * it's not actually atomic.  In order to get correct shader clocks, we
       * need to do a loop where we do
       *
       *    CS2R SV_CLOCK_HI
       *    CS2R SV_CLOCK_LO
       *    CS2R SV_CLOCK_HI
       *    CS2R SV_CLOCK_LO
       *    CS2R SV_CLOCK_HI
       *    ...
       *
       * The moment two high values are the same, we take the low value
       * between them and that gives us our clock.
       *
       * In order to make sure we don't run into any weird races, we also need
       * to insert a barrier after every load to ensure the one load completes
       * before we kick off the next load.  Otherwise, if one load happens to
       * be faster than the other (they are variable latency, after all) we're
       * still guaranteed that the loads happen in the order we want.
       */
      nir_variable *clock =
         nir_local_variable_create(b->impl, glsl_uvec2_type(), NULL);

      nir_def *clock_hi = nak_nir_load_sysval(b, NAK_SV_CLOCK_HI, 0);
      nir_ssa_bar_nv(b, clock_hi);

      nir_store_var(b, clock, nir_vec2(b, nir_imm_int(b, 0), clock_hi), 0x3);

      nir_push_loop(b);
      {
         nir_def *last_clock = nir_load_var(b, clock);

         nir_def *clock_lo = nak_nir_load_sysval(b, NAK_SV_CLOCK_LO, 0);
         nir_ssa_bar_nv(b, clock_lo);

         clock_hi = nak_nir_load_sysval(b, NAK_SV_CLOCK + 1, 0);
         nir_ssa_bar_nv(b, clock_hi);

         nir_store_var(b, clock, nir_vec2(b, clock_lo, clock_hi), 0x3);

         nir_break_if(b, nir_ieq(b, clock_hi, nir_channel(b, last_clock, 1)));
      }
      nir_pop_loop(b, NULL);

      val = nir_load_var(b, clock);
      if (intrin->def.bit_size == 64)
         val = nir_pack_64_2x32(b, val);
      break;
   }

   case nir_intrinsic_load_warps_per_sm_nv:
      val = nir_imm_int(b, nak->warps_per_sm);
      break;

   case nir_intrinsic_load_sm_count_nv:
      val = nak_nir_load_sysval(b, NAK_SV_VIRTCFG, 0);
      val = nir_ubitfield_extract_imm(b, val, 20, 9);
      break;

   case nir_intrinsic_load_warp_id_nv:
      val = nak_nir_load_sysval(b, NAK_SV_VIRTID, 0);
      val = nir_ubitfield_extract_imm(b, val, 8, 7);
      break;

   case nir_intrinsic_load_sm_id_nv:
      val = nak_nir_load_sysval(b, NAK_SV_VIRTID, 0);
      val = nir_ubitfield_extract_imm(b, val, 20, 9);
      break;

   default:
      return false;
   }

   if (intrin->def.bit_size == 1)
      val = nir_i2b(b, val);

   nir_def_rewrite_uses(&intrin->def, val);

   return true;
}

static bool
nak_nir_lower_system_values(nir_shader *nir, const struct nak_compiler *nak)
{
   return nir_shader_intrinsics_pass(nir, nak_nir_lower_system_value_intrin,
                                     nir_metadata_none,
                                     (void *)nak);
}

struct nak_xfb_info
nak_xfb_from_nir(const struct nak_compiler *nak,
                 const struct nir_xfb_info *nir_xfb)
{
   if (nir_xfb == NULL)
      return (struct nak_xfb_info) { };

   struct nak_xfb_info nak_xfb = { };

   u_foreach_bit(b, nir_xfb->buffers_written) {
      nak_xfb.stride[b] = nir_xfb->buffers[b].stride;
      nak_xfb.stream[b] = nir_xfb->buffer_to_stream[b];
   }
   memset(nak_xfb.attr_index, 0xff, sizeof(nak_xfb.attr_index)); /* = skip */

   for (unsigned o = 0; o < nir_xfb->output_count; o++) {
      const nir_xfb_output_info *out = &nir_xfb->outputs[o];
      const uint8_t b = out->buffer;
      assert(nir_xfb->buffers_written & BITFIELD_BIT(b));

      const uint16_t attr_addr = nak_varying_attr_addr(nak, out->location);
      assert(attr_addr % 4 == 0);
      const uint16_t attr_idx = attr_addr / 4;

      assert(out->offset % 4 == 0);
      uint8_t out_idx = out->offset / 4;

      u_foreach_bit(c, out->component_mask)
         nak_xfb.attr_index[b][out_idx++] = attr_idx + c;

      nak_xfb.attr_count[b] = MAX2(nak_xfb.attr_count[b], out_idx);
   }

   return nak_xfb;
}

static bool
lower_fs_output_intrin(nir_builder *b, nir_intrinsic_instr *intrin, void *_data)
{
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   const nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);
   uint16_t addr = nak_fs_out_addr(sem.location, sem.dual_source_blend_index) +
                   nir_src_as_uint(intrin->src[1]) * 16 +
                   nir_intrinsic_component(intrin) * 4;

   nir_def *data = intrin->src[0].ssa;

   /* The fs_out_nv intrinsic is always scalar */
   u_foreach_bit(c, nir_intrinsic_write_mask(intrin)) {
      if (nir_scalar_is_undef(nir_scalar_resolved(data, c)))
         continue;

      nir_fs_out_nv(b, nir_channel(b, data, c), .base = addr + c * 4);
   }

   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
nak_nir_lower_fs_outputs(nir_shader *nir)
{
   if (nir->info.outputs_written == 0)
      return false;

   bool progress = nir_shader_intrinsics_pass(nir, lower_fs_output_intrin,
                                              nir_metadata_control_flow,
                                              NULL);

   if (progress) {
      /* We need a copy_fs_outputs_nv intrinsic so NAK knows where to place
       * the final copy.  This needs to be in the last block, after all
       * store_output intrinsics.
       */
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      nir_builder b = nir_builder_at(nir_after_impl(impl));
      nir_copy_fs_outputs_nv(&b);
   }

   return progress;
}

static bool
nak_nir_remove_barrier_intrin(nir_builder *b, nir_intrinsic_instr *barrier,
                              UNUSED void *_data)
{
   if (barrier->intrinsic != nir_intrinsic_barrier)
      return false;

   mesa_scope exec_scope = nir_intrinsic_execution_scope(barrier);
   assert(exec_scope <= SCOPE_WORKGROUP &&
          "Control barrier with scope > WORKGROUP");

   if (exec_scope == SCOPE_WORKGROUP &&
       nak_nir_workgroup_has_one_subgroup(b->shader))
      exec_scope = SCOPE_SUBGROUP;

   /* Because we're guaranteeing maximal convergence via warp barriers,
    * subgroup barriers do nothing.
    */
   if (exec_scope <= SCOPE_SUBGROUP)
      exec_scope = SCOPE_NONE;

   const nir_variable_mode mem_modes = nir_intrinsic_memory_modes(barrier);
   if (exec_scope == SCOPE_NONE && mem_modes == 0) {
      nir_instr_remove(&barrier->instr);
      return true;
   }

   /* In this case, we're leaving the barrier there */
   b->shader->info.uses_control_barrier = true;

   bool progress = false;
   if (exec_scope != nir_intrinsic_execution_scope(barrier)) {
      nir_intrinsic_set_execution_scope(barrier, exec_scope);
      progress = true;
   }

   return progress;
}

static bool
nak_nir_remove_barriers(nir_shader *nir)
{
   /* We'll set this back to true if we leave any barriers in place */
   nir->info.uses_control_barrier = false;

   return nir_shader_intrinsics_pass(nir, nak_nir_remove_barrier_intrin,
                                     nir_metadata_control_flow | nir_metadata_divergence,
                                     NULL);
}

static bool
nak_mem_vectorize_cb(unsigned align_mul, unsigned align_offset,
                     unsigned bit_size, unsigned num_components,
                     int64_t hole_size, nir_intrinsic_instr *low,
                     nir_intrinsic_instr *high, void *cb_data)
{
   /*
    * Since we legalize these later with nir_lower_mem_access_bit_sizes,
    * we can optimistically combine anything that might be profitable
    */
   assert(util_is_power_of_two_nonzero(align_mul));

   if (hole_size > 0)
      return false;

   unsigned max_bytes = 128u / 8u;
   if (low->intrinsic == nir_intrinsic_ldc_nv ||
       low->intrinsic == nir_intrinsic_ldcx_nv)
      max_bytes = 64u / 8u;

   align_mul = MIN2(align_mul, max_bytes);
   align_offset = align_offset % align_mul;
   return align_offset + num_components * (bit_size / 8) <= align_mul;
}

static nir_mem_access_size_align
nak_mem_access_size_align(nir_intrinsic_op intrin,
                          uint8_t bytes, uint8_t bit_size,
                          uint32_t align_mul, uint32_t align_offset,
                          bool offset_is_const, enum gl_access_qualifier access,
                          const void *cb_data)
{
   const uint32_t align = nir_combined_align(align_mul, align_offset);
   assert(util_is_power_of_two_nonzero(align));

   unsigned bytes_pow2;
   if (nir_intrinsic_infos[intrin].has_dest) {
      /* Reads can over-fetch a bit if the alignment is okay. */
      bytes_pow2 = util_next_power_of_two(bytes);
   } else {
      bytes_pow2 = 1 << (util_last_bit(bytes) - 1);
   }

   unsigned chunk_bytes = MIN3(bytes_pow2, align, 16);
   assert(util_is_power_of_two_nonzero(chunk_bytes));
   if (intrin == nir_intrinsic_ldc_nv ||
       intrin == nir_intrinsic_ldcx_nv)
      chunk_bytes = MIN2(chunk_bytes, 8);

   if ((intrin == nir_intrinsic_ldc_nv ||
        intrin == nir_intrinsic_ldcx_nv) && align < 4) {
      /* CBufs require 4B alignment unless we're doing a ldc.u8 or ldc.i8.
       * In particular, this applies to ldc.u16 which means we either have to
       * fall back to two ldc.u8 or use ldc.u32 and shift stuff around to get
       * the 16bit value out.  Fortunately, nir_lower_mem_access_bit_sizes()
       * can handle over-alignment for reads.
       */
      if (align == 2 || offset_is_const) {
         return (nir_mem_access_size_align) {
            .bit_size = 32,
            .num_components = 1,
            .align = 4,
            .shift = nir_mem_access_shift_method_scalar,
         };
      } else {
         assert(align == 1);
         return (nir_mem_access_size_align) {
            .bit_size = 8,
            .num_components = 1,
            .align = 1,
            .shift = nir_mem_access_shift_method_scalar,
         };
      }
   } else if (chunk_bytes < 4) {
      return (nir_mem_access_size_align) {
         .bit_size = chunk_bytes * 8,
         .num_components = 1,
         .align = chunk_bytes,
         .shift = nir_mem_access_shift_method_scalar,
      };
   } else {
      return (nir_mem_access_size_align) {
         .bit_size = 32,
         .num_components = chunk_bytes / 4,
         .align = chunk_bytes,
         .shift = nir_mem_access_shift_method_scalar,
      };
   }
}

static bool
nir_shader_has_local_variables(const nir_shader *nir)
{
   nir_foreach_function(func, nir) {
      if (func->impl && !exec_list_is_empty(&func->impl->locals))
         return true;
   }

   return false;
}

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_vec4_slots(type, false, bindless);
}

static bool
atomic_supported(const nir_instr *instr, const void *data)
{
   /* Shared atomics don't support 64-bit arithmetic */
   const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   return !(intr->intrinsic == nir_intrinsic_shared_atomic &&
            intr->def.bit_size == 64);
}

static unsigned
split_conversions_cb(const nir_instr *instr, void *data)
{
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   unsigned src_bit_size = nir_src_bit_size(alu->src[0].src);
   unsigned dst_bit_size = alu->def.bit_size;

   /* We can't cross the 64-bit boundary in one conversion */
   if ((src_bit_size <= 32 && dst_bit_size <= 32) ||
       (src_bit_size >= 32 && dst_bit_size >= 32))
      return 0;

   return 32;
}

void
nak_postprocess_nir(nir_shader *nir,
                    const struct nak_compiler *nak,
                    nir_variable_mode robust2_modes,
                    const struct nak_fs_key *fs_key)
{
   UNUSED bool progress = false;

   nak_optimize_nir(nir, nak);

   const nir_lower_subgroups_options subgroups_options = {
      .subgroup_size = NAK_SUBGROUP_SIZE,
      .ballot_bit_size = 32,
      .ballot_components = 1,
      .lower_to_scalar = true,
      .lower_vote_feq = true,
      .lower_vote_ieq = nak->sm < 70,
      .lower_first_invocation_to_ballot = true,
      .lower_read_first_invocation = true,
      .lower_elect = true,
      .lower_quad_vote = true,
      .lower_inverse_ballot = true,
      .lower_rotate_to_shuffle = true
   };
   OPT(nir, nir_opt_uniform_subgroup, &subgroups_options);
   OPT(nir, nir_lower_subgroups, &subgroups_options);
   if (nak->sm >= 50) {
      // On Maxwell+ we need to lower shared 64-bit atomics into
      // compare-and-swap loops
      OPT(nir, nir_lower_atomics, atomic_supported);
   } else {
      // On Kepler we need to lower shared atomics into locked ld-st
      OPT(nir, nak_nir_lower_kepler_shared_atomics);
   }

   if (nir_shader_has_local_variables(nir)) {
      OPT(nir, nir_lower_vars_to_explicit_types, nir_var_function_temp,
          glsl_get_natural_size_align_bytes);
      OPT(nir, nir_lower_explicit_io, nir_var_function_temp,
          nir_address_format_32bit_offset);
      nak_optimize_nir(nir, nak);
   }

   OPT(nir, nir_opt_shrink_vectors, true);

   nir_load_store_vectorize_options vectorize_opts = {};
   vectorize_opts.modes = nir_var_mem_global |
                          nir_var_mem_ssbo |
                          nir_var_mem_shared |
                          nir_var_shader_temp;
   vectorize_opts.callback = nak_mem_vectorize_cb;
   vectorize_opts.robust_modes = robust2_modes;
   OPT(nir, nir_opt_load_store_vectorize, &vectorize_opts);

   nir_lower_mem_access_bit_sizes_options mem_bit_size_options = {
      .modes = nir_var_mem_constant | nir_var_mem_ubo | nir_var_mem_generic,
      .callback = nak_mem_access_size_align,
   };
   OPT(nir, nir_lower_mem_access_bit_sizes, &mem_bit_size_options);
   OPT(nir, nir_lower_bit_size, lower_bit_size_cb, (void *)nak);

   OPT(nir, nir_opt_combine_barriers, NULL, NULL);

   OPT(nir, nir_convert_to_lcssa, true, true);
   nir_divergence_analysis(nir);
   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      OPT(nir, nir_opt_tex_skip_helpers, true);
   OPT(nir, nak_nir_lower_scan_reduce, nak);

   nak_optimize_nir(nir, nak);

   OPT(nir, nak_nir_lower_tex, nak);
   OPT(nir, nir_lower_idiv, NULL);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   OPT(nir, nir_lower_indirect_derefs, 0, UINT32_MAX);

   if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      OPT(nir, nir_lower_tess_coord_z,
          nir->info.tess._primitive_mode == TESS_PRIMITIVE_TRIANGLES);
   }

   /* We need to do this before nak_nir_lower_system_values() because it
    * relies on the workgroup size being the actual HW workgroup size in
    * nir_intrinsic_load_subgroup_id.
    */
   if (gl_shader_stage_uses_workgroup(nir->info.stage) &&
       nir->info.derivative_group == DERIVATIVE_GROUP_QUADS) {
      assert(nir->info.workgroup_size[0] % 2 == 0);
      assert(nir->info.workgroup_size[1] % 2 == 0);
      nir->info.workgroup_size[0] *= 2;
      nir->info.workgroup_size[1] /= 2;
   }

   OPT(nir, nak_nir_lower_system_values, nak);

   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
   case MESA_SHADER_GEOMETRY:
      OPT(nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
          type_size_vec4, nir_lower_io_lower_64bit_to_32_new);
      OPT(nir, nir_opt_constant_folding);
      OPT(nir, nak_nir_lower_vtg_io, nak);
      if (nir->info.stage == MESA_SHADER_GEOMETRY)
         OPT(nir, nak_nir_lower_gs_intrinsics);
      break;

   case MESA_SHADER_FRAGMENT:
      OPT(nir, nir_lower_indirect_derefs,
          nir_var_shader_in | nir_var_shader_out, UINT32_MAX);
      OPT(nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
          type_size_vec4, nir_lower_io_lower_64bit_to_32_new |
          nir_lower_io_use_interpolated_input_intrinsics);
      OPT(nir, nir_opt_constant_folding);
      OPT(nir, nak_nir_lower_fs_inputs, nak, fs_key);
      OPT(nir, nak_nir_lower_fs_outputs);
      break;

   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      break;

   default:
      UNREACHABLE("Unsupported shader stage");
   }

   OPT(nir, nir_lower_doubles, NULL, nak->nir_options.lower_doubles_options);
   OPT(nir, nir_lower_int64);

   nak_optimize_nir(nir, nak);

   do {
      progress = false;
      OPT(nir, nir_opt_algebraic_late);
      OPT(nir, nak_nir_lower_algebraic_late, nak);

      /* If we're lowering fp64 sat but not min/max, the sat lowering may have
       * been undone by nir_opt_algebraic.  Lower sat again just to be sure.
       */
      if ((nak->nir_options.lower_doubles_options & nir_lower_dsat) &&
          !(nak->nir_options.lower_doubles_options & nir_lower_dminmax))
         OPT(nir, nir_lower_doubles, NULL, nir_lower_dsat);

      if (progress) {
         OPT(nir, nir_opt_constant_folding);
         OPT(nir, nir_copy_prop);
         OPT(nir, nir_opt_dce);
         OPT(nir, nir_opt_cse);
      }
   } while (progress);

   if (nak->sm < 70) {
      const nir_split_conversions_options split_conv_opts = {
         .callback = split_conversions_cb,
         .has_convert_alu_types = true,
      };
      OPT(nir, nir_split_conversions, &split_conv_opts);
   }

   /* Re-materialize load_const instructions in the blocks that use them.
    * This is both a register pressure optimization and a ensures correctness
    * in the presence of all of the control flow modifications we're about to
    * do.  Without this, we can't rely on anything to be constant in NIR to
    * NAK translation.
    */
   if (OPT(nir, nak_nir_rematerialize_load_const))
      OPT(nir, nir_opt_dce);

   OPT(nir, nir_convert_to_lcssa, false, false);

   if (nak->sm >= 73) {
      OPT(nir, nak_nir_mark_lcssa_invariants);
      if (OPT(nir, nak_nir_lower_non_uniform_ldcx, nak)) {
         OPT(nir, nir_copy_prop);
         OPT(nir, nir_opt_dce);
      }
   }

   OPT(nir, nak_nir_remove_barriers);

   /* Call divergence analysis regardless of sm version. */
   nir_divergence_analysis(nir);

   if (nak->sm >= 70) {
      if (nak_should_print_nir()) {
         fprintf(stderr, "Structured NIR for %s shader:\n",
                 _mesa_shader_stage_to_string(nir->info.stage));
         nir_print_shader(nir, stderr);
      }
      OPT(nir, nak_nir_lower_cf);
   }

   /* Re-index blocks and compact SSA defs because we'll use them to index
    * arrays
    */
   nir_foreach_function(func, nir) {
      if (func->impl) {
         nir_index_blocks(func->impl);
         nir_index_ssa_defs(func->impl);

         /* Ensure that divergence information is correct. */
         assert(func->impl->valid_metadata & nir_metadata_divergence);
      }
   }

   if (nak_should_print_nir()) {
      fprintf(stderr, "NIR for %s shader:\n",
              _mesa_shader_stage_to_string(nir->info.stage));
      nir_print_shader(nir, stderr);
   }
}

static bool
scalar_is_imm_int(nir_scalar x, unsigned bits, bool is_signed)
{
   if (!nir_scalar_is_const(x))
      return false;

   if (is_signed) {
      int64_t imm = nir_scalar_as_int(x);
      return u_intN_min(bits) <= imm && imm <= u_intN_max(bits);
   } else {
      return nir_scalar_as_uint(x) < u_uintN_max(bits);
   }
}

struct nak_io_addr_offset
nak_get_io_addr_offset(nir_def *addr, uint8_t imm_bits)
{
   nir_scalar addr_s = {
      .def = addr,
      .comp = 0,
   };

   /* If the entire address is constant, it's an unsigned immediate */
   if (scalar_is_imm_int(addr_s, imm_bits, false)) {
      /* Base is a dumb name for this.  It should be offset */
      return (struct nak_io_addr_offset) {
         .offset = nir_scalar_as_int(addr_s),
      };
   }

   addr_s = nir_scalar_chase_movs(addr_s);
   if (!nir_scalar_is_alu(addr_s) ||
       nir_scalar_alu_op(addr_s) != nir_op_iadd) {
      return (struct nak_io_addr_offset) {
         .base = addr_s,
      };
   }

   for (unsigned i = 0; i < 2; i++) {
      nir_scalar off_s = nir_scalar_chase_alu_src(addr_s, i);
      off_s = nir_scalar_chase_movs(off_s);

      /* If it's imm+indirect then the immediate is signed */
      if (scalar_is_imm_int(off_s, imm_bits, true)) {
         return (struct nak_io_addr_offset) {
            .base = nir_scalar_chase_alu_src(addr_s, 1 - i),
            .offset = nir_scalar_as_int(off_s),
         };
      }
   }

   return (struct nak_io_addr_offset) {
      .base = addr_s,
   };
}
