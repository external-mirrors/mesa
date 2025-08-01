/*
 * Copyright (C) 2020 Google, Inc.
 * Copyright (C) 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"

/**
 * Lower mediump inputs and/or outputs to 16 bits.
 *
 * \param modes            Whether to lower inputs, outputs, or both.
 * \param varying_mask     Determines which varyings to skip (VS inputs,
 *    FS outputs, and patch varyings ignore this mask).
 * \param use_16bit_slots  Remap lowered slots to* VARYING_SLOT_VARn_16BIT.
 */
bool
nir_lower_mediump_io(nir_shader *nir, nir_variable_mode modes,
                     uint64_t varying_mask, bool use_16bit_slots)
{
   bool changed = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   assert(impl);

   nir_builder b = nir_builder_create(impl);

   nir_foreach_block_safe(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         nir_variable_mode mode;
         nir_intrinsic_instr *intr = nir_get_io_intrinsic(instr, modes, &mode);
         if (!intr)
            continue;

         nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
         nir_def *(*convert)(nir_builder *, nir_def *);
         bool is_varying = !(nir->info.stage == MESA_SHADER_VERTEX &&
                             mode == nir_var_shader_in) &&
                           !(nir->info.stage == MESA_SHADER_FRAGMENT &&
                             mode == nir_var_shader_out);

         if (is_varying && sem.location <= VARYING_SLOT_VAR31 &&
             !(varying_mask & BITFIELD64_BIT(sem.location))) {
            continue; /* can't lower */
         }

         if (nir_intrinsic_has_src_type(intr)) {
            /* Stores. */
            nir_alu_type type = nir_intrinsic_src_type(intr);

            nir_op upconvert_op;
            switch (type) {
            case nir_type_float32:
               convert = nir_f2fmp;
               upconvert_op = nir_op_f2f32;
               break;
            case nir_type_int32:
               convert = nir_i2imp;
               upconvert_op = nir_op_i2i32;
               break;
            case nir_type_uint32:
               convert = nir_i2imp;
               upconvert_op = nir_op_u2u32;
               break;
            default:
               continue; /* already lowered? */
            }

            /* Check that the output is mediump, or (for fragment shader
             * outputs) is a conversion from a mediump value, and lower it to
             * mediump.  Note that we don't automatically apply it to
             * gl_FragDepth, as GLSL ES declares it highp and so hardware such
             * as Adreno a6xx doesn't expect a half-float output for it.
             */
            nir_def *val = intr->src[0].ssa;
            bool is_fragdepth = (nir->info.stage == MESA_SHADER_FRAGMENT &&
                                 sem.location == FRAG_RESULT_DEPTH);
            if (!sem.medium_precision &&
                (is_varying || is_fragdepth || val->parent_instr->type != nir_instr_type_alu ||
                 nir_def_as_alu(val)->op != upconvert_op)) {
               continue;
            }

            /* Convert the 32-bit store into a 16-bit store. */
            b.cursor = nir_before_instr(&intr->instr);
            nir_src_rewrite(&intr->src[0], convert(&b, intr->src[0].ssa));
            nir_intrinsic_set_src_type(intr, (type & ~32) | 16);
         } else {
            if (!sem.medium_precision)
               continue;

            /* Loads. */
            nir_alu_type type = nir_intrinsic_dest_type(intr);

            switch (type) {
            case nir_type_float32:
               convert = nir_f2f32;
               break;
            case nir_type_int32:
               convert = nir_i2i32;
               break;
            case nir_type_uint32:
               convert = nir_u2u32;
               break;
            default:
               continue; /* already lowered? */
            }

            /* Convert the 32-bit load into a 16-bit load. */
            b.cursor = nir_after_instr(&intr->instr);
            intr->def.bit_size = 16;
            nir_intrinsic_set_dest_type(intr, (type & ~32) | 16);
            nir_def *dst = convert(&b, &intr->def);
            nir_def_rewrite_uses_after(&intr->def, dst);
         }

         if (use_16bit_slots && is_varying &&
             sem.location >= VARYING_SLOT_VAR0 &&
             sem.location <= VARYING_SLOT_VAR31) {
            unsigned index = sem.location - VARYING_SLOT_VAR0;

            sem.location = VARYING_SLOT_VAR0_16BIT + index / 2;
            sem.high_16bits = index % 2;
            nir_intrinsic_set_io_semantics(intr, sem);
         }
         changed = true;
      }
   }

   if (changed && use_16bit_slots)
      nir_recompute_io_bases(nir, modes);

   return nir_progress(changed, impl, nir_metadata_control_flow);
}

