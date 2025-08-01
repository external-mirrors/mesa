/*
 * Copyright © 2016 Intel Corporation
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

/*
 * NOTE: The header can be included multiple times, from the same file.
 */

/*
 * Gen-specific function declarations.  This header must *not* be included
 * directly.  Instead, it is included multiple times by anv_private.h.
 *
 * In this header file, the usual genx() macro is available.
 */

#ifndef ANV_PRIVATE_H
#error This file is included by means other than anv_private.h
#endif

struct intel_sample_positions;
struct intel_urb_config;
struct anv_async_submit;
struct anv_embedded_sampler;
struct anv_pipeline_embedded_sampler_binding;
struct anv_trtt_bind;

typedef struct nir_builder nir_builder;
typedef struct nir_shader nir_shader;

void genX(init_physical_device_state)(struct anv_physical_device *device);

VkResult genX(init_device_state)(struct anv_device *device);

void genX(init_cps_device_state)(struct anv_device *device);

uint32_t genX(call_internal_shader)(nir_builder *b,
                                    enum anv_internal_kernel_name shader_name);

void
genX(set_fast_clear_state)(struct anv_cmd_buffer *cmd_buffer,
                           const struct anv_image *image,
                           const enum isl_format format,
                           const struct isl_swizzle swizzle,
                           union isl_color_value clear_color);

void
genX(cmd_buffer_load_clear_color)(struct anv_cmd_buffer *cmd_buffer,
                                  struct anv_state surface_state,
                                  const struct anv_image_view *iview);

void genX(cmd_buffer_emit_bt_pool_base_address)(struct anv_cmd_buffer *cmd_buffer);

void genX(cmd_buffer_emit_state_base_address)(struct anv_cmd_buffer *cmd_buffer);

void genX(cmd_buffer_apply_pipe_flushes)(struct anv_cmd_buffer *cmd_buffer);

void
genX(cmd_buffer_update_color_aux_op)(struct anv_cmd_buffer *cmd_buffer,
                                     enum isl_aux_op aux_op);

void genX(cmd_buffer_emit_gfx12_depth_wa)(struct anv_cmd_buffer *cmd_buffer,
                                          const struct isl_surf *surf);

void genX(cmd_buffer_set_binding_for_gfx8_vb_flush)(struct anv_cmd_buffer *cmd_buffer,
                                                    int vb_index,
                                                    struct anv_address vb_address,
                                                    uint32_t vb_size);
void genX(cmd_buffer_update_dirty_vbs_for_gfx8_vb_flush)(struct anv_cmd_buffer *cmd_buffer,
                                                         uint32_t access_type,
                                                         uint64_t vb_used);

void genX(cmd_buffer_emit_hashing_mode)(struct anv_cmd_buffer *cmd_buffer,
                                        unsigned width, unsigned height,
                                        unsigned scale);

void genX(urb_workaround)(struct anv_cmd_buffer *cmd_buffer,
                          const struct intel_urb_config *urb_cfg);

void genX(flush_pipeline_select_3d)(struct anv_cmd_buffer *cmd_buffer);
void genX(flush_pipeline_select_gpgpu)(struct anv_cmd_buffer *cmd_buffer);
void genX(emit_pipeline_select)(struct anv_batch *batch, uint32_t pipeline,
                                const struct anv_device *device);

void genX(apply_task_urb_workaround)(struct anv_cmd_buffer *cmd_buffer);

void genX(batch_emit_pipeline_vertex_input)(struct anv_batch *batch,
                                            struct anv_device *device,
                                            struct anv_graphics_pipeline *pipeline,
                                            const struct vk_vertex_input_state *vi);

enum anv_pipe_bits
genX(emit_apply_pipe_flushes)(struct anv_batch *batch,
                              struct anv_device *device,
                              uint32_t current_pipeline,
                              enum anv_pipe_bits bits,
                              enum anv_pipe_bits *emitted_flush_bits);
void
genX(invalidate_aux_map)(struct anv_batch *batch,
                         struct anv_device *device,
                         enum intel_engine_class engine_class,
                         enum anv_pipe_bits bits);

#if INTEL_WA_14018283232_GFX_VER
void genX(batch_emit_wa_14018283232)(struct anv_batch *batch);

static inline void
genX(cmd_buffer_ensure_wa_14018283232)(struct anv_cmd_buffer *cmd_buffer,
                                       bool toggle)
{
   struct anv_gfx_dynamic_state *hw_state =
      &cmd_buffer->state.gfx.dyn_state;
   if (intel_needs_workaround(cmd_buffer->device->info, 14018283232) &&
       hw_state->wa_14018283232_toggle != toggle) {
      hw_state->wa_14018283232_toggle = toggle;
      BITSET_SET(hw_state->dirty, ANV_GFX_STATE_WA_14018283232);
      genX(batch_emit_wa_14018283232)(&cmd_buffer->batch);
   }
}
#endif

