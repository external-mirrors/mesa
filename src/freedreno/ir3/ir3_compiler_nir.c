/*
 * Copyright © 2015 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <stdarg.h>

#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_string.h"

#include "ir3_compiler.h"
#include "ir3_image.h"
#include "ir3_nir.h"
#include "ir3_shader.h"

#include "instr-a3xx.h"
#include "ir3.h"
#include "ir3_context.h"
#include "ir3_ra.h"

static struct ir3_instruction_rpt
rpt_instr(struct ir3_instruction *instr, unsigned nrpt)
{
   struct ir3_instruction_rpt dst = {{0}};

   for (unsigned i = 0; i < nrpt; ++i)
      dst.rpts[i] = instr;

   return dst;
}

static void
cp_instrs(struct ir3_instruction *dst[], struct ir3_instruction *instrs[],
          unsigned n)
{
   for (unsigned i = 0; i < n; ++i)
      dst[i] = instrs[i];
}

static struct ir3_instruction_rpt
create_immed_rpt(struct ir3_builder *build, unsigned nrpt, unsigned val)
{
   return rpt_instr(create_immed(build, val), nrpt);
}

static struct ir3_instruction_rpt
create_immed_shared_rpt(struct ir3_builder *build, unsigned nrpt, uint32_t val,
                        bool shared)
{
   return rpt_instr(create_immed_shared(build, val, shared), nrpt);
}

static struct ir3_instruction_rpt
create_immed_typed_rpt(struct ir3_builder *build, unsigned nrpt, unsigned val,
                       type_t type)
{
   return rpt_instr(create_immed_typed(build, val, type), nrpt);
}

static inline struct ir3_instruction_rpt
create_immed_typed_shared_rpt(struct ir3_builder *build, unsigned nrpt,
                              uint32_t val, type_t type, bool shared)
{
   return rpt_instr(create_immed_typed_shared(build, val, type, shared), nrpt);
}

static void
set_instr_flags(struct ir3_instruction *instrs[], unsigned n,
                ir3_instruction_flags flags)
{
   for (unsigned i = 0; i < n; ++i)
      instrs[i]->flags |= flags;
}

static void
set_cat1_round(struct ir3_instruction *instrs[], unsigned n, round_t round)
{
   for (unsigned i = 0; i < n; ++i)
      instrs[i]->cat1.round = round;
}

static void
set_cat2_condition(struct ir3_instruction *instrs[], unsigned n,
                   unsigned condition)
{
   for (unsigned i = 0; i < n; ++i)
      instrs[i]->cat2.condition = condition;
}

static void
set_dst_flags(struct ir3_instruction *instrs[], unsigned n,
              ir3_register_flags flags)
{
   for (unsigned i = 0; i < n; ++i)
      instrs[i]->dsts[0]->flags |= flags;
}

void
ir3_handle_nonuniform(struct ir3_instruction *instr,
                      nir_intrinsic_instr *intrin)
{
   if (nir_intrinsic_has_access(intrin) &&
       (nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM)) {
      instr->flags |= IR3_INSTR_NONUNIF;
   }
}

void
ir3_handle_bindless_cat6(struct ir3_instruction *instr, nir_src rsrc)
{
   nir_intrinsic_instr *intrin = ir3_bindless_resource(rsrc);
   if (!intrin)
      return;

   instr->flags |= IR3_INSTR_B;
   instr->cat6.base = nir_intrinsic_desc_set(intrin);
}

static struct ir3_instruction *
create_input(struct ir3_context *ctx, unsigned compmask)
{
   struct ir3_instruction *in;

   in = ir3_instr_create_at(ir3_before_terminator(ctx->in_block),
                            OPC_META_INPUT, 1, 0);
   in->input.sysval = ~0;
   __ssa_dst(in)->wrmask = compmask;

   array_insert(ctx->ir, ctx->ir->inputs, in);

   return in;
}

static struct ir3_instruction_rpt
create_frag_input(struct ir3_context *ctx, struct ir3_instruction *coord,
                  unsigned n, unsigned ncomp)
{
   struct ir3_builder *build = &ctx->build;
   struct ir3_instruction_rpt instr;
   /* packed inloc is fixed up later: */
   struct ir3_instruction_rpt inloc;

   for (unsigned i = 0; i < ncomp; i++)
      inloc.rpts[i] = create_immed(build, n + i);

   if (coord) {
      instr =
         ir3_BARY_F_rpt(build, ncomp, inloc, 0, rpt_instr(coord, ncomp), 0);
   } else if (ctx->compiler->flat_bypass) {
      if (ctx->compiler->gen >= 6) {
         instr = ir3_FLAT_B_rpt(build, ncomp, inloc, 0, inloc, 0);
      } else {
         for (unsigned i = 0; i < ncomp; i++) {
            instr.rpts[i] =
               ir3_LDLV(build, inloc.rpts[i], 0, create_immed(build, 1), 0);
            instr.rpts[i]->cat6.type = TYPE_U32;
            instr.rpts[i]->cat6.iim_val = 1;
         }
      }
   } else {
      instr = ir3_BARY_F_rpt(build, ncomp, inloc, 0,
                             rpt_instr(ctx->ij[IJ_PERSP_PIXEL], ncomp), 0);

      for (unsigned i = 0; i < ncomp; i++)
         instr.rpts[i]->srcs[1]->wrmask = 0x3;
   }

   return instr;
}

static struct ir3_instruction *
create_driver_param(struct ir3_context *ctx, uint32_t dp)
{
   /* first four vec4 sysval's reserved for UBOs: */
   /* NOTE: dp is in scalar, but there can be >4 dp components: */
   unsigned r = ir3_const_reg(ir3_const_state(ctx->so),
                              IR3_CONST_ALLOC_DRIVER_PARAMS, dp);
   return create_uniform(&ctx->build, r);
}

static struct ir3_instruction *
create_driver_param_indirect(struct ir3_context *ctx, uint32_t dp,
                             struct ir3_instruction *address)
{
   /* first four vec4 sysval's reserved for UBOs: */
   /* NOTE: dp is in scalar, but there can be >4 dp components: */
   const struct ir3_const_state *const_state = ir3_const_state(ctx->so);
   unsigned n =
      const_state->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4;
   return create_uniform_indirect(&ctx->build, n * 4 + dp, TYPE_U32, address);
}

/*
 * Adreno's comparisons produce a 1 for true and 0 for false, in either 16 or
 * 32-bit registers.  We use NIR's 1-bit integers to represent bools, and
 * trust that we will only see and/or/xor on those 1-bit values, so we can
 * safely store NIR i1s in a 32-bit reg while always containing either a 1 or
 * 0.
 */

/*
 * alu/sfu instructions:
 */

static struct ir3_instruction_rpt
create_cov(struct ir3_context *ctx, unsigned nrpt,
           struct ir3_instruction_rpt src, unsigned src_bitsize, nir_op op)
{
   type_t src_type, dst_type;

   switch (op) {
   case nir_op_f2f32:
   case nir_op_f2f16_rtne:
   case nir_op_f2f16_rtz:
   case nir_op_f2f16:
   case nir_op_f2i32:
   case nir_op_f2i16:
   case nir_op_f2i8:
   case nir_op_f2u32:
   case nir_op_f2u16:
   case nir_op_f2u8:
      switch (src_bitsize) {
      case 32:
         src_type = TYPE_F32;
         break;
      case 16:
         src_type = TYPE_F16;
         break;
      default:
         ir3_context_error(ctx, "invalid src bit size: %u", src_bitsize);
      }
      break;

   case nir_op_i2f32:
   case nir_op_i2f16:
   case nir_op_i2i32:
   case nir_op_i2i16:
   case nir_op_i2i8:
      switch (src_bitsize) {
      case 32:
         src_type = TYPE_S32;
         break;
      case 16:
         src_type = TYPE_S16;
         break;
      case 8:
         src_type = TYPE_U8;
         break;
      default:
         ir3_context_error(ctx, "invalid src bit size: %u", src_bitsize);
      }
      break;

   case nir_op_u2f32:
   case nir_op_u2f16:
   case nir_op_u2u32:
   case nir_op_u2u16:
   case nir_op_u2u8:
      switch (src_bitsize) {
      case 32:
         src_type = TYPE_U32;
         break;
      case 16:
         src_type = TYPE_U16;
         break;
      case 8:
         src_type = TYPE_U8;
         break;
      default:
         ir3_context_error(ctx, "invalid src bit size: %u", src_bitsize);
      }
      break;

   case nir_op_b2f16:
   case nir_op_b2f32:
   case nir_op_b2i8:
   case nir_op_b2i16:
   case nir_op_b2i32:
      src_type = ctx->compiler->bool_type;
      break;

   default:
      ir3_context_error(ctx, "invalid conversion op: %u", op);
   }

   switch (op) {
   case nir_op_f2f32:
   case nir_op_i2f32:
   case nir_op_u2f32:
   case nir_op_b2f32:
      dst_type = TYPE_F32;
      break;

   case nir_op_f2f16_rtne:
   case nir_op_f2f16_rtz:
   case nir_op_f2f16:
   case nir_op_i2f16:
   case nir_op_u2f16:
   case nir_op_b2f16:
      dst_type = TYPE_F16;
      break;

   case nir_op_f2i32:
   case nir_op_i2i32:
   case nir_op_b2i32:
      dst_type = TYPE_S32;
      break;

   case nir_op_f2i16:
   case nir_op_i2i16:
   case nir_op_b2i16:
      dst_type = TYPE_S16;
      break;

   case nir_op_f2i8:
   case nir_op_i2i8:
   case nir_op_b2i8:
      dst_type = TYPE_U8;
      break;

   case nir_op_f2u32:
   case nir_op_u2u32:
      dst_type = TYPE_U32;
      break;

   case nir_op_f2u16:
   case nir_op_u2u16:
      dst_type = TYPE_U16;
      break;

   case nir_op_f2u8:
   case nir_op_u2u8:
      dst_type = TYPE_U8;
      break;

   default:
      ir3_context_error(ctx, "invalid conversion op: %u", op);
   }

   if (src_type == dst_type)
      return src;

   /* Zero-extension of 8-bit values doesn't work with `cov`, so simple masking
    * is used to achieve the result.
    */
   if (src_type == TYPE_U8 && full_type(dst_type) == TYPE_U32) {
      struct ir3_instruction_rpt mask =
         create_immed_typed_rpt(&ctx->build, nrpt, 0xff, TYPE_U8);
      struct ir3_instruction_rpt cov =
         ir3_AND_B_rpt(&ctx->build, nrpt, src, 0, mask, 0);
      set_dst_flags(cov.rpts, nrpt, type_flags(dst_type));
      return cov;
   }

   /* Conversion of 8-bit values into floating-point values doesn't work with
    * a simple `cov`, instead the 8-bit values first have to be converted into
    * corresponding 16-bit values and converted from there.
    */
   if (src_type == TYPE_U8 && full_type(dst_type) == TYPE_F32) {
      assert(op == nir_op_u2f16 || op == nir_op_i2f16 ||
             op == nir_op_u2f32 || op == nir_op_i2f32);

      struct ir3_instruction_rpt cov;
      if (op == nir_op_u2f16 || op == nir_op_u2f32) {
         struct ir3_instruction_rpt mask =
            create_immed_typed_rpt(&ctx->build, nrpt, 0xff, TYPE_U8);
         cov = ir3_AND_B_rpt(&ctx->build, nrpt, src, 0, mask, 0);
         set_dst_flags(cov.rpts, nrpt, IR3_REG_HALF);
         cov = ir3_COV_rpt(&ctx->build, nrpt, cov, TYPE_U16, dst_type);
      } else {
         cov = ir3_COV_rpt(&ctx->build, nrpt, src, TYPE_U8, TYPE_S16);
         cov = ir3_COV_rpt(&ctx->build, nrpt, cov, TYPE_S16, dst_type);
      }
      return cov;
   }

   /* Conversion of floating-point values to 8-bit values also doesn't work
    * through a single `cov`, instead the conversion has to go through the
    * corresponding 16-bit type that's then truncated.
    */
   if (full_type(src_type) == TYPE_F32 && dst_type == TYPE_U8) {
      assert(op == nir_op_f2u8 || op == nir_op_f2i8);

      type_t intermediate_type = op == nir_op_f2u8 ? TYPE_U16 : TYPE_S16;
      struct ir3_instruction_rpt cov =
         ir3_COV_rpt(&ctx->build, nrpt, src, src_type, intermediate_type);
      cov = ir3_COV_rpt(&ctx->build, nrpt, cov, intermediate_type, TYPE_U8);
      return cov;
   }

   struct ir3_instruction_rpt cov =
      ir3_COV_rpt(&ctx->build, nrpt, src, src_type, dst_type);

   if (op == nir_op_f2f16_rtne) {
      set_cat1_round(cov.rpts, nrpt, ROUND_EVEN);
   } else if (op == nir_op_f2f16_rtz) {
      set_cat1_round(cov.rpts, nrpt, ROUND_ZERO);
   } else if (dst_type == TYPE_F16 || dst_type == TYPE_F32) {
      unsigned execution_mode = ctx->s->info.float_controls_execution_mode;
      nir_alu_type type =
         dst_type == TYPE_F16 ? nir_type_float16 : nir_type_float32;
      nir_rounding_mode rounding_mode =
         nir_get_rounding_mode_from_float_controls(execution_mode, type);
      if (rounding_mode == nir_rounding_mode_rtne)
         set_cat1_round(cov.rpts, nrpt, ROUND_EVEN);
      else if (rounding_mode == nir_rounding_mode_rtz)
         set_cat1_round(cov.rpts, nrpt, ROUND_ZERO);
   }

   return cov;
}

/* For shift instructions NIR always has shift amount as 32 bit integer */
static struct ir3_instruction_rpt
resize_shift_amount(struct ir3_context *ctx, unsigned nrpt,
                    struct ir3_instruction_rpt src, unsigned bs)
{
   if (bs == 16)
      return ir3_COV_rpt(&ctx->build, nrpt, src, TYPE_U32, TYPE_U16);
   else if (bs == 8)
      return ir3_COV_rpt(&ctx->build, nrpt, src, TYPE_U32, TYPE_U8);
   else
      return src;
}

static void
emit_alu_dot_4x8_as_dp4acc(struct ir3_context *ctx, nir_alu_instr *alu,
                           struct ir3_instruction **dst,
                           struct ir3_instruction **src)
{
   if (ctx->compiler->has_compliant_dp4acc) {
      dst[0] = ir3_DP4ACC(&ctx->build, src[0], 0, src[1], 0, src[2], 0);

      /* This is actually the LHS signedness attribute.
       * IR3_SRC_UNSIGNED ~ unsigned LHS (i.e. OpUDot and OpUDotAccSat).
       */
      if (alu->op == nir_op_udot_4x8_uadd ||
          alu->op == nir_op_udot_4x8_uadd_sat) {
         dst[0]->cat3.signedness = IR3_SRC_UNSIGNED;
      } else {
         dst[0]->cat3.signedness = IR3_SRC_MIXED;
      }

      /* This is actually the RHS signedness attribute.
       * IR3_SRC_PACKED_HIGH ~ signed RHS (i.e. OpSDot and OpSDotAccSat).
       */
      if (alu->op == nir_op_sdot_4x8_iadd ||
          alu->op == nir_op_sdot_4x8_iadd_sat) {
         dst[0]->cat3.packed = IR3_SRC_PACKED_HIGH;
      } else {
         dst[0]->cat3.packed = IR3_SRC_PACKED_LOW;
      }

      if (alu->op == nir_op_udot_4x8_uadd_sat ||
          alu->op == nir_op_sdot_4x8_iadd_sat ||
          alu->op == nir_op_sudot_4x8_iadd_sat) {
         dst[0]->flags |= IR3_INSTR_SAT;
      }
      return;
   }

   struct ir3_instruction *accumulator = NULL;
   if (alu->op == nir_op_udot_4x8_uadd_sat) {
      accumulator = create_immed(&ctx->build, 0);
   } else {
      accumulator = src[2];
   }

   dst[0] = ir3_DP4ACC(&ctx->build, src[0], 0, src[1], 0, accumulator, 0);

   if (alu->op == nir_op_udot_4x8_uadd ||
       alu->op == nir_op_udot_4x8_uadd_sat) {
      dst[0]->cat3.signedness = IR3_SRC_UNSIGNED;
   } else {
      dst[0]->cat3.signedness = IR3_SRC_MIXED;
   }

   /* For some reason (sat) doesn't work in unsigned case so
    * we have to emulate it.
    */
   if (alu->op == nir_op_udot_4x8_uadd_sat) {
      dst[0] = ir3_ADD_U(&ctx->build, dst[0], 0, src[2], 0);
      dst[0]->flags |= IR3_INSTR_SAT;
   } else if (alu->op == nir_op_sudot_4x8_iadd_sat) {
      dst[0]->flags |= IR3_INSTR_SAT;
   }
}

static void
emit_alu_dot_4x8_as_dp2acc(struct ir3_context *ctx, nir_alu_instr *alu,
                           struct ir3_instruction **dst,
                           struct ir3_instruction **src)
{
   int signedness;
   if (alu->op == nir_op_udot_4x8_uadd ||
       alu->op == nir_op_udot_4x8_uadd_sat) {
      signedness = IR3_SRC_UNSIGNED;
   } else {
      signedness = IR3_SRC_MIXED;
   }

   struct ir3_instruction *accumulator = NULL;
   if (alu->op == nir_op_udot_4x8_uadd_sat ||
       alu->op == nir_op_sudot_4x8_iadd_sat) {
      accumulator = create_immed(&ctx->build, 0);
   } else {
      accumulator = src[2];
   }

   dst[0] = ir3_DP2ACC(&ctx->build, src[0], 0, src[1], 0, accumulator, 0);
   dst[0]->cat3.packed = IR3_SRC_PACKED_LOW;
   dst[0]->cat3.signedness = signedness;

   dst[0] = ir3_DP2ACC(&ctx->build, src[0], 0, src[1], 0, dst[0], 0);
   dst[0]->cat3.packed = IR3_SRC_PACKED_HIGH;
   dst[0]->cat3.signedness = signedness;

   if (alu->op == nir_op_udot_4x8_uadd_sat) {
      dst[0] = ir3_ADD_U(&ctx->build, dst[0], 0, src[2], 0);
      dst[0]->flags |= IR3_INSTR_SAT;
   } else if (alu->op == nir_op_sudot_4x8_iadd_sat) {
      dst[0] = ir3_ADD_S(&ctx->build, dst[0], 0, src[2], 0);
      dst[0]->flags |= IR3_INSTR_SAT;
   }
}

static bool
all_sat_compatible(struct ir3_instruction *instrs[], unsigned n)
{
   for (unsigned i = 0; i < n; i++) {
      if (!is_sat_compatible(instrs[i]->opc))
         return false;
   }

   return true;
}

/* Is src the only use of its def, taking components into account. */
static bool
is_unique_use(nir_src *src)
{
   nir_def *def = src->ssa;

   if (list_is_singular(&def->uses))
      return true;

   nir_component_mask_t src_read_mask = nir_src_components_read(src);

   nir_foreach_use (use, def) {
      if (use == src)
         continue;

      if (nir_src_components_read(use) & src_read_mask)
         return false;
   }

   return true;
}

