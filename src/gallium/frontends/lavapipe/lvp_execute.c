/*
 * Copyright © 2019 Red Hat.
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

/* use a gallium context to execute a command buffer */

#include "lvp_private.h"
#include "lvp_acceleration_structure.h"

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "lvp_conv.h"

#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_from_mesa.h"

#include "util/format/u_format.h"
#include "util/u_surface.h"
#include "util/u_sampler.h"
#include "util/box.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#include "util/u_prim_restart.h"
#include "util/format/u_format_zs.h"
#include "util/ptralloc.h"
#include "tgsi/tgsi_from_mesa.h"

#include "vk_blend.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_descriptor_update_template.h"
#include "vk_util.h"
#include "vk_enum_to_str.h"

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>

#define DOUBLE_EQ(a, b) (fabs((a) - (b)) < DBL_EPSILON)

enum gs_output {
  GS_OUTPUT_NONE,
  GS_OUTPUT_NOT_LINES,
  GS_OUTPUT_LINES,
};

struct descriptor_buffer_offset {
   uint32_t buffer_index;
   VkDeviceSize offset;

   const struct lvp_descriptor_set_layout *sampler_layout;
};

struct lvp_render_attachment {
   struct lvp_image_view *imgv;
   VkResolveModeFlags resolve_mode;
   struct lvp_image_view *resolve_imgv;
   VkAttachmentLoadOp load_op;
   VkAttachmentStoreOp store_op;
   VkClearValue clear_value;
   bool read_only;
};

struct lvp_conditional_rendering_state {
   struct pipe_resource *buffer;
   uint32_t offset;
   bool condition;
   bool enabled;
};

struct rendering_state {
   struct pipe_context *pctx;
   struct lvp_device *device;
   struct u_upload_mgr *uploader;
   struct cso_context *cso;

   bool blend_dirty;
   bool rs_dirty;
   bool dsa_dirty;
   bool dsa_no_stencil;
   bool stencil_ref_dirty;
   bool clip_state_dirty;
   bool blend_color_dirty;
   bool ve_dirty;
   bool vb_dirty;
   bool constbuf_dirty[LVP_SHADER_STAGES];
   bool pcbuf_dirty[LVP_SHADER_STAGES];
   bool has_pcbuf[LVP_SHADER_STAGES];
   bool vp_dirty;
   bool scissor_dirty;
   bool ib_dirty;
   bool sample_mask_dirty;
   bool min_samples_dirty;
   bool poison_mem;
   bool noop_fs_bound;
   struct pipe_draw_indirect_info indirect_info;
   struct pipe_draw_info info;

   struct pipe_grid_info dispatch_info;
   struct pipe_grid_info trace_rays_info;
   struct pipe_framebuffer_state framebuffer;
   int fb_map[PIPE_MAX_COLOR_BUFS];
   bool fb_remapped;

   struct pipe_blend_state blend_state;
   struct {
      float offset_units;
      float offset_scale;
      float offset_clamp;
      VkDepthBiasRepresentationEXT representation;
      bool enabled;
   } depth_bias;
   struct pipe_rasterizer_state rs_state;
   struct pipe_depth_stencil_alpha_state dsa_state;

   struct pipe_blend_color blend_color;
   struct pipe_stencil_ref stencil_ref;
   struct pipe_clip_state clip_state;

   int num_scissors;
   struct pipe_scissor_state scissors[16];

   int num_viewports;
   struct pipe_viewport_state viewports[16];
   struct {
      float min, max;
   } depth[16];

   uint8_t patch_vertices;
   uint8_t index_size;
   unsigned index_offset;
   unsigned index_buffer_size; //UINT32_MAX for unset
   struct pipe_resource *index_buffer;
   struct pipe_constant_buffer const_buffer[LVP_SHADER_STAGES][16];
   struct lvp_descriptor_set *desc_sets[LVP_PIPELINE_TYPE_COUNT][MAX_SETS];
   struct pipe_resource *desc_buffers[MAX_SETS];
   uint8_t *desc_buffer_addrs[MAX_SETS];
   struct descriptor_buffer_offset desc_buffer_offsets[LVP_PIPELINE_TYPE_COUNT][MAX_SETS];
   int num_const_bufs[LVP_SHADER_STAGES];
   int num_vb;
   unsigned start_vb;
   bool vb_strides_dirty;
   unsigned vb_strides[PIPE_MAX_ATTRIBS];
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   size_t vb_sizes[PIPE_MAX_ATTRIBS]; //UINT32_MAX for unset
   uint8_t vertex_buffer_index[PIPE_MAX_ATTRIBS]; /* temp storage to sort for start_vb */
   struct cso_velems_state velem;

   bool disable_multisample;
   enum gs_output gs_output_lines : 2;

   uint32_t color_write_disables:8;
   uint32_t pad:13;

   void *velems_cso;

   uint8_t push_constants[128 * 4];
   uint16_t push_size[LVP_PIPELINE_TYPE_COUNT];
   uint16_t gfx_push_sizes[LVP_SHADER_STAGES];

   VkRect2D render_area;
   bool suspending;
   uint32_t color_att_count;
   struct lvp_render_attachment color_att[PIPE_MAX_COLOR_BUFS];
   struct lvp_render_attachment depth_att;
   struct lvp_render_attachment stencil_att;
   struct lvp_image_view *ds_imgv;
   struct lvp_image_view *ds_resolve_imgv;
   uint32_t                                     forced_sample_count;
   VkResolveModeFlagBits                        forced_depth_resolve_mode;
   VkResolveModeFlagBits                        forced_stencil_resolve_mode;

   uint32_t sample_mask;
   unsigned min_samples;
   unsigned rast_samples;
   float min_sample_shading;
   bool force_min_sample;
   bool sample_shading;
   bool depth_clamp_sets_clip;

   uint32_t num_so_targets;
   struct pipe_stream_output_target *so_targets[PIPE_MAX_SO_BUFFERS];
   uint32_t so_offsets[PIPE_MAX_SO_BUFFERS];

   struct lvp_shader *shaders[LVP_SHADER_STAGES];
   bool compute_shader_dirty;

   bool tess_ccw;
   void *tess_states[2];

   struct util_dynarray push_desc_sets;
   struct util_dynarray internal_buffers;

   struct lvp_pipeline *exec_graph;

   struct lvp_conditional_rendering_state conditional_rendering;

   struct {
      struct lvp_shader *compute_shader;
      uint8_t push_constants[128 * 4];
   } saved;
};

static struct pipe_resource *
get_buffer_resource(struct pipe_context *ctx, void *mem)
{
   struct pipe_screen *pscreen = ctx->screen;
   struct pipe_resource templ = {0};

   if (!mem)
      return NULL;

   templ.screen = pscreen;
   templ.target = PIPE_BUFFER;
   templ.format = PIPE_FORMAT_R8_UNORM;
   templ.width0 = UINT32_MAX;
   templ.height0 = 1;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.bind |= PIPE_BIND_CONSTANT_BUFFER;
   templ.flags = PIPE_RESOURCE_FLAG_DONT_OVER_ALLOCATE;

   uint64_t size;
   struct pipe_resource *pres = pscreen->resource_create_unbacked(pscreen, &templ, &size);

   struct llvmpipe_memory_allocation alloc = {
      .cpu_addr = mem,
   };

   pscreen->resource_bind_backing(pscreen, pres, (void *)&alloc, 0, 0, 0);
   return pres;
}

ALWAYS_INLINE static void
assert_subresource_layers(const struct pipe_resource *pres,
                          const struct lvp_image *image,
                          const VkImageSubresourceLayers *layers, const VkOffset3D *offsets)
{
#ifndef NDEBUG
   if (pres->target == PIPE_TEXTURE_3D) {
      assert(layers->baseArrayLayer == 0);
      assert(layers->layerCount == 1);
      assert(offsets[0].z <= pres->depth0);
      assert(offsets[1].z <= pres->depth0);
   } else {
      assert(layers->baseArrayLayer < pres->array_size);
      assert(layers->baseArrayLayer + vk_image_subresource_layer_count(&image->vk, layers) <= pres->array_size);
      assert(offsets[0].z == 0);
      assert(offsets[1].z == 1);
   }
#endif
}

static void finish_fence(struct rendering_state *state)
{
   struct pipe_fence_handle *handle = NULL;

   state->pctx->flush(state->pctx, &handle, 0);

   state->pctx->screen->fence_finish(state->pctx->screen,
                                     NULL,
                                     handle, OS_TIMEOUT_INFINITE);
   state->pctx->screen->fence_reference(state->pctx->screen,
                                        &handle, NULL);
}

static unsigned
get_pcbuf_size(struct rendering_state *state, enum pipe_shader_type pstage)
{
   enum lvp_pipeline_type type =
      ffs(lvp_pipeline_types_from_shader_stages(mesa_to_vk_shader_stage(pstage))) - 1;
   return state->has_pcbuf[pstage] ? state->push_size[type] : 0;
}

static void
update_pcbuf(struct rendering_state *state, enum pipe_shader_type pstage,
             enum pipe_shader_type api_stage)
{
   unsigned size = get_pcbuf_size(state, api_stage);
   if (size) {
      uint8_t *mem;
      struct pipe_constant_buffer cbuf;
      cbuf.buffer_size = size;
      cbuf.buffer = NULL;
      cbuf.user_buffer = NULL;
      u_upload_alloc(state->uploader, 0, size, 64, &cbuf.buffer_offset, &cbuf.buffer, (void**)&mem);
      memcpy(mem, state->push_constants, size);
      state->pctx->set_constant_buffer(state->pctx, pstage, 0, true, &cbuf);
   }
   state->pcbuf_dirty[api_stage] = false;
}

static void emit_compute_state(struct rendering_state *state)
{
   if (state->pcbuf_dirty[MESA_SHADER_COMPUTE])
      update_pcbuf(state, MESA_SHADER_COMPUTE, MESA_SHADER_COMPUTE);

   if (state->constbuf_dirty[MESA_SHADER_COMPUTE]) {
      for (unsigned i = 0; i < state->num_const_bufs[MESA_SHADER_COMPUTE]; i++)
         state->pctx->set_constant_buffer(state->pctx, MESA_SHADER_COMPUTE,
                                          i + 1, false, &state->const_buffer[MESA_SHADER_COMPUTE][i]);
      state->constbuf_dirty[MESA_SHADER_COMPUTE] = false;
   }

   if (state->compute_shader_dirty)
      state->pctx->bind_compute_state(state->pctx, state->shaders[MESA_SHADER_COMPUTE]->shader_cso);

   state->compute_shader_dirty = false;

   state->pcbuf_dirty[MESA_SHADER_RAYGEN] = true;
   state->constbuf_dirty[MESA_SHADER_RAYGEN] = true;
}

static void
emit_fb_state(struct rendering_state *state)
{
   if (state->fb_remapped) {
      struct pipe_framebuffer_state fb = state->framebuffer;
      memset(fb.cbufs, 0, sizeof(fb.cbufs));
      for (unsigned i = 0; i < fb.nr_cbufs; i++) {
         if (state->fb_map[i] < PIPE_MAX_COLOR_BUFS)
            fb.cbufs[state->fb_map[i]] = state->framebuffer.cbufs[i];
      }
      state->pctx->set_framebuffer_state(state->pctx, &fb);
   } else {
      state->pctx->set_framebuffer_state(state->pctx, &state->framebuffer);
   }
}

static void
update_min_samples(struct rendering_state *state)
{
   state->min_samples = 1;
   if (state->sample_shading) {
      state->min_samples = ceil(state->rast_samples * state->min_sample_shading);
      if (state->min_samples > 1)
         state->min_samples = state->rast_samples;
      if (state->min_samples < 1)
         state->min_samples = 1;
   }
   if (state->force_min_sample)
      state->min_samples = state->rast_samples;
   if (state->rast_samples != state->framebuffer.samples) {
      state->framebuffer.samples = state->rast_samples;
      emit_fb_state(state);
   }
}

static void update_vertex_elements_buffer_index(struct rendering_state *state)
{
   for (int i = 0; i < state->velem.count; i++)
      state->velem.velems[i].vertex_buffer_index = state->vertex_buffer_index[i] - state->start_vb;
}

static void emit_state(struct rendering_state *state)
{
   if (!state->shaders[MESA_SHADER_FRAGMENT] && !state->noop_fs_bound) {
      state->pctx->bind_fs_state(state->pctx, state->device->noop_fs);
      state->noop_fs_bound = true;
   }
   if (state->blend_dirty) {
      uint32_t mask = 0;
      /* zero out the colormask values for disabled attachments */
      if (state->color_write_disables) {
         u_foreach_bit(att, state->color_write_disables) {
            mask |= state->blend_state.rt[att].colormask << (att * 4);
            state->blend_state.rt[att].colormask = 0;
         }
      }
      if (state->fb_remapped) {
         struct pipe_blend_state blend = state->blend_state;
         for (unsigned i = 0; i < state->framebuffer.nr_cbufs; i++) {
            if (state->fb_map[i] < PIPE_MAX_COLOR_BUFS) {
               blend.rt[state->fb_map[i]] = state->blend_state.rt[i];
            }
         }
         cso_set_blend(state->cso, &blend);
      } else {
         cso_set_blend(state->cso, &state->blend_state);
      }
      /* reset colormasks using saved bitmask */
      if (state->color_write_disables) {
         const uint32_t att_mask = BITFIELD_MASK(4);
         u_foreach_bit(att, state->color_write_disables) {
            state->blend_state.rt[att].colormask = (mask >> (att * 4)) & att_mask;
         }
      }
      state->blend_dirty = false;
   }

   if (state->rs_dirty) {
      bool ms = state->rs_state.multisample;
      if (state->disable_multisample &&
          (state->gs_output_lines == GS_OUTPUT_LINES ||
           (!state->shaders[MESA_SHADER_GEOMETRY] && u_reduced_prim(state->info.mode) == MESA_PRIM_LINES)))
         state->rs_state.multisample = false;
      assert(offsetof(struct pipe_rasterizer_state, offset_clamp) - offsetof(struct pipe_rasterizer_state, offset_units) == sizeof(float) * 2);
      if (state->depth_bias.enabled) {
         state->rs_state.offset_units = state->depth_bias.offset_units;
         state->rs_state.offset_scale = state->depth_bias.offset_scale;
         state->rs_state.offset_clamp = state->depth_bias.offset_clamp;
         state->rs_state.offset_tri = true;
         state->rs_state.offset_line = true;
         state->rs_state.offset_point = true;

         state->rs_state.offset_units_unscaled =
            state->depth_bias.representation == VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT ||
            state->depth_bias.representation == VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT;

         if (state->depth_bias.representation == VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT) {
            enum pipe_format depth_format = util_format_get_depth_only(state->depth_att.imgv->pformat);
            const struct util_format_description *desc = util_format_description(depth_format);
            state->rs_state.offset_units *= util_get_depth_format_mrd(desc);
         }
      } else {
         state->rs_state.offset_units = 0.0f;
         state->rs_state.offset_scale = 0.0f;
         state->rs_state.offset_clamp = 0.0f;
         state->rs_state.offset_tri = false;
         state->rs_state.offset_line = false;
         state->rs_state.offset_point = false;
      }
      cso_set_rasterizer(state->cso, &state->rs_state);
      state->rs_dirty = false;
      state->rs_state.multisample = ms;
   }

   if (state->dsa_dirty) {
      bool s0_enabled = state->dsa_state.stencil[0].enabled;
      bool s1_enabled = state->dsa_state.stencil[1].enabled;
      if (state->dsa_no_stencil) {
         state->dsa_state.stencil[0].enabled = false;
         state->dsa_state.stencil[1].enabled = false;
      }
      cso_set_depth_stencil_alpha(state->cso, &state->dsa_state);
      state->dsa_dirty = false;
      state->dsa_state.stencil[0].enabled = s0_enabled;
      state->dsa_state.stencil[1].enabled = s1_enabled;
   }

   if (state->sample_mask_dirty) {
      cso_set_sample_mask(state->cso, state->sample_mask);
      state->sample_mask_dirty = false;
   }

   if (state->min_samples_dirty) {
      update_min_samples(state);
      cso_set_min_samples(state->cso, state->min_samples);
      state->min_samples_dirty = false;
   }

   if (state->blend_color_dirty) {
      state->pctx->set_blend_color(state->pctx, &state->blend_color);
      state->blend_color_dirty = false;
   }

   if (state->stencil_ref_dirty) {
      cso_set_stencil_ref(state->cso, state->stencil_ref);
      state->stencil_ref_dirty = false;
   }

   if (state->ve_dirty)
      update_vertex_elements_buffer_index(state);

   if (state->vb_strides_dirty) {
      for (unsigned i = 0; i < state->velem.count; i++)
         state->velem.velems[i].src_stride = state->vb_strides[state->velem.velems[i].vertex_buffer_index];
      state->ve_dirty = true;
      state->vb_strides_dirty = false;
   }

   if (state->ve_dirty) {
      cso_set_vertex_elements(state->cso, &state->velem);
      state->ve_dirty = false;
   }

   if (state->vb_dirty) {
      cso_set_vertex_buffers(state->cso, state->num_vb, false, state->vb);
      state->vb_dirty = false;
   }

   lvp_forall_gfx_stage(sh) {
      if (state->constbuf_dirty[sh]) {
         for (unsigned idx = 0; idx < state->num_const_bufs[sh]; idx++)
            state->pctx->set_constant_buffer(state->pctx, sh,
                                             idx + 1, false, &state->const_buffer[sh][idx]);
      }
      state->constbuf_dirty[sh] = false;
   }

   lvp_forall_gfx_stage(sh) {
      if (state->pcbuf_dirty[sh])
         update_pcbuf(state, sh, sh);
   }

   if (state->vp_dirty) {
      state->pctx->set_viewport_states(state->pctx, 0, state->num_viewports, state->viewports);
      state->vp_dirty = false;
   }

   if (state->scissor_dirty) {
      state->pctx->set_scissor_states(state->pctx, 0, state->num_scissors, state->scissors);
      state->scissor_dirty = false;
   }
}

static void
handle_compute_shader(struct rendering_state *state, struct lvp_shader *shader)
{
   state->shaders[MESA_SHADER_COMPUTE] = shader;

   state->has_pcbuf[MESA_SHADER_COMPUTE] = shader->push_constant_size > 0;

   if (!state->has_pcbuf[MESA_SHADER_COMPUTE])
      state->pcbuf_dirty[MESA_SHADER_COMPUTE] = false;

   state->dispatch_info.block[0] = shader->pipeline_nir->nir->info.workgroup_size[0];
   state->dispatch_info.block[1] = shader->pipeline_nir->nir->info.workgroup_size[1];
   state->dispatch_info.block[2] = shader->pipeline_nir->nir->info.workgroup_size[2];
   state->compute_shader_dirty = true;
}

static void handle_compute_pipeline(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   LVP_FROM_HANDLE(lvp_pipeline, pipeline, cmd->u.bind_pipeline.pipeline);

   handle_compute_shader(state, &pipeline->shaders[MESA_SHADER_COMPUTE]);
}

static void handle_ray_tracing_pipeline(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   LVP_FROM_HANDLE(lvp_pipeline, pipeline, cmd->u.bind_pipeline.pipeline);

   struct lvp_shader *shader = &pipeline->shaders[MESA_SHADER_RAYGEN];

   state->shaders[MESA_SHADER_RAYGEN] = shader;

   state->has_pcbuf[MESA_SHADER_RAYGEN] = shader->push_constant_size > 0;

   if (!state->has_pcbuf[MESA_SHADER_RAYGEN])
      state->pcbuf_dirty[MESA_SHADER_RAYGEN] = false;

   state->trace_rays_info.block[0] = shader->pipeline_nir->nir->info.workgroup_size[0];
   state->trace_rays_info.block[1] = shader->pipeline_nir->nir->info.workgroup_size[1];
   state->trace_rays_info.block[2] = shader->pipeline_nir->nir->info.workgroup_size[2];
}

static void
set_viewport_depth_xform(struct rendering_state *state, unsigned idx)
{
   double n = state->depth[idx].min;
   double f = state->depth[idx].max;

   if (!state->rs_state.clip_halfz) {
      state->viewports[idx].scale[2] = 0.5 * (f - n);
      state->viewports[idx].translate[2] = 0.5 * (n + f);
   } else {
      state->viewports[idx].scale[2] = (f - n);
      state->viewports[idx].translate[2] = n;
   }
}

static void
get_viewport_xform(struct rendering_state *state,
                   const VkViewport *viewport,
                   unsigned idx)
{
   float x = viewport->x;
   float y = viewport->y;
   float half_width = 0.5f * viewport->width;
   float half_height = 0.5f * viewport->height;

   state->viewports[idx].scale[0] = half_width;
   state->viewports[idx].translate[0] = half_width + x;
   state->viewports[idx].scale[1] = half_height;
   state->viewports[idx].translate[1] = half_height + y;

   memcpy(&state->depth[idx].min, &viewport->minDepth, sizeof(float) * 2);
}

static void
update_samples(struct rendering_state *state, VkSampleCountFlags samples)
{
   state->rast_samples = samples;
   state->rs_dirty |= state->rs_state.multisample != (samples > 1);
   state->rs_state.multisample = samples > 1;
   state->min_samples_dirty = true;
}

static void
handle_graphics_stages(struct rendering_state *state, VkShaderStageFlagBits shader_stages, bool dynamic_tess_origin)
{
   u_foreach_bit(b, shader_stages) {
      VkShaderStageFlagBits vk_stage = (1 << b);
      gl_shader_stage stage = vk_to_mesa_shader_stage(vk_stage);

      state->has_pcbuf[stage] = false;

      switch (vk_stage) {
      case VK_SHADER_STAGE_FRAGMENT_BIT:
         state->pctx->bind_fs_state(state->pctx, state->shaders[MESA_SHADER_FRAGMENT]->shader_cso);
         state->noop_fs_bound = false;
         break;
      case VK_SHADER_STAGE_VERTEX_BIT:
         state->pctx->bind_vs_state(state->pctx, state->shaders[MESA_SHADER_VERTEX]->shader_cso);
         break;
      case VK_SHADER_STAGE_GEOMETRY_BIT:
         state->pctx->bind_gs_state(state->pctx, state->shaders[MESA_SHADER_GEOMETRY]->shader_cso);
         state->gs_output_lines = state->shaders[MESA_SHADER_GEOMETRY]->pipeline_nir->nir->info.gs.output_primitive == MESA_PRIM_LINES ? GS_OUTPUT_LINES : GS_OUTPUT_NOT_LINES;
         break;
      case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
         state->pctx->bind_tcs_state(state->pctx, state->shaders[MESA_SHADER_TESS_CTRL]->shader_cso);
         break;
      case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
         state->tess_states[0] = NULL;
         state->tess_states[1] = NULL;
         if (dynamic_tess_origin) {
            state->tess_states[0] = state->shaders[MESA_SHADER_TESS_EVAL]->shader_cso;
            state->tess_states[1] = state->shaders[MESA_SHADER_TESS_EVAL]->tess_ccw_cso;
            state->pctx->bind_tes_state(state->pctx, state->tess_states[state->tess_ccw]);
         } else {
            state->pctx->bind_tes_state(state->pctx, state->shaders[MESA_SHADER_TESS_EVAL]->shader_cso);
         }
         if (!dynamic_tess_origin)
            state->tess_ccw = false;
         break;
      case VK_SHADER_STAGE_TASK_BIT_EXT:
         state->pctx->bind_ts_state(state->pctx, state->shaders[MESA_SHADER_TASK]->shader_cso);
         break;
      case VK_SHADER_STAGE_MESH_BIT_EXT:
         state->pctx->bind_ms_state(state->pctx, state->shaders[MESA_SHADER_MESH]->shader_cso);
         break;
      default:
         assert(0);
         break;
      }
   }
}

static void
unbind_graphics_stages(struct rendering_state *state, VkShaderStageFlagBits shader_stages)
{
   u_foreach_bit(vkstage, shader_stages) {
      gl_shader_stage stage = vk_to_mesa_shader_stage(1<<vkstage);
      state->has_pcbuf[stage] = false;
      switch (stage) {
      case MESA_SHADER_FRAGMENT:
         if (state->shaders[MESA_SHADER_FRAGMENT])
            state->pctx->bind_fs_state(state->pctx, NULL);
         state->noop_fs_bound = false;
         break;
      case MESA_SHADER_GEOMETRY:
         if (state->shaders[MESA_SHADER_GEOMETRY])
            state->pctx->bind_gs_state(state->pctx, NULL);
         break;
      case MESA_SHADER_TESS_CTRL:
         if (state->shaders[MESA_SHADER_TESS_CTRL])
            state->pctx->bind_tcs_state(state->pctx, NULL);
         break;
      case MESA_SHADER_TESS_EVAL:
         if (state->shaders[MESA_SHADER_TESS_EVAL])
            state->pctx->bind_tes_state(state->pctx, NULL);
         break;
      case MESA_SHADER_VERTEX:
         if (state->shaders[MESA_SHADER_VERTEX])
            state->pctx->bind_vs_state(state->pctx, NULL);
         break;
      case MESA_SHADER_TASK:
         if (state->shaders[MESA_SHADER_TASK])
            state->pctx->bind_ts_state(state->pctx, NULL);
         break;
      case MESA_SHADER_MESH:
         if (state->shaders[MESA_SHADER_MESH])
            state->pctx->bind_ms_state(state->pctx, NULL);
         break;
      default:
         UNREACHABLE("what stage is this?!");
      }
      state->shaders[stage] = NULL;
   }
}

static void
handle_graphics_pushconsts(struct rendering_state *state, gl_shader_stage stage, struct lvp_shader *shader)
{
   state->has_pcbuf[stage] = shader->push_constant_size > 0;
   if (!state->has_pcbuf[stage])
      state->pcbuf_dirty[stage] = false;
}

