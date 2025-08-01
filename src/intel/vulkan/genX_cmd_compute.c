/*
 * Copyright © 2015 Intel Corporation
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

#include <assert.h>
#include <stdbool.h>

#include "anv_private.h"
#include "anv_measure.h"

#include "common/intel_common.h"
#include "common/intel_compute_slm.h"
#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"
#include "genxml/genX_rt_pack.h"
#include "common/intel_genX_state_brw.h"

#include "ds/intel_tracepoints.h"

#include "genX_mi_builder.h"

void
genX(cmd_buffer_ensure_cfe_state)(struct anv_cmd_buffer *cmd_buffer,
                                  uint32_t total_scratch)
{
#if GFX_VERx10 >= 125
   assert(cmd_buffer->state.current_pipeline == GPGPU);

   struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;

   if (total_scratch <= comp_state->scratch_size)
      return;

   const struct intel_device_info *devinfo = cmd_buffer->device->info;
   anv_batch_emit(&cmd_buffer->batch, GENX(CFE_STATE), cfe) {
      cfe.MaximumNumberofThreads = devinfo->max_cs_threads * devinfo->subslice_total;

      uint32_t scratch_surf;
      struct anv_scratch_pool *scratch_pool =
         (cmd_buffer->vk.pool->flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT) ?
          &cmd_buffer->device->protected_scratch_pool :
          &cmd_buffer->device->scratch_pool;
      struct anv_bo *scratch_bo =
            anv_scratch_pool_alloc(cmd_buffer->device, scratch_pool,
                                   MESA_SHADER_COMPUTE,
                                   total_scratch);
      anv_reloc_list_add_bo(cmd_buffer->batch.relocs, scratch_bo);
      scratch_surf = anv_scratch_pool_get_surf(cmd_buffer->device, scratch_pool,
                                               total_scratch);
      cfe.ScratchSpaceBuffer = scratch_surf >> ANV_SCRATCH_SPACE_SHIFT(GFX_VER);
#if GFX_VER >= 20
      switch (cmd_buffer->device->physical->instance->stack_ids) {
      case 256:  cfe.StackIDControl = StackIDs256;  break;
      case 512:  cfe.StackIDControl = StackIDs512;  break;
      case 1024: cfe.StackIDControl = StackIDs1024; break;
      case 2048: cfe.StackIDControl = StackIDs2048; break;
      default:   UNREACHABLE("invalid stack_ids value");
      }

#if INTEL_WA_14021821874_GFX_VER || INTEL_WA_14018813551_GFX_VER
      /* Wa_14021821874, Wa_14018813551:
       *
       * "StackIDControlOverride_RTGlobals = 0 (i.e. 2k)". We
       * already set stack size per ray to 64 in brw_nir_lower_rt_intrinsics
       * as the workaround also requires.
       */
      if (intel_needs_workaround(cmd_buffer->device->info, 14021821874) ||
          intel_needs_workaround(cmd_buffer->device->info, 14018813551))
         cfe.StackIDControl = StackIDs2048;
#endif

#endif

      cfe.OverDispatchControl = 2; /* 50% overdispatch */
   }

   comp_state->scratch_size = total_scratch;
#else
   UNREACHABLE("Invalid call");
#endif
}

static void
cmd_buffer_flush_compute_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
   const UNUSED struct intel_device_info *devinfo = cmd_buffer->device->info;
   struct anv_compute_pipeline *pipeline =
      anv_pipeline_to_compute(comp_state->base.pipeline);

   assert(comp_state->shader);

   genX(cmd_buffer_config_l3)(cmd_buffer,
                              pipeline->cs->prog_data->total_shared > 0 ?
                              device->l3_slm_config : device->l3_config);

   genX(cmd_buffer_update_color_aux_op(cmd_buffer, ISL_AUX_OP_NONE));

   genX(flush_descriptor_buffers)(cmd_buffer, &comp_state->base);

   genX(flush_pipeline_select_gpgpu)(cmd_buffer);

   /* Apply any pending pipeline flushes we may have.  We want to apply them
    * now because, if any of those flushes are for things like push constants,
    * the GPU will read the state at weird times.
    */
   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   if (cmd_buffer->state.compute.pipeline_dirty) {
#if GFX_VERx10 < 125
      /* From the Sky Lake PRM Vol 2a, MEDIA_VFE_STATE:
       *
       *    "A stalling PIPE_CONTROL is required before MEDIA_VFE_STATE unless
       *    the only bits that are changed are scoreboard related: Scoreboard
       *    Enable, Scoreboard Type, Scoreboard Mask, Scoreboard * Delta. For
       *    these scoreboard related states, a MEDIA_STATE_FLUSH is
       *    sufficient."
       */
      anv_add_pending_pipe_bits(cmd_buffer,
                              ANV_PIPE_CS_STALL_BIT,
                              "flush compute state");
      genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
#endif

      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->base.batch);

#if GFX_VERx10 >= 125
      const struct brw_cs_prog_data *prog_data = get_cs_prog_data(comp_state);
      genX(cmd_buffer_ensure_cfe_state)(cmd_buffer, prog_data->base.total_scratch);
#endif

      /* Changing the pipeline affects the push constants layout (different
       * amount of cross/per thread allocations). The allocation is also
       * bounded to just the amount consummed by the pipeline (see
       * anv_cmd_buffer_cs_push_constants). So we force the reallocation for
       * every pipeline change.
       *
       * On Gfx12.0 we're also seeing failures in the dEQP-VK.memory_model.*
       * tests when run in parallel. This is likely a HW issue with push
       * constants & context save/restore.
       *
       * TODO: optimize this on Gfx12.5+ where the shader is not using per
       * thread allocations and is also pulling the data using SEND messages.
       * We should be able to limit reallocations only the data actually
       * changes.
       */
      cmd_buffer->state.push_constants_dirty |= VK_SHADER_STAGE_COMPUTE_BIT;
      comp_state->base.push_constants_data_dirty = true;
   }

   cmd_buffer->state.descriptors_dirty |=
      genX(cmd_buffer_flush_push_descriptors)(cmd_buffer,
                                              &cmd_buffer->state.compute.base);

   if ((cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
       cmd_buffer->state.compute.pipeline_dirty) {
      genX(cmd_buffer_flush_descriptor_sets)(
         cmd_buffer,
         &cmd_buffer->state.compute.base,
         VK_SHADER_STAGE_COMPUTE_BIT,
         (const struct anv_shader_bin **)&comp_state->shader, 1);
      cmd_buffer->state.descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE_BIT;

#if GFX_VERx10 < 125
      uint32_t iface_desc_data_dw[GENX(INTERFACE_DESCRIPTOR_DATA_length)];
      struct GENX(INTERFACE_DESCRIPTOR_DATA) desc = {
         .BindingTablePointer =
            cmd_buffer->state.binding_tables[MESA_SHADER_COMPUTE].offset,
         .SamplerStatePointer =
            cmd_buffer->state.samplers[MESA_SHADER_COMPUTE].offset,
      };
      GENX(INTERFACE_DESCRIPTOR_DATA_pack)(NULL, iface_desc_data_dw, &desc);

      struct anv_state state =
         anv_cmd_buffer_merge_dynamic(cmd_buffer, iface_desc_data_dw,
                                      pipeline->gfx9.interface_descriptor_data,
                                      GENX(INTERFACE_DESCRIPTOR_DATA_length),
                                      64);

      uint32_t size = GENX(INTERFACE_DESCRIPTOR_DATA_length) * sizeof(uint32_t);
      anv_batch_emit(&cmd_buffer->batch,
                     GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD), mid) {
         mid.InterfaceDescriptorTotalLength        = size;
         mid.InterfaceDescriptorDataStartAddress   = state.offset;
      }
#endif
   }

   if (cmd_buffer->state.push_constants_dirty & VK_SHADER_STAGE_COMPUTE_BIT) {

      if (comp_state->base.push_constants_state.alloc_size == 0 ||
          comp_state->base.push_constants_data_dirty) {
         comp_state->base.push_constants_state =
            anv_cmd_buffer_cs_push_constants(cmd_buffer);
         comp_state->base.push_constants_data_dirty = false;
      }

#if GFX_VERx10 < 125
      if (comp_state->base.push_constants_state.alloc_size) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_CURBE_LOAD), curbe) {
            curbe.CURBETotalDataLength    = comp_state->base.push_constants_state.alloc_size;
            curbe.CURBEDataStartAddress   = comp_state->base.push_constants_state.offset;
         }
      }