static void
emit_alu(struct ir3_context *ctx, nir_alu_instr *alu)
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   struct ir3_instruction_rpt dst, src[info->num_inputs];
   unsigned bs[info->num_inputs]; /* bit size */
   struct ir3_builder *b = &ctx->build;
   unsigned dst_sz;
   unsigned dst_bitsize = ir3_bitsize(ctx, alu->def.bit_size);
   type_t dst_type = type_uint_size(dst_bitsize);

   dst_sz = alu->def.num_components;
   assert(dst_sz == 1 || ir3_supports_vectorized_nir_op(alu->op));

   bool use_shared = !alu->def.divergent &&
      ctx->compiler->has_scalar_alu &&
      /* it probably isn't worth emulating these with scalar-only ops */
      alu->op != nir_op_udot_4x8_uadd &&
      alu->op != nir_op_udot_4x8_uadd_sat &&
      alu->op != nir_op_sdot_4x8_iadd &&
      alu->op != nir_op_sdot_4x8_iadd_sat &&
      alu->op != nir_op_sudot_4x8_iadd &&
      alu->op != nir_op_sudot_4x8_iadd_sat;

   struct ir3_instruction **def = ir3_get_def(ctx, &alu->def, dst_sz);

   /* Vectors are special in that they have non-scalarized writemasks,
    * and just take the first swizzle channel for each argument in
    * order into each writemask channel.
    */
   if ((alu->op == nir_op_vec2) || (alu->op == nir_op_vec3) ||
       (alu->op == nir_op_vec4) || (alu->op == nir_op_vec8) ||
       (alu->op == nir_op_vec16)) {
      for (int i = 0; i < info->num_inputs; i++) {
         nir_alu_src *asrc = &alu->src[i];
         struct ir3_instruction *src =
            ir3_get_src_shared(ctx, &asrc->src, use_shared)[asrc->swizzle[0]];
         compile_assert(ctx, src);
         def[i] = ir3_MOV(b, src, dst_type);
      }

      ir3_instr_create_rpt(def, info->num_inputs);
      ir3_put_def(ctx, &alu->def);
      return;
   }

   assert(dst_sz <= ARRAY_SIZE(src[0].rpts));

   for (int i = 0; i < info->num_inputs; i++) {
      nir_alu_src *asrc = &alu->src[i];
      struct ir3_instruction *const *input_src =
         ir3_get_src_shared(ctx, &asrc->src, use_shared);
      bs[i] = nir_src_bit_size(asrc->src);

      for (unsigned rpt = 0; rpt < dst_sz; rpt++) {
         src[i].rpts[rpt] = input_src[asrc->swizzle[rpt]];
         compile_assert(ctx, src[i].rpts[rpt]);
      }
   }

   switch (alu->op) {
   case nir_op_mov:
      dst = ir3_MOV_rpt(b, dst_sz, src[0], dst_type);
      break;

   case nir_op_f2f32:
   case nir_op_f2f16_rtne:
   case nir_op_f2f16_rtz:
   case nir_op_f2f16:
   case nir_op_f2i32:
   case nir_op_f2i16:
   case nir_op_f2i8:
   case nir_op_f2u32:
   case nir_op_f2u16:
   case nir_op_f2u8:
   case nir_op_i2f32:
   case nir_op_i2f16:
   case nir_op_i2i32:
   case nir_op_i2i16:
   case nir_op_i2i8:
   case nir_op_u2f32:
   case nir_op_u2f16:
   case nir_op_u2u32:
   case nir_op_u2u16:
   case nir_op_u2u8:
   case nir_op_b2f16:
   case nir_op_b2f32:
   case nir_op_b2i8:
   case nir_op_b2i16:
   case nir_op_b2i32:
      dst = create_cov(ctx, dst_sz, src[0], bs[0], alu->op);
      break;

   case nir_op_u2u64:
      assert(dst_sz == 1);
      dst.rpts[0] = ir3_64b(b, ir3_MOV(b, src[0].rpts[0], TYPE_U32),
                            create_immed_shared(b, 0, use_shared));
      break;

   case nir_op_fquantize2f16:
      dst = create_cov(ctx, dst_sz,
                       create_cov(ctx, dst_sz, src[0], 32, nir_op_f2f16_rtne),
                       16, nir_op_f2f32);
      break;

   case nir_op_b2b1:
      /* b2b1 will appear when translating from
       *
       * - nir_intrinsic_load_shared of a 32-bit 0/~0 value.
       * - nir_intrinsic_load_constant of a 32-bit 0/~0 value
       *
       * A negate can turn those into a 1 or 0 for us.
       */
      dst = ir3_ABSNEG_S_rpt(b, dst_sz, src[0], IR3_REG_SNEG);
      break;

   case nir_op_b2b32:
      /* b2b32 will appear when converting our 1-bit bools to a store_shared
       * argument.
       *
       * A negate can turn those into a ~0 for us.
       */
      dst = ir3_ABSNEG_S_rpt(b, dst_sz, src[0], IR3_REG_SNEG);
      break;

   case nir_op_fneg:
      dst = ir3_ABSNEG_F_rpt(b, dst_sz, src[0], IR3_REG_FNEG);
      break;
   case nir_op_fabs:
      dst = ir3_ABSNEG_F_rpt(b, dst_sz, src[0], IR3_REG_FABS);
      break;
   case nir_op_fmax:
      dst = ir3_MAX_F_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_fmin:
      dst = ir3_MIN_F_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_fsat:
      /* if there is just a single use of the src, and it supports
       * (sat) bit, we can just fold the (sat) flag back to the
       * src instruction and create a mov.  This is easier for cp
       * to eliminate.
       */
      if (all_sat_compatible(src[0].rpts, dst_sz) &&
          is_unique_use(&alu->src[0].src)) {
         set_instr_flags(src[0].rpts, dst_sz, IR3_INSTR_SAT);
         dst = ir3_MOV_rpt(b, dst_sz, src[0], dst_type);
      } else {
         /* otherwise generate a max.f that saturates.. blob does
          * similar (generating a cat2 mov using max.f)
          */
         dst = ir3_MAX_F_rpt(b, dst_sz, src[0], 0, src[0], 0);
         set_instr_flags(dst.rpts, dst_sz, IR3_INSTR_SAT);
      }
      break;
   case nir_op_fmul:
      dst = ir3_MUL_F_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_fadd:
      dst = ir3_ADD_F_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_fsub:
      dst = ir3_ADD_F_rpt(b, dst_sz, src[0], 0, src[1], IR3_REG_FNEG);
      break;
   case nir_op_ffma:
      /* The scalar ALU doesn't support mad, so expand to mul+add so that we
       * don't unnecessarily fall back to non-earlypreamble. This is safe
       * because at least on a6xx+ mad is unfused.
       */
      if (use_shared) {
         struct ir3_instruction_rpt mul01 =
            ir3_MUL_F_rpt(b, dst_sz, src[0], 0, src[1], 0);

         if (is_half(src[0].rpts[0])) {
            set_dst_flags(mul01.rpts, dst_sz, IR3_REG_HALF);
         }

         dst = ir3_ADD_F_rpt(b, dst_sz, mul01, 0, src[2], 0);
      } else {
         dst = ir3_MAD_F32_rpt(b, dst_sz, src[0], 0, src[1], 0, src[2], 0);
      }
      break;
   case nir_op_flt:
      dst = ir3_CMPS_F_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_LT);
      break;
   case nir_op_fge:
      dst = ir3_CMPS_F_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_GE);
      break;
   case nir_op_feq:
      dst = ir3_CMPS_F_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_EQ);
      break;
   case nir_op_fneu:
      dst = ir3_CMPS_F_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_NE);
      break;
   case nir_op_fceil:
      dst = ir3_CEIL_F_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_ffloor:
      dst = ir3_FLOOR_F_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_ftrunc:
      dst = ir3_TRUNC_F_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_fround_even:
      dst = ir3_RNDNE_F_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_fsign:
      dst = ir3_SIGN_F_rpt(b, dst_sz, src[0], 0);
      break;

   case nir_op_fsin:
      dst = ir3_SIN_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_fcos:
      dst = ir3_COS_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_frsq:
      dst = ir3_RSQ_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_frcp:
      assert(dst_sz == 1);
      dst.rpts[0] = ir3_RCP(b, src[0].rpts[0], 0);
      break;
   case nir_op_flog2:
      dst = ir3_LOG2_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_fexp2:
      dst = ir3_EXP2_rpt(b, dst_sz, src[0], 0);
      break;
   case nir_op_fsqrt:
      dst = ir3_SQRT_rpt(b, dst_sz, src[0], 0);
      break;

   case nir_op_iabs:
      dst = ir3_ABSNEG_S_rpt(b, dst_sz, src[0], IR3_REG_SABS);
      break;
   case nir_op_iadd:
      dst = ir3_ADD_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_iadd3:
      if (use_shared) {
         /* sad doesn't support the scalar ALU so expand to two adds so that we
          * don't unnecessarily fall back to non-earlypreamble.
          */
         struct ir3_instruction_rpt add01 =
            ir3_ADD_U_rpt(b, dst_sz, src[0], 0, src[1], 0);

         if (is_half(src[0].rpts[0])) {
            set_dst_flags(add01.rpts, dst_sz, IR3_REG_HALF);
         }

         dst = ir3_ADD_U_rpt(b, dst_sz, add01, 0, src[2], 0);
      } else {
         if (is_half(src[0].rpts[0])) {
            dst = ir3_SAD_S16_rpt(b, dst_sz, src[0], 0, src[1], 0, src[2], 0);
         } else {
            dst = ir3_SAD_S32_rpt(b, dst_sz, src[0], 0, src[1], 0, src[2], 0);
         }
      }
      break;
   case nir_op_ihadd:
      dst = ir3_ADD_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_dst_flags(dst.rpts, dst_sz, IR3_REG_EI);
      break;
   case nir_op_uhadd:
      dst = ir3_ADD_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_dst_flags(dst.rpts, dst_sz, IR3_REG_EI);
      break;
   case nir_op_iand:
      dst = ir3_AND_B_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_imax:
      dst = ir3_MAX_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_umax:
      dst = ir3_MAX_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_imin:
      dst = ir3_MIN_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_umin:
      dst = ir3_MIN_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_umul_low:
      dst = ir3_MULL_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_imadsh_mix16:
      if (use_shared) {
         struct ir3_instruction_rpt sixteen =
            create_immed_shared_rpt(b, dst_sz, 16, true);
         struct ir3_instruction_rpt src1 =
            ir3_SHR_B_rpt(b, dst_sz, src[1], 0, sixteen, 0);
         struct ir3_instruction_rpt mul =
            ir3_MULL_U_rpt(b, dst_sz, src[0], 0, src1, 0);
         dst = ir3_ADD_U_rpt(b, dst_sz,
                             ir3_SHL_B_rpt(b, dst_sz, mul, 0, sixteen, 0), 0,
                             src[2], 0);
      } else {
         dst = ir3_MADSH_M16_rpt(b, dst_sz, src[0], 0, src[1], 0, src[2], 0);
      }
      break;
   case nir_op_imad24_ir3:
      if (use_shared) {
         dst = ir3_ADD_U_rpt(b, dst_sz,
                             ir3_MUL_U24_rpt(b, dst_sz, src[0], 0, src[1], 0),
                             0, src[2], 0);
      } else {
         dst = ir3_MAD_S24_rpt(b, dst_sz, src[0], 0, src[1], 0, src[2], 0);
      }
      break;
   case nir_op_imul:
      compile_assert(ctx, alu->def.bit_size == 8 || alu->def.bit_size == 16);
      dst = ir3_MUL_S24_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_imul24:
      dst = ir3_MUL_S24_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_ineg:
      dst = ir3_ABSNEG_S_rpt(b, dst_sz, src[0], IR3_REG_SNEG);
      break;
   case nir_op_inot:
      if (bs[0] == 1) {
         struct ir3_instruction_rpt one = create_immed_typed_shared_rpt(
            b, dst_sz, 1, ctx->compiler->bool_type, use_shared);
         dst = ir3_SUB_U_rpt(b, dst_sz, one, 0, src[0], 0);
      } else {
         dst = ir3_NOT_B_rpt(b, dst_sz, src[0], 0);
      }
      break;
   case nir_op_ior:
      dst = ir3_OR_B_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_ishl:
      dst = ir3_SHL_B_rpt(b, dst_sz, src[0], 0,
                          resize_shift_amount(ctx, dst_sz, src[1], bs[0]), 0);
      break;
   case nir_op_ishr:
      dst = ir3_ASHR_B_rpt(b, dst_sz, src[0], 0,
                           resize_shift_amount(ctx, dst_sz, src[1], bs[0]), 0);
      break;
   case nir_op_isub:
      dst = ir3_SUB_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_ixor:
      dst = ir3_XOR_B_rpt(b, dst_sz, src[0], 0, src[1], 0);
      break;
   case nir_op_ushr:
      dst = ir3_SHR_B_rpt(b, dst_sz, src[0], 0,
                          resize_shift_amount(ctx, dst_sz, src[1], bs[0]), 0);
      break;
   case nir_op_shrm_ir3:
      dst = ir3_SHRM_rpt(b, dst_sz,
                         resize_shift_amount(ctx, dst_sz, src[1], bs[0]), 0,
                         src[0], 0, src[2], 0);
      break;
   case nir_op_shlm_ir3:
      dst = ir3_SHLM_rpt(b, dst_sz,
                         resize_shift_amount(ctx, dst_sz, src[1], bs[0]), 0,
                         src[0], 0, src[2], 0);
      break;
   case nir_op_shrg_ir3:
      dst = ir3_SHRG_rpt(b, dst_sz,
                         resize_shift_amount(ctx, dst_sz, src[1], bs[0]), 0,
                         src[0], 0, src[2], 0);
      break;
   case nir_op_shlg_ir3:
      dst = ir3_SHLG_rpt(b, dst_sz,
                         resize_shift_amount(ctx, dst_sz, src[1], bs[0]), 0,
                         src[0], 0, src[2], 0);
      break;
   case nir_op_andg_ir3:
      dst = ir3_ANDG_rpt(b, dst_sz, src[0], 0, src[1], 0, src[2], 0);
      break;
   case nir_op_ilt:
      dst = ir3_CMPS_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_LT);
      break;
   case nir_op_ige:
      dst = ir3_CMPS_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_GE);
      break;
   case nir_op_ieq:
      dst = ir3_CMPS_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_EQ);
      break;
   case nir_op_ine:
      dst = ir3_CMPS_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_NE);
      break;
   case nir_op_ult:
      dst = ir3_CMPS_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_LT);
      break;
   case nir_op_uge:
      dst = ir3_CMPS_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_cat2_condition(dst.rpts, dst_sz, IR3_COND_GE);
      break;

   case nir_op_icsel_eqz:
   case nir_op_bcsel: {
      struct ir3_instruction_rpt conds;

      compile_assert(ctx, bs[1] == bs[2]);

      /* TODO: repeat the covs when possible. */
      for (unsigned rpt = 0; rpt < dst_sz; ++rpt) {
         struct ir3_instruction *cond =
            ir3_get_cond_for_nonzero_compare(src[0].rpts[rpt]);

         /* The condition's size has to match the other two arguments' size, so
          * convert down if necessary.
          *
          * Single hashtable is fine, because the conversion will either be
          * 16->32 or 32->16, but never both
          */
         if (is_half(src[1].rpts[rpt]) != is_half(cond)) {
            struct hash_entry *prev_entry = _mesa_hash_table_search(
               ctx->sel_cond_conversions, src[0].rpts[rpt]);
            if (prev_entry) {
               cond = prev_entry->data;
            } else {
               if (is_half(cond)) {
                  if (bs[0] == 8) {
                     /* Zero-extension of an 8-bit value has to be done through
                      * masking, as in create_cov.
                      */
                     struct ir3_instruction *mask =
                        create_immed_typed(b, 0xff, TYPE_U8);
                     cond = ir3_AND_B(b, cond, 0, mask, 0);
                  } else {
                     cond = ir3_COV(b, cond, TYPE_U16, TYPE_U32);
                  }
               } else {
                  cond = ir3_COV(b, cond, TYPE_U32, TYPE_U16);
               }
               _mesa_hash_table_insert(ctx->sel_cond_conversions,
                                       src[0].rpts[rpt], cond);
            }
         }
         conds.rpts[rpt] = cond;
      }

      if (alu->op == nir_op_icsel_eqz) {
         struct ir3_instruction_rpt tmp = src[1];
         src[1] = src[2];
         src[2] = tmp;
      }

      if (is_half(src[1].rpts[0]))
         dst = ir3_SEL_B16_rpt(b, dst_sz, src[1], 0, conds, 0, src[2], 0);
      else
         dst = ir3_SEL_B32_rpt(b, dst_sz, src[1], 0, conds, 0, src[2], 0);
      break;
   }

   case nir_op_bit_count: {
      if (ctx->compiler->gen < 5 ||
          (src[0].rpts[0]->dsts[0]->flags & IR3_REG_HALF)) {
         dst = ir3_CBITS_B_rpt(b, dst_sz, src[0], 0);
         break;
      }

      // We need to do this 16b at a time on a5xx+a6xx.  Once half-precision
      // support is in place, this should probably move to a NIR lowering pass:
      struct ir3_instruction_rpt hi, lo;

      hi = ir3_COV_rpt(
         b, dst_sz,
         ir3_SHR_B_rpt(b, dst_sz, src[0], 0,
                       create_immed_shared_rpt(b, dst_sz, 16, use_shared), 0),
         TYPE_U32, TYPE_U16);
      lo = ir3_COV_rpt(b, dst_sz, src[0], TYPE_U32, TYPE_U16);

      hi = ir3_CBITS_B_rpt(b, dst_sz, hi, 0);
      lo = ir3_CBITS_B_rpt(b, dst_sz, lo, 0);

      // TODO maybe the builders should default to making dst half-precision
      // if the src's were half precision, to make this less awkward.. otoh
      // we should probably just do this lowering in NIR.
      set_dst_flags(hi.rpts, dst_sz, IR3_REG_HALF);
      set_dst_flags(lo.rpts, dst_sz, IR3_REG_HALF);

      dst = ir3_ADD_S_rpt(b, dst_sz, hi, 0, lo, 0);
      set_dst_flags(dst.rpts, dst_sz, IR3_REG_HALF);
      dst = ir3_COV_rpt(b, dst_sz, dst, TYPE_U16, TYPE_U32);
      break;
   }
   case nir_op_ifind_msb: {
      struct ir3_instruction_rpt cmp;
      dst = ir3_CLZ_S_rpt(b, dst_sz, src[0], 0);
      cmp =
         ir3_CMPS_S_rpt(b, dst_sz, dst, 0,
                        create_immed_shared_rpt(b, dst_sz, 0, use_shared), 0);
      set_cat2_condition(cmp.rpts, dst_sz, IR3_COND_GE);
      dst = ir3_SEL_B32_rpt(
         b, dst_sz,
         ir3_SUB_U_rpt(b, dst_sz,
                       create_immed_shared_rpt(b, dst_sz, 31, use_shared), 0,
                       dst, 0),
         0, cmp, 0, dst, 0);
      break;
   }
   case nir_op_ufind_msb:
      dst = ir3_CLZ_B_rpt(b, dst_sz, src[0], 0);
      dst = ir3_SEL_B32_rpt(
         b, dst_sz,
         ir3_SUB_U_rpt(b, dst_sz,
                       create_immed_shared_rpt(b, dst_sz, 31, use_shared), 0,
                       dst, 0),
         0, src[0], 0, dst, 0);
      break;
   case nir_op_find_lsb:
      dst = ir3_BFREV_B_rpt(b, dst_sz, src[0], 0);
      dst = ir3_CLZ_B_rpt(b, dst_sz, dst, 0);
      break;
   case nir_op_bitfield_reverse:
      dst = ir3_BFREV_B_rpt(b, dst_sz, src[0], 0);
      break;

   case nir_op_uadd_sat:
      dst = ir3_ADD_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_instr_flags(dst.rpts, dst_sz, IR3_INSTR_SAT);
      break;
   case nir_op_iadd_sat:
      dst = ir3_ADD_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_instr_flags(dst.rpts, dst_sz, IR3_INSTR_SAT);
      break;
   case nir_op_usub_sat:
      dst = ir3_SUB_U_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_instr_flags(dst.rpts, dst_sz, IR3_INSTR_SAT);
      break;
   case nir_op_isub_sat:
      dst = ir3_SUB_S_rpt(b, dst_sz, src[0], 0, src[1], 0);
      set_instr_flags(dst.rpts, dst_sz, IR3_INSTR_SAT);
      break;
   case nir_op_pack_64_2x32_split: {
      dst.rpts[0] = ir3_64b(b, src[0].rpts[0], src[1].rpts[0]);
      break;
   }
   case nir_op_unpack_64_2x32_split_x: {
      dst.rpts[0] = ir3_MOV(b, ir3_64b_get_lo(src[0].rpts[0]), TYPE_U32);
      break;
   }
   case nir_op_unpack_64_2x32_split_y: {
      dst.rpts[0] = ir3_MOV(b, ir3_64b_get_hi(src[0].rpts[0]), TYPE_U32);
      break;
   }
   case nir_op_udot_4x8_uadd:
   case nir_op_udot_4x8_uadd_sat:
   case nir_op_sdot_4x8_iadd:
   case nir_op_sdot_4x8_iadd_sat:
   case nir_op_sudot_4x8_iadd:
   case nir_op_sudot_4x8_iadd_sat: {
      assert(dst_sz == 1);

      struct ir3_instruction *src_rpt0[] = {src[0].rpts[0], src[1].rpts[0],
                                            src[2].rpts[0]};

      if (ctx->compiler->has_dp4acc) {
         emit_alu_dot_4x8_as_dp4acc(ctx, alu, dst.rpts, src_rpt0);
      } else if (ctx->compiler->has_dp2acc) {
         emit_alu_dot_4x8_as_dp2acc(ctx, alu, dst.rpts, src_rpt0);
      } else {
         ir3_context_error(ctx, "ALU op should have been lowered: %s\n",
                           nir_op_infos[alu->op].name);
      }

      break;
   }

   default:
      ir3_context_error(ctx, "Unhandled ALU op: %s\n",
                        nir_op_infos[alu->op].name);
      break;
   }

   if (nir_alu_type_get_base_type(info->output_type) == nir_type_bool) {
      assert(alu->def.bit_size == 1 || alu->op == nir_op_b2b32);
   } else {
      /* 1-bit values stored in 32-bit registers are only valid for certain
       * ALU ops.
       */
      switch (alu->op) {
      case nir_op_mov:
      case nir_op_iand:
      case nir_op_ior:
      case nir_op_ixor:
      case nir_op_inot:
      case nir_op_bcsel:
      case nir_op_andg_ir3:
         break;
      default:
         compile_assert(ctx, alu->def.bit_size != 1);
      }
   }

   cp_instrs(def, dst.rpts, dst_sz);
   ir3_put_def(ctx, &alu->def);
}

static void
emit_intrinsic_load_ubo_ldc(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                            struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;

   /* This is only generated for us by nir_lower_ubo_vec4, which leaves base =
    * 0.
    */
   assert(nir_intrinsic_base(intr) == 0);

   unsigned ncomp = intr->num_components;
   struct ir3_instruction *offset = ir3_get_src(ctx, &intr->src[1])[0];
   struct ir3_instruction *idx = ir3_get_src(ctx, &intr->src[0])[0];
   struct ir3_instruction *ldc = ir3_LDC(b, idx, 0, offset, 0);
   ldc->dsts[0]->wrmask = MASK(ncomp);
   ldc->cat6.iim_val = ncomp;
   ldc->cat6.d = nir_intrinsic_component(intr);
   ldc->cat6.type = utype_def(&intr->def);

   ir3_handle_bindless_cat6(ldc, intr->src[0]);
   if (ldc->flags & IR3_INSTR_B)
      ctx->so->bindless_ubo = true;
   ir3_handle_nonuniform(ldc, intr);

   if (!intr->def.divergent &&
       ctx->compiler->has_scalar_alu) {
      ldc->dsts[0]->flags |= IR3_REG_SHARED;
      ldc->flags |= IR3_INSTR_U;
   }

   ir3_split_dest(b, dst, ldc, 0, ncomp);
}

static void
emit_intrinsic_copy_ubo_to_uniform(struct ir3_context *ctx,
                                   nir_intrinsic_instr *intr)
{
   struct ir3_builder *b = &ctx->build;

   unsigned base = nir_intrinsic_base(intr);
   unsigned size = nir_intrinsic_range(intr);

   struct ir3_instruction *addr1 = ir3_create_addr1(&ctx->build, base);

   struct ir3_instruction *offset = ir3_get_src(ctx, &intr->src[1])[0];
   struct ir3_instruction *idx = ir3_get_src(ctx, &intr->src[0])[0];
   struct ir3_instruction *ldc = ir3_LDC_K(b, idx, 0, offset, 0);
   ldc->cat6.iim_val = size;
   ldc->barrier_class = ldc->barrier_conflict = IR3_BARRIER_CONST_W;

   ir3_handle_bindless_cat6(ldc, intr->src[0]);
   if (ldc->flags & IR3_INSTR_B)
      ctx->so->bindless_ubo = true;

   ir3_instr_set_address(ldc, addr1);

   /* The assembler isn't aware of what value a1.x has, so make sure that
    * constlen includes the ldc.k here.
    */
   ctx->so->constlen =
      MAX2(ctx->so->constlen, DIV_ROUND_UP(base + size * 4, 4));

   array_insert(ctx->block, ctx->block->keeps, ldc);
}

static void
emit_intrinsic_copy_global_to_uniform(struct ir3_context *ctx,
                                      nir_intrinsic_instr *intr)
{
   struct ir3_builder *b = &ctx->build;

   unsigned size = nir_intrinsic_range(intr);
   unsigned dst = nir_intrinsic_range_base(intr);
   unsigned addr_offset = nir_intrinsic_base(intr);
   unsigned dst_lo = dst & 0xff;
   unsigned dst_hi = dst >> 8;

   struct ir3_instruction *a1 = NULL;
   if (dst_hi)
      a1 = ir3_create_addr1(&ctx->build, dst_hi << 8);

   struct ir3_instruction *addr =
      ir3_collect(b, ir3_get_src(ctx, &intr->src[0])[0]);
   struct ir3_instruction *ldg = ir3_LDG_K(b, create_immed(b, dst_lo), 0, addr, 0, 
                                           create_immed(b, addr_offset), 0,
                                           create_immed(b, size), 0);
   ldg->barrier_class = ldg->barrier_conflict = IR3_BARRIER_CONST_W;
   ldg->cat6.type = TYPE_U32;

   if (a1) {
      ir3_instr_set_address(ldg, a1);
      ldg->flags |= IR3_INSTR_A1EN;
   }

   /* The assembler isn't aware of what value a1.x has, so make sure that
    * constlen includes the ldg.k here.
    */
   ctx->so->constlen =
      MAX2(ctx->so->constlen, DIV_ROUND_UP(dst + size * 4, 4));

   array_insert(ctx->block, ctx->block->keeps, ldg);
}


/* handles direct/indirect UBO reads: */
static void
emit_intrinsic_load_ubo(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                        struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *base_lo, *base_hi, *addr, *src0, *src1;
   const struct ir3_const_state *const_state = ir3_const_state(ctx->so);
   unsigned ubo = ir3_const_reg(const_state, IR3_CONST_ALLOC_UBO_PTRS, 0);
   const unsigned ptrsz = ir3_pointer_size(ctx->compiler);

   int off = 0;

   /* First src is ubo index, which could either be an immed or not: */
   src0 = ir3_get_src(ctx, &intr->src[0])[0];
   if (is_same_type_mov(src0) && (src0->srcs[0]->flags & IR3_REG_IMMED)) {
      base_lo = create_uniform(b, ubo + (src0->srcs[0]->iim_val * ptrsz));
      base_hi = create_uniform(b, ubo + (src0->srcs[0]->iim_val * ptrsz) + 1);
   } else {
      base_lo = create_uniform_indirect(b, ubo, TYPE_U32,
                                        ir3_get_addr0(ctx, src0, ptrsz));
      base_hi = create_uniform_indirect(b, ubo + 1, TYPE_U32,
                                        ir3_get_addr0(ctx, src0, ptrsz));

      /* NOTE: since relative addressing is used, make sure constlen is
       * at least big enough to cover all the UBO addresses, since the
       * assembler won't know what the max address reg is.
       */
      ctx->so->constlen = MAX2(
         ctx->so->constlen,
         const_state->allocs.consts[IR3_CONST_ALLOC_UBO_PTRS].offset_vec4 +
            (ctx->s->info.num_ubos * ptrsz));
   }

   /* note: on 32bit gpu's base_hi is ignored and DCE'd */
   addr = base_lo;

   if (nir_src_is_const(intr->src[1])) {
      off += nir_src_as_uint(intr->src[1]);
   } else {
      /* For load_ubo_indirect, second src is indirect offset: */
      src1 = ir3_get_src(ctx, &intr->src[1])[0];

      /* and add offset to addr: */
      addr = ir3_ADD_S(b, addr, 0, src1, 0);
   }

   /* if offset is to large to encode in the ldg, split it out: */
   if ((off + (intr->num_components * 4)) > 1024) {
      /* split out the minimal amount to improve the odds that
       * cp can fit the immediate in the add.s instruction:
       */
      unsigned off2 = off + (intr->num_components * 4) - 1024;
      addr = ir3_ADD_S(b, addr, 0, create_immed(b, off2), 0);
      off -= off2;
   }

   if (ptrsz == 2) {
      struct ir3_instruction *carry;

      /* handle 32b rollover, ie:
       *   if (addr < base_lo)
       *      base_hi++
       */
      carry = ir3_CMPS_U(b, addr, 0, base_lo, 0);
      carry->cat2.condition = IR3_COND_LT;
      base_hi = ir3_ADD_S(b, base_hi, 0, carry, 0);

      addr = ir3_collect(b, addr, base_hi);
   }

   for (int i = 0; i < intr->num_components; i++) {
      struct ir3_instruction *load =
         ir3_LDG(b, addr, 0, create_immed(b, off + i * 4), 0,
                 create_immed(b, 1), 0); /* num components */
      load->cat6.type = TYPE_U32;
      dst[i] = load;
   }
}

/* src[] = { block_index } */
static void
emit_intrinsic_ssbo_size(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                         struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *ibo = ir3_ssbo_to_ibo(ctx, intr->src[0]);
   struct ir3_instruction *resinfo = ir3_RESINFO(b, ibo, 0);
   resinfo->cat6.iim_val = 1;
   resinfo->cat6.d = ctx->compiler->gen >= 6 ? 1 : 2;
   resinfo->cat6.type = TYPE_U32;
   resinfo->cat6.typed = false;
   /* resinfo has no writemask and always writes out 3 components */
   resinfo->dsts[0]->wrmask = MASK(3);
   ir3_handle_bindless_cat6(resinfo, intr->src[0]);
   ir3_handle_nonuniform(resinfo, intr);

   if (ctx->compiler->gen >= 6) {
      ir3_split_dest(b, dst, resinfo, 0, 1);
   } else {
      /* On a5xx, resinfo returns the low 16 bits of ssbo size in .x and the high 16 bits in .y */
      struct ir3_instruction *resinfo_dst[2];
      ir3_split_dest(b, resinfo_dst, resinfo, 0, 2);
      *dst = ir3_ADD_U(b, ir3_SHL_B(b, resinfo_dst[1], 0, create_immed(b, 16), 0), 0, resinfo_dst[0], 0);
   }
}

/* src[] = { offset }. const_index[] = { base } */
static void
emit_intrinsic_load_shared(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                           struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *ldl, *offset;
   unsigned base;

   offset = ir3_get_src(ctx, &intr->src[0])[0];
   base = nir_intrinsic_base(intr);

   ldl = ir3_LDL(b, offset, 0, create_immed(b, base), 0,
                 create_immed(b, intr->num_components), 0);

   ldl->cat6.type = utype_def(&intr->def);
   ldl->dsts[0]->wrmask = MASK(intr->num_components);

   ldl->barrier_class = IR3_BARRIER_SHARED_R;
   ldl->barrier_conflict = IR3_BARRIER_SHARED_W;

   ir3_split_dest(b, dst, ldl, 0, intr->num_components);
}

/* src[] = { value, offset }. const_index[] = { base, write_mask } */
static void
emit_intrinsic_store_shared(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *stl, *offset;
   struct ir3_instruction *const *value;
   unsigned base, wrmask, ncomp;

   value = ir3_get_src(ctx, &intr->src[0]);
   offset = ir3_get_src(ctx, &intr->src[1])[0];

   base = nir_intrinsic_base(intr);
   wrmask = nir_intrinsic_write_mask(intr);
   ncomp = ffs(~wrmask) - 1;

   assert(wrmask == BITFIELD_MASK(intr->num_components));

   stl = ir3_STL(b, offset, 0, ir3_create_collect(b, value, ncomp), 0,
                 create_immed(b, ncomp), 0);
   stl->cat6.dst_offset = base;
   stl->cat6.type = utype_src(intr->src[0]);
   stl->barrier_class = IR3_BARRIER_SHARED_W;
   stl->barrier_conflict = IR3_BARRIER_SHARED_R | IR3_BARRIER_SHARED_W;

   array_insert(ctx->block, ctx->block->keeps, stl);
}

/* src[] = { offset }. const_index[] = { base } */
static void
emit_intrinsic_load_shared_ir3(struct ir3_context *ctx,
                               nir_intrinsic_instr *intr,
                               struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *load, *offset;
   unsigned base;

   offset = ir3_get_src(ctx, &intr->src[0])[0];
   base = nir_intrinsic_base(intr);

   load = ir3_LDLW(b, offset, 0, create_immed(b, base), 0,
                   create_immed(b, intr->num_components), 0);

   /* for a650, use LDL for tess ctrl inputs: */
   if (ctx->so->type == MESA_SHADER_TESS_CTRL && ctx->compiler->tess_use_shared)
      load->opc = OPC_LDL;

   load->cat6.type = utype_def(&intr->def);
   load->dsts[0]->wrmask = MASK(intr->num_components);

   load->barrier_class = IR3_BARRIER_SHARED_R;
   load->barrier_conflict = IR3_BARRIER_SHARED_W;

   ir3_split_dest(b, dst, load, 0, intr->num_components);
}

/* src[] = { value, offset }. const_index[] = { base } */
static void
emit_intrinsic_store_shared_ir3(struct ir3_context *ctx,
                                nir_intrinsic_instr *intr)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *store, *offset;
   struct ir3_instruction *const *value;

   value = ir3_get_src(ctx, &intr->src[0]);
   offset = ir3_get_src(ctx, &intr->src[1])[0];

   store = ir3_STLW(b, offset, 0,
                    ir3_create_collect(b, value, intr->num_components), 0,
                    create_immed(b, intr->num_components), 0);

   /* for a650, use STL for vertex outputs used by tess ctrl shader: */
   if (ctx->so->type == MESA_SHADER_VERTEX && ctx->so->key.tessellation &&
       ctx->compiler->tess_use_shared)
      store->opc = OPC_STL;

   store->cat6.dst_offset = nir_intrinsic_base(intr);
   store->cat6.type = utype_src(intr->src[0]);
   store->barrier_class = IR3_BARRIER_SHARED_W;
   store->barrier_conflict = IR3_BARRIER_SHARED_R | IR3_BARRIER_SHARED_W;

   array_insert(ctx->block, ctx->block->keeps, store);
}

/*
 * CS shared variable atomic intrinsics
 *
 * All of the shared variable atomic memory operations read a value from
 * memory, compute a new value using one of the operations below, write the
 * new value to memory, and return the original value read.
 *
 * All operations take 2 sources except CompSwap that takes 3. These
 * sources represent:
 *
 * 0: The offset into the shared variable storage region that the atomic
 *    operation will operate on.
 * 1: The data parameter to the atomic function (i.e. the value to add
 *    in, etc).
 * 2: For CompSwap only: the second data parameter.
 */