static inline bool
genX(need_wa_16014912113)(const struct intel_urb_config *prev_urb_cfg,
                          const struct intel_urb_config *next_urb_cfg)
{
#if INTEL_NEEDS_WA_16014912113
   /* When the config change and there was at a previous config. */
   return intel_urb_setup_changed(prev_urb_cfg, next_urb_cfg,
                                  MESA_SHADER_TESS_EVAL) &&
          prev_urb_cfg->size[0] != 0;
#else
   return false;
#endif
}

void genX(batch_emit_wa_16014912113)(struct anv_batch *batch,
                                     const struct intel_urb_config *urb_cfg);

static inline bool
genX(cmd_buffer_set_coarse_pixel_active)(struct anv_cmd_buffer *cmd_buffer,
                                         enum anv_coarse_pixel_state state)
{
#if INTEL_WA_18038825448_GFX_VER
   struct anv_cmd_graphics_state *gfx =
      &cmd_buffer->state.gfx;
   if (intel_needs_workaround(cmd_buffer->device->info, 18038825448) &&
       gfx->dyn_state.coarse_state != state) {
      gfx->dyn_state.coarse_state = state;
      BITSET_SET(gfx->dyn_state.dirty, ANV_GFX_STATE_COARSE_STATE);
      return true;
   }
   return false;
#else
   return false;
#endif
}

void genX(emit_so_memcpy_init)(struct anv_memcpy_state *state,
                               struct anv_device *device,
                               struct anv_cmd_buffer *cmd_buffer,
                               struct anv_batch *batch);

void genX(emit_so_memcpy_fini)(struct anv_memcpy_state *state);

void genX(emit_so_memcpy_end)(struct anv_memcpy_state *state);

void genX(emit_so_memcpy)(struct anv_memcpy_state *state,
                          struct anv_address dst, struct anv_address src,
                          uint32_t size);

void genX(emit_l3_config)(struct anv_batch *batch,
                          const struct anv_device *device,
                          const struct intel_l3_config *cfg);

void genX(cmd_buffer_config_l3)(struct anv_cmd_buffer *cmd_buffer,
                                const struct intel_l3_config *cfg);

void genX(flush_descriptor_buffers)(struct anv_cmd_buffer *cmd_buffer,
                                    struct anv_cmd_pipeline_state *pipe_state);

uint32_t
genX(cmd_buffer_flush_descriptor_sets)(struct anv_cmd_buffer *cmd_buffer,
                                       struct anv_cmd_pipeline_state *pipe_state,
                                       const VkShaderStageFlags dirty,
                                       const struct anv_shader_bin **shaders,
                                       uint32_t num_shaders);

void genX(cmd_buffer_flush_gfx_hw_state)(struct anv_cmd_buffer *cmd_buffer);

void genX(cmd_buffer_flush_gfx_runtime_state)(struct anv_cmd_buffer *cmd_buffer);

void genX(cmd_buffer_flush_gfx_hw_state)(struct anv_cmd_buffer *cmd_buffer);

void genX(cmd_buffer_flush_gfx_state)(struct anv_cmd_buffer *cmd_buffer);

void genX(cmd_buffer_flush_compute_state)(struct anv_cmd_buffer *cmd_buffer);

void genX(cmd_buffer_enable_pma_fix)(struct anv_cmd_buffer *cmd_buffer,
                                     bool enable);

void genX(cmd_buffer_mark_image_written)(struct anv_cmd_buffer *cmd_buffer,
                                         const struct anv_image *image,
                                         VkImageAspectFlagBits aspect,
                                         enum isl_aux_usage aux_usage,
                                         uint32_t level,
                                         uint32_t base_layer,
                                         uint32_t layer_count);

void genX(cmd_emit_conditional_render_predicate)(struct anv_cmd_buffer *cmd_buffer);

struct anv_address genX(cmd_buffer_ray_query_globals)(struct anv_cmd_buffer *cmd_buffer);

void genX(cmd_buffer_ensure_cfe_state)(struct anv_cmd_buffer *cmd_buffer,
                                       uint32_t total_scratch);

void
genX(emit_urb_setup)(struct anv_batch *batch,
                     const struct anv_device *device,
                     const struct intel_urb_config *urb_cfg);

void genX(emit_sample_pattern)(struct anv_batch *batch,
                               const struct vk_sample_locations_state *sl);

void genX(cmd_buffer_so_memcpy)(struct anv_cmd_buffer *cmd_buffer,
                                struct anv_address dst, struct anv_address src,
                                uint32_t size);

void genX(blorp_init_dynamic_states)(struct blorp_context *context);

void genX(blorp_exec)(struct blorp_batch *batch,
                      const struct blorp_params *params);

