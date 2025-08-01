/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2023 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"
#include "radv_nir.h"
#include "radv_shader.h"
#include "radv_shader_args.h"
#include "radv_shader_info.h"

struct ctx {
   const struct radv_shader_stage *fs_stage;
   const struct radv_graphics_state_key *gfx_state;
};

static bool
pass(nir_builder *b, nir_intrinsic_instr *intrin, void *data)
{
   b->cursor = nir_after_instr(&intrin->instr);

   const struct ctx *ctx = data;
   const struct radv_graphics_state_key *gfx_state = ctx->gfx_state;
   const struct radv_shader_info *info = &ctx->fs_stage->info;
   const struct radv_shader_args *args = &ctx->fs_stage->args;

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_sample_mask_in: {
      nir_def *sample_coverage = nir_load_vector_arg_amd(b, 1, .base = args->ac.sample_coverage.arg_index);

      nir_def *def = NULL;
      if (info->ps.uses_sample_shading || gfx_state->ms.sample_shading_enable) {
         /* gl_SampleMaskIn[0] = (SampleCoverage & (PsIterMask << gl_SampleID)). */
         nir_def *ps_state = nir_load_scalar_arg_amd(b, 1, .base = args->ps_state.arg_index);
         nir_def *ps_iter_mask =
            nir_ubfe_imm(b, ps_state, PS_STATE_PS_ITER_MASK__SHIFT, util_bitcount(PS_STATE_PS_ITER_MASK__MASK));
         nir_def *sample_id = nir_load_sample_id(b);
         def = nir_iand(b, sample_coverage, nir_ishl(b, ps_iter_mask, sample_id));
      } else {
         def = sample_coverage;
      }

      nir_def_replace(&intrin->def, def);
      return true;
   }
   case nir_intrinsic_load_frag_coord: {
      if (!gfx_state->adjust_frag_coord_z)
         return false;

      if (!(nir_def_components_read(&intrin->def) & (1 << 2)))
         return false;

      nir_def *frag_z = nir_channel(b, &intrin->def, 2);

      /* adjusted_frag_z = dFdxFine(frag_z) * 0.0625 + frag_z */
      nir_def *adjusted_frag_z = nir_ddx_fine(b, frag_z);
      adjusted_frag_z = nir_ffma_imm1(b, adjusted_frag_z, 0.0625f, frag_z);

      /* VRS Rate X = Ancillary[2:3] */
      nir_def *ancillary = nir_load_vector_arg_amd(b, 1, .base = args->ac.ancillary.arg_index);
      nir_def *x_rate = nir_ubfe_imm(b, ancillary, 2, 2);

      /* xRate = xRate == 0x1 ? adjusted_frag_z : frag_z. */
      nir_def *cond = nir_ieq_imm(b, x_rate, 1);
      frag_z = nir_bcsel(b, cond, adjusted_frag_z, frag_z);

      nir_def *new_dest = nir_vector_insert_imm(b, &intrin->def, frag_z, 2);
      nir_def_rewrite_uses_after(&intrin->def, new_dest);
      return true;
   }
   case nir_intrinsic_load_barycentric_at_sample: {
      nir_def *num_samples = nir_load_rasterization_samples_amd(b);
      nir_def *new_dest;

      if (gfx_state->dynamic_rasterization_samples) {
         nir_def *res1, *res2;

         nir_push_if(b, nir_ieq_imm(b, num_samples, 1));
         {
            res1 = nir_load_barycentric_pixel(b, 32, .interp_mode = nir_intrinsic_interp_mode(intrin));
         }
         nir_push_else(b, NULL);
         {
            nir_def *sample_pos = nir_load_sample_positions_amd(b, 32, intrin->src[0].ssa, num_samples);

            /* sample_pos -= 0.5 */
            sample_pos = nir_fadd_imm(b, sample_pos, -0.5f);

            res2 = nir_load_barycentric_at_offset(b, 32, sample_pos, .interp_mode = nir_intrinsic_interp_mode(intrin));
         }
         nir_pop_if(b, NULL);

         new_dest = nir_if_phi(b, res1, res2);
      } else {
         if (!gfx_state->ms.rasterization_samples) {
            new_dest = nir_load_barycentric_pixel(b, 32, .interp_mode = nir_intrinsic_interp_mode(intrin));
         } else {
            nir_def *sample_pos = nir_load_sample_positions_amd(b, 32, intrin->src[0].ssa, num_samples);

            /* sample_pos -= 0.5 */
            sample_pos = nir_fadd_imm(b, sample_pos, -0.5f);

            new_dest =
               nir_load_barycentric_at_offset(b, 32, sample_pos, .interp_mode = nir_intrinsic_interp_mode(intrin));
         }
      }

      nir_def_replace(&intrin->def, new_dest);
      return true;
   }
   default:
      return false;
   }
}

bool
radv_nir_lower_fs_intrinsics(nir_shader *nir, const struct radv_shader_stage *fs_stage,
                             const struct radv_graphics_state_key *gfx_state)
{
   struct ctx ctx = {.fs_stage = fs_stage, .gfx_state = gfx_state};
   return nir_shader_intrinsics_pass(nir, pass, nir_metadata_none, &ctx);
}

static bool
lower_load_input_attachment(nir_builder *b, nir_intrinsic_instr *intrin, void *state)
{
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_input_attachment_coord: {
      b->cursor = nir_before_instr(&intrin->instr);

      nir_def *pos = nir_f2i32(b, nir_load_frag_coord(b));
      nir_def *layer = nir_load_layer_id(b);
      nir_def *coord = nir_vec3(b, nir_channel(b, pos, 0), nir_channel(b, pos, 1), layer);

      nir_def_replace(&intrin->def, coord);
      return true;
   }
   default:
      return false;
   }
}

bool
radv_nir_lower_fs_input_attachment(nir_shader *nir)
{
   if (!nir->info.fs.uses_fbfetch_output)
      return false;

   return nir_shader_intrinsics_pass(nir, lower_load_input_attachment, nir_metadata_control_flow, NULL);
}