static bool
clear_mediump_io_flag(struct nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   /* The mediump flag must be preserved for XFB, but other IO
    * doesn't need it.
    */
   if (nir_intrinsic_has_io_semantics(intr) &&
       !nir_instr_xfb_write_mask(intr)) {
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);

      if (sem.medium_precision) {
         sem.medium_precision = 0;
         nir_intrinsic_set_io_semantics(intr, sem);
         return true;
      }
   }
   return false;
}

/* Set nir_io_semantics.medium_precision to 0 if it has no effect.
 *
 * This is recommended after nir_lower_mediump_io and before
 * nir_opt_varyings / nir_opt_vectorize_io.
 */
bool
nir_clear_mediump_io_flag(nir_shader *nir)
{
   return nir_shader_intrinsics_pass(nir, clear_mediump_io_flag, nir_metadata_all, NULL);
}

static bool
is_mediump_or_lowp(unsigned precision)
{
   return precision == GLSL_PRECISION_LOW || precision == GLSL_PRECISION_MEDIUM;
}

static bool
try_lower_mediump_var(nir_variable *var, nir_variable_mode modes, struct set *set)
{
   if (!(var->data.mode & modes) || !is_mediump_or_lowp(var->data.precision))
      return false;

   if (set && _mesa_set_search(set, var))
      return false;

   const struct glsl_type *new_type = glsl_type_to_16bit(var->type);
   if (var->type == new_type)
      return false;

   var->type = new_type;
   return true;
}

static bool
nir_lower_mediump_vars_impl(nir_function_impl *impl, nir_variable_mode modes,
                            bool any_lowered)
{
   bool progress = false;

   if (modes & nir_var_function_temp) {
      nir_foreach_function_temp_variable(var, impl) {
         any_lowered = try_lower_mediump_var(var, modes, NULL) || any_lowered;
      }
   }
   if (!any_lowered)
      return false;

   nir_builder b = nir_builder_create(impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         switch (instr->type) {
         case nir_instr_type_deref: {
            nir_deref_instr *deref = nir_instr_as_deref(instr);

            if (deref->modes & modes) {
               switch (deref->deref_type) {
               case nir_deref_type_var:
                  deref->type = deref->var->type;
                  break;
               case nir_deref_type_array:
               case nir_deref_type_array_wildcard:
                  deref->type = glsl_get_array_element(nir_deref_instr_parent(deref)->type);
                  break;
               case nir_deref_type_struct:
                  deref->type = glsl_get_struct_field(nir_deref_instr_parent(deref)->type, deref->strct.index);
                  break;
               default:
                  nir_print_instr(instr, stderr);
                  UNREACHABLE("unsupported deref type");
               }
            }

            break;
         }

         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_load_deref: {

               if (intrin->def.bit_size != 32)
                  break;

               nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
               if (glsl_get_bit_size(deref->type) != 16)
                  break;

               intrin->def.bit_size = 16;

               b.cursor = nir_after_instr(&intrin->instr);
               nir_def *replace = NULL;
               switch (glsl_get_base_type(deref->type)) {
               case GLSL_TYPE_FLOAT16:
                  replace = nir_f2f32(&b, &intrin->def);
                  break;
               case GLSL_TYPE_INT16:
                  replace = nir_i2i32(&b, &intrin->def);
                  break;
               case GLSL_TYPE_UINT16:
                  replace = nir_u2u32(&b, &intrin->def);
                  break;
               default:
                  UNREACHABLE("Invalid 16-bit type");
               }

               nir_def_rewrite_uses_after(&intrin->def, replace);
               progress = true;
               break;
            }

            case nir_intrinsic_store_deref: {
               nir_def *data = intrin->src[1].ssa;
               if (data->bit_size != 32)
                  break;

               nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
               if (glsl_get_bit_size(deref->type) != 16)
                  break;

               b.cursor = nir_before_instr(&intrin->instr);
               nir_def *replace = NULL;
               switch (glsl_get_base_type(deref->type)) {
               case GLSL_TYPE_FLOAT16:
                  replace = nir_f2fmp(&b, data);
                  break;
               case GLSL_TYPE_INT16:
               case GLSL_TYPE_UINT16:
                  replace = nir_i2imp(&b, data);
                  break;
               default:
                  UNREACHABLE("Invalid 16-bit type");
               }

               nir_src_rewrite(&intrin->src[1], replace);
               progress = true;
               break;
            }

            case nir_intrinsic_copy_deref: {
               nir_deref_instr *dst = nir_src_as_deref(intrin->src[0]);
               nir_deref_instr *src = nir_src_as_deref(intrin->src[1]);
               /* If we convert once side of a copy and not the other, that
                * would be very bad.
                */
               if (nir_deref_mode_may_be(dst, modes) ||
                   nir_deref_mode_may_be(src, modes)) {
                  assert(nir_deref_mode_must_be(dst, modes));
                  assert(nir_deref_mode_must_be(src, modes));
               }
               break;
            }

            default:
               break;
            }
            break;
         }

         default:
            break;
         }
      }
   }

   return nir_progress(progress, impl, nir_metadata_control_flow);
}