static struct ir3_instruction *
emit_intrinsic_atomic_shared(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *atomic, *src0, *src1;
   type_t type = TYPE_U32;

   src0 = ir3_get_src(ctx, &intr->src[0])[0]; /* offset */
   src1 = ir3_get_src(ctx, &intr->src[1])[0]; /* value */

   switch (nir_intrinsic_atomic_op(intr)) {
   case nir_atomic_op_iadd:
      atomic = ir3_ATOMIC_ADD(b, src0, 0, src1, 0);
      break;
   case nir_atomic_op_imin:
      atomic = ir3_ATOMIC_MIN(b, src0, 0, src1, 0);
      type = TYPE_S32;
      break;
   case nir_atomic_op_umin:
      atomic = ir3_ATOMIC_MIN(b, src0, 0, src1, 0);
      break;
   case nir_atomic_op_imax:
      atomic = ir3_ATOMIC_MAX(b, src0, 0, src1, 0);
      type = TYPE_S32;
      break;
   case nir_atomic_op_umax:
      atomic = ir3_ATOMIC_MAX(b, src0, 0, src1, 0);
      break;
   case nir_atomic_op_iand:
      atomic = ir3_ATOMIC_AND(b, src0, 0, src1, 0);
      break;
   case nir_atomic_op_ior:
      atomic = ir3_ATOMIC_OR(b, src0, 0, src1, 0);
      break;
   case nir_atomic_op_ixor:
      atomic = ir3_ATOMIC_XOR(b, src0, 0, src1, 0);
      break;
   case nir_atomic_op_xchg:
      atomic = ir3_ATOMIC_XCHG(b, src0, 0, src1, 0);
      break;
   case nir_atomic_op_cmpxchg:
      /* for cmpxchg, src1 is [ui]vec2(data, compare): */
      src1 = ir3_collect(b, ir3_get_src(ctx, &intr->src[2])[0], src1);
      atomic = ir3_ATOMIC_CMPXCHG(b, src0, 0, src1, 0);
      break;
   default:
      UNREACHABLE("boo");
   }

   atomic->cat6.iim_val = 1;
   atomic->cat6.d = 1;
   atomic->cat6.type = type;
   atomic->barrier_class = IR3_BARRIER_SHARED_W;
   atomic->barrier_conflict = IR3_BARRIER_SHARED_R | IR3_BARRIER_SHARED_W;

   /* even if nothing consume the result, we can't DCE the instruction: */
   array_insert(ctx->block, ctx->block->keeps, atomic);

   return atomic;
}

static void
stp_ldp_offset(struct ir3_context *ctx, nir_src *src,
               struct ir3_instruction **offset, int32_t *base)
{
   struct ir3_builder *b = &ctx->build;

   if (nir_src_is_const(*src)) {
      unsigned src_offset = nir_src_as_uint(*src);
      /* The base offset field is only 13 bits, and it's signed. Try to make the
       * offset constant whenever the original offsets are similar, to avoid
       * creating too many constants in the final shader.
       */
      *base = ((int32_t) src_offset << (32 - 13)) >> (32 - 13);
      uint32_t offset_val = src_offset - *base;
      *offset = create_immed(b, offset_val);
   } else {
      /* TODO: match on nir_iadd with a constant that fits */
      *base = 0;
      *offset = ir3_get_src(ctx, src)[0];
   }
}

/* src[] = { offset }. */
static void
emit_intrinsic_load_scratch(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                            struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *ldp, *offset;
   int32_t base;

   stp_ldp_offset(ctx, &intr->src[0], &offset, &base);

   ldp = ir3_LDP(b, offset, 0, create_immed(b, base), 0,
                 create_immed(b, intr->num_components), 0);

   ldp->cat6.type = utype_def(&intr->def);
   ldp->dsts[0]->wrmask = MASK(intr->num_components);

   ldp->barrier_class = IR3_BARRIER_PRIVATE_R;
   ldp->barrier_conflict = IR3_BARRIER_PRIVATE_W;

   ir3_split_dest(b, dst, ldp, 0, intr->num_components);
}

/* src[] = { value, offset }. const_index[] = { write_mask } */
static void
emit_intrinsic_store_scratch(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *stp, *offset;
   struct ir3_instruction *const *value;
   unsigned wrmask, ncomp;
   int32_t base;

   value = ir3_get_src(ctx, &intr->src[0]);

   stp_ldp_offset(ctx, &intr->src[1], &offset, &base);

   wrmask = nir_intrinsic_write_mask(intr);
   ncomp = ffs(~wrmask) - 1;

   assert(wrmask == BITFIELD_MASK(intr->num_components));

   stp = ir3_STP(b, offset, 0, ir3_create_collect(b, value, ncomp), 0,
                 create_immed(b, ncomp), 0);
   stp->cat6.dst_offset = base;
   stp->cat6.type = utype_src(intr->src[0]);
   stp->barrier_class = IR3_BARRIER_PRIVATE_W;
   stp->barrier_conflict = IR3_BARRIER_PRIVATE_R | IR3_BARRIER_PRIVATE_W;

   array_insert(ctx->block, ctx->block->keeps, stp);
}

struct tex_src_info {
   /* For prefetch */
   unsigned tex_base, samp_base, tex_idx, samp_idx;
   /* For normal tex instructions */
   unsigned base, a1_val, flags;
   struct ir3_instruction *samp_tex;
};

/* TODO handle actual indirect/dynamic case.. which is going to be weird
 * to handle with the image_mapping table..
 */
static struct tex_src_info
get_image_ssbo_samp_tex_src(struct ir3_context *ctx, nir_src *src, bool image)
{
   struct ir3_builder *b = &ctx->build;
   struct tex_src_info info = {0};
   nir_intrinsic_instr *bindless_tex = ir3_bindless_resource(*src);

   if (bindless_tex) {
      /* Bindless case */
      ctx->so->bindless_tex = true;
      info.flags |= IR3_INSTR_B;

      /* Gather information required to determine which encoding to
       * choose as well as for prefetch.
       */
      info.tex_base = nir_intrinsic_desc_set(bindless_tex);
      bool tex_const = nir_src_is_const(bindless_tex->src[0]);
      if (tex_const)
         info.tex_idx = nir_src_as_uint(bindless_tex->src[0]);
      info.samp_idx = 0;

      /* Choose encoding. */
      if (tex_const && info.tex_idx < 256) {
         if (info.tex_idx < 16) {
            /* Everything fits within the instruction */
            info.base = info.tex_base;
         } else {
            info.base = info.tex_base;
            if (ctx->compiler->gen <= 6) {
               info.a1_val = info.tex_idx << 3;
            } else {
               info.a1_val = info.samp_idx << 3;
            }
            info.flags |= IR3_INSTR_A1EN;
         }
         info.samp_tex = NULL;
      } else {
         info.flags |= IR3_INSTR_S2EN;
         info.base = info.tex_base;

         /* Note: the indirect source is now a vec2 instead of hvec2 */
         struct ir3_instruction *texture, *sampler;

         texture = ir3_get_src(ctx, src)[0];
         sampler = create_immed(b, 0);
         info.samp_tex = ir3_collect(b, texture, sampler);
      }
   } else {
      info.flags |= IR3_INSTR_S2EN;
      unsigned slot = nir_src_as_uint(*src);
      unsigned tex_idx = image ?
            ir3_image_to_tex(&ctx->so->image_mapping, slot) :
            ir3_ssbo_to_tex(&ctx->so->image_mapping, slot);
      struct ir3_instruction *texture, *sampler;

      ctx->so->num_samp = MAX2(ctx->so->num_samp, tex_idx + 1);

      texture = create_immed_typed(b, tex_idx, TYPE_U16);
      sampler = create_immed_typed(b, tex_idx, TYPE_U16);

      info.samp_tex = ir3_collect(b, texture, sampler);
   }

   return info;
}

static struct ir3_instruction *
emit_sam(struct ir3_context *ctx, opc_t opc, struct tex_src_info info,
         type_t type, unsigned wrmask, struct ir3_instruction *src0,
         struct ir3_instruction *src1)
{
   struct ir3_instruction *sam, *addr;
   if (info.flags & IR3_INSTR_A1EN) {
      addr = ir3_create_addr1(&ctx->build, info.a1_val);
   }
   sam = ir3_SAM(&ctx->build, opc, type, wrmask, info.flags, info.samp_tex,
                 src0, src1);
   if (info.flags & IR3_INSTR_A1EN) {
      ir3_instr_set_address(sam, addr);
   }
   if (info.flags & IR3_INSTR_B) {
      sam->cat5.tex_base = info.base;
      sam->cat5.samp = info.samp_idx;
      sam->cat5.tex  = info.tex_idx;
   }
   return sam;
}

/* src[] = { deref, coord, sample_index }. const_index[] = {} */
static void
emit_intrinsic_load_image(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                          struct ir3_instruction **dst)
{
   /* If the image can be written, must use LDIB to retrieve data, rather than
    * through ISAM (which uses the texture cache and won't get previous writes).
    */
   if (!(nir_intrinsic_access(intr) & ACCESS_CAN_REORDER)) {
      ctx->funcs->emit_intrinsic_load_image(ctx, intr, dst);
      return;
   }

   /* The sparse set of texture descriptors for non-coherent load_images means we can't do indirection, so
    * fall back to coherent load.
    */
   if (ctx->compiler->gen >= 5 &&
       !ir3_bindless_resource(intr->src[0]) &&
       !nir_src_is_const(intr->src[0])) {
      ctx->funcs->emit_intrinsic_load_image(ctx, intr, dst);
      return;
   }

   struct ir3_builder *b = &ctx->build;
   struct tex_src_info info = get_image_ssbo_samp_tex_src(ctx, &intr->src[0], true);
   struct ir3_instruction *sam;
   struct ir3_instruction *const *src0 = ir3_get_src(ctx, &intr->src[1]);
   struct ir3_instruction *coords[4];
   unsigned flags, ncoords = ir3_get_image_coords(intr, &flags);
   type_t type = ir3_get_type_for_image_intrinsic(intr);

   info.flags |= flags;

   /* hw doesn't do 1d, so we treat it as 2d with height of 1, and patch up the
    * y coord. Note that the array index must come after the fake y coord.
    */
   enum glsl_sampler_dim dim = nir_intrinsic_image_dim(intr);
   if (dim == GLSL_SAMPLER_DIM_1D || dim == GLSL_SAMPLER_DIM_BUF) {
      coords[0] = src0[0];
      coords[1] = create_immed(b, 0);
      for (unsigned i = 1; i < ncoords; i++)
         coords[i + 1] = src0[i];
      ncoords++;
   } else {
      for (unsigned i = 0; i < ncoords; i++)
         coords[i] = src0[i];
   }

   sam = emit_sam(ctx, OPC_ISAM, info, type, 0b1111,
                  ir3_create_collect(b, coords, ncoords), NULL);

   ir3_handle_nonuniform(sam, intr);

   sam->barrier_class = IR3_BARRIER_IMAGE_R;
   sam->barrier_conflict = IR3_BARRIER_IMAGE_W;

   ir3_split_dest(b, dst, sam, 0, 4);
}

/* A4xx version of image_size, see ir3_a6xx.c for newer resinfo version. */
void
emit_intrinsic_image_size_tex(struct ir3_context *ctx,
                              nir_intrinsic_instr *intr,
                              struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;
   struct tex_src_info info = get_image_ssbo_samp_tex_src(ctx, &intr->src[0], true);
   struct ir3_instruction *sam, *lod;
   unsigned flags, ncoords = ir3_get_image_coords(intr, &flags);
   type_t dst_type = intr->def.bit_size == 16 ? TYPE_U16 : TYPE_U32;

   info.flags |= flags;
   assert(nir_src_as_uint(intr->src[1]) == 0);
   lod = create_immed(b, 0);
   sam = emit_sam(ctx, OPC_GETSIZE, info, dst_type, 0b1111, lod, NULL);

   /* Array size actually ends up in .w rather than .z. This doesn't
    * matter for miplevel 0, but for higher mips the value in z is
    * minified whereas w stays. Also, the value in TEX_CONST_3_DEPTH is
    * returned, which means that we have to add 1 to it for arrays for
    * a3xx.
    *
    * Note use a temporary dst and then copy, since the size of the dst
    * array that is passed in is based on nir's understanding of the
    * result size, not the hardware's
    */
   struct ir3_instruction *tmp[4];

   ir3_split_dest(b, tmp, sam, 0, 4);

   for (unsigned i = 0; i < ncoords; i++)
      dst[i] = tmp[i];

   if (flags & IR3_INSTR_A) {
      if (ctx->compiler->levels_add_one) {
         dst[ncoords - 1] = ir3_ADD_U(b, tmp[3], 0, create_immed(b, 1), 0);
      } else {
         dst[ncoords - 1] = ir3_MOV(b, tmp[3], TYPE_U32);
      }
   }
}

static struct tex_src_info
get_bindless_samp_src(struct ir3_context *ctx, nir_src *tex,
                      nir_src *samp)
{
   struct ir3_builder *b = &ctx->build;
   struct tex_src_info info = {0};

   info.flags |= IR3_INSTR_B;

   /* Gather information required to determine which encoding to
    * choose as well as for prefetch.
    */
   nir_intrinsic_instr *bindless_tex = NULL;
   bool tex_const;
   if (tex) {
      ctx->so->bindless_tex = true;
      bindless_tex = ir3_bindless_resource(*tex);
      assert(bindless_tex);
      info.tex_base = nir_intrinsic_desc_set(bindless_tex);
      tex_const = nir_src_is_const(bindless_tex->src[0]);
      if (tex_const)
         info.tex_idx = nir_src_as_uint(bindless_tex->src[0]);
   } else {
      /* To simplify some of the logic below, assume the index is
       * constant 0 when it's not enabled.
       */
      tex_const = true;
      info.tex_idx = 0;
   }
   nir_intrinsic_instr *bindless_samp = NULL;
   bool samp_const;
   if (samp) {
      ctx->so->bindless_samp = true;
      bindless_samp = ir3_bindless_resource(*samp);
      assert(bindless_samp);
      info.samp_base = nir_intrinsic_desc_set(bindless_samp);
      samp_const = nir_src_is_const(bindless_samp->src[0]);
      if (samp_const)
         info.samp_idx = nir_src_as_uint(bindless_samp->src[0]);
   } else {
      samp_const = true;
      info.samp_idx = 0;
   }

   /* Choose encoding. */
   if (tex_const && samp_const && info.tex_idx < 256 &&
       info.samp_idx < 256) {
      if (info.tex_idx < 16 && info.samp_idx < 16 &&
          (!bindless_tex || !bindless_samp ||
           info.tex_base == info.samp_base)) {
         /* Everything fits within the instruction */
         info.base = info.tex_base;
      } else {
         info.base = info.tex_base;
         if (ctx->compiler->gen <= 6) {
            info.a1_val = info.tex_idx << 3 | info.samp_base;
         } else {
            info.a1_val = info.samp_idx << 3 | info.samp_base;
         }

         info.flags |= IR3_INSTR_A1EN;
      }
      info.samp_tex = NULL;
   } else {
      info.flags |= IR3_INSTR_S2EN;
      /* In the indirect case, we only use a1.x to store the sampler
       * base if it differs from the texture base.
       */
      if (!bindless_tex || !bindless_samp ||
          info.tex_base == info.samp_base) {
         info.base = info.tex_base;
      } else {
         info.base = info.tex_base;
         info.a1_val = info.samp_base;
         info.flags |= IR3_INSTR_A1EN;
      }

      /* Note: the indirect source is now a vec2 instead of hvec2
       */
      struct ir3_instruction *texture, *sampler;

      if (bindless_tex) {
         texture = ir3_get_src(ctx, tex)[0];
      } else {
         texture = create_immed(b, 0);
      }

      if (bindless_samp) {
         sampler = ir3_get_src(ctx, samp)[0];
      } else {
         sampler = create_immed(b, 0);
      }
      info.samp_tex = ir3_collect(b, texture, sampler);
   }

   return info;
}

static void
emit_readonly_load_uav(struct ir3_context *ctx,
                       nir_intrinsic_instr *intr,
                       nir_src *index,
                       struct ir3_instruction *coords,
                       unsigned imm_offset,
                       bool uav_load,
                       struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;
   struct tex_src_info info = get_image_ssbo_samp_tex_src(ctx, index, false);

   struct ir3_instruction *src1;
   if (ctx->compiler->has_isam_v && !uav_load) {
      src1 = create_immed(b, imm_offset);
   } else {
      assert(imm_offset == 0);
      src1 = NULL;
   }

   unsigned num_components = intr->def.num_components;
   struct ir3_instruction *sam =
      emit_sam(ctx, OPC_ISAM, info, utype_for_size(intr->def.bit_size),
               MASK(num_components), coords, src1);

   ir3_handle_nonuniform(sam, intr);

   sam->barrier_class = IR3_BARRIER_BUFFER_R;
   sam->barrier_conflict = IR3_BARRIER_BUFFER_W;

   ir3_split_dest(b, dst, sam, 0, num_components);

   if (ctx->compiler->has_isam_v && !uav_load) {
      sam->flags |= (IR3_INSTR_V | IR3_INSTR_INV_1D);

      if (imm_offset) {
         sam->flags |= IR3_INSTR_IMM_OFFSET;
      }
   }
}

/* src[] = { buffer_index, offset }. No const_index */
static void
emit_intrinsic_load_ssbo(struct ir3_context *ctx,
                         nir_intrinsic_instr *intr,
                         struct ir3_instruction **dst)
{
   /* Note: we can only use isam for vectorized loads/stores if isam.v is
    * available.
    * Note: isam also can't handle 8-bit loads.
    */
   if (!(nir_intrinsic_access(intr) & ACCESS_CAN_REORDER) ||
       (intr->def.num_components > 1 && !ctx->compiler->has_isam_v) ||
       (ctx->compiler->options.storage_8bit && intr->def.bit_size == 8) ||
       !ctx->compiler->has_isam_ssbo) {
      ctx->funcs->emit_intrinsic_load_ssbo(ctx, intr, dst);
      return;
   }

   struct ir3_builder *b = &ctx->build;
   nir_src *offset_src = &intr->src[2];
   struct ir3_instruction *coords = NULL;
   unsigned imm_offset = 0;

   if (ctx->compiler->has_isam_v) {
      ir3_lower_imm_offset(ctx, intr, offset_src, 8, &coords, &imm_offset);
   } else {
      coords =
         ir3_collect(b, ir3_get_src(ctx, offset_src)[0], create_immed(b, 0));
   }

   emit_readonly_load_uav(ctx, intr, &intr->src[0], coords, imm_offset, false, dst);
}

static void
emit_intrinsic_load_uav(struct ir3_context *ctx,
                        nir_intrinsic_instr *intr,
                        struct ir3_instruction **dst)
{
   /* Note: isam currently can't handle vectorized loads/stores */
   if (!(nir_intrinsic_access(intr) & ACCESS_CAN_REORDER) ||
       intr->def.num_components > 1 ||
       !ctx->compiler->has_isam_ssbo) {
      ctx->funcs->emit_intrinsic_load_uav(ctx, intr, dst);
      return;
   }

   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *coords =
      ir3_create_collect(b, ir3_get_src(ctx, &intr->src[1]), 2);
   emit_readonly_load_uav(ctx, intr, &intr->src[0], coords, 0, true, dst);
}

static void
emit_control_barrier(struct ir3_context *ctx)
{
   /* Hull shaders dispatch 32 wide so an entire patch will always
    * fit in a single warp and execute in lock-step. Consequently,
    * we don't need to do anything for TCS barriers. Emitting
    * barrier instruction will deadlock.
    */
   if (ctx->so->type == MESA_SHADER_TESS_CTRL)
      return;

   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *barrier = ir3_BAR(b);
   barrier->cat7.g = true;
   if (ctx->compiler->gen < 6)
      barrier->cat7.l = true;
   barrier->flags = IR3_INSTR_SS | IR3_INSTR_SY;
   barrier->barrier_class = IR3_BARRIER_EVERYTHING;
   array_insert(ctx->block, ctx->block->keeps, barrier);

   ctx->so->has_barrier = true;
}

static void
emit_intrinsic_barrier(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction *barrier;

   /* TODO: find out why there is a major difference of .l usage
    * between a5xx and a6xx,
    */

   mesa_scope exec_scope = nir_intrinsic_execution_scope(intr);
   mesa_scope mem_scope = nir_intrinsic_memory_scope(intr);
   nir_variable_mode modes = nir_intrinsic_memory_modes(intr);
   /* loads/stores are always cache-coherent so we can filter out
    * available/visible.
    */
   nir_memory_semantics semantics =
      nir_intrinsic_memory_semantics(intr) & (NIR_MEMORY_ACQUIRE |
                                              NIR_MEMORY_RELEASE);

   if (ctx->so->type == MESA_SHADER_TESS_CTRL) {
      /* Remove mode corresponding to TCS patch barriers because hull shaders
       * dispatch 32 wide so an entire patch will always fit in a single warp
       * and execute in lock-step.
       *
       * TODO: memory barrier also tells us not to reorder stores, this
       * information is lost here (backend doesn't reorder stores so we
       * are safe for now).
       */
      modes &= ~nir_var_shader_out;
   }

   assert(!(modes & nir_var_shader_out));

   if ((modes & (nir_var_mem_shared | nir_var_mem_ssbo | nir_var_mem_global |
                 nir_var_image)) && semantics) {
      barrier = ir3_FENCE(b);
      barrier->cat7.r = true;
      barrier->cat7.w = true;

      if (modes & (nir_var_mem_ssbo | nir_var_image | nir_var_mem_global)) {
         barrier->cat7.g = true;
      }

      if (ctx->compiler->gen >= 6) {
         if (modes & (nir_var_mem_ssbo | nir_var_image)) {
            barrier->cat7.l = true;
         }
      } else {
         if (modes & (nir_var_mem_shared | nir_var_mem_ssbo | nir_var_image)) {
            barrier->cat7.l = true;
         }
      }

      barrier->barrier_class = 0;
      barrier->barrier_conflict = 0;

      if (modes & nir_var_mem_shared) {
         barrier->barrier_class |= IR3_BARRIER_SHARED_W;
         barrier->barrier_conflict |=
            IR3_BARRIER_SHARED_R | IR3_BARRIER_SHARED_W;
      }

      if (modes & (nir_var_mem_ssbo | nir_var_mem_global)) {
         barrier->barrier_class |= IR3_BARRIER_BUFFER_W;
         barrier->barrier_conflict |=
            IR3_BARRIER_BUFFER_R | IR3_BARRIER_BUFFER_W;
      }

      if (modes & nir_var_image) {
         barrier->barrier_class |= IR3_BARRIER_IMAGE_W;
         barrier->barrier_conflict |=
            IR3_BARRIER_IMAGE_W | IR3_BARRIER_IMAGE_R;
      }

      /* make sure barrier doesn't get DCE'd */
      array_insert(ctx->block, ctx->block->keeps, barrier);

      if (ctx->compiler->gen >= 7 && mem_scope > SCOPE_WORKGROUP &&
          modes & (nir_var_mem_ssbo | nir_var_image) &&
          semantics & NIR_MEMORY_ACQUIRE) {
         /* "r + l" is not enough to synchronize reads with writes from other
          * workgroups, we can disable them since they are useless here.
          */
         barrier->cat7.r = false;
         barrier->cat7.l = false;

         struct ir3_instruction *ccinv = ir3_CCINV(b);
         /* A7XX TODO: ccinv should just stick to the barrier,
          * the barrier class/conflict introduces unnecessary waits.
          */
         ccinv->barrier_class = barrier->barrier_class;
         ccinv->barrier_conflict = barrier->barrier_conflict;
         array_insert(ctx->block, ctx->block->keeps, ccinv);
      }
   }

   if (exec_scope >= SCOPE_WORKGROUP) {
      emit_control_barrier(ctx);
   }
}

static void
add_sysval_input_compmask(struct ir3_context *ctx, gl_system_value slot,
                          unsigned compmask, struct ir3_instruction *instr)
{
   struct ir3_shader_variant *so = ctx->so;
   unsigned n = so->inputs_count++;

   assert(instr->opc == OPC_META_INPUT);
   instr->input.inidx = n;
   instr->input.sysval = slot;

   so->inputs[n].sysval = true;
   so->inputs[n].slot = slot;
   so->inputs[n].compmask = compmask;
   so->total_in++;

   so->sysval_in += util_last_bit(compmask);
}

static struct ir3_instruction *
create_sysval_input(struct ir3_context *ctx, gl_system_value slot,
                    unsigned compmask)
{
   assert(compmask);
   struct ir3_instruction *sysval = create_input(ctx, compmask);
   add_sysval_input_compmask(ctx, slot, compmask, sysval);
   return sysval;
}

static struct ir3_instruction *
get_barycentric(struct ir3_context *ctx, enum ir3_bary bary)
{
   STATIC_ASSERT(SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + IJ_PERSP_PIXEL ==
                 SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL);
   STATIC_ASSERT(SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + IJ_PERSP_SAMPLE ==
                 SYSTEM_VALUE_BARYCENTRIC_PERSP_SAMPLE);
   STATIC_ASSERT(SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + IJ_PERSP_CENTROID ==
                 SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTROID);
   STATIC_ASSERT(SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + IJ_PERSP_CENTER_RHW ==
                 SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTER_RHW);
   STATIC_ASSERT(SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + IJ_LINEAR_PIXEL ==
                 SYSTEM_VALUE_BARYCENTRIC_LINEAR_PIXEL);
   STATIC_ASSERT(SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + IJ_LINEAR_CENTROID ==
                 SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID);
   STATIC_ASSERT(SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + IJ_LINEAR_SAMPLE ==
                 SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE);

   if (!ctx->ij[bary]) {
      struct ir3_instruction *xy[2];
      struct ir3_instruction *ij;
      struct ir3_builder build =
         ir3_builder_at(ir3_before_terminator(ctx->in_block));

      ij = create_sysval_input(ctx, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL +
                               bary, 0x3);
      ir3_split_dest(&build, xy, ij, 0, 2);

      ctx->ij[bary] = ir3_create_collect(&build, xy, 2);
   }

   return ctx->ij[bary];
}

static void
emit_intrinsic_barycentric(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                           struct ir3_instruction **dst)
{
   gl_system_value sysval = ir3_nir_intrinsic_barycentric_sysval(intr);

   if (!ctx->so->key.msaa && ctx->compiler->gen < 6) {
      switch (sysval) {
      case SYSTEM_VALUE_BARYCENTRIC_PERSP_SAMPLE:
         sysval = SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL;
         break;
      case SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTROID:
         sysval = SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL;
         break;
      case SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE:
         sysval = SYSTEM_VALUE_BARYCENTRIC_LINEAR_PIXEL;
         break;
      case SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID:
         sysval = SYSTEM_VALUE_BARYCENTRIC_LINEAR_PIXEL;
         break;
      default:
         break;
      }
   }

   enum ir3_bary bary = sysval - SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL;

   struct ir3_instruction *ij = get_barycentric(ctx, bary);
   ir3_split_dest(&ctx->build, dst, ij, 0, 2);
}

static struct ir3_instruction *
get_frag_coord(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   if (!ctx->frag_coord) {
      struct ir3_block *block = ir3_after_preamble(ctx->ir);
      struct ir3_builder b = ir3_builder_at(ir3_before_terminator(block));
      struct ir3_instruction_rpt xyzw;
      struct ir3_instruction *hw_frag_coord;

      hw_frag_coord = create_sysval_input(ctx, SYSTEM_VALUE_FRAG_COORD, 0xf);
      ir3_split_dest(&b, xyzw.rpts, hw_frag_coord, 0, 4);

      /* for frag_coord.xy, we get unsigned values.. we need
       * to subtract (integer) 8 and divide by 16 (right-
       * shift by 4) then convert to float:
       *
       *    sub.s tmp, src, 8
       *    shr.b tmp, tmp, 4
       *    mov.u32f32 dst, tmp
       *
       */
      struct ir3_instruction_rpt xy =
         ir3_COV_rpt(&b, 2, xyzw, TYPE_U32, TYPE_F32);
      xy = ir3_MUL_F_rpt(&b, 2, xy, 0, create_immed_rpt(&b, 2, fui(1.0 / 16.0)),
                         0);
      cp_instrs(xyzw.rpts, xy.rpts, 2);
      ctx->frag_coord = ir3_create_collect(&b, xyzw.rpts, 4);
   }

   ctx->so->fragcoord_compmask |= nir_def_components_read(&intr->def);

   return ctx->frag_coord;
}

/* This is a bit of a hack until ir3_context is converted to store SSA values
 * as ir3_register's instead of ir3_instruction's. Pick out a given destination
 * of an instruction with multiple destinations using a mov that will get folded
 * away by ir3_cp.
 */
