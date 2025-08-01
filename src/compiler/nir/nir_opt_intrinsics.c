/*
 * Copyright © 2017 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"
#include "nir_search_helpers.h"

/**
 * \file nir_opt_intrinsics.c
 */

static bool
src_is_single_use_shuffle(nir_src src, nir_def **data, nir_def **index)
{
   nir_intrinsic_instr *shuffle = nir_src_as_intrinsic(src);
   if (shuffle == NULL || shuffle->intrinsic != nir_intrinsic_shuffle)
      return false;

   /* This is only called when src is part of an ALU op so requiring no if
    * uses is reasonable.  If we ever want to use this from an if statement,
    * we can change it then.
    */
   if (!list_is_singular(&shuffle->def.uses))
      return false;

   if (nir_def_used_by_if(&shuffle->def))
      return false;

   *data = shuffle->src[0].ssa;
   *index = shuffle->src[1].ssa;

   return true;
}

static nir_def *
try_opt_bcsel_of_shuffle(nir_builder *b, nir_alu_instr *alu,
                         bool block_has_discard)
{
   assert(alu->op == nir_op_bcsel);

   /* If we've seen a discard in this block, don't do the optimization.  We
    * could try to do something fancy where we check if the shuffle is on our
    * side of the discard or not but this is good enough for correctness for
    * now and subgroup ops in the presence of discard aren't common.
    */
   if (block_has_discard)
      return NULL;

   if (!nir_alu_src_is_trivial_ssa(alu, 0))
      return NULL;

   nir_def *data1, *index1;
   if (!nir_alu_src_is_trivial_ssa(alu, 1) ||
       nir_def_block(alu->src[1].src.ssa) != alu->instr.block ||
       !src_is_single_use_shuffle(alu->src[1].src, &data1, &index1))
      return NULL;

   nir_def *data2, *index2;
   if (!nir_alu_src_is_trivial_ssa(alu, 2) ||
       nir_def_block(alu->src[2].src.ssa) != alu->instr.block ||
       !src_is_single_use_shuffle(alu->src[2].src, &data2, &index2))
      return NULL;

   if (data1 != data2)
      return NULL;

   nir_def *index = nir_bcsel(b, alu->src[0].src.ssa, index1, index2);
   nir_def *shuffle = nir_shuffle(b, data1, index);

   return shuffle;
}

/* load_front_face ? a : -a -> load_front_face_sign * a */
static nir_def *
try_opt_front_face_fsign(nir_builder *b, nir_alu_instr *alu)
{
   if (alu->def.bit_size != 32 ||
       !nir_src_as_intrinsic(alu->src[0].src) ||
       nir_src_as_intrinsic(alu->src[0].src)->intrinsic != nir_intrinsic_load_front_face ||
       !is_only_used_as_float(alu) ||
       !nir_alu_srcs_negative_equal_typed(alu, alu, 1, 2, nir_type_float))
      return NULL;

   nir_def *src = nir_ssa_for_alu_src(b, alu, 1);

   return nir_fmul(b, nir_load_front_face_fsign(b), src);
}

static bool
src_is_quad_broadcast(nir_block *block, nir_src src, nir_intrinsic_instr **intrin)
{
   nir_intrinsic_instr *broadcast = nir_src_as_intrinsic(src);
   if (broadcast == NULL || broadcast->instr.block != block)
      return false;

   switch (broadcast->intrinsic) {
   case nir_intrinsic_quad_broadcast:
      if (!nir_src_is_const(broadcast->src[1]))
         return false;
      FALLTHROUGH;
   case nir_intrinsic_quad_swap_horizontal:
   case nir_intrinsic_quad_swap_vertical:
   case nir_intrinsic_quad_swap_diagonal:
   case nir_intrinsic_quad_swizzle_amd:
      *intrin = broadcast;
      return true;
   default:
      return false;
   }
}

static bool
src_is_alu(nir_op op, nir_src src, nir_src srcs[2])
{
   nir_alu_instr *alu = nir_src_as_alu_instr(src);
   if (alu == NULL || alu->op != op)
      return false;

   if (!nir_alu_src_is_trivial_ssa(alu, 0) || !nir_alu_src_is_trivial_ssa(alu, 1))
      return false;

   srcs[0] = alu->src[0].src;
   srcs[1] = alu->src[1].src;

   return true;
}