#endif

      cmd_buffer->state.push_constants_dirty &= ~VK_SHADER_STAGE_COMPUTE_BIT;
   }

   cmd_buffer->state.compute.pipeline_dirty = false;

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
}

void
genX(cmd_buffer_flush_compute_state)(struct anv_cmd_buffer *cmd_buffer)
{
   cmd_buffer_flush_compute_state(cmd_buffer);
}

static void
anv_cmd_buffer_push_workgroups(struct anv_cmd_buffer *cmd_buffer,
                               const struct brw_cs_prog_data *prog_data,
                               uint32_t baseGroupX,
                               uint32_t baseGroupY,
                               uint32_t baseGroupZ,
                               uint32_t groupCountX,
                               uint32_t groupCountY,
                               uint32_t groupCountZ,
                               struct anv_address indirect_group)
{
   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   struct anv_push_constants *push =
      &cmd_buffer->state.compute.base.push_constants;
   bool updated = false;
   if (push->cs.base_work_group_id[0] != baseGroupX ||
       push->cs.base_work_group_id[1] != baseGroupY ||
       push->cs.base_work_group_id[2] != baseGroupZ) {
      push->cs.base_work_group_id[0] = baseGroupX;
      push->cs.base_work_group_id[1] = baseGroupY;
      push->cs.base_work_group_id[2] = baseGroupZ;
      updated = true;
   }

   /* On Gfx12.5+ this value goes into the inline parameter register */
   if (GFX_VERx10 < 125 && prog_data->uses_num_work_groups) {
      if (anv_address_is_null(indirect_group)) {
         if (push->cs.num_work_groups[0] != groupCountX ||
             push->cs.num_work_groups[1] != groupCountY ||
             push->cs.num_work_groups[2] != groupCountZ) {
            push->cs.num_work_groups[0] = groupCountX;
            push->cs.num_work_groups[1] = groupCountY;
            push->cs.num_work_groups[2] = groupCountZ;
            updated = true;
         }
      } else {
         uint64_t addr64 = anv_address_physical(indirect_group);
         uint32_t lower_addr32 = addr64 & 0xffffffff;
         uint32_t upper_addr32 = addr64 >> 32;
         if (push->cs.num_work_groups[0] != UINT32_MAX ||
             push->cs.num_work_groups[1] != lower_addr32 ||
             push->cs.num_work_groups[2] != upper_addr32) {
            push->cs.num_work_groups[0] = UINT32_MAX;
            push->cs.num_work_groups[1] = lower_addr32;
            push->cs.num_work_groups[2] = upper_addr32;
            updated = true;
         }
      }
   }

   if (updated) {
      cmd_buffer->state.push_constants_dirty |= VK_SHADER_STAGE_COMPUTE_BIT;
      cmd_buffer->state.compute.base.push_constants_data_dirty = true;
   }
}

#define GPGPU_DISPATCHDIMX 0x2500
#define GPGPU_DISPATCHDIMY 0x2504
#define GPGPU_DISPATCHDIMZ 0x2508

static void
compute_load_indirect_params(struct anv_cmd_buffer *cmd_buffer,
                             const struct anv_address indirect_addr,
                             bool is_unaligned_size_x)
{
   struct mi_builder b;
   mi_builder_init(&b, cmd_buffer->device->info, &cmd_buffer->batch);

   struct mi_value size_x = mi_mem32(anv_address_add(indirect_addr, 0));

   /* Convert unaligned thread invocations to aligned thread group in X
    * dimension for unaligned shader dispatches during ray tracing phase.
    */
   if (is_unaligned_size_x) {
      const uint32_t mocs = isl_mocs(&cmd_buffer->device->isl_dev, 0, false);
      mi_builder_set_mocs(&b, mocs);

      struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
      const struct brw_cs_prog_data *prog_data = get_cs_prog_data(comp_state);

      assert(util_is_power_of_two_or_zero(prog_data->local_size[0]));
      size_x = mi_udiv32_imm(&b, size_x, prog_data->local_size[0]);
      size_x = mi_iadd(&b, size_x, mi_imm(1));
   }

   struct mi_value size_y = mi_mem32(anv_address_add(indirect_addr, 4));
   struct mi_value size_z = mi_mem32(anv_address_add(indirect_addr, 8));

   mi_store(&b, mi_reg32(GPGPU_DISPATCHDIMX), size_x);
   mi_store(&b, mi_reg32(GPGPU_DISPATCHDIMY), size_y);
   mi_store(&b, mi_reg32(GPGPU_DISPATCHDIMZ), size_z);
}

static void
compute_store_indirect_params(struct anv_cmd_buffer *cmd_buffer,
                             const struct anv_address indirect_addr)
{
   struct mi_builder b;
   mi_builder_init(&b, cmd_buffer->device->info, &cmd_buffer->batch);

   struct mi_value size_x = mi_mem32(anv_address_add(indirect_addr, 0));
   struct mi_value size_y = mi_mem32(anv_address_add(indirect_addr, 4));
   struct mi_value size_z = mi_mem32(anv_address_add(indirect_addr, 8));

   mi_store(&b, size_x, mi_reg32(GPGPU_DISPATCHDIMX));
   mi_store(&b, size_y, mi_reg32(GPGPU_DISPATCHDIMY));
   mi_store(&b, size_z, mi_reg32(GPGPU_DISPATCHDIMZ));
}


#if GFX_VERx10 >= 125

static inline struct GENX(INTERFACE_DESCRIPTOR_DATA)
get_interface_descriptor_data(struct anv_cmd_buffer *cmd_buffer,
                              const struct anv_shader_bin *shader,
                              const struct brw_cs_prog_data *prog_data,
                              const struct intel_cs_dispatch_info *dispatch)
{
   const struct intel_device_info *devinfo = cmd_buffer->device->info;

   return (struct GENX(INTERFACE_DESCRIPTOR_DATA)) {
      .SamplerCount = DIV_ROUND_UP(CLAMP(shader->bind_map.sampler_count, 0, 16), 4),
      .KernelStartPointer = shader->kernel.offset,
      .SamplerStatePointer = cmd_buffer->state.samplers[MESA_SHADER_COMPUTE].offset,
      .BindingTablePointer = cmd_buffer->state.binding_tables[MESA_SHADER_COMPUTE].offset,
      /* Typically set to 0 to avoid prefetching on every thread dispatch. */
      .BindingTableEntryCount = devinfo->verx10 == 125 ?
         0 : MIN2(shader->bind_map.surface_count, 30),
      .NumberofThreadsinGPGPUThreadGroup = dispatch->threads,
      .ThreadGroupDispatchSize = intel_compute_threads_group_dispatch_size(dispatch->threads),
      .SharedLocalMemorySize = intel_compute_slm_encode_size(GFX_VER, prog_data->base.total_shared),
      .PreferredSLMAllocationSize =
         intel_compute_preferred_slm_calc_encode_size(devinfo,
                                                      prog_data->base.total_shared,
                                                      dispatch->group_size,
                                                      dispatch->simd_size),
      .NumberOfBarriers = prog_data->uses_barrier,
#if GFX_VER >= 30
      .RegistersPerThread = ptl_register_blocks(prog_data->base.grf_used),
#endif
   };
}