bool
nir_lower_mediump_vars(nir_shader *shader, nir_variable_mode modes)
{
   bool progress = false;

   if (modes & ~nir_var_function_temp) {
      /* Don't lower GLES mediump atomic ops to 16-bit -- no hardware is expecting that. */
      struct set *no_lower_set = _mesa_pointer_set_create(NULL);
      nir_foreach_block(block, nir_shader_get_entrypoint(shader)) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_deref_atomic:
            case nir_intrinsic_deref_atomic_swap: {
               nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
               nir_variable *var = nir_deref_instr_get_variable(deref);

               /* If we have atomic derefs that we can't track, then don't lower any mediump.  */
               if (!var) {
                  ralloc_free(no_lower_set);
                  return false;
               }

               _mesa_set_add(no_lower_set, var);
               break;
            }

            default:
               break;
            }
         }
      }

      nir_foreach_variable_in_shader(var, shader) {
         progress = try_lower_mediump_var(var, modes, no_lower_set) || progress;
      }

      ralloc_free(no_lower_set);
   }

   nir_foreach_function_impl(impl, shader) {
      if (nir_lower_mediump_vars_impl(impl, modes, progress))
         progress = true;
   }

   return progress;
}

/**
 * Fix types of source operands of texture opcodes according to
 * the constraints by inserting the appropriate conversion opcodes.
 *
 * For example, if the type of derivatives must be equal to texture
 * coordinates and the type of the texture bias must be 32-bit, there
 * will be 2 constraints describing that.
 */
static bool
legalize_16bit_sampler_srcs(nir_builder *b, nir_instr *instr, void *data)
{
   bool progress = false;
   nir_tex_src_type_constraint *constraints = data;

   if (instr->type != nir_instr_type_tex)
      return false;

   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int8_t map[nir_num_tex_src_types];
   memset(map, -1, sizeof(map));

   /* Create a mapping from src_type to src[i]. */
   for (unsigned i = 0; i < tex->num_srcs; i++)
      map[tex->src[i].src_type] = i;

   /* Legalize src types. */
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      nir_tex_src_type_constraint c = constraints[tex->src[i].src_type];

      if (!c.legalize_type)
         continue;

      /* Determine the required bit size for the src. */
      unsigned bit_size;
      if (c.bit_size) {
         bit_size = c.bit_size;
      } else {
         if (map[c.match_src] == -1)
            continue; /* e.g. txs */

         bit_size = tex->src[map[c.match_src]].src.ssa->bit_size;
      }

      /* Check if the type is legal. */
      if (bit_size == tex->src[i].src.ssa->bit_size)
         continue;

      /* Fix the bit size. */
      bool is_sint = nir_tex_instr_src_type(tex, i) == nir_type_int;
      bool is_uint = nir_tex_instr_src_type(tex, i) == nir_type_uint;
      nir_def *(*convert)(nir_builder *, nir_def *);

      switch (bit_size) {
      case 16:
         convert = is_sint ? nir_i2i16 : is_uint ? nir_u2u16
                                                 : nir_f2f16;
         break;
      case 32:
         convert = is_sint ? nir_i2i32 : is_uint ? nir_u2u32
                                                 : nir_f2f32;
         break;
      default:
         assert(!"unexpected bit size");
         continue;
      }

      b->cursor = nir_before_instr(&tex->instr);
      nir_src_rewrite(&tex->src[i].src, convert(b, tex->src[i].src.ssa));
      progress = true;
   }

   return progress;
}

