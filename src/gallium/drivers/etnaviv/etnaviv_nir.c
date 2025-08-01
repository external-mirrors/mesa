/*
 * Copyright (c) 2019 Zodiac Inflight Innovations
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Jonathan Marek <jonathan@marek.ca>
 */

#include "etnaviv_nir.h"

static inline int
color_index_for_location(unsigned location)
{
   assert(location != FRAG_RESULT_COLOR &&
          "gl_FragColor must be lowered before nir_lower_blend");

   if (location < FRAG_RESULT_DATA0)
      return -1;
   else
      return location - FRAG_RESULT_DATA0;
}

/* io related lowering
 * run after lower_int_to_float because it adds i2f/f2i ops
 */
bool
etna_lower_io(nir_shader *shader, struct etna_shader_variant *v)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      nir_builder b = nir_builder_create(impl);
      bool func_progress = false;

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

               switch (intr->intrinsic) {
               case nir_intrinsic_load_front_face: {
                  /* HW front_face is 0.0/1.0, not 0/~0u for bool
                   * lower with a comparison with 0
                   */
                  intr->def.bit_size = 32;

                  b.cursor = nir_after_instr(instr);

                  nir_def *ssa = nir_ine_imm(&b, &intr->def, 0);
                  if (v->key.front_ccw)
                     nir_def_as_alu(ssa)->op = nir_op_ieq;

                  nir_def_rewrite_uses_after(&intr->def, ssa);

                  func_progress = true;
               } break;
               case nir_intrinsic_store_deref: {
                  nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
                  if (shader->info.stage != MESA_SHADER_FRAGMENT || !v->key.frag_rb_swap)
                     break;

                  assert(deref->deref_type == nir_deref_type_var);

                  int rt = color_index_for_location(deref->var->data.location);
                  if (rt == -1)
                     break;

                  if (!(v->key.frag_rb_swap & (1 << rt)))
                     break;

                  b.cursor = nir_before_instr(instr);

                  nir_def *ssa = nir_mov(&b, intr->src[1].ssa);
                  nir_alu_instr *alu = nir_def_as_alu(ssa);
                  alu->src[0].swizzle[0] = 2;
                  alu->src[0].swizzle[2] = 0;
                  nir_src_rewrite(&intr->src[1], ssa);

                  func_progress = true;
               } break;
               case nir_intrinsic_load_vertex_id:
               case nir_intrinsic_load_instance_id:
                  /* detect use of vertex_id/instance_id */
                  v->vs_id_in_reg = v->infile.num_reg;
                  break;
               default:
                  break;
               }
            }
         }
      }

      nir_progress(func_progress, impl, nir_metadata_none);

      progress |= func_progress;
   }

   return progress;
}

static bool
etna_lower_alu_impl(nir_function_impl *impl, bool has_new_transcendentals)
{
   nir_shader *shader = impl->function->shader;
   bool progress = false;

   nir_builder b = nir_builder_create(impl);

   /* in a seperate loop so we can apply the multiple-uniform logic to the new fmul */
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;

         nir_alu_instr *alu = nir_instr_as_alu(instr);
         /* multiply sin/cos src by constant
          * TODO: do this earlier (but it breaks const_prop opt)
          */
         if (alu->op == nir_op_fsin || alu->op == nir_op_fcos) {
            b.cursor = nir_before_instr(instr);

            nir_def *imm = has_new_transcendentals ?
               nir_imm_float(&b, 1.0 / M_PI) :
               nir_imm_float(&b, 2.0 / M_PI);

            nir_src_rewrite(&alu->src[0].src,
                            nir_fmul(&b, alu->src[0].src.ssa, imm));

            progress = true;
         }

         /* change transcendental ops to vec2 and insert vec1 mul for the result
          * TODO: do this earlier (but it breaks with optimizations)
          */
         if (has_new_transcendentals && (
             alu->op == nir_op_fdiv || alu->op == nir_op_flog2 ||
             alu->op == nir_op_fsin || alu->op == nir_op_fcos)) {
            nir_def *ssa = &alu->def;

            assert(ssa->num_components == 1);

            nir_alu_instr *mul = nir_alu_instr_create(shader, nir_op_fmul);
            mul->src[0].src = mul->src[1].src = nir_src_for_ssa(ssa);
            mul->src[1].swizzle[0] = 1;

            nir_def_init(&mul->instr, &mul->def, 1, 32);

            alu->src[0].swizzle[1] = 0;
            ssa->num_components = 2;

            nir_instr_insert_after(instr, &mul->instr);

            nir_def_rewrite_uses_after(ssa, &mul->def);
            progress = true;
         }
      }
   }

   return nir_progress(progress, impl, nir_metadata_none);
}

bool
etna_lower_alu(nir_shader *shader, bool has_new_transcendentals)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      progress |= etna_lower_alu_impl(impl, has_new_transcendentals);
   }

   return progress;
}