static void
compute_update_async_threads_limit(struct anv_cmd_buffer *cmd_buffer,
                                   const struct brw_cs_prog_data *prog_data,
                                   const struct intel_cs_dispatch_info *dispatch)
{
   const struct intel_device_info *devinfo = cmd_buffer->device->info;
   uint8_t pixel_async_compute_thread_limit, z_pass_async_compute_thread_limit,
           np_z_async_throttle_settings;
   bool slm_or_barrier_enabled = prog_data->base.total_shared != 0 || prog_data->uses_barrier;

   if (cmd_buffer->queue_family->engine_class != INTEL_ENGINE_CLASS_COMPUTE ||
       GFX_VERx10 >= 300)
      return;

   intel_compute_engine_async_threads_limit(devinfo, dispatch->threads,
                                            slm_or_barrier_enabled,
                                            &pixel_async_compute_thread_limit,
                                            &z_pass_async_compute_thread_limit,
                                            &np_z_async_throttle_settings);

   if (cmd_buffer->state.compute.pixel_async_compute_thread_limit != pixel_async_compute_thread_limit ||
       cmd_buffer->state.compute.z_pass_async_compute_thread_limit != z_pass_async_compute_thread_limit ||
       cmd_buffer->state.compute.np_z_async_throttle_settings != np_z_async_throttle_settings) {

      cmd_buffer->state.compute.pixel_async_compute_thread_limit = pixel_async_compute_thread_limit;
      cmd_buffer->state.compute.z_pass_async_compute_thread_limit = z_pass_async_compute_thread_limit;
      cmd_buffer->state.compute.np_z_async_throttle_settings = np_z_async_throttle_settings;

      anv_batch_emit(&cmd_buffer->batch, GENX(STATE_COMPUTE_MODE), cm) {
#if GFX_VER >= 20
         cm.AsyncComputeThreadLimit = pixel_async_compute_thread_limit;
         cm.ZPassAsyncComputeThreadLimit = z_pass_async_compute_thread_limit;
         cm.ZAsyncThrottlesettings = np_z_async_throttle_settings;
         cm.AsyncComputeThreadLimitMask = 0x7;
         cm.ZPassAsyncComputeThreadLimitMask = 0x7;
         cm.ZAsyncThrottlesettingsMask = 0x3;
#else
         cm.PixelAsyncComputeThreadLimit = pixel_async_compute_thread_limit;
         cm.ZPassAsyncComputeThreadLimit = z_pass_async_compute_thread_limit;
         cm.PixelAsyncComputeThreadLimitMask = 0x7;
         cm.ZPassAsyncComputeThreadLimitMask = 0x7;
         if (intel_device_info_is_mtl_or_arl(devinfo)) {
            cm.ZAsyncThrottlesettings = np_z_async_throttle_settings;
            cm.ZAsyncThrottlesettingsMask = 0x3;
         }
#endif
      }
   }
}

static inline void
emit_indirect_compute_walker(struct anv_cmd_buffer *cmd_buffer,
                             const struct brw_cs_prog_data *prog_data,
                             struct anv_address indirect_addr)
{
   const struct intel_device_info *devinfo = cmd_buffer->device->info;
   assert(devinfo->has_indirect_unroll);

   struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
   bool predicate = cmd_buffer->state.conditional_render_enabled;

   const struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(devinfo, prog_data, NULL);
   const int dispatch_size = dispatch.simd_size / 16;

   uint64_t indirect_addr64 = anv_address_physical(indirect_addr);

   uint64_t push_addr64 = anv_address_physical(
      anv_state_pool_state_address(&cmd_buffer->device->general_state_pool,
                                   comp_state->base.push_constants_state));

   compute_update_async_threads_limit(cmd_buffer, prog_data, &dispatch);

   struct GENX(COMPUTE_WALKER_BODY) body =  {
      .SIMDSize                 = dispatch_size,
      /* HSD 14016252163: Use of Morton walk order (and batching using a batch
       * size of 4) is expected to increase sampler cache hit rates by
       * increasing sample address locality within a subslice.
       */
#if GFX_VER >= 30
      .DispatchWalkOrder        = prog_data->uses_sampler ?
                                  MortonWalk :
                                  LinearWalk,
      .ThreadGroupBatchSize     = prog_data->uses_sampler ? TG_BATCH_4 :
                                                            TG_BATCH_1,
#endif
      .MessageSIMD              = dispatch_size,
      .GenerateLocalID          = prog_data->generate_local_id != 0,
      .EmitLocal                = prog_data->generate_local_id,
      .WalkOrder                = prog_data->walk_order,
      .TileLayout               = prog_data->walk_order == INTEL_WALK_ORDER_YXZ ?
                                  TileY32bpe : Linear,
      .LocalXMaximum            = prog_data->local_size[0] - 1,
      .LocalYMaximum            = prog_data->local_size[1] - 1,
      .LocalZMaximum            = prog_data->local_size[2] - 1,
      .ExecutionMask            = dispatch.right_mask,
      .PostSync.MOCS            = anv_mocs(cmd_buffer->device, NULL, 0),
      .InterfaceDescriptor =
         get_interface_descriptor_data(cmd_buffer, comp_state->shader,
                                       prog_data, &dispatch),
      .EmitInlineParameter      = prog_data->uses_inline_data,
      .InlineData               = {
         [ANV_INLINE_PARAM_PUSH_ADDRESS_OFFSET / 4 + 0]   = push_addr64 & 0xffffffff,
         [ANV_INLINE_PARAM_PUSH_ADDRESS_OFFSET / 4 + 1]   = push_addr64 >> 32,
         [ANV_INLINE_PARAM_NUM_WORKGROUPS_OFFSET / 4 + 0] = UINT32_MAX,
         [ANV_INLINE_PARAM_NUM_WORKGROUPS_OFFSET / 4 + 1] = indirect_addr64 & 0xffffffff,
         [ANV_INLINE_PARAM_NUM_WORKGROUPS_OFFSET / 4 + 2] = indirect_addr64 >> 32,
      },
   };

   cmd_buffer->state.last_indirect_dispatch =
      anv_batch_emitn(
         &cmd_buffer->batch,
         GENX(EXECUTE_INDIRECT_DISPATCH_length),
         GENX(EXECUTE_INDIRECT_DISPATCH),
         .PredicateEnable            = predicate,
         .MaxCount                   = 1,
         .body                       = body,
         .ArgumentBufferStartAddress = indirect_addr,
         .MOCS                       = anv_mocs(cmd_buffer->device,
                                                indirect_addr.bo, 0),
      );
}

