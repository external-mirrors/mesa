/*
 * Copyright (c) 2017-2019 Lima Project
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
 */

#include <string.h>

#include "util/ralloc.h"
#include "util/u_debug.h"
#include "util/u_screen.h"
#include "util/xmlconfig.h"
#include "renderonly/renderonly.h"

#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/lima_drm.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"
#include "lima_program.h"
#include "lima_bo.h"
#include "lima_fence.h"
#include "lima_format.h"
#include "lima_disk_cache.h"
#include "ir/lima_ir.h"

#include "xf86drm.h"

int lima_plb_max_blk = 0;
int lima_plb_pp_stream_cache_size = 0;

static void
lima_screen_destroy(struct pipe_screen *pscreen)
{
   struct lima_screen *screen = lima_screen(pscreen);

   slab_destroy_parent(&screen->transfer_pool);

   if (screen->ro)
      screen->ro->destroy(screen->ro);

   if (screen->pp_buffer)
      lima_bo_unreference(screen->pp_buffer);

   lima_bo_cache_fini(screen);
   lima_bo_table_fini(screen);
   disk_cache_destroy(screen->disk_cache);
   lima_resource_screen_destroy(screen);
   ralloc_free(screen);
}

static const char *
lima_screen_get_name(struct pipe_screen *pscreen)
{
   struct lima_screen *screen = lima_screen(pscreen);

   switch (screen->gpu_type) {
   case DRM_LIMA_PARAM_GPU_ID_MALI400:
     return "Mali400";
   case DRM_LIMA_PARAM_GPU_ID_MALI450:
     return "Mali450";
   }

   return NULL;
}

static const char *
lima_screen_get_vendor(struct pipe_screen *pscreen)
{
   return "Mesa";
}

static const char *
lima_screen_get_device_vendor(struct pipe_screen *pscreen)
{
   return "ARM";
}

static void
lima_init_shader_caps(struct pipe_screen *screen)
{
   struct pipe_shader_caps *caps =
      (struct pipe_shader_caps *)&screen->shader_caps[PIPE_SHADER_VERTEX];

   caps->max_instructions =
   caps->max_alu_instructions =
   caps->max_tex_instructions =
   caps->max_tex_indirections = 16384; /* need investigate */
   caps->max_control_flow_depth = 1024;
   caps->max_inputs = 16; /* attributes */
   caps->max_outputs = LIMA_MAX_VARYING_NUM; /* varying */
   /* Mali-400 GP provides space for 304 vec4 uniforms, globals and
    * temporary variables. */
   caps->max_const_buffer0_size = 304 * 4 * sizeof(float);
   caps->max_const_buffers = 1;
   caps->max_temps = 256; /* need investigate */

   caps = (struct pipe_shader_caps *)&screen->shader_caps[PIPE_SHADER_FRAGMENT];

   caps->max_instructions =
   caps->max_alu_instructions =
   caps->max_tex_instructions =
   caps->max_tex_indirections = 16384; /* need investigate */
   caps->max_inputs = LIMA_MAX_VARYING_NUM - 1; /* varying, minus gl_Position */
   caps->max_control_flow_depth = 1024;
   /* The Mali-PP supports a uniform table up to size 32768 total.
    * However, indirect access to an uniform only supports indices up
    * to 8192 (a 2048 vec4 array). To prevent indices bigger than that,
    * limit max const buffer size to 8192 for now. */
   caps->max_const_buffer0_size = 2048 * 4 * sizeof(float);
   caps->max_const_buffers = 1;
   caps->max_sampler_views =
   caps->max_texture_samplers = 16; /* need investigate */
   caps->max_temps = 256; /* need investigate */
   caps->indirect_const_addr = true;
}