static nir_def *
try_opt_quad_vote(nir_builder *b, nir_alu_instr *alu, bool block_has_discard)
{
   if (block_has_discard)
      return NULL;

   if (!nir_alu_src_is_trivial_ssa(alu, 0) || !nir_alu_src_is_trivial_ssa(alu, 1))
      return NULL;

   nir_intrinsic_instr *quad_broadcasts[4];
   nir_src srcs[2][2];
   bool found = false;

   /* Match (broadcast0 op broadcast1) op (broadcast2 op broadcast3). */
   found = src_is_alu(alu->op, alu->src[0].src, srcs[0]) &&
           src_is_alu(alu->op, alu->src[1].src, srcs[1]) &&
           src_is_quad_broadcast(alu->instr.block, srcs[0][0], &quad_broadcasts[0]) &&
           src_is_quad_broadcast(alu->instr.block, srcs[0][1], &quad_broadcasts[1]) &&
           src_is_quad_broadcast(alu->instr.block, srcs[1][0], &quad_broadcasts[2]) &&
           src_is_quad_broadcast(alu->instr.block, srcs[1][1], &quad_broadcasts[3]);

   /* Match ((broadcast2 op broadcast3) op broadcast1) op broadcast0). */
   if (!found) {
      if ((src_is_alu(alu->op, alu->src[0].src, srcs[0]) &&
           src_is_quad_broadcast(alu->instr.block, alu->src[1].src, &quad_broadcasts[0])) ||
          (src_is_alu(alu->op, alu->src[1].src, srcs[0]) &&
           src_is_quad_broadcast(alu->instr.block, alu->src[0].src, &quad_broadcasts[0]))) {
         /* ((broadcast2 || broadcast3) || broadcast1) */
         if ((src_is_alu(alu->op, srcs[0][0], srcs[1]) &&
              src_is_quad_broadcast(alu->instr.block, srcs[0][1], &quad_broadcasts[1])) ||
             (src_is_alu(alu->op, srcs[0][1], srcs[1]) &&
              src_is_quad_broadcast(alu->instr.block, srcs[0][0], &quad_broadcasts[1]))) {
            /* (broadcast2 || broadcast3) */
            found = src_is_quad_broadcast(alu->instr.block, srcs[1][0], &quad_broadcasts[2]) &&
                    src_is_quad_broadcast(alu->instr.block, srcs[1][1], &quad_broadcasts[3]);
         }
      }
   }

   if (!found)
      return NULL;

   /* Check if each lane in a quad reduces all lanes in the quad, and if all broadcasts read the
    * same data.
    */
   uint16_t lanes_read = 0;
   for (unsigned i = 0; i < 4; i++) {
      if (!nir_srcs_equal(quad_broadcasts[i]->src[0], quad_broadcasts[0]->src[0]))
         return NULL;

      for (unsigned j = 0; j < 4; j++) {
         unsigned lane;
         switch (quad_broadcasts[i]->intrinsic) {
         case nir_intrinsic_quad_broadcast:
            lane = nir_src_as_uint(quad_broadcasts[i]->src[1]) & 0x3;
            break;
         case nir_intrinsic_quad_swap_horizontal:
            lane = j ^ 1;
            break;
         case nir_intrinsic_quad_swap_vertical:
            lane = j ^ 2;
            break;
         case nir_intrinsic_quad_swap_diagonal:
            lane = 3 - j;
            break;
         case nir_intrinsic_quad_swizzle_amd:
            lane = (nir_intrinsic_swizzle_mask(quad_broadcasts[i]) >> (j * 2)) & 0x3;
            break;
         default:
            UNREACHABLE("");
         }
         lanes_read |= (1 << lane) << (j * 4);
      }
   }

   if (lanes_read != 0xffff)
      return NULL;

   /* Create quad vote. */
   if (alu->op == nir_op_iand)
      return nir_quad_vote_all(b, 1, quad_broadcasts[0]->src[0].ssa);
   else
      return nir_quad_vote_any(b, 1, quad_broadcasts[0]->src[0].ssa);
}

static bool
opt_intrinsics_alu(nir_builder *b, nir_alu_instr *alu,
                   bool block_has_discard, const struct nir_shader_compiler_options *options)
{
   nir_def *replacement = NULL;

   switch (alu->op) {
   case nir_op_bcsel:
      replacement = try_opt_bcsel_of_shuffle(b, alu, block_has_discard);
      if (!replacement && options->optimize_load_front_face_fsign)
         replacement = try_opt_front_face_fsign(b, alu);
      break;
   case nir_op_iand:
   case nir_op_ior:
      if (alu->def.bit_size == 1 && options->optimize_quad_vote_to_reduce)
         replacement = try_opt_quad_vote(b, alu, block_has_discard);
      break;
   default:
      break;
   }

   if (replacement) {
      nir_def_replace(&alu->def, replacement);
      return true;
   } else {
      return false;
   }
}