static inline void
emit_compute_walker(struct anv_cmd_buffer *cmd_buffer,
                    struct anv_address indirect_addr,
                    const struct brw_cs_prog_data *prog_data,
                    struct intel_cs_dispatch_info dispatch,
                    uint32_t groupCountX, uint32_t groupCountY,
                    uint32_t groupCountZ)
{
   const struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
   const bool predicate = cmd_buffer->state.conditional_render_enabled;

   compute_update_async_threads_limit(cmd_buffer, prog_data, &dispatch);

   uint32_t num_workgroup_data[3];
   if (!anv_address_is_null(indirect_addr)) {
      uint64_t indirect_addr64 = anv_address_physical(indirect_addr);
      num_workgroup_data[0] = UINT32_MAX;
      num_workgroup_data[1] = indirect_addr64 & 0xffffffff;
      num_workgroup_data[2] = indirect_addr64 >> 32;
   } else {
      num_workgroup_data[0] = groupCountX;
      num_workgroup_data[1] = groupCountY;
      num_workgroup_data[2] = groupCountZ;
   }

   uint64_t push_addr64 = anv_address_physical(
      anv_state_pool_state_address(&cmd_buffer->device->general_state_pool,
                                   comp_state->base.push_constants_state));

   struct GENX(COMPUTE_WALKER_BODY) body = {
      .SIMDSize                       = dispatch.simd_size / 16,
      .MessageSIMD                    = dispatch.simd_size / 16,
      .GenerateLocalID                = prog_data->generate_local_id != 0,
      .EmitLocal                      = prog_data->generate_local_id,
      .WalkOrder                      = prog_data->walk_order,
      .TileLayout = prog_data->walk_order == INTEL_WALK_ORDER_YXZ ?
                    TileY32bpe : Linear,
      .LocalXMaximum                  = prog_data->local_size[0] - 1,
      .LocalYMaximum                  = prog_data->local_size[1] - 1,
      .LocalZMaximum                  = prog_data->local_size[2] - 1,
      .ThreadGroupIDXDimension        = groupCountX,
      .ThreadGroupIDYDimension        = groupCountY,
      .ThreadGroupIDZDimension        = groupCountZ,
      .ExecutionMask                  = dispatch.right_mask,
      .PostSync                       = {
         .MOCS                        = anv_mocs(cmd_buffer->device, NULL, 0),
      },
      .InterfaceDescriptor =
         get_interface_descriptor_data(cmd_buffer, comp_state->shader,
                                       prog_data, &dispatch),
      .EmitInlineParameter            = prog_data->uses_inline_data,
      .InlineData                     = {
         [ANV_INLINE_PARAM_PUSH_ADDRESS_OFFSET / 4 + 0]   = push_addr64 & 0xffffffff,
         [ANV_INLINE_PARAM_PUSH_ADDRESS_OFFSET / 4 + 1]   = push_addr64 >> 32,
         [ANV_INLINE_PARAM_NUM_WORKGROUPS_OFFSET / 4 + 0] = num_workgroup_data[0],
         [ANV_INLINE_PARAM_NUM_WORKGROUPS_OFFSET / 4 + 1] = num_workgroup_data[1],
         [ANV_INLINE_PARAM_NUM_WORKGROUPS_OFFSET / 4 + 2] = num_workgroup_data[2],
      },
#if GFX_VER >= 30
         /* HSD 14016252163 */
      .DispatchWalkOrder = prog_data->uses_sampler ? MortonWalk : LinearWalk,
      .ThreadGroupBatchSize = prog_data->uses_sampler ? TG_BATCH_4 : TG_BATCH_1,
#endif
   };

   cmd_buffer->state.last_compute_walker =
      anv_batch_emitn(
         &cmd_buffer->batch,
         GENX(COMPUTE_WALKER_length),
         GENX(COMPUTE_WALKER),
         .IndirectParameterEnable        = !anv_address_is_null(indirect_addr),
         .PredicateEnable                = predicate,
         .body                           = body,
#if GFX_VERx10 == 125
         .SystolicModeEnable             = prog_data->uses_systolic,
#endif
      );
}

#else /* #if GFX_VERx10 >= 125 */

static inline void
emit_gpgpu_walker(struct anv_cmd_buffer *cmd_buffer,
                  bool indirect,
                  const struct brw_cs_prog_data *prog_data,
                  uint32_t groupCountX, uint32_t groupCountY,
                  uint32_t groupCountZ)
{
   const bool predicate = cmd_buffer->state.conditional_render_enabled;

   const struct intel_device_info *devinfo = cmd_buffer->device->info;
   const struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(devinfo, prog_data, NULL);

   anv_batch_emit(&cmd_buffer->batch, GENX(GPGPU_WALKER), ggw) {
      ggw.IndirectParameterEnable      = indirect;
      ggw.PredicateEnable              = predicate;
      ggw.SIMDSize                     = dispatch.simd_size / 16;
      ggw.ThreadDepthCounterMaximum    = 0;
      ggw.ThreadHeightCounterMaximum   = 0;
      ggw.ThreadWidthCounterMaximum    = dispatch.threads - 1;
      ggw.ThreadGroupIDXDimension      = groupCountX;
      ggw.ThreadGroupIDYDimension      = groupCountY;
      ggw.ThreadGroupIDZDimension      = groupCountZ;
      ggw.RightExecutionMask           = dispatch.right_mask;
      ggw.BottomExecutionMask          = 0xffffffff;
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_STATE_FLUSH), msf);
}

#endif /* #if GFX_VERx10 >= 125 */

static inline void
emit_cs_walker(struct anv_cmd_buffer *cmd_buffer,
               const struct brw_cs_prog_data *prog_data,
               struct intel_cs_dispatch_info dispatch,
               struct anv_address indirect_addr,
               uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ,
               bool is_unaligned_size_x)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_instance *instance = device->physical->instance;
   bool is_indirect = !anv_address_is_null(indirect_addr);

   struct mi_builder b;
   if (unlikely(instance->debug & ANV_DEBUG_SHADER_HASH)) {
      mi_builder_init(&b, device->info, &cmd_buffer->batch);
      mi_builder_set_mocs(&b, isl_mocs(&device->isl_dev, 0, false));
      mi_store(&b, mi_mem32(device->workaround_address),
                   mi_imm(prog_data->base.source_hash));
   }

#if GFX_VERx10 >= 125
   /* For unaligned dispatch, we need to tweak the dispatch value with
    * MI_MATH, so we can't use indirect HW instructions.
    */
   if (is_indirect && !is_unaligned_size_x &&
       cmd_buffer->device->info->has_indirect_unroll) {
      emit_indirect_compute_walker(cmd_buffer, prog_data,
                                   indirect_addr);
      return;
   }
#endif

   if (is_indirect)
      compute_load_indirect_params(cmd_buffer, indirect_addr,
            is_unaligned_size_x);

#if GFX_VERx10 >= 125
   emit_compute_walker(cmd_buffer, indirect_addr, prog_data,
                       dispatch, groupCountX, groupCountY, groupCountZ);
#else
   emit_gpgpu_walker(cmd_buffer, is_indirect, prog_data,
                     groupCountX, groupCountY, groupCountZ);
#endif
}

void genX(CmdDispatchBase)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    baseGroupX,
    uint32_t                                    baseGroupY,
    uint32_t                                    baseGroupZ,
    uint32_t                                    groupCountX,
    uint32_t                                    groupCountY,
    uint32_t                                    groupCountZ)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
   const struct brw_cs_prog_data *prog_data = get_cs_prog_data(comp_state);
   struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(cmd_buffer->device->info, prog_data, NULL);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   anv_cmd_buffer_push_workgroups(cmd_buffer, prog_data,
                                  baseGroupX, baseGroupY, baseGroupZ,
                                  groupCountX, groupCountY, groupCountZ,
                                  ANV_NULL_ADDRESS);

   anv_measure_snapshot(cmd_buffer,
                        INTEL_SNAPSHOT_COMPUTE,
                        "compute",
                        groupCountX * groupCountY * groupCountZ *
                        prog_data->local_size[0] * prog_data->local_size[1] *
                        prog_data->local_size[2]);

   trace_intel_begin_compute(&cmd_buffer->trace);

   cmd_buffer_flush_compute_state(cmd_buffer);

   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

   genX(emit_breakpoint)(&cmd_buffer->batch, cmd_buffer->device, true);

   emit_cs_walker(cmd_buffer, prog_data, dispatch,
                  ANV_NULL_ADDRESS /* no indirect data */,
                  groupCountX, groupCountY, groupCountZ,
                  false);

   genX(emit_breakpoint)(&cmd_buffer->batch, cmd_buffer->device, false);

   trace_intel_end_compute(&cmd_buffer->trace,
                           groupCountX, groupCountY, groupCountZ,
                           prog_data->base.source_hash);
}

