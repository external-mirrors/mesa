/*
 * Copyright 2023 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include "asahi/compiler/agx_compile.h"
#include "asahi/layout/layout.h"
#include "agx_abi.h"
#include "agx_pack.h"
#include "agx_ppp.h"
#include "libagx_shaders.h"

#define AGX_MAX_OCCLUSION_QUERIES (32768)
#define AGX_MAX_VIEWPORTS         (16)

static inline enum agx_sampler_states
agx_translate_sampler_state_count(unsigned count, bool extended)
{
   assert(count <= 17 && "max 17 sampler state registers supported");

   if (count == 0) {
      return AGX_SAMPLER_STATES_0;
   } else if (extended) {
      if (count <= 8)
         return AGX_SAMPLER_STATES_8_EXTENDED;
      else
         return AGX_SAMPLER_STATES_16_EXTENDED;
   } else {
      if (count <= 4)
         return AGX_SAMPLER_STATES_4_COMPACT;
      else if (count <= 8)
         return AGX_SAMPLER_STATES_8_COMPACT;
      else if (count <= 12)
         return AGX_SAMPLER_STATES_12_COMPACT;
      else
         return AGX_SAMPLER_STATES_16_COMPACT;
   }
}

static void
agx_pack_txf_sampler(struct agx_sampler_packed *out)
{
   agx_pack(out, SAMPLER, cfg) {
      /* Allow mipmapping. This is respected by txf, weirdly. */
      cfg.minimum_lod = 0.0;
      cfg.maximum_lod = INFINITY;
      cfg.mip_filter = AGX_MIP_FILTER_NEAREST;

      /* Out-of-bounds reads must return 0 */
      cfg.wrap_s = AGX_WRAP_CLAMP_TO_BORDER;
      cfg.wrap_t = AGX_WRAP_CLAMP_TO_BORDER;
      cfg.wrap_r = AGX_WRAP_CLAMP_TO_BORDER;
      cfg.border_colour = AGX_BORDER_COLOUR_TRANSPARENT_BLACK;
   }
}

/* Channels agree for RGBA but are weird for force 0/1 */

static inline enum agx_channel
agx_channel_from_pipe(enum pipe_swizzle in)
{
   STATIC_ASSERT((enum agx_channel)PIPE_SWIZZLE_X == AGX_CHANNEL_R);
   STATIC_ASSERT((enum agx_channel)PIPE_SWIZZLE_Y == AGX_CHANNEL_G);
   STATIC_ASSERT((enum agx_channel)PIPE_SWIZZLE_Z == AGX_CHANNEL_B);
   STATIC_ASSERT((enum agx_channel)PIPE_SWIZZLE_W == AGX_CHANNEL_A);
   STATIC_ASSERT((enum agx_channel)PIPE_SWIZZLE_0 & 0x4);
   STATIC_ASSERT((enum agx_channel)PIPE_SWIZZLE_1 & 0x4);
   STATIC_ASSERT((enum agx_channel)PIPE_SWIZZLE_NONE & 0x4);

   if ((in & 0x4) == 0)
      return (enum agx_channel)in;
   else if (in == PIPE_SWIZZLE_1)
      return AGX_CHANNEL_1;
   else
      return AGX_CHANNEL_0;
}

static inline enum agx_layout
agx_translate_layout(enum ail_tiling tiling)
{
   switch (tiling) {
   case AIL_TILING_GPU:
      return AGX_LAYOUT_GPU;
   case AIL_TILING_TWIDDLED:
      return AGX_LAYOUT_TWIDDLED;
   case AIL_TILING_LINEAR:
      return AGX_LAYOUT_LINEAR;
   }

   UNREACHABLE("Invalid tiling");
}

static inline enum agx_zls_tiling
agx_translate_zls_tiling(enum ail_tiling tiling)
{
   switch (tiling) {
   case AIL_TILING_GPU:
      return AGX_ZLS_TILING_GPU;
   case AIL_TILING_TWIDDLED:
      return AGX_ZLS_TILING_TWIDDLED;
   default:
      UNREACHABLE("Invalid ZLS tiling");
   }
}

struct agx_zls {
   bool z_load, z_store;
   bool s_load, s_store;
};