static struct ir3_instruction *
create_multidst_mov(struct ir3_builder *build, struct ir3_register *dst)
{
   struct ir3_instruction *mov = ir3_build_instr(build, OPC_MOV, 1, 1);
   unsigned dst_flags = dst->flags & IR3_REG_HALF;
   unsigned src_flags = dst->flags & (IR3_REG_HALF | IR3_REG_SHARED);

   __ssa_dst(mov)->flags |= dst_flags;
   struct ir3_register *src =
      ir3_src_create(mov, INVALID_REG, IR3_REG_SSA | src_flags);
   src->wrmask = dst->wrmask;
   src->def = dst;
   assert(!(dst->flags & IR3_REG_RELATIV));
   mov->cat1.src_type = mov->cat1.dst_type =
      (dst->flags & IR3_REG_HALF) ? TYPE_U16 : TYPE_U32;
   return mov;
}

static reduce_op_t
get_reduce_op(nir_op opc)
{
   switch (opc) {
   case nir_op_iadd: return REDUCE_OP_ADD_U;
   case nir_op_fadd: return REDUCE_OP_ADD_F;
   case nir_op_imul: return REDUCE_OP_MUL_U;
   case nir_op_fmul: return REDUCE_OP_MUL_F;
   case nir_op_umin: return REDUCE_OP_MIN_U;
   case nir_op_imin: return REDUCE_OP_MIN_S;
   case nir_op_fmin: return REDUCE_OP_MIN_F;
   case nir_op_umax: return REDUCE_OP_MAX_U;
   case nir_op_imax: return REDUCE_OP_MAX_S;
   case nir_op_fmax: return REDUCE_OP_MAX_F;
   case nir_op_iand: return REDUCE_OP_AND_B;
   case nir_op_ior:  return REDUCE_OP_OR_B;
   case nir_op_ixor: return REDUCE_OP_XOR_B;
   default:
      UNREACHABLE("unknown NIR reduce op");
   }
}

static uint32_t
get_reduce_identity(nir_op opc, unsigned size)
{
   switch (opc) {
   case nir_op_iadd:
      return 0;
   case nir_op_fadd: 
      return size == 32 ? fui(0.0f) : _mesa_float_to_half(0.0f);
   case nir_op_imul:
      return 1;
   case nir_op_fmul:
      return size == 32 ? fui(1.0f) : _mesa_float_to_half(1.0f);
   case nir_op_umax:
      return 0;
   case nir_op_imax:
      return size == 32 ? INT32_MIN : (uint32_t)INT16_MIN;
   case nir_op_fmax:
      return size == 32 ? fui(-INFINITY) : _mesa_float_to_half(-INFINITY);
   case nir_op_umin:
      return size == 32 ? UINT32_MAX : UINT16_MAX;
   case nir_op_imin:
      return size == 32 ? INT32_MAX : (uint32_t)INT16_MAX;
   case nir_op_fmin:
      return size == 32 ? fui(INFINITY) : _mesa_float_to_half(INFINITY);
   case nir_op_iand:
      return size == 32 ? ~0 : (size == 16 ? (uint32_t)(uint16_t)~0 : 1);
   case nir_op_ior:
      return 0;
   case nir_op_ixor:
      return 0;
   default:
      UNREACHABLE("unknown NIR reduce op");
   }
}

static struct ir3_instruction *
emit_intrinsic_reduce(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
   nir_op nir_reduce_op = (nir_op) nir_intrinsic_reduction_op(intr);
   reduce_op_t reduce_op = get_reduce_op(nir_reduce_op);
   unsigned dst_size = intr->def.bit_size;
   unsigned flags = (ir3_bitsize(ctx, dst_size) == 16) ? IR3_REG_HALF : 0;

   /* Note: the shared reg is initialized to the identity, so we need it to
    * always be 32-bit even when the source isn't because half shared regs are
    * not supported.
    */
   struct ir3_instruction *identity = create_immed_shared(
      &ctx->build, get_reduce_identity(nir_reduce_op, dst_size), true);

   /* OPC_SCAN_MACRO has the following destinations:
    * - Exclusive scan result (interferes with source)
    * - Inclusive scan result
    * - Shared reg reduction result, must be initialized to the identity
    *
    * The loop computes all three results at the same time, we just have to
    * choose which destination to return.
    */
   struct ir3_instruction *scan =
      ir3_build_instr(&ctx->build, OPC_SCAN_MACRO, 3, 2);
   scan->cat1.reduce_op = reduce_op;

   struct ir3_register *exclusive = __ssa_dst(scan);
   exclusive->flags |= flags | IR3_REG_EARLY_CLOBBER;
   struct ir3_register *inclusive = __ssa_dst(scan);
   inclusive->flags |= flags;
   struct ir3_register *reduce = __ssa_dst(scan);
   reduce->flags |= IR3_REG_SHARED;

   /* The 32-bit multiply macro reads its sources after writing a partial result
    * to the destination, therefore inclusive also interferes with the source.
    */
   if (reduce_op == REDUCE_OP_MUL_U && dst_size == 32)
      inclusive->flags |= IR3_REG_EARLY_CLOBBER;

   /* Normal source */
   __ssa_src(scan, src, 0);

   /* shared reg tied source */
   struct ir3_register *reduce_init = __ssa_src(scan, identity, IR3_REG_SHARED);
   ir3_reg_tie(reduce, reduce_init);
   
   struct ir3_register *dst;
   switch (intr->intrinsic) {
   case nir_intrinsic_reduce: dst = reduce; break;
   case nir_intrinsic_inclusive_scan: dst = inclusive; break;
   case nir_intrinsic_exclusive_scan: dst = exclusive; break;
   default:
      UNREACHABLE("unknown reduce intrinsic");
   }

   return create_multidst_mov(&ctx->build, dst);
}

static struct ir3_instruction *
emit_intrinsic_reduce_clusters(struct ir3_context *ctx,
                               nir_intrinsic_instr *intr)
{
   nir_op nir_reduce_op = (nir_op)nir_intrinsic_reduction_op(intr);
   reduce_op_t reduce_op = get_reduce_op(nir_reduce_op);
   unsigned dst_size = intr->def.bit_size;

   bool need_exclusive =
      intr->intrinsic == nir_intrinsic_exclusive_scan_clusters_ir3;
   bool need_scratch = reduce_op == REDUCE_OP_MUL_U && dst_size == 32;

   /* Note: the shared reg is initialized to the identity, so we need it to
    * always be 32-bit even when the source isn't because half shared regs are
    * not supported.
    */
   struct ir3_instruction *identity = create_immed_shared(
      &ctx->build, get_reduce_identity(nir_reduce_op, dst_size), true);

   struct ir3_instruction *inclusive_src = ir3_get_src(ctx, &intr->src[0])[0];

   struct ir3_instruction *exclusive_src = NULL;
   if (need_exclusive)
         exclusive_src = ir3_get_src(ctx, &intr->src[1])[0];

   /* OPC_SCAN_CLUSTERS_MACRO has the following destinations:
    * - Shared reg reduction result, must be initialized to the identity
    * - Inclusive scan result
    * - (iff exclusive) Exclusive scan result. Conditionally added because
    *   calculating the exclusive value is optional (i.e., not a side-effect of
    *   calculating the inclusive value) and won't be DCE'd anymore at this
    *   point.
    * - (iff 32b mul_u) Scratch register. We try to emit "op rx, ry, rx" for
    *   most ops but this isn't possible for the 32b mul_u macro since its
    *   destination is clobbered. So conditionally allocate an extra
    *   register in that case.
    *
    * Note that the getlast loop this macro expands to iterates over all
    * clusters. However, for each iteration, not only the fibers in the current
    * cluster are active but all later ones as well. Since they still need their
    * sources when their cluster is handled, all destinations interfere with
    * the sources.
    */
   unsigned ndst = 2 + need_exclusive + need_scratch;
   unsigned nsrc = 2 + need_exclusive;
   struct ir3_instruction *scan =
      ir3_build_instr(&ctx->build, OPC_SCAN_CLUSTERS_MACRO, ndst, nsrc);
   scan->cat1.reduce_op = reduce_op;

   unsigned dst_flags = IR3_REG_EARLY_CLOBBER;
   if (ir3_bitsize(ctx, dst_size) == 16)
      dst_flags |= IR3_REG_HALF;

   struct ir3_register *reduce = __ssa_dst(scan);
   reduce->flags |= IR3_REG_SHARED;
   struct ir3_register *inclusive = __ssa_dst(scan);
   inclusive->flags |= dst_flags;

   struct ir3_register *exclusive = NULL;
   if (need_exclusive) {
      exclusive = __ssa_dst(scan);
      exclusive->flags |= dst_flags;
   }

   if (need_scratch) {
      struct ir3_register *scratch = __ssa_dst(scan);
      scratch->flags |= dst_flags;
   }

   struct ir3_register *reduce_init = __ssa_src(scan, identity, IR3_REG_SHARED);
   ir3_reg_tie(reduce, reduce_init);

   __ssa_src(scan, inclusive_src, 0);

   if (need_exclusive)
      __ssa_src(scan, exclusive_src, 0);

   struct ir3_register *dst;
   switch (intr->intrinsic) {
   case nir_intrinsic_reduce_clusters_ir3:
      dst = reduce;
      break;
   case nir_intrinsic_inclusive_scan_clusters_ir3:
      dst = inclusive;
      break;
   case nir_intrinsic_exclusive_scan_clusters_ir3: {
      assert(exclusive != NULL);
      dst = exclusive;
      break;
   }
   default:
      UNREACHABLE("unknown reduce intrinsic");
   }

   return create_multidst_mov(&ctx->build, dst);
}

static struct ir3_instruction *
emit_intrinsic_brcst_active(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   struct ir3_instruction *default_src = ir3_get_src(ctx, &intr->src[0])[0];
   struct ir3_instruction *brcst_val = ir3_get_src(ctx, &intr->src[1])[0];
   return ir3_BRCST_ACTIVE(&ctx->build, nir_intrinsic_cluster_size(intr),
                           brcst_val, default_src);
}

static ir3_shfl_mode
shfl_mode(nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_rotate:
      return SHFL_RDOWN;
   case nir_intrinsic_shuffle_up_uniform_ir3:
      return SHFL_RUP;
   case nir_intrinsic_shuffle_down_uniform_ir3:
      return SHFL_RDOWN;
   case nir_intrinsic_shuffle_xor_uniform_ir3:
      return SHFL_XOR;
   default:
      UNREACHABLE("unsupported shfl");
   }
}

static struct ir3_instruction *
emit_shfl(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   assert(ctx->compiler->has_shfl);

   struct ir3_instruction *val = ir3_get_src(ctx, &intr->src[0])[0];
   struct ir3_instruction *idx = ir3_get_src(ctx, &intr->src[1])[0];

   struct ir3_instruction *shfl = ir3_SHFL(&ctx->build, val, 0, idx, 0);
   shfl->cat6.shfl_mode = shfl_mode(intr);
   shfl->cat6.type = is_half(val) ? TYPE_U16 : TYPE_U32;

   return shfl;
}

static void
emit_ray_intersection(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                      struct ir3_instruction **dst)
{
   struct ir3_builder *b = &ctx->build;

   ctx->so->info.uses_ray_intersection = true;

   struct ir3_instruction *bvh_base =
      ir3_create_collect(b, ir3_get_src(ctx, &intr->src[0]), 2);
   struct ir3_instruction *idx = ir3_get_src(ctx, &intr->src[1])[0];

   struct ir3_instruction *ray_info =
      ir3_create_collect(b, ir3_get_src(ctx, &intr->src[2]), 8);
   struct ir3_instruction *flags = ir3_get_src(ctx, &intr->src[3])[0];

   struct ir3_instruction *dst_init =
      ir3_collect(b, NULL, NULL, NULL, create_immed(b, 0), NULL);

   struct ir3_instruction *ray_intersection =
      ir3_RAY_INTERSECTION(b, bvh_base, 0, idx, 0, ray_info, 0, flags, 0,
                           dst_init, 0);
   ray_intersection->dsts[0]->wrmask = MASK(5);
   ir3_reg_tie(ray_intersection->dsts[0], ray_intersection->srcs[4]);

   ir3_split_dest(b, dst, ray_intersection, 0, 5);
}

static void setup_input(struct ir3_context *ctx, nir_intrinsic_instr *intr);
static void setup_output(struct ir3_context *ctx, nir_intrinsic_instr *intr);

static struct ir3_instruction *
apply_mov_half_shared_quirk(struct ir3_context *ctx,
                            struct ir3_instruction *src,
                            struct ir3_instruction *dst)
{
   if (!ctx->compiler->mov_half_shared_quirk) {
      return dst;
   }

   /* Work around a bug with half-register non-shared -> shared moves by
    * adding an extra mov here so that the original destination stays full.
    */
   if (src->dsts[0]->flags & IR3_REG_HALF) {
      if (dst->opc == OPC_MOVS) {
         /* For movs, we have to fix up its dst_type and then convert back to
          * its original dst_type. Note that this might generate movs.u8u32
          * which doesn't work correctly, but since we convert back using
          * cov.u32u8, the end result will be correct.
          */
         type_t dst_type = dst->cat1.dst_type;
         assert(type_uint(dst_type));

         dst->cat1.dst_type = TYPE_U32;
         dst->dsts[0]->flags &= ~IR3_REG_HALF;
         dst = ir3_COV(&ctx->build, dst, dst->cat1.dst_type, dst_type);
      } else {
         dst = ir3_MOV(&ctx->build, dst, TYPE_U32);
      }
      if (!ctx->compiler->has_scalar_alu)
         dst->dsts[0]->flags &= ~IR3_REG_SHARED;
   }

   return dst;
}

static void
make_dst_dummy(struct ir3_instruction *instr)
{
   assert(instr->dsts_count == 1);

   struct ir3_register *dst = instr->dsts[0];
   dst->flags &= ~IR3_REG_SSA;
   dst->flags |= IR3_REG_DUMMY;
   dst->num = INVALID_REG;
}