static void
emit_unaligned_cs_walker(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    baseGroupX,
    uint32_t                                    baseGroupY,
    uint32_t                                    baseGroupZ,
    uint32_t                                    groupCountX,
    uint32_t                                    groupCountY,
    uint32_t                                    groupCountZ,
    struct intel_cs_dispatch_info               dispatch)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
   const struct brw_cs_prog_data *prog_data = get_cs_prog_data(comp_state);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   anv_cmd_buffer_push_workgroups(cmd_buffer, prog_data,
                                  baseGroupX, baseGroupY, baseGroupZ,
                                  groupCountX, groupCountY, groupCountZ,
                                  ANV_NULL_ADDRESS);

   /* RT shaders have Y and Z local size set to 1 always. */
   assert(prog_data->local_size[1] == 1 && prog_data->local_size[2] == 1);

   /* RT shaders dispatched with group Y and Z set to 1 always. */
   assert(groupCountY == 1 && groupCountZ == 1);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   anv_measure_snapshot(cmd_buffer,
                        INTEL_SNAPSHOT_COMPUTE,
                        "compute-unaligned-cs-walker",
                        groupCountX * groupCountY * groupCountZ *
                        prog_data->local_size[0] * prog_data->local_size[1] *
                        prog_data->local_size[2]);

   trace_intel_begin_compute(&cmd_buffer->trace);

   assert(!prog_data->uses_num_work_groups);
   genX(cmd_buffer_flush_compute_state)(cmd_buffer);

   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

#if GFX_VERx10 >= 125
   emit_compute_walker(cmd_buffer, ANV_NULL_ADDRESS, prog_data,
                       dispatch, groupCountX, groupCountY, groupCountZ);
#endif

   trace_intel_end_compute(&cmd_buffer->trace,
                           groupCountX, groupCountY, groupCountZ,
                           prog_data->base.source_hash);
}

/*
 * Dispatch compute work item with unaligned thread invocations.
 *
 * This helper takes unaligned thread invocations, convert it into aligned
 * thread group count and dispatch compute work items.
 *
 * We launch two CS walker, one with aligned part and another CS walker
 * with single group for remaining thread invocations.
 *
 * This function is now specifically for BVH building.
 */
void
genX(cmd_dispatch_unaligned)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    invocations_x,
    uint32_t                                    invocations_y,
    uint32_t                                    invocations_z)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
   const struct brw_cs_prog_data *prog_data = get_cs_prog_data(comp_state);

   /* Group X can be unaligned for RT dispatches. */
   uint32_t groupCountX = invocations_x / prog_data->local_size[0];
   uint32_t groupCountY = invocations_y;
   uint32_t groupCountZ = invocations_z;

   struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(cmd_buffer->device->info, prog_data, NULL);

   /* Launch first CS walker with aligned group count X. */
   if (groupCountX) {
      emit_unaligned_cs_walker(commandBuffer, 0, 0, 0, groupCountX,
                               groupCountY, groupCountZ, dispatch);
   }

   uint32_t unaligned_invocations_x = invocations_x % prog_data->local_size[0];
   if (unaligned_invocations_x) {
      dispatch.threads = DIV_ROUND_UP(unaligned_invocations_x,
                                      dispatch.simd_size);

      /* Make sure the 2nd walker has the same amount of invocations per
       * workgroup as the 1st walker, so that gl_GlobalInvocationsID can be
       * calculated correctly with baseGroup.
       */
      assert(dispatch.threads * dispatch.simd_size == prog_data->local_size[0]);

      const uint32_t remainder = unaligned_invocations_x & (dispatch.simd_size - 1);
      if (remainder > 0) {
         dispatch.right_mask = ~0u >> (32 - remainder);
      } else {
         dispatch.right_mask = ~0u >> (32 - dispatch.simd_size);
      }

      /* Launch second CS walker for unaligned part. */
      emit_unaligned_cs_walker(commandBuffer, groupCountX, 0, 0, 1, 1, 1,
                               dispatch);
   }
}

/*
 * This dispatches compute work item with indirect parameters.
 * Helper also makes the unaligned thread invocations aligned.
 */
void
genX(cmd_buffer_dispatch_indirect)(struct anv_cmd_buffer *cmd_buffer,
                                   struct anv_address indirect_addr,
                                   bool is_unaligned_size_x)
{
   struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
   const struct brw_cs_prog_data *prog_data = get_cs_prog_data(comp_state);
   UNUSED struct anv_batch *batch = &cmd_buffer->batch;
   struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(cmd_buffer->device->info, prog_data, NULL);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   anv_cmd_buffer_push_workgroups(cmd_buffer, prog_data,
                                  0, 0, 0, 0, 0, 0, indirect_addr);

   anv_measure_snapshot(cmd_buffer,
                        INTEL_SNAPSHOT_COMPUTE,
                        "compute indirect",
                        0);

   trace_intel_begin_compute_indirect(&cmd_buffer->trace);

   cmd_buffer_flush_compute_state(cmd_buffer);

   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

   genX(emit_breakpoint)(&cmd_buffer->batch, cmd_buffer->device, true);

   emit_cs_walker(cmd_buffer, prog_data, dispatch, indirect_addr,
                  0, 0, 0, is_unaligned_size_x);

   genX(emit_breakpoint)(&cmd_buffer->batch, cmd_buffer->device, false);

   trace_intel_end_compute_indirect(&cmd_buffer->trace,
                                    anv_address_utrace(indirect_addr),
                                    prog_data->base.source_hash);
}

void genX(CmdDispatchIndirect)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_address addr = anv_address_add(buffer->address, offset);

   genX(cmd_buffer_dispatch_indirect)(cmd_buffer, addr, false);
}

struct anv_address
genX(cmd_buffer_ray_query_globals)(struct anv_cmd_buffer *cmd_buffer)
{
#if GFX_VERx10 >= 125
   struct anv_device *device = cmd_buffer->device;

   struct anv_state state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer,
                                           BRW_RT_DISPATCH_GLOBALS_SIZE, 64);
   struct brw_rt_scratch_layout layout;
   uint32_t stack_ids_per_dss = 2048; /* TODO: can we use a lower value in
                                       * some cases?
                                       */
   brw_rt_compute_scratch_layout(&layout, device->info,
                                 stack_ids_per_dss, 1 << 10);

   uint8_t idx = anv_get_ray_query_bo_index(cmd_buffer);

   const struct GENX(RT_DISPATCH_GLOBALS) rtdg = {
      .MemBaseAddress = (struct anv_address) {
         /* The ray query HW computes offsets from the top of the buffer, so
          * let the address at the end of the buffer.
          */
         .bo = device->ray_query_bo[idx],
         .offset = device->ray_query_bo[idx]->size
      },
      .AsyncRTStackSize = layout.ray_stack_stride / 64,
      .NumDSSRTStacks = layout.stack_ids_per_dss,
      .MaxBVHLevels = BRW_RT_MAX_BVH_LEVELS,
      .Flags = RT_DEPTH_TEST_LESS_EQUAL,
      .ResumeShaderTable = (struct anv_address) {
         .bo = cmd_buffer->state.ray_query_shadow_bo,
      },
   };
   GENX(RT_DISPATCH_GLOBALS_pack)(NULL, state.map, &rtdg);

   return anv_cmd_buffer_temporary_state_address(cmd_buffer, state);
#else
   UNREACHABLE("Not supported");
#endif
}

#if GFX_VERx10 >= 125
static void
calc_local_trace_size(uint8_t local_shift[3], const uint32_t global[3])
{
   unsigned total_shift = 0;
   memset(local_shift, 0, 3);

   bool progress;
   do {
      progress = false;
      for (unsigned i = 0; i < 3; i++) {
         assert(global[i] > 0);
         if ((1 << local_shift[i]) < global[i]) {
            progress = true;
            local_shift[i]++;
            total_shift++;
         }

         if (total_shift == 3)
            return;
      }
   } while(progress);

   /* Assign whatever's left to x */
   local_shift[0] += 3 - total_shift;
}