static void
lima_init_screen_caps(struct pipe_screen *screen)
{
   struct pipe_caps *caps = (struct pipe_caps *)&screen->caps;

   u_init_pipe_screen_caps(screen, 1);

   caps->npot_textures = true;
   caps->blend_equation_separate = true;
   caps->uma = true;
   caps->clip_halfz = true;
   caps->native_fence_fd = true;
   caps->fragment_shader_texture_lod = true;
   caps->texture_swizzle = true;
   caps->vertex_color_unclamped = true;
   caps->texture_barrier = true;
   caps->surface_sample_count = true;

   /* not clear supported */
   caps->fs_coord_origin_upper_left = true;
   caps->fs_coord_origin_lower_left = true;
   caps->fs_coord_pixel_center_integer = true;
   caps->fs_coord_pixel_center_half_integer = true;

   caps->fs_position_is_sysval = true;
   caps->fs_point_is_sysval = true;
   caps->fs_face_is_integer_sysval = true;

   caps->texture_half_float_linear = true;

   caps->max_texture_2d_size = 1 << (LIMA_MAX_MIP_LEVELS - 1);
   caps->max_texture_3d_levels =
   caps->max_texture_cube_levels = LIMA_MAX_MIP_LEVELS;

   caps->vendor_id = 0x13B5;

   caps->video_memory = 0;

   caps->pci_group =
   caps->pci_bus =
   caps->pci_device =
   caps->pci_function = 0;

   caps->texture_transfer_modes = 0;

   caps->shareable_shaders = false;

   caps->alpha_test = true;

   caps->flatshade = false;
   caps->two_sided_color = false;
   caps->clip_planes = 0;

   caps->fragment_shader_derivatives = true;

   /* Mali4x0 PP doesn't have a swizzle for load_input, so use POT-aligned
    * varyings to avoid unnecessary movs for vec3 and precision downgrade
    * in case if this vec3 is coordinates for a sampler
    */
   caps->prefer_pot_aligned_varyings = true;

   caps->max_dual_source_render_targets = true;

   caps->min_line_width =
   caps->min_line_width_aa =
   caps->min_point_size =
   caps->min_point_size_aa = 1;

   caps->point_size_granularity =
   caps->line_width_granularity = 0.1;

   caps->max_line_width =
   caps->max_line_width_aa =
   caps->max_point_size =
   caps->max_point_size_aa = 100.0f;

   caps->max_texture_anisotropy = 16.0f;

   caps->max_texture_lod_bias = 15.0f;
}