bool
nir_legalize_16bit_sampler_srcs(nir_shader *nir,
                                nir_tex_src_type_constraints constraints)
{
   return nir_shader_instructions_pass(nir, legalize_16bit_sampler_srcs,
                                       nir_metadata_control_flow,
                                       constraints);
}

static bool
const_is_f16(nir_scalar scalar)
{
   double value = nir_scalar_as_float(scalar);
   uint16_t fp16_val = _mesa_float_to_half(value);
   bool is_denorm = (fp16_val & 0x7fff) != 0 && (fp16_val & 0x7fff) <= 0x3ff;
   return value == _mesa_half_to_float(fp16_val) && !is_denorm;
}

static bool
const_is_u16(nir_scalar scalar)
{
   uint64_t value = nir_scalar_as_uint(scalar);
   return value == (uint16_t)value;
}

static bool
const_is_i16(nir_scalar scalar)
{
   int64_t value = nir_scalar_as_int(scalar);
   return value == (int16_t)value;
}

static bool
can_opt_16bit_src(nir_def *ssa, nir_alu_type src_type, bool sext_matters)
{
   bool opt_f16 = src_type == nir_type_float32;
   bool opt_u16 = src_type == nir_type_uint32 && sext_matters;
   bool opt_i16 = src_type == nir_type_int32 && sext_matters;
   bool opt_i16_u16 = (src_type == nir_type_uint32 || src_type == nir_type_int32) && !sext_matters;

   bool can_opt = opt_f16 || opt_u16 || opt_i16 || opt_i16_u16;
   for (unsigned i = 0; can_opt && i < ssa->num_components; i++) {
      nir_scalar comp = nir_scalar_resolved(ssa, i);
      if (nir_scalar_is_undef(comp))
         continue;
      else if (nir_scalar_is_const(comp)) {
         if (opt_f16)
            can_opt &= const_is_f16(comp);
         else if (opt_u16)
            can_opt &= const_is_u16(comp);
         else if (opt_i16)
            can_opt &= const_is_i16(comp);
         else if (opt_i16_u16)
            can_opt &= (const_is_u16(comp) || const_is_i16(comp));
      } else if (nir_scalar_is_alu(comp)) {
         nir_alu_instr *alu = nir_def_as_alu(comp.def);
         bool is_16bit = alu->src[0].src.ssa->bit_size == 16;

         if ((alu->op == nir_op_f2f32 && is_16bit) ||
             alu->op == nir_op_unpack_half_2x16_split_x ||
             alu->op == nir_op_unpack_half_2x16_split_y)
            can_opt &= opt_f16;
         else if (alu->op == nir_op_i2i32 && is_16bit)
            can_opt &= opt_i16 || opt_i16_u16;
         else if (alu->op == nir_op_u2u32 && is_16bit)
            can_opt &= opt_u16 || opt_i16_u16;
         else
            return false;
      } else {
         return false;
      }
   }

   return can_opt;
}