static void handle_graphics_pipeline(struct lvp_pipeline *pipeline,
                                     struct rendering_state *state)
{
   const struct vk_graphics_pipeline_state *ps = &pipeline->graphics_state;
   lvp_pipeline_shaders_compile(pipeline, true);
   bool dynamic_tess_origin = BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_TS_DOMAIN_ORIGIN);
   unbind_graphics_stages(state,
                          (~pipeline->graphics_state.shader_stages) &
                          (VK_SHADER_STAGE_ALL_GRAPHICS |
                           VK_SHADER_STAGE_TASK_BIT_EXT |
                           VK_SHADER_STAGE_MESH_BIT_EXT));
   lvp_forall_gfx_stage(sh) {
      if (pipeline->graphics_state.shader_stages & mesa_to_vk_shader_stage(sh))
         state->shaders[sh] = &pipeline->shaders[sh];
   }

   handle_graphics_stages(state, pipeline->graphics_state.shader_stages, dynamic_tess_origin);
   lvp_forall_gfx_stage(sh) {
      handle_graphics_pushconsts(state, sh, &pipeline->shaders[sh]);
   }

   /* rasterization state */
   if (ps->rs) {
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLAMP_ENABLE))
         state->rs_state.depth_clamp = ps->rs->depth_clamp_enable;
      if (BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE)) {
         state->depth_clamp_sets_clip = false;
      } else {
         state->depth_clamp_sets_clip =
            ps->rs->depth_clip_enable == VK_MESA_DEPTH_CLIP_ENABLE_NOT_CLAMP;
         if (state->depth_clamp_sets_clip)
            state->rs_state.depth_clip_near = state->rs_state.depth_clip_far = !state->rs_state.depth_clamp;
         else
            state->rs_state.depth_clip_near = state->rs_state.depth_clip_far =
               vk_rasterization_state_depth_clip_enable(ps->rs);
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE))
         state->rs_state.rasterizer_discard = ps->rs->rasterizer_discard_enable;

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE)) {
         state->rs_state.line_smooth = pipeline->line_smooth;
         state->rs_state.line_rectangular = pipeline->line_rectangular;
      }
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE))
         state->rs_state.line_stipple_enable = ps->rs->line.stipple.enable;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE)) {
         state->rs_state.fill_front = vk_polygon_mode_to_pipe(ps->rs->polygon_mode);
         state->rs_state.fill_back = vk_polygon_mode_to_pipe(ps->rs->polygon_mode);
      }
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX)) {
         state->rs_state.flatshade_first =
            ps->rs->provoking_vertex == VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_LINE_WIDTH))
         state->rs_state.line_width = ps->rs->line.width;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE)) {
         state->rs_state.line_stipple_factor = ps->rs->line.stipple.factor - 1;
         state->rs_state.line_stipple_pattern = ps->rs->line.stipple.pattern;
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE))
         state->depth_bias.enabled = ps->rs->depth_bias.enable;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS)) {
         state->depth_bias.offset_units = ps->rs->depth_bias.constant_factor;
         state->depth_bias.offset_scale = ps->rs->depth_bias.slope_factor;
         state->depth_bias.offset_clamp = ps->rs->depth_bias.clamp;
         state->depth_bias.representation = ps->rs->depth_bias.representation;
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_CULL_MODE))
         state->rs_state.cull_face = vk_cull_to_pipe(ps->rs->cull_mode);

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_FRONT_FACE))
         state->rs_state.front_ccw = (ps->rs->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);
      state->rs_dirty = true;
   }

   if (ps->ds) {
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE))
         state->dsa_state.depth_enabled = ps->ds->depth.test_enable;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE))
         state->dsa_state.depth_writemask = ps->ds->depth.write_enable;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP))
         state->dsa_state.depth_func = ps->ds->depth.compare_op;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_ENABLE))
         state->dsa_state.depth_bounds_test = ps->ds->depth.bounds_test.enable;

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_BOUNDS)) {
         state->dsa_state.depth_bounds_min = ps->ds->depth.bounds_test.min;
         state->dsa_state.depth_bounds_max = ps->ds->depth.bounds_test.max;
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE)) {
         state->dsa_state.stencil[0].enabled = ps->ds->stencil.test_enable;
         state->dsa_state.stencil[1].enabled = ps->ds->stencil.test_enable;
      }

      const struct vk_stencil_test_face_state *front = &ps->ds->stencil.front;
      const struct vk_stencil_test_face_state *back = &ps->ds->stencil.back;

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_OP)) {
         state->dsa_state.stencil[0].func = front->op.compare;
         state->dsa_state.stencil[0].fail_op = vk_conv_stencil_op(front->op.fail);
         state->dsa_state.stencil[0].zpass_op = vk_conv_stencil_op(front->op.pass);
         state->dsa_state.stencil[0].zfail_op = vk_conv_stencil_op(front->op.depth_fail);

         state->dsa_state.stencil[1].func = back->op.compare;
         state->dsa_state.stencil[1].fail_op = vk_conv_stencil_op(back->op.fail);
         state->dsa_state.stencil[1].zpass_op = vk_conv_stencil_op(back->op.pass);
         state->dsa_state.stencil[1].zfail_op = vk_conv_stencil_op(back->op.depth_fail);
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK)) {
         state->dsa_state.stencil[0].valuemask = front->compare_mask;
         state->dsa_state.stencil[1].valuemask = back->compare_mask;
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK)) {
         state->dsa_state.stencil[0].writemask = front->write_mask;
         state->dsa_state.stencil[1].writemask = back->write_mask;
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE)) {
         state->stencil_ref.ref_value[0] = front->reference;
         state->stencil_ref.ref_value[1] = back->reference;
         state->stencil_ref_dirty = true;
      }
      state->dsa_dirty = true;
   }

   if (ps->cb) {
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_CB_LOGIC_OP_ENABLE))
         state->blend_state.logicop_enable = ps->cb->logic_op_enable;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_CB_LOGIC_OP))
         state->blend_state.logicop_func = vk_logic_op_to_pipe(ps->cb->logic_op);

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES))
         state->color_write_disables = ~ps->cb->color_write_enables;

      for (unsigned i = 0; i < ps->cb->attachment_count; i++) {
         const struct vk_color_blend_attachment_state *att = &ps->cb->attachments[i];
         if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_CB_WRITE_MASKS))
            state->blend_state.rt[i].colormask = att->write_mask;
         if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_CB_BLEND_ENABLES))
            state->blend_state.rt[i].blend_enable = att->blend_enable;

         if (!att->blend_enable) {
            state->blend_state.rt[i].rgb_func = 0;
            state->blend_state.rt[i].rgb_src_factor = 0;
            state->blend_state.rt[i].rgb_dst_factor = 0;
            state->blend_state.rt[i].alpha_func = 0;
            state->blend_state.rt[i].alpha_src_factor = 0;
            state->blend_state.rt[i].alpha_dst_factor = 0;
         } else if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_CB_BLEND_EQUATIONS)) {
            state->blend_state.rt[i].rgb_func = vk_blend_op_to_pipe(att->color_blend_op);
            state->blend_state.rt[i].rgb_src_factor = vk_blend_factor_to_pipe(att->src_color_blend_factor);
            state->blend_state.rt[i].rgb_dst_factor = vk_blend_factor_to_pipe(att->dst_color_blend_factor);
            state->blend_state.rt[i].alpha_func = vk_blend_op_to_pipe(att->alpha_blend_op);
            state->blend_state.rt[i].alpha_src_factor = vk_blend_factor_to_pipe(att->src_alpha_blend_factor);
            state->blend_state.rt[i].alpha_dst_factor = vk_blend_factor_to_pipe(att->dst_alpha_blend_factor);
         }

         /* At least llvmpipe applies the blend factor prior to the blend function,
          * regardless of what function is used. (like i965 hardware).
          * It means for MIN/MAX the blend factor has to be stomped to ONE.
          */
         if (att->color_blend_op == VK_BLEND_OP_MIN ||
             att->color_blend_op == VK_BLEND_OP_MAX) {
            state->blend_state.rt[i].rgb_src_factor = PIPE_BLENDFACTOR_ONE;
            state->blend_state.rt[i].rgb_dst_factor = PIPE_BLENDFACTOR_ONE;
         }

         if (att->alpha_blend_op == VK_BLEND_OP_MIN ||
             att->alpha_blend_op == VK_BLEND_OP_MAX) {
            state->blend_state.rt[i].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
            state->blend_state.rt[i].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
         }
      }
      state->blend_dirty = true;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS)) {
         memcpy(state->blend_color.color, ps->cb->blend_constants, 4 * sizeof(float));
         state->blend_color_dirty = true;
      }
   } else if (ps->rp->color_attachment_count == 0) {
      memset(&state->blend_state, 0, sizeof(state->blend_state));
      state->blend_state.rt[0].colormask = 0xf;
      state->blend_dirty = true;
   }

   if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE))
      state->disable_multisample = pipeline->disable_multisample;
   if (ps->ms) {
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK)) {
         state->sample_mask = ps->ms->sample_mask;
         state->sample_mask_dirty = true;
      }
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE)) {
         state->blend_state.alpha_to_coverage = ps->ms->alpha_to_coverage_enable;
         state->blend_state.alpha_to_coverage_dither = state->blend_state.alpha_to_coverage;
      }
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_ONE_ENABLE))
         state->blend_state.alpha_to_one = ps->ms->alpha_to_one_enable;
      state->force_min_sample = pipeline->force_min_sample;
      state->sample_shading = ps->ms->sample_shading_enable;
      state->min_sample_shading = ps->ms->min_sample_shading;
      state->min_samples_dirty = true;
      state->blend_dirty = true;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES))
         update_samples(state, ps->ms->rasterization_samples);
   } else {
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK) &&
          !BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_ONE_ENABLE))
         state->rs_state.multisample = false;
      state->sample_shading = false;
      state->force_min_sample = false;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK)) {
         state->sample_mask_dirty = state->sample_mask != 0xffffffff;
         state->sample_mask = 0xffffffff;
         state->min_samples_dirty = !!state->min_samples;
         state->min_samples = 0;
      }
      state->blend_dirty |= state->blend_state.alpha_to_coverage || state->blend_state.alpha_to_one;
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE)) {
         state->blend_state.alpha_to_coverage = false;
         state->blend_state.alpha_to_coverage_dither = false;
      }
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_ONE_ENABLE))
         state->blend_state.alpha_to_one = false;
      state->rs_dirty = true;
   }

   if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_VI) && ps->vi) {
      u_foreach_bit(a, ps->vi->attributes_valid) {
         uint32_t b = ps->vi->attributes[a].binding;
         state->velem.velems[a].src_offset = ps->vi->attributes[a].offset;
         state->vertex_buffer_index[a] = b;
         state->velem.velems[a].src_format =
            lvp_vk_format_to_pipe_format(ps->vi->attributes[a].format);
         state->velem.velems[a].dual_slot = false;

         uint32_t d = ps->vi->bindings[b].divisor;
         switch (ps->vi->bindings[b].input_rate) {
         case VK_VERTEX_INPUT_RATE_VERTEX:
            state->velem.velems[a].instance_divisor = 0;
            break;
         case VK_VERTEX_INPUT_RATE_INSTANCE:
            state->velem.velems[a].instance_divisor = d ? d : UINT32_MAX;
            break;
         default:
            UNREACHABLE("Invalid vertex input rate");
         }

         if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_VI_BINDING_STRIDES)) {
            state->vb_strides[b] = ps->vi->bindings[b].stride;
            state->vb_strides_dirty = true;
            state->ve_dirty = true;
         }
      }

      state->velem.count = util_last_bit(ps->vi->attributes_valid);
      state->vb_dirty = true;
      state->ve_dirty = true;
   }

   if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY) && ps->ia) {
      state->info.mode = vk_conv_topology(ps->ia->primitive_topology);
      state->rs_dirty = true;
   }
   if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE) && ps->ia)
      state->info.primitive_restart = ps->ia->primitive_restart_enable;

   if (ps->ts && !BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_TS_PATCH_CONTROL_POINTS)) {
      if (state->patch_vertices != ps->ts->patch_control_points)
         state->pctx->set_patch_vertices(state->pctx, ps->ts->patch_control_points);
      state->patch_vertices = ps->ts->patch_control_points;
   }

   if (ps->vp) {
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT)) {
         state->num_viewports = ps->vp->viewport_count;
         state->vp_dirty = true;
      }
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_VP_SCISSOR_COUNT)) {
         state->num_scissors = ps->vp->scissor_count;
         state->scissor_dirty = true;
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_VP_VIEWPORTS)) {
         for (uint32_t i = 0; i < ps->vp->viewport_count; i++) {
            get_viewport_xform(state, &ps->vp->viewports[i], i);
            set_viewport_depth_xform(state, i);
         }
         state->vp_dirty = true;
      }
      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_VP_SCISSORS)) {
         for (uint32_t i = 0; i < ps->vp->scissor_count; i++) {
            const VkRect2D *ss = &ps->vp->scissors[i];
            state->scissors[i].minx = ss->offset.x;
            state->scissors[i].miny = ss->offset.y;
            state->scissors[i].maxx = ss->offset.x + ss->extent.width;
            state->scissors[i].maxy = ss->offset.y + ss->extent.height;
         }
         state->scissor_dirty = true;
      }

      if (!BITSET_TEST(ps->dynamic, MESA_VK_DYNAMIC_VP_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE) &&
          state->rs_state.clip_halfz != !ps->vp->depth_clip_negative_one_to_one) {
         state->rs_state.clip_halfz = !ps->vp->depth_clip_negative_one_to_one;
         state->rs_dirty = true;
         for (uint32_t i = 0; i < state->num_viewports; i++)
            set_viewport_depth_xform(state, i);
         state->vp_dirty = true;
      }
   }
}

static void handle_pipeline(struct vk_cmd_queue_entry *cmd,
                            struct rendering_state *state)
{
   LVP_FROM_HANDLE(lvp_pipeline, pipeline, cmd->u.bind_pipeline.pipeline);
   pipeline->used = true;
   if (pipeline->type == LVP_PIPELINE_COMPUTE) {
      handle_compute_pipeline(cmd, state);
   } else if (pipeline->type == LVP_PIPELINE_RAY_TRACING) {
      handle_ray_tracing_pipeline(cmd, state);
   } else if (pipeline->type == LVP_PIPELINE_GRAPHICS) {
      handle_graphics_pipeline(pipeline, state);
   } else if (pipeline->type == LVP_PIPELINE_EXEC_GRAPH) {
      state->exec_graph = pipeline;
   }
   if (pipeline->layout) {
      state->push_size[pipeline->type] = pipeline->layout->push_constant_size;
   } else {
      for (unsigned i = 0; i < ARRAY_SIZE(pipeline->shaders); i++)
         if (pipeline->shaders[i].push_constant_size) {
            state->push_size[pipeline->type] = pipeline->shaders[i].push_constant_size;
            break;
         }
   }
}

static void handle_vertex_buffers2(struct vk_cmd_queue_entry *cmd,
                                   struct rendering_state *state)
{
   struct vk_cmd_bind_vertex_buffers2 *vcb = &cmd->u.bind_vertex_buffers2;

   int i;
   for (i = 0; i < vcb->binding_count; i++) {
      int idx = i + vcb->first_binding;

      state->vb[idx].buffer_offset = vcb->offsets[i];
      if (state->vb_sizes[idx] != UINT32_MAX)
         pipe_resource_reference(&state->vb[idx].buffer.resource, NULL);
      state->vb[idx].buffer.resource = vcb->buffers[i] && (!vcb->sizes || vcb->sizes[i]) ? lvp_buffer_from_handle(vcb->buffers[i])->bo : NULL;
      if (state->vb[idx].buffer.resource && vcb->sizes) {
         if (vcb->sizes[i] == VK_WHOLE_SIZE || vcb->offsets[i] + vcb->sizes[i] >= state->vb[idx].buffer.resource->width0) {
            state->vb_sizes[idx] = UINT32_MAX;
         } else {
            struct pipe_transfer *xfer;
            uint8_t *mem = pipe_buffer_map(state->pctx, state->vb[idx].buffer.resource, 0, &xfer);
            state->pctx->buffer_unmap(state->pctx, xfer);
            state->vb[idx].buffer.resource = get_buffer_resource(state->pctx, mem);
            state->vb[idx].buffer.resource->width0 = MIN2(vcb->offsets[i] + vcb->sizes[i], state->vb[idx].buffer.resource->width0);
            state->vb_sizes[idx] = vcb->sizes[i];
         }
      } else {
         state->vb_sizes[idx] = UINT32_MAX;
      }

      if (vcb->strides) {
         state->vb_strides[idx] = vcb->strides[i];
         state->vb_strides_dirty = true;
      }
   }
   if (vcb->first_binding < state->start_vb)
      state->start_vb = vcb->first_binding;
   if (vcb->first_binding + vcb->binding_count >= state->num_vb)
      state->num_vb = vcb->first_binding + vcb->binding_count;
   state->vb_dirty = true;
}

static void
handle_set_stage_buffer(struct rendering_state *state,
                        struct pipe_resource *bo,
                        size_t offset,
                        gl_shader_stage stage,
                        uint32_t index)
{
   state->const_buffer[stage][index].buffer = bo;
   state->const_buffer[stage][index].buffer_offset = offset;
   state->const_buffer[stage][index].buffer_size = bo->width0;
   state->const_buffer[stage][index].user_buffer = NULL;

   state->constbuf_dirty[stage] = true;

   if (state->num_const_bufs[stage] <= index)
      state->num_const_bufs[stage] = index + 1;
}

static void handle_set_stage(struct rendering_state *state,
                             struct lvp_descriptor_set *set,
                             enum lvp_pipeline_type pipeline_type,
                             gl_shader_stage stage,
                             uint32_t index)
{
   state->desc_sets[pipeline_type][index] = set;
   handle_set_stage_buffer(state, set->bo, 0, stage, index);
}

static void
apply_dynamic_offsets(struct lvp_descriptor_set **out_set, const uint32_t *offsets, uint32_t offset_count,
                      struct rendering_state *state)
{
   if (!offset_count)
      return;

   struct lvp_descriptor_set *in_set = *out_set;

   struct lvp_descriptor_set *set;
   lvp_descriptor_set_create(state->device, in_set->layout, &set);

   util_dynarray_append(&state->push_desc_sets, struct lvp_descriptor_set *, set);

   memcpy(set->map, in_set->map, in_set->bo->width0);

   *out_set = set;

   for (uint32_t i = 0; i < set->layout->binding_count; i++) {
      const struct lvp_descriptor_set_binding_layout *binding = &set->layout->binding[i];
      if (!vk_descriptor_type_is_dynamic(binding->type))
         continue;

      struct lp_descriptor *desc = set->map;
      desc += binding->descriptor_index;

      for (uint32_t j = 0; j < binding->array_size; j++) {
         uint32_t offset_index = binding->dynamic_index + j;
         if (offset_index >= offset_count)
            return;

         desc[j].buffer.u = (uint32_t *)((uint8_t *)desc[j].buffer.u + offsets[offset_index]);
      }
   }
}