static bool
try_opt_exclusive_scan_to_inclusive(nir_builder *b, nir_intrinsic_instr *intrin)
{
   if (intrin->def.num_components != 1)
      return false;

   nir_op reduction_op = nir_intrinsic_reduction_op(intrin);

   nir_foreach_use_including_if(src, &intrin->def) {
      if (nir_src_is_if(src) || nir_src_parent_instr(src)->type != nir_instr_type_alu)
         return false;

      nir_alu_instr *alu = nir_instr_as_alu(nir_src_parent_instr(src));

      if (alu->op != reduction_op)
         return false;

      /* Don't reassociate exact float operations. */
      if (nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type) == nir_type_float && alu->exact)
         return false;

      /* SPIR-V rules for fmax/fmin scans are *very* stupid.
       * The required identity is Inf instead of NaN but if one input
       * is NaN, the other value has to be returned.
       *
       * This means for invocation 0:
       * min(subgroupExclusiveMin(NaN), NaN) -> Inf
       * subgroupInclusiveMin(NaN) -> undefined (NaN for any sane backend)
       *
       * SPIR-V [NF]Min/Max don't allow undefined result, even with standard
       * float controls.
       */
      if (alu->op == nir_op_fmax || alu->op == nir_op_fmin)
         return false;

      if (alu->def.num_components != 1)
         return false;

      nir_alu_src *alu_src = list_entry(src, nir_alu_src, src);
      unsigned src_index = alu_src - alu->src;

      assert(src_index < 2 && nir_op_infos[alu->op].num_inputs == 2);

      nir_scalar scan_scalar = nir_scalar_resolved(intrin->src[0].ssa, 0);
      nir_scalar op_scalar = nir_scalar_resolved(alu->src[!src_index].src.ssa,
                                                 alu->src[!src_index].swizzle[0]);

      if (!nir_scalar_equal(scan_scalar, op_scalar))
         return false;
   }

   /* Convert to inclusive scan. */
   nir_def *incl_scan = nir_inclusive_scan(b, intrin->src[0].ssa, .reduction_op = reduction_op);

   nir_foreach_use_including_if_safe(src, &intrin->def) {
      /* Remove alu. */
      nir_alu_instr *alu = nir_instr_as_alu(nir_src_parent_instr(src));
      nir_def_replace(&alu->def, incl_scan);
   }

   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
opt_intrinsics_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                      const struct nir_shader_compiler_options *options)
{
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_sample_mask_in: {
      /* Transform:
       *   gl_SampleMaskIn == 0 ---> gl_HelperInvocation
       *   gl_SampleMaskIn != 0 ---> !gl_HelperInvocation
       */
      if (!options->optimize_sample_mask_in)
         return false;

      bool progress = false;
      nir_foreach_use_safe(use_src, &intrin->def) {
         if (nir_src_parent_instr(use_src)->type == nir_instr_type_alu) {
            nir_alu_instr *alu = nir_instr_as_alu(nir_src_parent_instr(use_src));

            if ((alu->op != nir_op_ieq && alu->op != nir_op_ine) || alu->def.num_components != 1)
               continue;

            nir_alu_src *alu_src = list_entry(use_src, nir_alu_src, src);
            unsigned src_index = alu_src - alu->src;
            nir_scalar other = nir_scalar_chase_alu_src(nir_get_scalar(&alu->def, 0), !src_index);

            if (!nir_scalar_is_const(other) || nir_scalar_as_uint(other))
               continue;

            nir_cf_node *cf_node = &intrin->instr.block->cf_node;
            while (cf_node->parent)
               cf_node = cf_node->parent;

            nir_function_impl *func_impl = nir_cf_node_as_function(cf_node);

            /* We need to insert load_helper before any demote,
             * which is only possible in the entry point function
             */
            if (func_impl != nir_shader_get_entrypoint(b->shader))
               break;

            b->cursor = nir_before_impl(func_impl);

            nir_def *new_expr = nir_load_helper_invocation(b, 1);

            if (alu->op == nir_op_ine)
               new_expr = nir_inot(b, new_expr);

            nir_def_replace(&alu->def, new_expr);
            progress = true;
         }
      }
      return progress;
   }
   case nir_intrinsic_exclusive_scan:
      return try_opt_exclusive_scan_to_inclusive(b, intrin);
   default:
      return false;
   }
}

static bool
opt_intrinsics_impl(nir_function_impl *impl,
                    const struct nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_create(impl);
   bool progress = false;

   nir_foreach_block(block, impl) {
      bool block_has_discard = false;

      nir_foreach_instr_safe(instr, block) {
         b.cursor = nir_before_instr(instr);

         switch (instr->type) {
         case nir_instr_type_alu:
            if (opt_intrinsics_alu(&b, nir_instr_as_alu(instr),
                                   block_has_discard, options))
               progress = true;
            break;

         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            if (intrin->intrinsic == nir_intrinsic_demote ||
                intrin->intrinsic == nir_intrinsic_demote_if ||
                intrin->intrinsic == nir_intrinsic_terminate ||
                intrin->intrinsic == nir_intrinsic_terminate_if)
               block_has_discard = true;

            if (opt_intrinsics_intrin(&b, intrin, options))
               progress = true;
            break;
         }

         default:
            break;
         }
      }
   }

   return progress;
}

bool
nir_opt_intrinsics(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      bool impl_progress = opt_intrinsics_impl(impl, shader->options);
      progress |= nir_progress(impl_progress, impl,
                               nir_metadata_control_flow);
   }

   return progress;
}