static void
opt_16bit_src(nir_builder *b, nir_instr *instr, nir_src *src, nir_alu_type src_type)
{
   b->cursor = nir_before_instr(instr);

   nir_scalar new_comps[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < src->ssa->num_components; i++) {
      nir_scalar comp = nir_scalar_resolved(src->ssa, i);

      if (nir_scalar_is_undef(comp))
         new_comps[i] = nir_get_scalar(nir_undef(b, 1, 16), 0);
      else if (nir_scalar_is_const(comp)) {
         nir_def *constant;
         if (src_type == nir_type_float32)
            constant = nir_imm_float16(b, nir_scalar_as_float(comp));
         else
            constant = nir_imm_intN_t(b, nir_scalar_as_uint(comp), 16);
         new_comps[i] = nir_get_scalar(constant, 0);
      } else {
         /* conversion instruction */
         new_comps[i] = nir_scalar_chase_alu_src(comp, 0);
         if (new_comps[i].def->bit_size != 16) {
            assert(new_comps[i].def->bit_size == 32);

            nir_def *extract = nir_mov_scalar(b, new_comps[i]);
            switch (nir_scalar_alu_op(comp)) {
            case nir_op_unpack_half_2x16_split_x:
               extract = nir_unpack_32_2x16_split_x(b, extract);
               break;
            case nir_op_unpack_half_2x16_split_y:
               extract = nir_unpack_32_2x16_split_y(b, extract);
               break;
            default:
               UNREACHABLE("unsupported alu op");
            }

            new_comps[i] = nir_get_scalar(extract, 0);
         }
      }
   }

   nir_def *new_vec = nir_vec_scalars(b, new_comps, src->ssa->num_components);

   nir_src_rewrite(src, new_vec);
}

static bool
opt_16bit_store_data(nir_builder *b, nir_intrinsic_instr *instr)
{
   nir_alu_type src_type = nir_intrinsic_src_type(instr);
   nir_src *data_src = &instr->src[3];

   b->cursor = nir_before_instr(&instr->instr);

   if (!can_opt_16bit_src(data_src->ssa, src_type, true))
      return false;

   opt_16bit_src(b, &instr->instr, data_src, src_type);

   nir_intrinsic_set_src_type(instr, (src_type & ~32) | 16);

   return true;
}

static bool
opt_16bit_destination(nir_def *ssa, nir_alu_type dest_type, unsigned exec_mode,
                      struct nir_opt_16bit_tex_image_options *options)
{
   bool opt_f2f16 = dest_type == nir_type_float32;
   bool opt_i2i16 = (dest_type == nir_type_int32 || dest_type == nir_type_uint32) &&
                    !options->integer_dest_saturates;
   bool opt_i2i16_sat = dest_type == nir_type_int32 && options->integer_dest_saturates;
   bool opt_u2u16_sat = dest_type == nir_type_uint32 && options->integer_dest_saturates;

   nir_rounding_mode rdm = options->rounding_mode;
   nir_rounding_mode src_rdm =
      nir_get_rounding_mode_from_float_controls(exec_mode, nir_type_float16);

   nir_foreach_use(use, ssa) {
      nir_instr *instr = nir_src_parent_instr(use);
      if (instr->type != nir_instr_type_alu)
         return false;

      nir_alu_instr *alu = nir_instr_as_alu(instr);

      switch (alu->op) {
      case nir_op_pack_half_2x16_split:
         if (alu->src[0].src.ssa != alu->src[1].src.ssa)
            return false;
         FALLTHROUGH;
      case nir_op_pack_half_2x16:
         /* pack_half rounding is undefined */
         if (!opt_f2f16)
            return false;
         break;
      case nir_op_pack_half_2x16_rtz_split:
         if (alu->src[0].src.ssa != alu->src[1].src.ssa)
            return false;
         FALLTHROUGH;
      case nir_op_f2f16_rtz:
         if (rdm != nir_rounding_mode_rtz || !opt_f2f16)
            return false;
         break;
      case nir_op_f2f16_rtne:
         if (rdm != nir_rounding_mode_rtne || !opt_f2f16)
            return false;
         break;
      case nir_op_f2f16:
      case nir_op_f2fmp:
         if (src_rdm != rdm && src_rdm != nir_rounding_mode_undef)
            return false;
         if (!opt_f2f16)
            return false;
         break;
      case nir_op_i2i16:
      case nir_op_i2imp:
      case nir_op_u2u16:
         if (!opt_i2i16)
            return false;
         break;
      case nir_op_pack_sint_2x16:
         if (!opt_i2i16_sat)
            return false;
         break;
      case nir_op_pack_uint_2x16:
         if (!opt_u2u16_sat)
            return false;
         break;
      default:
         return false;
      }
   }