static inline void
agx_pack_zls_control(struct agx_zls_control_packed *packed,
                     const struct ail_layout *z, const struct ail_layout *s,
                     struct agx_zls *args)
{
   agx_pack(packed, ZLS_CONTROL, cfg) {
      if (z) {
         cfg.z_store = args->z_store;
         cfg.z_load = args->z_load;
         cfg.z_load_compress = cfg.z_store_compress = z->compressed;
         cfg.z_load_tiling = cfg.z_store_tiling =
            agx_translate_zls_tiling(z->tiling);

         if (z->format == PIPE_FORMAT_Z16_UNORM) {
            cfg.z_format = AGX_ZLS_FORMAT_16;
         } else {
            cfg.z_format = AGX_ZLS_FORMAT_32F;
         }
      }

      if (s) {
         cfg.s_load = args->s_load;
         cfg.s_store = args->s_store;
         cfg.s_load_compress = cfg.s_store_compress = s->compressed;
         cfg.s_load_tiling = cfg.s_store_tiling =
            agx_translate_zls_tiling(s->tiling);
      }
   }
}

static enum agx_sample_count
agx_translate_sample_count(unsigned samples)
{
   switch (samples) {
   case 2:
      return AGX_SAMPLE_COUNT_2;
   case 4:
      return AGX_SAMPLE_COUNT_4;
   default:
      UNREACHABLE("Invalid sample count");
   }
}

static enum agx_conservative_depth
agx_translate_depth_layout(enum gl_frag_depth_layout layout)
{
   switch (layout) {
   case FRAG_DEPTH_LAYOUT_ANY:
      return AGX_CONSERVATIVE_DEPTH_ANY;
   case FRAG_DEPTH_LAYOUT_LESS:
      return AGX_CONSERVATIVE_DEPTH_LESS;
   case FRAG_DEPTH_LAYOUT_GREATER:
      return AGX_CONSERVATIVE_DEPTH_GREATER;
   case FRAG_DEPTH_LAYOUT_UNCHANGED:
      return AGX_CONSERVATIVE_DEPTH_UNCHANGED;
   default:
      UNREACHABLE("depth layout should have been canonicalized");
   }
}

static void
agx_pack_fragment_face_2(struct agx_fragment_face_2_packed *out,
                         enum agx_object_type object_type,
                         struct agx_shader_info *info)
{
   agx_pack(out, FRAGMENT_FACE_2, cfg) {
      /* These act like disables, ANDed in the hardware. Setting them like this
       * means the draw-time flag is used.
       */
      cfg.disable_depth_write = true;
      cfg.depth_function = AGX_ZS_FUNC_ALWAYS;

      cfg.object_type = object_type;
      cfg.conservative_depth =
         info ? agx_translate_depth_layout(info->depth_layout)
              : AGX_CONSERVATIVE_DEPTH_UNCHANGED;
   }
}

static void
agx_ppp_fragment_face_2(struct agx_ppp_update *ppp,
                        enum agx_object_type object_type,
                        struct agx_shader_info *info)
{
   struct agx_fragment_face_2_packed packed;
   agx_pack_fragment_face_2(&packed, object_type, info);
   agx_ppp_push_packed(ppp, &packed, FRAGMENT_FACE_2);
}

static inline uint32_t
agx_pack_line_width(float line_width)
{
   /* Line width is packed in a 4:4 fixed point format */
   unsigned line_width_fixed = ((unsigned)(line_width * 16.0f)) - 1;

   /* Clamp to maximum line width */
   return MIN2(line_width_fixed, 0xFF);
}

/*
 * Despite having both a layout *and* a flag that I only see Metal use with null
 * textures, AGX doesn't seem to have "real" null textures. Instead we need to
 * bind an arbitrary address and throw away the results to read all 0's.
 * Accordingly, the caller must pass some address that lives at least as long as
 * the texture descriptor itself.
 */
static void
agx_set_null_texture(struct agx_texture_packed *tex)
{
   agx_pack(tex, TEXTURE, cfg) {
      cfg.layout = AGX_LAYOUT_TWIDDLED;
      cfg.channels = AGX_CHANNELS_R8;
      cfg.type = AGX_TEXTURE_TYPE_UNORM /* don't care */;
      cfg.swizzle_r = AGX_CHANNEL_0;
      cfg.swizzle_g = AGX_CHANNEL_0;
      cfg.swizzle_b = AGX_CHANNEL_0;
      cfg.swizzle_a = AGX_CHANNEL_0;
      cfg.address = AGX_ZERO_PAGE_ADDRESS;
   }
}