void genX(batch_emit_secondary_call)(struct anv_batch *batch,
                                     struct anv_device *device,
                                     struct anv_address secondary_addr,
                                     struct anv_address secondary_return_addr);

void *genX(batch_emit_return)(struct anv_batch *batch);

void genX(cmd_emit_timestamp)(struct anv_batch *batch,
                              struct anv_device *device,
                              struct anv_address addr,
                              enum anv_timestamp_capture_type type,
                              void *data);

void genX(cmd_capture_data)(struct anv_batch *batch,
                            struct anv_device *device,
                            struct anv_address dst_addr,
                            struct anv_address src_addr,
                            uint32_t size_B);

void
genX(batch_emit_post_3dprimitive_was)(struct anv_batch *batch,
                                      const struct anv_device *device,
                                      uint32_t primitive_topology,
                                      uint32_t vertex_count);

void genX(batch_emit_fast_color_dummy_blit)(struct anv_batch *batch,
                                            struct anv_device *device);

void
genX(graphics_pipeline_emit)(struct anv_graphics_pipeline *pipeline,
                             const struct vk_graphics_pipeline_state *state);

void
genX(compute_pipeline_emit)(struct anv_compute_pipeline *pipeline);

void
genX(ray_tracing_pipeline_emit)(struct anv_ray_tracing_pipeline *pipeline);

#if GFX_VERx10 >= 300
#define anv_shader_bin_get_handler(bin, local_arg_offset) ({         \
   assert((local_arg_offset) % 8 == 0);                              \
   const struct brw_bs_prog_data *prog_data =                        \
      brw_bs_prog_data_const(bin->prog_data);                        \
   assert(prog_data->simd_size == 16);                               \
                                                                     \
   (struct GENX(CALL_STACK_HANDLER)) {                               \
      .OffsetToLocalArguments = (local_arg_offset) / 8,              \
      .BindlessShaderDispatchMode = RT_SIMD16,                       \
      .KernelStartPointer = bin->kernel.offset,                      \
      .RegistersPerThread = ptl_register_blocks(prog_data->base.grf_used), \
   };                                                                \
})
#endif

#if GFX_VERx10 >= 300
#define anv_shader_bin_get_bsr(bin, local_arg_offset) ({             \
   assert((local_arg_offset) % 8 == 0);                              \
   const struct brw_bs_prog_data *prog_data =                        \
      brw_bs_prog_data_const(bin->prog_data);                        \
   assert(prog_data->simd_size == 16);                               \
                                                                     \
   (struct GENX(BINDLESS_SHADER_RECORD)) {                           \
      .OffsetToLocalArguments = (local_arg_offset) / 8,              \
      .BindlessShaderDispatchMode = RT_SIMD16,                       \
      .KernelStartPointer = bin->kernel.offset,                      \
      .RegistersPerThread = ptl_register_blocks(prog_data->base.grf_used), \
   };                                                                \
})
#else
#define anv_shader_bin_get_bsr(bin, local_arg_offset) ({             \
   assert((local_arg_offset) % 8 == 0);                              \
   const struct brw_bs_prog_data *prog_data =                        \
      brw_bs_prog_data_const(bin->prog_data);                        \
   assert(prog_data->simd_size == 8 || prog_data->simd_size == 16);  \
                                                                     \
   (struct GENX(BINDLESS_SHADER_RECORD)) {                           \
      .OffsetToLocalArguments = (local_arg_offset) / 8,              \
      .BindlessShaderDispatchMode =                                  \
         prog_data->simd_size == 16 ? RT_SIMD16 : RT_SIMD8,          \
      .KernelStartPointer = bin->kernel.offset,                      \
   };                                                                \
})
#endif

void
genX(batch_set_preemption)(struct anv_batch *batch,
                           struct anv_device *device,
                           uint32_t current_pipeline,
                           bool value);

void
genX(cmd_buffer_set_preemption)(struct anv_cmd_buffer *cmd_buffer, bool value);

void
genX(batch_emit_pipe_control)(struct anv_batch *batch,
                              const struct intel_device_info *devinfo,
                              uint32_t current_pipeline,
                              enum anv_pipe_bits bits,
                              const char *reason);

void
genX(batch_emit_pipe_control_write)(struct anv_batch *batch,
                                    const struct intel_device_info *devinfo,
                                    uint32_t current_pipeline,
                                    uint32_t post_sync_op,
                                    struct anv_address address,
                                    uint32_t imm_data,
                                    enum anv_pipe_bits bits,
                                    const char *reason);

#define genx_batch_emit_pipe_control(a, b, c, d) \
genX(batch_emit_pipe_control) (a, b, c, d, __func__)

#define genx_batch_emit_pipe_control_write(a, b, c, d, e, f, g) \
genX(batch_emit_pipe_control_write) (a, b, c, d, e, f, g, __func__)