   /* All uses are the same conversions. Replace them with mov. */
   nir_foreach_use(use, ssa) {
      nir_alu_instr *alu = nir_instr_as_alu(nir_src_parent_instr(use));
      switch (alu->op) {
      case nir_op_f2f16_rtne:
      case nir_op_f2f16_rtz:
      case nir_op_f2f16:
      case nir_op_f2fmp:
      case nir_op_i2i16:
      case nir_op_i2imp:
      case nir_op_u2u16:
         alu->op = nir_op_mov;
         break;
      case nir_op_pack_half_2x16_rtz_split:
      case nir_op_pack_half_2x16_split:
         alu->op = nir_op_pack_32_2x16_split;
         break;
      case nir_op_pack_32_2x16_split:
         /* Split opcodes have two operands, so the iteration
          * for the second use will already observe the
          * updated opcode.
          */
         break;
      case nir_op_pack_half_2x16:
      case nir_op_pack_sint_2x16:
      case nir_op_pack_uint_2x16:
         alu->op = nir_op_pack_32_2x16;
         break;
      default:
         UNREACHABLE("unsupported conversion op");
      };
   }

   ssa->bit_size = 16;
   return true;
}

static bool
opt_16bit_image_dest(nir_intrinsic_instr *instr, unsigned exec_mode,
                     struct nir_opt_16bit_tex_image_options *options)
{
   nir_alu_type dest_type = nir_intrinsic_dest_type(instr);

   if (!(nir_alu_type_get_base_type(dest_type) & options->opt_image_dest_types))
      return false;

   if (!opt_16bit_destination(&instr->def, dest_type, exec_mode, options))
      return false;

   nir_intrinsic_set_dest_type(instr, (dest_type & ~32) | 16);

   return true;
}

static bool
opt_16bit_tex_dest(nir_tex_instr *tex, unsigned exec_mode,
                   struct nir_opt_16bit_tex_image_options *options)
{
   /* Skip sparse residency */
   if (tex->is_sparse)
      return false;

   if (tex->op != nir_texop_tex &&
       tex->op != nir_texop_txb &&
       tex->op != nir_texop_txd &&
       tex->op != nir_texop_txl &&
       tex->op != nir_texop_txf &&
       tex->op != nir_texop_txf_ms &&
       tex->op != nir_texop_tg4 &&
       tex->op != nir_texop_tex_prefetch &&
       tex->op != nir_texop_fragment_fetch_amd)
      return false;

   if (!(nir_alu_type_get_base_type(tex->dest_type) & options->opt_tex_dest_types))
      return false;

   if (!opt_16bit_destination(&tex->def, tex->dest_type, exec_mode, options))
      return false;

   tex->dest_type = (tex->dest_type & ~32) | 16;
   return true;
}

static bool
opt_16bit_tex_srcs(nir_builder *b, nir_tex_instr *tex,
                   struct nir_opt_tex_srcs_options *options)
{
   if (tex->op != nir_texop_tex &&
       tex->op != nir_texop_txb &&
       tex->op != nir_texop_txd &&
       tex->op != nir_texop_txl &&
       tex->op != nir_texop_txf &&
       tex->op != nir_texop_txf_ms &&
       tex->op != nir_texop_tg4 &&
       tex->op != nir_texop_tex_prefetch &&
       tex->op != nir_texop_fragment_fetch_amd &&
       tex->op != nir_texop_fragment_mask_fetch_amd)
      return false;

   if (!(options->sampler_dims & BITFIELD_BIT(tex->sampler_dim)))
      return false;

   if (nir_tex_instr_src_index(tex, nir_tex_src_backend1) >= 0)
      return false;

   unsigned opt_srcs = 0;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      /* Filter out sources that should be ignored. */
      if (!(BITFIELD_BIT(tex->src[i].src_type) & options->src_types))
         continue;

      nir_src *src = &tex->src[i].src;

      nir_alu_type src_type = nir_tex_instr_src_type(tex, i) | src->ssa->bit_size;

      /* Zero-extension (u16) and sign-extension (i16) have
       * the same behavior here - txf returns 0 if bit 15 is set
       * because it's out of bounds and the higher bits don't
       * matter. With the exception of a texel buffer, which could
       * be arbitrary large.
       */
      bool sext_matters = tex->sampler_dim == GLSL_SAMPLER_DIM_BUF;
      if (!can_opt_16bit_src(src->ssa, src_type, sext_matters))
         return false;