static void
handle_descriptor_sets(VkBindDescriptorSetsInfoKHR *bds, struct rendering_state *state)
{
   LVP_FROM_HANDLE(lvp_pipeline_layout, layout, bds->layout);

   uint32_t dynamic_offset_index = 0;

   uint32_t types = lvp_pipeline_types_from_shader_stages(bds->stageFlags);
   u_foreach_bit(pipeline_type, types) {
      for (uint32_t i = 0; i < bds->descriptorSetCount; i++) {
         if (state->desc_buffers[bds->firstSet + i]) {
            /* always unset descriptor buffers when binding sets */
            if (pipeline_type == LVP_PIPELINE_COMPUTE) {
                  bool changed = state->const_buffer[MESA_SHADER_COMPUTE][bds->firstSet + i].buffer == state->desc_buffers[bds->firstSet + i];
                  state->constbuf_dirty[MESA_SHADER_COMPUTE] |= changed;
            } else if (pipeline_type == LVP_PIPELINE_RAY_TRACING) {
                  bool changed = state->const_buffer[MESA_SHADER_RAYGEN][bds->firstSet + i].buffer == state->desc_buffers[bds->firstSet + i];
                  state->constbuf_dirty[MESA_SHADER_RAYGEN] |= changed;
            } else {
               lvp_forall_gfx_stage(j) {
                  bool changed = state->const_buffer[j][bds->firstSet + i].buffer == state->desc_buffers[bds->firstSet + i];
                  state->constbuf_dirty[j] |= changed;
               }
            }
         }
         if (!layout->vk.set_layouts[bds->firstSet + i])
            continue;

         struct lvp_descriptor_set *set = lvp_descriptor_set_from_handle(bds->pDescriptorSets[i]);
         if (!set)
            continue;

         apply_dynamic_offsets(&set, bds->pDynamicOffsets + dynamic_offset_index,
                              bds->dynamicOffsetCount - dynamic_offset_index, state);

         dynamic_offset_index += set->layout->dynamic_offset_count;

         if (pipeline_type == LVP_PIPELINE_COMPUTE || pipeline_type == LVP_PIPELINE_EXEC_GRAPH) {
            if (set->layout->shader_stages & VK_SHADER_STAGE_COMPUTE_BIT)
               handle_set_stage(state, set, pipeline_type, MESA_SHADER_COMPUTE, bds->firstSet + i);
            continue;
         }

         if (pipeline_type == LVP_PIPELINE_RAY_TRACING) {
            if (set->layout->shader_stages & LVP_RAY_TRACING_STAGES)
               handle_set_stage(state, set, pipeline_type, MESA_SHADER_RAYGEN, bds->firstSet + i);
            continue;
         }

         if (set->layout->shader_stages & VK_SHADER_STAGE_VERTEX_BIT)
            handle_set_stage(state, set, pipeline_type, MESA_SHADER_VERTEX, bds->firstSet + i);

         if (set->layout->shader_stages & VK_SHADER_STAGE_GEOMETRY_BIT)
            handle_set_stage(state, set, pipeline_type, MESA_SHADER_GEOMETRY, bds->firstSet + i);

         if (set->layout->shader_stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
            handle_set_stage(state, set, pipeline_type, MESA_SHADER_TESS_CTRL, bds->firstSet + i);

         if (set->layout->shader_stages & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
            handle_set_stage(state, set, pipeline_type, MESA_SHADER_TESS_EVAL, bds->firstSet + i);

         if (set->layout->shader_stages & VK_SHADER_STAGE_FRAGMENT_BIT)
            handle_set_stage(state, set, pipeline_type, MESA_SHADER_FRAGMENT, bds->firstSet + i);

         if (set->layout->shader_stages & VK_SHADER_STAGE_TASK_BIT_EXT)
            handle_set_stage(state, set, pipeline_type, MESA_SHADER_TASK, bds->firstSet + i);

         if (set->layout->shader_stages & VK_SHADER_STAGE_MESH_BIT_EXT)
            handle_set_stage(state, set, pipeline_type, MESA_SHADER_MESH, bds->firstSet + i);
      }
   }
}

static void
handle_descriptor_sets_cmd(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   VkBindDescriptorSetsInfoKHR *bds = cmd->u.bind_descriptor_sets2.bind_descriptor_sets_info;
   handle_descriptor_sets(bds, state);
}

static struct pipe_surface create_img_surface_bo(struct rendering_state *state,
                                                  VkImageSubresourceRange *range,
                                                  struct pipe_resource *bo,
                                                  enum pipe_format pformat,
                                                  int base_layer, int layer_count,
                                                  int level)
{
   if (pformat == PIPE_FORMAT_NONE)
      return (struct pipe_surface){0};

   struct pipe_surface template = {
      .format = pformat,
      .texture = bo,
      .first_layer = range->baseArrayLayer + base_layer,
      .last_layer = range->baseArrayLayer + base_layer + layer_count - 1,
      .level = range->baseMipLevel + level,
   };
   return template;
}
static struct pipe_surface create_img_surface(struct rendering_state *state,
                                               struct lvp_image_view *imgv,
                                               VkFormat format,
                                               int base_layer, int layer_count)
{
   VkImageSubresourceRange imgv_subres =
      vk_image_view_subresource_range(&imgv->vk);

   return create_img_surface_bo(state, &imgv_subres, imgv->image->planes[0].bo,
                                lvp_vk_format_to_pipe_format(format),
                                base_layer, layer_count, 0);
}

static void add_img_view_surface(struct rendering_state *state,
                                 struct lvp_image_view *imgv,
                                 int layer_count)
{
   imgv->surface = create_img_surface(state, imgv, imgv->vk.format, 0, layer_count);
}

static bool
render_needs_clear(struct rendering_state *state)
{
   for (uint32_t i = 0; i < state->color_att_count; i++) {
      if (state->color_att[i].load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
         return true;
   }
   if (state->depth_att.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
      return true;
   if (state->stencil_att.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
      return true;
   return false;
}

static void clear_attachment_layers(struct rendering_state *state,
                                    struct lvp_image_view *imgv,
                                    const VkRect2D *rect,
                                    unsigned base_layer, unsigned layer_count,
                                    unsigned ds_clear_flags, double dclear_val,
                                    uint32_t sclear_val,
                                    union pipe_color_union *col_val)
{
   struct pipe_surface clear_surf = create_img_surface(state,
                                                       imgv,
                                                       imgv->vk.format,
                                                       base_layer,
                                                       layer_count);

   if (ds_clear_flags) {
      state->pctx->clear_depth_stencil(state->pctx,
                                       &clear_surf,
                                       ds_clear_flags,
                                       dclear_val, sclear_val,
                                       rect->offset.x, rect->offset.y,
                                       rect->extent.width, rect->extent.height,
                                       true);
   } else {
      state->pctx->clear_render_target(state->pctx, &clear_surf,
                                       col_val,
                                       rect->offset.x, rect->offset.y,
                                       rect->extent.width, rect->extent.height,
                                       true);
   }
}

static void render_clear(struct rendering_state *state)
{
   for (uint32_t i = 0; i < state->color_att_count; i++) {
      if (state->color_att[i].load_op != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      union pipe_color_union color_clear_val = { 0 };
      const VkClearValue value = state->color_att[i].clear_value;
      color_clear_val.ui[0] = value.color.uint32[0];
      color_clear_val.ui[1] = value.color.uint32[1];
      color_clear_val.ui[2] = value.color.uint32[2];
      color_clear_val.ui[3] = value.color.uint32[3];

      struct lvp_image_view *imgv = state->color_att[i].imgv;

      if (state->framebuffer.viewmask) {
         u_foreach_bit(i, state->framebuffer.viewmask)
            clear_attachment_layers(state, imgv, &state->render_area,
                                    i, 1, 0, 0, 0, &color_clear_val);
      } else {
         state->pctx->clear_render_target(state->pctx,
                                          &imgv->surface,
                                          &color_clear_val,
                                          state->render_area.offset.x,
                                          state->render_area.offset.y,
                                          state->render_area.extent.width,
                                          state->render_area.extent.height,
                                          false);
      }
   }

   uint32_t ds_clear_flags = 0;
   double dclear_val = 0;
   if (state->depth_att.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      ds_clear_flags |= PIPE_CLEAR_DEPTH;
      dclear_val = state->depth_att.clear_value.depthStencil.depth;
   }

   uint32_t sclear_val = 0;
   if (state->stencil_att.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      ds_clear_flags |= PIPE_CLEAR_STENCIL;
      sclear_val = state->stencil_att.clear_value.depthStencil.stencil;
   }

   if (ds_clear_flags) {
      if (state->framebuffer.viewmask) {
         u_foreach_bit(i, state->framebuffer.viewmask)
            clear_attachment_layers(state, state->ds_imgv, &state->render_area,
                                    i, 1, ds_clear_flags, dclear_val, sclear_val, NULL);
      } else {
         state->pctx->clear_depth_stencil(state->pctx,
                                          &state->ds_imgv->surface,
                                          ds_clear_flags,
                                          dclear_val, sclear_val,
                                          state->render_area.offset.x,
                                          state->render_area.offset.y,
                                          state->render_area.extent.width,
                                          state->render_area.extent.height,
                                          false);
      }
   }
}

static void render_clear_fast(struct rendering_state *state)
{
   /*
    * the state tracker clear interface only works if all the attachments have the same
    * clear color.
    */
   /* llvmpipe doesn't support scissored clears yet */
   if (state->render_area.offset.x || state->render_area.offset.y)
      goto slow_clear;

   if (state->render_area.extent.width != state->framebuffer.width ||
       state->render_area.extent.height != state->framebuffer.height)
      goto slow_clear;

   if (state->framebuffer.viewmask)
      goto slow_clear;

   if (state->conditional_rendering.enabled)
      goto slow_clear;

   uint32_t buffers = 0;
   bool has_color_value = false;
   VkClearValue color_value = {0};
   for (uint32_t i = 0; i < state->color_att_count; i++) {
      if (state->color_att[i].load_op != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      buffers |= (PIPE_CLEAR_COLOR0 << i);

      if (has_color_value) {
         if (memcmp(&color_value, &state->color_att[i].clear_value, sizeof(VkClearValue)))
            goto slow_clear;
      } else {
         memcpy(&color_value, &state->color_att[i].clear_value, sizeof(VkClearValue));
         has_color_value = true;
      }
   }

   double dclear_val = 0;
   if (state->depth_att.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      buffers |= PIPE_CLEAR_DEPTH;
      dclear_val = state->depth_att.clear_value.depthStencil.depth;
   }

   uint32_t sclear_val = 0;
   if (state->stencil_att.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      buffers |= PIPE_CLEAR_STENCIL;
      sclear_val = state->stencil_att.clear_value.depthStencil.stencil;
   }

   union pipe_color_union col_val;
   for (unsigned i = 0; i < 4; i++)
      col_val.ui[i] = color_value.color.uint32[i];

   state->pctx->clear(state->pctx, buffers,
                      NULL, &col_val,
                      dclear_val, sclear_val);
   return;

slow_clear:
   render_clear(state);
}

static struct lvp_image_view *
destroy_multisample_surface(struct rendering_state *state, struct lvp_image_view *imgv)
{
   assert(imgv->image->vk.samples > 1);
   struct lvp_image_view *base = imgv->multisample;
   base->multisample = NULL;
   free((void*)imgv->image);
   free(imgv);
   return base;
}

static void
resolve_ds(struct rendering_state *state, bool multi)
{
   VkResolveModeFlagBits depth_resolve_mode = multi ? state->forced_depth_resolve_mode : state->depth_att.resolve_mode;
   VkResolveModeFlagBits stencil_resolve_mode = multi ? state->forced_stencil_resolve_mode : state->stencil_att.resolve_mode;
   if (!depth_resolve_mode && !stencil_resolve_mode)
      return;

   struct lvp_image_view *src_imgv = state->ds_imgv;
   if (multi && !src_imgv->multisample)
      return;
   if (!multi && src_imgv->image->vk.samples == 1)
      return;

   assert(state->depth_att.resolve_imgv == NULL ||
          state->stencil_att.resolve_imgv == NULL ||
          state->depth_att.resolve_imgv == state->stencil_att.resolve_imgv ||
          multi);
   struct lvp_image_view *dst_imgv =
      multi ? src_imgv->multisample :
      state->depth_att.resolve_imgv ? state->depth_att.resolve_imgv :
                                      state->stencil_att.resolve_imgv;

   unsigned num_blits = 1;
   if (depth_resolve_mode != stencil_resolve_mode)
      num_blits = 2;

   for (unsigned i = 0; i < num_blits; i++) {
      if (i == 0 && depth_resolve_mode == VK_RESOLVE_MODE_NONE)
         continue;

      if (i == 1 && stencil_resolve_mode == VK_RESOLVE_MODE_NONE)
         continue;

      struct pipe_blit_info info = {0};

      info.src.resource = src_imgv->image->planes[0].bo;
      info.dst.resource = dst_imgv->image->planes[0].bo;
      info.src.format = src_imgv->pformat;
      info.dst.format = dst_imgv->pformat;
      info.filter = PIPE_TEX_FILTER_NEAREST;

      if (num_blits == 1)
         info.mask = PIPE_MASK_ZS;
      else if (i == 0)
         info.mask = PIPE_MASK_Z;
      else
         info.mask = PIPE_MASK_S;

      if (i == 0 && depth_resolve_mode == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
         info.sample0_only = true;
      if (i == 1 && stencil_resolve_mode == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
         info.sample0_only = true;

      info.src.box.x = state->render_area.offset.x;
      info.src.box.y = state->render_area.offset.y;
      info.src.box.width = state->render_area.extent.width;
      info.src.box.height = state->render_area.extent.height;
      info.src.box.depth = state->framebuffer.layers;

      info.dst.box = info.src.box;

      state->pctx->blit(state->pctx, &info);
   }
   if (multi)
      state->ds_imgv = destroy_multisample_surface(state, state->ds_imgv);
}

static void
resolve_color(struct rendering_state *state, bool multi)
{
   for (uint32_t i = 0; i < state->color_att_count; i++) {
      if (!state->color_att[i].resolve_mode &&
          !(multi && state->forced_sample_count && state->color_att[i].imgv))
         continue;

      struct lvp_image_view *src_imgv = state->color_att[i].imgv;
      /* skip non-msrtss resolves during msrtss resolve */
      if (multi && !src_imgv->multisample)
         continue;
      struct lvp_image_view *dst_imgv = multi ? src_imgv->multisample : state->color_att[i].resolve_imgv;

      struct pipe_blit_info info = { 0 };

      info.src.resource = src_imgv->image->planes[0].bo;
      info.dst.resource = dst_imgv->image->planes[0].bo;
      info.src.format = src_imgv->pformat;
      info.dst.format = dst_imgv->pformat;
      info.filter = PIPE_TEX_FILTER_NEAREST;
      info.mask = PIPE_MASK_RGBA;
      info.src.box.x = state->render_area.offset.x;
      info.src.box.y = state->render_area.offset.y;
      info.src.box.width = state->render_area.extent.width;
      info.src.box.height = state->render_area.extent.height;
      info.src.box.depth = state->framebuffer.layers;

      info.dst.box = info.src.box;
      info.src.box.z = src_imgv->vk.base_array_layer;
      info.dst.box.z = dst_imgv->vk.base_array_layer;

      info.src.level = src_imgv->vk.base_mip_level;
      info.dst.level = dst_imgv->vk.base_mip_level;

      state->pctx->blit(state->pctx, &info);
   }

   if (!multi)
      return;
   for (uint32_t i = 0; i < state->color_att_count; i++) {
      struct lvp_image_view *src_imgv = state->color_att[i].imgv;
      if (src_imgv && src_imgv->multisample) //check if it has a msrtss view
         state->color_att[i].imgv = destroy_multisample_surface(state, src_imgv);
   }
}

static void render_resolve(struct rendering_state *state)
{
   if (state->forced_sample_count) {
      resolve_ds(state, true);
      resolve_color(state, true);
   }
   resolve_ds(state, false);
   resolve_color(state, false);
}

static void
replicate_attachment(struct rendering_state *state,
                     struct lvp_image_view *src,
                     struct lvp_image_view *dst)
{
   unsigned level = dst->surface.level;
   const struct pipe_box box = {
      .x = 0,
      .y = 0,
      .z = 0,
      .width = u_minify(dst->image->planes[0].bo->width0, level),
      .height = u_minify(dst->image->planes[0].bo->height0, level),
      .depth = u_minify(dst->image->planes[0].bo->depth0, level),
   };
   state->pctx->resource_copy_region(state->pctx, dst->image->planes[0].bo, level,
                                     0, 0, 0, src->image->planes[0].bo, level, &box);
}

static struct lvp_image_view *
create_multisample_surface(struct rendering_state *state, struct lvp_image_view *imgv, uint32_t samples, bool replicate)
{
   assert(!imgv->multisample);

   struct pipe_resource templ = *imgv->surface.texture;
   templ.nr_samples = samples;
   struct lvp_image *image = mem_dup(imgv->image, sizeof(struct lvp_image));
   image->vk.samples = samples;
   image->planes[0].pmem = NULL;
   image->planes[0].bo = state->pctx->screen->resource_create(state->pctx->screen, &templ);

   struct lvp_image_view *multi = mem_dup(imgv, sizeof(struct lvp_image_view));
   multi->image = image;
   multi->surface = imgv->surface;
   multi->surface.texture = image->planes[0].bo;
   imgv->multisample = multi;
   multi->multisample = imgv;
   if (replicate)
      replicate_attachment(state, imgv, multi);
   return multi;
}

static bool
att_needs_replicate(const struct rendering_state *state,
                    const struct lvp_image_view *imgv,
                    VkAttachmentLoadOp load_op)
{
   if (load_op == VK_ATTACHMENT_LOAD_OP_LOAD ||
       load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
      return true;
   if (state->render_area.offset.x || state->render_area.offset.y)
      return true;
   if (state->render_area.extent.width < imgv->image->vk.extent.width ||
       state->render_area.extent.height < imgv->image->vk.extent.height)
      return true;
   return false;
}


static void
render_att_init(struct lvp_render_attachment* att,
                const VkRenderingAttachmentInfo *vk_att,
                bool poison_mem, bool stencil)
{
   if (vk_att == NULL || vk_att->imageView == VK_NULL_HANDLE) {
      *att = (struct lvp_render_attachment) {
         .load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      };
      return;
   }

   *att = (struct lvp_render_attachment) {
      .imgv = lvp_image_view_from_handle(vk_att->imageView),
      .load_op = vk_att->loadOp,
      .store_op = vk_att->storeOp,
      .clear_value = vk_att->clearValue,
   };
   if (util_format_is_depth_or_stencil(att->imgv->pformat)) {
      if (stencil) {
         att->read_only =
            (vk_att->imageLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL ||
             vk_att->imageLayout == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL);
      } else {
         att->read_only =
            (vk_att->imageLayout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ||
             vk_att->imageLayout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
      }
   }
   if (poison_mem && !att->read_only && att->load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
      att->load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
      if (util_format_is_depth_or_stencil(att->imgv->pformat)) {
         att->clear_value.depthStencil.depth = 0.12351251;
         att->clear_value.depthStencil.stencil = rand() % UINT8_MAX;
      } else {
         memset(att->clear_value.color.uint32, rand() % UINT8_MAX,
                sizeof(att->clear_value.color.uint32));
      }
   }

   if (vk_att->resolveImageView && vk_att->resolveMode) {
      att->resolve_imgv = lvp_image_view_from_handle(vk_att->resolveImageView);
      att->resolve_mode = vk_att->resolveMode;
   }
}


static void
handle_begin_rendering(struct vk_cmd_queue_entry *cmd,
                       struct rendering_state *state)
{
   const VkRenderingInfo *info = cmd->u.begin_rendering.rendering_info;
   bool resuming = (info->flags & VK_RENDERING_RESUMING_BIT) == VK_RENDERING_RESUMING_BIT;
   bool suspending = (info->flags & VK_RENDERING_SUSPENDING_BIT) == VK_RENDERING_SUSPENDING_BIT;

   state->fb_remapped = false;
   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; i++)
      state->fb_map[i] = i;

   const VkMultisampledRenderToSingleSampledInfoEXT *ssi =
         vk_find_struct_const(info->pNext, MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT);
   if (ssi && ssi->multisampledRenderToSingleSampledEnable) {
      state->forced_sample_count = ssi->rasterizationSamples;
      state->forced_depth_resolve_mode = info->pDepthAttachment ? info->pDepthAttachment->resolveMode : 0;
      state->forced_stencil_resolve_mode = info->pStencilAttachment ? info->pStencilAttachment->resolveMode : 0;
   } else {
      state->forced_sample_count = 0;
      state->forced_depth_resolve_mode = 0;
      state->forced_stencil_resolve_mode = 0;
   }

   state->framebuffer.viewmask = info->viewMask;
   state->render_area = info->renderArea;
   state->suspending = suspending;
   state->framebuffer.width = info->renderArea.offset.x +
                              info->renderArea.extent.width;
   state->framebuffer.height = info->renderArea.offset.y +
                               info->renderArea.extent.height;
   state->framebuffer.layers = info->viewMask ? util_last_bit(info->viewMask) : info->layerCount;
   assert(info->colorAttachmentCount <= PIPE_MAX_COLOR_BUFS);
   state->framebuffer.nr_cbufs = info->colorAttachmentCount;

   state->color_att_count = info->colorAttachmentCount;
   memset(state->framebuffer.cbufs, 0, sizeof(state->framebuffer.cbufs));
   for (unsigned i = 0; i < info->colorAttachmentCount; i++) {
      render_att_init(&state->color_att[i], &info->pColorAttachments[i], state->poison_mem, false);
      if (state->color_att[i].imgv) {
         struct lvp_image_view *imgv = state->color_att[i].imgv;
         add_img_view_surface(state, imgv,
                              state->framebuffer.layers);
         if (state->forced_sample_count && imgv->image->vk.samples == 1)
            state->color_att[i].imgv = create_multisample_surface(state, imgv, state->forced_sample_count,
                                                                  att_needs_replicate(state, imgv, state->color_att[i].load_op));
         state->framebuffer.cbufs[i] = state->color_att[i].imgv->surface;
         assert(state->render_area.offset.x + state->render_area.extent.width <= state->framebuffer.cbufs[i].texture->width0);
         assert(state->render_area.offset.y + state->render_area.extent.height <= state->framebuffer.cbufs[i].texture->height0);
      } else {
         memset(&state->framebuffer.cbufs[i], 0, sizeof(state->framebuffer.cbufs[i]));
      }
   }

   render_att_init(&state->depth_att, info->pDepthAttachment, state->poison_mem, false);
   render_att_init(&state->stencil_att, info->pStencilAttachment, state->poison_mem, true);
   state->dsa_no_stencil = !state->stencil_att.imgv;
   state->dsa_dirty = true;
   if (state->depth_att.imgv || state->stencil_att.imgv) {
      assert(state->depth_att.imgv == NULL ||
             state->stencil_att.imgv == NULL ||
             state->depth_att.imgv == state->stencil_att.imgv);
      state->ds_imgv = state->depth_att.imgv ? state->depth_att.imgv :
                                               state->stencil_att.imgv;
      struct lvp_image_view *imgv = state->ds_imgv;
      add_img_view_surface(state, imgv,
                           state->framebuffer.layers);
      if (state->forced_sample_count && imgv->image->vk.samples == 1) {
         VkAttachmentLoadOp load_op;
         if (state->depth_att.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR ||
             state->stencil_att.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
            load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
         else if (state->depth_att.load_op == VK_ATTACHMENT_LOAD_OP_LOAD ||
                  state->stencil_att.load_op == VK_ATTACHMENT_LOAD_OP_LOAD)
            load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
         else
            load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
         state->ds_imgv = create_multisample_surface(state, imgv, state->forced_sample_count,
                                                     att_needs_replicate(state, imgv, load_op));
      }
      state->framebuffer.zsbuf = state->ds_imgv->surface;
      assert(state->render_area.offset.x + state->render_area.extent.width <= state->framebuffer.zsbuf.texture->width0);
      assert(state->render_area.offset.y + state->render_area.extent.height <= state->framebuffer.zsbuf.texture->height0);
   } else {
      state->ds_imgv = NULL;
      memset(&state->framebuffer.zsbuf, 0, sizeof(state->framebuffer.zsbuf));
   }

   state->pctx->set_framebuffer_state(state->pctx,
                                      &state->framebuffer);

   if (!resuming && render_needs_clear(state))
      render_clear_fast(state);
}

static void handle_end_rendering(struct vk_cmd_queue_entry *cmd,
                                 struct rendering_state *state)
{
   if (state->suspending)
      return;
   render_resolve(state);
   if (!state->poison_mem)
      return;

   /* ensure that textures are correctly framebuffer-referenced in llvmpipe */
   if (state->fb_remapped) {
      state->fb_remapped = false;
      emit_fb_state(state);
   }

   union pipe_color_union color_clear_val;
   memset(color_clear_val.ui, rand() % UINT8_MAX, sizeof(color_clear_val.ui));

   for (unsigned i = 0; i < state->framebuffer.nr_cbufs; i++) {
      if (state->color_att[i].imgv && state->color_att[i].store_op == VK_ATTACHMENT_STORE_OP_DONT_CARE) {
         if (state->framebuffer.viewmask) {
            u_foreach_bit(i, state->framebuffer.viewmask)
               clear_attachment_layers(state, state->color_att[i].imgv, &state->render_area,
                                       i, 1, 0, 0, 0, &color_clear_val);
         } else {
            state->pctx->clear_render_target(state->pctx,
                                             &state->color_att[i].imgv->surface,
                                             &color_clear_val,
                                             state->render_area.offset.x,
                                             state->render_area.offset.y,
                                             state->render_area.extent.width,
                                             state->render_area.extent.height,
                                             false);
         }
      }
   }
   uint32_t ds_clear_flags = 0;
   if (state->depth_att.imgv && !state->depth_att.read_only && state->depth_att.store_op == VK_ATTACHMENT_STORE_OP_DONT_CARE)
      ds_clear_flags |= PIPE_CLEAR_DEPTH;
   if (state->stencil_att.imgv && !state->stencil_att.read_only && state->stencil_att.store_op == VK_ATTACHMENT_STORE_OP_DONT_CARE)
      ds_clear_flags |= PIPE_CLEAR_STENCIL;
   double dclear_val = 0.2389234;
   uint32_t sclear_val = rand() % UINT8_MAX;
   if (ds_clear_flags) {
      if (state->framebuffer.viewmask) {
         u_foreach_bit(i, state->framebuffer.viewmask)
            clear_attachment_layers(state, state->ds_imgv, &state->render_area,
                                    i, 1, ds_clear_flags, dclear_val, sclear_val, NULL);
      } else {
         state->pctx->clear_depth_stencil(state->pctx,
                                          &state->ds_imgv->surface,
                                          ds_clear_flags,
                                          dclear_val, sclear_val,
                                          state->render_area.offset.x,
                                          state->render_area.offset.y,
                                          state->render_area.extent.width,
                                          state->render_area.extent.height,
                                          false);
      }
   }
}

static void
handle_rendering_attachment_locations(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   VkRenderingAttachmentLocationInfoKHR *set = cmd->u.set_rendering_attachment_locations.location_info;
   state->fb_remapped = true;
   memset(state->fb_map, PIPE_MAX_COLOR_BUFS, sizeof(state->fb_map));
   assert(state->color_att_count == set->colorAttachmentCount);
   for (unsigned i = 0; i < state->color_att_count; i++) {
      if (set->pColorAttachmentLocations[i] == VK_ATTACHMENT_UNUSED)
         continue;
      state->fb_map[i] = set->pColorAttachmentLocations[i];
   }
   emit_fb_state(state);
}

static void
handle_rendering_input_attachment_indices(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   /* do nothing */
}

static void handle_draw(struct vk_cmd_queue_entry *cmd,
                        struct rendering_state *state)
{
   struct pipe_draw_start_count_bias draw;

   state->info.index_size = 0;
   state->info.index.resource = NULL;
   state->info.start_instance = cmd->u.draw.first_instance;
   state->info.instance_count = cmd->u.draw.instance_count;

   draw.start = cmd->u.draw.first_vertex;
   draw.count = cmd->u.draw.vertex_count;
   draw.index_bias = 0;

   state->pctx->draw_vbo(state->pctx, &state->info, 0, NULL, &draw, 1);
}

static void handle_draw_multi(struct vk_cmd_queue_entry *cmd,
                              struct rendering_state *state)
{
   struct pipe_draw_start_count_bias *draws = calloc(cmd->u.draw_multi_ext.draw_count,
                                                     sizeof(*draws));

   state->info.index_size = 0;
   state->info.index.resource = NULL;
   state->info.start_instance = cmd->u.draw_multi_ext.first_instance;
   state->info.instance_count = cmd->u.draw_multi_ext.instance_count;
   if (cmd->u.draw_multi_ext.draw_count > 1)
      state->info.increment_draw_id = true;

   for (unsigned i = 0; i < cmd->u.draw_multi_ext.draw_count; i++) {
      draws[i].start = cmd->u.draw_multi_ext.vertex_info[i].firstVertex;
      draws[i].count = cmd->u.draw_multi_ext.vertex_info[i].vertexCount;
      draws[i].index_bias = 0;
   }

   if (cmd->u.draw_multi_indexed_ext.draw_count)
      state->pctx->draw_vbo(state->pctx, &state->info, 0, NULL, draws, cmd->u.draw_multi_ext.draw_count);

   free(draws);
}

static void set_viewport(unsigned first_viewport, unsigned viewport_count,
                         const VkViewport* viewports,
                         struct rendering_state *state)
{
   unsigned base = 0;
   if (first_viewport == UINT32_MAX)
      state->num_viewports = viewport_count;
   else
      base = first_viewport;

   for (unsigned i = 0; i < viewport_count; i++) {
      int idx = i + base;
      const VkViewport *vp = &viewports[i];
      get_viewport_xform(state, vp, idx);
      set_viewport_depth_xform(state, idx);
   }
   state->vp_dirty = true;
}

static void handle_set_viewport(struct vk_cmd_queue_entry *cmd,
                                struct rendering_state *state)
{
   set_viewport(cmd->u.set_viewport.first_viewport,
                cmd->u.set_viewport.viewport_count,
                cmd->u.set_viewport.viewports,
                state);
}

static void handle_set_viewport_with_count(struct vk_cmd_queue_entry *cmd,
                                           struct rendering_state *state)
{
   set_viewport(UINT32_MAX,
                cmd->u.set_viewport_with_count.viewport_count,
                cmd->u.set_viewport_with_count.viewports,
                state);
}

static void set_scissor(unsigned first_scissor,
                        unsigned scissor_count,
                        const VkRect2D *scissors,
                        struct rendering_state *state)
{
   unsigned base = 0;
   if (first_scissor == UINT32_MAX)
      state->num_scissors = scissor_count;
   else
      base = first_scissor;

   for (unsigned i = 0; i < scissor_count; i++) {
      unsigned idx = i + base;
      const VkRect2D *ss = &scissors[i];
      state->scissors[idx].minx = ss->offset.x;
      state->scissors[idx].miny = ss->offset.y;
      state->scissors[idx].maxx = ss->offset.x + ss->extent.width;
      state->scissors[idx].maxy = ss->offset.y + ss->extent.height;
   }
   state->scissor_dirty = true;
}

static void handle_set_scissor(struct vk_cmd_queue_entry *cmd,
                               struct rendering_state *state)
{
   set_scissor(cmd->u.set_scissor.first_scissor,
               cmd->u.set_scissor.scissor_count,
               cmd->u.set_scissor.scissors,
               state);
}

static void handle_set_scissor_with_count(struct vk_cmd_queue_entry *cmd,
                                          struct rendering_state *state)
{
   set_scissor(UINT32_MAX,
               cmd->u.set_scissor_with_count.scissor_count,
               cmd->u.set_scissor_with_count.scissors,
               state);
}

static void handle_set_line_width(struct vk_cmd_queue_entry *cmd,
                                  struct rendering_state *state)
{
   state->rs_state.line_width = cmd->u.set_line_width.line_width;
   state->rs_dirty = true;
}

static void handle_set_depth_bias(struct vk_cmd_queue_entry *cmd,
                                  struct rendering_state *state)
{
   state->depth_bias.offset_units = cmd->u.set_depth_bias.depth_bias_constant_factor;
   state->depth_bias.offset_scale = cmd->u.set_depth_bias.depth_bias_slope_factor;
   state->depth_bias.offset_clamp = cmd->u.set_depth_bias.depth_bias_clamp;
   state->rs_dirty = true;
}

static void handle_set_depth_bias2(struct vk_cmd_queue_entry *cmd,
                                   struct rendering_state *state)
{
   VkDepthBiasInfoEXT *info = cmd->u.set_depth_bias2_ext.depth_bias_info;

   state->depth_bias.offset_units = info->depthBiasConstantFactor;
   state->depth_bias.offset_scale = info->depthBiasSlopeFactor;
   state->depth_bias.offset_clamp = info->depthBiasClamp;

   const VkDepthBiasRepresentationInfoEXT *representation_info =
      vk_find_struct_const(info->pNext, DEPTH_BIAS_REPRESENTATION_INFO_EXT);
   state->depth_bias.representation =
      representation_info ? representation_info->depthBiasRepresentation
                          : VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT;

   state->rs_dirty = true;
}

static void handle_set_blend_constants(struct vk_cmd_queue_entry *cmd,
                                       struct rendering_state *state)
{
   memcpy(state->blend_color.color, cmd->u.set_blend_constants.blend_constants, 4 * sizeof(float));
   state->blend_color_dirty = true;
}

static void handle_set_depth_bounds(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   state->dsa_dirty |= !DOUBLE_EQ(state->dsa_state.depth_bounds_min, cmd->u.set_depth_bounds.min_depth_bounds);
   state->dsa_dirty |= !DOUBLE_EQ(state->dsa_state.depth_bounds_max, cmd->u.set_depth_bounds.max_depth_bounds);
   state->dsa_state.depth_bounds_min = cmd->u.set_depth_bounds.min_depth_bounds;
   state->dsa_state.depth_bounds_max = cmd->u.set_depth_bounds.max_depth_bounds;
}

static void handle_set_stencil_compare_mask(struct vk_cmd_queue_entry *cmd,
                                            struct rendering_state *state)
{
   if (cmd->u.set_stencil_compare_mask.face_mask & VK_STENCIL_FACE_FRONT_BIT)
      state->dsa_state.stencil[0].valuemask = cmd->u.set_stencil_compare_mask.compare_mask;
   if (cmd->u.set_stencil_compare_mask.face_mask & VK_STENCIL_FACE_BACK_BIT)
      state->dsa_state.stencil[1].valuemask = cmd->u.set_stencil_compare_mask.compare_mask;
   state->dsa_dirty = true;
}

static void handle_set_stencil_write_mask(struct vk_cmd_queue_entry *cmd,
                                          struct rendering_state *state)
{
   if (cmd->u.set_stencil_write_mask.face_mask & VK_STENCIL_FACE_FRONT_BIT)
      state->dsa_state.stencil[0].writemask = cmd->u.set_stencil_write_mask.write_mask;
   if (cmd->u.set_stencil_write_mask.face_mask & VK_STENCIL_FACE_BACK_BIT)
      state->dsa_state.stencil[1].writemask = cmd->u.set_stencil_write_mask.write_mask;
   state->dsa_dirty = true;
}

static void handle_set_stencil_reference(struct vk_cmd_queue_entry *cmd,
                                         struct rendering_state *state)
{
   if (cmd->u.set_stencil_reference.face_mask & VK_STENCIL_FACE_FRONT_BIT)
      state->stencil_ref.ref_value[0] = cmd->u.set_stencil_reference.reference;
   if (cmd->u.set_stencil_reference.face_mask & VK_STENCIL_FACE_BACK_BIT)
      state->stencil_ref.ref_value[1] = cmd->u.set_stencil_reference.reference;
   state->stencil_ref_dirty = true;
}

static void
copy_depth_rect(uint8_t * dst,
                enum pipe_format dst_format,
                unsigned dst_stride,
                unsigned dst_x,
                unsigned dst_y,
                unsigned width,
                unsigned height,
                const uint8_t * src,
                enum pipe_format src_format,
                int src_stride,
                unsigned src_x,
                unsigned src_y)
{
   int src_stride_pos = src_stride < 0 ? -src_stride : src_stride;
   int src_blocksize = util_format_get_blocksize(src_format);
   int src_blockwidth = util_format_get_blockwidth(src_format);
   int src_blockheight = util_format_get_blockheight(src_format);
   int dst_blocksize = util_format_get_blocksize(dst_format);
   int dst_blockwidth = util_format_get_blockwidth(dst_format);
   int dst_blockheight = util_format_get_blockheight(dst_format);

   assert(src_blocksize > 0);
   assert(src_blockwidth > 0);
   assert(src_blockheight > 0);

   dst_x /= dst_blockwidth;
   dst_y /= dst_blockheight;
   width = (width + src_blockwidth - 1)/src_blockwidth;
   height = (height + src_blockheight - 1)/src_blockheight;
   src_x /= src_blockwidth;
   src_y /= src_blockheight;

   dst += dst_x * dst_blocksize;
   src += src_x * src_blocksize;
   dst += dst_y * dst_stride;
   src += src_y * src_stride_pos;

   if (dst_format == PIPE_FORMAT_S8_UINT) {
      if (src_format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {
         util_format_z32_float_s8x24_uint_unpack_s_8uint(dst, dst_stride,
                                                         src, src_stride,
                                                         width, height);
      } else if (src_format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
         util_format_z24_unorm_s8_uint_unpack_s_8uint(dst, dst_stride,
                                                      src, src_stride,
                                                      width, height);
      } else {
         abort();
      }
   } else if (dst_format == PIPE_FORMAT_Z24X8_UNORM) {
      util_format_z24_unorm_s8_uint_unpack_z24(dst, dst_stride,
                                               src, src_stride,
                                               width, height);
   } else if (dst_format == PIPE_FORMAT_Z32_FLOAT) {
      if (src_format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {
         util_format_z32_float_s8x24_uint_unpack_z_float((float *)dst, dst_stride,
                                                         src, src_stride,
                                                         width, height);
      } else {
         abort();
      }
   } else if (dst_format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {
      if (src_format == PIPE_FORMAT_Z32_FLOAT)
         util_format_z32_float_s8x24_uint_pack_z_float(dst, dst_stride,
                                                       (float *)src, src_stride,
                                                       width, height);
      else if (src_format == PIPE_FORMAT_S8_UINT)
         util_format_z32_float_s8x24_uint_pack_s_8uint(dst, dst_stride,
                                                       src, src_stride,
                                                       width, height);
      else
         abort();
   } else if (dst_format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
      if (src_format == PIPE_FORMAT_S8_UINT)
         util_format_z24_unorm_s8_uint_pack_s_8uint(dst, dst_stride,
                                                    src, src_stride,
                                                    width, height);
      else if (src_format == PIPE_FORMAT_Z24X8_UNORM)
         util_format_z24_unorm_s8_uint_pack_z24(dst, dst_stride,
                                                src, src_stride,
                                                width, height);
      else
         abort();
   }
}

static void
copy_depth_box(uint8_t *dst,
               enum pipe_format dst_format,
               unsigned dst_stride, uint64_t dst_slice_stride,
               unsigned dst_x, unsigned dst_y, unsigned dst_z,
               unsigned width, unsigned height, unsigned depth,
               const uint8_t * src,
               enum pipe_format src_format,
               int src_stride, uint64_t src_slice_stride,
               unsigned src_x, unsigned src_y, unsigned src_z)
{
   dst += dst_z * dst_slice_stride;
   src += src_z * src_slice_stride;
   for (unsigned z = 0; z < depth; ++z) {
      copy_depth_rect(dst,
                      dst_format,
                      dst_stride,
                      dst_x, dst_y,
                      width, height,
                      src,
                      src_format,
                      src_stride,
                      src_x, src_y);

      dst += dst_slice_stride;
      src += src_slice_stride;
   }
}

static unsigned
subresource_layercount(const struct lvp_image *image, const VkImageSubresourceLayers *sub)
{
   if (sub->layerCount != VK_REMAINING_ARRAY_LAYERS)
      return sub->layerCount;
   return image->vk.array_layers - sub->baseArrayLayer;
}

static void handle_copy_image_to_buffer2(struct vk_cmd_queue_entry *cmd,
                                             struct rendering_state *state)
{
   const struct VkCopyImageToBufferInfo2 *copycmd = cmd->u.copy_image_to_buffer2.copy_image_to_buffer_info;
   LVP_FROM_HANDLE(lvp_image, src_image, copycmd->srcImage);
   struct pipe_box box, dbox;
   struct pipe_transfer *src_t, *dst_t;
   uint8_t *src_data, *dst_data;

   for (uint32_t i = 0; i < copycmd->regionCount; i++) {
      const VkBufferImageCopy2 *region = &copycmd->pRegions[i];
      const VkImageAspectFlagBits aspects = copycmd->pRegions[i].imageSubresource.aspectMask;
      uint8_t plane = lvp_image_aspects_to_plane(src_image, aspects);

      box.x = region->imageOffset.x;
      box.y = region->imageOffset.y;
      box.z = src_image->vk.image_type == VK_IMAGE_TYPE_3D ? region->imageOffset.z : region->imageSubresource.baseArrayLayer;
      box.width = region->imageExtent.width;
      box.height = region->imageExtent.height;
      box.depth = src_image->vk.image_type == VK_IMAGE_TYPE_3D ? region->imageExtent.depth : subresource_layercount(src_image, &region->imageSubresource);

      src_data = state->pctx->texture_map(state->pctx,
                                           src_image->planes[plane].bo,
                                           region->imageSubresource.mipLevel,
                                           PIPE_MAP_READ,
                                           &box,
                                           &src_t);

      dbox.x = region->bufferOffset;
      dbox.y = 0;
      dbox.z = 0;
      dbox.width = lvp_buffer_from_handle(copycmd->dstBuffer)->bo->width0 - region->bufferOffset;
      dbox.height = 1;
      dbox.depth = 1;
      dst_data = state->pctx->buffer_map(state->pctx,
                                           lvp_buffer_from_handle(copycmd->dstBuffer)->bo,
                                           0,
                                           PIPE_MAP_WRITE,
                                           &dbox,
                                           &dst_t);

      enum pipe_format src_format = src_image->planes[plane].bo->format;
      enum pipe_format dst_format = src_format;
      if (util_format_is_depth_or_stencil(src_format)) {
         if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT) {
            dst_format = util_format_get_depth_only(src_format);
         } else if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
            dst_format = PIPE_FORMAT_S8_UINT;
         }
      }

      const struct vk_image_buffer_layout buffer_layout =
         vk_image_buffer_copy_layout(&src_image->vk, &copycmd->pRegions[i]);
      if (src_format != dst_format) {
         copy_depth_box(dst_data, dst_format,
                        buffer_layout.row_stride_B,
                        buffer_layout.image_stride_B,
                        0, 0, 0,
                        region->imageExtent.width,
                        region->imageExtent.height,
                        box.depth,
                        src_data, src_format, src_t->stride, src_t->layer_stride, 0, 0, 0);
      } else {
         util_copy_box((uint8_t *)dst_data, src_format,
                       buffer_layout.row_stride_B,
                       buffer_layout.image_stride_B,
                       0, 0, 0,
                       region->imageExtent.width,
                       region->imageExtent.height,
                       box.depth,
                       src_data, src_t->stride, src_t->layer_stride, 0, 0, 0);
      }
      state->pctx->texture_unmap(state->pctx, src_t);
      state->pctx->buffer_unmap(state->pctx, dst_t);
   }
}

static void handle_copy_buffer_to_image(struct vk_cmd_queue_entry *cmd,
                                        struct rendering_state *state)
{
   const struct VkCopyBufferToImageInfo2 *copycmd = cmd->u.copy_buffer_to_image2.copy_buffer_to_image_info;
   LVP_FROM_HANDLE(lvp_image, dst_image, copycmd->dstImage);

   for (uint32_t i = 0; i < copycmd->regionCount; i++) {
      const VkBufferImageCopy2 *region = &copycmd->pRegions[i];
      struct pipe_box box, sbox;
      struct pipe_transfer *src_t, *dst_t;
      void *src_data, *dst_data;
      const VkImageAspectFlagBits aspects = copycmd->pRegions[i].imageSubresource.aspectMask;
      uint8_t plane = lvp_image_aspects_to_plane(dst_image, aspects);

      sbox.x = region->bufferOffset;
      sbox.y = 0;
      sbox.z = 0;
      sbox.width = lvp_buffer_from_handle(copycmd->srcBuffer)->bo->width0;
      sbox.height = 1;
      sbox.depth = 1;
      src_data = state->pctx->buffer_map(state->pctx,
                                           lvp_buffer_from_handle(copycmd->srcBuffer)->bo,
                                           0,
                                           PIPE_MAP_READ,
                                           &sbox,
                                           &src_t);


      box.x = region->imageOffset.x;
      box.y = region->imageOffset.y;
      box.z = dst_image->vk.image_type == VK_IMAGE_TYPE_3D ? region->imageOffset.z : region->imageSubresource.baseArrayLayer;
      box.width = region->imageExtent.width;
      box.height = region->imageExtent.height;
      box.depth = dst_image->vk.image_type == VK_IMAGE_TYPE_3D ? region->imageExtent.depth : subresource_layercount(dst_image, &region->imageSubresource);

      dst_data = state->pctx->texture_map(state->pctx,
                                           dst_image->planes[plane].bo,
                                           region->imageSubresource.mipLevel,
                                           PIPE_MAP_WRITE,
                                           &box,
                                           &dst_t);

      enum pipe_format dst_format = dst_image->planes[plane].bo->format;
      enum pipe_format src_format = dst_format;
      if (util_format_is_depth_or_stencil(dst_format)) {
         if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT) {
            src_format = util_format_get_depth_only(dst_image->planes[plane].bo->format);
         } else if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
            src_format = PIPE_FORMAT_S8_UINT;
         }
      }

      const struct vk_image_buffer_layout buffer_layout =
         vk_image_buffer_copy_layout(&dst_image->vk, &copycmd->pRegions[i]);
      if (src_format != dst_format) {
         copy_depth_box(dst_data, dst_format,
                        dst_t->stride, dst_t->layer_stride,
                        0, 0, 0,
                        region->imageExtent.width,
                        region->imageExtent.height,
                        box.depth,
                        src_data, src_format,
                        buffer_layout.row_stride_B,
                        buffer_layout.image_stride_B,
                        0, 0, 0);
      } else {
         util_copy_box(dst_data, dst_format,
                       dst_t->stride, dst_t->layer_stride,
                       0, 0, 0,
                       region->imageExtent.width,
                       region->imageExtent.height,
                       box.depth,
                       src_data,
                       buffer_layout.row_stride_B,
                       buffer_layout.image_stride_B,
                       0, 0, 0);
      }
      state->pctx->buffer_unmap(state->pctx, src_t);
      state->pctx->texture_unmap(state->pctx, dst_t);
   }
}

static enum pipe_format
find_depth_format(VkFormat format, VkImageAspectFlagBits aspect)
{
   if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
      switch (format) {
      case VK_FORMAT_D32_SFLOAT:
      case VK_FORMAT_D32_SFLOAT_S8_UINT:
      case VK_FORMAT_D24_UNORM_S8_UINT:
         return PIPE_FORMAT_Z32_FLOAT;
      case VK_FORMAT_D16_UNORM:
      case VK_FORMAT_D16_UNORM_S8_UINT:
         return PIPE_FORMAT_Z16_UNORM;
      default:
         UNREACHABLE("unsupported format/aspect combo");
      }
   }
   assert(aspect == VK_IMAGE_ASPECT_STENCIL_BIT);
   switch (format) {
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
   case VK_FORMAT_D24_UNORM_S8_UINT:
   case VK_FORMAT_D16_UNORM_S8_UINT:
   case VK_FORMAT_S8_UINT:
      return PIPE_FORMAT_S8_UINT;
   default:
      UNREACHABLE("unsupported format/aspect combo");
   }
}

static void handle_copy_image(struct vk_cmd_queue_entry *cmd,
                              struct rendering_state *state)
{
   const struct VkCopyImageInfo2 *copycmd = cmd->u.copy_image2.copy_image_info;
   LVP_FROM_HANDLE(lvp_image, src_image, copycmd->srcImage);
   LVP_FROM_HANDLE(lvp_image, dst_image, copycmd->dstImage);

   for (uint32_t i = 0; i < copycmd->regionCount; i++) {
      const VkImageCopy2 *region = &copycmd->pRegions[i];
      const VkImageAspectFlagBits src_aspects =
         copycmd->pRegions[i].srcSubresource.aspectMask;
      uint8_t src_plane = lvp_image_aspects_to_plane(src_image, src_aspects);
      const VkImageAspectFlagBits dst_aspects =
         copycmd->pRegions[i].dstSubresource.aspectMask;
      uint8_t dst_plane = lvp_image_aspects_to_plane(dst_image, dst_aspects);
      struct pipe_box src_box, dst_box;
      src_box.x = region->srcOffset.x;
      src_box.y = region->srcOffset.y;
      dst_box.x = region->dstOffset.x;
      dst_box.y = region->dstOffset.y;
      dst_box.width = src_box.width = region->extent.width;
      dst_box.height = src_box.height = region->extent.height;
      if (src_image->planes[src_plane].bo->target == PIPE_TEXTURE_3D) {
         dst_box.depth = src_box.depth = region->extent.depth;
         src_box.z = region->srcOffset.z;
         dst_box.z = region->dstOffset.z;
      } else {
         src_box.depth = subresource_layercount(src_image, &region->srcSubresource);
         dst_box.depth = subresource_layercount(dst_image, &region->dstSubresource);
         src_box.z = region->srcSubresource.baseArrayLayer;
         dst_box.z = region->dstSubresource.baseArrayLayer;
      }


      unsigned dstz = dst_image->planes[dst_plane].bo->target == PIPE_TEXTURE_3D ?
                      region->dstOffset.z :
                      region->dstSubresource.baseArrayLayer;
      enum pipe_format src_format = src_image->planes[src_plane].bo->format,
                       dst_format = dst_image->planes[dst_plane].bo->format;
      /* special-casing for maintenance8 zs<->color copies */
      if (util_format_is_depth_or_stencil(src_format) !=
          util_format_is_depth_or_stencil(dst_format) &&
          util_format_get_blocksize(src_format) != util_format_get_blocksize(dst_format)) {
         if (util_format_is_depth_or_stencil(src_image->planes[src_plane].bo->format))
            dst_format = find_depth_format(src_image->vk.format, region->srcSubresource.aspectMask);
         else
            src_format = find_depth_format(dst_image->vk.format, region->dstSubresource.aspectMask);
         struct pipe_transfer *src_t, *dst_t;
         void *src_data, *dst_data;
         src_data = state->pctx->texture_map(state->pctx,
                                             src_image->planes[src_plane].bo,
                                             region->srcSubresource.mipLevel,
                                             PIPE_MAP_READ,
                                             &src_box,
                                             &src_t);
         dst_data = state->pctx->texture_map(state->pctx,
                                             dst_image->planes[dst_plane].bo,
                                             region->dstSubresource.mipLevel,
                                             PIPE_MAP_WRITE,
                                             &dst_box,
                                             &dst_t);
         copy_depth_box(dst_data, dst_format,
                        dst_t->stride, dst_t->layer_stride,
                        0, 0, 0,
                        region->extent.width,
                        region->extent.height,
                        dst_box.depth,
                        src_data, src_format,
                        src_t->stride, src_t->layer_stride,
                        0, 0, 0);
         state->pctx->texture_unmap(state->pctx, src_t);
         state->pctx->texture_unmap(state->pctx, dst_t);
      } else {
         state->pctx->resource_copy_region(state->pctx, dst_image->planes[dst_plane].bo,
                                          region->dstSubresource.mipLevel,
                                          region->dstOffset.x,
                                          region->dstOffset.y,
                                          dstz,
                                          src_image->planes[src_plane].bo,
                                          region->srcSubresource.mipLevel,
                                          &src_box);
      }
   }
}

static void handle_copy_buffer(struct vk_cmd_queue_entry *cmd,
                               struct rendering_state *state)
{
   const VkCopyBufferInfo2 *copycmd = cmd->u.copy_buffer2.copy_buffer_info;

   for (uint32_t i = 0; i < copycmd->regionCount; i++) {
      const VkBufferCopy2 *region = &copycmd->pRegions[i];
      struct pipe_box box = { 0 };
      u_box_1d(region->srcOffset, region->size, &box);
      state->pctx->resource_copy_region(state->pctx, lvp_buffer_from_handle(copycmd->dstBuffer)->bo, 0,
                                        region->dstOffset, 0, 0,
                                        lvp_buffer_from_handle(copycmd->srcBuffer)->bo, 0, &box);
   }
}

static void handle_blit_image(struct vk_cmd_queue_entry *cmd,
                              struct rendering_state *state)
{
   VkBlitImageInfo2 *blitcmd = cmd->u.blit_image2.blit_image_info;
   LVP_FROM_HANDLE(lvp_image, src_image, blitcmd->srcImage);
   LVP_FROM_HANDLE(lvp_image, dst_image, blitcmd->dstImage);

   struct pipe_blit_info info = {
      .src.resource = src_image->planes[0].bo,
      .dst.resource = dst_image->planes[0].bo,
      .src.format = src_image->planes[0].bo->format,
      .dst.format = dst_image->planes[0].bo->format,
      .mask = util_format_is_depth_or_stencil(info.src.format) ? PIPE_MASK_ZS : PIPE_MASK_RGBA,
      .filter = blitcmd->filter == VK_FILTER_NEAREST ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR,
   };

   for (uint32_t i = 0; i < blitcmd->regionCount; i++) {
      int srcX0, srcX1, srcY0, srcY1, srcZ0, srcZ1;
      unsigned dstX0, dstX1, dstY0, dstY1, dstZ0, dstZ1;

      srcX0 = blitcmd->pRegions[i].srcOffsets[0].x;
      srcX1 = blitcmd->pRegions[i].srcOffsets[1].x;
      srcY0 = blitcmd->pRegions[i].srcOffsets[0].y;
      srcY1 = blitcmd->pRegions[i].srcOffsets[1].y;
      srcZ0 = blitcmd->pRegions[i].srcOffsets[0].z;
      srcZ1 = blitcmd->pRegions[i].srcOffsets[1].z;

      dstX0 = blitcmd->pRegions[i].dstOffsets[0].x;
      dstX1 = blitcmd->pRegions[i].dstOffsets[1].x;
      dstY0 = blitcmd->pRegions[i].dstOffsets[0].y;
      dstY1 = blitcmd->pRegions[i].dstOffsets[1].y;
      dstZ0 = blitcmd->pRegions[i].dstOffsets[0].z;
      dstZ1 = blitcmd->pRegions[i].dstOffsets[1].z;

      if (dstX0 < dstX1) {
         info.dst.box.x = dstX0;
         info.src.box.x = srcX0;
         info.dst.box.width = dstX1 - dstX0;
         info.src.box.width = srcX1 - srcX0;
      } else {
         info.dst.box.x = dstX1;
         info.src.box.x = srcX1;
         info.dst.box.width = dstX0 - dstX1;
         info.src.box.width = srcX0 - srcX1;
      }

      if (dstY0 < dstY1) {
         info.dst.box.y = dstY0;
         info.src.box.y = srcY0;
         info.dst.box.height = dstY1 - dstY0;
         info.src.box.height = srcY1 - srcY0;
      } else {
         info.dst.box.y = dstY1;
         info.src.box.y = srcY1;
         info.dst.box.height = dstY0 - dstY1;
         info.src.box.height = srcY0 - srcY1;
      }

      assert_subresource_layers(info.src.resource, src_image, &blitcmd->pRegions[i].srcSubresource, blitcmd->pRegions[i].srcOffsets);
      assert_subresource_layers(info.dst.resource, dst_image, &blitcmd->pRegions[i].dstSubresource, blitcmd->pRegions[i].dstOffsets);
      if (src_image->planes[0].bo->target == PIPE_TEXTURE_3D) {
         if (dstZ0 < dstZ1) {
            if (dst_image->planes[0].bo->target == PIPE_TEXTURE_3D) {
               info.dst.box.z = dstZ0;
               info.dst.box.depth = dstZ1 - dstZ0;
            } else {
               info.dst.box.z = blitcmd->pRegions[i].dstSubresource.baseArrayLayer;
               info.dst.box.depth = subresource_layercount(dst_image, &blitcmd->pRegions[i].dstSubresource);
            }
            info.src.box.z = srcZ0;
            info.src.box.depth = srcZ1 - srcZ0;
         } else {
            if (dst_image->planes[0].bo->target == PIPE_TEXTURE_3D) {
               info.dst.box.z = dstZ1;
               info.dst.box.depth = dstZ0 - dstZ1;
            } else {
               info.dst.box.z = blitcmd->pRegions[i].dstSubresource.baseArrayLayer;
               info.dst.box.depth = subresource_layercount(dst_image, &blitcmd->pRegions[i].dstSubresource);
            }
            info.src.box.z = srcZ1;
            info.src.box.depth = srcZ0 - srcZ1;
         }
      } else {
         info.src.box.z = blitcmd->pRegions[i].srcSubresource.baseArrayLayer;
         info.dst.box.z = blitcmd->pRegions[i].dstSubresource.baseArrayLayer;
         info.src.box.depth = subresource_layercount(src_image, &blitcmd->pRegions[i].srcSubresource);
         info.dst.box.depth = subresource_layercount(dst_image, &blitcmd->pRegions[i].dstSubresource);
      }

      info.src.level = blitcmd->pRegions[i].srcSubresource.mipLevel;
      info.dst.level = blitcmd->pRegions[i].dstSubresource.mipLevel;
      state->pctx->blit(state->pctx, &info);
   }
}

static void handle_fill_buffer(struct vk_cmd_queue_entry *cmd,
                               struct rendering_state *state)
{
   struct vk_cmd_fill_buffer *fillcmd = &cmd->u.fill_buffer;
   uint32_t size = fillcmd->size;
   struct lvp_buffer *dst = lvp_buffer_from_handle(fillcmd->dst_buffer);

   size = vk_buffer_range(&dst->vk, fillcmd->dst_offset, fillcmd->size);
   if (fillcmd->size == VK_WHOLE_SIZE)
      size = ROUND_DOWN_TO(size, 4);

   state->pctx->clear_buffer(state->pctx,
                             dst->bo,
                             fillcmd->dst_offset,
                             size,
                             &fillcmd->data,
                             4);
}

static void handle_update_buffer(struct vk_cmd_queue_entry *cmd,
                                 struct rendering_state *state)
{
   struct vk_cmd_update_buffer *updcmd = &cmd->u.update_buffer;
   uint32_t *dst;
   struct pipe_transfer *dst_t;
   struct pipe_box box;

   u_box_1d(updcmd->dst_offset, updcmd->data_size, &box);
   dst = state->pctx->buffer_map(state->pctx,
                                   lvp_buffer_from_handle(updcmd->dst_buffer)->bo,
                                   0,
                                   PIPE_MAP_WRITE,
                                   &box,
                                   &dst_t);

   memcpy(dst, updcmd->data, updcmd->data_size);
   state->pctx->buffer_unmap(state->pctx, dst_t);
}

static void handle_draw_indexed(struct vk_cmd_queue_entry *cmd,
                                struct rendering_state *state)
{
   struct pipe_draw_start_count_bias draw = {0};

   state->info.index_bounds_valid = false;
   state->info.min_index = 0;
   state->info.max_index = ~0U;
   state->info.index_size = state->index_size;
   state->info.index.resource = state->index_buffer;
   state->info.start_instance = cmd->u.draw_indexed.first_instance;
   state->info.instance_count = cmd->u.draw_indexed.instance_count;

   if (state->info.primitive_restart)
      state->info.restart_index = util_prim_restart_index_from_size(state->info.index_size);

   draw.count = MIN2(cmd->u.draw_indexed.index_count, state->index_buffer_size / state->index_size);
   draw.index_bias = cmd->u.draw_indexed.vertex_offset;
   /* TODO: avoid calculating multiple times if cmdbuf is submitted again */
   draw.start = util_clamped_uadd(state->index_offset / state->index_size,
                                  cmd->u.draw_indexed.first_index);

   state->info.index_bias_varies = !cmd->u.draw_indexed.vertex_offset;
   state->pctx->draw_vbo(state->pctx, &state->info, 0, NULL, &draw, 1);
}

static void handle_draw_multi_indexed(struct vk_cmd_queue_entry *cmd,
                                      struct rendering_state *state)
{
   struct pipe_draw_start_count_bias *draws = calloc(cmd->u.draw_multi_indexed_ext.draw_count,
                                                     sizeof(*draws));

   state->info.index_bounds_valid = false;
   state->info.min_index = 0;
   state->info.max_index = ~0U;
   state->info.index_size = state->index_size;
   state->info.index.resource = state->index_buffer;
   state->info.start_instance = cmd->u.draw_multi_indexed_ext.first_instance;
   state->info.instance_count = cmd->u.draw_multi_indexed_ext.instance_count;
   if (cmd->u.draw_multi_indexed_ext.draw_count > 1)
      state->info.increment_draw_id = true;

   if (state->info.primitive_restart)
      state->info.restart_index = util_prim_restart_index_from_size(state->info.index_size);

   unsigned size = cmd->u.draw_multi_indexed_ext.draw_count * sizeof(struct pipe_draw_start_count_bias);
   memcpy(draws, cmd->u.draw_multi_indexed_ext.index_info, size);
   if (state->index_buffer_size != UINT32_MAX) {
      for (unsigned i = 0; i < cmd->u.draw_multi_indexed_ext.draw_count; i++)
         draws[i].count = MIN2(draws[i].count, state->index_buffer_size / state->index_size - draws[i].start);
   }

   /* only the first member is read if index_bias_varies is true */
   if (cmd->u.draw_multi_indexed_ext.draw_count &&
       cmd->u.draw_multi_indexed_ext.vertex_offset)
      draws[0].index_bias = *cmd->u.draw_multi_indexed_ext.vertex_offset;

   /* TODO: avoid calculating multiple times if cmdbuf is submitted again */
   for (unsigned i = 0; i < cmd->u.draw_multi_indexed_ext.draw_count; i++)
      draws[i].start = util_clamped_uadd(state->index_offset / state->index_size,
                                         draws[i].start);

   state->info.index_bias_varies = !cmd->u.draw_multi_indexed_ext.vertex_offset;

   if (cmd->u.draw_multi_indexed_ext.draw_count)
      state->pctx->draw_vbo(state->pctx, &state->info, 0, NULL, draws, cmd->u.draw_multi_indexed_ext.draw_count);

   free(draws);
}

static void handle_draw_indirect(struct vk_cmd_queue_entry *cmd,
                                 struct rendering_state *state, bool indexed)
{
   struct pipe_draw_start_count_bias draw = {0};
   struct pipe_resource *index = NULL;
   if (indexed) {
      state->info.index_bounds_valid = false;
      state->info.index_size = state->index_size;
      state->info.index.resource = state->index_buffer;
      state->info.max_index = ~0U;
      if (state->info.primitive_restart)
         state->info.restart_index = util_prim_restart_index_from_size(state->info.index_size);
      if (state->index_offset || state->index_buffer_size != UINT32_MAX) {
         struct pipe_transfer *xfer;
         uint8_t *mem = pipe_buffer_map(state->pctx, state->index_buffer, 0, &xfer);
         state->pctx->buffer_unmap(state->pctx, xfer);
         index = get_buffer_resource(state->pctx, mem + state->index_offset);
         index->width0 = MIN2(state->index_buffer->width0 - state->index_offset, state->index_buffer_size);
         state->info.index.resource = index;
      }
   } else
      state->info.index_size = 0;
   state->indirect_info.offset = cmd->u.draw_indirect.offset;
   state->indirect_info.stride = cmd->u.draw_indirect.stride;
   state->indirect_info.draw_count = cmd->u.draw_indirect.draw_count;
   state->indirect_info.buffer = lvp_buffer_from_handle(cmd->u.draw_indirect.buffer)->bo;

   state->pctx->draw_vbo(state->pctx, &state->info, 0, &state->indirect_info, &draw, 1);
   pipe_resource_reference(&index, NULL);
}

static void handle_index_buffer(struct vk_cmd_queue_entry *cmd,
                                struct rendering_state *state)
{
   struct vk_cmd_bind_index_buffer *ib = &cmd->u.bind_index_buffer;
   state->index_size = vk_index_type_to_bytes(ib->index_type);
   state->index_buffer_size = UINT32_MAX;

   if (ib->buffer) {
      state->index_offset = ib->offset;
      state->index_buffer = lvp_buffer_from_handle(ib->buffer)->bo;
   } else {
      state->index_offset = 0;
      state->index_buffer = state->device->zero_buffer;
   }

   state->ib_dirty = true;
}

static void handle_index_buffer2(struct vk_cmd_queue_entry *cmd,
                                 struct rendering_state *state)
{
   struct vk_cmd_bind_index_buffer2 *ib = &cmd->u.bind_index_buffer2;

   if (ib->buffer) {
      state->index_size = vk_index_type_to_bytes(ib->index_type);
      state->index_buffer_size = ib->size;
      state->index_offset = ib->offset;
      state->index_buffer = lvp_buffer_from_handle(ib->buffer)->bo;
   } else {
      state->index_size = 4;
      state->index_buffer_size = UINT32_MAX;
      state->index_offset = 0;
      state->index_buffer = state->device->zero_buffer;
   }

   state->ib_dirty = true;
}

static void handle_dispatch(struct vk_cmd_queue_entry *cmd,
                            struct rendering_state *state)
{
   state->dispatch_info.grid[0] = cmd->u.dispatch.group_count_x;
   state->dispatch_info.grid[1] = cmd->u.dispatch.group_count_y;
   state->dispatch_info.grid[2] = cmd->u.dispatch.group_count_z;
   state->dispatch_info.grid_base[0] = 0;
   state->dispatch_info.grid_base[1] = 0;
   state->dispatch_info.grid_base[2] = 0;
   state->dispatch_info.indirect = NULL;
   state->pctx->launch_grid(state->pctx, &state->dispatch_info);
}

static void handle_dispatch_base(struct vk_cmd_queue_entry *cmd,
                                 struct rendering_state *state)
{
   state->dispatch_info.grid[0] = cmd->u.dispatch_base.group_count_x;
   state->dispatch_info.grid[1] = cmd->u.dispatch_base.group_count_y;
   state->dispatch_info.grid[2] = cmd->u.dispatch_base.group_count_z;
   state->dispatch_info.grid_base[0] = cmd->u.dispatch_base.base_group_x;
   state->dispatch_info.grid_base[1] = cmd->u.dispatch_base.base_group_y;
   state->dispatch_info.grid_base[2] = cmd->u.dispatch_base.base_group_z;
   state->dispatch_info.indirect = NULL;
   state->pctx->launch_grid(state->pctx, &state->dispatch_info);
}

static void handle_dispatch_indirect(struct vk_cmd_queue_entry *cmd,
                                     struct rendering_state *state)
{
   state->dispatch_info.indirect = lvp_buffer_from_handle(cmd->u.dispatch_indirect.buffer)->bo;
   state->dispatch_info.indirect_offset = cmd->u.dispatch_indirect.offset;
   state->pctx->launch_grid(state->pctx, &state->dispatch_info);
}

static void handle_push_constants(struct vk_cmd_queue_entry *cmd,
                                  struct rendering_state *state)
{
   VkPushConstantsInfoKHR *pci = cmd->u.push_constants2.push_constants_info;
   memcpy(state->push_constants + pci->offset, pci->pValues, pci->size);

   VkShaderStageFlags stage_flags = pci->stageFlags;
   state->pcbuf_dirty[MESA_SHADER_VERTEX] |= (stage_flags & VK_SHADER_STAGE_VERTEX_BIT) > 0;
   state->pcbuf_dirty[MESA_SHADER_FRAGMENT] |= (stage_flags & VK_SHADER_STAGE_FRAGMENT_BIT) > 0;
   state->pcbuf_dirty[MESA_SHADER_GEOMETRY] |= (stage_flags & VK_SHADER_STAGE_GEOMETRY_BIT) > 0;
   state->pcbuf_dirty[MESA_SHADER_TESS_CTRL] |= (stage_flags & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) > 0;
   state->pcbuf_dirty[MESA_SHADER_TESS_EVAL] |= (stage_flags & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) > 0;
   state->pcbuf_dirty[MESA_SHADER_COMPUTE] |= (stage_flags & VK_SHADER_STAGE_COMPUTE_BIT) > 0;
   state->pcbuf_dirty[MESA_SHADER_TASK] |= (stage_flags & VK_SHADER_STAGE_TASK_BIT_EXT) > 0;
   state->pcbuf_dirty[MESA_SHADER_MESH] |= (stage_flags & VK_SHADER_STAGE_MESH_BIT_EXT) > 0;
   state->pcbuf_dirty[MESA_SHADER_RAYGEN] |= (stage_flags & LVP_RAY_TRACING_STAGES) > 0;
}

static void lvp_execute_cmd_buffer(struct list_head *cmds,
                                   struct rendering_state *state, bool print_cmds);

static void handle_execute_commands(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state, bool print_cmds)
{
   for (unsigned i = 0; i < cmd->u.execute_commands.command_buffer_count; i++) {
      LVP_FROM_HANDLE(lvp_cmd_buffer, secondary_buf, cmd->u.execute_commands.command_buffers[i]);
      lvp_execute_cmd_buffer(&secondary_buf->vk.cmd_queue.cmds, state, print_cmds);
   }
}

static void handle_event_set2(struct vk_cmd_queue_entry *cmd,
                             struct rendering_state *state)
{
   LVP_FROM_HANDLE(lvp_event, event, cmd->u.set_event2.event);

   VkPipelineStageFlags2 src_stage_mask = 0;

   for (uint32_t i = 0; i < cmd->u.set_event2.dependency_info->memoryBarrierCount; i++)
      src_stage_mask |= cmd->u.set_event2.dependency_info->pMemoryBarriers[i].srcStageMask;
   for (uint32_t i = 0; i < cmd->u.set_event2.dependency_info->bufferMemoryBarrierCount; i++)
      src_stage_mask |= cmd->u.set_event2.dependency_info->pBufferMemoryBarriers[i].srcStageMask;
   for (uint32_t i = 0; i < cmd->u.set_event2.dependency_info->imageMemoryBarrierCount; i++)
      src_stage_mask |= cmd->u.set_event2.dependency_info->pImageMemoryBarriers[i].srcStageMask;

   if (src_stage_mask & VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)
      state->pctx->flush(state->pctx, NULL, 0);
   event->event_storage = 1;
}

static void handle_event_reset2(struct vk_cmd_queue_entry *cmd,
                               struct rendering_state *state)
{
   LVP_FROM_HANDLE(lvp_event, event, cmd->u.reset_event2.event);

   if (cmd->u.reset_event2.stage_mask == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
      state->pctx->flush(state->pctx, NULL, 0);
   event->event_storage = 0;
}

static void handle_wait_events2(struct vk_cmd_queue_entry *cmd,
                               struct rendering_state *state)
{
   finish_fence(state);
   for (unsigned i = 0; i < cmd->u.wait_events2.event_count; i++) {
      LVP_FROM_HANDLE(lvp_event, event, cmd->u.wait_events2.events[i]);

      while (event->event_storage != true);
   }
}

static void handle_pipeline_barrier(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   finish_fence(state);
}

static void handle_begin_query(struct vk_cmd_queue_entry *cmd,
                               struct rendering_state *state)
{
   struct vk_cmd_begin_query *qcmd = &cmd->u.begin_query;
   LVP_FROM_HANDLE(lvp_query_pool, pool, qcmd->query_pool);

   if (pool->type == VK_QUERY_TYPE_PIPELINE_STATISTICS &&
       pool->pipeline_stats & VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT)
      emit_compute_state(state);

   emit_state(state);

   uint32_t count = util_bitcount(state->framebuffer.viewmask ? state->framebuffer.viewmask : BITFIELD_BIT(0));
   for (unsigned idx = 0; idx < count; idx++) {
      if (!pool->queries[qcmd->query + idx]) {
         enum pipe_query_type qtype = pool->base_type;
         pool->queries[qcmd->query + idx] = state->pctx->create_query(state->pctx,
                                                               qtype, 0);
      }

      state->pctx->begin_query(state->pctx, pool->queries[qcmd->query + idx]);
      if (idx)
         state->pctx->end_query(state->pctx, pool->queries[qcmd->query + idx]);
   }
}

static void handle_end_query(struct vk_cmd_queue_entry *cmd,
                             struct rendering_state *state)
{
   struct vk_cmd_end_query *qcmd = &cmd->u.end_query;
   LVP_FROM_HANDLE(lvp_query_pool, pool, qcmd->query_pool);
   assert(pool->queries[qcmd->query]);

   state->pctx->end_query(state->pctx, pool->queries[qcmd->query]);
}


static void handle_begin_query_indexed_ext(struct vk_cmd_queue_entry *cmd,
                                           struct rendering_state *state)
{
   struct vk_cmd_begin_query_indexed_ext *qcmd = &cmd->u.begin_query_indexed_ext;
   LVP_FROM_HANDLE(lvp_query_pool, pool, qcmd->query_pool);

   if (pool->type == VK_QUERY_TYPE_PIPELINE_STATISTICS &&
       pool->pipeline_stats & VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT)
      emit_compute_state(state);

   emit_state(state);

   uint32_t count = util_bitcount(state->framebuffer.viewmask ? state->framebuffer.viewmask : BITFIELD_BIT(0));
   for (unsigned idx = 0; idx < count; idx++) {
      if (!pool->queries[qcmd->query + idx]) {
         enum pipe_query_type qtype = pool->base_type;
         pool->queries[qcmd->query + idx] = state->pctx->create_query(state->pctx,
                                                                      qtype, qcmd->index);
      }

      state->pctx->begin_query(state->pctx, pool->queries[qcmd->query + idx]);
      if (idx)
         state->pctx->end_query(state->pctx, pool->queries[qcmd->query + idx]);
   }
}

static void handle_end_query_indexed_ext(struct vk_cmd_queue_entry *cmd,
                                         struct rendering_state *state)
{
   struct vk_cmd_end_query_indexed_ext *qcmd = &cmd->u.end_query_indexed_ext;
   LVP_FROM_HANDLE(lvp_query_pool, pool, qcmd->query_pool);
   assert(pool->queries[qcmd->query]);

   state->pctx->end_query(state->pctx, pool->queries[qcmd->query]);
}

static void handle_reset_query_pool(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   struct vk_cmd_reset_query_pool *qcmd = &cmd->u.reset_query_pool;
   LVP_FROM_HANDLE(lvp_query_pool, pool, qcmd->query_pool);

   if (pool->base_type >= PIPE_QUERY_TYPES)
      return;

   for (unsigned i = qcmd->first_query; i < qcmd->first_query + qcmd->query_count; i++) {
      if (pool->queries[i]) {
         state->pctx->destroy_query(state->pctx, pool->queries[i]);
         pool->queries[i] = NULL;
      }
   }
}

static void handle_write_timestamp2(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   struct vk_cmd_write_timestamp2 *qcmd = &cmd->u.write_timestamp2;
   LVP_FROM_HANDLE(lvp_query_pool, pool, qcmd->query_pool);

   if (!(qcmd->stage == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT))
      state->pctx->flush(state->pctx, NULL, 0);

   uint32_t count = util_bitcount(state->framebuffer.viewmask ? state->framebuffer.viewmask : BITFIELD_BIT(0));
   for (unsigned idx = 0; idx < count; idx++) {
      if (!pool->queries[qcmd->query + idx]) {
         pool->queries[qcmd->query + idx] = state->pctx->create_query(state->pctx, PIPE_QUERY_TIMESTAMP, 0);
      }

      state->pctx->end_query(state->pctx, pool->queries[qcmd->query + idx]);
   }
}

static void handle_copy_query_pool_results(struct vk_cmd_queue_entry *cmd,
                                           struct rendering_state *state)
{
   struct vk_cmd_copy_query_pool_results *copycmd = &cmd->u.copy_query_pool_results;
   LVP_FROM_HANDLE(lvp_query_pool, pool, copycmd->query_pool);
   enum pipe_query_flags flags = (copycmd->flags & VK_QUERY_RESULT_WAIT_BIT) ? PIPE_QUERY_WAIT : 0;

   if (copycmd->flags & VK_QUERY_RESULT_PARTIAL_BIT)
      flags |= PIPE_QUERY_PARTIAL;
   unsigned result_size = copycmd->flags & VK_QUERY_RESULT_64_BIT ? 8 : 4;
   for (unsigned i = copycmd->first_query; i < copycmd->first_query + copycmd->query_count; i++) {
      unsigned offset = copycmd->dst_offset + (copycmd->stride * (i - copycmd->first_query));

      if (pool->base_type >= PIPE_QUERY_TYPES) {
         struct pipe_transfer *transfer;
         uint8_t *map = pipe_buffer_map(state->pctx, lvp_buffer_from_handle(copycmd->dst_buffer)->bo, PIPE_MAP_WRITE, &transfer);
         map += offset;

         if (copycmd->flags & VK_QUERY_RESULT_64_BIT) {
            uint64_t *dst = (uint64_t *)map;
            uint64_t *src = (uint64_t *)pool->data;
            *dst = src[i];
            if (copycmd->flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
               *(dst + 1) = 1;
         } else {
            uint32_t *dst = (uint32_t *)map;
            uint64_t *src = (uint64_t *)pool->data;
            *dst = (uint32_t) (src[i] & UINT32_MAX);
            if (copycmd->flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
               *(dst + 1) = 1;
         }

         state->pctx->buffer_unmap(state->pctx, transfer);

         continue;
      }

      if (pool->queries[i]) {
         unsigned num_results = 0;
         if (copycmd->flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
            if (pool->type == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
               num_results = util_bitcount(pool->pipeline_stats);
            } else
               num_results = pool-> type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT ? 2 : 1;
            state->pctx->get_query_result_resource(state->pctx,
                                                   pool->queries[i],
                                                   flags,
                                                   copycmd->flags & VK_QUERY_RESULT_64_BIT ? PIPE_QUERY_TYPE_U64 : PIPE_QUERY_TYPE_U32,
                                                   -1,
                                                   lvp_buffer_from_handle(copycmd->dst_buffer)->bo,
                                                   offset + num_results * result_size);
         }
         if (pool->type == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
            num_results = 0;
            u_foreach_bit(bit, pool->pipeline_stats)
               state->pctx->get_query_result_resource(state->pctx,
                                                      pool->queries[i],
                                                      flags,
                                                      copycmd->flags & VK_QUERY_RESULT_64_BIT ? PIPE_QUERY_TYPE_U64 : PIPE_QUERY_TYPE_U32,
                                                      bit,
                                                      lvp_buffer_from_handle(copycmd->dst_buffer)->bo,
                                                      offset + num_results++ * result_size);
         } else {
            state->pctx->get_query_result_resource(state->pctx,
                                                   pool->queries[i],
                                                   flags,
                                                   copycmd->flags & VK_QUERY_RESULT_64_BIT ? PIPE_QUERY_TYPE_U64 : PIPE_QUERY_TYPE_U32,
                                                   0,
                                                   lvp_buffer_from_handle(copycmd->dst_buffer)->bo,
                                                   offset);
         }
      } else {
         /* if no queries emitted yet, just reset the buffer to 0 so avail is reported correctly */
         if (copycmd->flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
            struct pipe_transfer *src_t;
            uint32_t *map;

            struct pipe_box box = {0};
            box.x = offset;
            box.width = copycmd->stride;
            box.height = 1;
            box.depth = 1;
            map = state->pctx->buffer_map(state->pctx,
                                            lvp_buffer_from_handle(copycmd->dst_buffer)->bo, 0, PIPE_MAP_READ, &box,
                                            &src_t);

            memset(map, 0, box.width);
            state->pctx->buffer_unmap(state->pctx, src_t);
         }
      }
   }
}

static void handle_clear_color_image(struct vk_cmd_queue_entry *cmd,
                                     struct rendering_state *state)
{
   LVP_FROM_HANDLE(lvp_image, image, cmd->u.clear_color_image.image);

   enum pipe_format format = image->planes[0].bo->format;
   const struct util_format_description *desc = util_format_description(format);
   if (util_format_is_int64(desc))
      format = util_format_get_array(desc->channel[0].type, 32, desc->nr_channels * 2, false, true);

   union util_color uc;
   uint32_t *col_val = uc.ui;
   util_pack_color_union(format, &uc, (void*)cmd->u.clear_color_image.color);
   for (unsigned i = 0; i < cmd->u.clear_color_image.range_count; i++) {
      VkImageSubresourceRange *range = &cmd->u.clear_color_image.ranges[i];
      struct pipe_box box;
      box.x = 0;
      box.y = 0;
      box.z = 0;

      uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);
      for (unsigned j = range->baseMipLevel; j < range->baseMipLevel + level_count; j++) {
         box.width = u_minify(image->planes[0].bo->width0, j);
         box.height = u_minify(image->planes[0].bo->height0, j);
         box.depth = 1;
         if (image->planes[0].bo->target == PIPE_TEXTURE_3D) {
            box.depth = u_minify(image->planes[0].bo->depth0, j);
         } else if (image->planes[0].bo->target == PIPE_TEXTURE_1D_ARRAY) {
            box.y = range->baseArrayLayer;
            box.height = vk_image_subresource_layer_count(&image->vk, range);
            box.depth = 1;
         } else {
            box.z = range->baseArrayLayer;
            box.depth = vk_image_subresource_layer_count(&image->vk, range);
         }

         state->pctx->clear_texture(state->pctx, image->planes[0].bo,
                                    j, &box, (void *)col_val);
      }
   }
}

static void handle_clear_ds_image(struct vk_cmd_queue_entry *cmd,
                                  struct rendering_state *state)
{
   LVP_FROM_HANDLE(lvp_image, image, cmd->u.clear_depth_stencil_image.image);
   for (unsigned i = 0; i < cmd->u.clear_depth_stencil_image.range_count; i++) {
      VkImageSubresourceRange *range = &cmd->u.clear_depth_stencil_image.ranges[i];
      uint32_t ds_clear_flags = 0;
      if (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
         ds_clear_flags |= PIPE_CLEAR_DEPTH;
      if (range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
         ds_clear_flags |= PIPE_CLEAR_STENCIL;

      uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);
      for (unsigned j = 0; j < level_count; j++) {
         struct pipe_surface surf;
         unsigned width, height, depth;
         width = u_minify(image->planes[0].bo->width0, range->baseMipLevel + j);
         height = u_minify(image->planes[0].bo->height0, range->baseMipLevel + j);

         if (image->planes[0].bo->target == PIPE_TEXTURE_3D) {
            depth = u_minify(image->planes[0].bo->depth0, range->baseMipLevel + j);
         } else {
            depth = vk_image_subresource_layer_count(&image->vk, range);
         }

         surf = create_img_surface_bo(state, range,
                                      image->planes[0].bo, image->planes[0].bo->format,
                                      0, depth, j);

         state->pctx->clear_depth_stencil(state->pctx,
                                          &surf,
                                          ds_clear_flags,
                                          cmd->u.clear_depth_stencil_image.depth_stencil->depth,
                                          cmd->u.clear_depth_stencil_image.depth_stencil->stencil,
                                          0, 0,
                                          width, height, false);
      }
   }
}

static void handle_clear_attachments(struct vk_cmd_queue_entry *cmd,
                                     struct rendering_state *state)
{
   for (uint32_t a = 0; a < cmd->u.clear_attachments.attachment_count; a++) {
      VkClearAttachment *att = &cmd->u.clear_attachments.attachments[a];
      struct lvp_image_view *imgv;

      if (att->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
         imgv = state->color_att[att->colorAttachment].imgv;
      } else {
         imgv = state->ds_imgv;
      }
      if (!imgv)
         continue;

      union pipe_color_union col_val;
      double dclear_val = 0;
      uint32_t sclear_val = 0;
      uint32_t ds_clear_flags = 0;
      if (att->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         ds_clear_flags |= PIPE_CLEAR_DEPTH;
         dclear_val = att->clearValue.depthStencil.depth;
      }
      if (att->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         ds_clear_flags |= PIPE_CLEAR_STENCIL;
         sclear_val = att->clearValue.depthStencil.stencil;
      }
      if (att->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         for (unsigned i = 0; i < 4; i++)
            col_val.ui[i] = att->clearValue.color.uint32[i];
      }

      for (uint32_t r = 0; r < cmd->u.clear_attachments.rect_count; r++) {

         VkClearRect *rect = &cmd->u.clear_attachments.rects[r];
         /* avoid crashing on spec violations */
         rect->rect.offset.x = MAX2(rect->rect.offset.x, 0);
         rect->rect.offset.y = MAX2(rect->rect.offset.y, 0);
         rect->rect.extent.width = MIN2(rect->rect.extent.width, state->framebuffer.width - rect->rect.offset.x);
         rect->rect.extent.height = MIN2(rect->rect.extent.height, state->framebuffer.height - rect->rect.offset.y);
         if (state->framebuffer.viewmask) {
            u_foreach_bit(i, state->framebuffer.viewmask)
               clear_attachment_layers(state, imgv, &rect->rect,
                                       i, 1,
                                       ds_clear_flags, dclear_val, sclear_val,
                                       &col_val);
         } else
            clear_attachment_layers(state, imgv, &rect->rect,
                                    rect->baseArrayLayer, rect->layerCount,
                                    ds_clear_flags, dclear_val, sclear_val,
                                    &col_val);
      }
   }
}

static void handle_resolve_image(struct vk_cmd_queue_entry *cmd,
                                 struct rendering_state *state)
{
   VkResolveImageInfo2 *resolvecmd = cmd->u.resolve_image2.resolve_image_info;
   LVP_FROM_HANDLE(lvp_image, src_image, resolvecmd->srcImage);
   LVP_FROM_HANDLE(lvp_image, dst_image, resolvecmd->dstImage);

   struct pipe_blit_info info = {0};
   info.src.resource = src_image->planes[0].bo;
   info.dst.resource = dst_image->planes[0].bo;
   info.src.format = src_image->planes[0].bo->format;
   info.dst.format = dst_image->planes[0].bo->format;
   info.mask = util_format_is_depth_or_stencil(info.src.format) ? PIPE_MASK_ZS : PIPE_MASK_RGBA;
   info.filter = PIPE_TEX_FILTER_NEAREST;

   for (uint32_t i = 0; i < resolvecmd->regionCount; i++) {
      int srcX0, srcY0;
      unsigned dstX0, dstY0;

      srcX0 = resolvecmd->pRegions[i].srcOffset.x;
      srcY0 = resolvecmd->pRegions[i].srcOffset.y;

      dstX0 = resolvecmd->pRegions[i].dstOffset.x;
      dstY0 = resolvecmd->pRegions[i].dstOffset.y;

      info.dst.box.x = dstX0;
      info.dst.box.y = dstY0;
      info.src.box.x = srcX0;
      info.src.box.y = srcY0;

      info.dst.box.width = resolvecmd->pRegions[i].extent.width;
      info.src.box.width = resolvecmd->pRegions[i].extent.width;
      info.dst.box.height = resolvecmd->pRegions[i].extent.height;
      info.src.box.height = resolvecmd->pRegions[i].extent.height;

      info.dst.box.depth = subresource_layercount(dst_image, &resolvecmd->pRegions[i].dstSubresource);
      info.src.box.depth = subresource_layercount(src_image, &resolvecmd->pRegions[i].srcSubresource);

      info.src.level = resolvecmd->pRegions[i].srcSubresource.mipLevel;
      info.src.box.z = resolvecmd->pRegions[i].srcOffset.z + resolvecmd->pRegions[i].srcSubresource.baseArrayLayer;

      info.dst.level = resolvecmd->pRegions[i].dstSubresource.mipLevel;
      info.dst.box.z = resolvecmd->pRegions[i].dstOffset.z + resolvecmd->pRegions[i].dstSubresource.baseArrayLayer;

      state->pctx->blit(state->pctx, &info);
   }
}

static void handle_draw_indirect_count(struct vk_cmd_queue_entry *cmd,
                                       struct rendering_state *state, bool indexed)
{
   struct pipe_draw_start_count_bias draw = {0};
   struct pipe_resource *index = NULL;
   if (indexed) {
      state->info.index_bounds_valid = false;
      state->info.index_size = state->index_size;
      state->info.index.resource = state->index_buffer;
      state->info.max_index = ~0U;
      if (state->index_offset || state->index_buffer_size != UINT32_MAX) {
         struct pipe_transfer *xfer;
         uint8_t *mem = pipe_buffer_map(state->pctx, state->index_buffer, 0, &xfer);
         state->pctx->buffer_unmap(state->pctx, xfer);
         index = get_buffer_resource(state->pctx, mem + state->index_offset);
         index->width0 = MIN2(state->index_buffer->width0 - state->index_offset, state->index_buffer_size);
         state->info.index.resource = index;
      }
   } else
      state->info.index_size = 0;
   state->indirect_info.offset = cmd->u.draw_indirect_count.offset;
   state->indirect_info.stride = cmd->u.draw_indirect_count.stride;
   state->indirect_info.draw_count = cmd->u.draw_indirect_count.max_draw_count;
   state->indirect_info.buffer = lvp_buffer_from_handle(cmd->u.draw_indirect_count.buffer)->bo;
   state->indirect_info.indirect_draw_count_offset = cmd->u.draw_indirect_count.count_buffer_offset;
   state->indirect_info.indirect_draw_count = lvp_buffer_from_handle(cmd->u.draw_indirect_count.count_buffer)->bo;

   state->pctx->draw_vbo(state->pctx, &state->info, 0, &state->indirect_info, &draw, 1);
   pipe_resource_reference(&index, NULL);
}

static void handle_push_descriptor_set(struct vk_cmd_queue_entry *cmd,
                                       struct rendering_state *state)
{
   VkPushDescriptorSetInfoKHR *pds = cmd->u.push_descriptor_set2.push_descriptor_set_info;
   LVP_FROM_HANDLE(lvp_pipeline_layout, layout, pds->layout);
   struct lvp_descriptor_set_layout *set_layout = (struct lvp_descriptor_set_layout *)layout->vk.set_layouts[pds->set];

   struct lvp_descriptor_set *set;
   lvp_descriptor_set_create(state->device, set_layout, &set);

   util_dynarray_append(&state->push_desc_sets, struct lvp_descriptor_set *, set);

   uint32_t types = lvp_pipeline_types_from_shader_stages(pds->stageFlags);
   u_foreach_bit(pipeline_type, types) {
      struct lvp_descriptor_set *base = state->desc_sets[pipeline_type][pds->set];
      if (base)
         memcpy(set->map, base->map, MIN2(set->bo->width0, base->bo->width0));

      VkDescriptorSet set_handle = lvp_descriptor_set_to_handle(set);

      VkWriteDescriptorSet *writes = (void*)pds->pDescriptorWrites;
      for (uint32_t i = 0; i < pds->descriptorWriteCount; i++)
         writes[i].dstSet = set_handle;

      lvp_UpdateDescriptorSets(lvp_device_to_handle(state->device), pds->descriptorWriteCount, pds->pDescriptorWrites, 0, NULL);

      VkBindDescriptorSetsInfoKHR bind_info = {
         .stageFlags = pds->stageFlags,
         .layout = pds->layout,
         .firstSet = pds->set,
         .descriptorSetCount = 1,
         .pDescriptorSets = &set_handle,
      };
      handle_descriptor_sets(&bind_info, state);
   }
}

static void handle_push_descriptor_set_with_template(struct vk_cmd_queue_entry *cmd,
                                                     struct rendering_state *state)
{
   VkPushDescriptorSetWithTemplateInfoKHR *pds = cmd->u.push_descriptor_set_with_template2.push_descriptor_set_with_template_info;
   LVP_FROM_HANDLE(vk_descriptor_update_template, templ, pds->descriptorUpdateTemplate);
   LVP_FROM_HANDLE(lvp_pipeline_layout, layout, pds->layout);
   struct lvp_descriptor_set_layout *set_layout = (struct lvp_descriptor_set_layout *)layout->vk.set_layouts[pds->set];

   struct lvp_descriptor_set *set;
   lvp_descriptor_set_create(state->device, set_layout, &set);

   util_dynarray_append(&state->push_desc_sets, struct lvp_descriptor_set *, set);

   struct lvp_descriptor_set *base = state->desc_sets[lvp_pipeline_type_from_bind_point(templ->bind_point)][pds->set];
   if (base)
      memcpy(set->map, base->map, MIN2(set->bo->width0, base->bo->width0));

   VkDescriptorSet set_handle = lvp_descriptor_set_to_handle(set);
   lvp_descriptor_set_update_with_template(lvp_device_to_handle(state->device), set_handle,
                                           pds->descriptorUpdateTemplate, pds->pData);

   VkBindDescriptorSetsInfoKHR bind_cmd = {
      .stageFlags = vk_shader_stages_from_bind_point(templ->bind_point),
      .layout = pds->layout,
      .firstSet = pds->set,
      .descriptorSetCount = 1,
      .pDescriptorSets = &set_handle,
   };
   handle_descriptor_sets(&bind_cmd, state);
}

static void handle_bind_transform_feedback_buffers(struct vk_cmd_queue_entry *cmd,
                                                   struct rendering_state *state)
{
   struct vk_cmd_bind_transform_feedback_buffers_ext *btfb = &cmd->u.bind_transform_feedback_buffers_ext;

   for (unsigned i = 0; i < btfb->binding_count; i++) {
      int idx = i + btfb->first_binding;
      uint32_t size;
      struct lvp_buffer *buf = lvp_buffer_from_handle(btfb->buffers[i]);

      size = vk_buffer_range(&buf->vk, btfb->offsets[i], btfb->sizes ? btfb->sizes[i] : VK_WHOLE_SIZE);

      if (state->so_targets[idx])
         state->pctx->stream_output_target_destroy(state->pctx, state->so_targets[idx]);

      state->so_targets[idx] = state->pctx->create_stream_output_target(state->pctx,
                                                                        lvp_buffer_from_handle(btfb->buffers[i])->bo,
                                                                        btfb->offsets[i],
                                                                        size);
   }
   state->num_so_targets = btfb->first_binding + btfb->binding_count;
}

static void handle_begin_transform_feedback(struct vk_cmd_queue_entry *cmd,
                                            struct rendering_state *state)
{
   struct vk_cmd_begin_transform_feedback_ext *btf = &cmd->u.begin_transform_feedback_ext;
   uint32_t offsets[4] = {0};

   for (unsigned i = 0; btf->counter_buffers && i < btf->counter_buffer_count; i++) {
      if (!btf->counter_buffers[i])
         continue;

      pipe_buffer_read(state->pctx,
                       btf->counter_buffers ? lvp_buffer_from_handle(btf->counter_buffers[i])->bo : NULL,
                       btf->counter_buffer_offsets ? btf->counter_buffer_offsets[i] : 0,
                       4,
                       &offsets[i]);
   }
   state->pctx->set_stream_output_targets(state->pctx, state->num_so_targets,
                                          state->so_targets, offsets, MESA_PRIM_UNKNOWN);
}

static void handle_end_transform_feedback(struct vk_cmd_queue_entry *cmd,
                                          struct rendering_state *state)
{
   struct vk_cmd_end_transform_feedback_ext *etf = &cmd->u.end_transform_feedback_ext;

   if (etf->counter_buffer_count) {
      for (unsigned i = 0; etf->counter_buffers && i < etf->counter_buffer_count; i++) {
         if (!etf->counter_buffers[i])
            continue;

         uint32_t offset;
         offset = state->pctx->stream_output_target_offset(state->so_targets[i]);

         pipe_buffer_write(state->pctx,
                           etf->counter_buffers ? lvp_buffer_from_handle(etf->counter_buffers[i])->bo : NULL,
                           etf->counter_buffer_offsets ? etf->counter_buffer_offsets[i] : 0,
                           4,
                           &offset);
      }
   }
   state->pctx->set_stream_output_targets(state->pctx, 0, NULL, NULL, 0);
}

static void handle_draw_indirect_byte_count(struct vk_cmd_queue_entry *cmd,
                                            struct rendering_state *state)
{
   struct vk_cmd_draw_indirect_byte_count_ext *dibc = &cmd->u.draw_indirect_byte_count_ext;
   struct pipe_draw_start_count_bias draw = {0};

   pipe_buffer_read(state->pctx,
                    lvp_buffer_from_handle(dibc->counter_buffer)->bo,
                    dibc->counter_buffer_offset,
                    4, &draw.count);

   draw.count -= dibc->counter_offset;
   state->info.start_instance = cmd->u.draw_indirect_byte_count_ext.first_instance;
   state->info.instance_count = cmd->u.draw_indirect_byte_count_ext.instance_count;
   state->info.index_size = 0;

   draw.count /= cmd->u.draw_indirect_byte_count_ext.vertex_stride;
   state->pctx->draw_vbo(state->pctx, &state->info, 0, NULL, &draw, 1);
}

static void
lvp_emit_conditional_rendering(struct rendering_state *state)
{
   if (state->conditional_rendering.enabled) {
      state->pctx->render_condition_mem(
         state->pctx,
         state->conditional_rendering.buffer,
         state->conditional_rendering.offset,
         state->conditional_rendering.condition);
   } else {
      state->pctx->render_condition_mem(state->pctx, NULL, 0, false);
   }
}

static void handle_begin_conditional_rendering(struct vk_cmd_queue_entry *cmd,
                                               struct rendering_state *state)
{
   struct VkConditionalRenderingBeginInfoEXT *bcr = cmd->u.begin_conditional_rendering_ext.conditional_rendering_begin;
   state->conditional_rendering.buffer = lvp_buffer_from_handle(bcr->buffer)->bo;
   state->conditional_rendering.offset = bcr->offset;
   state->conditional_rendering.condition = bcr->flags & VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT;
   state->conditional_rendering.enabled = true;
   lvp_emit_conditional_rendering(state);
}

static void handle_end_conditional_rendering(struct rendering_state *state)
{
   state->conditional_rendering.enabled = false;
   lvp_emit_conditional_rendering(state);
}

static void handle_set_vertex_input(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   const struct vk_cmd_set_vertex_input_ext *vertex_input = &cmd->u.set_vertex_input_ext;
   const struct VkVertexInputBindingDescription2EXT *bindings = vertex_input->vertex_binding_descriptions;
   const struct VkVertexInputAttributeDescription2EXT *attrs = vertex_input->vertex_attribute_descriptions;
   int max_location = -1;
   for (unsigned i = 0; i < vertex_input->vertex_attribute_description_count; i++) {
      const struct VkVertexInputBindingDescription2EXT *binding = NULL;
      unsigned location = attrs[i].location;

      for (unsigned j = 0; j < vertex_input->vertex_binding_description_count; j++) {
         const struct VkVertexInputBindingDescription2EXT *b = &bindings[j];
         if (b->binding == attrs[i].binding) {
            binding = b;
            break;
         }
      }
      assert(binding);
      state->velem.velems[location].src_offset = attrs[i].offset;
      state->vertex_buffer_index[location] = attrs[i].binding;
      state->velem.velems[location].src_format = lvp_vk_format_to_pipe_format(attrs[i].format);
      state->velem.velems[location].src_stride = binding->stride;
      uint32_t d = binding->divisor;
      switch (binding->inputRate) {
      case VK_VERTEX_INPUT_RATE_VERTEX:
         state->velem.velems[location].instance_divisor = 0;
         break;
      case VK_VERTEX_INPUT_RATE_INSTANCE:
         state->velem.velems[location].instance_divisor = d ? d : UINT32_MAX;
         break;
      default:
         assert(0);
         break;
      }

      if ((int)location > max_location)
         max_location = location;
   }
   state->velem.count = max_location + 1;
   state->vb_strides_dirty = false;
   state->vb_dirty = true;
   state->ve_dirty = true;
}

static void handle_set_cull_mode(struct vk_cmd_queue_entry *cmd,
                                 struct rendering_state *state)
{
   state->rs_state.cull_face = vk_cull_to_pipe(cmd->u.set_cull_mode.cull_mode);
   state->rs_dirty = true;
}

static void handle_set_front_face(struct vk_cmd_queue_entry *cmd,
                                  struct rendering_state *state)
{
   state->rs_state.front_ccw = (cmd->u.set_front_face.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);
   state->rs_dirty = true;
}

static void handle_set_primitive_topology(struct vk_cmd_queue_entry *cmd,
                                          struct rendering_state *state)
{
   state->info.mode = vk_conv_topology(cmd->u.set_primitive_topology.primitive_topology);
   state->rs_dirty = true;
}

static void handle_set_depth_test_enable(struct vk_cmd_queue_entry *cmd,
                                         struct rendering_state *state)
{
   state->dsa_dirty |= state->dsa_state.depth_enabled != cmd->u.set_depth_test_enable.depth_test_enable;
   state->dsa_state.depth_enabled = cmd->u.set_depth_test_enable.depth_test_enable;
}

static void handle_set_depth_write_enable(struct vk_cmd_queue_entry *cmd,
                                          struct rendering_state *state)
{
   state->dsa_dirty |= state->dsa_state.depth_writemask != cmd->u.set_depth_write_enable.depth_write_enable;
   state->dsa_state.depth_writemask = cmd->u.set_depth_write_enable.depth_write_enable;
}

static void handle_set_depth_compare_op(struct vk_cmd_queue_entry *cmd,
                                        struct rendering_state *state)
{
   state->dsa_dirty |= state->dsa_state.depth_func != cmd->u.set_depth_compare_op.depth_compare_op;
   state->dsa_state.depth_func = cmd->u.set_depth_compare_op.depth_compare_op;
}

static void handle_set_depth_bounds_test_enable(struct vk_cmd_queue_entry *cmd,
                                                struct rendering_state *state)
{
   state->dsa_dirty |= state->dsa_state.depth_bounds_test != cmd->u.set_depth_bounds_test_enable.depth_bounds_test_enable;
   state->dsa_state.depth_bounds_test = cmd->u.set_depth_bounds_test_enable.depth_bounds_test_enable;
}

static void handle_set_stencil_test_enable(struct vk_cmd_queue_entry *cmd,
                                           struct rendering_state *state)
{
   state->dsa_dirty |= state->dsa_state.stencil[0].enabled != cmd->u.set_stencil_test_enable.stencil_test_enable ||
                       state->dsa_state.stencil[1].enabled != cmd->u.set_stencil_test_enable.stencil_test_enable;
   state->dsa_state.stencil[0].enabled = cmd->u.set_stencil_test_enable.stencil_test_enable;
   state->dsa_state.stencil[1].enabled = cmd->u.set_stencil_test_enable.stencil_test_enable;
}

static void handle_set_stencil_op(struct vk_cmd_queue_entry *cmd,
                                  struct rendering_state *state)
{
   if (cmd->u.set_stencil_op.face_mask & VK_STENCIL_FACE_FRONT_BIT) {
      state->dsa_state.stencil[0].func = cmd->u.set_stencil_op.compare_op;
      state->dsa_state.stencil[0].fail_op = vk_conv_stencil_op(cmd->u.set_stencil_op.fail_op);
      state->dsa_state.stencil[0].zpass_op = vk_conv_stencil_op(cmd->u.set_stencil_op.pass_op);
      state->dsa_state.stencil[0].zfail_op = vk_conv_stencil_op(cmd->u.set_stencil_op.depth_fail_op);
   }

   if (cmd->u.set_stencil_op.face_mask & VK_STENCIL_FACE_BACK_BIT) {
      state->dsa_state.stencil[1].func = cmd->u.set_stencil_op.compare_op;
      state->dsa_state.stencil[1].fail_op = vk_conv_stencil_op(cmd->u.set_stencil_op.fail_op);
      state->dsa_state.stencil[1].zpass_op = vk_conv_stencil_op(cmd->u.set_stencil_op.pass_op);
      state->dsa_state.stencil[1].zfail_op = vk_conv_stencil_op(cmd->u.set_stencil_op.depth_fail_op);
   }
   state->dsa_dirty = true;
}

static void handle_set_line_stipple(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   state->rs_state.line_stipple_factor = cmd->u.set_line_stipple.line_stipple_factor - 1;
   state->rs_state.line_stipple_pattern = cmd->u.set_line_stipple.line_stipple_pattern;
   state->rs_dirty = true;
}

static void handle_set_depth_bias_enable(struct vk_cmd_queue_entry *cmd,
                                         struct rendering_state *state)
{
   state->rs_dirty |= state->depth_bias.enabled != cmd->u.set_depth_bias_enable.depth_bias_enable;
   state->depth_bias.enabled = cmd->u.set_depth_bias_enable.depth_bias_enable;
}

static void handle_set_logic_op(struct vk_cmd_queue_entry *cmd,
                                struct rendering_state *state)
{
   unsigned op = vk_logic_op_to_pipe(cmd->u.set_logic_op_ext.logic_op);
   state->rs_dirty |= state->blend_state.logicop_func != op;
   state->blend_state.logicop_func = op;
}

static void handle_set_patch_control_points(struct vk_cmd_queue_entry *cmd,
                                            struct rendering_state *state)
{
   if (state->patch_vertices != cmd->u.set_patch_control_points_ext.patch_control_points)
      state->pctx->set_patch_vertices(state->pctx, cmd->u.set_patch_control_points_ext.patch_control_points);
   state->patch_vertices = cmd->u.set_patch_control_points_ext.patch_control_points;
}

static void handle_set_primitive_restart_enable(struct vk_cmd_queue_entry *cmd,
                                                struct rendering_state *state)
{
   state->info.primitive_restart = cmd->u.set_primitive_restart_enable.primitive_restart_enable;
}

static void handle_set_rasterizer_discard_enable(struct vk_cmd_queue_entry *cmd,
                                                 struct rendering_state *state)
{
   state->rs_dirty |= state->rs_state.rasterizer_discard != cmd->u.set_rasterizer_discard_enable.rasterizer_discard_enable;
   state->rs_state.rasterizer_discard = cmd->u.set_rasterizer_discard_enable.rasterizer_discard_enable;
}

static void handle_set_color_write_enable(struct vk_cmd_queue_entry *cmd,
                                          struct rendering_state *state)
{
   uint8_t disable_mask = 0; //PIPE_MAX_COLOR_BUFS is max attachment count

   for (unsigned i = 0; i < cmd->u.set_color_write_enable_ext.attachment_count; i++) {
      /* this is inverted because cmdbufs are zero-initialized, meaning only 'true'
       * can be detected with a bool, and the default is to enable color writes
       */
      if (cmd->u.set_color_write_enable_ext.color_write_enables[i] != VK_TRUE)
         disable_mask |= BITFIELD_BIT(i);
   }

   state->blend_dirty |= state->color_write_disables != disable_mask;
   state->color_write_disables = disable_mask;
}

static void handle_set_polygon_mode(struct vk_cmd_queue_entry *cmd,
                                    struct rendering_state *state)
{
   unsigned polygon_mode = vk_polygon_mode_to_pipe(cmd->u.set_polygon_mode_ext.polygon_mode);
   if (state->rs_state.fill_front != polygon_mode)
      state->rs_dirty = true;
   state->rs_state.fill_front = polygon_mode;
   if (state->rs_state.fill_back != polygon_mode)
      state->rs_dirty = true;
   state->rs_state.fill_back = polygon_mode;
}

static void handle_set_tessellation_domain_origin(struct vk_cmd_queue_entry *cmd,
                                                  struct rendering_state *state)
{
   bool tess_ccw = cmd->u.set_tessellation_domain_origin_ext.domain_origin == VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT;
   if (tess_ccw == state->tess_ccw)
      return;
   state->tess_ccw = tess_ccw;
   if (state->tess_states[state->tess_ccw])
      state->pctx->bind_tes_state(state->pctx, state->tess_states[state->tess_ccw]);
}

static void handle_set_depth_clamp_enable(struct vk_cmd_queue_entry *cmd,
                                          struct rendering_state *state)
{
   state->rs_dirty |= state->rs_state.depth_clamp != cmd->u.set_depth_clamp_enable_ext.depth_clamp_enable;
   state->rs_state.depth_clamp = !!cmd->u.set_depth_clamp_enable_ext.depth_clamp_enable;
   if (state->depth_clamp_sets_clip)
      state->rs_state.depth_clip_near = state->rs_state.depth_clip_far = !state->rs_state.depth_clamp;
}

static void handle_set_depth_clip_enable(struct vk_cmd_queue_entry *cmd,
                                         struct rendering_state *state)
{
   state->rs_dirty |= state->rs_state.depth_clip_far != !!cmd->u.set_depth_clip_enable_ext.depth_clip_enable;
   state->rs_state.depth_clip_near = state->rs_state.depth_clip_far = !!cmd->u.set_depth_clip_enable_ext.depth_clip_enable;
}

static void handle_set_logic_op_enable(struct vk_cmd_queue_entry *cmd,
                                         struct rendering_state *state)
{
   state->blend_dirty |= state->blend_state.logicop_enable != !!cmd->u.set_logic_op_enable_ext.logic_op_enable;
   state->blend_state.logicop_enable = !!cmd->u.set_logic_op_enable_ext.logic_op_enable;
}

static void handle_set_sample_mask(struct vk_cmd_queue_entry *cmd,
                                   struct rendering_state *state)
{
   unsigned mask = cmd->u.set_sample_mask_ext.sample_mask ? cmd->u.set_sample_mask_ext.sample_mask[0] : 0xffffffff;
   state->sample_mask_dirty |= state->sample_mask != mask;
   state->sample_mask = mask;
}

static void handle_set_samples(struct vk_cmd_queue_entry *cmd,
                               struct rendering_state *state)
{
   update_samples(state, cmd->u.set_rasterization_samples_ext.rasterization_samples);
}

static void handle_set_alpha_to_coverage(struct vk_cmd_queue_entry *cmd,
                                         struct rendering_state *state)
{
   state->blend_dirty |=
      state->blend_state.alpha_to_coverage != !!cmd->u.set_alpha_to_coverage_enable_ext.alpha_to_coverage_enable;
   state->blend_state.alpha_to_coverage = !!cmd->u.set_alpha_to_coverage_enable_ext.alpha_to_coverage_enable;
   state->blend_state.alpha_to_coverage_dither = state->blend_state.alpha_to_coverage;
}

static void handle_set_alpha_to_one(struct vk_cmd_queue_entry *cmd,
                                         struct rendering_state *state)
{
   state->blend_dirty |=
      state->blend_state.alpha_to_one != !!cmd->u.set_alpha_to_one_enable_ext.alpha_to_one_enable;
   state->blend_state.alpha_to_one = !!cmd->u.set_alpha_to_one_enable_ext.alpha_to_one_enable;
   if (state->blend_state.alpha_to_one)
      state->rs_state.multisample = true;
}

static void handle_set_halfz(struct vk_cmd_queue_entry *cmd,
                             struct rendering_state *state)
{
   if (state->rs_state.clip_halfz == !cmd->u.set_depth_clip_negative_one_to_one_ext.negative_one_to_one)
      return;
   state->rs_dirty = true;
   state->rs_state.clip_halfz = !cmd->u.set_depth_clip_negative_one_to_one_ext.negative_one_to_one;
   /* handle dynamic state: convert from one transform to the other */
   for (unsigned i = 0; i < state->num_viewports; i++)
      set_viewport_depth_xform(state, i);
   state->vp_dirty = true;
}

static void handle_set_line_rasterization_mode(struct vk_cmd_queue_entry *cmd,
                                               struct rendering_state *state)
{
   VkLineRasterizationModeKHR lineRasterizationMode = cmd->u.set_line_rasterization_mode_ext.line_rasterization_mode;
   /* not even going to bother trying dirty tracking on this */
   state->rs_dirty = true;
   state->rs_state.line_smooth = lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR;
   state->rs_state.line_rectangular = lineRasterizationMode != VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR;;
   state->disable_multisample = lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR ||
                                lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR;
}

static void handle_set_line_stipple_enable(struct vk_cmd_queue_entry *cmd,
                                           struct rendering_state *state)
{
   state->rs_dirty |= state->rs_state.line_stipple_enable != !!cmd->u.set_line_stipple_enable_ext.stippled_line_enable;
   state->rs_state.line_stipple_enable = cmd->u.set_line_stipple_enable_ext.stippled_line_enable;
}

static void handle_set_provoking_vertex_mode(struct vk_cmd_queue_entry *cmd,
                                             struct rendering_state *state)
{
   bool flatshade_first = cmd->u.set_provoking_vertex_mode_ext.provoking_vertex_mode != VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   state->rs_dirty |= state->rs_state.flatshade_first != flatshade_first;
   state->rs_state.flatshade_first = flatshade_first;
}

static void handle_set_color_blend_enable(struct vk_cmd_queue_entry *cmd,
                                          struct rendering_state *state)
{
   for (unsigned i = 0; i < cmd->u.set_color_blend_enable_ext.attachment_count; i++) {
      if (state->blend_state.rt[cmd->u.set_color_blend_enable_ext.first_attachment + i].blend_enable != !!cmd->u.set_color_blend_enable_ext.color_blend_enables[i]) {
         state->blend_dirty = true;
      }
      state->blend_state.rt[cmd->u.set_color_blend_enable_ext.first_attachment + i].blend_enable = !!cmd->u.set_color_blend_enable_ext.color_blend_enables[i];
   }
}

static void handle_set_color_write_mask(struct vk_cmd_queue_entry *cmd,
                                        struct rendering_state *state)
{
   for (unsigned i = 0; i < cmd->u.set_color_write_mask_ext.attachment_count; i++) {
      if (state->blend_state.rt[cmd->u.set_color_write_mask_ext.first_attachment + i].colormask != cmd->u.set_color_write_mask_ext.color_write_masks[i])
         state->blend_dirty = true;
      state->blend_state.rt[cmd->u.set_color_write_mask_ext.first_attachment + i].colormask = cmd->u.set_color_write_mask_ext.color_write_masks[i];
   }
}

static void handle_set_color_blend_equation(struct vk_cmd_queue_entry *cmd,
                                            struct rendering_state *state)
{
   const VkColorBlendEquationEXT *cb = cmd->u.set_color_blend_equation_ext.color_blend_equations;
   state->blend_dirty = true;
   for (unsigned i = 0; i < cmd->u.set_color_blend_equation_ext.attachment_count; i++) {
      state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].rgb_func = vk_blend_op_to_pipe(cb[i].colorBlendOp);
      state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].rgb_src_factor = vk_blend_factor_to_pipe(cb[i].srcColorBlendFactor);
      state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].rgb_dst_factor = vk_blend_factor_to_pipe(cb[i].dstColorBlendFactor);
      state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].alpha_func = vk_blend_op_to_pipe(cb[i].alphaBlendOp);
      state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].alpha_src_factor = vk_blend_factor_to_pipe(cb[i].srcAlphaBlendFactor);
      state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].alpha_dst_factor = vk_blend_factor_to_pipe(cb[i].dstAlphaBlendFactor);

      /* At least llvmpipe applies the blend factor prior to the blend function,
       * regardless of what function is used. (like i965 hardware).
       * It means for MIN/MAX the blend factor has to be stomped to ONE.
       */
      if (cb[i].colorBlendOp == VK_BLEND_OP_MIN ||
          cb[i].colorBlendOp == VK_BLEND_OP_MAX) {
         state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].rgb_src_factor = PIPE_BLENDFACTOR_ONE;
         state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].rgb_dst_factor = PIPE_BLENDFACTOR_ONE;
      }

      if (cb[i].alphaBlendOp == VK_BLEND_OP_MIN ||
          cb[i].alphaBlendOp == VK_BLEND_OP_MAX) {
         state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
         state->blend_state.rt[cmd->u.set_color_blend_equation_ext.first_attachment + i].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
      }
   }
}

static void
handle_shaders(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct vk_cmd_bind_shaders_ext *bind = &cmd->u.bind_shaders_ext;

   bool gfx = false;
   VkShaderStageFlagBits vkstages = 0;
   unsigned new_stages = 0;
   unsigned null_stages = 0;
   for (unsigned i = 0; i < bind->stage_count; i++) {
      gl_shader_stage stage = vk_to_mesa_shader_stage(bind->stages[i]);
      assert(stage != MESA_SHADER_NONE && stage <= MESA_SHADER_MESH);
      LVP_FROM_HANDLE(lvp_shader, shader, bind->shaders ? bind->shaders[i] : VK_NULL_HANDLE);
      if (stage == MESA_SHADER_FRAGMENT) {
         if (shader) {
            state->force_min_sample = shader->pipeline_nir->nir->info.fs.uses_sample_shading;
            state->sample_shading = state->force_min_sample;
            update_samples(state, state->rast_samples);
         } else {
            state->force_min_sample = false;
            state->sample_shading = false;
         }
      }
      if (shader) {
         vkstages |= bind->stages[i];
         new_stages |= BITFIELD_BIT(stage);
         state->shaders[stage] = shader;
      } else {
         if (state->shaders[stage])
            null_stages |= bind->stages[i];
      }

      if (stage != MESA_SHADER_COMPUTE) {
         state->gfx_push_sizes[stage] = shader ? shader->layout->push_constant_size : 0;
         gfx = true;
      } else {
         state->push_size[1] = shader ? shader->layout->push_constant_size : 0;
      }
   }

   if ((new_stages | null_stages) & LVP_STAGE_MASK_GFX) {
      VkShaderStageFlags all_gfx = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT;
      unbind_graphics_stages(state, null_stages & all_gfx);
      handle_graphics_stages(state, vkstages & all_gfx, true);
      u_foreach_bit(i, new_stages) {
         handle_graphics_pushconsts(state, i, state->shaders[i]);
      }
   }
   /* ignore compute unbinds */
   if (new_stages & BITFIELD_BIT(MESA_SHADER_COMPUTE)) {
      handle_compute_shader(state, state->shaders[MESA_SHADER_COMPUTE]);
   }

   if (gfx) {
      state->push_size[0] = 0;
      for (unsigned i = 0; i < ARRAY_SIZE(state->gfx_push_sizes); i++)
         state->push_size[0] += state->gfx_push_sizes[i];
   }
}

static void
update_mesh_state(struct rendering_state *state)
{
   if (state->shaders[MESA_SHADER_TASK]) {
      state->dispatch_info.block[0] = state->shaders[MESA_SHADER_TASK]->pipeline_nir->nir->info.workgroup_size[0];
      state->dispatch_info.block[1] = state->shaders[MESA_SHADER_TASK]->pipeline_nir->nir->info.workgroup_size[1];
      state->dispatch_info.block[2] = state->shaders[MESA_SHADER_TASK]->pipeline_nir->nir->info.workgroup_size[2];
   } else {
      state->dispatch_info.block[0] = state->shaders[MESA_SHADER_MESH]->pipeline_nir->nir->info.workgroup_size[0];
      state->dispatch_info.block[1] = state->shaders[MESA_SHADER_MESH]->pipeline_nir->nir->info.workgroup_size[1];
      state->dispatch_info.block[2] = state->shaders[MESA_SHADER_MESH]->pipeline_nir->nir->info.workgroup_size[2];
   }
}

static void handle_draw_mesh_tasks(struct vk_cmd_queue_entry *cmd,
                                   struct rendering_state *state)
{
   update_mesh_state(state);
   state->dispatch_info.grid[0] = cmd->u.draw_mesh_tasks_ext.group_count_x;
   state->dispatch_info.grid[1] = cmd->u.draw_mesh_tasks_ext.group_count_y;
   state->dispatch_info.grid[2] = cmd->u.draw_mesh_tasks_ext.group_count_z;
   state->dispatch_info.grid_base[0] = 0;
   state->dispatch_info.grid_base[1] = 0;
   state->dispatch_info.grid_base[2] = 0;
   state->dispatch_info.draw_count = 1;
   state->dispatch_info.indirect = NULL;
   state->pctx->draw_mesh_tasks(state->pctx, &state->dispatch_info);
}

static void handle_draw_mesh_tasks_indirect(struct vk_cmd_queue_entry *cmd,
                                            struct rendering_state *state)
{
   update_mesh_state(state);
   state->dispatch_info.indirect = lvp_buffer_from_handle(cmd->u.draw_mesh_tasks_indirect_ext.buffer)->bo;
   state->dispatch_info.indirect_offset = cmd->u.draw_mesh_tasks_indirect_ext.offset;
   state->dispatch_info.indirect_stride = cmd->u.draw_mesh_tasks_indirect_ext.stride;
   state->dispatch_info.draw_count = cmd->u.draw_mesh_tasks_indirect_ext.draw_count;
   state->pctx->draw_mesh_tasks(state->pctx, &state->dispatch_info);
}

static void handle_draw_mesh_tasks_indirect_count(struct vk_cmd_queue_entry *cmd,
                                                  struct rendering_state *state)
{
   update_mesh_state(state);
   state->dispatch_info.indirect = lvp_buffer_from_handle(cmd->u.draw_mesh_tasks_indirect_count_ext.buffer)->bo;
   state->dispatch_info.indirect_offset = cmd->u.draw_mesh_tasks_indirect_count_ext.offset;
   state->dispatch_info.indirect_stride = cmd->u.draw_mesh_tasks_indirect_count_ext.stride;
   state->dispatch_info.draw_count = cmd->u.draw_mesh_tasks_indirect_count_ext.max_draw_count;
   state->dispatch_info.indirect_draw_count_offset = cmd->u.draw_mesh_tasks_indirect_count_ext.count_buffer_offset;
   state->dispatch_info.indirect_draw_count = lvp_buffer_from_handle(cmd->u.draw_mesh_tasks_indirect_count_ext.count_buffer)->bo;
   state->pctx->draw_mesh_tasks(state->pctx, &state->dispatch_info);
}

static VkBuffer
get_buffer(struct rendering_state *state, const uint8_t *ptr, size_t *offset)
{
   simple_mtx_lock(&state->device->bda_lock);
   hash_table_foreach(&state->device->bda, he) {
      const uint8_t *bda = he->key;
      if (ptr < bda)
         continue;
      struct lvp_buffer *buffer = he->data;
      if (bda + buffer->vk.size > ptr) {
         *offset = ptr - bda;
         simple_mtx_unlock(&state->device->bda_lock);
         return lvp_buffer_to_handle(buffer);
      }
   }
   fprintf(stderr, "unrecognized BDA!\n");
   abort();
}

static size_t
process_sequence_ext(struct rendering_state *state,
                     struct lvp_indirect_execution_set *iset, struct lvp_indirect_command_layout_ext *elayout,
                     struct list_head *list, uint8_t *pbuf, size_t max_size,
                     uint8_t *stream, uint32_t seq, uint32_t maxDrawCount,
                     bool print_cmds)
{
   size_t size = 0;
   assert(elayout->vk.token_count);
   for (uint32_t t = 0; t < elayout->vk.token_count; t++){
      const VkIndirectCommandsLayoutTokenEXT *token = &elayout->tokens[t];
      uint32_t offset = elayout->vk.stride * seq + token->offset;
      void *input = stream + offset;

      struct vk_cmd_queue_entry *cmd = (struct vk_cmd_queue_entry*)(pbuf + size);
      cmd->type = lvp_ext_dgc_token_to_cmd_type(elayout, token);
      size_t cmd_size = vk_cmd_queue_type_sizes[cmd->type];
      uint8_t *cmdptr = (void*)(pbuf + size + cmd_size);

      if (max_size < size + lvp_ext_dgc_token_size(elayout, token))
         abort();

      if (print_cmds)
         fprintf(stderr, "DGC %s\n", vk_IndirectCommandsTokenTypeEXT_to_str(token->type));
      switch (token->type) {
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_EXECUTION_SET_EXT: {
         uint32_t *data = input;
         const VkIndirectCommandsExecutionSetTokenEXT *info = token->data.pExecutionSet;
         if (info->type == VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT) {
            cmd->u.bind_pipeline.pipeline_bind_point = lvp_pipeline_types_from_shader_stages(info->shaderStages);
            cmd->u.bind_pipeline.pipeline = iset->array[*data];
            /* validate */
            lvp_pipeline_from_handle(cmd->u.bind_pipeline.pipeline);

            assert(cmd->u.bind_pipeline.pipeline && "cannot bind null pipeline!");
         } else {
            unsigned count = util_bitcount(info->shaderStages);
            cmd->u.bind_shaders_ext.stage_count = count;
            cmd->u.bind_shaders_ext.stages = (void*)cmdptr;
            int i = 0;
            u_foreach_bit(stage, info->shaderStages) {
               cmd->u.bind_shaders_ext.stages[i] = BITFIELD_BIT(stage);
               assert(cmd->u.bind_shaders_ext.stages[i] && "cannot bind null shader stage!");
               i++;
            }
            cmd->u.bind_shaders_ext.shaders = (void*)(cmdptr + sizeof(int64_t) * count);
            for (unsigned i = 0; i < count; i++) {
               cmd->u.bind_shaders_ext.shaders[i] = iset->array[data[i]];
               if (cmd->u.bind_shaders_ext.shaders[i])
                  lvp_shader_from_handle(cmd->u.bind_shaders_ext.shaders[i]);
            }
         }
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT:
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_SEQUENCE_INDEX_EXT: {
         uint32_t *data = input;
         const VkIndirectCommandsPushConstantTokenEXT *info = token->data.pPushConstant;
         cmd->u.push_constants2.push_constants_info = (void*)cmdptr;
         VkPushConstantsInfoKHR *pci = cmd->u.push_constants2.push_constants_info;
         pci->layout = elayout->vk.layout;
         pci->stageFlags = VK_SHADER_STAGE_ALL;
         pci->offset = info->updateRange.offset;
         pci->size = info->updateRange.size;
         pci->pValues = (void*)((uint8_t*)cmdptr + sizeof(VkPushConstantsInfoKHR));
         if (token->type == VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT)
            memcpy((void*)pci->pValues, data, info->updateRange.size);
         else
            memcpy((void*)pci->pValues, &seq, info->updateRange.size);

         break;
      }
/* these are the DXGI format values to avoid needing the full header */
#define DXGI_FORMAT_R32_UINT 42
#define DXGI_FORMAT_R16_UINT 57
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT: {
         const VkIndirectCommandsIndexBufferTokenEXT *info = token->data.pIndexBuffer;
         VkBindIndexBufferIndirectCommandEXT *data = input;
         cmd->u.bind_index_buffer2.offset = 0;
         if (data->bufferAddress)
            cmd->u.bind_index_buffer2.buffer = get_buffer(state, (void*)(uintptr_t)data->bufferAddress, (size_t*)&cmd->u.bind_index_buffer.offset);
         else
            cmd->u.bind_index_buffer2.buffer = VK_NULL_HANDLE;
         if (info->mode == VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT) {
            cmd->u.bind_index_buffer2.index_type = data->indexType;
         } else {
            switch ((int)data->indexType) {
            case DXGI_FORMAT_R32_UINT:
               cmd->u.bind_index_buffer2.index_type = VK_INDEX_TYPE_UINT32;
               break;
            case DXGI_FORMAT_R16_UINT:
               cmd->u.bind_index_buffer2.index_type = VK_INDEX_TYPE_UINT16;
               break;
            default:
               UNREACHABLE("unknown DXGI index type!");
            }
         }
         cmd->u.bind_index_buffer2.size = data->size;
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_VERTEX_BUFFER_EXT: {
         VkBindVertexBufferIndirectCommandEXT *data = input;
         cmd_size += sizeof(*cmd->u.bind_vertex_buffers2.buffers) + sizeof(*cmd->u.bind_vertex_buffers2.offsets);
         cmd_size += sizeof(*cmd->u.bind_vertex_buffers2.sizes) + sizeof(*cmd->u.bind_vertex_buffers2.strides);
         if (max_size < size + cmd_size)
            abort();

         cmd->u.bind_vertex_buffers2.first_binding = token->data.pVertexBuffer->vertexBindingUnit;
         cmd->u.bind_vertex_buffers2.binding_count = 1;

         cmd->u.bind_vertex_buffers2.buffers = (void*)cmdptr;
         uint32_t alloc_offset = sizeof(*cmd->u.bind_vertex_buffers2.buffers);

         cmd->u.bind_vertex_buffers2.offsets = (void*)(cmdptr + alloc_offset);
         alloc_offset += sizeof(*cmd->u.bind_vertex_buffers2.offsets);

         cmd->u.bind_vertex_buffers2.sizes = (void*)(cmdptr + alloc_offset);
         alloc_offset += sizeof(*cmd->u.bind_vertex_buffers2.sizes);

         cmd->u.bind_vertex_buffers2.offsets[0] = 0;
         cmd->u.bind_vertex_buffers2.buffers[0] = data->bufferAddress ? get_buffer(state, (void*)(uintptr_t)data->bufferAddress, (size_t*)&cmd->u.bind_vertex_buffers2.offsets[0]) : VK_NULL_HANDLE;
         cmd->u.bind_vertex_buffers2.sizes[0] = data->size;

         cmd->u.bind_vertex_buffers2.strides = (void*)(cmdptr + alloc_offset);
         cmd->u.bind_vertex_buffers2.strides[0] = data->stride;
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT: {
         VkDispatchIndirectCommand *data = input;
         memcpy(&cmd->u.dispatch, data, sizeof(VkDispatchIndirectCommand));
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT: {
         VkDrawIndexedIndirectCommand *data = input;
         memcpy(&cmd->u.draw_indexed, data, sizeof(VkDrawIndexedIndirectCommand));
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT: {
         VkDrawIndirectCommand *data = input;
         memcpy(&cmd->u.draw, data, sizeof(VkDrawIndirectCommand));
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_COUNT_EXT: {
         VkDrawIndirectCountIndirectCommandEXT *data = input;

         cmd->u.draw_indexed_indirect.buffer = get_buffer(state, (void*)(uintptr_t)data->bufferAddress, (size_t*)&cmd->u.draw_indexed_indirect.offset);
         cmd->u.draw_indexed_indirect.draw_count = MIN2(data->commandCount, maxDrawCount);
         cmd->u.draw_indexed_indirect.stride = data->stride;
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_COUNT_EXT: {
         VkDrawIndirectCountIndirectCommandEXT *data = input;

         cmd->u.draw_indirect.buffer = get_buffer(state, (void*)(uintptr_t)data->bufferAddress, (size_t*)&cmd->u.draw_indirect.offset);
         cmd->u.draw_indirect.draw_count = MIN2(data->commandCount, maxDrawCount);
         cmd->u.draw_indirect.stride = data->stride;
         break;
      }
      // only available if VK_EXT_mesh_shader is supported
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT: {
         VkDrawMeshTasksIndirectCommandEXT *data = input;
         memcpy(&cmd->u.draw_mesh_tasks_ext, data, sizeof(VkDrawIndirectCountIndirectCommandEXT));
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_COUNT_EXT: {
         VkDrawIndirectCountIndirectCommandEXT *data = input;

         cmd->u.draw_mesh_tasks_indirect_ext.buffer = get_buffer(state, (void*)(uintptr_t)data->bufferAddress, (size_t*)&cmd->u.draw_mesh_tasks_indirect_ext.offset);
         cmd->u.draw_mesh_tasks_indirect_ext.draw_count = MIN2(data->commandCount, maxDrawCount);
         cmd->u.draw_mesh_tasks_indirect_ext.stride = data->stride;
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_TRACE_RAYS2_EXT: {
         VkTraceRaysIndirectCommand2KHR *data = input;
         VkStridedDeviceAddressRegionKHR *sbts = (void*)cmdptr;

         cmd->u.trace_rays_khr.raygen_shader_binding_table = &sbts[0];
         cmd->u.trace_rays_khr.raygen_shader_binding_table->deviceAddress = data->raygenShaderRecordAddress;
         cmd->u.trace_rays_khr.raygen_shader_binding_table->stride = data->raygenShaderRecordSize;
         cmd->u.trace_rays_khr.raygen_shader_binding_table->size = data->raygenShaderRecordSize;

         cmd->u.trace_rays_khr.miss_shader_binding_table = &sbts[1];
         cmd->u.trace_rays_khr.miss_shader_binding_table->deviceAddress = data->missShaderBindingTableAddress;
         cmd->u.trace_rays_khr.miss_shader_binding_table->stride = data->missShaderBindingTableStride;
         cmd->u.trace_rays_khr.miss_shader_binding_table->size = data->missShaderBindingTableSize;

         cmd->u.trace_rays_khr.hit_shader_binding_table = &sbts[2];
         cmd->u.trace_rays_khr.hit_shader_binding_table->deviceAddress = data->hitShaderBindingTableAddress;
         cmd->u.trace_rays_khr.hit_shader_binding_table->stride = data->hitShaderBindingTableStride;
         cmd->u.trace_rays_khr.hit_shader_binding_table->size = data->hitShaderBindingTableSize;

         cmd->u.trace_rays_khr.callable_shader_binding_table = &sbts[3];
         cmd->u.trace_rays_khr.callable_shader_binding_table->deviceAddress = data->callableShaderBindingTableAddress;
         cmd->u.trace_rays_khr.callable_shader_binding_table->stride = data->callableShaderBindingTableStride;
         cmd->u.trace_rays_khr.callable_shader_binding_table->size = data->callableShaderBindingTableSize;

         cmd->u.trace_rays_khr.width = data->width;
         cmd->u.trace_rays_khr.height = data->height;
         cmd->u.trace_rays_khr.depth = data->depth;

         break;
      }
      default:
         UNREACHABLE("unknown token type");
         break;
      }
      size += lvp_ext_dgc_token_size(elayout, token);
      list_addtail(&cmd->cmd_link, list);
   }
   return size;
}

static void
handle_preprocess_generated_commands_ext(struct vk_cmd_queue_entry *cmd, struct rendering_state *state, bool print_cmds)
{
   VkGeneratedCommandsInfoEXT *pre = cmd->u.preprocess_generated_commands_ext.generated_commands_info;
   VK_FROM_HANDLE(lvp_indirect_command_layout_ext, elayout, pre->indirectCommandsLayout);
   VK_FROM_HANDLE(lvp_indirect_execution_set, iset, pre->indirectExecutionSet);

   unsigned seq_count = pre->maxSequenceCount;
   if (pre->sequenceCountAddress) {
      uint32_t *count = (void*)(uintptr_t)pre->sequenceCountAddress;
      seq_count = MIN2(seq_count, *count);
   }

   struct list_head *list = (void*)(uintptr_t)pre->preprocessAddress;
   size_t size = sizeof(struct list_head);
   size_t max_size = pre->preprocessSize;
   if (size > max_size)
      abort();
   list_inithead(list);

   size_t offset = size;
   uint8_t *p = (void*)(uintptr_t)pre->preprocessAddress;
   for (unsigned i = 0; i < seq_count; i++) {
      offset += process_sequence_ext(state, iset, elayout, list, p + offset, max_size, (void*)(uintptr_t)pre->indirectAddress, i, pre->maxDrawCount, print_cmds);
      assert(offset);
   }

   /* vk_cmd_queue will copy the binary and break the list, so null the tail pointer */
   list->prev->next = NULL;
}

static void
handle_execute_generated_commands_ext(struct vk_cmd_queue_entry *cmd, struct rendering_state *state, bool print_cmds)
{
   VkGeneratedCommandsInfoEXT *gen = cmd->u.execute_generated_commands_ext.generated_commands_info;
   struct vk_cmd_execute_generated_commands_ext *exec = &cmd->u.execute_generated_commands_ext;
   if (!exec->is_preprocessed) {
      struct vk_cmd_queue_entry pre;
      pre.u.preprocess_generated_commands_ext.generated_commands_info = exec->generated_commands_info;
      handle_preprocess_generated_commands_ext(&pre, state, print_cmds);
   }
   uint8_t *p = (void*)(uintptr_t)gen->preprocessAddress;
   struct list_head *list = (void*)p;

   struct vk_cmd_queue_entry *exec_cmd = list_first_entry(list, struct vk_cmd_queue_entry, cmd_link);
   if (exec_cmd)
      lvp_execute_cmd_buffer(list, state, print_cmds);
}

static void
handle_descriptor_buffers(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   const struct vk_cmd_bind_descriptor_buffers_ext *bind = &cmd->u.bind_descriptor_buffers_ext;
   for (unsigned i = 0; i < bind->buffer_count; i++) {
      struct pipe_resource *pres = get_buffer_resource(state->pctx, (void *)(uintptr_t)bind->binding_infos[i].address);
      state->desc_buffer_addrs[i] = (void *)(uintptr_t)bind->binding_infos[i].address;
      pipe_resource_reference(&state->desc_buffers[i], pres);
      /* leave only one ref on rendering_state */
      pipe_resource_reference(&pres, NULL);
   }
}

static bool
descriptor_layouts_equal(const struct lvp_descriptor_set_layout *a, const struct lvp_descriptor_set_layout *b)
{
   const uint8_t *pa = (const uint8_t*)a, *pb = (const uint8_t*)b;
   uint32_t hash_start_offset = sizeof(struct vk_descriptor_set_layout);
   uint32_t binding_offset = offsetof(struct lvp_descriptor_set_layout, binding);
   /* base equal */
   if (memcmp(pa + hash_start_offset, pb + hash_start_offset, binding_offset - hash_start_offset))
      return false;

   /* bindings equal */
   if (a->binding_count != b->binding_count)
      return false;
   size_t binding_size = a->binding_count * sizeof(struct lvp_descriptor_set_binding_layout);
   const struct lvp_descriptor_set_binding_layout *la = a->binding;
   const struct lvp_descriptor_set_binding_layout *lb = b->binding;
   if (memcmp(la, lb, binding_size)) {
      for (unsigned i = 0; i < a->binding_count; i++) {
         if (memcmp(&la[i], &lb[i], offsetof(struct lvp_descriptor_set_binding_layout, immutable_samplers)))
            return false;
      }
   }

   /* immutable sampler equal */
   if (a->immutable_sampler_count != b->immutable_sampler_count)
      return false;
   if (a->immutable_sampler_count) {
      size_t sampler_size = a->immutable_sampler_count * sizeof(struct lvp_sampler *);
      if (memcmp(pa + binding_offset + binding_size, pb + binding_offset + binding_size, sampler_size)) {
         struct lvp_sampler **sa = (struct lvp_sampler **)(pa + binding_offset);
         struct lvp_sampler **sb = (struct lvp_sampler **)(pb + binding_offset);
         for (unsigned i = 0; i < a->immutable_sampler_count; i++) {
            if (memcmp(sa[i], sb[i], sizeof(struct lvp_sampler)))
               return false;
         }
      }
   }
   return true;
}

static void
bind_db_samplers(struct rendering_state *state, enum lvp_pipeline_type pipeline_type, unsigned set)
{
   const struct lvp_descriptor_set_layout *set_layout = state->desc_buffer_offsets[pipeline_type][set].sampler_layout;
   if (!set_layout)
      return;
   unsigned buffer_index = state->desc_buffer_offsets[pipeline_type][set].buffer_index;
   if (!state->desc_buffer_addrs[buffer_index]) {
      if (set_layout->immutable_set) {
         state->desc_sets[pipeline_type][set] = set_layout->immutable_set;
         if (pipeline_type == LVP_PIPELINE_RAY_TRACING) {
            handle_set_stage_buffer(state, set_layout->immutable_set->bo, 0, MESA_SHADER_RAYGEN, set);
         } else {
            u_foreach_bit(stage, set_layout->shader_stages)
               handle_set_stage_buffer(state, set_layout->immutable_set->bo, 0, vk_to_mesa_shader_stage(1<<stage), set);
         }
      }
      return;
   }
   uint8_t *db = state->desc_buffer_addrs[buffer_index] + state->desc_buffer_offsets[pipeline_type][set].offset;
   uint32_t did_update = 0;
   for (uint32_t binding_index = 0; binding_index < set_layout->binding_count; binding_index++) {
      const struct lvp_descriptor_set_binding_layout *bind_layout = &set_layout->binding[binding_index];
      if (!bind_layout->immutable_samplers)
         continue;

      struct lp_descriptor *desc = (void*)db;
      desc += bind_layout->descriptor_index;

      for (uint32_t sampler_index = 0; sampler_index < bind_layout->array_size; sampler_index++) {
         if (bind_layout->immutable_samplers[sampler_index]) {
            struct lp_descriptor *immutable_desc = &bind_layout->immutable_samplers[sampler_index]->desc;
            desc[sampler_index].sampler = immutable_desc->sampler;
            desc[sampler_index].texture.sampler_index = immutable_desc->texture.sampler_index;
            if (pipeline_type == LVP_PIPELINE_RAY_TRACING) {
               did_update |= BITFIELD_BIT(MESA_SHADER_RAYGEN);
            } else {
               u_foreach_bit(stage, set_layout->shader_stages)
                  did_update |= BITFIELD_BIT(vk_to_mesa_shader_stage(1<<stage));
            }
         }
      }
   }
   u_foreach_bit(stage, did_update)
      state->constbuf_dirty[stage] = true;
}

static void
handle_descriptor_buffer_embedded_samplers(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   const VkBindDescriptorBufferEmbeddedSamplersInfoEXT *bind = cmd->u.bind_descriptor_buffer_embedded_samplers2_ext.bind_descriptor_buffer_embedded_samplers_info;
   LVP_FROM_HANDLE(lvp_pipeline_layout, layout, bind->layout);

   if (!layout->vk.set_layouts[bind->set])
      return;

   const struct lvp_descriptor_set_layout *set_layout = get_set_layout(layout, bind->set);
   if (!set_layout->immutable_sampler_count)
      return;
   uint32_t types = lvp_pipeline_types_from_shader_stages(bind->stageFlags);
   u_foreach_bit(pipeline_type, types) {
      state->desc_buffer_offsets[pipeline_type][bind->set].sampler_layout = set_layout;
      bind_db_samplers(state, pipeline_type, bind->set);
   }
}

static void
handle_descriptor_buffer_offsets(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   VkSetDescriptorBufferOffsetsInfoEXT *dbo = cmd->u.set_descriptor_buffer_offsets2_ext.set_descriptor_buffer_offsets_info;
   uint32_t types = lvp_pipeline_types_from_shader_stages(dbo->stageFlags);
   u_foreach_bit(pipeline_type, types) {
      for (unsigned i = 0; i < dbo->setCount; i++) {
         LVP_FROM_HANDLE(lvp_pipeline_layout, layout, dbo->layout);
         unsigned idx = dbo->firstSet + i;
         state->desc_buffer_offsets[pipeline_type][idx].buffer_index = dbo->pBufferIndices[i];
         state->desc_buffer_offsets[pipeline_type][idx].offset = dbo->pOffsets[i];
         const struct lvp_descriptor_set_layout *set_layout = get_set_layout(layout, idx);

         if (pipeline_type == LVP_PIPELINE_RAY_TRACING) {
            handle_set_stage_buffer(state, state->desc_buffers[dbo->pBufferIndices[i]], dbo->pOffsets[i], MESA_SHADER_RAYGEN, idx);
         } else {
            /* set for all stages */
            u_foreach_bit(stage, set_layout->shader_stages) {
               gl_shader_stage pstage = vk_to_mesa_shader_stage(1<<stage);
               handle_set_stage_buffer(state, state->desc_buffers[dbo->pBufferIndices[i]], dbo->pOffsets[i], pstage, idx);
            }
         }
         bind_db_samplers(state, pipeline_type, idx);
      }
   }
}

static void *
lvp_push_internal_buffer(struct rendering_state *state, gl_shader_stage stage, uint32_t size)
{
   if (!size)
      return NULL;

   struct pipe_shader_buffer buffer = {
      .buffer_size = size,
   };

   uint8_t *mem;
   u_upload_alloc(state->uploader, 0, size, 64, &buffer.buffer_offset, &buffer.buffer, (void**)&mem);

   state->pctx->set_shader_buffers(state->pctx, stage, 0, 1, &buffer, 0x1);

   util_dynarray_append(&state->internal_buffers, struct pipe_resource *, buffer.buffer);

   return mem;
}

#ifdef VK_ENABLE_BETA_EXTENSIONS

static void
dispatch_graph(struct rendering_state *state, const VkDispatchGraphInfoAMDX *info, void *scratch)
{
   VK_FROM_HANDLE(lvp_pipeline, pipeline, state->exec_graph->groups[info->nodeIndex]);
   struct lvp_shader *shader = &pipeline->shaders[MESA_SHADER_COMPUTE];
   nir_shader *nir = shader->pipeline_nir->nir;

   VkPipelineShaderStageNodeCreateInfoAMDX enqueue_node_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NODE_CREATE_INFO_AMDX,
      .pName = pipeline->exec_graph.next_name,
   };

   for (uint32_t i = 0; i < info->payloadCount; i++) {
      const void *payload = (const void *)((const uint8_t *)info->payloads.hostAddress + i * info->payloadStride);

      /* The spec doesn't specify any useful limits for enqueued payloads.
       * Since we allocate them in scratch memory (provided to the dispatch entrypoint),
       * we need to execute recursive shaders one to keep scratch requirements finite.
       */
      VkDispatchIndirectCommand dispatch = *(const VkDispatchIndirectCommand *)payload;
      if (nir->info.cs.workgroup_count[0]) {
         dispatch.x = nir->info.cs.workgroup_count[0];
         dispatch.y = nir->info.cs.workgroup_count[1];
         dispatch.z = nir->info.cs.workgroup_count[2];
      }

      state->dispatch_info.indirect = NULL;
      state->dispatch_info.grid[0] = 1;
      state->dispatch_info.grid[1] = 1;
      state->dispatch_info.grid[2] = 1;

      for (uint32_t z = 0; z < dispatch.z; z++) {
         for (uint32_t y = 0; y < dispatch.y; y++) {
            for (uint32_t x = 0; x < dispatch.x; x++) {
               handle_compute_shader(state, shader);
               emit_compute_state(state);

               state->dispatch_info.grid_base[0] = x;
               state->dispatch_info.grid_base[1] = y;
               state->dispatch_info.grid_base[2] = z;

               struct lvp_exec_graph_internal_data *internal_data =
                  lvp_push_internal_buffer(state, MESA_SHADER_COMPUTE, sizeof(struct lvp_exec_graph_internal_data));
               internal_data->payload_in = (void *)payload;
               internal_data->payloads = (void *)scratch;

               state->pctx->launch_grid(state->pctx, &state->dispatch_info);

               /* Amazing performance. */
               finish_fence(state);

               for (uint32_t enqueue = 0; enqueue < ARRAY_SIZE(internal_data->outputs); enqueue++) {
                  struct lvp_exec_graph_shader_output *output = &internal_data->outputs[enqueue];
                  if (!output->payload_count)
                     continue;

                  VkDispatchGraphInfoAMDX enqueue_info = {
                     .payloadCount = output->payload_count,
                     .payloads.hostAddress = (uint8_t *)scratch + enqueue * nir->info.cs.node_payloads_size,
                     .payloadStride = nir->info.cs.node_payloads_size,
                  };

                  enqueue_node_info.index = output->node_index;

                  ASSERTED VkResult result = lvp_GetExecutionGraphPipelineNodeIndexAMDX(
                     lvp_device_to_handle(state->device), lvp_pipeline_to_handle(state->exec_graph),
                     &enqueue_node_info, &enqueue_info.nodeIndex);
                  assert(result == VK_SUCCESS);

                  dispatch_graph(state, &enqueue_info, (uint8_t *)scratch + pipeline->exec_graph.scratch_size);
               }
            }
         }
      }
   }
}

static void
handle_dispatch_graph(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   const struct vk_cmd_dispatch_graph_amdx *dispatch = &cmd->u.dispatch_graph_amdx;

   for (uint32_t i = 0; i < dispatch->count_info->count; i++) {
      const VkDispatchGraphInfoAMDX *info = (const void *)((const uint8_t *)dispatch->count_info->infos.hostAddress +
                                                           i * dispatch->count_info->stride);

      dispatch_graph(state, info, (void *)(uintptr_t)dispatch->scratch);
   }
}
#endif

static struct pipe_resource *
get_buffer_pipe(struct rendering_state *state, const void *ptr)
{
   size_t offset;
   VK_FROM_HANDLE(lvp_buffer, buffer, get_buffer(state, ptr, &offset));
   return buffer->bo;
}

static void
handle_copy_acceleration_structure(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct vk_cmd_copy_acceleration_structure_khr *copy = &cmd->u.copy_acceleration_structure_khr;

   VK_FROM_HANDLE(vk_acceleration_structure, src_accel_struct, copy->info->src);
   VK_FROM_HANDLE(vk_acceleration_structure, dst_accel_struct, copy->info->dst);

   struct lvp_bvh_header *src = (void *)(uintptr_t)vk_acceleration_structure_get_va(src_accel_struct);
   struct lvp_bvh_header *dst = (void *)(uintptr_t)vk_acceleration_structure_get_va(dst_accel_struct);
   memcpy(dst, src, src->compacted_size);
}

static void
handle_copy_memory_to_acceleration_structure(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct vk_cmd_copy_memory_to_acceleration_structure_khr *copy = &cmd->u.copy_memory_to_acceleration_structure_khr;

   VK_FROM_HANDLE(vk_acceleration_structure, accel_struct, copy->info->dst);

   struct lvp_bvh_header *dst = (void *)(uintptr_t)vk_acceleration_structure_get_va(accel_struct);
   const struct lvp_accel_struct_serialization_header *src = copy->info->src.hostAddress;

   memcpy(dst, &src->instances[src->instance_count], src->compacted_size);

   for (uint32_t i = 0; i < src->instance_count; i++) {
      uint8_t *leaf_nodes = (uint8_t *)dst;
      leaf_nodes += dst->leaf_nodes_offset;
      struct lvp_bvh_instance_node *node = (struct lvp_bvh_instance_node *)leaf_nodes;
      node[i].bvh_ptr = src->instances[i];
   }
}

static void
handle_copy_acceleration_structure_to_memory(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct vk_cmd_copy_acceleration_structure_to_memory_khr *copy = &cmd->u.copy_acceleration_structure_to_memory_khr;

   VK_FROM_HANDLE(vk_acceleration_structure, accel_struct, copy->info->src);

   struct lvp_bvh_header *src = (void *)(uintptr_t)vk_acceleration_structure_get_va(accel_struct);
   struct lvp_accel_struct_serialization_header *dst = copy->info->dst.hostAddress;

   lvp_device_get_cache_uuid(dst->driver_uuid);
   lvp_device_get_cache_uuid(dst->accel_struct_compat);
   dst->serialization_size = src->serialization_size;
   dst->compacted_size = src->compacted_size;
   dst->instance_count = src->instance_count;

   for (uint32_t i = 0; i < src->instance_count; i++) {
      uint8_t *leaf_nodes = (uint8_t *)src;
      leaf_nodes += src->leaf_nodes_offset;
      struct lvp_bvh_instance_node *node = (struct lvp_bvh_instance_node *)leaf_nodes;
      dst->instances[i] = node[i].bvh_ptr;
   }

   memcpy(&dst->instances[dst->instance_count], src, src->compacted_size);
}

static void
handle_write_acceleration_structures_properties(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct vk_cmd_write_acceleration_structures_properties_khr *write = &cmd->u.write_acceleration_structures_properties_khr;

   VK_FROM_HANDLE(lvp_query_pool, pool, write->query_pool);

   uint64_t *dst = pool->data;
   dst += write->first_query;

   for (uint32_t i = 0; i < write->acceleration_structure_count; i++) {
      VK_FROM_HANDLE(vk_acceleration_structure, accel_struct, write->acceleration_structures[i]);

      struct lvp_bvh_header *header = (void *)(uintptr_t)vk_acceleration_structure_get_va(accel_struct);

      switch ((uint32_t)pool->base_type) {
      case LVP_QUERY_ACCELERATION_STRUCTURE_COMPACTED_SIZE:
         dst[i] = header->compacted_size;
         break;
      case LVP_QUERY_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE: {
         dst[i] = header->serialization_size;
         break;
      }
      case LVP_QUERY_ACCELERATION_STRUCTURE_SIZE:
         dst[i] = header->compacted_size;
         break;
      case LVP_QUERY_ACCELERATION_STRUCTURE_INSTANCE_COUNT: {
         dst[i] = header->instance_count;
         break;
      }
      default:
         UNREACHABLE("Unsupported query type");
      }
   }
}

static void
lvp_trace_rays(struct rendering_state *state, VkTraceRaysIndirectCommand2KHR *command)
{
   /* Emit ray tracing state. */
   bool pcbuf_dirty = state->pcbuf_dirty[MESA_SHADER_RAYGEN];
   if (pcbuf_dirty)
      update_pcbuf(state, MESA_SHADER_COMPUTE, MESA_SHADER_RAYGEN);

   if (state->constbuf_dirty[MESA_SHADER_RAYGEN]) {
      for (unsigned i = 0; i < state->num_const_bufs[MESA_SHADER_RAYGEN]; i++)
         state->pctx->set_constant_buffer(state->pctx, MESA_SHADER_COMPUTE,
                                          i + 1, false, &state->const_buffer[MESA_SHADER_RAYGEN][i]);
      state->constbuf_dirty[MESA_SHADER_RAYGEN] = false;
   }

   state->pctx->bind_compute_state(state->pctx, state->shaders[MESA_SHADER_RAYGEN]->shader_cso);

   state->pcbuf_dirty[MESA_SHADER_COMPUTE] = true;
   state->constbuf_dirty[MESA_SHADER_COMPUTE] = true;
   state->compute_shader_dirty = true;

   /* Dispatch. The spec states that conditional rendering only affects compute dispatches
    * so ray tracing dispatches have to suspend it.
    */
   state->trace_rays_info.grid[0] = DIV_ROUND_UP(command->width, state->trace_rays_info.block[0]);
   state->trace_rays_info.grid[1] = DIV_ROUND_UP(command->height, state->trace_rays_info.block[1]);
   state->trace_rays_info.grid[2] = DIV_ROUND_UP(command->depth, state->trace_rays_info.block[2]);

   bool conditional_rendering_enabled = state->conditional_rendering.enabled;
   if (conditional_rendering_enabled) {
      state->conditional_rendering.enabled = false;
      lvp_emit_conditional_rendering(state);
   }

   state->pctx->launch_grid(state->pctx, &state->trace_rays_info);

   if (conditional_rendering_enabled) {
      state->conditional_rendering.enabled = true;
      lvp_emit_conditional_rendering(state);
   }
}

static void
handle_trace_rays(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct vk_cmd_trace_rays_khr *trace = &cmd->u.trace_rays_khr;

   VkTraceRaysIndirectCommand2KHR *command = lvp_push_internal_buffer(
      state, MESA_SHADER_COMPUTE, sizeof(VkTraceRaysIndirectCommand2KHR));

   *command = (VkTraceRaysIndirectCommand2KHR) {
      .raygenShaderRecordAddress = trace->raygen_shader_binding_table->deviceAddress,
      .raygenShaderRecordSize = trace->raygen_shader_binding_table->size,
      .missShaderBindingTableAddress = trace->miss_shader_binding_table->deviceAddress,
      .missShaderBindingTableSize = trace->miss_shader_binding_table->size,
      .missShaderBindingTableStride = trace->miss_shader_binding_table->stride,
      .hitShaderBindingTableAddress = trace->hit_shader_binding_table->deviceAddress,
      .hitShaderBindingTableSize = trace->hit_shader_binding_table->size,
      .hitShaderBindingTableStride = trace->hit_shader_binding_table->stride,
      .callableShaderBindingTableAddress = trace->callable_shader_binding_table->deviceAddress,
      .callableShaderBindingTableSize = trace->callable_shader_binding_table->size,
      .callableShaderBindingTableStride = trace->callable_shader_binding_table->stride,
      .width = trace->width,
      .height = trace->height,
      .depth = trace->depth,
   };

   lvp_trace_rays(state, command);
}

static void
handle_trace_rays_indirect(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct vk_cmd_trace_rays_indirect_khr *trace = &cmd->u.trace_rays_indirect_khr;

   size_t indirect_offset;
   VkBuffer _indirect = get_buffer(state, (void *)(uintptr_t)trace->indirect_device_address, &indirect_offset);
   VK_FROM_HANDLE(lvp_buffer, indirect, _indirect);

   struct pipe_transfer *transfer;
   const uint8_t *map = pipe_buffer_map(state->pctx, indirect->bo, PIPE_MAP_READ, &transfer);
   map += indirect_offset;
   const VkTraceRaysIndirectCommandKHR *src = (const void *)map;

   VkTraceRaysIndirectCommand2KHR *command = lvp_push_internal_buffer(
      state, MESA_SHADER_COMPUTE, sizeof(VkTraceRaysIndirectCommand2KHR));

   *command = (VkTraceRaysIndirectCommand2KHR) {
      .raygenShaderRecordAddress = trace->raygen_shader_binding_table->deviceAddress,
      .raygenShaderRecordSize = trace->raygen_shader_binding_table->size,
      .missShaderBindingTableAddress = trace->miss_shader_binding_table->deviceAddress,
      .missShaderBindingTableSize = trace->miss_shader_binding_table->size,
      .missShaderBindingTableStride = trace->miss_shader_binding_table->stride,
      .hitShaderBindingTableAddress = trace->hit_shader_binding_table->deviceAddress,
      .hitShaderBindingTableSize = trace->hit_shader_binding_table->size,
      .hitShaderBindingTableStride = trace->hit_shader_binding_table->stride,
      .callableShaderBindingTableAddress = trace->callable_shader_binding_table->deviceAddress,
      .callableShaderBindingTableSize = trace->callable_shader_binding_table->size,
      .callableShaderBindingTableStride = trace->callable_shader_binding_table->stride,
      .width = src->width,
      .height = src->height,
      .depth = src->depth,
   };

   state->pctx->buffer_unmap(state->pctx, transfer);

   lvp_trace_rays(state, command);
}

static void
handle_trace_rays_indirect2(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct vk_cmd_trace_rays_indirect2_khr *trace = &cmd->u.trace_rays_indirect2_khr;

   size_t indirect_offset;
   VkBuffer _indirect = get_buffer(state, (void *)(uintptr_t)trace->indirect_device_address, &indirect_offset);
   VK_FROM_HANDLE(lvp_buffer, indirect, _indirect);

   struct pipe_transfer *transfer;
   const uint8_t *map = pipe_buffer_map(state->pctx, indirect->bo, PIPE_MAP_READ, &transfer);
   map += indirect_offset;
   const VkTraceRaysIndirectCommand2KHR *src = (const void *)map;

   VkTraceRaysIndirectCommand2KHR *command = lvp_push_internal_buffer(
      state, MESA_SHADER_COMPUTE, sizeof(VkTraceRaysIndirectCommand2KHR));
   *command = *src;

   state->pctx->buffer_unmap(state->pctx, transfer);

   lvp_trace_rays(state, command);
}

static void
handle_write_buffer_cp(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct lvp_cmd_write_buffer_cp *write = cmd->driver_data;

   finish_fence(state);

   memcpy((void *)(uintptr_t)write->addr, write->data, write->size);
}

static void
handle_dispatch_unaligned(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   assert(cmd->u.dispatch.group_count_y == 1);
   assert(cmd->u.dispatch.group_count_z == 1);

   uint32_t last_block_size = state->dispatch_info.block[0];

   state->dispatch_info.grid[0] = cmd->u.dispatch.group_count_x / last_block_size;
   state->dispatch_info.grid[1] = 1;
   state->dispatch_info.grid[2] = 1;
   state->dispatch_info.grid_base[0] = 0;
   state->dispatch_info.grid_base[1] = 0;
   state->dispatch_info.grid_base[2] = 0;
   state->dispatch_info.indirect = NULL;
   state->pctx->launch_grid(state->pctx, &state->dispatch_info);

   if (cmd->u.dispatch.group_count_x % last_block_size) {
      state->dispatch_info.block[0] = cmd->u.dispatch.group_count_x % last_block_size;
      state->dispatch_info.grid[0] = 1;
      state->dispatch_info.grid_base[0] = cmd->u.dispatch.group_count_x / last_block_size;
      state->pctx->launch_grid(state->pctx, &state->dispatch_info);
      state->dispatch_info.block[0] = last_block_size;
   }
}

static void
handle_fill_buffer_addr(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct lvp_cmd_fill_buffer_addr *fill = cmd->driver_data;

   finish_fence(state);

   uint32_t *dst = (void *)(uintptr_t)fill->addr;
   for (uint32_t i = 0; i < fill->size / 4; i++) {
      dst[i] = fill->data;
   }
}

static void
handle_encode_as(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   struct lvp_cmd_encode_as *encode = cmd->driver_data;

   finish_fence(state);

   lvp_encode_as(encode->dst, encode->intermediate_as_addr,
                 encode->intermediate_header_addr, encode->leaf_count,
                 encode->geometry_type);
}

static void
handle_save_state(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   state->saved.compute_shader = state->shaders[MESA_SHADER_COMPUTE];
   memcpy(state->saved.push_constants, state->push_constants, sizeof(state->push_constants));
}

static void
handle_restore_state(struct vk_cmd_queue_entry *cmd, struct rendering_state *state)
{
   if (state->saved.compute_shader)
      handle_compute_shader(state, state->saved.compute_shader);

   memcpy(state->push_constants, state->saved.push_constants, sizeof(state->push_constants));
   state->pcbuf_dirty[MESA_SHADER_COMPUTE] = true;
}

void lvp_add_enqueue_cmd_entrypoints(struct vk_device_dispatch_table *disp)
{
   struct vk_device_dispatch_table cmd_enqueue_dispatch;
   vk_device_dispatch_table_from_entrypoints(&cmd_enqueue_dispatch,
      &vk_cmd_enqueue_device_entrypoints, true);

#define ENQUEUE_CMD(CmdName) \
   assert(cmd_enqueue_dispatch.CmdName != NULL); \
   disp->CmdName = cmd_enqueue_dispatch.CmdName;

   /* This list needs to match what's in lvp_execute_cmd_buffer exactly */
   ENQUEUE_CMD(CmdBindPipeline)
   ENQUEUE_CMD(CmdSetViewport)
   ENQUEUE_CMD(CmdSetViewportWithCount)
   ENQUEUE_CMD(CmdSetScissor)
   ENQUEUE_CMD(CmdSetScissorWithCount)
   ENQUEUE_CMD(CmdSetLineWidth)
   ENQUEUE_CMD(CmdSetDepthBias)
   ENQUEUE_CMD(CmdSetBlendConstants)
   ENQUEUE_CMD(CmdSetDepthBounds)
   ENQUEUE_CMD(CmdSetStencilCompareMask)
   ENQUEUE_CMD(CmdSetStencilWriteMask)
   ENQUEUE_CMD(CmdSetStencilReference)
   ENQUEUE_CMD(CmdBindDescriptorSets2KHR)
   ENQUEUE_CMD(CmdBindIndexBuffer)
   ENQUEUE_CMD(CmdBindIndexBuffer2KHR)
   ENQUEUE_CMD(CmdBindVertexBuffers2)
   ENQUEUE_CMD(CmdDraw)
   ENQUEUE_CMD(CmdDrawMultiEXT)
   ENQUEUE_CMD(CmdDrawIndexed)
   ENQUEUE_CMD(CmdDrawIndirect)
   ENQUEUE_CMD(CmdDrawIndexedIndirect)
   ENQUEUE_CMD(CmdDrawMultiIndexedEXT)
   ENQUEUE_CMD(CmdDispatch)
   ENQUEUE_CMD(CmdDispatchBase)
   ENQUEUE_CMD(CmdDispatchIndirect)
   ENQUEUE_CMD(CmdCopyBuffer2)
   ENQUEUE_CMD(CmdCopyImage2)
   ENQUEUE_CMD(CmdBlitImage2)
   ENQUEUE_CMD(CmdCopyBufferToImage2)
   ENQUEUE_CMD(CmdCopyImageToBuffer2)
   ENQUEUE_CMD(CmdUpdateBuffer)
   ENQUEUE_CMD(CmdFillBuffer)
   ENQUEUE_CMD(CmdClearColorImage)
   ENQUEUE_CMD(CmdClearDepthStencilImage)
   ENQUEUE_CMD(CmdClearAttachments)
   ENQUEUE_CMD(CmdResolveImage2)
   ENQUEUE_CMD(CmdBeginQueryIndexedEXT)
   ENQUEUE_CMD(CmdEndQueryIndexedEXT)
   ENQUEUE_CMD(CmdBeginQuery)
   ENQUEUE_CMD(CmdEndQuery)
   ENQUEUE_CMD(CmdResetQueryPool)
   ENQUEUE_CMD(CmdCopyQueryPoolResults)
   ENQUEUE_CMD(CmdExecuteCommands)
   ENQUEUE_CMD(CmdDrawIndirectCount)
   ENQUEUE_CMD(CmdDrawIndexedIndirectCount)
   ENQUEUE_CMD(CmdBindTransformFeedbackBuffersEXT)
   ENQUEUE_CMD(CmdBeginTransformFeedbackEXT)
   ENQUEUE_CMD(CmdEndTransformFeedbackEXT)
   ENQUEUE_CMD(CmdDrawIndirectByteCountEXT)
   ENQUEUE_CMD(CmdBeginConditionalRenderingEXT)
   ENQUEUE_CMD(CmdEndConditionalRenderingEXT)
   ENQUEUE_CMD(CmdSetVertexInputEXT)
   ENQUEUE_CMD(CmdSetCullMode)
   ENQUEUE_CMD(CmdSetFrontFace)
   ENQUEUE_CMD(CmdSetPrimitiveTopology)
   ENQUEUE_CMD(CmdSetDepthTestEnable)
   ENQUEUE_CMD(CmdSetDepthWriteEnable)
   ENQUEUE_CMD(CmdSetDepthCompareOp)
   ENQUEUE_CMD(CmdSetDepthBoundsTestEnable)
   ENQUEUE_CMD(CmdSetStencilTestEnable)
   ENQUEUE_CMD(CmdSetStencilOp)
   ENQUEUE_CMD(CmdSetLineStippleEXT)
   ENQUEUE_CMD(CmdSetLineStippleKHR)
   ENQUEUE_CMD(CmdSetDepthBiasEnable)
   ENQUEUE_CMD(CmdSetLogicOpEXT)
   ENQUEUE_CMD(CmdSetPatchControlPointsEXT)
   ENQUEUE_CMD(CmdSetPrimitiveRestartEnable)
   ENQUEUE_CMD(CmdSetRasterizerDiscardEnable)
   ENQUEUE_CMD(CmdSetColorWriteEnableEXT)
   ENQUEUE_CMD(CmdBeginRendering)
   ENQUEUE_CMD(CmdEndRendering)
   ENQUEUE_CMD(CmdSetDeviceMask)
   ENQUEUE_CMD(CmdPipelineBarrier2)
   ENQUEUE_CMD(CmdResetEvent2)
   ENQUEUE_CMD(CmdSetEvent2)
   ENQUEUE_CMD(CmdWaitEvents2)
   ENQUEUE_CMD(CmdWriteTimestamp2)
   ENQUEUE_CMD(CmdPushConstants2KHR)
   ENQUEUE_CMD(CmdPushDescriptorSet2KHR)
   ENQUEUE_CMD(CmdPushDescriptorSetWithTemplate2KHR)
   ENQUEUE_CMD(CmdBindDescriptorBuffersEXT)
   ENQUEUE_CMD(CmdSetDescriptorBufferOffsets2EXT)
   ENQUEUE_CMD(CmdBindDescriptorBufferEmbeddedSamplers2EXT)

   ENQUEUE_CMD(CmdSetPolygonModeEXT)
   ENQUEUE_CMD(CmdSetTessellationDomainOriginEXT)
   ENQUEUE_CMD(CmdSetDepthClampEnableEXT)
   ENQUEUE_CMD(CmdSetDepthClipEnableEXT)
   ENQUEUE_CMD(CmdSetLogicOpEnableEXT)
   ENQUEUE_CMD(CmdSetSampleMaskEXT)
   ENQUEUE_CMD(CmdSetRasterizationSamplesEXT)
   ENQUEUE_CMD(CmdSetAlphaToCoverageEnableEXT)
   ENQUEUE_CMD(CmdSetAlphaToOneEnableEXT)
   ENQUEUE_CMD(CmdSetDepthClipNegativeOneToOneEXT)
   ENQUEUE_CMD(CmdSetLineRasterizationModeEXT)
   ENQUEUE_CMD(CmdSetLineStippleEnableEXT)
   ENQUEUE_CMD(CmdSetProvokingVertexModeEXT)
   ENQUEUE_CMD(CmdSetColorBlendEnableEXT)
   ENQUEUE_CMD(CmdSetColorBlendEquationEXT)
   ENQUEUE_CMD(CmdSetColorWriteMaskEXT)

   ENQUEUE_CMD(CmdBindShadersEXT)
   /* required for EXT_shader_object */
   ENQUEUE_CMD(CmdSetCoverageModulationModeNV)
   ENQUEUE_CMD(CmdSetCoverageModulationTableEnableNV)
   ENQUEUE_CMD(CmdSetCoverageModulationTableNV)
   ENQUEUE_CMD(CmdSetCoverageReductionModeNV)
   ENQUEUE_CMD(CmdSetCoverageToColorEnableNV)
   ENQUEUE_CMD(CmdSetCoverageToColorLocationNV)
   ENQUEUE_CMD(CmdSetRepresentativeFragmentTestEnableNV)
   ENQUEUE_CMD(CmdSetShadingRateImageEnableNV)
   ENQUEUE_CMD(CmdSetViewportSwizzleNV)
   ENQUEUE_CMD(CmdSetViewportWScalingEnableNV)
   ENQUEUE_CMD(CmdSetAttachmentFeedbackLoopEnableEXT)
   ENQUEUE_CMD(CmdDrawMeshTasksEXT)
   ENQUEUE_CMD(CmdDrawMeshTasksIndirectEXT)
   ENQUEUE_CMD(CmdDrawMeshTasksIndirectCountEXT)

   ENQUEUE_CMD(CmdBindPipelineShaderGroupNV)
   ENQUEUE_CMD(CmdPreprocessGeneratedCommandsNV)
   ENQUEUE_CMD(CmdExecuteGeneratedCommandsNV)
   ENQUEUE_CMD(CmdPreprocessGeneratedCommandsEXT)
   ENQUEUE_CMD(CmdExecuteGeneratedCommandsEXT)

#ifdef VK_ENABLE_BETA_EXTENSIONS
   ENQUEUE_CMD(CmdInitializeGraphScratchMemoryAMDX)
   ENQUEUE_CMD(CmdDispatchGraphIndirectCountAMDX)
   ENQUEUE_CMD(CmdDispatchGraphIndirectAMDX)
   ENQUEUE_CMD(CmdDispatchGraphAMDX)
#endif

   ENQUEUE_CMD(CmdSetRenderingAttachmentLocationsKHR)
   ENQUEUE_CMD(CmdSetRenderingInputAttachmentIndicesKHR)

   ENQUEUE_CMD(CmdCopyAccelerationStructureKHR)
   ENQUEUE_CMD(CmdCopyMemoryToAccelerationStructureKHR)
   ENQUEUE_CMD(CmdCopyAccelerationStructureToMemoryKHR)
   ENQUEUE_CMD(CmdBuildAccelerationStructuresIndirectKHR)
   ENQUEUE_CMD(CmdWriteAccelerationStructuresPropertiesKHR)

   ENQUEUE_CMD(CmdSetRayTracingPipelineStackSizeKHR)
   ENQUEUE_CMD(CmdTraceRaysIndirect2KHR)
   ENQUEUE_CMD(CmdTraceRaysIndirectKHR)
   ENQUEUE_CMD(CmdTraceRaysKHR)

   ENQUEUE_CMD(CmdSetDepthBias2EXT)

#undef ENQUEUE_CMD
}

static void lvp_execute_cmd_buffer(struct list_head *cmds,
                                   struct rendering_state *state, bool print_cmds)
{
   struct vk_cmd_queue_entry *cmd;
   bool did_flush = false;

   LIST_FOR_EACH_ENTRY(cmd, cmds, cmd_link) {
      if (cmd->type >= VK_CMD_TYPE_COUNT) {
         uint32_t type = cmd->type;
         if (type == LVP_CMD_WRITE_BUFFER_CP) {
            handle_write_buffer_cp(cmd, state);
         } else if (type == LVP_CMD_DISPATCH_UNALIGNED) {
            emit_compute_state(state);
            handle_dispatch_unaligned(cmd, state);
         } else if (type == LVP_CMD_FILL_BUFFER_ADDR) {
            handle_fill_buffer_addr(cmd, state);
         } else if (type == LVP_CMD_ENCODE_AS) {
            handle_encode_as(cmd, state);
         } else if (type == LVP_CMD_SAVE_STATE) {
            handle_save_state(cmd, state);
         } else if (type == LVP_CMD_RESTORE_STATE) {
            handle_restore_state(cmd, state);
         }
         continue;
      }

      if (print_cmds)
         fprintf(stderr, "%s\n", vk_cmd_queue_type_names[cmd->type]);
      switch ((unsigned)cmd->type) {
      case VK_CMD_BIND_PIPELINE:
         handle_pipeline(cmd, state);
         break;
      case VK_CMD_SET_VIEWPORT:
         handle_set_viewport(cmd, state);
         break;
      case VK_CMD_SET_VIEWPORT_WITH_COUNT:
         handle_set_viewport_with_count(cmd, state);
         break;
      case VK_CMD_SET_SCISSOR:
         handle_set_scissor(cmd, state);
         break;
      case VK_CMD_SET_SCISSOR_WITH_COUNT:
         handle_set_scissor_with_count(cmd, state);
         break;
      case VK_CMD_SET_LINE_WIDTH:
         handle_set_line_width(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_BIAS:
         handle_set_depth_bias(cmd, state);
         break;
      case VK_CMD_SET_BLEND_CONSTANTS:
         handle_set_blend_constants(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_BOUNDS:
         handle_set_depth_bounds(cmd, state);
         break;
      case VK_CMD_SET_STENCIL_COMPARE_MASK:
         handle_set_stencil_compare_mask(cmd, state);
         break;
      case VK_CMD_SET_STENCIL_WRITE_MASK:
         handle_set_stencil_write_mask(cmd, state);
         break;
      case VK_CMD_SET_STENCIL_REFERENCE:
         handle_set_stencil_reference(cmd, state);
         break;
      case VK_CMD_BIND_DESCRIPTOR_SETS2:
         handle_descriptor_sets_cmd(cmd, state);
         break;
      case VK_CMD_BIND_INDEX_BUFFER:
         handle_index_buffer(cmd, state);
         break;
      case VK_CMD_BIND_INDEX_BUFFER2:
         handle_index_buffer2(cmd, state);
         break;
      case VK_CMD_BIND_VERTEX_BUFFERS2:
         handle_vertex_buffers2(cmd, state);
         break;
      case VK_CMD_DRAW:
         emit_state(state);
         handle_draw(cmd, state);
         break;
      case VK_CMD_DRAW_MULTI_EXT:
         emit_state(state);
         handle_draw_multi(cmd, state);
         break;
      case VK_CMD_DRAW_INDEXED:
         emit_state(state);
         handle_draw_indexed(cmd, state);
         break;
      case VK_CMD_DRAW_INDIRECT:
         emit_state(state);
         handle_draw_indirect(cmd, state, false);
         break;
      case VK_CMD_DRAW_INDEXED_INDIRECT:
         emit_state(state);
         handle_draw_indirect(cmd, state, true);
         break;
      case VK_CMD_DRAW_MULTI_INDEXED_EXT:
         emit_state(state);
         handle_draw_multi_indexed(cmd, state);
         break;
      case VK_CMD_DISPATCH:
         emit_compute_state(state);
         handle_dispatch(cmd, state);
         break;
      case VK_CMD_DISPATCH_BASE:
         emit_compute_state(state);
         handle_dispatch_base(cmd, state);
         break;
      case VK_CMD_DISPATCH_INDIRECT:
         emit_compute_state(state);
         handle_dispatch_indirect(cmd, state);
         break;
      case VK_CMD_COPY_BUFFER2:
         handle_copy_buffer(cmd, state);
         break;
      case VK_CMD_COPY_IMAGE2:
         handle_copy_image(cmd, state);
         break;
      case VK_CMD_BLIT_IMAGE2:
         handle_blit_image(cmd, state);
         break;
      case VK_CMD_COPY_BUFFER_TO_IMAGE2:
         handle_copy_buffer_to_image(cmd, state);
         break;
      case VK_CMD_COPY_IMAGE_TO_BUFFER2:
         handle_copy_image_to_buffer2(cmd, state);
         break;
      case VK_CMD_UPDATE_BUFFER:
         handle_update_buffer(cmd, state);
         break;
      case VK_CMD_FILL_BUFFER:
         handle_fill_buffer(cmd, state);
         break;
      case VK_CMD_CLEAR_COLOR_IMAGE:
         handle_clear_color_image(cmd, state);
         break;
      case VK_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
         handle_clear_ds_image(cmd, state);
         break;
      case VK_CMD_CLEAR_ATTACHMENTS:
         handle_clear_attachments(cmd, state);
         break;
      case VK_CMD_RESOLVE_IMAGE2:
         handle_resolve_image(cmd, state);
         break;
      case VK_CMD_PIPELINE_BARRIER2:
         /* flushes are actually stalls, so multiple flushes are redundant */
         if (did_flush)
            continue;
         handle_pipeline_barrier(cmd, state);
         did_flush = true;
         continue;
      case VK_CMD_BEGIN_QUERY_INDEXED_EXT:
         handle_begin_query_indexed_ext(cmd, state);
         break;
      case VK_CMD_END_QUERY_INDEXED_EXT:
         handle_end_query_indexed_ext(cmd, state);
         break;
      case VK_CMD_BEGIN_QUERY:
         handle_begin_query(cmd, state);
         break;
      case VK_CMD_END_QUERY:
         handle_end_query(cmd, state);
         break;
      case VK_CMD_RESET_QUERY_POOL:
         handle_reset_query_pool(cmd, state);
         break;
      case VK_CMD_COPY_QUERY_POOL_RESULTS:
         handle_copy_query_pool_results(cmd, state);
         break;
      case VK_CMD_PUSH_CONSTANTS2:
         handle_push_constants(cmd, state);
         break;
      case VK_CMD_EXECUTE_COMMANDS:
         handle_execute_commands(cmd, state, print_cmds);
         break;
      case VK_CMD_DRAW_INDIRECT_COUNT:
         emit_state(state);
         handle_draw_indirect_count(cmd, state, false);
         break;
      case VK_CMD_DRAW_INDEXED_INDIRECT_COUNT:
         emit_state(state);
         handle_draw_indirect_count(cmd, state, true);
         break;
      case VK_CMD_PUSH_DESCRIPTOR_SET2:
         handle_push_descriptor_set(cmd, state);
         break;
      case VK_CMD_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE2:
         handle_push_descriptor_set_with_template(cmd, state);
         break;
      case VK_CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS_EXT:
         handle_bind_transform_feedback_buffers(cmd, state);
         break;
      case VK_CMD_BEGIN_TRANSFORM_FEEDBACK_EXT:
         handle_begin_transform_feedback(cmd, state);
         break;
      case VK_CMD_END_TRANSFORM_FEEDBACK_EXT:
         handle_end_transform_feedback(cmd, state);
         break;
      case VK_CMD_DRAW_INDIRECT_BYTE_COUNT_EXT:
         emit_state(state);
         handle_draw_indirect_byte_count(cmd, state);
         break;
      case VK_CMD_BEGIN_CONDITIONAL_RENDERING_EXT:
         handle_begin_conditional_rendering(cmd, state);
         break;
      case VK_CMD_END_CONDITIONAL_RENDERING_EXT:
         handle_end_conditional_rendering(state);
         break;
      case VK_CMD_SET_VERTEX_INPUT_EXT:
         handle_set_vertex_input(cmd, state);
         break;
      case VK_CMD_SET_CULL_MODE:
         handle_set_cull_mode(cmd, state);
         break;
      case VK_CMD_SET_FRONT_FACE:
         handle_set_front_face(cmd, state);
         break;
      case VK_CMD_SET_PRIMITIVE_TOPOLOGY:
         handle_set_primitive_topology(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_TEST_ENABLE:
         handle_set_depth_test_enable(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_WRITE_ENABLE:
         handle_set_depth_write_enable(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_COMPARE_OP:
         handle_set_depth_compare_op(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_BOUNDS_TEST_ENABLE:
         handle_set_depth_bounds_test_enable(cmd, state);
         break;
      case VK_CMD_SET_STENCIL_TEST_ENABLE:
         handle_set_stencil_test_enable(cmd, state);
         break;
      case VK_CMD_SET_STENCIL_OP:
         handle_set_stencil_op(cmd, state);
         break;
      case VK_CMD_SET_LINE_STIPPLE:
         handle_set_line_stipple(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_BIAS_ENABLE:
         handle_set_depth_bias_enable(cmd, state);
         break;
      case VK_CMD_SET_LOGIC_OP_EXT:
         handle_set_logic_op(cmd, state);
         break;
      case VK_CMD_SET_PATCH_CONTROL_POINTS_EXT:
         handle_set_patch_control_points(cmd, state);
         break;
      case VK_CMD_SET_PRIMITIVE_RESTART_ENABLE:
         handle_set_primitive_restart_enable(cmd, state);
         break;
      case VK_CMD_SET_RASTERIZER_DISCARD_ENABLE:
         handle_set_rasterizer_discard_enable(cmd, state);
         break;
      case VK_CMD_SET_COLOR_WRITE_ENABLE_EXT:
         handle_set_color_write_enable(cmd, state);
         break;
      case VK_CMD_BEGIN_RENDERING:
         handle_begin_rendering(cmd, state);
         break;
      case VK_CMD_END_RENDERING:
         handle_end_rendering(cmd, state);
         break;
      case VK_CMD_SET_DEVICE_MASK:
         /* no-op */
         break;
      case VK_CMD_RESET_EVENT2:
         handle_event_reset2(cmd, state);
         break;
      case VK_CMD_SET_EVENT2:
         handle_event_set2(cmd, state);
         break;
      case VK_CMD_WAIT_EVENTS2:
         handle_wait_events2(cmd, state);
         break;
      case VK_CMD_WRITE_TIMESTAMP2:
         handle_write_timestamp2(cmd, state);
         break;
      case VK_CMD_SET_POLYGON_MODE_EXT:
         handle_set_polygon_mode(cmd, state);
         break;
      case VK_CMD_SET_TESSELLATION_DOMAIN_ORIGIN_EXT:
         handle_set_tessellation_domain_origin(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_CLAMP_ENABLE_EXT:
         handle_set_depth_clamp_enable(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_CLIP_ENABLE_EXT:
         handle_set_depth_clip_enable(cmd, state);
         break;
      case VK_CMD_SET_LOGIC_OP_ENABLE_EXT:
         handle_set_logic_op_enable(cmd, state);
         break;
      case VK_CMD_SET_SAMPLE_MASK_EXT:
         handle_set_sample_mask(cmd, state);
         break;
      case VK_CMD_SET_RASTERIZATION_SAMPLES_EXT:
         handle_set_samples(cmd, state);
         break;
      case VK_CMD_SET_ALPHA_TO_COVERAGE_ENABLE_EXT:
         handle_set_alpha_to_coverage(cmd, state);
         break;
      case VK_CMD_SET_ALPHA_TO_ONE_ENABLE_EXT:
         handle_set_alpha_to_one(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT:
         handle_set_halfz(cmd, state);
         break;
      case VK_CMD_SET_LINE_RASTERIZATION_MODE_EXT:
         handle_set_line_rasterization_mode(cmd, state);
         break;
      case VK_CMD_SET_LINE_STIPPLE_ENABLE_EXT:
         handle_set_line_stipple_enable(cmd, state);
         break;
      case VK_CMD_SET_PROVOKING_VERTEX_MODE_EXT:
         handle_set_provoking_vertex_mode(cmd, state);
         break;
      case VK_CMD_SET_COLOR_BLEND_ENABLE_EXT:
         handle_set_color_blend_enable(cmd, state);
         break;
      case VK_CMD_SET_COLOR_WRITE_MASK_EXT:
         handle_set_color_write_mask(cmd, state);
         break;
      case VK_CMD_SET_COLOR_BLEND_EQUATION_EXT:
         handle_set_color_blend_equation(cmd, state);
         break;
      case VK_CMD_BIND_SHADERS_EXT:
         handle_shaders(cmd, state);
         break;
      case VK_CMD_SET_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT:
         break;
      case VK_CMD_DRAW_MESH_TASKS_EXT:
         emit_state(state);
         handle_draw_mesh_tasks(cmd, state);
         break;
      case VK_CMD_DRAW_MESH_TASKS_INDIRECT_EXT:
         emit_state(state);
         handle_draw_mesh_tasks_indirect(cmd, state);
         break;
      case VK_CMD_DRAW_MESH_TASKS_INDIRECT_COUNT_EXT:
         emit_state(state);
         handle_draw_mesh_tasks_indirect_count(cmd, state);
         break;
      case VK_CMD_PREPROCESS_GENERATED_COMMANDS_EXT:
         handle_preprocess_generated_commands_ext(cmd, state, print_cmds);
         break;
      case VK_CMD_EXECUTE_GENERATED_COMMANDS_EXT:
         handle_execute_generated_commands_ext(cmd, state, print_cmds);
         break;
      case VK_CMD_BIND_DESCRIPTOR_BUFFERS_EXT:
         handle_descriptor_buffers(cmd, state);
         break;
      case VK_CMD_SET_DESCRIPTOR_BUFFER_OFFSETS2_EXT:
         handle_descriptor_buffer_offsets(cmd, state);
         break;
      case VK_CMD_BIND_DESCRIPTOR_BUFFER_EMBEDDED_SAMPLERS2_EXT:
         handle_descriptor_buffer_embedded_samplers(cmd, state);
         break;
#ifdef VK_ENABLE_BETA_EXTENSIONS
      case VK_CMD_INITIALIZE_GRAPH_SCRATCH_MEMORY_AMDX:
         break;
      case VK_CMD_DISPATCH_GRAPH_INDIRECT_COUNT_AMDX:
         break;
      case VK_CMD_DISPATCH_GRAPH_INDIRECT_AMDX:
         break;
      case VK_CMD_DISPATCH_GRAPH_AMDX:
         handle_dispatch_graph(cmd, state);
         break;
#endif
      case VK_CMD_SET_RENDERING_ATTACHMENT_LOCATIONS:
         handle_rendering_attachment_locations(cmd, state);
         break;
      case VK_CMD_SET_RENDERING_INPUT_ATTACHMENT_INDICES:
         handle_rendering_input_attachment_indices(cmd, state);
         break;
      case VK_CMD_COPY_ACCELERATION_STRUCTURE_KHR:
         handle_copy_acceleration_structure(cmd, state);
         break;
      case VK_CMD_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_KHR:
         handle_copy_memory_to_acceleration_structure(cmd, state);
         break;
      case VK_CMD_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_KHR:
         handle_copy_acceleration_structure_to_memory(cmd, state);
         break;
      case VK_CMD_BUILD_ACCELERATION_STRUCTURES_INDIRECT_KHR:
         break;
      case VK_CMD_WRITE_ACCELERATION_STRUCTURES_PROPERTIES_KHR:
         handle_write_acceleration_structures_properties(cmd, state);
         break;
      case VK_CMD_SET_RAY_TRACING_PIPELINE_STACK_SIZE_KHR:
         break;
      case VK_CMD_TRACE_RAYS_INDIRECT2_KHR:
         handle_trace_rays_indirect2(cmd, state);
         break;
      case VK_CMD_TRACE_RAYS_INDIRECT_KHR:
         handle_trace_rays_indirect(cmd, state);
         break;
      case VK_CMD_TRACE_RAYS_KHR:
         handle_trace_rays(cmd, state);
         break;
      case VK_CMD_SET_DEPTH_BIAS2_EXT:
         handle_set_depth_bias2(cmd, state);
         break;
      default:
         fprintf(stderr, "Unsupported command %s\n", vk_cmd_queue_type_names[cmd->type]);
         UNREACHABLE("Unsupported command");
         break;
      }
      did_flush = false;
      if (!cmd->cmd_link.next)
         break;
   }
}

VkResult lvp_execute_cmds(struct lvp_device *device,
                          struct lvp_queue *queue,
                          struct lvp_cmd_buffer *cmd_buffer)
{
   struct rendering_state *state = queue->state;
   memset(state, 0, sizeof(*state));
   state->pctx = queue->ctx;
   state->device = device;
   state->uploader = queue->uploader;
   state->cso = queue->cso;
   state->blend_dirty = true;
   state->dsa_dirty = true;
   state->rs_dirty = true;
   state->vp_dirty = true;
   state->rs_state.point_line_tri_clip = true;
   state->rs_state.unclamped_fragment_depth_values = device->vk.enabled_extensions.EXT_depth_range_unrestricted;
   state->sample_mask_dirty = true;
   state->min_samples_dirty = true;
   state->sample_mask = UINT32_MAX;
   state->poison_mem = device->poison_mem;
   util_dynarray_init(&state->push_desc_sets, NULL);
   util_dynarray_init(&state->internal_buffers, NULL);

   /* default values */
   state->min_sample_shading = 1;
   state->num_viewports = 1;
   state->num_scissors = 1;
   state->rs_state.line_width = 1.0;
   state->rs_state.flatshade_first = true;
   state->rs_state.clip_halfz = true;
   state->rs_state.front_ccw = true;
   state->rs_state.point_size_per_vertex = true;
   state->rs_state.point_quad_rasterization = true;
   state->rs_state.half_pixel_center = true;
   state->rs_state.scissor = true;
   state->rs_state.no_ms_sample_mask_out = true;
   state->blend_state.independent_blend_enable = true;

   state->index_size = 4;
   state->index_buffer_size = sizeof(uint32_t);
   state->index_buffer = state->device->zero_buffer;

   /* create a gallium context */
   lvp_execute_cmd_buffer(&cmd_buffer->vk.cmd_queue.cmds, state, device->print_cmds);

   state->start_vb = -1;
   state->num_vb = 0;
   cso_unbind_context(queue->cso);
   for (unsigned i = 0; i < ARRAY_SIZE(state->so_targets); i++) {
      if (state->so_targets[i]) {
         state->pctx->stream_output_target_destroy(state->pctx, state->so_targets[i]);
      }
   }

   finish_fence(state);

   util_dynarray_foreach (&state->push_desc_sets, struct lvp_descriptor_set *, set)
      lvp_descriptor_set_destroy(device, *set);

   util_dynarray_fini(&state->push_desc_sets);

   util_dynarray_foreach (&state->internal_buffers, struct pipe_resource *, buffer)
      pipe_resource_reference(buffer, NULL);

   util_dynarray_fini(&state->internal_buffers);

   for (unsigned i = 0; i < ARRAY_SIZE(state->desc_buffers); i++)
      pipe_resource_reference(&state->desc_buffers[i], NULL);

   return VK_SUCCESS;
}

size_t
lvp_get_rendering_state_size(void)
{
   return sizeof(struct rendering_state);
}
