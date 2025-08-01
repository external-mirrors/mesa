/*
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "asahi/compiler/agx_nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/bitset.h"
#include "agx_state.h"
#include "nir.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"

/*
 * Lower binding table textures and images to texture state registers and (if
 * necessary) bindless access into an internal table mapped like additional
 * texture state registers. The following layout is used:
 *
 *    1. Textures
 *    2. Images (read/write interleaved)
 */

static bool
lower_sampler(nir_builder *b, nir_tex_instr *tex)
{
   if (!nir_tex_instr_need_sampler(tex))
      return false;

   nir_def *index = nir_steal_tex_src(tex, nir_tex_src_sampler_offset);
   if (!index)
      index = nir_imm_int(b, tex->sampler_index);

   nir_tex_instr_add_src(tex, nir_tex_src_sampler_handle,
                         nir_load_sampler_handle_agx(b, index));
   return true;
}

static bool
lower(nir_builder *b, nir_instr *instr, void *data)
{
   bool *uses_bindless_samplers = data;
   bool progress = false;
   bool force_bindless = agx_nir_needs_texture_crawl(instr);
   b->cursor = nir_before_instr(instr);

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

      switch (intr->intrinsic) {
      case nir_intrinsic_image_load:
      case nir_intrinsic_image_store:
      case nir_intrinsic_image_size:
      case nir_intrinsic_image_samples:
      case nir_intrinsic_image_atomic:
      case nir_intrinsic_image_atomic_swap:
         break;
      default:
         return false;
      }

      nir_def *index = intr->src[0].ssa;
      nir_scalar index_scalar = nir_scalar_resolved(index, 0);

      /* Remap according to the driver layout */
      unsigned offset = BITSET_LAST_BIT(b->shader->info.textures_used);

      /* For reads and queries, we use the texture descriptor which is first.
       * Writes and atomics use the PBE descriptor.
       */
      if (intr->intrinsic != nir_intrinsic_image_load &&
          intr->intrinsic != nir_intrinsic_image_size &&
          intr->intrinsic != nir_intrinsic_image_samples)
         offset++;

      /* If we can determine statically that the image fits in texture state
       * registers, avoid lowering to bindless access.
       */
      if (nir_scalar_is_const(index_scalar) && !force_bindless) {
         unsigned idx = (nir_scalar_as_uint(index_scalar) * 2) + offset;

         if (idx < AGX_NUM_TEXTURE_STATE_REGS) {
            nir_src_rewrite(&intr->src[0], nir_imm_intN_t(b, idx, 16));
            return true;
         }
      }

      /* Otherwise, lower to bindless...
       *
       * The driver uploads enough null texture/PBE descriptors for robustness
       * given the shader limit, but we still need to clamp since we're lowering
       * to bindless so the hardware doesn't know the limit.
       *
       * The GL spec says out-of-bounds image indexing is undefined, but
       * faulting is not acceptable for robustness.
       */
      index = nir_umin(
         b, index,
         nir_imm_intN_t(b, b->shader->info.num_images - 1, index->bit_size));

      index = nir_iadd_imm(b, nir_imul_imm(b, index, 2), offset);

      nir_rewrite_image_intrinsic(intr, nir_load_texture_handle_agx(b, index),
                                  true);
   } else if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);

      if (((BITSET_COUNT(b->shader->info.samplers_used) > 16) &&
           (nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset) >= 0 ||
            tex->sampler_index >= 16)) &&
          lower_sampler(b, tex)) {
         progress = true;
         *uses_bindless_samplers = true;
      }

      /* Nothing to do for "real" bindless */
      if (nir_tex_instr_src_index(tex, nir_tex_src_texture_handle) >= 0)
         return progress;

      /* Textures are mapped 1:1, so if we can prove it fits in a texture state
       * register, use the texture state register.
       */
      if (tex->texture_index < AGX_NUM_TEXTURE_STATE_REGS &&
          nir_tex_instr_src_index(tex, nir_tex_src_texture_offset) == -1 &&
          !force_bindless)
         return progress;

      /* Otherwise, lower to bindless. Could be optimized. */
      nir_def *index = nir_steal_tex_src(tex, nir_tex_src_texture_offset);
      if (!index)
         index = nir_imm_int(b, tex->texture_index);

      /* As above */
      index = nir_umin(
         b, index,
         nir_imm_intN_t(b, b->shader->info.num_textures - 1, index->bit_size));

      nir_tex_instr_add_src(tex, nir_tex_src_texture_handle,
                            nir_load_texture_handle_agx(b, index));
   }

   return true;
}

bool
agx_nir_lower_bindings(nir_shader *shader, bool *uses_bindless_samplers)
{
   /* First lower index to offset so we can lower more naturally */
   bool progress = nir_lower_tex(
      shader, &(nir_lower_tex_options){.lower_index_to_offset = true});

   /* Next run constant folding so the constant optimizations above have a
    * chance.
    */
   progress |= nir_opt_constant_folding(shader);

   progress |= nir_shader_instructions_pass(
      shader, lower, nir_metadata_control_flow, uses_bindless_samplers);
   return progress;
}