static void
agx_set_null_pbe(struct agx_pbe_packed *pbe)
{
   agx_pack(pbe, PBE, cfg) {
      cfg.width = 1;
      cfg.height = 1;
      cfg.levels = 1;
      cfg.layout = AGX_LAYOUT_TWIDDLED;
      cfg.channels = AGX_CHANNELS_R8;
      cfg.type = AGX_TEXTURE_TYPE_UNORM /* don't care */;
      cfg.swizzle_r = AGX_CHANNEL_R;
      cfg.swizzle_g = AGX_CHANNEL_R;
      cfg.swizzle_b = AGX_CHANNEL_R;
      cfg.swizzle_a = AGX_CHANNEL_R;
      cfg.buffer = AGX_SCRATCH_PAGE_ADDRESS;
   }
}

/*
 * Determine the maximum vertex/divided instance index.  For robustness,
 * the index will be clamped to this before reading (if soft fault is
 * disabled).
 *
 * Index i accesses up to (exclusive) offset:
 *
 *    src_offset + (i * stride) + elsize_B
 *
 * so we require
 *
 *    src_offset + (i * stride) + elsize_B <= size
 *
 * <==>
 *
 *    i <= floor((size - src_offset - elsize_B) / stride)
 */
static inline uint32_t
agx_calculate_vbo_clamp(uint64_t vbuf, enum pipe_format format, uint32_t size_B,
                        uint32_t stride_B, uint32_t offset_B,
                        uint64_t *vbuf_out)
{
   unsigned elsize_B = util_format_get_blocksize(format);
   unsigned subtracted_B = offset_B + elsize_B;

   /* If at least one index is valid, determine the max. Otherwise, direct reads
    * to zero.
    */
   if (size_B >= subtracted_B) {
      *vbuf_out = vbuf + offset_B;

      /* If stride is zero, do not clamp, everything is valid. */
      if (stride_B)
         return ((size_B - subtracted_B) / stride_B);
      else
         return UINT32_MAX;
   } else {
      *vbuf_out = AGX_ZERO_PAGE_ADDRESS;
      return 0;
   }
}

static struct libagx_decompress_args
agx_fill_decompress_args(struct ail_layout *layout, unsigned layer,
                         unsigned level, uint64_t ptr, uint64_t images)
{
   return (struct libagx_decompress_args){
      .images = images,
      .tile_uncompressed = ail_tile_mode_uncompressed(layout->format),
      .metadata = ptr + layout->metadata_offset_B +
                  layout->level_offsets_compressed_B[level] +
                  (layer * layout->compression_layer_stride_B),
      .metadata_layer_stride_tl = layout->compression_layer_stride_B / 8,
      .metadata_width_tl = ail_metadata_width_tl(layout, level),
      .metadata_height_tl = ail_metadata_height_tl(layout, level),
   };
}

#undef libagx_decompress
#define libagx_decompress(context, grid, barrier, layout, layer, level, ptr,   \
                          images)                                              \
   libagx_decompress_struct(                                                   \
      context, grid, barrier,                                                  \
      agx_fill_decompress_args(layout, layer, level, ptr, images),             \
      util_logbase2(layout->sample_count_sa))

#define libagx_tessellate(context, grid, barrier, prim, mode, state)           \
   if (prim == TESS_PRIMITIVE_QUADS) {                                         \
      libagx_tess_quad(context, grid, barrier, state, mode);                   \
   } else if (prim == TESS_PRIMITIVE_TRIANGLES) {                              \
      libagx_tess_tri(context, grid, barrier, state, mode);                    \
   } else {                                                                    \
      assert(prim == TESS_PRIMITIVE_ISOLINES);                                 \
      libagx_tess_isoline(context, grid, barrier, state, mode);                \
   }

struct agx_border_packed;

void agx_pack_border(struct agx_border_packed *out, const uint32_t in[4],
                     enum pipe_format format);