static struct GENX(RT_SHADER_TABLE)
vk_sdar_to_shader_table(const VkStridedDeviceAddressRegionKHR *region)
{
   return (struct GENX(RT_SHADER_TABLE)) {
      .BaseAddress = anv_address_from_u64(region->deviceAddress),
      .Stride = region->stride,
   };
}

struct trace_params {
   /* If is_sbt_indirect, use indirect_sbts_addr to build RT_DISPATCH_GLOBALS
    * with mi_builder.
    */
   bool is_sbt_indirect;
   const VkStridedDeviceAddressRegionKHR *raygen_sbt;
   const VkStridedDeviceAddressRegionKHR *miss_sbt;
   const VkStridedDeviceAddressRegionKHR *hit_sbt;
   const VkStridedDeviceAddressRegionKHR *callable_sbt;

   /* A pointer to a VkTraceRaysIndirectCommand2KHR structure */
   uint64_t indirect_sbts_addr;

   /* If is_indirect, use launch_size_addr to program the dispatch size. */
   bool is_launch_size_indirect;
   uint32_t launch_size[3];

   /* A pointer a uint32_t[3] */
   uint64_t launch_size_addr;
};

static struct anv_state
cmd_buffer_emit_rt_dispatch_globals(struct anv_cmd_buffer *cmd_buffer,
                                    struct trace_params *params)
{
   assert(!params->is_sbt_indirect);
   assert(params->miss_sbt != NULL);
   assert(params->hit_sbt != NULL);
   assert(params->callable_sbt != NULL);

   struct anv_cmd_ray_tracing_state *rt = &cmd_buffer->state.rt;

   struct anv_state rtdg_state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer,
                                           BRW_RT_PUSH_CONST_OFFSET +
                                           sizeof(struct anv_push_constants),
                                           64);

   struct GENX(RT_DISPATCH_GLOBALS) rtdg = {
      .MemBaseAddress     = (struct anv_address) {
         .bo = rt->scratch.bo,
         .offset = rt->scratch.layout.ray_stack_start,
      },
#if GFX_VERx10 == 300
      .CallStackHandler   = anv_shader_bin_get_handler(
         cmd_buffer->device->rt_trivial_return, 0),
#else
      .CallStackHandler   = anv_shader_bin_get_bsr(
         cmd_buffer->device->rt_trivial_return, 0),
#endif
      .AsyncRTStackSize   = rt->scratch.layout.ray_stack_stride / 64,
      .NumDSSRTStacks     = rt->scratch.layout.stack_ids_per_dss,
      .MaxBVHLevels       = BRW_RT_MAX_BVH_LEVELS,
      .Flags              = RT_DEPTH_TEST_LESS_EQUAL,
#if GFX_VER >= 30
      .HitGroupStride     = params->hit_sbt->stride,
      .MissGroupStride    = params->miss_sbt->stride,
      .HitGroupTable      =
         anv_address_from_u64(params->hit_sbt->deviceAddress),
      .MissGroupTable     =
         anv_address_from_u64(params->miss_sbt->deviceAddress),
#else
      .HitGroupTable      = vk_sdar_to_shader_table(params->hit_sbt),
      .MissGroupTable     = vk_sdar_to_shader_table(params->miss_sbt),
#endif
      .SWStackSize        = rt->scratch.layout.sw_stack_size / 64,
      .LaunchWidth        = params->launch_size[0],
      .LaunchHeight       = params->launch_size[1],
      .LaunchDepth        = params->launch_size[2],
#if GFX_VER >= 30
      .CallableGroupTable =
         anv_address_from_u64(params->callable_sbt->deviceAddress),
      .CallableGroupStride = params->callable_sbt->stride,
#else
      .CallableGroupTable = vk_sdar_to_shader_table(params->callable_sbt),
#endif
   };
   GENX(RT_DISPATCH_GLOBALS_pack)(NULL, rtdg_state.map, &rtdg);

   return rtdg_state;
}

static struct mi_value
mi_build_sbt_entry(struct mi_builder *b,
                   uint64_t addr_field_addr,
                   uint64_t stride_field_addr)
{
   return mi_ior(b,
                 mi_iand(b, mi_mem64(anv_address_from_u64(addr_field_addr)),
                            mi_imm(BITFIELD64_BIT(49) - 1)),
                 mi_ishl_imm(b, mi_mem32(anv_address_from_u64(stride_field_addr)),
                                48));
}

static struct anv_state
cmd_buffer_emit_rt_dispatch_globals_indirect(struct anv_cmd_buffer *cmd_buffer,
                                             struct trace_params *params)
{
   struct anv_cmd_ray_tracing_state *rt = &cmd_buffer->state.rt;

   struct anv_state rtdg_state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer,
                                           BRW_RT_PUSH_CONST_OFFSET +
                                           sizeof(struct anv_push_constants),
                                           64);

   struct GENX(RT_DISPATCH_GLOBALS) rtdg = {
      .MemBaseAddress     = (struct anv_address) {
         .bo = rt->scratch.bo,
         .offset = rt->scratch.layout.ray_stack_start,
      },
#if GFX_VERx10 == 300
      .CallStackHandler   = anv_shader_bin_get_handler(
         cmd_buffer->device->rt_trivial_return, 0),
#else
      .CallStackHandler   = anv_shader_bin_get_bsr(
         cmd_buffer->device->rt_trivial_return, 0),
#endif
      .AsyncRTStackSize   = rt->scratch.layout.ray_stack_stride / 64,
      .NumDSSRTStacks     = rt->scratch.layout.stack_ids_per_dss,
      .MaxBVHLevels       = BRW_RT_MAX_BVH_LEVELS,
      .Flags              = RT_DEPTH_TEST_LESS_EQUAL,
      .SWStackSize        = rt->scratch.layout.sw_stack_size / 64,
   };
   GENX(RT_DISPATCH_GLOBALS_pack)(NULL, rtdg_state.map, &rtdg);

   struct anv_address rtdg_addr =
      anv_cmd_buffer_temporary_state_address(cmd_buffer, rtdg_state);

   struct mi_builder b;
   mi_builder_init(&b, cmd_buffer->device->info, &cmd_buffer->batch);
   const uint32_t mocs = anv_mocs_for_address(cmd_buffer->device, &rtdg_addr);
   mi_builder_set_mocs(&b, mocs);
   mi_builder_set_write_check(&b, true);

   /* Fill the MissGroupTable, HitGroupTable & CallableGroupTable fields of
    * RT_DISPATCH_GLOBALS using the mi_builder.
    */
   mi_store(&b,
            mi_mem64(
               anv_address_add(
                  rtdg_addr,
                  GENX(RT_DISPATCH_GLOBALS_MissGroupTable_start) / 8)),
            mi_build_sbt_entry(&b,
                               params->indirect_sbts_addr +
                               offsetof(VkTraceRaysIndirectCommand2KHR,
                                        missShaderBindingTableAddress),
                               params->indirect_sbts_addr +
                               offsetof(VkTraceRaysIndirectCommand2KHR,
                                        missShaderBindingTableStride)));
   mi_store(&b,
            mi_mem64(
               anv_address_add(
                  rtdg_addr,
                  GENX(RT_DISPATCH_GLOBALS_HitGroupTable_start) / 8)),
            mi_build_sbt_entry(&b,
                               params->indirect_sbts_addr +
                               offsetof(VkTraceRaysIndirectCommand2KHR,
                                        hitShaderBindingTableAddress),
                               params->indirect_sbts_addr +
                               offsetof(VkTraceRaysIndirectCommand2KHR,
                                        hitShaderBindingTableStride)));
   mi_store(&b,
            mi_mem64(
               anv_address_add(
                  rtdg_addr,
                  GENX(RT_DISPATCH_GLOBALS_CallableGroupTable_start) / 8)),
            mi_build_sbt_entry(&b,
                               params->indirect_sbts_addr +
                               offsetof(VkTraceRaysIndirectCommand2KHR,
                                        callableShaderBindingTableAddress),
                               params->indirect_sbts_addr +
                               offsetof(VkTraceRaysIndirectCommand2KHR,
                                        callableShaderBindingTableStride)));

   return rtdg_state;
}