static void
emit_intrinsic(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->intrinsic];
   struct ir3_instruction **dst;
   struct ir3_instruction *const *src;
   struct ir3_builder *b = &ctx->build;
   unsigned dest_components = nir_intrinsic_dest_components(intr);
   int idx;
   bool create_rpt = false;

   if (info->has_dest) {
      dst = ir3_get_def(ctx, &intr->def, dest_components);
   } else {
      dst = NULL;
   }

   const struct ir3_const_state *const_state = ir3_const_state(ctx->so);
   const unsigned primitive_param =
      const_state->allocs.consts[IR3_CONST_ALLOC_PRIMITIVE_PARAM].offset_vec4 * 4;
   const unsigned primitive_map =
      const_state->allocs.consts[IR3_CONST_ALLOC_PRIMITIVE_MAP].offset_vec4 * 4;

   switch (intr->intrinsic) {
   case nir_intrinsic_decl_reg:
      /* There's logically nothing to do, but this has a destination in NIR so
       * plug in something... It will get DCE'd.
       */
      dst[0] = create_immed(b, 0);
      break;

   case nir_intrinsic_load_reg:
   case nir_intrinsic_load_reg_indirect: {
      struct ir3_array *arr = ir3_get_array(ctx, intr->src[0].ssa);
      struct ir3_instruction *addr = NULL;

      if (intr->intrinsic == nir_intrinsic_load_reg_indirect) {
         addr = ir3_get_addr0(ctx, ir3_get_src(ctx, &intr->src[1])[0],
                              dest_components);
      }

      ASSERTED nir_intrinsic_instr *decl = nir_reg_get_decl(intr->src[0].ssa);
      assert(dest_components == nir_intrinsic_num_components(decl));

      for (unsigned i = 0; i < dest_components; i++) {
         unsigned n = nir_intrinsic_base(intr) * dest_components + i;
         compile_assert(ctx, n < arr->length);
         dst[i] = ir3_create_array_load(ctx, arr, n, addr);
      }

      break;
   }

   case nir_intrinsic_store_reg:
   case nir_intrinsic_store_reg_indirect: {
      struct ir3_array *arr = ir3_get_array(ctx, intr->src[1].ssa);
      unsigned num_components = nir_src_num_components(intr->src[0]);
      struct ir3_instruction *addr = NULL;

      ASSERTED nir_intrinsic_instr *decl = nir_reg_get_decl(intr->src[1].ssa);
      assert(num_components == nir_intrinsic_num_components(decl));

      struct ir3_instruction *const *value = ir3_get_src(ctx, &intr->src[0]);

      if (intr->intrinsic == nir_intrinsic_store_reg_indirect) {
         addr = ir3_get_addr0(ctx, ir3_get_src(ctx, &intr->src[2])[0],
                              num_components);
      }

      u_foreach_bit(i, nir_intrinsic_write_mask(intr)) {
         assert(i < num_components);

         unsigned n = nir_intrinsic_base(intr) * num_components + i;
         compile_assert(ctx, n < arr->length);
         if (value[i])
            ir3_create_array_store(ctx, arr, n, value[i], addr);
      }

      break;
   }

   case nir_intrinsic_load_const_ir3:
      idx = nir_intrinsic_base(intr);
      if (nir_src_is_const(intr->src[0])) {
         idx += nir_src_as_uint(intr->src[0]);
         for (int i = 0; i < dest_components; i++) {
            dst[i] = create_uniform_typed(
               b, idx + i,
               intr->def.bit_size == 16 ? TYPE_F16 : TYPE_F32);
         }
         create_rpt = true;
      } else {
         src = ctx->compiler->has_scalar_alu ?
            ir3_get_src_maybe_shared(ctx, &intr->src[0]) : 
            ir3_get_src(ctx, &intr->src[0]);
         for (int i = 0; i < dest_components; i++) {
            dst[i] = create_uniform_indirect(
               b, idx + i,
               intr->def.bit_size == 16 ? TYPE_F16 : TYPE_F32,
               ir3_get_addr0(ctx, src[0], 1));
            /* Since this may not be foldable into conversions into shared
             * registers, manually make it shared. Optimizations can undo this if
             * the user can't use shared regs.
             */
            if (ctx->compiler->has_scalar_alu && !intr->def.divergent)
               dst[i]->dsts[0]->flags |= IR3_REG_SHARED;
         }

         ctx->has_relative_load_const_ir3 = true;
      }
      break;

   case nir_intrinsic_load_vs_primitive_stride_ir3:
      dst[0] = create_uniform(b, primitive_param + 0);
      break;
   case nir_intrinsic_load_vs_vertex_stride_ir3:
      dst[0] = create_uniform(b, primitive_param + 1);
      break;
   case nir_intrinsic_load_hs_patch_stride_ir3:
      dst[0] = create_uniform(b, primitive_param + 2);
      break;
   case nir_intrinsic_load_patch_vertices_in:
      dst[0] = create_uniform(b, primitive_param + 3);
      break;
   case nir_intrinsic_load_tess_param_base_ir3:
      dst[0] = create_uniform(b, primitive_param + 4);
      dst[1] = create_uniform(b, primitive_param + 5);
      break;
   case nir_intrinsic_load_tess_factor_base_ir3:
      dst[0] = create_uniform(b, primitive_param + 6);
      dst[1] = create_uniform(b, primitive_param + 7);
      break;

   case nir_intrinsic_load_primitive_location_ir3:
      idx = nir_intrinsic_driver_location(intr);
      dst[0] = create_uniform(b, primitive_map + idx);
      break;

   case nir_intrinsic_load_gs_header_ir3:
      dst[0] = ctx->gs_header;
      break;
   case nir_intrinsic_load_tcs_header_ir3:
      dst[0] = ctx->tcs_header;
      break;

   case nir_intrinsic_load_rel_patch_id_ir3:
      dst[0] = ctx->rel_patch_id;
      break;

   case nir_intrinsic_load_primitive_id:
      if (!ctx->primitive_id) {
         ctx->primitive_id =
            create_sysval_input(ctx, SYSTEM_VALUE_PRIMITIVE_ID, 0x1);
      }
      dst[0] = ctx->primitive_id;
      break;

   case nir_intrinsic_load_tess_coord_xy:
      if (!ctx->tess_coord) {
         ctx->tess_coord =
            create_sysval_input(ctx, SYSTEM_VALUE_TESS_COORD, 0x3);
      }
      ir3_split_dest(b, dst, ctx->tess_coord, 0, 2);
      break;

   case nir_intrinsic_store_global_ir3:
      ctx->funcs->emit_intrinsic_store_global_ir3(ctx, intr);
      break;
   case nir_intrinsic_load_global_ir3:
      ctx->funcs->emit_intrinsic_load_global_ir3(ctx, intr, dst);
      break;

   case nir_intrinsic_load_ubo:
      emit_intrinsic_load_ubo(ctx, intr, dst);
      break;
   case nir_intrinsic_load_ubo_vec4:
      emit_intrinsic_load_ubo_ldc(ctx, intr, dst);
      break;
   case nir_intrinsic_copy_ubo_to_uniform_ir3:
      emit_intrinsic_copy_ubo_to_uniform(ctx, intr);
      break;
   case nir_intrinsic_copy_global_to_uniform_ir3:
      emit_intrinsic_copy_global_to_uniform(ctx, intr);
      break;
   case nir_intrinsic_load_frag_coord:
   case nir_intrinsic_load_frag_coord_unscaled_ir3:
      ir3_split_dest(b, dst, get_frag_coord(ctx, intr), 0, 4);
      break;
   case nir_intrinsic_load_sample_pos_from_id: {
      /* NOTE: blob seems to always use TYPE_F16 and then cov.f16f32,
       * but that doesn't seem necessary.
       */
      struct ir3_instruction *offset =
         ir3_RGETPOS(b, ir3_get_src(ctx, &intr->src[0])[0], 0);
      offset->dsts[0]->wrmask = 0x3;
      offset->cat5.type = TYPE_F32;

      ir3_split_dest(b, dst, offset, 0, 2);

      break;
   }
   case nir_intrinsic_load_persp_center_rhw_ir3:
      if (!ctx->ij[IJ_PERSP_CENTER_RHW]) {
         ctx->ij[IJ_PERSP_CENTER_RHW] =
            create_sysval_input(ctx, SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTER_RHW, 0x1);
      }
      dst[0] = ctx->ij[IJ_PERSP_CENTER_RHW];
      break;
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_barycentric_pixel:
      emit_intrinsic_barycentric(ctx, intr, dst);
      break;
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_input:
      setup_input(ctx, intr);
      break;
   /* All SSBO intrinsics should have been lowered by 'lower_io_offsets'
    * pass and replaced by an ir3-specifc version that adds the
    * dword-offset in the last source.
    */
   case nir_intrinsic_load_ssbo_ir3:
      emit_intrinsic_load_ssbo(ctx, intr, dst);
      break;
   case nir_intrinsic_load_uav_ir3:
      emit_intrinsic_load_uav(ctx, intr, dst);
      break;
   case nir_intrinsic_store_ssbo_ir3:
      ctx->funcs->emit_intrinsic_store_ssbo(ctx, intr);
      break;
   case nir_intrinsic_get_ssbo_size:
      emit_intrinsic_ssbo_size(ctx, intr, dst);
      break;
   case nir_intrinsic_ssbo_atomic_ir3:
   case nir_intrinsic_ssbo_atomic_swap_ir3:
      dst[0] = ctx->funcs->emit_intrinsic_atomic_ssbo(ctx, intr);
      break;
   case nir_intrinsic_load_shared:
      emit_intrinsic_load_shared(ctx, intr, dst);
      break;
   case nir_intrinsic_store_shared:
      emit_intrinsic_store_shared(ctx, intr);
      break;
   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap:
      dst[0] = emit_intrinsic_atomic_shared(ctx, intr);
      break;
   case nir_intrinsic_load_scratch:
      emit_intrinsic_load_scratch(ctx, intr, dst);
      break;
   case nir_intrinsic_store_scratch:
      emit_intrinsic_store_scratch(ctx, intr);
      break;
   case nir_intrinsic_image_load:
   case nir_intrinsic_bindless_image_load:
      emit_intrinsic_load_image(ctx, intr, dst);
      break;
   case nir_intrinsic_image_store:
   case nir_intrinsic_bindless_image_store:
      ctx->funcs->emit_intrinsic_store_image(ctx, intr);
      break;
   case nir_intrinsic_image_size:
   case nir_intrinsic_bindless_image_size:
      ctx->funcs->emit_intrinsic_image_size(ctx, intr, dst);
      break;
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_image_atomic_swap:
   case nir_intrinsic_bindless_image_atomic_swap:
      dst[0] = ctx->funcs->emit_intrinsic_atomic_image(ctx, intr);
      break;
   case nir_intrinsic_barrier:
      emit_intrinsic_barrier(ctx, intr);
      /* note that blk ptr no longer valid, make that obvious: */
      b = NULL;
      break;
   case nir_intrinsic_store_output:
   case nir_intrinsic_store_per_view_output:
      setup_output(ctx, intr);
      break;
   case nir_intrinsic_load_base_vertex:
   case nir_intrinsic_load_first_vertex:
      if (!ctx->basevertex) {
         ctx->basevertex = create_driver_param(ctx, IR3_DP_VS(vtxid_base));
      }
      dst[0] = ctx->basevertex;
      break;
   case nir_intrinsic_load_is_indexed_draw:
      if (!ctx->is_indexed_draw) {
         ctx->is_indexed_draw = create_driver_param(ctx, IR3_DP_VS(is_indexed_draw));
      }
      dst[0] = ctx->is_indexed_draw;
      break;
   case nir_intrinsic_load_draw_id:
      if (!ctx->draw_id) {
         ctx->draw_id = create_driver_param(ctx, IR3_DP_VS(draw_id));
      }
      dst[0] = ctx->draw_id;
      break;
   case nir_intrinsic_load_base_instance:
      if (!ctx->base_instance) {
         ctx->base_instance = create_driver_param(ctx, IR3_DP_VS(instid_base));
      }
      dst[0] = ctx->base_instance;
      break;
   case nir_intrinsic_load_view_index:
      if (!ctx->view_index) {
         ctx->view_index =
            create_sysval_input(ctx, SYSTEM_VALUE_VIEW_INDEX, 0x1);
      }
      dst[0] = ctx->view_index;
      break;
   case nir_intrinsic_load_vertex_id_zero_base:
   case nir_intrinsic_load_vertex_id:
      if (!ctx->vertex_id) {
         gl_system_value sv = (intr->intrinsic == nir_intrinsic_load_vertex_id)
                                 ? SYSTEM_VALUE_VERTEX_ID
                                 : SYSTEM_VALUE_VERTEX_ID_ZERO_BASE;
         ctx->vertex_id = create_sysval_input(ctx, sv, 0x1);
      }
      dst[0] = ctx->vertex_id;
      break;
   case nir_intrinsic_load_instance_id:
      if (!ctx->instance_id) {
         ctx->instance_id =
            create_sysval_input(ctx, SYSTEM_VALUE_INSTANCE_ID, 0x1);
      }
      dst[0] = ctx->instance_id;
      break;
   case nir_intrinsic_load_sample_id:
      if (!ctx->samp_id) {
         ctx->samp_id = create_sysval_input(ctx, SYSTEM_VALUE_SAMPLE_ID, 0x1);
         ctx->samp_id->dsts[0]->flags |= IR3_REG_HALF;
      }
      dst[0] = ir3_COV(b, ctx->samp_id, TYPE_U16, TYPE_U32);
      break;
   case nir_intrinsic_load_sample_mask_in:
      if (!ctx->samp_mask_in) {
         ctx->so->reads_smask = true;
         ctx->samp_mask_in =
            create_sysval_input(ctx, SYSTEM_VALUE_SAMPLE_MASK_IN, 0x1);
      }
      dst[0] = ctx->samp_mask_in;
      break;
   case nir_intrinsic_load_user_clip_plane:
      idx = nir_intrinsic_ucp_id(intr);
      for (int i = 0; i < dest_components; i++) {
         unsigned n = idx * 4 + i;
         dst[i] = create_driver_param(ctx, IR3_DP_VS(ucp[0].x) + n);
      }
      create_rpt = true;
      break;
   case nir_intrinsic_load_front_face:
      if (!ctx->frag_face) {
         ctx->so->frag_face = true;
         ctx->frag_face =
            create_sysval_input(ctx, SYSTEM_VALUE_FRONT_FACE, 0x1);
         ctx->frag_face->dsts[0]->flags |= IR3_REG_HALF;
      }
      /* for fragface, we get -1 for back and 0 for front. However this is
       * the inverse of what nir expects (where ~0 is true).
       */
      dst[0] = ir3_CMPS_S(b, ctx->frag_face, 0,
                          create_immed_typed(b, 0, TYPE_U16), 0);
      dst[0]->cat2.condition = IR3_COND_EQ;
      break;
   case nir_intrinsic_load_local_invocation_id:
      if (!ctx->local_invocation_id) {
         ctx->local_invocation_id =
            create_sysval_input(ctx, SYSTEM_VALUE_LOCAL_INVOCATION_ID, 0x7);
      }
      ir3_split_dest(b, dst, ctx->local_invocation_id, 0, 3);
      break;
   case nir_intrinsic_load_workgroup_id:
      if (ctx->compiler->has_shared_regfile) {
         if (!ctx->work_group_id) {
            ctx->work_group_id =
               create_sysval_input(ctx, SYSTEM_VALUE_WORKGROUP_ID, 0x7);
            ctx->work_group_id->dsts[0]->flags |= IR3_REG_SHARED;
         }
         ir3_split_dest(b, dst, ctx->work_group_id, 0, 3);
      } else {
         /* For a3xx/a4xx, this comes in via const injection by the hw */
         for (int i = 0; i < dest_components; i++) {
            dst[i] = create_driver_param(ctx, IR3_DP_CS(workgroup_id_x) + i);
         }
      }
      break;
   case nir_intrinsic_load_frag_shading_rate: {
      if (!ctx->frag_shading_rate) {
         ctx->so->reads_shading_rate = true;
         ctx->frag_shading_rate =
            create_sysval_input(ctx, SYSTEM_VALUE_FRAG_SHADING_RATE, 0x1);
      }
      dst[0] = ctx->frag_shading_rate;
      break;
   }
   case nir_intrinsic_load_base_workgroup_id:
      for (int i = 0; i < dest_components; i++) {
         dst[i] = create_driver_param(ctx, IR3_DP_CS(base_group_x) + i);
      }
      create_rpt = true;
      break;
   case nir_intrinsic_load_num_workgroups:
      for (int i = 0; i < dest_components; i++) {
         dst[i] = create_driver_param(ctx, IR3_DP_CS(num_work_groups_x) + i);
      }
      create_rpt = true;
      break;
   case nir_intrinsic_load_workgroup_size:
      for (int i = 0; i < dest_components; i++) {
         dst[i] = create_driver_param(ctx, IR3_DP_CS(local_group_size_x) + i);
      }
      create_rpt = true;
      break;
   case nir_intrinsic_load_subgroup_size: {
      assert(ctx->so->type == MESA_SHADER_COMPUTE ||
             ctx->so->type == MESA_SHADER_FRAGMENT);
      unsigned size = ctx->so->type == MESA_SHADER_COMPUTE ?
         IR3_DP_CS(subgroup_size) : IR3_DP_FS(subgroup_size);
      dst[0] = create_driver_param(ctx, size);
      break;
   }
   case nir_intrinsic_load_subgroup_id_shift_ir3:
      dst[0] = create_driver_param(ctx, IR3_DP_CS(subgroup_id_shift));
      break;
   case nir_intrinsic_load_work_dim:
      dst[0] = create_driver_param(ctx, IR3_DP_CS(work_dim));
      break;
   case nir_intrinsic_load_subgroup_invocation:
      assert(ctx->compiler->has_getfiberid);
      dst[0] = ir3_GETFIBERID(b);
      dst[0]->cat6.type = TYPE_U32;
      __ssa_dst(dst[0]);
      break;
   case nir_intrinsic_load_tess_level_outer_default:
      for (int i = 0; i < dest_components; i++) {
         dst[i] = create_driver_param(ctx, IR3_DP_TCS(default_outer_level_x) + i);
      }
      create_rpt = true;
      break;
   case nir_intrinsic_load_tess_level_inner_default:
      for (int i = 0; i < dest_components; i++) {
         dst[i] = create_driver_param(ctx, IR3_DP_TCS(default_inner_level_x) + i);
      }
      create_rpt = true;
      break;
   case nir_intrinsic_load_frag_invocation_count:
      dst[0] = create_driver_param(ctx, IR3_DP_FS(frag_invocation_count));
      break;
   case nir_intrinsic_load_frag_size_ir3:
   case nir_intrinsic_load_frag_offset_ir3: {
      unsigned param =
         intr->intrinsic == nir_intrinsic_load_frag_size_ir3 ?
         IR3_DP_FS(frag_size) : IR3_DP_FS(frag_offset);
      if (nir_src_is_const(intr->src[0])) {
         uint32_t view = nir_src_as_uint(intr->src[0]);
         for (int i = 0; i < dest_components; i++) {
            dst[i] = create_driver_param(ctx, param + 4 * view + i);
         }
         create_rpt = true;
      } else {
         struct ir3_instruction *view = ir3_get_src(ctx, &intr->src[0])[0];
         for (int i = 0; i < dest_components; i++) {
            dst[i] = create_driver_param_indirect(ctx, param + i,
                                                  ir3_get_addr0(ctx, view, 4));
         }
         ctx->so->constlen =
            MAX2(ctx->so->constlen,
                 const_state->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4 +
                    param / 4 + nir_intrinsic_range(intr));
      }
      break;
   }
   case nir_intrinsic_demote:
   case nir_intrinsic_demote_if:
   case nir_intrinsic_terminate:
   case nir_intrinsic_terminate_if: {
      struct ir3_instruction *cond, *kill;

      if (intr->intrinsic == nir_intrinsic_demote_if ||
          intr->intrinsic == nir_intrinsic_terminate_if) {
         /* conditional discard: */
         src = ir3_get_src(ctx, &intr->src[0]);
         cond = src[0];
      } else {
         /* unconditional discard: */
         cond = create_immed_typed(b, 1, ctx->compiler->bool_type);
      }

      /* NOTE: only cmps.*.* can write p0.x: */
      struct ir3_instruction *zero =
            create_immed_typed(b, 0, is_half(cond) ? TYPE_U16 : TYPE_U32);
      cond = ir3_CMPS_S(b, cond, 0, zero, 0);
      cond->cat2.condition = IR3_COND_NE;

      /* condition always goes in predicate register: */
      cond->dsts[0]->flags |= IR3_REG_PREDICATE;

      if (intr->intrinsic == nir_intrinsic_demote ||
          intr->intrinsic == nir_intrinsic_demote_if) {
         kill = ir3_DEMOTE(b, cond, 0);
      } else {
         kill = ir3_KILL(b, cond, 0);
      }

      /* - Side-effects should not be moved on a different side of the kill
       * - Instructions that depend on active fibers should not be reordered
       */
      kill->barrier_class = IR3_BARRIER_IMAGE_W | IR3_BARRIER_BUFFER_W |
                            IR3_BARRIER_ACTIVE_FIBERS_W;
      kill->barrier_conflict = IR3_BARRIER_IMAGE_W | IR3_BARRIER_BUFFER_W |
                               IR3_BARRIER_ACTIVE_FIBERS_R;
      kill->srcs[0]->flags |= IR3_REG_PREDICATE;

      array_insert(ctx->block, ctx->block->keeps, kill);
      ctx->so->has_kill = true;

      break;
   }

   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      struct ir3_instruction *pred = ir3_get_predicate(ctx, src);
      if (intr->intrinsic == nir_intrinsic_vote_any)
         dst[0] = ir3_ANY_MACRO(b, pred, 0);
      else
         dst[0] = ir3_ALL_MACRO(b, pred, 0);
      dst[0]->srcs[0]->flags |= IR3_REG_PREDICATE;
      break;
   }
   case nir_intrinsic_elect:
      dst[0] = ir3_ELECT_MACRO(b);
      dst[0]->flags |= IR3_INSTR_NEEDS_HELPERS;
      break;
   case nir_intrinsic_elect_any_ir3:
      dst[0] = ir3_ELECT_MACRO(b);
      break;
   case nir_intrinsic_preamble_start_ir3:
      dst[0] = ir3_SHPS_MACRO(b);
      break;

   case nir_intrinsic_read_invocation_cond_ir3: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      struct ir3_instruction *cond = ir3_get_src(ctx, &intr->src[1])[0];
      dst[0] = ir3_READ_COND_MACRO(b, ir3_get_predicate(ctx, cond), 0, src, 0);
      dst[0]->dsts[0]->flags |= IR3_REG_SHARED;
      dst[0]->srcs[0]->flags |= IR3_REG_PREDICATE;
      dst[0] = apply_mov_half_shared_quirk(ctx, src, dst[0]);
      break;
   }

   case nir_intrinsic_read_invocation: {
      struct ir3_instruction *const *srcs = ir3_get_src(ctx, &intr->src[0]);
      nir_src *nir_invocation = &intr->src[1];
      struct ir3_instruction *invocation = ir3_get_src(ctx, nir_invocation)[0];

      if (!nir_src_is_const(*nir_invocation)) {
         invocation = ir3_get_addr0(ctx, invocation, 1);
      }

      for (unsigned i = 0; i < intr->def.num_components; i++) {
         dst[i] = ir3_MOVS(b, srcs[i], invocation,
                           type_uint_size(intr->def.bit_size));
         dst[i] = apply_mov_half_shared_quirk(ctx, srcs[i], dst[i]);
      }

      create_rpt = true;
      break;
   }

   case nir_intrinsic_read_first_invocation: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_READ_FIRST_MACRO(b, src, 0);
      dst[0]->dsts[0]->flags |= IR3_REG_SHARED;
      dst[0] = apply_mov_half_shared_quirk(ctx, src, dst[0]);
      break;
   }

   case nir_intrinsic_read_getlast_ir3: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_READ_GETLAST_MACRO(b, src, 0);
      dst[0]->dsts[0]->flags |= IR3_REG_SHARED;
      dst[0] = apply_mov_half_shared_quirk(ctx, src, dst[0]);
      break;
   }

   case nir_intrinsic_ballot: {
      struct ir3_instruction *ballot;
      unsigned components = intr->def.num_components;
      if (nir_src_is_const(intr->src[0]) && nir_src_as_bool(intr->src[0])) {
         /* ballot(true) is just MOVMSK */
         ballot = ir3_MOVMSK(b, components);
      } else {
         struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
         struct ir3_instruction *pred = ir3_get_predicate(ctx, src);
         ballot = ir3_BALLOT_MACRO(b, pred, components);
         ballot->srcs[0]->flags |= IR3_REG_PREDICATE;
      }

      ballot->barrier_class = IR3_BARRIER_ACTIVE_FIBERS_R;
      ballot->barrier_conflict = IR3_BARRIER_ACTIVE_FIBERS_W;

      ir3_split_dest(b, dst, ballot, 0, components);
      break;
   }

   case nir_intrinsic_quad_broadcast: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      struct ir3_instruction *idx = ir3_get_src(ctx, &intr->src[1])[0];

      type_t dst_type = type_uint_size(intr->def.bit_size);

      if (dst_type != TYPE_U32)
         idx = ir3_COV(b, idx, TYPE_U32, dst_type);

      dst[0] = ir3_QUAD_SHUFFLE_BRCST(b, src, 0, idx, 0);
      dst[0]->cat5.type = dst_type;
      break;
   }

   case nir_intrinsic_quad_swap_horizontal: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_QUAD_SHUFFLE_HORIZ(b, src, 0);
      dst[0]->cat5.type = type_uint_size(intr->def.bit_size);
      break;
   }

   case nir_intrinsic_quad_swap_vertical: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_QUAD_SHUFFLE_VERT(b, src, 0);
      dst[0]->cat5.type = type_uint_size(intr->def.bit_size);
      break;
   }

   case nir_intrinsic_quad_swap_diagonal: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_QUAD_SHUFFLE_DIAG(b, src, 0);
      dst[0]->cat5.type = type_uint_size(intr->def.bit_size);
      break;
   }
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_coarse: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_DSX(b, src, 0);
      dst[0]->cat5.type = TYPE_F32;
      break;
   }
   case nir_intrinsic_ddx_fine: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_DSXPP_MACRO(b, src, 0);
      dst[0]->cat5.type = TYPE_F32;
      break;
   }
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_coarse: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_DSY(b, src, 0);
      dst[0]->cat5.type = TYPE_F32;
      break;
   }
   case nir_intrinsic_ddy_fine: {
      struct ir3_instruction *src = ir3_get_src(ctx, &intr->src[0])[0];
      dst[0] = ir3_DSYPP_MACRO(b, src, 0);
      dst[0]->cat5.type = TYPE_F32;
      break;
   }
   case nir_intrinsic_load_shared_ir3:
      emit_intrinsic_load_shared_ir3(ctx, intr, dst);
      break;
   case nir_intrinsic_store_shared_ir3:
      emit_intrinsic_store_shared_ir3(ctx, intr);
      break;
   case nir_intrinsic_bindless_resource_ir3:
      dst[0] = ir3_get_src(ctx, &intr->src[0])[0];
      break;
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap: {
      dst[0] = ctx->funcs->emit_intrinsic_atomic_global(ctx, intr);
      break;
   }

   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan:
      dst[0] = emit_intrinsic_reduce(ctx, intr);
      break;

   case nir_intrinsic_reduce_clusters_ir3:
   case nir_intrinsic_inclusive_scan_clusters_ir3:
   case nir_intrinsic_exclusive_scan_clusters_ir3:
      dst[0] = emit_intrinsic_reduce_clusters(ctx, intr);
      break;

   case nir_intrinsic_brcst_active_ir3:
      dst[0] = emit_intrinsic_brcst_active(ctx, intr);
      break;

   case nir_intrinsic_preamble_end_ir3: {
      ir3_SHPE(b);
      break;
   }
   case nir_intrinsic_store_const_ir3: {
      unsigned components = nir_src_num_components(intr->src[0]);
      unsigned dst = nir_intrinsic_base(intr);

      struct ir3_instruction *src =
         ir3_create_collect(b, ir3_get_src_shared(ctx, &intr->src[0],
                                                  ctx->compiler->has_scalar_alu),
                            components);
      ir3_store_const(ctx->so, b, src, dst);
      break;
   }
   case nir_intrinsic_copy_push_const_to_uniform_ir3: {
      struct ir3_instruction *load =
         ir3_build_instr(b, OPC_PUSH_CONSTS_LOAD_MACRO, 0, 0);
      array_insert(ctx->block, ctx->block->keeps, load);

      load->push_consts.dst_base = nir_src_as_uint(intr->src[0]);
      load->push_consts.src_base = nir_intrinsic_base(intr);
      load->push_consts.src_size = nir_intrinsic_range(intr);

      ctx->so->constlen =
         MAX2(ctx->so->constlen,
              DIV_ROUND_UP(
                 load->push_consts.dst_base + load->push_consts.src_size, 4));
      break;
   }
   case nir_intrinsic_prefetch_sam_ir3: {
      struct tex_src_info info =
         get_bindless_samp_src(ctx, &intr->src[0], &intr->src[1]);
      struct ir3_instruction *sam =
         emit_sam(ctx, OPC_SAM, info, TYPE_F32, 0b1111, NULL, NULL);

      make_dst_dummy(sam);
      array_insert(ctx->block, ctx->block->keeps, sam);
      break;
   }
   case nir_intrinsic_prefetch_tex_ir3: {
      struct ir3_instruction *idx = ir3_get_src(ctx, &intr->src[0])[0];
      struct ir3_instruction *resinfo = ir3_RESINFO(b, idx, 0);
      resinfo->cat6.iim_val = 1;
      resinfo->cat6.d = 1;
      resinfo->cat6.type = TYPE_U32;
      resinfo->cat6.typed = false;

      ir3_handle_bindless_cat6(resinfo, intr->src[0]);
      if (resinfo->flags & IR3_INSTR_B)
         ctx->so->bindless_tex = true;

      make_dst_dummy(resinfo);
      array_insert(ctx->block, ctx->block->keeps, resinfo);
      break;
   }
   case nir_intrinsic_prefetch_ubo_ir3: {
      struct ir3_instruction *offset = create_immed(b, 0);
      struct ir3_instruction *idx = ir3_get_src(ctx, &intr->src[0])[0];
      struct ir3_instruction *ldc = ir3_LDC(b, idx, 0, offset, 0);
      ldc->cat6.iim_val = 1;
      ldc->cat6.type = TYPE_U32;

      ir3_handle_bindless_cat6(ldc, intr->src[0]);
      if (ldc->flags & IR3_INSTR_B)
         ctx->so->bindless_ubo = true;

      make_dst_dummy(ldc);
      array_insert(ctx->block, ctx->block->keeps, ldc);
      break;
   }
   case nir_intrinsic_rotate:
   case nir_intrinsic_shuffle_up_uniform_ir3:
   case nir_intrinsic_shuffle_down_uniform_ir3:
   case nir_intrinsic_shuffle_xor_uniform_ir3:
      dst[0] = emit_shfl(ctx, intr);
      break;
   case nir_intrinsic_ray_intersection_ir3:
      emit_ray_intersection(ctx, intr, dst);
      break;
   default:
      ir3_context_error(ctx, "Unhandled intrinsic type: %s\n",
                        nir_intrinsic_infos[intr->intrinsic].name);
      break;
   }

   if (info->has_dest) {
      if (create_rpt)
         ir3_instr_create_rpt(dst, dest_components);
      ir3_put_def(ctx, &intr->def);
   }
}

static void
emit_load_const(struct ir3_context *ctx, nir_load_const_instr *instr)
{
   unsigned bit_size = ir3_bitsize(ctx, instr->def.bit_size);
   struct ir3_instruction **dst =
      ir3_get_dst_ssa(ctx, &instr->def, instr->def.num_components);

   if (bit_size <= 8) {
      for (int i = 0; i < instr->def.num_components; i++)
         dst[i] = create_immed_typed(&ctx->build, instr->value[i].u8, TYPE_U8);
   } else if (bit_size <= 16) {
      for (int i = 0; i < instr->def.num_components; i++)
         dst[i] =
            create_immed_typed(&ctx->build, instr->value[i].u16, TYPE_U16);
   } else if (bit_size <= 32) {
      for (int i = 0; i < instr->def.num_components; i++)
         dst[i] =
            create_immed_typed(&ctx->build, instr->value[i].u32, TYPE_U32);
   } else {
      assert(instr->def.num_components == 1);
      for (int i = 0; i < instr->def.num_components; i++) {
         dst[i] = ir3_64b_immed(&ctx->build, instr->value[i].u64);
      }
   }
}

static void
emit_undef(struct ir3_context *ctx, nir_undef_instr *undef)
{
   struct ir3_instruction **dst =
      ir3_get_dst_ssa(ctx, &undef->def, undef->def.num_components);
   type_t type = utype_for_size(ir3_bitsize(ctx, undef->def.bit_size));

   /* backend doesn't want undefined instructions, so just plug
    * in 0.0..
    */
   for (int i = 0; i < undef->def.num_components; i++)
      dst[i] = create_immed_typed(&ctx->build, fui(0.0), type);
}

/*
 * texture fetch/sample instructions:
 */

static type_t
get_tex_dest_type(nir_tex_instr *tex)
{
   type_t type;

   switch (tex->dest_type) {
   case nir_type_float32:
      return TYPE_F32;
   case nir_type_float16:
      return TYPE_F16;
   case nir_type_int32:
      return TYPE_S32;
   case nir_type_int16:
      return TYPE_S16;
   case nir_type_bool32:
   case nir_type_uint32:
      return TYPE_U32;
   case nir_type_bool16:
   case nir_type_uint16:
      return TYPE_U16;
   case nir_type_invalid:
   default:
      UNREACHABLE("bad dest_type");
   }

   return type;
}

static void
tex_info(nir_tex_instr *tex, unsigned *flagsp, unsigned *coordsp)
{
   unsigned coords =
      glsl_get_sampler_dim_coordinate_components(tex->sampler_dim);
   unsigned flags = 0;

   /* note: would use tex->coord_components.. except txs.. also,
    * since array index goes after shadow ref, we don't want to
    * count it:
    */
   if (coords == 3)
      flags |= IR3_INSTR_3D;

   if (tex->is_shadow && tex->op != nir_texop_lod)
      flags |= IR3_INSTR_S;

   if (tex->is_array && tex->op != nir_texop_lod)
      flags |= IR3_INSTR_A;

   *flagsp = flags;
   *coordsp = coords;
}

/* Gets the sampler/texture idx as a hvec2.  Which could either be dynamic
 * or immediate (in which case it will get lowered later to a non .s2en
 * version of the tex instruction which encode tex/samp as immediates:
 */
static struct tex_src_info
get_tex_samp_tex_src(struct ir3_context *ctx, nir_tex_instr *tex)
{
   struct ir3_builder *b = &ctx->build;
   struct tex_src_info info = {0};
   int texture_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_handle);
   int sampler_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_handle);
   struct ir3_instruction *texture, *sampler;

   if (texture_idx >= 0 || sampler_idx >= 0) {
      /* Bindless case */
      info = get_bindless_samp_src(ctx,
                                   texture_idx >= 0 ? &tex->src[texture_idx].src : NULL,
                                   sampler_idx >= 0 ? &tex->src[sampler_idx].src : NULL);

      if (tex->texture_non_uniform || tex->sampler_non_uniform)
         info.flags |= IR3_INSTR_NONUNIF;
   } else {
      info.flags |= IR3_INSTR_S2EN;
      texture_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_offset);
      sampler_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset);
      if (texture_idx >= 0) {
         texture = ir3_get_src(ctx, &tex->src[texture_idx].src)[0];
         texture = ir3_COV(b, texture, TYPE_U32, TYPE_U16);
      } else {
         /* TODO what to do for dynamic case? I guess we only need the
          * max index for astc srgb workaround so maybe not a problem
          * to worry about if we don't enable indirect samplers for
          * a4xx?
          */
         ctx->max_texture_index =
            MAX2(ctx->max_texture_index, tex->texture_index);
         texture = create_immed_typed(b, tex->texture_index, TYPE_U16);
         info.tex_idx = tex->texture_index;
      }

      if (sampler_idx >= 0) {
         sampler = ir3_get_src(ctx, &tex->src[sampler_idx].src)[0];
         sampler = ir3_COV(b, sampler, TYPE_U32, TYPE_U16);
      } else {
         sampler = create_immed_typed(b, tex->sampler_index, TYPE_U16);
         info.samp_idx = tex->texture_index;
      }

      info.samp_tex = ir3_collect(b, texture, sampler);
   }

   return info;
}