void genX(batch_emit_breakpoint)(struct anv_batch *batch,
                                 struct anv_device *device,
                                 bool emit_before_draw);

static inline void
genX(emit_breakpoint)(struct anv_batch *batch,
                      struct anv_device *device,
                      bool emit_before_draw_or_dispatch)
{
   if (INTEL_DEBUG(DEBUG_DRAW_BKP) || INTEL_DEBUG(DEBUG_DISPATCH_BKP))
      genX(batch_emit_breakpoint)(batch, device, emit_before_draw_or_dispatch);
}

void
genX(cmd_buffer_begin_companion)(struct anv_cmd_buffer *buffer,
                                 VkCommandBufferLevel level);

struct anv_state
genX(cmd_buffer_begin_companion_rcs_syncpoint)(struct anv_cmd_buffer *cmd_buffer);

void
genX(cmd_buffer_end_companion_rcs_syncpoint)(struct anv_cmd_buffer *cmd_buffer,
                                             struct anv_state syncpoint);
void
genX(cmd_write_buffer_cp)(struct anv_cmd_buffer *cmd_buffer,
                          VkDeviceAddress dstAddr,
                          void *data, uint32_t size);

void
genX(emit_simple_shader_init)(struct anv_simple_shader *state);

void
genX(emit_simple_shader_dispatch)(struct anv_simple_shader *state,
                                  uint32_t num_threads,
                                  struct anv_state push_state);

struct anv_state
genX(simple_shader_alloc_push)(struct anv_simple_shader *state, uint32_t size);

struct anv_address
genX(simple_shader_push_state_address)(struct anv_simple_shader *state,
                                       struct anv_state push_state);

void
genX(emit_simple_shader_end)(struct anv_simple_shader *state);

VkResult genX(init_trtt_context_state)(struct anv_async_submit *submit);

void genX(write_trtt_entries)(struct anv_async_submit *submit,
                              struct anv_trtt_bind *l3l2_binds,
                              uint32_t n_l3l2_binds,
                              struct anv_trtt_bind *l1_binds,
                              uint32_t n_l1_binds);

void genX(async_submit_end)(struct anv_async_submit *submit);

void
genX(cmd_buffer_emit_push_descriptor_buffer_surface)(struct anv_cmd_buffer *cmd_buffer,
                                                     struct anv_descriptor_set *set);

void
genX(cmd_buffer_emit_push_descriptor_surfaces)(struct anv_cmd_buffer *cmd_buffer,
                                               struct anv_descriptor_set *set);

static inline VkShaderStageFlags
genX(cmd_buffer_flush_push_descriptors)(struct anv_cmd_buffer *cmd_buffer,
                                        struct anv_cmd_pipeline_state *state)
{
   if (state->push_buffer_stages == 0 && state->push_descriptor_stages == 0)
      return 0;

   assert(state->push_descriptor_index != UINT8_MAX);
   struct anv_descriptor_set *set =
      state->descriptors[state->push_descriptor_index];
   assert(set->is_push);

   const VkShaderStageFlags push_buffer_dirty =
      cmd_buffer->state.push_descriptors_dirty & state->push_buffer_stages;
   if (push_buffer_dirty) {
      if (set->desc_surface_state.map == NULL)
         genX(cmd_buffer_emit_push_descriptor_buffer_surface)(cmd_buffer, set);

      /* Force the next push descriptor update to allocate a new descriptor set. */
      state->push_descriptor.set_used_on_gpu = true;
   }

   const VkShaderStageFlags push_descriptor_dirty =
      cmd_buffer->state.push_descriptors_dirty & state->push_descriptor_stages;
   if (push_descriptor_dirty) {
      genX(cmd_buffer_emit_push_descriptor_surfaces)(cmd_buffer, set);

      /* Force the next push descriptor update to allocate a new descriptor set. */
      state->push_descriptor.set_used_on_gpu = true;
   }

   /* Clear the dirty stages now that we've generated the surface states for
    * them.
    */
   cmd_buffer->state.push_descriptors_dirty &=
      ~(push_descriptor_dirty | push_buffer_dirty);

   /* Return the binding table stages that need to be updated */
   return push_buffer_dirty | push_descriptor_dirty;
}

void genX(emit_embedded_sampler)(struct anv_device *device,
                                 struct anv_embedded_sampler *sampler,
                                 struct anv_pipeline_embedded_sampler_binding *binding);

void
genX(cmd_buffer_dispatch_indirect)(struct anv_cmd_buffer *cmd_buffer,
                                   struct anv_address indirect_addr,
                                   bool is_unaligned_size_x);

void
genX(cmd_dispatch_unaligned)(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    invocations_x,
   uint32_t                                    invocations_y,
   uint32_t                                    invocations_z);