static void
cmd_buffer_trace_rays(struct anv_cmd_buffer *cmd_buffer,
                      struct trace_params *params)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_cmd_ray_tracing_state *rt = &cmd_buffer->state.rt;
   struct anv_ray_tracing_pipeline *pipeline =
      anv_pipeline_to_ray_tracing(rt->base.pipeline);

   if (INTEL_DEBUG(DEBUG_RT_NO_TRACE))
      return;

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   /* If we have a known degenerate launch size, just bail */
   if (!params->is_launch_size_indirect &&
       (params->launch_size[0] == 0 ||
        params->launch_size[1] == 0 ||
        params->launch_size[2] == 0))
      return;

   trace_intel_begin_rays(&cmd_buffer->trace);

   genX(cmd_buffer_config_l3)(cmd_buffer, device->l3_config);

   genX(cmd_buffer_update_color_aux_op(cmd_buffer, ISL_AUX_OP_NONE));

   genX(flush_descriptor_buffers)(cmd_buffer, &rt->base);

   genX(flush_pipeline_select_gpgpu)(cmd_buffer);

   cmd_buffer->state.rt.pipeline_dirty = false;

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   genX(cmd_buffer_flush_push_descriptors)(cmd_buffer,
                                           &cmd_buffer->state.rt.base);

   /* Add these to the reloc list as they're internal buffers that don't
    * actually have relocs to pick them up manually.
    *
    * TODO(RT): This is a bit of a hack
    */
   anv_reloc_list_add_bo(cmd_buffer->batch.relocs,
                         rt->scratch.bo);
   anv_reloc_list_add_bo(cmd_buffer->batch.relocs,
                         cmd_buffer->device->btd_fifo_bo);

   /* Allocate and set up our RT_DISPATCH_GLOBALS */
   struct anv_state rtdg_state =
      params->is_sbt_indirect ?
      cmd_buffer_emit_rt_dispatch_globals_indirect(cmd_buffer, params) :
      cmd_buffer_emit_rt_dispatch_globals(cmd_buffer, params);

   assert(rtdg_state.alloc_size >= (BRW_RT_PUSH_CONST_OFFSET +
                                    sizeof(struct anv_push_constants)));
   assert(GENX(RT_DISPATCH_GLOBALS_length) * 4 <= BRW_RT_PUSH_CONST_OFFSET);
   /* Push constants go after the RT_DISPATCH_GLOBALS */
   memcpy(rtdg_state.map + BRW_RT_PUSH_CONST_OFFSET,
          &cmd_buffer->state.rt.base.push_constants,
          sizeof(struct anv_push_constants));

   struct anv_address rtdg_addr =
      anv_cmd_buffer_temporary_state_address(cmd_buffer, rtdg_state);

   uint8_t local_size_log2[3];
   uint32_t global_size[3] = {};
   if (params->is_launch_size_indirect) {
      /* Pick a local size that's probably ok.  We assume most TraceRays calls
       * will use a two-dimensional dispatch size.  Worst case, our initial
       * dispatch will be a little slower than it has to be.
       */
      local_size_log2[0] = 2;
      local_size_log2[1] = 1;
      local_size_log2[2] = 0;

      struct mi_builder b;
      mi_builder_init(&b, cmd_buffer->device->info, &cmd_buffer->batch);
      const uint32_t mocs = anv_mocs_for_address(cmd_buffer->device, &rtdg_addr);
      mi_builder_set_mocs(&b, mocs);
      mi_builder_set_write_check(&b, true);

      struct mi_value launch_size[3] = {
         mi_mem32(anv_address_from_u64(params->launch_size_addr + 0)),
         mi_mem32(anv_address_from_u64(params->launch_size_addr + 4)),
         mi_mem32(anv_address_from_u64(params->launch_size_addr + 8)),
      };

      /* Store the original launch size into RT_DISPATCH_GLOBALS */
      mi_store(&b, mi_mem32(anv_address_add(rtdg_addr,
                                            GENX(RT_DISPATCH_GLOBALS_LaunchWidth_start) / 8)),
               mi_value_ref(&b, launch_size[0]));
      mi_store(&b, mi_mem32(anv_address_add(rtdg_addr,
                                            GENX(RT_DISPATCH_GLOBALS_LaunchHeight_start) / 8)),
               mi_value_ref(&b, launch_size[1]));
      mi_store(&b, mi_mem32(anv_address_add(rtdg_addr,
                                            GENX(RT_DISPATCH_GLOBALS_LaunchDepth_start) / 8)),
               mi_value_ref(&b, launch_size[2]));

      /* Compute the global dispatch size */
      for (unsigned i = 0; i < 3; i++) {
         if (local_size_log2[i] == 0)
            continue;

         /* global_size = DIV_ROUND_UP(launch_size, local_size)
          *
          * Fortunately for us MI_ALU math is 64-bit and , mi_ushr32_imm
          * has the semantics of shifting the enture 64-bit value and taking
          * the bottom 32 so we don't have to worry about roll-over.
          */
         uint32_t local_size = 1 << local_size_log2[i];
         launch_size[i] = mi_iadd(&b, launch_size[i],
                                      mi_imm(local_size - 1));
         launch_size[i] = mi_ushr32_imm(&b, launch_size[i],
                                            local_size_log2[i]);
      }

      mi_store(&b, mi_reg32(GPGPU_DISPATCHDIMX), launch_size[0]);
      mi_store(&b, mi_reg32(GPGPU_DISPATCHDIMY), launch_size[1]);
      mi_store(&b, mi_reg32(GPGPU_DISPATCHDIMZ), launch_size[2]);

   } else {
      calc_local_trace_size(local_size_log2, params->launch_size);

      for (unsigned i = 0; i < 3; i++) {
         /* We have to be a bit careful here because DIV_ROUND_UP adds to the
          * numerator value may overflow.  Cast to uint64_t to avoid this.
          */
         uint32_t local_size = 1 << local_size_log2[i];
         global_size[i] = DIV_ROUND_UP((uint64_t)params->launch_size[i], local_size);
      }
   }

#if GFX_VERx10 == 125
   /* Wa_14014427904 - We need additional invalidate/flush when
    * emitting NP state commands with ATS-M in compute mode.
    */
   if (intel_device_info_is_atsm(device->info) &&
      cmd_buffer->queue_family->engine_class == INTEL_ENGINE_CLASS_COMPUTE) {
      genx_batch_emit_pipe_control(&cmd_buffer->batch,
                                   cmd_buffer->device->info,
                                   cmd_buffer->state.current_pipeline,
                                   ANV_PIPE_CS_STALL_BIT |
                                   ANV_PIPE_STATE_CACHE_INVALIDATE_BIT |
                                   ANV_PIPE_CONSTANT_CACHE_INVALIDATE_BIT |
                                   ANV_PIPE_UNTYPED_DATAPORT_CACHE_FLUSH_BIT |
                                   ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT |
                                   ANV_PIPE_INSTRUCTION_CACHE_INVALIDATE_BIT |
                                   ANV_PIPE_HDC_PIPELINE_FLUSH_BIT);
   }