static bool
lima_screen_is_format_supported(struct pipe_screen *pscreen,
                                enum pipe_format format,
                                enum pipe_texture_target target,
                                unsigned sample_count,
                                unsigned storage_sample_count,
                                unsigned usage)
{
   switch (target) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_3D:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_CUBE:
      break;
   default:
      return false;
   }

   if (MAX2(1, sample_count) != MAX2(1, storage_sample_count))
      return false;

   /* Utgard supports 16x, but for now limit it to 4x */
   if (sample_count > 1 && sample_count != 4)
      return false;

   if (usage & PIPE_BIND_RENDER_TARGET) {
      if (!lima_format_pixel_supported(format))
         return false;

      /* multisample unsupported with half float target */
      if (sample_count > 1 && util_format_is_float(format))
         return false;
   }

   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      switch (format) {
      case PIPE_FORMAT_Z16_UNORM:
      case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      case PIPE_FORMAT_Z24X8_UNORM:
         break;
      default:
         return false;
      }
   }

   if (usage & PIPE_BIND_VERTEX_BUFFER) {
      switch (format) {
      case PIPE_FORMAT_R32_FLOAT:
      case PIPE_FORMAT_R32G32_FLOAT:
      case PIPE_FORMAT_R32G32B32_FLOAT:
      case PIPE_FORMAT_R32G32B32A32_FLOAT:
      case PIPE_FORMAT_R32_FIXED:
      case PIPE_FORMAT_R32G32_FIXED:
      case PIPE_FORMAT_R32G32B32_FIXED:
      case PIPE_FORMAT_R32G32B32A32_FIXED:
      case PIPE_FORMAT_R16_FLOAT:
      case PIPE_FORMAT_R16G16_FLOAT:
      case PIPE_FORMAT_R16G16B16_FLOAT:
      case PIPE_FORMAT_R16G16B16A16_FLOAT:
      case PIPE_FORMAT_R32_UNORM:
      case PIPE_FORMAT_R32G32_UNORM:
      case PIPE_FORMAT_R32G32B32_UNORM:
      case PIPE_FORMAT_R32G32B32A32_UNORM:
      case PIPE_FORMAT_R32_SNORM:
      case PIPE_FORMAT_R32G32_SNORM:
      case PIPE_FORMAT_R32G32B32_SNORM:
      case PIPE_FORMAT_R32G32B32A32_SNORM:
      case PIPE_FORMAT_R32_USCALED:
      case PIPE_FORMAT_R32G32_USCALED:
      case PIPE_FORMAT_R32G32B32_USCALED:
      case PIPE_FORMAT_R32G32B32A32_USCALED:
      case PIPE_FORMAT_R32_SSCALED:
      case PIPE_FORMAT_R32G32_SSCALED:
      case PIPE_FORMAT_R32G32B32_SSCALED:
      case PIPE_FORMAT_R32G32B32A32_SSCALED:
      case PIPE_FORMAT_R16_UNORM:
      case PIPE_FORMAT_R16G16_UNORM:
      case PIPE_FORMAT_R16G16B16_UNORM:
      case PIPE_FORMAT_R16G16B16A16_UNORM:
      case PIPE_FORMAT_R16_SNORM:
      case PIPE_FORMAT_R16G16_SNORM:
      case PIPE_FORMAT_R16G16B16_SNORM:
      case PIPE_FORMAT_R16G16B16A16_SNORM:
      case PIPE_FORMAT_R16_USCALED:
      case PIPE_FORMAT_R16G16_USCALED:
      case PIPE_FORMAT_R16G16B16_USCALED:
      case PIPE_FORMAT_R16G16B16A16_USCALED:
      case PIPE_FORMAT_R16_SSCALED:
      case PIPE_FORMAT_R16G16_SSCALED:
      case PIPE_FORMAT_R16G16B16_SSCALED:
      case PIPE_FORMAT_R16G16B16A16_SSCALED:
      case PIPE_FORMAT_R8_UNORM:
      case PIPE_FORMAT_R8G8_UNORM:
      case PIPE_FORMAT_R8G8B8_UNORM:
      case PIPE_FORMAT_R8G8B8A8_UNORM:
      case PIPE_FORMAT_R8_SNORM:
      case PIPE_FORMAT_R8G8_SNORM:
      case PIPE_FORMAT_R8G8B8_SNORM:
      case PIPE_FORMAT_R8G8B8A8_SNORM:
      case PIPE_FORMAT_R8_USCALED:
      case PIPE_FORMAT_R8G8_USCALED:
      case PIPE_FORMAT_R8G8B8_USCALED:
      case PIPE_FORMAT_R8G8B8A8_USCALED:
      case PIPE_FORMAT_R8_SSCALED:
      case PIPE_FORMAT_R8G8_SSCALED:
      case PIPE_FORMAT_R8G8B8_SSCALED:
      case PIPE_FORMAT_R8G8B8A8_SSCALED:
         break;
      default:
         return false;
      }
   }

   if (usage & PIPE_BIND_INDEX_BUFFER) {
      switch (format) {
      case PIPE_FORMAT_R8_UINT:
      case PIPE_FORMAT_R16_UINT:
      case PIPE_FORMAT_R32_UINT:
         break;
      default:
         return false;
      }
   }

   if (usage & PIPE_BIND_SAMPLER_VIEW)
      return lima_format_texel_supported(format);

   return true;
}