      opt_srcs |= (1 << i);
   }

   u_foreach_bit(i, opt_srcs) {
      nir_src *src = &tex->src[i].src;
      nir_alu_type src_type = nir_tex_instr_src_type(tex, i) | src->ssa->bit_size;
      opt_16bit_src(b, &tex->instr, src, src_type);
   }

   return !!opt_srcs;
}

static bool
opt_16bit_image_srcs(nir_builder *b, nir_intrinsic_instr *instr, int lod_idx)
{
   enum glsl_sampler_dim dim = nir_intrinsic_image_dim(instr);
   bool is_ms = (dim == GLSL_SAMPLER_DIM_MS || dim == GLSL_SAMPLER_DIM_SUBPASS_MS);
   nir_src *coords = &instr->src[1];
   nir_src *sample = is_ms ? &instr->src[2] : NULL;
   nir_src *lod = lod_idx >= 0 ? &instr->src[lod_idx] : NULL;

   if (dim == GLSL_SAMPLER_DIM_BUF ||
       !can_opt_16bit_src(coords->ssa, nir_type_int32, false) ||
       (sample && !can_opt_16bit_src(sample->ssa, nir_type_int32, false)) ||
       (lod && !can_opt_16bit_src(lod->ssa, nir_type_int32, false)))
      return false;

   opt_16bit_src(b, &instr->instr, coords, nir_type_int32);
   if (sample)
      opt_16bit_src(b, &instr->instr, sample, nir_type_int32);
   if (lod)
      opt_16bit_src(b, &instr->instr, lod, nir_type_int32);

   return true;
}

static bool
opt_16bit_tex_image(nir_builder *b, nir_instr *instr, void *params)
{
   struct nir_opt_16bit_tex_image_options *options = params;
   unsigned exec_mode = b->shader->info.float_controls_execution_mode;
   bool progress = false;

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instr);

      switch (intrinsic->intrinsic) {
      case nir_intrinsic_bindless_image_store:
      case nir_intrinsic_image_deref_store:
      case nir_intrinsic_image_store:
         if (options->opt_image_store_data)
            progress |= opt_16bit_store_data(b, intrinsic);
         if (options->opt_image_srcs)
            progress |= opt_16bit_image_srcs(b, intrinsic, 4);
         break;
      case nir_intrinsic_bindless_image_load:
      case nir_intrinsic_image_deref_load:
      case nir_intrinsic_image_load:
         if (options->opt_image_dest_types)
            progress |= opt_16bit_image_dest(intrinsic, exec_mode, options);
         if (options->opt_image_srcs)
            progress |= opt_16bit_image_srcs(b, intrinsic, 3);
         break;
      case nir_intrinsic_bindless_image_sparse_load:
      case nir_intrinsic_image_deref_sparse_load:
      case nir_intrinsic_image_sparse_load:
         if (options->opt_image_srcs)
            progress |= opt_16bit_image_srcs(b, intrinsic, 3);
         break;
      case nir_intrinsic_bindless_image_atomic:
      case nir_intrinsic_bindless_image_atomic_swap:
      case nir_intrinsic_image_deref_atomic:
      case nir_intrinsic_image_deref_atomic_swap:
      case nir_intrinsic_image_atomic:
      case nir_intrinsic_image_atomic_swap:
         if (options->opt_image_srcs)
            progress |= opt_16bit_image_srcs(b, intrinsic, -1);
         break;
      default:
         break;
      }
   } else if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);

      if (options->opt_tex_dest_types)
         progress |= opt_16bit_tex_dest(tex, exec_mode, options);

      for (unsigned i = 0; i < options->opt_srcs_options_count; i++) {
         progress |= opt_16bit_tex_srcs(b, tex, &options->opt_srcs_options[i]);
      }
   }

   return progress;
}

bool
nir_opt_16bit_tex_image(nir_shader *nir,
                        struct nir_opt_16bit_tex_image_options *options)
{
   return nir_shader_instructions_pass(nir,
                                       opt_16bit_tex_image,
                                       nir_metadata_control_flow,
                                       options);
}