static void
emit_tex(struct ir3_context *ctx, nir_tex_instr *tex)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction **dst, *sam, *src0[12], *src1[4];
   struct ir3_instruction *const *coord, *const *off, *const *ddx, *const *ddy;
   struct ir3_instruction *lod, *compare, *proj, *sample_index;
   struct tex_src_info info = {0};
   bool has_bias = false, has_lod = false, has_proj = false, has_off = false;
   unsigned i, coords, flags, ncomp;
   unsigned nsrc0 = 0, nsrc1 = 0;
   type_t type;
   opc_t opc = 0;

   ncomp = tex->def.num_components;

   coord = off = ddx = ddy = NULL;
   lod = proj = compare = sample_index = NULL;

   dst = ir3_get_def(ctx, &tex->def, ncomp);

   for (unsigned i = 0; i < tex->num_srcs; i++) {
      switch (tex->src[i].src_type) {
      case nir_tex_src_coord:
         coord = ir3_get_src(ctx, &tex->src[i].src);
         break;
      case nir_tex_src_bias:
         lod = ir3_get_src(ctx, &tex->src[i].src)[0];
         has_bias = true;
         break;
      case nir_tex_src_lod:
         lod = ir3_get_src(ctx, &tex->src[i].src)[0];
         has_lod = true;
         break;
      case nir_tex_src_comparator: /* shadow comparator */
         compare = ir3_get_src(ctx, &tex->src[i].src)[0];
         break;
      case nir_tex_src_projector:
         proj = ir3_get_src(ctx, &tex->src[i].src)[0];
         has_proj = true;
         break;
      case nir_tex_src_offset:
         off = ir3_get_src(ctx, &tex->src[i].src);
         has_off = true;
         break;
      case nir_tex_src_ddx:
         ddx = ir3_get_src(ctx, &tex->src[i].src);
         break;
      case nir_tex_src_ddy:
         ddy = ir3_get_src(ctx, &tex->src[i].src);
         break;
      case nir_tex_src_ms_index:
         sample_index = ir3_get_src(ctx, &tex->src[i].src)[0];
         break;
      case nir_tex_src_texture_offset:
      case nir_tex_src_sampler_offset:
      case nir_tex_src_texture_handle:
      case nir_tex_src_sampler_handle:
         /* handled in get_tex_samp_src() */
         break;
      default:
         ir3_context_error(ctx, "Unhandled NIR tex src type: %d\n",
                           tex->src[i].src_type);
         return;
      }
   }

   switch (tex->op) {
   case nir_texop_tex_prefetch:
      compile_assert(ctx, !has_bias);
      compile_assert(ctx, !has_lod);
      compile_assert(ctx, !compare);
      compile_assert(ctx, !has_proj);
      compile_assert(ctx, !has_off);
      compile_assert(ctx, !ddx);
      compile_assert(ctx, !ddy);
      compile_assert(ctx, !sample_index);
      compile_assert(
         ctx, nir_tex_instr_src_index(tex, nir_tex_src_texture_offset) < 0);
      compile_assert(
         ctx, nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset) < 0);

      if (ctx->so->num_sampler_prefetch < ctx->prefetch_limit) {
         opc = OPC_META_TEX_PREFETCH;
         ctx->so->num_sampler_prefetch++;
         break;
      }
      FALLTHROUGH;
   case nir_texop_tex:
      opc = has_lod ? OPC_SAML : OPC_SAM;
      break;
   case nir_texop_txb:
      opc = OPC_SAMB;
      break;
   case nir_texop_txl:
      opc = OPC_SAML;
      break;
   case nir_texop_txd:
      opc = OPC_SAMGQ;
      break;
   case nir_texop_txf:
      opc = OPC_ISAML;
      break;
   case nir_texop_lod:
      opc = OPC_GETLOD;
      break;
   case nir_texop_tg4:
      switch (tex->component) {
      case 0:
         opc = OPC_GATHER4R;
         break;
      case 1:
         opc = OPC_GATHER4G;
         break;
      case 2:
         opc = OPC_GATHER4B;
         break;
      case 3:
         opc = OPC_GATHER4A;
         break;
      }
      break;
   case nir_texop_txf_ms_fb:
   case nir_texop_txf_ms:
      opc = OPC_ISAMM;
      break;
   default:
      ir3_context_error(ctx, "Unhandled NIR tex type: %d\n", tex->op);
      return;
   }

   tex_info(tex, &flags, &coords);

   /*
    * lay out the first argument in the proper order:
    *  - actual coordinates first
    *  - shadow reference
    *  - array index
    *  - projection w
    *  - starting at offset 4, dpdx.xy, dpdy.xy
    *
    * bias/lod go into the second arg
    */

   /* insert tex coords: */
   for (i = 0; i < coords; i++)
      src0[i] = coord[i];

   nsrc0 = i;

   type_t coord_pad_type = is_half(coord[0]) ? TYPE_U16 : TYPE_U32;
   /* scale up integer coords for TXF based on the LOD */
   if (ctx->compiler->unminify_coords && (opc == OPC_ISAML)) {
      assert(has_lod);
      for (i = 0; i < coords; i++)
         src0[i] = ir3_SHL_B(b, src0[i], 0, lod, 0);
   }

   if (coords == 1) {
      /* hw doesn't do 1d, so we treat it as 2d with
       * height of 1, and patch up the y coord.
       */
      if (is_isam(opc)) {
         src0[nsrc0++] = create_immed_typed(b, 0, coord_pad_type);
      } else if (is_half(coord[0])) {
         src0[nsrc0++] = create_immed_typed(b, _mesa_float_to_half(0.5), coord_pad_type);
      } else {
         src0[nsrc0++] = create_immed_typed(b, fui(0.5), coord_pad_type);
      }
   }

   if (tex->is_shadow && tex->op != nir_texop_lod)
      src0[nsrc0++] = compare;

   if (tex->is_array && tex->op != nir_texop_lod)
      src0[nsrc0++] = coord[coords];

   if (has_proj) {
      src0[nsrc0++] = proj;
      flags |= IR3_INSTR_P;
   }

   /* pad to 4, then ddx/ddy: */
   if (tex->op == nir_texop_txd) {
      while (nsrc0 < 4)
         src0[nsrc0++] = create_immed_typed(b, fui(0.0), coord_pad_type);
      for (i = 0; i < coords; i++)
         src0[nsrc0++] = ddx[i];
      if (coords < 2)
         src0[nsrc0++] = create_immed_typed(b, fui(0.0), coord_pad_type);
      for (i = 0; i < coords; i++)
         src0[nsrc0++] = ddy[i];
      if (coords < 2)
         src0[nsrc0++] = create_immed_typed(b, fui(0.0), coord_pad_type);
   }

   /* NOTE a3xx (and possibly a4xx?) might be different, using isaml
    * with scaled x coord according to requested sample:
    */
   if (opc == OPC_ISAMM) {
      if (ctx->compiler->txf_ms_with_isaml) {
         /* the samples are laid out in x dimension as
          *     0 1 2 3
          * x_ms = (x << ms) + sample_index;
          */
         struct ir3_instruction *ms;
         ms = create_immed(b, (ctx->samples >> (2 * tex->texture_index)) & 3);

         src0[0] = ir3_SHL_B(b, src0[0], 0, ms, 0);
         src0[0] = ir3_ADD_U(b, src0[0], 0, sample_index, 0);

         opc = OPC_ISAML;
      } else {
         src0[nsrc0++] = sample_index;
      }
   }

   /*
    * second argument (if applicable):
    *  - offsets
    *  - lod
    *  - bias
    */
   if (has_off | has_lod | has_bias) {
      if (has_off) {
         unsigned off_coords = coords;
         if (tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE)
            off_coords--;
         for (i = 0; i < off_coords; i++)
            src1[nsrc1++] = off[i];
         if (off_coords < 2)
            src1[nsrc1++] = create_immed_typed(b, fui(0.0), coord_pad_type);
         flags |= IR3_INSTR_O;
      }

      if (has_lod | has_bias)
         src1[nsrc1++] = lod;
   }

   type = get_tex_dest_type(tex);

   if (opc == OPC_GETLOD)
      type = TYPE_S32;

   if (tex->op == nir_texop_txf_ms_fb) {
      compile_assert(ctx, ctx->so->type == MESA_SHADER_FRAGMENT);

      ctx->so->fb_read = true;
      if (ctx->compiler->options.bindless_fb_read_descriptor >= 0) {
         ctx->so->bindless_tex = true;
         info.flags = IR3_INSTR_B;
         info.base = ctx->compiler->options.bindless_fb_read_descriptor;
         struct ir3_instruction *texture, *sampler;

         int base_index =
            nir_tex_instr_src_index(tex, nir_tex_src_texture_handle);
         nir_src tex_src = tex->src[base_index].src;

         if (nir_src_is_const(tex_src)) {
            texture = create_immed_typed(b,
               nir_src_as_uint(tex_src) + ctx->compiler->options.bindless_fb_read_slot,
               TYPE_U32);
         } else {
            texture = create_immed_typed(
               b, ctx->compiler->options.bindless_fb_read_slot, TYPE_U32);
            struct ir3_instruction *base =
               ir3_get_src(ctx, &tex->src[base_index].src)[0];
            texture = ir3_ADD_U(b, texture, 0, base, 0);
         }
         sampler = create_immed_typed(b, 0, TYPE_U32);
         info.samp_tex = ir3_collect(b, texture, sampler);
         info.flags |= IR3_INSTR_S2EN;
         if (tex->texture_non_uniform) {
            info.flags |= IR3_INSTR_NONUNIF;
         }
      } else {
         /* Otherwise append a sampler to be patched into the texture
          * state:
          */
         info.samp_tex =
            ir3_collect(b, create_immed_typed(b, ctx->so->num_samp, TYPE_U16),
                        create_immed_typed(b, ctx->so->num_samp, TYPE_U16));
         info.flags = IR3_INSTR_S2EN;
      }

      ctx->so->num_samp++;
   } else {
      info = get_tex_samp_tex_src(ctx, tex);
   }

   bool tg4_swizzle_fixup = false;
   if (tex->op == nir_texop_tg4 && ctx->compiler->gen == 4 &&
         ctx->sampler_swizzles[tex->texture_index] != 0x688 /* rgba */) {
      uint16_t swizzles = ctx->sampler_swizzles[tex->texture_index];
      uint16_t swizzle = (swizzles >> (tex->component * 3)) & 7;
      if (swizzle > 3) {
         /* this would mean that we can just return 0 / 1, no texturing
          * necessary
          */
         struct ir3_instruction *imm = create_immed(b,
               type_float(type) ? fui(swizzle - 4) : (swizzle - 4));
         for (int i = 0; i < 4; i++)
            dst[i] = imm;
         ir3_put_def(ctx, &tex->def);
         return;
      }
      opc = OPC_GATHER4R + swizzle;
      tg4_swizzle_fixup = true;
   }

   struct ir3_instruction *col0 = ir3_create_collect(b, src0, nsrc0);
   struct ir3_instruction *col1 = ir3_create_collect(b, src1, nsrc1);

   if (opc == OPC_META_TEX_PREFETCH) {
      int idx = nir_tex_instr_src_index(tex, nir_tex_src_coord);

      struct ir3_builder build =
         ir3_builder_at(ir3_before_terminator(ctx->in_block));
      sam = ir3_SAM(&build, opc, type, MASK(ncomp), 0, NULL,
                    get_barycentric(ctx, IJ_PERSP_PIXEL), 0);
      sam->prefetch.input_offset = ir3_nir_coord_offset(tex->src[idx].src.ssa, NULL);
      /* make sure not to add irrelevant flags like S2EN */
      sam->flags = flags | (info.flags & IR3_INSTR_B);
      sam->prefetch.tex = info.tex_idx;
      sam->prefetch.samp = info.samp_idx;
      sam->prefetch.tex_base = info.tex_base;
      sam->prefetch.samp_base = info.samp_base;
   } else {
      info.flags |= flags;
      sam = emit_sam(ctx, opc, info, type, MASK(ncomp), col0, col1);
   }

   if (tg4_swizzle_fixup) {
      /* TODO: fix-up for ASTC when alpha is selected? */
      array_insert(ctx->ir, ctx->ir->tg4, sam);

      ir3_split_dest(b, dst, sam, 0, 4);

      uint8_t tex_bits = ctx->sampler_swizzles[tex->texture_index] >> 12;
      if (!type_float(type) && tex_bits != 3 /* 32bpp */ &&
            tex_bits != 0 /* key unset */) {
         uint8_t bits = 0;
         switch (tex_bits) {
         case 1: /* 8bpp */
            bits = 8;
            break;
         case 2: /* 16bpp */
            bits = 16;
            break;
         case 4: /* 10bpp or 2bpp for alpha */
            if (opc == OPC_GATHER4A)
               bits = 2;
            else
               bits = 10;
            break;
         default:
            assert(0);
         }

         sam->cat5.type = TYPE_F32;
         for (int i = 0; i < 4; i++) {
            /* scale and offset the unorm data */
            dst[i] = ir3_MAD_F32(b, dst[i], 0, create_immed(b, fui((1 << bits) - 1)), 0, create_immed(b, fui(0.5f)), 0);
            /* convert the scaled value to integer */
            dst[i] = ir3_COV(b, dst[i], TYPE_F32, TYPE_U32);
            /* sign extend for signed values */
            if (type == TYPE_S32) {
               dst[i] = ir3_SHL_B(b, dst[i], 0, create_immed(b, 32 - bits), 0);
               dst[i] = ir3_ASHR_B(b, dst[i], 0, create_immed(b, 32 - bits), 0);
            }
         }
      }
   } else if ((ctx->astc_srgb & (1 << tex->texture_index)) &&
       tex->op != nir_texop_tg4 && /* leave out tg4, unless it's on alpha? */
       !nir_tex_instr_is_query(tex)) {
      assert(opc != OPC_META_TEX_PREFETCH);

      /* only need first 3 components: */
      sam->dsts[0]->wrmask = 0x7;
      ir3_split_dest(b, dst, sam, 0, 3);

      /* we need to sample the alpha separately with a non-SRGB
       * texture state:
       */
      sam = ir3_SAM(b, opc, type, 0b1000, flags | info.flags, info.samp_tex,
                    col0, col1);

      array_insert(ctx->ir, ctx->ir->astc_srgb, sam);

      /* fixup .w component: */
      ir3_split_dest(b, &dst[3], sam, 3, 1);
   } else {
      /* normal (non-workaround) case: */
      ir3_split_dest(b, dst, sam, 0, ncomp);
   }

   /* GETLOD returns results in 4.8 fixed point */
   if (opc == OPC_GETLOD) {
      bool half = tex->def.bit_size == 16;
      struct ir3_instruction *factor =
         half ? create_immed_typed(b, _mesa_float_to_half(1.0 / 256), TYPE_F16)
              : create_immed(b, fui(1.0 / 256));

      for (i = 0; i < 2; i++) {
         dst[i] = ir3_MUL_F(
            b, ir3_COV(b, dst[i], TYPE_S32, half ? TYPE_F16 : TYPE_F32), 0,
            factor, 0);
      }
   }

   ir3_put_def(ctx, &tex->def);
}

static void
emit_tex_info(struct ir3_context *ctx, nir_tex_instr *tex, unsigned idx)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction **dst, *sam;
   type_t dst_type = get_tex_dest_type(tex);
   struct tex_src_info info = get_tex_samp_tex_src(ctx, tex);

   dst = ir3_get_def(ctx, &tex->def, 1);

   sam = emit_sam(ctx, OPC_GETINFO, info, dst_type, 1 << idx, NULL, NULL);

   /* even though there is only one component, since it ends
    * up in .y/.z/.w rather than .x, we need a split_dest()
    */
   ir3_split_dest(b, dst, sam, idx, 1);

   /* The # of levels comes from getinfo.z. We need to add 1 to it, since
    * the value in TEX_CONST_0 is zero-based.
    */
   if (ctx->compiler->levels_add_one)
      dst[0] = ir3_ADD_U(b, dst[0], 0, create_immed(b, 1), 0);

   ir3_put_def(ctx, &tex->def);
}

static void
emit_tex_txs(struct ir3_context *ctx, nir_tex_instr *tex)
{
   struct ir3_builder *b = &ctx->build;
   struct ir3_instruction **dst, *sam;
   struct ir3_instruction *lod;
   unsigned flags, coords;
   type_t dst_type = get_tex_dest_type(tex);
   struct tex_src_info info = get_tex_samp_tex_src(ctx, tex);

   tex_info(tex, &flags, &coords);
   info.flags |= flags;

   /* Actually we want the number of dimensions, not coordinates. This
    * distinction only matters for cubes.
    */
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE)
      coords = 2;

   dst = ir3_get_def(ctx, &tex->def, 4);

   int lod_idx = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   compile_assert(ctx, lod_idx >= 0);

   lod = ir3_get_src(ctx, &tex->src[lod_idx].src)[0];

   if (tex->sampler_dim != GLSL_SAMPLER_DIM_BUF) {
      sam = emit_sam(ctx, OPC_GETSIZE, info, dst_type, 0b1111, lod, NULL);
   } else {
      /*
       * The maximum value which OPC_GETSIZE could return for one dimension
       * is 0x007ff0, however sampler buffer could be much bigger.
       * Blob uses OPC_GETBUF for them.
       */
      sam = emit_sam(ctx, OPC_GETBUF, info, dst_type, 0b1111, NULL, NULL);
   }

   ir3_split_dest(b, dst, sam, 0, 4);

   /* Array size actually ends up in .w rather than .z. This doesn't
    * matter for miplevel 0, but for higher mips the value in z is
    * minified whereas w stays. Also, the value in TEX_CONST_3_DEPTH is
    * returned, which means that we have to add 1 to it for arrays.
    */
   if (tex->is_array) {
      if (ctx->compiler->levels_add_one) {
         dst[coords] = ir3_ADD_U(b, dst[3], 0, create_immed(b, 1), 0);
      } else {
         dst[coords] = ir3_MOV(b, dst[3], TYPE_U32);
      }
   }

   ir3_put_def(ctx, &tex->def);
}

/* phi instructions are left partially constructed.  We don't resolve
 * their srcs until the end of the shader, since (eg. loops) one of
 * the phi's srcs might be defined after the phi due to back edges in
 * the CFG.
 */
static void
emit_phi(struct ir3_context *ctx, nir_phi_instr *nphi)
{
   struct ir3_instruction *phi, **dst;

   unsigned num_components = nphi->def.num_components;
   dst = ir3_get_def(ctx, &nphi->def, num_components);

   if (exec_list_is_singular(&nphi->srcs)) {
      nir_phi_src *src = list_entry(exec_list_get_head(&nphi->srcs),
                                    nir_phi_src, node);
      if (nphi->def.divergent == src->src.ssa->divergent) {
         struct ir3_instruction *const *srcs =
            ir3_get_src_maybe_shared(ctx, &src->src);
         memcpy(dst, srcs, num_components * sizeof(struct ir3_instruction *));
         ir3_put_def(ctx, &nphi->def);
         return;
      }
   }

   for (unsigned i = 0; i < num_components; i++) {
      phi = ir3_build_instr(&ctx->build, OPC_META_PHI, 1,
                            exec_list_length(&nphi->srcs));
      __ssa_dst(phi);
      phi->phi.nphi = nphi;
      phi->phi.comp = i;

      if (ctx->compiler->has_scalar_alu && !nphi->def.divergent)
         phi->dsts[0]->flags |= IR3_REG_SHARED;

      dst[i] = phi;
   }

   ir3_put_def(ctx, &nphi->def);
}

static struct ir3_block *get_block(struct ir3_context *ctx,
                                   const nir_block *nblock);

static struct ir3_instruction *
read_phi_src(struct ir3_context *ctx, struct ir3_block *blk,
             struct ir3_instruction *phi, nir_phi_instr *nphi)
{
   if (!blk->nblock) {
      struct ir3_builder build = ir3_builder_at(ir3_before_terminator(blk));
      struct ir3_instruction *continue_phi =
         ir3_build_instr(&build, OPC_META_PHI, 1, blk->predecessors_count);
      __ssa_dst(continue_phi)->flags = phi->dsts[0]->flags;

      for (unsigned i = 0; i < blk->predecessors_count; i++) {
         struct ir3_instruction *src =
            read_phi_src(ctx, blk->predecessors[i], phi, nphi);
         if (src)
            __ssa_src(continue_phi, src, 0);
         else
            ir3_src_create(continue_phi, INVALID_REG, phi->dsts[0]->flags);
      }

      return continue_phi;
   }

   nir_foreach_phi_src (nsrc, nphi) {
      if (blk->nblock == nsrc->pred) {
         if (nsrc->src.ssa->parent_instr->type == nir_instr_type_undef) {
            /* Create an ir3 undef */
            return NULL;
         } else {
            /* We need to insert the move at the end of the block */
            struct ir3_block *old_block = ctx->block;
            ir3_context_set_block(ctx, blk);
            struct ir3_instruction *src = ir3_get_src_shared(
               ctx, &nsrc->src,
               phi->dsts[0]->flags & IR3_REG_SHARED)[phi->phi.comp];
            ir3_context_set_block(ctx, old_block);
            return src;
         }
      }
   }

   UNREACHABLE("couldn't find phi node ir3 block");
   return NULL;
}

static void
resolve_phis(struct ir3_context *ctx, struct ir3_block *block)
{
   foreach_instr (phi, &block->instr_list) {
      if (phi->opc != OPC_META_PHI)
         break;

      nir_phi_instr *nphi = phi->phi.nphi;

      if (!nphi) /* skip continue phis created above */
         continue;

      for (unsigned i = 0; i < block->predecessors_count; i++) {
         struct ir3_block *pred = block->predecessors[i];
         struct ir3_instruction *src = read_phi_src(ctx, pred, phi, nphi);
         if (src) {
            __ssa_src(phi, src, 0);
         } else {
            /* Create an ir3 undef */
            ir3_src_create(phi, INVALID_REG, phi->dsts[0]->flags);
         }
      }
   }
}

static void
emit_jump(struct ir3_context *ctx, nir_jump_instr *jump)
{
   switch (jump->type) {
   case nir_jump_break:
   case nir_jump_continue:
   case nir_jump_return:
      /* I *think* we can simply just ignore this, and use the
       * successor block link to figure out where we need to
       * jump to for break/continue
       */
      break;
   default:
      ir3_context_error(ctx, "Unhandled NIR jump type: %d\n", jump->type);
      break;
   }
}

static void
emit_instr(struct ir3_context *ctx, nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      emit_alu(ctx, nir_instr_as_alu(instr));
      break;
   case nir_instr_type_deref:
      /* ignored, handled as part of the intrinsic they are src to */
      break;
   case nir_instr_type_intrinsic:
      emit_intrinsic(ctx, nir_instr_as_intrinsic(instr));
      break;
   case nir_instr_type_load_const:
      emit_load_const(ctx, nir_instr_as_load_const(instr));
      break;
   case nir_instr_type_undef:
      emit_undef(ctx, nir_instr_as_undef(instr));
      break;
   case nir_instr_type_tex: {
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      /* couple tex instructions get special-cased:
       */
      switch (tex->op) {
      case nir_texop_txs:
         emit_tex_txs(ctx, tex);
         break;
      case nir_texop_query_levels:
         emit_tex_info(ctx, tex, 2);
         break;
      case nir_texop_texture_samples:
         emit_tex_info(ctx, tex, 3);
         break;
      default:
         emit_tex(ctx, tex);
         break;
      }
      break;
   }
   case nir_instr_type_jump:
      emit_jump(ctx, nir_instr_as_jump(instr));
      break;
   case nir_instr_type_phi:
      emit_phi(ctx, nir_instr_as_phi(instr));
      break;
   case nir_instr_type_call:
   case nir_instr_type_parallel_copy:
      ir3_context_error(ctx, "Unhandled NIR instruction type: %d\n",
                        instr->type);
      break;
   }
}

static struct ir3_block *
get_block(struct ir3_context *ctx, const nir_block *nblock)
{
   struct ir3_block *block;
   struct hash_entry *hentry;

   hentry = _mesa_hash_table_search(ctx->block_ht, nblock);
   if (hentry)
      return hentry->data;

   block = ir3_block_create(ctx->ir);
   block->nblock = nblock;
   _mesa_hash_table_insert(ctx->block_ht, nblock, block);

   return block;
}

static struct ir3_block *
get_block_or_continue(struct ir3_context *ctx, const nir_block *nblock)
{
   struct hash_entry *hentry;

   hentry = _mesa_hash_table_search(ctx->continue_block_ht, nblock);
   if (hentry)
      return hentry->data;

   return get_block(ctx, nblock);
}

static struct ir3_block *
create_continue_block(struct ir3_context *ctx, const nir_block *nblock)
{
   struct ir3_block *block = ir3_block_create(ctx->ir);
   block->nblock = NULL;
   _mesa_hash_table_insert(ctx->continue_block_ht, nblock, block);
   return block;
}

static void
emit_block(struct ir3_context *ctx, nir_block *nblock)
{
   ir3_context_set_block(ctx, get_block(ctx, nblock));

   list_addtail(&ctx->block->node, &ctx->ir->block_list);

   ctx->block->loop_depth = ctx->loop_depth;

   /* re-emit addr register in each block if needed: */
   for (int i = 0; i < ARRAY_SIZE(ctx->addr0_ht); i++) {
      _mesa_hash_table_destroy(ctx->addr0_ht[i], NULL);
      ctx->addr0_ht[i] = NULL;
   }

   nir_foreach_instr (instr, nblock) {
      ctx->cur_instr = instr;
      emit_instr(ctx, instr);
      ctx->cur_instr = NULL;
      if (ctx->error)
         return;
   }

   for (int i = 0; i < ARRAY_SIZE(ctx->block->successors); i++) {
      if (nblock->successors[i]) {
         ctx->block->successors[i] =
            get_block_or_continue(ctx, nblock->successors[i]);
      }
   }

   /* Emit unconditional branch if we only have one successor. Conditional
    * branches are emitted in emit_if.
    */
   if (ctx->block->successors[0] && !ctx->block->successors[1]) {
      if (!ir3_block_get_terminator(ctx->block))
         ir3_JUMP(&ctx->build);
   }

   _mesa_hash_table_clear(ctx->sel_cond_conversions, NULL);
}

static void emit_cf_list(struct ir3_context *ctx, struct exec_list *list);

/* Get the ir3 branch condition for a given nir source. This will strip any inot
 * instructions and set *inv when the condition should be inverted. This
 * inversion can be directly folded into branches (in the inv1/inv2 fields)
 * instead of adding an explicit not.b/sub.u instruction.
 */
static struct ir3_instruction *
get_branch_condition(struct ir3_context *ctx, nir_src *src, unsigned comp,
                     bool *inv)
{
   struct ir3_instruction *condition = ir3_get_src(ctx, src)[comp];

   if (src->ssa->parent_instr->type == nir_instr_type_alu) {
      nir_alu_instr *nir_cond = nir_def_as_alu(src->ssa);

      if (nir_cond->op == nir_op_inot) {
         struct ir3_instruction *inv_cond = get_branch_condition(
            ctx, &nir_cond->src[0].src, nir_cond->src[0].swizzle[comp], inv);
         *inv = !*inv;
         return inv_cond;
      }
   }

   *inv = false;
   return ir3_get_predicate(ctx, condition);
}

/* Try to fold br (and/or cond1, cond2) into braa/brao cond1, cond2.
 */
static struct ir3_instruction *
fold_conditional_branch(struct ir3_context *ctx, struct nir_src *nir_cond)
{
   if (!ctx->compiler->has_branch_and_or)
      return NULL;

   if (nir_cond->ssa->parent_instr->type != nir_instr_type_alu)
      return NULL;

   nir_alu_instr *alu_cond = nir_def_as_alu(nir_cond->ssa);

   if ((alu_cond->op != nir_op_iand) && (alu_cond->op != nir_op_ior))
      return NULL;

   /* If the result of the and/or is also used for something else than an if
    * condition, the and/or cannot be removed. In that case, we will end-up with
    * extra predicate conversions for the conditions without actually removing
    * any instructions, resulting in an increase of instructions. Let's not fold
    * the conditions in the branch in that case.
    */
   if (!nir_def_only_used_by_if(&alu_cond->def))
      return NULL;

   bool inv1, inv2;
   struct ir3_instruction *cond1 = get_branch_condition(
      ctx, &alu_cond->src[0].src, alu_cond->src[0].swizzle[0], &inv1);
   struct ir3_instruction *cond2 = get_branch_condition(
      ctx, &alu_cond->src[1].src, alu_cond->src[1].swizzle[0], &inv2);

   struct ir3_instruction *branch;
   if (alu_cond->op == nir_op_iand) {
      branch = ir3_BRAA(&ctx->build, cond1, IR3_REG_PREDICATE, cond2,
                        IR3_REG_PREDICATE);
   } else {
      branch = ir3_BRAO(&ctx->build, cond1, IR3_REG_PREDICATE, cond2,
                        IR3_REG_PREDICATE);
   }

   branch->cat0.inv1 = inv1;
   branch->cat0.inv2 = inv2;
   return branch;
}

static bool
instr_can_be_predicated(nir_instr *instr)
{
   /* Anything that doesn't expand to control-flow can be predicated. */
   switch (instr->type) {
   case nir_instr_type_alu:
   case nir_instr_type_deref:
   case nir_instr_type_tex:
   case nir_instr_type_load_const:
   case nir_instr_type_undef:
   case nir_instr_type_phi:
   case nir_instr_type_parallel_copy:
      return true;
   case nir_instr_type_call:
   case nir_instr_type_jump:
      return false;
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_reduce:
      case nir_intrinsic_inclusive_scan:
      case nir_intrinsic_exclusive_scan:
      case nir_intrinsic_reduce_clusters_ir3:
      case nir_intrinsic_inclusive_scan_clusters_ir3:
      case nir_intrinsic_exclusive_scan_clusters_ir3:
      case nir_intrinsic_brcst_active_ir3:
      case nir_intrinsic_ballot:
      case nir_intrinsic_elect:
      case nir_intrinsic_elect_any_ir3:
      case nir_intrinsic_read_invocation_cond_ir3:
      case nir_intrinsic_demote:
      case nir_intrinsic_demote_if:
      case nir_intrinsic_terminate:
      case nir_intrinsic_terminate_if:
         return false;
      default:
         return true;
      }
   }
   }

   UNREACHABLE("Checked all cases");
}

static bool
nif_can_be_predicated(nir_if *nif)
{
   /* For non-divergent branches, predication is more expensive than a branch
    * because the latter can potentially skip all instructions.
    */
   if (!nir_src_is_divergent(&nif->condition))
      return false;

   /* Although it could potentially be possible to allow a limited form of
    * nested predication (e.g., by resetting the predication mask after a nested
    * branch), let's avoid this for now and only use predication for leaf
    * branches. That is, for ifs that contain exactly one block in both branches
    * (note that they always contain at least one block).
    */
   if (!exec_list_is_singular(&nif->then_list) ||
       !exec_list_is_singular(&nif->else_list)) {
      return false;
   }

   nir_foreach_instr (instr, nir_if_first_then_block(nif)) {
      if (!instr_can_be_predicated(instr))
         return false;
   }

   nir_foreach_instr (instr, nir_if_first_else_block(nif)) {
      if (!instr_can_be_predicated(instr))
         return false;
   }

   return true;
}

/* A typical if-else block like this:
 * if (cond) {
 *     tblock;
 * } else {
 *     fblock;
 * }
 * Will be emitted as:
 *        |-- i --|
 *        | ...   |
 *        | predt |
 *        |-------|
 *    succ0 /   \ succ1
 * |-- i+1 --| |-- i+2 --|
 * | tblock  | | fblock  |
 * | predf   | | jump    |
 * |---------| |---------|
 *    succ0 \   / succ0
 *        |-- j --|
 *        |  ...  |
 *        |-------|
 * Where the numbers at the top of blocks are their indices. That is, the true
 * block and false block are laid-out contiguously after the current block. This
 * layout is verified during legalization in prede_sched which also inserts the
 * final prede instruction. Note that we don't insert prede right away to allow
 * opt_jump to optimize the jump in the false block.
 */
static struct ir3_instruction *
emit_predicated_branch(struct ir3_context *ctx, nir_if *nif)
{
   if (!ctx->compiler->has_predication)
      return NULL;
   if (!nif_can_be_predicated(nif))
      return NULL;

   struct ir3_block *then_block = get_block(ctx, nir_if_first_then_block(nif));
   struct ir3_block *else_block = get_block(ctx, nir_if_first_else_block(nif));
   assert(list_is_empty(&then_block->instr_list) &&
          list_is_empty(&else_block->instr_list));

   bool inv;
   struct ir3_instruction *condition =
      get_branch_condition(ctx, &nif->condition, 0, &inv);
   struct ir3_builder then_build = ir3_builder_at(ir3_after_block(then_block));
   struct ir3_instruction *pred, *pred_inv;

   if (!inv) {
      pred = ir3_PREDT(&ctx->build, condition, IR3_REG_PREDICATE);
      pred_inv = ir3_PREDF(&then_build, condition, IR3_REG_PREDICATE);
   } else {
      pred = ir3_PREDF(&ctx->build, condition, IR3_REG_PREDICATE);
      pred_inv = ir3_PREDT(&then_build, condition, IR3_REG_PREDICATE);
   }

   pred->srcs[0]->num = REG_P0_X;
   pred_inv->srcs[0]->num = REG_P0_X;
   return pred;
}

