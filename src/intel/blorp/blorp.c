/*
 * Copyright © 2012 Intel Corporation
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

#include "blorp_priv.h"
#include "compiler/intel_nir.h"
#include "dev/intel_device_info.h"
#include "util/u_math.h"

enum intel_measure_snapshot_type
blorp_op_to_intel_measure_snapshot(enum blorp_op op)
{
   enum intel_measure_snapshot_type vals[] = {
#define MAP(name) [BLORP_OP_##name] = INTEL_SNAPSHOT_##name
      MAP(BLIT),
      MAP(COPY),
      MAP(CCS_AMBIGUATE),
      MAP(CCS_COLOR_CLEAR),
      MAP(CCS_PARTIAL_RESOLVE),
      MAP(CCS_RESOLVE),
      MAP(HIZ_AMBIGUATE),
      MAP(HIZ_CLEAR),
      MAP(HIZ_RESOLVE),
      MAP(MCS_AMBIGUATE),
      MAP(MCS_COLOR_CLEAR),
      MAP(MCS_PARTIAL_RESOLVE),
      MAP(SLOW_COLOR_CLEAR),
      MAP(SLOW_DEPTH_CLEAR),
#undef MAP
   };
   assert(op < ARRAY_SIZE(vals));

   return vals[op];
}

const char *blorp_op_to_name(enum blorp_op op)
{
   const char *names[] = {
#define MAP(name) [BLORP_OP_##name] = #name
      MAP(BLIT),
      MAP(COPY),
      MAP(CCS_AMBIGUATE),
      MAP(CCS_COLOR_CLEAR),
      MAP(CCS_PARTIAL_RESOLVE),
      MAP(CCS_RESOLVE),
      MAP(HIZ_AMBIGUATE),
      MAP(HIZ_CLEAR),
      MAP(HIZ_RESOLVE),
      MAP(MCS_AMBIGUATE),
      MAP(MCS_COLOR_CLEAR),
      MAP(MCS_PARTIAL_RESOLVE),
      MAP(SLOW_COLOR_CLEAR),
      MAP(SLOW_DEPTH_CLEAR),
#undef MAP
   };
   assert(op < ARRAY_SIZE(names));

   return names[op];
}

const char *
blorp_shader_type_to_name(enum blorp_shader_type type)
{
   static const char *shader_name[] = {
      [BLORP_SHADER_TYPE_COPY]                = "BLORP-copy",
      [BLORP_SHADER_TYPE_BLIT]                = "BLORP-blit",
      [BLORP_SHADER_TYPE_CLEAR]               = "BLORP-clear",
      [BLORP_SHADER_TYPE_MCS_PARTIAL_RESOLVE] = "BLORP-mcs-partial-resolve",
      [BLORP_SHADER_TYPE_LAYER_OFFSET_VS]     = "BLORP-layer-offset-vs",
      [BLORP_SHADER_TYPE_GFX4_SF]             = "BLORP-gfx4-sf",
   };
   assert(type < ARRAY_SIZE(shader_name));

   return shader_name[type];
}

const char *
blorp_shader_pipeline_to_name(enum blorp_shader_pipeline pipe)
{
   static const char *pipeline_name[] = {
      [BLORP_SHADER_PIPELINE_RENDER]  = "render",
      [BLORP_SHADER_PIPELINE_COMPUTE] = "compute",
   };
   assert(pipe < ARRAY_SIZE(pipeline_name));

   return pipeline_name[pipe];
}

void
blorp_init(struct blorp_context *blorp, void *driver_ctx,
           struct isl_device *isl_dev, const struct blorp_config *config)
{
   memset(blorp, 0, sizeof(*blorp));

   blorp->driver_ctx = driver_ctx;
   blorp->isl_dev = isl_dev;
   if (config)
      blorp->config = *config;

   blorp->compiler = rzalloc(NULL, struct blorp_compiler);
}

void
blorp_finish(struct blorp_context *blorp)
{
   ralloc_free(blorp->compiler);
   blorp->driver_ctx = NULL;
}

void
blorp_batch_init(struct blorp_context *blorp,
                 struct blorp_batch *batch, void *driver_batch,
                 enum blorp_batch_flags flags)
{
   batch->blorp = blorp;
   batch->driver_batch = driver_batch;
   batch->flags = flags;
}

void
blorp_batch_finish(struct blorp_batch *batch)
{
   batch->blorp = NULL;
}

void
blorp_surface_info_init(struct blorp_batch *batch,
                            struct blorp_surface_info *info,
                            const struct blorp_surf *surf,
                            unsigned int level, float layer,
                            enum isl_format format, bool is_dest)
{
   struct blorp_context *blorp = batch->blorp;
   memset(info, 0, sizeof(*info));
   assert(level < surf->surf->levels);
   assert(layer < MAX2(surf->surf->logical_level0_px.depth >> level,
                       surf->surf->logical_level0_px.array_len));

   info->enabled = true;

   if (format == ISL_FORMAT_UNSUPPORTED)
      format = surf->surf->format;

   info->surf = *surf->surf;
   info->addr = surf->addr;

   info->aux_usage = surf->aux_usage;
   if (!blorp_address_is_null(surf->aux_addr)) {
      info->aux_surf = *surf->aux_surf;
      info->aux_addr = surf->aux_addr;
   }

   info->clear_color = surf->clear_color;
   info->clear_color_addr = surf->clear_color_addr;

   isl_surf_usage_flags_t view_usage;
   if (is_dest) {
      if (batch->flags & BLORP_BATCH_USE_COMPUTE)
         view_usage = ISL_SURF_USAGE_STORAGE_BIT;
      else
         view_usage = ISL_SURF_USAGE_RENDER_TARGET_BIT;
   } else {
      view_usage = ISL_SURF_USAGE_TEXTURE_BIT;
   }

   info->view = (struct isl_view) {
      .usage = view_usage,
      .format = format,
      .base_level = level,
      .levels = 1,
      .swizzle = ISL_SWIZZLE_IDENTITY,
   };

   info->view.array_len =
      MAX2(u_minify(info->surf.logical_level0_px.depth, level),
           info->surf.logical_level0_px.array_len);

   if (!is_dest &&
       (info->surf.dim == ISL_SURF_DIM_3D ||
        info->surf.msaa_layout == ISL_MSAA_LAYOUT_ARRAY)) {
      /* 3-D textures don't support base_array layer and neither do 2-D
       * multisampled textures on IVB so we need to pass it through the
       * sampler in those cases.  These are also two cases where we are
       * guaranteed that we won't be doing any funny surface hacks.
       */
      info->view.base_array_layer = 0;
      info->z_offset = layer;
   } else {
      info->view.base_array_layer = layer;

      assert(info->view.array_len >= info->view.base_array_layer);
      info->view.array_len -= info->view.base_array_layer;
      info->z_offset = 0;
   }

   /* Sandy Bridge and earlier have a limit of a maximum of 512 layers for
    * layered rendering.
    */
   if (is_dest && blorp->isl_dev->info->ver <= 6)
      info->view.array_len = MIN2(info->view.array_len, 512);

   if (surf->tile_x_sa || surf->tile_y_sa) {
      /* This is only allowed on simple 2D surfaces without MSAA */
      assert(info->surf.dim == ISL_SURF_DIM_2D);
      assert(info->surf.samples == 1);
      assert(info->surf.levels == 1);
      assert(info->surf.logical_level0_px.array_len == 1);
      assert(info->aux_usage == ISL_AUX_USAGE_NONE);

      info->tile_x_sa = surf->tile_x_sa;
      info->tile_y_sa = surf->tile_y_sa;

      /* Instead of using the X/Y Offset fields in RENDER_SURFACE_STATE, we
       * place the image at the tile boundary and offset our sampling or
       * rendering.  For this reason, we need to grow the image by the offset
       * to ensure that the hardware doesn't think we've gone past the edge.
       */
      info->surf.logical_level0_px.w += surf->tile_x_sa;
      info->surf.logical_level0_px.h += surf->tile_y_sa;
      info->surf.phys_level0_sa.w += surf->tile_x_sa;
      info->surf.phys_level0_sa.h += surf->tile_y_sa;
   }
}