static bool
lima_screen_set_plb_max_blk(struct lima_screen *screen)
{
   if (lima_plb_max_blk) {
      screen->plb_max_blk = lima_plb_max_blk;
      return true;
   }

   if (screen->gpu_type == DRM_LIMA_PARAM_GPU_ID_MALI450)
      screen->plb_max_blk = 4096;
   else
      screen->plb_max_blk = 512;

   drmDevicePtr devinfo;

   if (drmGetDevice2(screen->fd, 0, &devinfo))
      return false;

   if (devinfo->bustype == DRM_BUS_PLATFORM && devinfo->deviceinfo.platform) {
      char **compatible = devinfo->deviceinfo.platform->compatible;

      if (compatible && *compatible)
         if (!strcmp("allwinner,sun50i-h5-mali", *compatible))
            screen->plb_max_blk = 2048;
   }

   drmFreeDevice(&devinfo);

   return true;
}

static bool
lima_screen_query_info(struct lima_screen *screen)
{
   drmVersionPtr version = drmGetVersion(screen->fd);
   if (!version)
      return false;

   if (version->version_major > 1 || version->version_minor > 0)
      screen->has_growable_heap_buffer = true;

   drmFreeVersion(version);

   if (lima_debug & LIMA_DEBUG_NO_GROW_HEAP)
      screen->has_growable_heap_buffer = false;

   struct drm_lima_get_param param;

   memset(&param, 0, sizeof(param));
   param.param = DRM_LIMA_PARAM_GPU_ID;
   if (drmIoctl(screen->fd, DRM_IOCTL_LIMA_GET_PARAM, &param))
      return false;

   switch (param.value) {
   case DRM_LIMA_PARAM_GPU_ID_MALI400:
   case DRM_LIMA_PARAM_GPU_ID_MALI450:
      screen->gpu_type = param.value;
      break;
   default:
      return false;
   }

   memset(&param, 0, sizeof(param));
   param.param = DRM_LIMA_PARAM_NUM_PP;
   if (drmIoctl(screen->fd, DRM_IOCTL_LIMA_GET_PARAM, &param))
      return false;

   screen->num_pp = param.value;

   lima_screen_set_plb_max_blk(screen);

   return true;
}

static const uint64_t lima_available_modifiers[] = {
   DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED,
   DRM_FORMAT_MOD_LINEAR,
};

static bool lima_is_modifier_external_only(enum pipe_format format)
{
   return util_format_is_yuv(format);
}

static void
lima_screen_query_dmabuf_modifiers(struct pipe_screen *pscreen,
                                   enum pipe_format format, int max,
                                   uint64_t *modifiers,
                                   unsigned int *external_only,
                                   int *count)
{
   int num_modifiers = ARRAY_SIZE(lima_available_modifiers);

   if (!modifiers) {
      *count = num_modifiers;
      return;
   }

   *count = MIN2(max, num_modifiers);
   for (int i = 0; i < *count; i++) {
      modifiers[i] = lima_available_modifiers[i];
      if (external_only)
         external_only[i] = lima_is_modifier_external_only(format);
   }
}

static bool
lima_screen_is_dmabuf_modifier_supported(struct pipe_screen *pscreen,
                                         uint64_t modifier,
                                         enum pipe_format format,
                                         bool *external_only)
{
   for (int i = 0; i < ARRAY_SIZE(lima_available_modifiers); i++) {
      if (lima_available_modifiers[i] == modifier) {
         if (external_only)
            *external_only = lima_is_modifier_external_only(format);

         return true;
      }
   }

   return false;
}