#endif

   anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_BTD), btd) {
      /* TODO: This is the timeout after which the bucketed thread dispatcher
       *       will kick off a wave of threads. We go with the lowest value
       *       for now. It could be tweaked on a per application basis
       *       (drirc).
       */
      btd.DispatchTimeoutCounter = _64clocks;
      /* BSpec 43851: "This field must be programmed to 6h i.e. memory backed
       *               buffer must be 128KB."
       */
      btd.PerDSSMemoryBackedBufferSize = 6;
      btd.MemoryBackedBufferBasePointer = (struct anv_address) { .bo = device->btd_fifo_bo };
      if (pipeline->base.scratch_size > 0) {
         struct anv_bo *scratch_bo =
            anv_scratch_pool_alloc(device,
                                   &device->scratch_pool,
                                   MESA_SHADER_COMPUTE,
                                   pipeline->base.scratch_size);
         anv_reloc_list_add_bo(cmd_buffer->batch.relocs,
                               scratch_bo);
         uint32_t scratch_surf =
            anv_scratch_pool_get_surf(cmd_buffer->device,
                                      &device->scratch_pool,
                                      pipeline->base.scratch_size);
         btd.ScratchSpaceBuffer = scratch_surf >> ANV_SCRATCH_SPACE_SHIFT(GFX_VER);
      }
#if INTEL_NEEDS_WA_14017794102 || INTEL_NEEDS_WA_14023061436
      btd.BTDMidthreadpreemption = false;
#endif

#if GFX_VER >= 30
      btd.RTMemStructures64bModeEnable = true;
#endif
   }

   genX(cmd_buffer_ensure_cfe_state)(cmd_buffer, pipeline->base.scratch_size);

   const struct brw_cs_prog_data *cs_prog_data =
      brw_cs_prog_data_const(device->rt_trampoline->prog_data);
   struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(device->info, cs_prog_data, NULL);

   const gl_shader_stage s = MESA_SHADER_RAYGEN;
   struct anv_state *surfaces = &cmd_buffer->state.binding_tables[s];
   struct anv_state *samplers = &cmd_buffer->state.samplers[s];
   struct brw_rt_raygen_trampoline_params trampoline_params = {
      .rt_disp_globals_addr = anv_address_physical(rtdg_addr),
      .raygen_bsr_addr =
         params->is_sbt_indirect ?
         (params->indirect_sbts_addr +
          offsetof(VkTraceRaysIndirectCommand2KHR,
                   raygenShaderRecordAddress)) :
         params->raygen_sbt->deviceAddress,
      .is_indirect = params->is_sbt_indirect,
      .local_group_size_log2 = {
         local_size_log2[0],
         local_size_log2[1],
         local_size_log2[2],
      },
   };

   compute_update_async_threads_limit(cmd_buffer, cs_prog_data, &dispatch);

   struct GENX(COMPUTE_WALKER_BODY) body =  {
      .SIMDSize                       = dispatch.simd_size / 16,
      .MessageSIMD                    = dispatch.simd_size / 16,
      .LocalXMaximum                  = (1 << local_size_log2[0]) - 1,
      .LocalYMaximum                  = (1 << local_size_log2[1]) - 1,
      .LocalZMaximum                  = (1 << local_size_log2[2]) - 1,
      .ThreadGroupIDXDimension        = global_size[0],
      .ThreadGroupIDYDimension        = global_size[1],
      .ThreadGroupIDZDimension        = global_size[2],
      .ExecutionMask                  = 0xff,
      .EmitInlineParameter            = true,
      .PostSync.MOCS                  = anv_mocs(pipeline->base.device, NULL, 0),
#if GFX_VER >= 30
         /* HSD 14016252163 */
      .DispatchWalkOrder = cs_prog_data->uses_sampler ? MortonWalk : LinearWalk,
      .ThreadGroupBatchSize = cs_prog_data->uses_sampler ? TG_BATCH_4 : TG_BATCH_1,
#endif

      .InterfaceDescriptor = (struct GENX(INTERFACE_DESCRIPTOR_DATA)) {
         .KernelStartPointer = device->rt_trampoline->kernel.offset,
         .SamplerStatePointer = samplers->offset,
         /* i965: DIV_ROUND_UP(CLAMP(stage_state->sampler_count, 0, 16), 4), */
         .SamplerCount = 0,
         .BindingTablePointer = surfaces->offset,
         .NumberofThreadsinGPGPUThreadGroup = 1,
         .ThreadGroupDispatchSize =
            intel_compute_threads_group_dispatch_size(dispatch.threads),
         .BTDMode = true,
#if INTEL_NEEDS_WA_14017794102 || INTEL_NEEDS_WA_14023061436
         .ThreadPreemption = false,
#endif
#if GFX_VER >= 30
         .RegistersPerThread = ptl_register_blocks(cs_prog_data->base.grf_used),
#endif
      },
   };

   STATIC_ASSERT(sizeof(trampoline_params) == 32);
   memcpy(body.InlineData, &trampoline_params, sizeof(trampoline_params));

   cmd_buffer->state.last_compute_walker =
      anv_batch_emitn(
         &cmd_buffer->batch,
         GENX(COMPUTE_WALKER_length),
         GENX(COMPUTE_WALKER),
         .IndirectParameterEnable  = params->is_launch_size_indirect,
         .PredicateEnable          = false,
         .body                     = body,
      );

   trace_intel_end_rays(&cmd_buffer->trace,
                        params->launch_size[0],
                        params->launch_size[1],
                        params->launch_size[2]);
}

void
genX(CmdTraceRaysKHR)(
    VkCommandBuffer                             commandBuffer,
    const VkStridedDeviceAddressRegionKHR*      pRaygenShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR*      pMissShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR*      pHitShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR*      pCallableShaderBindingTable,
    uint32_t                                    width,
    uint32_t                                    height,
    uint32_t                                    depth)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct trace_params params = {
      .is_sbt_indirect         = false,
      .raygen_sbt              = pRaygenShaderBindingTable,
      .miss_sbt                = pMissShaderBindingTable,
      .hit_sbt                 = pHitShaderBindingTable,
      .callable_sbt            = pCallableShaderBindingTable,
      .is_launch_size_indirect = false,
      .launch_size             = {
         width,
         height,
         depth,
      },
   };

   cmd_buffer_trace_rays(cmd_buffer, &params);
}

void
genX(CmdTraceRaysIndirectKHR)(
    VkCommandBuffer                             commandBuffer,
    const VkStridedDeviceAddressRegionKHR*      pRaygenShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR*      pMissShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR*      pHitShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR*      pCallableShaderBindingTable,
    VkDeviceAddress                             indirectDeviceAddress)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct trace_params params = {
      .is_sbt_indirect         = false,
      .raygen_sbt              = pRaygenShaderBindingTable,
      .miss_sbt                = pMissShaderBindingTable,
      .hit_sbt                 = pHitShaderBindingTable,
      .callable_sbt            = pCallableShaderBindingTable,
      .is_launch_size_indirect = true,
      .launch_size_addr        = indirectDeviceAddress,
   };

   cmd_buffer_trace_rays(cmd_buffer, &params);
}

void
genX(CmdTraceRaysIndirect2KHR)(
    VkCommandBuffer                             commandBuffer,
    VkDeviceAddress                             indirectDeviceAddress)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct trace_params params = {
      .is_sbt_indirect         = true,
      .indirect_sbts_addr      = indirectDeviceAddress,
      .is_launch_size_indirect = true,
      .launch_size_addr        = indirectDeviceAddress +
                                 offsetof(VkTraceRaysIndirectCommand2KHR, width),
   };

   cmd_buffer_trace_rays(cmd_buffer, &params);
}

#endif /* GFX_VERx10 >= 125 */