void
blorp_params_init(struct blorp_params *params)
{
   memset(params, 0, sizeof(*params));
   params->num_samples = 1;
   params->num_draw_buffers = 1;
   params->num_layers = 1;
}

void
blorp_hiz_op(struct blorp_batch *batch, struct blorp_surf *surf,
             uint32_t level, uint32_t start_layer, uint32_t num_layers,
             enum isl_aux_op op)
{
   const struct intel_device_info *devinfo = batch->blorp->isl_dev->info;

   struct blorp_params params;
   blorp_params_init(&params);

   params.hiz_op = op;
   params.full_surface_hiz_op = true;
   switch (op) {
   case ISL_AUX_OP_FULL_RESOLVE:
      params.op = BLORP_OP_HIZ_RESOLVE;
      break;
   case ISL_AUX_OP_AMBIGUATE:
      params.op = BLORP_OP_HIZ_AMBIGUATE;
      break;
   case ISL_AUX_OP_FAST_CLEAR:
      params.op = BLORP_OP_HIZ_CLEAR;
      break;
   case ISL_AUX_OP_PARTIAL_RESOLVE:
   case ISL_AUX_OP_NONE:
      UNREACHABLE("Invalid HiZ op");
   }

   for (uint32_t a = 0; a < num_layers; a++) {
      const uint32_t layer = start_layer + a;

      blorp_surface_info_init(batch, &params.depth, surf, level,
                                  layer, surf->surf->format, true);

      /* Align the rectangle primitive to 8x4 pixels.
       *
       * During fast depth clears, the emitted rectangle primitive  must be
       * aligned to 8x4 pixels.  From the Ivybridge PRM, Vol 2 Part 1 Section
       * 11.5.3.1 Depth Buffer Clear (and the matching section in the
       * Sandybridge PRM):
       *
       *     If Number of Multisamples is NUMSAMPLES_1, the rectangle must be
       *     aligned to an 8x4 pixel block relative to the upper left corner
       *     of the depth buffer [...]
       *
       * For hiz resolves, the rectangle must also be 8x4 aligned. Item
       * WaHizAmbiguate8x4Aligned from the Haswell workarounds page and the
       * Ivybridge simulator require the alignment.
       *
       * To be safe, let's just align the rect for all hiz operations and all
       * hardware generations.
       *
       * However, for some miptree slices of a Z24 texture, emitting an 8x4
       * aligned rectangle that covers the slice may clobber adjacent slices
       * if we strictly adhered to the texture alignments specified in the
       * PRM.  The Ivybridge PRM, Section "Alignment Unit Size", states that
       * SURFACE_STATE.Surface_Horizontal_Alignment should be 4 for Z24
       * surfaces, not 8. But commit 1f112cc increased the alignment from 4 to
       * 8, which prevents the clobbering.
       */
      params.x1 = u_minify(params.depth.surf.logical_level0_px.width,
                           params.depth.view.base_level);
      params.y1 = u_minify(params.depth.surf.logical_level0_px.height,
                           params.depth.view.base_level);
      params.x1 = ALIGN(params.x1, 8);
      params.y1 = ALIGN(params.y1, 4);

      if (params.depth.view.base_level == 0) {
         /* TODO: What about MSAA? */
         params.depth.surf.logical_level0_px.width = params.x1;
         params.depth.surf.logical_level0_px.height = params.y1;
      } else if (devinfo->ver >= 8 && devinfo->ver <= 9 &&
                 op == ISL_AUX_OP_AMBIGUATE) {
         /* On some platforms, it's not enough to just adjust the clear
          * rectangle when the LOD is greater than 0.
          *
          * From the BDW and SKL PRMs, Vol 7, "Optimized Hierarchical Depth
          * Buffer Resolve":
          *
          *    The following is required when performing a hierarchical depth
          *    buffer resolve:
          *
          *    - A rectangle primitive covering the full render target must be
          *      programmed on Xmin, Ymin, Xmax, and Ymax in the
          *      3DSTATE_WM_HZ_OP command.
          *
          *    - The rectangle primitive size must be aligned to 8x4 pixels.
          *
          * And from the Clear Rectangle programming note in 3DSTATE_WM_HZ_OP
          * (Vol 2a):
          *
          *    Hence the max values must be less than or equal to: ( Surface
          *    Width » LOD ) and ( Surface Height » LOD ) for X Max and Y Max
          *    respectively.
          *
          * This means that the extent of the LOD must be naturally
          * 8x4-aligned after minification of the base LOD. Since the base LOD
          * dimensions affect the placement of smaller LODs, it's not trivial
          * (nor possible, at times) to satisfy the requirement by adjusting
          * the base LOD extent. Just assert that the caller is accessing an
          * LOD that satisfies this requirement.
          */
         assert(u_minify(params.depth.surf.logical_level0_px.width,
                         params.depth.view.base_level) == params.x1);
         assert(u_minify(params.depth.surf.logical_level0_px.height,
                         params.depth.view.base_level) == params.y1);
      }

      params.dst.surf.samples = params.depth.surf.samples;
      params.dst.surf.logical_level0_px = params.depth.surf.logical_level0_px;
      params.depth_format =
         isl_format_get_depth_format(surf->surf->format, false);
      params.num_samples = params.depth.surf.samples;

      batch->blorp->exec(batch, &params);
   }
}