static const struct debug_named_value lima_debug_options[] = {
        { "gp",       LIMA_DEBUG_GP,
          "print GP shader compiler result of each stage" },
        { "pp",       LIMA_DEBUG_PP,
          "print PP shader compiler result of each stage" },
        { "dump",     LIMA_DEBUG_DUMP,
          "dump GPU command stream to $PWD/lima.dump" },
        { "shaderdb", LIMA_DEBUG_SHADERDB,
          "print shader information for shaderdb" },
        { "nobocache", LIMA_DEBUG_NO_BO_CACHE,
          "disable BO cache" },
        { "bocache", LIMA_DEBUG_BO_CACHE,
          "print debug info for BO cache" },
        { "notiling", LIMA_DEBUG_NO_TILING,
          "don't use tiled buffers" },
        { "nogrowheap",   LIMA_DEBUG_NO_GROW_HEAP,
          "disable growable heap buffer" },
        { "singlejob", LIMA_DEBUG_SINGLE_JOB,
          "disable multi job optimization" },
        { "precompile", LIMA_DEBUG_PRECOMPILE,
          "Precompile shaders for shader-db" },
        { "diskcache", LIMA_DEBUG_DISK_CACHE,
          "print debug info for shader disk cache" },
        { "noblit", LIMA_DEBUG_NO_BLIT,
          "use generic u_blitter instead of lima-specific" },
        DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(lima_debug, "LIMA_DEBUG", lima_debug_options, 0)
uint32_t lima_debug;

static void
lima_screen_parse_env(void)
{
   lima_debug = debug_get_option_lima_debug();

   lima_ctx_num_plb = debug_get_num_option("LIMA_CTX_NUM_PLB", LIMA_CTX_PLB_DEF_NUM);
   if (lima_ctx_num_plb > LIMA_CTX_PLB_MAX_NUM ||
       lima_ctx_num_plb < LIMA_CTX_PLB_MIN_NUM) {
      fprintf(stderr, "lima: LIMA_CTX_NUM_PLB %d out of range [%d %d], "
              "reset to default %d\n", lima_ctx_num_plb, LIMA_CTX_PLB_MIN_NUM,
              LIMA_CTX_PLB_MAX_NUM, LIMA_CTX_PLB_DEF_NUM);
      lima_ctx_num_plb = LIMA_CTX_PLB_DEF_NUM;
   }

   lima_plb_max_blk = debug_get_num_option("LIMA_PLB_MAX_BLK", 0);
   if (lima_plb_max_blk < 0 || lima_plb_max_blk > 65536) {
      fprintf(stderr, "lima: LIMA_PLB_MAX_BLK %d out of range [%d %d], "
              "reset to default %d\n", lima_plb_max_blk, 0, 65536, 0);
      lima_plb_max_blk = 0;
   }

   lima_ppir_force_spilling = debug_get_num_option("LIMA_PPIR_FORCE_SPILLING", 0);
   if (lima_ppir_force_spilling < 0) {
      fprintf(stderr, "lima: LIMA_PPIR_FORCE_SPILLING %d less than 0, "
              "reset to default 0\n", lima_ppir_force_spilling);
      lima_ppir_force_spilling = 0;
   }

   lima_plb_pp_stream_cache_size = debug_get_num_option("LIMA_PLB_PP_STREAM_CACHE_SIZE", 0);
   if (lima_plb_pp_stream_cache_size < 0) {
      fprintf(stderr, "lima: LIMA_PLB_PP_STREAM_CACHE_SIZE %d less than 0, "
              "reset to default 0\n", lima_plb_pp_stream_cache_size);
      lima_plb_pp_stream_cache_size = 0;
   }
}

static struct disk_cache *
lima_get_disk_shader_cache (struct pipe_screen *pscreen)
{
   struct lima_screen *screen = lima_screen(pscreen);

   return screen->disk_cache;
}

static int
lima_screen_get_fd(struct pipe_screen *pscreen)
{
   struct lima_screen *screen = lima_screen(pscreen);
   return screen->fd;
}

struct pipe_screen *
lima_screen_create(int fd, const struct pipe_screen_config *config,
                   struct renderonly *ro)
{
   uint64_t system_memory;
   struct lima_screen *screen;

   screen = rzalloc(NULL, struct lima_screen);
   if (!screen)
      return NULL;

   screen->fd = fd;
   screen->ro = ro;

   lima_screen_parse_env();

   /* Limit PP PLB stream cache size to 0.1% of system memory */
   if (!lima_plb_pp_stream_cache_size &&
       os_get_total_physical_memory(&system_memory))
      lima_plb_pp_stream_cache_size = system_memory >> 10;

   /* Set lower limit on PP PLB cache size */
   lima_plb_pp_stream_cache_size = MAX2(128 * 1024 * lima_ctx_num_plb,
                                        lima_plb_pp_stream_cache_size);

   driParseConfigFiles(config->options, config->options_info, 0,
                       "lima", NULL, NULL, NULL, 0, NULL, 0);

   if (!lima_screen_query_info(screen))
      goto err_out0;

   if (!lima_bo_cache_init(screen))
      goto err_out0;

   if (!lima_bo_table_init(screen))
      goto err_out1;

   screen->pp_ra = ppir_regalloc_init(screen);
   if (!screen->pp_ra)
      goto err_out2;

   screen->pp_buffer = lima_bo_create(screen, pp_buffer_size, 0);
   if (!screen->pp_buffer)
      goto err_out2;
   screen->pp_buffer->cacheable = false;

   /* fs program for clear buffer?
    */
   static const uint32_t pp_clear_program[] = {
      PP_CLEAR_PROGRAM
   };
   memcpy(lima_bo_map(screen->pp_buffer) + pp_clear_program_offset,
          pp_clear_program, sizeof(pp_clear_program));

   /* copy texture to framebuffer, used to reload gpu tile buffer
    * load.v $1 0.xy, texld 0, mov.v0 $0 ^tex_sampler, sync, stop
    */
   static const uint32_t pp_reload_program[] = {
      0x000005e6, 0xf1003c20, 0x00000000, 0x39001000,
      0x00000e4e, 0x000007cf, 0x00000000, 0x00000000,
   };
   memcpy(lima_bo_map(screen->pp_buffer) + pp_reload_program_offset,
          pp_reload_program, sizeof(pp_reload_program));

   /* 0/1/2 vertex index for reload/clear draw */
   static const uint8_t pp_shared_index[] = { 0, 1, 2 };
   memcpy(lima_bo_map(screen->pp_buffer) + pp_shared_index_offset,
          pp_shared_index, sizeof(pp_shared_index));

   /* 4096x4096 gl pos used for partial clear */
   static const float pp_clear_gl_pos[] = {
      4096, 0,    1, 1,
      0,    0,    1, 1,
      0,    4096, 1, 1,
   };
   memcpy(lima_bo_map(screen->pp_buffer) + pp_clear_gl_pos_offset,
          pp_clear_gl_pos, sizeof(pp_clear_gl_pos));

   /* is pp frame render state static? */
   uint32_t *pp_frame_rsw = lima_bo_map(screen->pp_buffer) + pp_frame_rsw_offset;
   memset(pp_frame_rsw, 0, 0x40);
   pp_frame_rsw[8] = 0x0000f008;
   pp_frame_rsw[9] = screen->pp_buffer->va + pp_clear_program_offset;
   pp_frame_rsw[13] = 0x00000100;

   for (unsigned i = 0; i <= MESA_SHADER_COMPUTE; i++)
      screen->base.nir_options[i] = lima_program_get_compiler_options(i);

   screen->base.destroy = lima_screen_destroy;
   screen->base.get_screen_fd = lima_screen_get_fd;
   screen->base.get_name = lima_screen_get_name;
   screen->base.get_vendor = lima_screen_get_vendor;
   screen->base.get_device_vendor = lima_screen_get_device_vendor;
   screen->base.context_create = lima_context_create;
   screen->base.is_format_supported = lima_screen_is_format_supported;
   screen->base.query_dmabuf_modifiers = lima_screen_query_dmabuf_modifiers;
   screen->base.is_dmabuf_modifier_supported = lima_screen_is_dmabuf_modifier_supported;
   screen->base.get_disk_shader_cache = lima_get_disk_shader_cache;

   lima_resource_screen_init(screen);
   lima_fence_screen_init(screen);
   lima_disk_cache_init(screen);

   lima_init_shader_caps(&screen->base);
   lima_init_screen_caps(&screen->base);

   slab_create_parent(&screen->transfer_pool, sizeof(struct lima_transfer), 16);

   return &screen->base;

err_out2:
   lima_bo_table_fini(screen);
err_out1:
   lima_bo_cache_fini(screen);
err_out0:
   ralloc_free(screen);
   return NULL;
}