static struct ir3_instruction *
emit_conditional_branch(struct ir3_context *ctx, nir_if *nif)
{
   nir_src *nir_cond = &nif->condition;
   struct ir3_instruction *folded = fold_conditional_branch(ctx, nir_cond);
   if (folded)
      return folded;

   struct ir3_instruction *predicated = emit_predicated_branch(ctx, nif);
   if (predicated)
      return predicated;

   bool inv1;
   struct ir3_instruction *cond1 =
      get_branch_condition(ctx, nir_cond, 0, &inv1);
   struct ir3_instruction *branch =
      ir3_BR(&ctx->build, cond1, IR3_REG_PREDICATE);
   branch->cat0.inv1 = inv1;
   return branch;
}

static void
emit_if(struct ir3_context *ctx, nir_if *nif)
{
   struct ir3_instruction *condition = ir3_get_src_maybe_shared(ctx, &nif->condition)[0];

   if (condition->opc == OPC_ANY_MACRO && condition->block == ctx->block) {
      struct ir3_instruction *pred = ssa(condition->srcs[0]);
      ir3_BANY(&ctx->build, pred, IR3_REG_PREDICATE);
   } else if (condition->opc == OPC_ALL_MACRO &&
              condition->block == ctx->block) {
      struct ir3_instruction *pred = ssa(condition->srcs[0]);
      ir3_BALL(&ctx->build, pred, IR3_REG_PREDICATE);
   } else if (condition->opc == OPC_ELECT_MACRO &&
              condition->block == ctx->block) {
      struct ir3_instruction *branch = ir3_GETONE(&ctx->build);
      branch->flags |= condition->flags & IR3_INSTR_NEEDS_HELPERS;
   } else if (condition->opc == OPC_SHPS_MACRO &&
              condition->block == ctx->block) {
      /* TODO: technically this only works if the block is the only user of the
       * shps, but we only use it in very constrained scenarios so this should
       * be ok.
       */
      ir3_SHPS(&ctx->build);
   } else {
      emit_conditional_branch(ctx, nif);
   }

   ctx->block->divergent_condition = nir_src_is_divergent(&nif->condition);

   emit_cf_list(ctx, &nif->then_list);
   emit_cf_list(ctx, &nif->else_list);
}

static bool
has_nontrivial_continue(nir_loop *nloop)
{
   struct nir_block *nstart = nir_loop_first_block(nloop);

   /* There's always one incoming edge from outside the loop, and if there
    * is more than one backedge from inside the loop (so more than 2 total
    * edges) then one must be a nontrivial continue.
    */
   if (nstart->predecessors->entries > 2)
      return true;

   /* Check whether the one backedge is a nontrivial continue. This can happen
    * if the loop ends with a break.
    */
   set_foreach (nstart->predecessors, entry) {
      nir_block *pred = (nir_block*)entry->key;
      if (pred == nir_loop_last_block(nloop) ||
          pred == nir_cf_node_as_block(nir_cf_node_prev(&nloop->cf_node)))
         continue;
      return true;
   }

   return false;
}

static void
emit_loop(struct ir3_context *ctx, nir_loop *nloop)
{
   assert(!nir_loop_has_continue_construct(nloop));
   ctx->loop_depth++;

   struct nir_block *nstart = nir_loop_first_block(nloop);
   struct ir3_block *continue_blk = NULL;

   /* If the loop has a continue statement that isn't at the end, then we need to
    * create a continue block in order to let control flow reconverge before
    * entering the next iteration of the loop.
    */
   if (has_nontrivial_continue(nloop)) {
      continue_blk = create_continue_block(ctx, nstart);
   }

   emit_cf_list(ctx, &nloop->body);

   if (continue_blk) {
      struct ir3_block *start = get_block(ctx, nstart);
      struct ir3_builder build = ir3_builder_at(ir3_after_block(continue_blk));
      ir3_JUMP(&build);
      continue_blk->successors[0] = start;
      continue_blk->loop_depth = ctx->loop_depth;
      list_addtail(&continue_blk->node, &ctx->ir->block_list);
   }

   ctx->so->loops++;
   ctx->loop_depth--;
}

static void
emit_cf_list(struct ir3_context *ctx, struct exec_list *list)
{
   foreach_list_typed (nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         emit_block(ctx, nir_cf_node_as_block(node));
         break;
      case nir_cf_node_if:
         emit_if(ctx, nir_cf_node_as_if(node));
         break;
      case nir_cf_node_loop:
         emit_loop(ctx, nir_cf_node_as_loop(node));
         break;
      case nir_cf_node_function:
         ir3_context_error(ctx, "TODO\n");
         break;
      }
   }
}

/* emit stream-out code.  At this point, the current block is the original
 * (nir) end block, and nir ensures that all flow control paths terminate
 * into the end block.  We re-purpose the original end block to generate
 * the 'if (vtxcnt < maxvtxcnt)' condition, then append the conditional
 * block holding stream-out write instructions, followed by the new end
 * block:
 *
 *   blockOrigEnd {
 *      p0.x = (vtxcnt < maxvtxcnt)
 *      // succs: blockStreamOut, blockNewEnd
 *   }
 *   blockStreamOut {
 *      // preds: blockOrigEnd
 *      ... stream-out instructions ...
 *      // succs: blockNewEnd
 *   }
 *   blockNewEnd {
 *      // preds: blockOrigEnd, blockStreamOut
 *   }
 */
static void
emit_stream_out(struct ir3_context *ctx)
{
   struct ir3 *ir = ctx->ir;
   struct ir3_stream_output_info *strmout = &ctx->so->stream_output;
   struct ir3_block *orig_end_block, *stream_out_block, *new_end_block;
   struct ir3_instruction *vtxcnt, *maxvtxcnt, *cond;
   struct ir3_instruction *bases[IR3_MAX_SO_BUFFERS];

   /* create vtxcnt input in input block at top of shader,
    * so that it is seen as live over the entire duration
    * of the shader:
    */
   vtxcnt = create_sysval_input(ctx, SYSTEM_VALUE_VERTEX_CNT, 0x1);
   maxvtxcnt = create_driver_param(ctx, IR3_DP_VS(vtxcnt_max));

   /* at this point, we are at the original 'end' block,
    * re-purpose this block to stream-out condition, then
    * append stream-out block and new-end block
    */
   orig_end_block = ctx->block;

   // maybe w/ store_global intrinsic, we could do this
   // stuff in nir->nir pass

   stream_out_block = ir3_block_create(ir);
   list_addtail(&stream_out_block->node, &ir->block_list);

   new_end_block = ir3_block_create(ir);
   list_addtail(&new_end_block->node, &ir->block_list);

   orig_end_block->successors[0] = stream_out_block;
   orig_end_block->successors[1] = new_end_block;

   stream_out_block->successors[0] = new_end_block;

   /* setup 'if (vtxcnt < maxvtxcnt)' condition: */
   cond = ir3_CMPS_S(&ctx->build, vtxcnt, 0, maxvtxcnt, 0);
   cond->dsts[0]->flags |= IR3_REG_PREDICATE;
   cond->cat2.condition = IR3_COND_LT;

   /* condition goes on previous block to the conditional,
    * since it is used to pick which of the two successor
    * paths to take:
    */
   ir3_BR(&ctx->build, cond, IR3_REG_PREDICATE);

   /* switch to stream_out_block to generate the stream-out
    * instructions:
    */
   ir3_context_set_block(ctx, stream_out_block);

   /* Calculate base addresses based on vtxcnt.  Instructions
    * generated for bases not used in following loop will be
    * stripped out in the backend.
    */
   for (unsigned i = 0; i < IR3_MAX_SO_BUFFERS; i++) {
      const struct ir3_const_state *const_state = ir3_const_state(ctx->so);
      unsigned stride = strmout->stride[i];
      struct ir3_instruction *base, *off;

      base = create_uniform(
         &ctx->build,
         ir3_const_reg(const_state, IR3_CONST_ALLOC_TFBO, i));

      /* 24-bit should be enough: */
      off = ir3_MUL_U24(&ctx->build, vtxcnt, 0,
                        create_immed(&ctx->build, stride * 4), 0);

      bases[i] = ir3_ADD_S(&ctx->build, off, 0, base, 0);
   }

   /* Generate the per-output store instructions: */
   for (unsigned i = 0; i < strmout->num_outputs; i++) {
      for (unsigned j = 0; j < strmout->output[i].num_components; j++) {
         unsigned c = j + strmout->output[i].start_component;
         struct ir3_instruction *base, *out, *stg;

         base = bases[strmout->output[i].output_buffer];
         out = ctx->outputs[regid(strmout->output[i].register_index, c)];

         stg = ir3_STG(
            &ctx->build, base, 0,
            create_immed(&ctx->build, (strmout->output[i].dst_offset + j) * 4),
            0, out, 0, create_immed(&ctx->build, 1), 0);
         stg->cat6.type = TYPE_U32;

         array_insert(ctx->block, ctx->block->keeps, stg);
      }
   }

   ir3_JUMP(&ctx->build);

   /* and finally switch to the new_end_block: */
   ir3_context_set_block(ctx, new_end_block);
}

static void
setup_predecessors(struct ir3 *ir)
{
   foreach_block (block, &ir->block_list) {
      for (int i = 0; i < ARRAY_SIZE(block->successors); i++) {
         if (block->successors[i])
            ir3_block_add_predecessor(block->successors[i], block);
      }
   }
}

static void
emit_function(struct ir3_context *ctx, nir_function_impl *impl)
{
   nir_metadata_require(impl, nir_metadata_block_index);

   emit_cf_list(ctx, &impl->body);
   emit_block(ctx, impl->end_block);

   /* at this point, we should have a single empty block,
    * into which we emit the 'end' instruction.
    */
   compile_assert(ctx, list_is_empty(&ctx->block->instr_list));

   /* If stream-out (aka transform-feedback) enabled, emit the
    * stream-out instructions, followed by a new empty block (into
    * which the 'end' instruction lands).
    *
    * NOTE: it is done in this order, rather than inserting before
    * we emit end_block, because NIR guarantees that all blocks
    * flow into end_block, and that end_block has no successors.
    * So by re-purposing end_block as the first block of stream-
    * out, we guarantee that all exit paths flow into the stream-
    * out instructions.
    */
   if ((ctx->compiler->gen < 5) &&
       (ctx->so->stream_output.num_outputs > 0) &&
       !ctx->so->binning_pass) {
      assert(ctx->so->type == MESA_SHADER_VERTEX);
      emit_stream_out(ctx);
   }

   setup_predecessors(ctx->ir);
   foreach_block (block, &ctx->ir->block_list) {
      resolve_phis(ctx, block);
   }
}

static void
setup_input(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   struct ir3_shader_variant *so = ctx->so;
   struct ir3_instruction *coord = NULL;

   if (intr->intrinsic == nir_intrinsic_load_interpolated_input)
      coord =
         ir3_create_collect(&ctx->build, ir3_get_src(ctx, &intr->src[0]), 2);

   compile_assert(ctx, nir_src_is_const(intr->src[coord ? 1 : 0]));

   unsigned frac = nir_intrinsic_component(intr);
   unsigned offset = nir_src_as_uint(intr->src[coord ? 1 : 0]);
   unsigned ncomp = nir_intrinsic_dest_components(intr);
   unsigned n = nir_intrinsic_base(intr) + offset;
   unsigned slot = nir_intrinsic_io_semantics(intr).location + offset;
   unsigned compmask = BITFIELD_MASK(ncomp + frac);

   /* Inputs are loaded using ldlw or ldg for other stages. */
   compile_assert(ctx, ctx->so->type == MESA_SHADER_FRAGMENT ||
                          ctx->so->type == MESA_SHADER_VERTEX);

   /* for clip+cull distances, unused components can't be eliminated because
    * they're read by fixed-function, even if there's a hole.  Note that
    * clip/cull distance arrays must be declared in the FS, so we can just
    * use the NIR clip/cull distances to avoid reading ucp_enables in the
    * shader key.
    */
   if (ctx->so->type == MESA_SHADER_FRAGMENT &&
       (slot == VARYING_SLOT_CLIP_DIST0 ||
        slot == VARYING_SLOT_CLIP_DIST1)) {
      unsigned clip_cull_mask = so->clip_mask | so->cull_mask;

      if (slot == VARYING_SLOT_CLIP_DIST0)
         compmask = clip_cull_mask & 0xf;
      else
         compmask = clip_cull_mask >> 4;
   }

   /* for a4xx+ rasterflat */
   if (so->inputs[n].rasterflat && ctx->so->key.rasterflat)
      coord = NULL;

   so->total_in += util_bitcount(compmask & ~so->inputs[n].compmask);

   so->inputs[n].slot = slot;
   so->inputs[n].compmask |= compmask;
   so->inputs_count = MAX2(so->inputs_count, n + 1);
   compile_assert(ctx, so->inputs_count < ARRAY_SIZE(so->inputs));
   so->inputs[n].flat = !coord;

   if (ctx->so->type == MESA_SHADER_FRAGMENT) {
      compile_assert(ctx, slot != VARYING_SLOT_POS);

      so->inputs[n].bary = true;
      unsigned idx = (n * 4) + frac;
      struct ir3_instruction_rpt instr =
         create_frag_input(ctx, coord, idx, ncomp);
      cp_instrs(ctx->last_dst, instr.rpts, ncomp);

      if (slot == VARYING_SLOT_PRIMITIVE_ID)
         so->reads_primid = true;

      so->inputs[n].inloc = 4 * n;
      so->varying_in = MAX2(so->varying_in, 4 * n + 4);
   } else {
      struct ir3_instruction *input = NULL;

      foreach_input (in, ctx->ir) {
         if (in->input.inidx == n) {
            input = in;
            break;
         }
      }

      if (!input) {
         input = create_input(ctx, compmask);
         input->input.inidx = n;
      } else {
         /* For aliased inputs, just append to the wrmask.. ie. if we
          * first see a vec2 index at slot N, and then later a vec4,
          * the wrmask of the resulting overlapped vec2 and vec4 is 0xf
          */
         input->dsts[0]->wrmask |= compmask;
      }

      for (int i = 0; i < ncomp + frac; i++) {
         unsigned idx = (n * 4) + i;
         compile_assert(ctx, idx < ctx->ninputs);

         /* fixup the src wrmask to avoid validation fail */
         if (ctx->inputs[idx] && (ctx->inputs[idx] != input)) {
            ctx->inputs[idx]->srcs[0]->wrmask = input->dsts[0]->wrmask;
            continue;
         }

         ir3_split_dest(&ctx->build, &ctx->inputs[idx], input, i, 1);
      }

      for (int i = 0; i < ncomp; i++) {
         unsigned idx = (n * 4) + i + frac;
         ctx->last_dst[i] = ctx->inputs[idx];
      }
   }
}

/* Initially we assign non-packed inloc's for varyings, as we don't really
 * know up-front which components will be unused.  After all the compilation
 * stages we scan the shader to see which components are actually used, and
 * re-pack the inlocs to eliminate unneeded varyings.
 */
static void
pack_inlocs(struct ir3_context *ctx)
{
   struct ir3_shader_variant *so = ctx->so;
   uint8_t used_components[so->inputs_count];

   memset(used_components, 0, sizeof(used_components));

   /*
    * First Step: scan shader to find which bary.f/ldlv remain:
    */

   foreach_block (block, &ctx->ir->block_list) {
      foreach_instr (instr, &block->instr_list) {
         if (is_input(instr)) {
            unsigned inloc = instr->srcs[0]->iim_val;
            unsigned i = inloc / 4;
            unsigned j = inloc % 4;

            compile_assert(ctx, instr->srcs[0]->flags & IR3_REG_IMMED);
            compile_assert(ctx, i < so->inputs_count);

            used_components[i] |= 1 << j;
         } else if (instr->opc == OPC_META_TEX_PREFETCH) {
            for (int n = 0; n < 2; n++) {
               unsigned inloc = instr->prefetch.input_offset + n;
               unsigned i = inloc / 4;
               unsigned j = inloc % 4;

               compile_assert(ctx, i < so->inputs_count);

               used_components[i] |= 1 << j;
            }
         }
      }
   }

   /*
    * Second Step: reassign varying inloc/slots:
    */

   unsigned inloc = 0;

   /* for clip+cull distances, unused components can't be eliminated because
    * they're read by fixed-function, even if there's a hole.  Note that
    * clip/cull distance arrays must be declared in the FS, so we can just
    * use the NIR clip/cull distances to avoid reading ucp_enables in the
    * shader key.
    */
   unsigned clip_cull_mask = so->clip_mask | so->cull_mask;

   so->varying_in = 0;

   for (unsigned i = 0; i < so->inputs_count; i++) {
      unsigned compmask = 0, maxcomp = 0;

      so->inputs[i].inloc = inloc;
      so->inputs[i].bary = false;

      if (so->inputs[i].slot == VARYING_SLOT_CLIP_DIST0 ||
          so->inputs[i].slot == VARYING_SLOT_CLIP_DIST1) {
         if (so->inputs[i].slot == VARYING_SLOT_CLIP_DIST0)
            compmask = clip_cull_mask & 0xf;
         else
            compmask = clip_cull_mask >> 4;
         used_components[i] = compmask;
      }

      for (unsigned j = 0; j < 4; j++) {
         if (!(used_components[i] & (1 << j)))
            continue;

         compmask |= (1 << j);
         maxcomp = j + 1;

         /* at this point, since used_components[i] mask is only
          * considering varyings (ie. not sysvals) we know this
          * is a varying:
          */
         so->inputs[i].bary = true;
      }

      if (so->inputs[i].bary) {
         so->varying_in++;
         so->inputs[i].compmask = (1 << maxcomp) - 1;
         inloc += maxcomp;
      }
   }

   /*
    * Third Step: reassign packed inloc's:
    */

   foreach_block (block, &ctx->ir->block_list) {
      foreach_instr (instr, &block->instr_list) {
         if (is_input(instr)) {
            unsigned inloc = instr->srcs[0]->iim_val;
            unsigned i = inloc / 4;
            unsigned j = inloc % 4;

            instr->srcs[0]->iim_val = so->inputs[i].inloc + j;
            if (instr->opc == OPC_FLAT_B)
               instr->srcs[1]->iim_val = instr->srcs[0]->iim_val;
         } else if (instr->opc == OPC_META_TEX_PREFETCH) {
            unsigned i = instr->prefetch.input_offset / 4;
            unsigned j = instr->prefetch.input_offset % 4;
            instr->prefetch.input_offset = so->inputs[i].inloc + j;
         }
      }
   }
}

static void
setup_output(struct ir3_context *ctx, nir_intrinsic_instr *intr)
{
   struct ir3_shader_variant *so = ctx->so;
   nir_io_semantics io = nir_intrinsic_io_semantics(intr);

   nir_src offset_src = *nir_get_io_offset_src(intr);
   compile_assert(ctx, nir_src_is_const(offset_src));

   unsigned offset = nir_src_as_uint(offset_src);
   unsigned frac = nir_intrinsic_component(intr);
   unsigned ncomp = nir_intrinsic_src_components(intr, 0);
   unsigned slot = io.location + offset;

   /* For per-view variables, each user-facing slot corresponds to multiple
    * views, each with a corresponding driver_location, and the view index
    * offsets the driver_location. */
   unsigned view_index = intr->intrinsic == nir_intrinsic_store_per_view_output
      ? nir_src_as_uint(intr->src[1])
      : 0;
   unsigned n = nir_intrinsic_base(intr) + offset + view_index;

   if (ctx->so->type == MESA_SHADER_FRAGMENT) {
      switch (slot) {
      case FRAG_RESULT_DEPTH:
         so->writes_pos = true;
         break;
      case FRAG_RESULT_COLOR:
         if (!ctx->s->info.fs.color_is_dual_source) {
            so->color0_mrt = 1;
         } else {
            slot = FRAG_RESULT_DATA0 + io.dual_source_blend_index;
            if (io.dual_source_blend_index > 0)
               so->dual_src_blend = true;
         }
         break;
      case FRAG_RESULT_SAMPLE_MASK:
         so->writes_smask = true;
         break;
      case FRAG_RESULT_STENCIL:
         so->writes_stencilref = true;
         break;
      default:
         slot += io.dual_source_blend_index; /* For dual-src blend */
         if (io.dual_source_blend_index > 0)
            so->dual_src_blend = true;
         if (slot >= FRAG_RESULT_DATA0)
            break;
         ir3_context_error(ctx, "unknown FS output name: %s\n",
                           gl_frag_result_name(slot));
      }
   } else if (ctx->so->type == MESA_SHADER_VERTEX ||
              ctx->so->type == MESA_SHADER_TESS_EVAL ||
              ctx->so->type == MESA_SHADER_GEOMETRY) {
      switch (slot) {
      case VARYING_SLOT_POS:
         so->writes_pos = true;
         break;
      case VARYING_SLOT_PSIZ:
         so->writes_psize = true;
         break;
      case VARYING_SLOT_VIEWPORT:
         so->writes_viewport = true;
         break;
      case VARYING_SLOT_PRIMITIVE_SHADING_RATE:
         so->writes_shading_rate = true;
         break;
      case VARYING_SLOT_PRIMITIVE_ID:
      case VARYING_SLOT_GS_VERTEX_FLAGS_IR3:
         assert(ctx->so->type == MESA_SHADER_GEOMETRY);
         FALLTHROUGH;
      case VARYING_SLOT_COL0:
      case VARYING_SLOT_COL1:
      case VARYING_SLOT_BFC0:
      case VARYING_SLOT_BFC1:
      case VARYING_SLOT_FOGC:
      case VARYING_SLOT_CLIP_DIST0:
      case VARYING_SLOT_CLIP_DIST1:
      case VARYING_SLOT_CLIP_VERTEX:
      case VARYING_SLOT_LAYER:
         break;
      default:
         if (slot >= VARYING_SLOT_VAR0)
            break;
         if ((VARYING_SLOT_TEX0 <= slot) && (slot <= VARYING_SLOT_TEX7))
            break;
         ir3_context_error(ctx, "unknown %s shader output name: %s\n",
                           _mesa_shader_stage_to_string(ctx->so->type),
                           gl_varying_slot_name_for_stage(slot, ctx->so->type));
      }
   } else {
      ir3_context_error(ctx, "unknown shader type: %d\n", ctx->so->type);
   }

   so->outputs_count = MAX2(so->outputs_count, n + 1);
   compile_assert(ctx, so->outputs_count <= ARRAY_SIZE(so->outputs));

   so->outputs[n].slot = slot;
   if (view_index > 0)
      so->multi_pos_output = true;
   so->outputs[n].view = view_index;

   for (int i = 0; i < ncomp; i++) {
      unsigned idx = (n * 4) + i + frac;
      compile_assert(ctx, idx < ctx->noutputs);
      ctx->outputs[idx] = create_immed(&ctx->build, fui(0.0));
   }

   /* if varying packing doesn't happen, we could end up in a situation
    * with "holes" in the output, and since the per-generation code that
    * sets up varying linkage registers doesn't expect to have more than
    * one varying per vec4 slot, pad the holes.
    *
    * Note that this should probably generate a performance warning of
    * some sort.
    */
   for (int i = 0; i < frac; i++) {
      unsigned idx = (n * 4) + i;
      if (!ctx->outputs[idx]) {
         ctx->outputs[idx] = create_immed(&ctx->build, fui(0.0));
      }
   }

   struct ir3_instruction *const *src = ir3_get_src(ctx, &intr->src[0]);
   for (int i = 0; i < ncomp; i++) {
      unsigned idx = (n * 4) + i + frac;
      ctx->outputs[idx] = src[i];
   }
}

static bool
uses_load_input(struct ir3_shader_variant *so)
{
   return so->type == MESA_SHADER_VERTEX || so->type == MESA_SHADER_FRAGMENT;
}

static bool
uses_store_output(struct ir3_shader_variant *so)
{
   switch (so->type) {
   case MESA_SHADER_VERTEX:
      return !so->key.has_gs && !so->key.tessellation;
   case MESA_SHADER_TESS_EVAL:
      return !so->key.has_gs;
   case MESA_SHADER_GEOMETRY:
   case MESA_SHADER_FRAGMENT:
      return true;
   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      return false;
   default:
      UNREACHABLE("unknown stage");
   }
}

static void
emit_instructions(struct ir3_context *ctx)
{
   MESA_TRACE_FUNC();

   nir_function_impl *fxn = nir_shader_get_entrypoint(ctx->s);

   /* some varying setup which can't be done in setup_input(): */
   if (ctx->so->type == MESA_SHADER_FRAGMENT) {
      nir_foreach_shader_in_variable (var, ctx->s) {
         /* set rasterflat flag for front/back color */
         if (var->data.interpolation == INTERP_MODE_NONE) {
            switch (var->data.location) {
            case VARYING_SLOT_COL0:
            case VARYING_SLOT_COL1:
            case VARYING_SLOT_BFC0:
            case VARYING_SLOT_BFC1:
               ctx->so->inputs[var->data.driver_location].rasterflat = true;
               break;
            default:
               break;
            }
         }
      }
   }

   if (uses_load_input(ctx->so)) {
      ctx->so->inputs_count = ctx->s->num_inputs;
      compile_assert(ctx, ctx->so->inputs_count < ARRAY_SIZE(ctx->so->inputs));
      ctx->ninputs = ctx->s->num_inputs * 4;
      ctx->inputs = rzalloc_array(ctx, struct ir3_instruction *, ctx->ninputs);
   } else {
      ctx->ninputs = 0;
      ctx->so->inputs_count = 0;
   }

   if (uses_store_output(ctx->so)) {
      ctx->noutputs = ctx->s->num_outputs * 4;
      ctx->outputs =
         rzalloc_array(ctx, struct ir3_instruction *, ctx->noutputs);
   } else {
      ctx->noutputs = 0;
   }

   ctx->ir = ir3_create(ctx->compiler, ctx->so);

   /* Create inputs in first block: */
   ir3_context_set_block(ctx, get_block(ctx, nir_start_block(fxn)));
   ctx->in_block = ctx->block;

   /* for fragment shader, the vcoord input register is used as the
    * base for bary.f varying fetch instrs:
    *
    * TODO defer creating ctx->ij_pixel and corresponding sysvals
    * until emit_intrinsic when we know they are actually needed.
    * For now, we defer creating ctx->ij_centroid, etc, since we
    * only need ij_pixel for "old style" varying inputs (ie.
    * tgsi_to_nir)
    */
   if (ctx->so->type == MESA_SHADER_FRAGMENT) {
      ctx->ij[IJ_PERSP_PIXEL] = create_input(ctx, 0x3);
   }

   /* Defer add_sysval_input() stuff until after setup_inputs(),
    * because sysvals need to be appended after varyings:
    */
   if (ctx->ij[IJ_PERSP_PIXEL]) {
      add_sysval_input_compmask(ctx, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL, 0x3,
                                ctx->ij[IJ_PERSP_PIXEL]);
   }

   /* Tesselation shaders always need primitive ID for indexing the
    * BO. Geometry shaders don't always need it but when they do it has be
    * delivered and unclobbered in the VS. To make things easy, we always
    * make room for it in VS/DS.
    */
   bool has_tess = ctx->so->key.tessellation != IR3_TESS_NONE;
   bool has_gs = ctx->so->key.has_gs;
   switch (ctx->so->type) {
   case MESA_SHADER_VERTEX:
      if (has_tess) {
         ctx->tcs_header =
            create_sysval_input(ctx, SYSTEM_VALUE_TCS_HEADER_IR3, 0x1);
         ctx->rel_patch_id =
            create_sysval_input(ctx, SYSTEM_VALUE_REL_PATCH_ID_IR3, 0x1);
         ctx->primitive_id =
            create_sysval_input(ctx, SYSTEM_VALUE_PRIMITIVE_ID, 0x1);
      } else if (has_gs) {
         ctx->gs_header =
            create_sysval_input(ctx, SYSTEM_VALUE_GS_HEADER_IR3, 0x1);
         ctx->primitive_id =
            create_sysval_input(ctx, SYSTEM_VALUE_PRIMITIVE_ID, 0x1);
      }
      break;
   case MESA_SHADER_TESS_CTRL:
      ctx->tcs_header =
         create_sysval_input(ctx, SYSTEM_VALUE_TCS_HEADER_IR3, 0x1);
      ctx->rel_patch_id =
         create_sysval_input(ctx, SYSTEM_VALUE_REL_PATCH_ID_IR3, 0x1);
      break;
   case MESA_SHADER_TESS_EVAL:
      if (has_gs) {
         ctx->gs_header =
            create_sysval_input(ctx, SYSTEM_VALUE_GS_HEADER_IR3, 0x1);
         ctx->primitive_id =
            create_sysval_input(ctx, SYSTEM_VALUE_PRIMITIVE_ID, 0x1);
      }
      ctx->rel_patch_id =
         create_sysval_input(ctx, SYSTEM_VALUE_REL_PATCH_ID_IR3, 0x1);
      break;
   case MESA_SHADER_GEOMETRY:
      ctx->gs_header =
         create_sysval_input(ctx, SYSTEM_VALUE_GS_HEADER_IR3, 0x1);
      break;
   default:
      break;
   }

   /* Find # of samplers. Just assume that we'll be reading from images.. if
    * it is write-only we don't have to count it, but after lowering derefs
    * is too late to compact indices for that.
    */
   ctx->so->num_samp =
      BITSET_LAST_BIT(ctx->s->info.textures_used) + ctx->s->info.num_images;

   /* Save off clip+cull information. Note that in OpenGL clip planes may
    * be individually enabled/disabled, and some gens handle lowering in
    * backend, so we also need to consider the shader key:
    */
   ctx->so->clip_mask = ctx->so->key.ucp_enables |
                        MASK(ctx->s->info.clip_distance_array_size);
   ctx->so->cull_mask = MASK(ctx->s->info.cull_distance_array_size)
                        << ctx->s->info.clip_distance_array_size;

   ctx->so->pvtmem_size = ctx->s->scratch_size;
   ctx->so->shared_size = ctx->s->info.shared_size;

   /* NOTE: need to do something more clever when we support >1 fxn */
   nir_foreach_reg_decl (decl, fxn) {
      ir3_declare_array(ctx, decl);
   }

   /* And emit the body: */
   ctx->impl = fxn;
   emit_function(ctx, fxn);

   if (ctx->so->type == MESA_SHADER_TESS_CTRL &&
       ctx->compiler->tess_use_shared) {
      /* Anything before shpe seems to be ignored in the main shader when early
       * preamble is enabled on a7xx, so we have to put the barrier after.
       */
      struct ir3_block *block = ir3_after_preamble(ctx->ir);
      struct ir3_builder build = ir3_builder_at(ir3_after_block(block));

      struct ir3_instruction *barrier = ir3_BAR(&build);
      barrier->flags = IR3_INSTR_SS | IR3_INSTR_SY;
      barrier->barrier_class = IR3_BARRIER_EVERYTHING;
      array_insert(block, block->keeps, barrier);
      ctx->so->has_barrier = true;

      /* Move the barrier to the beginning of the block but after any phi/input
       * meta instructions that must be at the beginning. It must be before we
       * load VS outputs.
       */
      foreach_instr (instr, &block->instr_list) {
         if (instr->opc != OPC_META_INPUT &&
             instr->opc != OPC_META_TEX_PREFETCH &&
             instr->opc != OPC_META_PHI) {
            ir3_instr_move_before(barrier, instr);
            break;
         }
      }
   }
}

/* Fixup tex sampler state for astc/srgb workaround instructions.  We
 * need to assign the tex state indexes for these after we know the
 * max tex index.
 */
static void
fixup_astc_srgb(struct ir3_context *ctx)
{
   struct ir3_shader_variant *so = ctx->so;
   /* indexed by original tex idx, value is newly assigned alpha sampler
    * state tex idx.  Zero is invalid since there is at least one sampler
    * if we get here.
    */
   unsigned alt_tex_state[16] = {0};
   unsigned tex_idx = ctx->max_texture_index + 1;
   unsigned idx = 0;

   so->astc_srgb.base = tex_idx;

   for (unsigned i = 0; i < ctx->ir->astc_srgb_count; i++) {
      struct ir3_instruction *sam = ctx->ir->astc_srgb[i];

      compile_assert(ctx, sam->cat5.tex < ARRAY_SIZE(alt_tex_state));

      if (alt_tex_state[sam->cat5.tex] == 0) {
         /* assign new alternate/alpha tex state slot: */
         alt_tex_state[sam->cat5.tex] = tex_idx++;
         so->astc_srgb.orig_idx[idx++] = sam->cat5.tex;
         so->astc_srgb.count++;
      }

      sam->cat5.tex = alt_tex_state[sam->cat5.tex];
   }
}

/* Fixup tex sampler state for tg4 workaround instructions.  We
 * need to assign the tex state indexes for these after we know the
 * max tex index.
 */
static void
fixup_tg4(struct ir3_context *ctx)
{
   struct ir3_shader_variant *so = ctx->so;
   /* indexed by original tex idx, value is newly assigned alpha sampler
    * state tex idx.  Zero is invalid since there is at least one sampler
    * if we get here.
    */
   unsigned alt_tex_state[16] = {0};
   unsigned tex_idx = ctx->max_texture_index + so->astc_srgb.count + 1;
   unsigned idx = 0;

   so->tg4.base = tex_idx;

   for (unsigned i = 0; i < ctx->ir->tg4_count; i++) {
      struct ir3_instruction *sam = ctx->ir->tg4[i];

      compile_assert(ctx, sam->cat5.tex < ARRAY_SIZE(alt_tex_state));

      if (alt_tex_state[sam->cat5.tex] == 0) {
         /* assign new alternate/alpha tex state slot: */
         alt_tex_state[sam->cat5.tex] = tex_idx++;
         so->tg4.orig_idx[idx++] = sam->cat5.tex;
         so->tg4.count++;
      }

      sam->cat5.tex = alt_tex_state[sam->cat5.tex];
   }
}

static bool
is_empty(struct ir3 *ir)
{
   foreach_block (block, &ir->block_list) {
      foreach_instr (instr, &block->instr_list) {
         return instr->opc == OPC_END;
      }
   }
   return true;
}

static void
collect_tex_prefetches(struct ir3_context *ctx, struct ir3 *ir)
{
   unsigned idx = 0;

   /* Collect sampling instructions eligible for pre-dispatch. */
   foreach_block (block, &ir->block_list) {
      foreach_instr_safe (instr, &block->instr_list) {
         if (instr->opc == OPC_META_TEX_PREFETCH) {
            assert(idx < ARRAY_SIZE(ctx->so->sampler_prefetch));
            struct ir3_sampler_prefetch *fetch =
               &ctx->so->sampler_prefetch[idx];
            idx++;

            fetch->bindless = instr->flags & IR3_INSTR_B;
            if (fetch->bindless) {
               /* In bindless mode, the index is actually the base */
               fetch->tex_id = instr->prefetch.tex_base;
               fetch->samp_id = instr->prefetch.samp_base;
               fetch->tex_bindless_id = instr->prefetch.tex;
               fetch->samp_bindless_id = instr->prefetch.samp;
            } else {
               fetch->tex_id = instr->prefetch.tex;
               fetch->samp_id = instr->prefetch.samp;
            }
            fetch->tex_opc = OPC_SAM;
            fetch->wrmask = instr->dsts[0]->wrmask;
            fetch->dst = instr->dsts[0]->num;
            fetch->src = instr->prefetch.input_offset;

            /* These are the limits on a5xx/a6xx, we might need to
             * revisit if SP_FS_PREFETCH[n] changes on later gens:
             */
            assert(fetch->dst <= 0x3f);
            assert(fetch->tex_id <= 0x1f);
            assert(fetch->samp_id <= 0xf);

            ctx->so->total_in =
               MAX2(ctx->so->total_in, instr->prefetch.input_offset + 2);

            fetch->half_precision = !!(instr->dsts[0]->flags & IR3_REG_HALF);

            /* Remove the prefetch placeholder instruction: */
            list_delinit(&instr->node);
         }
      }
   }
}

static bool
is_noop_subreg_move(struct ir3_instruction *instr)
{
   enum ir3_subreg_move subreg_move = ir3_is_subreg_move(instr);

   if (subreg_move == IR3_SUBREG_MOVE_NONE) {
      return false;
   }

   struct ir3_register *src = instr->srcs[0];
   struct ir3_register *dst = instr->dsts[0];
   unsigned offset = subreg_move == IR3_SUBREG_MOVE_LOWER ? 0 : 1;

   return ra_num_to_physreg(dst->num, dst->flags) ==
          ra_num_to_physreg(src->num, src->flags) + offset;
}

static bool
ir3_remove_noop_subreg_moves(struct ir3 *ir)
{
   if (!ir->compiler->mergedregs) {
      return false;
   }

   bool progress = false;

   foreach_block (block, &ir->block_list) {
      foreach_instr_safe (instr, &block->instr_list) {
         if (is_noop_subreg_move(instr)) {
            ir3_instr_remove(instr);
            progress = true;
         }
      }
   }

   return progress;
}

int
ir3_compile_shader_nir(struct ir3_compiler *compiler,
                       struct ir3_shader *shader,
                       struct ir3_shader_variant *so)
{
   struct ir3_context *ctx;
   struct ir3 *ir;
   int ret = 0, max_bary;
   bool progress;

   MESA_TRACE_FUNC();

   assert(!so->ir);

   ctx = ir3_context_init(compiler, shader, so);
   if (!ctx) {
      DBG("INIT failed!");
      ret = -1;
      goto out;
   }

   emit_instructions(ctx);

   if (ctx->error) {
      DBG("EMIT failed!");
      ret = -1;
      goto out;
   }

   ir = so->ir = ctx->ir;

   if (gl_shader_stage_is_compute(so->type)) {
      so->local_size[0] = ctx->s->info.workgroup_size[0];
      so->local_size[1] = ctx->s->info.workgroup_size[1];
      so->local_size[2] = ctx->s->info.workgroup_size[2];
      so->local_size_variable = ctx->s->info.workgroup_size_variable;
   }

   if (so->type == MESA_SHADER_FRAGMENT && so->reads_shading_rate &&
       !so->reads_smask &&
       compiler->reading_shading_rate_requires_smask_quirk) {
      create_sysval_input(ctx, SYSTEM_VALUE_SAMPLE_MASK_IN, 0x1);
   }

   /* Vertex shaders in a tessellation or geometry pipeline treat END as a
    * NOP and has an epilogue that writes the VS outputs to local storage, to
    * be read by the HS.  Then it resets execution mask (chmask) and chains
    * to the next shader (chsh). There are also a few output values which we
    * must send to the next stage via registers, and in order for both stages
    * to agree on the register used we must force these to be in specific
    * registers.
    */
   if ((so->type == MESA_SHADER_VERTEX &&
        (so->key.has_gs || so->key.tessellation)) ||
       (so->type == MESA_SHADER_TESS_EVAL && so->key.has_gs)) {
      struct ir3_instruction *outputs[3];
      unsigned outidxs[3];
      unsigned regids[3];
      unsigned outputs_count = 0;

      if (ctx->primitive_id) {
         unsigned n = so->outputs_count++;
         so->outputs[n].slot = VARYING_SLOT_PRIMITIVE_ID;

         struct ir3_instruction *out =
            ir3_collect(&ctx->build, ctx->primitive_id);
         outputs[outputs_count] = out;
         outidxs[outputs_count] = n;
         if (so->type == MESA_SHADER_VERTEX && ctx->rel_patch_id)
            regids[outputs_count] = regid(0, 2);
         else
            regids[outputs_count] = regid(0, 1);
         outputs_count++;
      }

      if (so->type == MESA_SHADER_VERTEX && ctx->rel_patch_id) {
         unsigned n = so->outputs_count++;
         so->outputs[n].slot = VARYING_SLOT_REL_PATCH_ID_IR3;
         struct ir3_instruction *out =
            ir3_collect(&ctx->build, ctx->rel_patch_id);
         outputs[outputs_count] = out;
         outidxs[outputs_count] = n;
         regids[outputs_count] = regid(0, 1);
         outputs_count++;
      }

      if (ctx->gs_header) {
         unsigned n = so->outputs_count++;
         so->outputs[n].slot = VARYING_SLOT_GS_HEADER_IR3;
         struct ir3_instruction *out = ir3_collect(&ctx->build, ctx->gs_header);
         outputs[outputs_count] = out;
         outidxs[outputs_count] = n;
         regids[outputs_count] = regid(0, 0);
         outputs_count++;
      }

      if (ctx->tcs_header) {
         unsigned n = so->outputs_count++;
         so->outputs[n].slot = VARYING_SLOT_TCS_HEADER_IR3;
         struct ir3_instruction *out =
            ir3_collect(&ctx->build, ctx->tcs_header);
         outputs[outputs_count] = out;
         outidxs[outputs_count] = n;
         regids[outputs_count] = regid(0, 0);
         outputs_count++;
      }

      struct ir3_instruction *chmask =
         ir3_build_instr(&ctx->build, OPC_CHMASK, 0, outputs_count);
      chmask->barrier_class = IR3_BARRIER_EVERYTHING;
      chmask->barrier_conflict = IR3_BARRIER_EVERYTHING;

      for (unsigned i = 0; i < outputs_count; i++)
         __ssa_src(chmask, outputs[i], 0)->num = regids[i];

      chmask->end.outidxs = ralloc_array(chmask, unsigned, outputs_count);
      memcpy(chmask->end.outidxs, outidxs, sizeof(unsigned) * outputs_count);

      array_insert(ctx->block, ctx->block->keeps, chmask);

      struct ir3_instruction *chsh = ir3_CHSH(&ctx->build);
      chsh->barrier_class = IR3_BARRIER_EVERYTHING;
      chsh->barrier_conflict = IR3_BARRIER_EVERYTHING;
   } else {
      assert((ctx->noutputs % 4) == 0);
      unsigned outidxs[ctx->noutputs / 4];
      struct ir3_instruction *outputs[ctx->noutputs / 4];
      unsigned outputs_count = 0;

      struct ir3_block *b = ctx->block;
      /* Insert these collect's in the block before the end-block if
       * possible, so that any moves they generate can be shuffled around to
       * reduce nop's:
       */
      if (ctx->block->predecessors_count == 1)
         b = ctx->block->predecessors[0];

      /* Setup IR level outputs, which are "collects" that gather
       * the scalar components of outputs.
       */
      for (unsigned i = 0; i < ctx->noutputs; i += 4) {
         unsigned ncomp = 0;
         /* figure out the # of components written:
          *
          * TODO do we need to handle holes, ie. if .x and .z
          * components written, but .y component not written?
          */
         for (unsigned j = 0; j < 4; j++) {
            if (!ctx->outputs[i + j])
               break;
            ncomp++;
         }

         /* Note that in some stages, like TCS, store_output is
          * lowered to memory writes, so no components of the
          * are "written" from the PoV of traditional store-
          * output instructions:
          */
         if (!ncomp)
            continue;

         struct ir3_builder build = ir3_builder_at(ir3_before_terminator(b));
         struct ir3_instruction *out =
            ir3_create_collect(&build, &ctx->outputs[i], ncomp);

         int outidx = i / 4;
         assert(outidx < so->outputs_count);

         outidxs[outputs_count] = outidx;
         outputs[outputs_count] = out;
         outputs_count++;
      }

      /* for a6xx+, binning and draw pass VS use same VBO state, so we
       * need to make sure not to remove any inputs that are used by
       * the nonbinning VS.
       */
      if (ctx->compiler->gen >= 6 && so->binning_pass &&
          so->type == MESA_SHADER_VERTEX) {
         for (int i = 0; i < ctx->ninputs; i++) {
            struct ir3_instruction *in = ctx->inputs[i];

            if (!in)
               continue;

            unsigned n = i / 4;
            unsigned c = i % 4;

            assert(n < so->nonbinning->inputs_count);

            if (so->nonbinning->inputs[n].sysval)
               continue;

            /* be sure to keep inputs, even if only used in VS */
            if (so->nonbinning->inputs[n].compmask & (1 << c))
               array_insert(in->block, in->block->keeps, in);
         }
      }

      struct ir3_instruction *end =
         ir3_build_instr(&ctx->build, OPC_END, 0, outputs_count);

      for (unsigned i = 0; i < outputs_count; i++) {
         __ssa_src(end, outputs[i], 0);
      }

      end->end.outidxs = ralloc_array(end, unsigned, outputs_count);
      memcpy(end->end.outidxs, outidxs, sizeof(unsigned) * outputs_count);

      array_insert(ctx->block, ctx->block->keeps, end);
   }

   if (so->type == MESA_SHADER_FRAGMENT &&
       ctx->s->info.fs.needs_coarse_quad_helper_invocations) {
      so->need_pixlod = true;
   }

   if (so->type == MESA_SHADER_FRAGMENT &&
       ctx->s->info.fs.needs_full_quad_helper_invocations) {
      so->need_full_quad = true;
   }

   /* If we're uploading immediates as part of the const state, we need to make
    * sure the binning and non-binning variants have the same size. Pre-allocate
    * for the binning variant, ir3_const_add_imm will ensure we don't add more
    * immediates than allowed.
    */
   if (so->binning_pass && !compiler->load_shader_consts_via_preamble &&
       so->nonbinning->imm_state.size) {
      ASSERTED bool success =
         ir3_const_ensure_imm_size(so, so->nonbinning->imm_state.size);
      assert(success);
   }

   ir3_debug_print(ir, "AFTER: nir->ir3");
   ir3_validate(ir);

   IR3_PASS(ir, ir3_remove_unreachable);

   IR3_PASS(ir, ir3_array_to_ssa);

   ir3_calc_reconvergence(so);

   IR3_PASS(ir, ir3_lower_shared_phis);

   do {
      progress = false;

      /* the folding doesn't seem to work reliably on a4xx */
      if (ctx->compiler->gen != 4)
         progress |= IR3_PASS(ir, ir3_cf, so);
      progress |= IR3_PASS(ir, ir3_cp, so, true);
      progress |= IR3_PASS(ir, ir3_cse);
      progress |= IR3_PASS(ir, ir3_dce, so);
      progress |= IR3_PASS(ir, ir3_opt_predicates, so);
      progress |= IR3_PASS(ir, ir3_shared_fold);
   } while (progress);

   progress = IR3_PASS(ir, ir3_create_alias_tex_regs);
   progress |= IR3_PASS(ir, ir3_create_alias_rt, so);

   if (IR3_PASS(ir, ir3_imm_const_to_preamble, so)) {
      progress = true;

      /* Propagate immediates created by ir3_imm_const_to_preamble but make sure
       * we don't lower any more immediates to const registers.
       */
      IR3_PASS(ir, ir3_cp, so, false);

      /* ir3_imm_const_to_preamble might create duplicate a1.x movs. */
      IR3_PASS(ir, ir3_cse);
   }

   if (progress) {
      IR3_PASS(ir, ir3_dce, so);
   }

   IR3_PASS(ir, ir3_sched_add_deps);

   /* At this point, all the dead code should be long gone: */
   assert(!IR3_PASS(ir, ir3_dce, so));

   ret = ir3_sched(ir);
   if (ret) {
      DBG("SCHED failed!");
      goto out;
   }

   ir3_debug_print(ir, "AFTER: ir3_sched");

   if (ctx->tcs_header) {
      /* We need to have these values in the same registers between VS and TCS
       * since the VS chains to TCS and doesn't get the sysvals redelivered.
       */

      ctx->tcs_header->dsts[0]->num = regid(0, 0);
      ctx->rel_patch_id->dsts[0]->num = regid(0, 1);
      if (ctx->primitive_id)
         ctx->primitive_id->dsts[0]->num = regid(0, 2);
   } else if (ctx->gs_header) {
      /* We need to have these values in the same registers between producer
       * (VS or DS) and GS since the producer chains to GS and doesn't get
       * the sysvals redelivered.
       */

      ctx->gs_header->dsts[0]->num = regid(0, 0);
      if (ctx->primitive_id)
         ctx->primitive_id->dsts[0]->num = regid(0, 1);
   } else if (so->num_sampler_prefetch) {
      assert(so->type == MESA_SHADER_FRAGMENT);
      int idx = 0;

      foreach_input (instr, ir) {
         if (instr->input.sysval !=
             (SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + so->prefetch_bary_type))
            continue;

         assert(idx < 2);
         instr->dsts[0]->num = idx;
         idx++;
      }
   }

   IR3_PASS(ir, ir3_cleanup_rpt, so);
   ret = ir3_ra(so);

   if (ret) {
      mesa_loge("ir3_ra() failed!");
      goto out;
   }

   IR3_PASS(ir, ir3_remove_noop_subreg_moves);
   IR3_PASS(ir, ir3_merge_rpt, so);
   IR3_PASS(ir, ir3_postsched, so);

   IR3_PASS(ir, ir3_legalize_relative);
   IR3_PASS(ir, ir3_lower_subgroups);

   /* This isn't valid to do when transform feedback is done in HW, which is
    * a4xx onward, because the VS may use components not read by the FS for
    * transform feedback. Ideally we'd delete this, but a5xx and earlier seem to
    * be broken without it.
    */
   if (so->type == MESA_SHADER_FRAGMENT && ctx->compiler->gen < 6)
      pack_inlocs(ctx);

   /*
    * Fixup inputs/outputs to point to the actual registers assigned:
    *
    * 1) initialize to r63.x (invalid/unused)
    * 2) iterate IR level inputs/outputs and update the variants
    *    inputs/outputs table based on the assigned registers for
    *    the remaining inputs/outputs.
    */

   for (unsigned i = 0; i < so->inputs_count; i++)
      so->inputs[i].regid = INVALID_REG;
   for (unsigned i = 0; i < so->outputs_count; i++)
      so->outputs[i].regid = INVALID_REG;

   struct ir3_instruction *end = ir3_find_end(so->ir);

   for (unsigned i = 0; i < end->srcs_count; i++) {
      unsigned outidx = end->end.outidxs[i];
      struct ir3_register *reg = end->srcs[i];

      so->outputs[outidx].regid = reg->num;
      so->outputs[outidx].half = !!(reg->flags & IR3_REG_HALF);
   }

   foreach_input (in, ir) {
      assert(in->opc == OPC_META_INPUT);
      unsigned inidx = in->input.inidx;
      so->inputs[inidx].regid = in->dsts[0]->num;
      so->inputs[inidx].half = !!(in->dsts[0]->flags & IR3_REG_HALF);
   }

   uint8_t clip_cull_mask = ctx->so->clip_mask | ctx->so->cull_mask;
   /* Having non-zero clip/cull mask and not writting corresponding regs
    * leads to a GPU fault on A7XX.
    */
   if (clip_cull_mask &&
       ir3_find_output_regid(ctx->so, VARYING_SLOT_CLIP_DIST0) == regid(63, 0)) {
      ctx->so->clip_mask &= 0xf0;
      ctx->so->cull_mask &= 0xf0;
   }
   if ((clip_cull_mask >> 4) &&
       ir3_find_output_regid(ctx->so, VARYING_SLOT_CLIP_DIST1) == regid(63, 0)) {
      ctx->so->clip_mask &= 0xf;
      ctx->so->cull_mask &= 0xf;
   }

   if (ctx->astc_srgb)
      fixup_astc_srgb(ctx);

   if (ctx->compiler->gen == 4 && ctx->s->info.uses_texture_gather)
      fixup_tg4(ctx);

   /* We need to do legalize after (for frag shader's) the "bary.f"
    * offsets (inloc) have been assigned.
    */
   IR3_PASS(ir, ir3_legalize, so, &max_bary);

   if (ctx->compiler->gen >= 7 && so->type == MESA_SHADER_COMPUTE) {
      struct ir3_instruction *end = ir3_find_end(so->ir);
      struct ir3_instruction *lock =
         ir3_build_instr(&ctx->build, OPC_LOCK, 0, 0);
      /* TODO: This flags should be set by scheduler only when needed */
      lock->flags = IR3_INSTR_SS | IR3_INSTR_SY | IR3_INSTR_JP;
      ir3_instr_move_before(lock, end);
      struct ir3_instruction *unlock =
         ir3_build_instr(&ctx->build, OPC_UNLOCK, 0, 0);
      ir3_instr_move_before(unlock, end);
   }

   so->pvtmem_size = ALIGN(so->pvtmem_size, compiler->pvtmem_per_fiber_align);

   /* Note that max_bary counts inputs that are not bary.f'd for FS: */
   if (so->type == MESA_SHADER_FRAGMENT)
      so->total_in = max_bary + 1;

   /* Collect sampling instructions eligible for pre-dispatch. */
   collect_tex_prefetches(ctx, ir);

   if ((ctx->so->type == MESA_SHADER_FRAGMENT) &&
       !ctx->s->info.fs.early_fragment_tests)
      ctx->so->no_earlyz |= ctx->s->info.writes_memory;

   if ((ctx->so->type == MESA_SHADER_FRAGMENT) &&
       ctx->s->info.fs.post_depth_coverage)
      so->post_depth_coverage = true;

   if (ctx->so->type == MESA_SHADER_FRAGMENT) {
      so->fs.depth_layout = ctx->s->info.fs.depth_layout;
   }

   ctx->so->sample_shading = ctx->s->info.fs.uses_sample_shading;

   if (ctx->has_relative_load_const_ir3) {
      /* NOTE: if relative addressing is used, we set
       * constlen in the compiler (to worst-case value)
       * since we don't know in the assembler what the max
       * addr reg value can be:
       */
      const struct ir3_const_state *const_state = ir3_const_state(ctx->so);
      const enum ir3_const_alloc_type rel_const_srcs[] = {
         IR3_CONST_ALLOC_INLINE_UNIFORM_ADDRS, IR3_CONST_ALLOC_UBO_RANGES,
         IR3_CONST_ALLOC_PREAMBLE, IR3_CONST_ALLOC_GLOBAL};
      for (int i = 0; i < ARRAY_SIZE(rel_const_srcs); i++) {
         const struct ir3_const_allocation *const_alloc =
            &const_state->allocs.consts[rel_const_srcs[i]];
         if (const_alloc->size_vec4 > 0) {
            ctx->so->constlen =
               MAX2(ctx->so->constlen,
                    const_alloc->offset_vec4 + const_alloc->size_vec4);
         }
      }
   }

   if (ctx->so->type == MESA_SHADER_FRAGMENT &&
       compiler->fs_must_have_non_zero_constlen_quirk) {
      so->constlen = MAX2(so->constlen, 4);
   }

   if (ctx->so->type == MESA_SHADER_VERTEX && ctx->compiler->gen >= 6) {
      so->constlen = MAX2(so->constlen, 8);
   }

   if (so->type == MESA_SHADER_FRAGMENT) {
      so->empty = is_empty(ir) && so->outputs_count == 0 &&
                  so->num_sampler_prefetch == 0;
      so->writes_only_color = !ctx->s->info.writes_memory && !so->has_kill &&
                              !so->writes_pos && !so->writes_smask &&
                              !so->writes_stencilref;
   }

   if (gl_shader_stage_is_compute(so->type)) {
      so->cs.local_invocation_id =
         ir3_find_sysval_regid(so, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
      so->cs.work_group_id =
         ir3_find_sysval_regid(so, SYSTEM_VALUE_WORKGROUP_ID);
   } else {
      so->vtxid_base = ir3_find_sysval_regid(so, SYSTEM_VALUE_VERTEX_ID_ZERO_BASE);
   }

out:
   if (ret) {
      if (so->ir)
         ir3_destroy(so->ir);
      so->ir = NULL;
   }
   ir3_context_free(ctx);

   return ret;
}
