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
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "util/mesa-sha1.h"
#include "util/os_time.h"
#include "common/intel_compute_slm.h"
#include "common/intel_sample_positions.h"
#include "compiler/brw_disasm.h"
#include "anv_private.h"
#include "compiler/brw_nir.h"
#include "compiler/brw_nir_rt.h"
#include "compiler/intel_nir.h"
#include "anv_nir.h"
#include "nir/nir_xfb_info.h"
#include "spirv/nir_spirv.h"
#include "vk_nir_convert_ycbcr.h"
#include "vk_nir.h"
#include "vk_pipeline.h"
#include "vk_render_pass.h"
#include "vk_util.h"

/* Eventually, this will become part of anv_CreateShader.  Unfortunately,
 * we can't do that yet because we don't have the ability to copy nir.
 */
static nir_shader *
anv_shader_stage_to_nir(struct anv_device *device,
                        VkPipelineCreateFlags2KHR pipeline_flags,
                        const VkPipelineShaderStageCreateInfo *stage_info,
                        enum brw_robustness_flags robust_flags,
                        void *mem_ctx)
{
   const struct anv_physical_device *pdevice = device->physical;
   const struct brw_compiler *compiler = pdevice->compiler;
   gl_shader_stage stage = vk_to_mesa_shader_stage(stage_info->stage);
   const nir_shader_compiler_options *nir_options =
      compiler->nir_options[stage];

   const struct spirv_to_nir_options spirv_options = {
      .ubo_addr_format = anv_nir_ubo_addr_format(pdevice, robust_flags),
      .ssbo_addr_format = anv_nir_ssbo_addr_format(pdevice, robust_flags),
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .push_const_addr_format = nir_address_format_logical,

      /* TODO: Consider changing this to an address format that has the NULL
       * pointer equals to 0.  That might be a better format to play nice
       * with certain code / code generators.
       */
      .shared_addr_format = nir_address_format_32bit_offset,

      .min_ubo_alignment = ANV_UBO_ALIGNMENT,
      .min_ssbo_alignment = ANV_SSBO_ALIGNMENT,
      .workarounds = {
         .lower_terminate_to_discard = pdevice->instance->lower_terminate_to_discard,
      },
   };

   nir_shader *nir;
   VkResult result =
      vk_pipeline_shader_stage_to_nir(&device->vk, pipeline_flags, stage_info,
                                      &spirv_options, nir_options,
                                      mem_ctx, &nir);
   if (result != VK_SUCCESS)
      return NULL;

   if (INTEL_DEBUG(intel_debug_flag_for_shader_stage(stage))) {
      /* src_hash is unknown at the point */
      if (!intel_shader_dump_filter) {
         fprintf(stderr, "NIR (from SPIR-V) for %s shader:\n",
                 gl_shader_stage_name(stage));
         nir_print_shader(nir, stderr);
      }
   }

   NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries,
              nir_shader_get_entrypoint(nir), true, false);

   return nir;
}

static VkResult
anv_pipeline_init(struct anv_pipeline *pipeline,
                  struct anv_device *device,
                  enum anv_pipeline_type type,
                  VkPipelineCreateFlags2KHR flags,
                  const VkAllocationCallbacks *pAllocator)
{
   VkResult result;

   memset(pipeline, 0, sizeof(*pipeline));

   vk_object_base_init(&device->vk, &pipeline->base,
                       VK_OBJECT_TYPE_PIPELINE);
   pipeline->device = device;

   /* It's the job of the child class to provide actual backing storage for
    * the batch by setting batch.start, batch.next, and batch.end.
    */
   pipeline->batch.alloc = pAllocator ? pAllocator : &device->vk.alloc;
   pipeline->batch.relocs = &pipeline->batch_relocs;
   pipeline->batch.status = VK_SUCCESS;

   const bool uses_relocs = device->physical->uses_relocs;
   result = anv_reloc_list_init(&pipeline->batch_relocs,
                                pipeline->batch.alloc, uses_relocs);
   if (result != VK_SUCCESS)
      return result;

   pipeline->mem_ctx = ralloc_context(NULL);

   pipeline->type = type;
   pipeline->flags = flags;

   util_dynarray_init(&pipeline->executables, pipeline->mem_ctx);

   anv_pipeline_sets_layout_init(&pipeline->layout, device,
                                 false /* independent_sets */);

   return VK_SUCCESS;
}

static void
anv_pipeline_init_layout(struct anv_pipeline *pipeline,
                         struct vk_pipeline_layout *pipeline_layout)
{
   if (pipeline_layout) {
      for (uint32_t s = 0; s < pipeline_layout->set_count; s++) {
         if (pipeline_layout->set_layouts[s] == NULL)
            continue;

         struct anv_descriptor_set_layout *set_layout =
            (struct anv_descriptor_set_layout *) pipeline_layout->set_layouts[s];
         anv_pipeline_sets_layout_add(&pipeline->layout, s, set_layout);
      }
   }

   anv_pipeline_sets_layout_hash(&pipeline->layout);
}

static void
anv_pipeline_finish(struct anv_pipeline *pipeline,
                    struct anv_device *device)
{
   anv_pipeline_sets_layout_fini(&pipeline->layout);
   anv_reloc_list_finish(&pipeline->batch_relocs);
   ralloc_free(pipeline->mem_ctx);
   vk_object_base_finish(&pipeline->base);
}

void anv_DestroyPipeline(
    VkDevice                                    _device,
    VkPipeline                                  _pipeline,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline, pipeline, _pipeline);

   if (!pipeline)
      return;

   ANV_RMV(resource_destroy, device, pipeline);

   switch (pipeline->type) {
   case ANV_PIPELINE_GRAPHICS:
   case ANV_PIPELINE_GRAPHICS_LIB: {
      struct anv_graphics_base_pipeline *gfx_pipeline =
         anv_pipeline_to_graphics_base(pipeline);

      for (unsigned s = 0; s < ARRAY_SIZE(gfx_pipeline->shaders); s++) {
         if (gfx_pipeline->shaders[s])
            anv_shader_bin_unref(device, gfx_pipeline->shaders[s]);
      }
      break;
   }

   case ANV_PIPELINE_COMPUTE: {
      struct anv_compute_pipeline *compute_pipeline =
         anv_pipeline_to_compute(pipeline);

      if (compute_pipeline->cs)
         anv_shader_bin_unref(device, compute_pipeline->cs);

      break;
   }

   case ANV_PIPELINE_RAY_TRACING: {
      struct anv_ray_tracing_pipeline *rt_pipeline =
         anv_pipeline_to_ray_tracing(pipeline);

      util_dynarray_foreach(&rt_pipeline->shaders,
                            struct anv_shader_bin *, shader) {
         anv_shader_bin_unref(device, *shader);
      }
      break;
   }

   default:
      UNREACHABLE("invalid pipeline type");
   }

   anv_pipeline_finish(pipeline, device);
   vk_free2(&device->vk.alloc, pAllocator, pipeline);
}

struct anv_pipeline_stage {
   gl_shader_stage stage;

   VkPipelineCreateFlags2KHR pipeline_flags;
   struct vk_pipeline_robustness_state rstate;

   /* VkComputePipelineCreateInfo, VkGraphicsPipelineCreateInfo or
    * VkRayTracingPipelineCreateInfoKHR pNext field
    */
   const void *pipeline_pNext;
   const VkPipelineShaderStageCreateInfo *info;

   unsigned char shader_sha1[20];
   uint32_t      source_hash;

   union brw_any_prog_key key;

   struct {
      gl_shader_stage stage;
      unsigned char sha1[20];
   } cache_key;

   nir_shader *nir;

   struct {
      nir_shader *nir;
      struct anv_shader_bin *bin;
   } imported;

   struct anv_push_descriptor_info push_desc_info;

   enum gl_subgroup_size subgroup_size_type;

   enum brw_robustness_flags robust_flags;

   struct anv_pipeline_bind_map bind_map;

   bool uses_bt_for_push_descs;

   union brw_any_prog_data prog_data;

   uint32_t num_stats;
   struct brw_compile_stats stats[3];
   char *disasm[3];

   VkPipelineCreationFeedback feedback;
   uint32_t feedback_idx;

   const unsigned *code;

   struct anv_shader_bin *bin;
};

static void
anv_stage_allocate_bind_map_tables(struct anv_pipeline *pipeline,
                                   struct anv_pipeline_stage *stage,
                                   void *mem_ctx)
{
   struct anv_pipeline_binding *surface_bindings =
      brw_shader_stage_requires_bindless_resources(stage->stage) ? NULL :
      rzalloc_array(mem_ctx, struct anv_pipeline_binding, 256);
   struct anv_pipeline_binding *sampler_bindings =
      brw_shader_stage_requires_bindless_resources(stage->stage) ? NULL :
      rzalloc_array(mem_ctx, struct anv_pipeline_binding, 256);
   struct anv_pipeline_embedded_sampler_binding *embedded_sampler_bindings =
      rzalloc_array(mem_ctx, struct anv_pipeline_embedded_sampler_binding,
                    anv_pipeline_sets_layout_embedded_sampler_count(
                       &pipeline->layout));

   stage->bind_map = (struct anv_pipeline_bind_map) {
      .layout_type = pipeline->layout.type,
      .surface_to_descriptor = surface_bindings,
      .sampler_to_descriptor = sampler_bindings,
      .embedded_sampler_to_binding = embedded_sampler_bindings,
   };
}

enum brw_robustness_flags
anv_get_robust_flags(const struct vk_pipeline_robustness_state *rstate)
{
   return
      ((rstate->storage_buffers !=
        VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT) ?
       BRW_ROBUSTNESS_SSBO : 0) |
      ((rstate->uniform_buffers !=
        VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT) ?
       BRW_ROBUSTNESS_UBO : 0);
}

static void
populate_base_prog_key(struct anv_pipeline_stage *stage,
                       const struct anv_device *device,
                       const enum intel_vue_layout vue_layout)
{
   stage->key.base.robust_flags = anv_get_robust_flags(&stage->rstate);
   stage->key.base.vue_layout = vue_layout;
   stage->key.base.limit_trig_input_range =
      device->physical->instance->limit_trig_input_range;
}

static void
populate_vs_prog_key(struct anv_pipeline_stage *stage,
                     const struct anv_device *device,
                     const enum intel_vue_layout vue_layout)
{
   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, vue_layout);

   stage->key.vs.vf_component_packing =
      device->physical->instance->vf_component_packing;
}

static void
populate_tcs_prog_key(struct anv_pipeline_stage *stage,
                      const struct anv_device *device,
                      unsigned input_vertices,
                      const enum intel_vue_layout vue_layout)
{
   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, vue_layout);

   stage->key.tcs.input_vertices = input_vertices;
}

static void
populate_tes_prog_key(struct anv_pipeline_stage *stage,
                      const struct anv_device *device,
                      const enum intel_vue_layout vue_layout)
{
   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, vue_layout);
}

static void
populate_gs_prog_key(struct anv_pipeline_stage *stage,
                     const struct anv_device *device,
                     const enum intel_vue_layout vue_layout)
{
   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, vue_layout);
}

static bool
pipeline_has_coarse_pixel(const BITSET_WORD *dynamic,
                          const struct vk_multisample_state *ms,
                          const struct vk_fragment_shading_rate_state *fsr)
{
   /* The Vulkan 1.2.199 spec says:
    *
    *    "If any of the following conditions are met, Cxy' must be set to
    *    {1,1}:
    *
    *     * If Sample Shading is enabled.
    *     * [...]"
    *
    * And "sample shading" is defined as follows:
    *
    *    "Sample shading is enabled for a graphics pipeline:
    *
    *     * If the interface of the fragment shader entry point of the
    *       graphics pipeline includes an input variable decorated with
    *       SampleId or SamplePosition. In this case minSampleShadingFactor
    *       takes the value 1.0.
    *
    *     * Else if the sampleShadingEnable member of the
    *       VkPipelineMultisampleStateCreateInfo structure specified when
    *       creating the graphics pipeline is set to VK_TRUE. In this case
    *       minSampleShadingFactor takes the value of
    *       VkPipelineMultisampleStateCreateInfo::minSampleShading.
    *
    *    Otherwise, sample shading is considered disabled."
    *
    * The first bullet above is handled by the back-end compiler because those
    * inputs both force per-sample dispatch.  The second bullet is handled
    * here.  Note that this sample shading being enabled has nothing to do
    * with minSampleShading.
    */
   if (ms != NULL && ms->sample_shading_enable)
      return false;

   /* Not dynamic & pipeline has a 1x1 fragment shading rate with no
    * possibility for element of the pipeline to change the value or fragment
    * shading rate not specified at all.
    */
   if (!BITSET_TEST(dynamic, MESA_VK_DYNAMIC_FSR) &&
       (fsr == NULL ||
        (fsr->fragment_size.width <= 1 &&
         fsr->fragment_size.height <= 1 &&
         fsr->combiner_ops[0] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR &&
         fsr->combiner_ops[1] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR)))
      return false;

   return true;
}

static void
populate_task_prog_key(struct anv_pipeline_stage *stage,
                       const struct anv_device *device)
{
   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, INTEL_VUE_LAYOUT_FIXED);

   stage->key.base.uses_inline_push_addr = true;
}

static void
populate_mesh_prog_key(struct anv_pipeline_stage *stage,
                       const struct anv_device *device,
                       const enum intel_vue_layout vue_layout)
{
   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, vue_layout);

   stage->key.base.uses_inline_push_addr = true;
}

static uint32_t
rp_color_mask(const struct vk_render_pass_state *rp)
{
   if (rp == NULL || !vk_render_pass_state_has_attachment_info(rp))
      return ((1u << MAX_RTS) - 1);

   uint32_t color_mask = 0;
   for (uint32_t i = 0; i < rp->color_attachment_count; i++) {
      if (rp->color_attachment_formats[i] != VK_FORMAT_UNDEFINED)
         color_mask |= BITFIELD_BIT(i);
   }

   return color_mask;
}

static void
populate_wm_prog_key(struct anv_pipeline_stage *stage,
                     const struct anv_graphics_base_pipeline *pipeline,
                     const BITSET_WORD *dynamic,
                     const struct vk_multisample_state *ms,
                     const struct vk_rasterization_state *rs,
                     const struct vk_fragment_shading_rate_state *fsr,
                     const struct vk_render_pass_state *rp,
                     const enum intel_sometimes is_mesh,
                     const enum intel_vue_layout vue_layout)
{
   const struct anv_device *device = pipeline->base.device;

   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, vue_layout);

   struct brw_wm_prog_key *key = &stage->key.wm;

   /* We set this to 0 here and set to the actual value before we call
    * brw_compile_fs.
    */
   key->input_slots_valid = 0;

   /* XXX Vulkan doesn't appear to specify */
   key->clamp_fragment_color = false;

   key->ignore_sample_mask_out = false;

   assert(rp == NULL || rp->color_attachment_count <= MAX_RTS);
   /* Consider all inputs as valid until look at the NIR variables. */
   key->color_outputs_valid = rp_color_mask(rp);
   key->nr_color_regions = util_last_bit(key->color_outputs_valid);

   /* To reduce possible shader recompilations we would need to know if
    * there is a SampleMask output variable to compute if we should emit
    * code to workaround the issue that hardware disables alpha to coverage
    * when there is SampleMask output.
    *
    * If the pipeline we compile the fragment shader in includes the output
    * interface, then we can be sure whether alpha_coverage is enabled or not.
    * If we don't have that output interface, then we have to compile the
    * shader with some conditionals.
    */
   if (ms != NULL) {
      /* VUID-VkGraphicsPipelineCreateInfo-rasterizerDiscardEnable-00751:
       *
       *   "If the pipeline is being created with fragment shader state,
       *    pMultisampleState must be a valid pointer to a valid
       *    VkPipelineMultisampleStateCreateInfo structure"
       *
       * It's also required for the fragment output interface.
       */
      key->multisample_fbo =
         BITSET_TEST(dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES) ?
         INTEL_SOMETIMES :
         ms->rasterization_samples > 1 ? INTEL_ALWAYS : INTEL_NEVER;
      key->persample_interp =
         BITSET_TEST(dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES) ?
         INTEL_SOMETIMES :
         (ms->sample_shading_enable &&
          (ms->min_sample_shading * ms->rasterization_samples) > 1) ?
         INTEL_ALWAYS : INTEL_NEVER;
      key->alpha_to_coverage =
         BITSET_TEST(dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE) ?
         INTEL_SOMETIMES :
         (ms->alpha_to_coverage_enable ? INTEL_ALWAYS : INTEL_NEVER);

      /* TODO: We should make this dynamic */
      if (device->physical->instance->sample_mask_out_opengl_behaviour)
         key->ignore_sample_mask_out = !key->multisample_fbo;
   } else {
      /* Consider all inputs as valid until we look at the NIR variables. */
      key->color_outputs_valid = (1u << MAX_RTS) - 1;
      key->nr_color_regions = MAX_RTS;

      key->alpha_to_coverage = INTEL_SOMETIMES;
      key->multisample_fbo = INTEL_SOMETIMES;
      key->persample_interp = INTEL_SOMETIMES;
   }

   if (device->info->verx10 >= 200) {
      if (rs != NULL) {
         key->provoking_vertex_last =
            BITSET_TEST(dynamic, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX) ?
            INTEL_SOMETIMES :
            rs->provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ?
            INTEL_ALWAYS : INTEL_NEVER;
      } else {
         key->provoking_vertex_last = INTEL_SOMETIMES;
      }
   } else {
      /* Pre-Xe2 we don't care about this at all, make sure it's always set to
       * NEVER to avoid it influencing the push constant.
       */
      key->provoking_vertex_last = INTEL_NEVER;
   }

   key->mesh_input = is_mesh;

   /* Vulkan doesn't support fixed-function alpha test */
   key->alpha_test_replicate_alpha = false;

  key->coarse_pixel =
     device->vk.enabled_extensions.KHR_fragment_shading_rate &&
     pipeline_has_coarse_pixel(dynamic, ms, fsr);

  key->null_push_constant_tbimr_workaround =
     device->info->needs_null_push_constant_tbimr_workaround;
}

static void
populate_cs_prog_key(struct anv_pipeline_stage *stage,
                     const struct anv_device *device)
{
   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, INTEL_VUE_LAYOUT_FIXED);

   stage->key.base.uses_inline_push_addr = device->info->verx10 >= 125;
}

static void
populate_bs_prog_key(struct anv_pipeline_stage *stage,
                     const struct anv_device *device,
                     uint32_t ray_flags)
{
   memset(&stage->key, 0, sizeof(stage->key));

   populate_base_prog_key(stage, device, INTEL_VUE_LAYOUT_FIXED);

   stage->key.bs.pipeline_ray_flags = ray_flags;
   stage->key.bs.pipeline_ray_flags = ray_flags;
}

static void
anv_stage_write_shader_hash(struct anv_pipeline_stage *stage,
                            const struct anv_device *device)
{
   vk_pipeline_robustness_state_fill(&device->vk,
                                     &stage->rstate,
                                     stage->pipeline_pNext,
                                     stage->info->pNext);

   vk_pipeline_hash_shader_stage(stage->pipeline_flags, stage->info,
                                 &stage->rstate, stage->shader_sha1);

   stage->robust_flags = anv_get_robust_flags(&stage->rstate);

   /* Use lowest dword of source shader sha1 for shader hash. */
   stage->source_hash = ((uint32_t*)stage->shader_sha1)[0];
}

static bool
anv_graphics_pipeline_stage_fragment_dynamic(const struct anv_pipeline_stage *stage)
{
   return stage->stage == MESA_SHADER_FRAGMENT &&
          brw_wm_prog_key_is_dynamic(&stage->key.wm);
}

static bool
anv_graphics_pipeline_stage_mesh_dynamic(const struct anv_pipeline_stage *stage)
{
   return stage->stage == MESA_SHADER_FRAGMENT &&
          stage->key.wm.mesh_input == INTEL_SOMETIMES;
}

static void
anv_pipeline_hash_common(struct mesa_sha1 *ctx,
                         const struct anv_pipeline *pipeline)
{
   struct anv_device *device = pipeline->device;

   _mesa_sha1_update(ctx, pipeline->layout.blake3, sizeof(pipeline->layout.blake3));

   const bool indirect_descriptors = device->physical->indirect_descriptors;
   _mesa_sha1_update(ctx, &indirect_descriptors, sizeof(indirect_descriptors));

   const bool lower_terminate_to_discard =
      device->physical->instance->lower_terminate_to_discard;
   _mesa_sha1_update(ctx, &lower_terminate_to_discard,
                     sizeof(lower_terminate_to_discard));

   const bool rba = device->robust_buffer_access;
   _mesa_sha1_update(ctx, &rba, sizeof(rba));

   const int spilling_rate = device->physical->compiler->spilling_rate;
   _mesa_sha1_update(ctx, &spilling_rate, sizeof(spilling_rate));

   const bool erwf = device->physical->instance->emulate_read_without_format;
   _mesa_sha1_update(ctx, &erwf, sizeof(erwf));
}

static void
anv_pipeline_hash_graphics(struct anv_graphics_base_pipeline *pipeline,
                           struct anv_pipeline_stage *stages,
                           uint32_t view_mask,
                           unsigned char *sha1_out)
{
   const struct anv_device *device = pipeline->base.device;
   struct mesa_sha1 ctx;
   _mesa_sha1_init(&ctx);

   anv_pipeline_hash_common(&ctx, &pipeline->base);

   _mesa_sha1_update(&ctx, &view_mask, sizeof(view_mask));

   for (uint32_t s = 0; s < ANV_GRAPHICS_SHADER_STAGE_COUNT; s++) {
      if (pipeline->base.active_stages & BITFIELD_BIT(s)) {
         _mesa_sha1_update(&ctx, stages[s].shader_sha1,
                           sizeof(stages[s].shader_sha1));
         _mesa_sha1_update(&ctx, &stages[s].key, brw_prog_key_size(s));
      }
   }

   if (stages[MESA_SHADER_MESH].info || stages[MESA_SHADER_TASK].info) {
      const uint8_t afs = device->physical->instance->assume_full_subgroups;
      _mesa_sha1_update(&ctx, &afs, sizeof(afs));

      const bool afs_shm = device->physical->instance->assume_full_subgroups_with_shared_memory;
      _mesa_sha1_update(&ctx, &afs_shm, sizeof(afs_shm));
   }

   _mesa_sha1_final(&ctx, sha1_out);
}

static void
anv_pipeline_hash_compute(struct anv_compute_pipeline *pipeline,
                          struct anv_pipeline_stage *stage,
                          unsigned char *sha1_out)
{
   const struct anv_device *device = pipeline->base.device;
   struct mesa_sha1 ctx;
   _mesa_sha1_init(&ctx);

   anv_pipeline_hash_common(&ctx, &pipeline->base);

   const uint8_t afs = device->physical->instance->assume_full_subgroups;
   _mesa_sha1_update(&ctx, &afs, sizeof(afs));

   const bool afswb = device->physical->instance->assume_full_subgroups_with_barrier;
   _mesa_sha1_update(&ctx, &afswb, sizeof(afswb));

   const bool afs_shm = device->physical->instance->assume_full_subgroups_with_shared_memory;
   _mesa_sha1_update(&ctx, &afs_shm, sizeof(afs_shm));

   _mesa_sha1_update(&ctx, stage->shader_sha1,
                     sizeof(stage->shader_sha1));
   _mesa_sha1_update(&ctx, &stage->key.cs, sizeof(stage->key.cs));

   _mesa_sha1_final(&ctx, sha1_out);
}

static void
anv_pipeline_hash_ray_tracing_shader(struct anv_ray_tracing_pipeline *pipeline,
                                     struct anv_pipeline_stage *stage,
                                     unsigned char *sha1_out)
{
   struct mesa_sha1 ctx;
   _mesa_sha1_init(&ctx);

   anv_pipeline_hash_common(&ctx, &pipeline->base);

   _mesa_sha1_update(&ctx, stage->shader_sha1, sizeof(stage->shader_sha1));
   _mesa_sha1_update(&ctx, &stage->key, sizeof(stage->key.bs));

   _mesa_sha1_final(&ctx, sha1_out);
}

static void
anv_pipeline_hash_ray_tracing_combined_shader(struct anv_ray_tracing_pipeline *pipeline,
                                              struct anv_pipeline_stage *intersection,
                                              struct anv_pipeline_stage *any_hit,
                                              unsigned char *sha1_out)
{
   struct mesa_sha1 ctx;
   _mesa_sha1_init(&ctx);

   _mesa_sha1_update(&ctx, pipeline->base.layout.blake3,
                     sizeof(pipeline->base.layout.blake3));

   const bool rba = pipeline->base.device->robust_buffer_access;
   _mesa_sha1_update(&ctx, &rba, sizeof(rba));

   _mesa_sha1_update(&ctx, intersection->shader_sha1, sizeof(intersection->shader_sha1));
   _mesa_sha1_update(&ctx, &intersection->key, sizeof(intersection->key.bs));
   _mesa_sha1_update(&ctx, any_hit->shader_sha1, sizeof(any_hit->shader_sha1));
   _mesa_sha1_update(&ctx, &any_hit->key, sizeof(any_hit->key.bs));

   _mesa_sha1_final(&ctx, sha1_out);
}

static VkResult
anv_pipeline_stage_get_nir(struct anv_pipeline *pipeline,
                           struct vk_pipeline_cache *cache,
                           void *mem_ctx,
                           struct anv_pipeline_stage *stage)
{
   const struct brw_compiler *compiler =
      pipeline->device->physical->compiler;
   const nir_shader_compiler_options *nir_options =
      compiler->nir_options[stage->stage];

   stage->nir = anv_device_search_for_nir(pipeline->device, cache,
                                          nir_options,
                                          stage->shader_sha1,
                                          mem_ctx);
   if (stage->nir) {
      assert(stage->nir->info.stage == stage->stage);
      return VK_SUCCESS;
   }

   /* VkPipelineShaderStageCreateInfo:
    *
    *    "If a pipeline is not found, pipeline compilation is not possible and
    *     the implementation must fail as specified by
    *     VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT."
    */
   if (vk_pipeline_shader_stage_has_identifier(stage->info))
      return VK_PIPELINE_COMPILE_REQUIRED;

   stage->nir = anv_shader_stage_to_nir(pipeline->device,
                                        stage->pipeline_flags, stage->info,
                                        stage->key.base.robust_flags, mem_ctx);
   if (stage->nir) {
      anv_device_upload_nir(pipeline->device, cache,
                            stage->nir, stage->shader_sha1);
      return VK_SUCCESS;
   }

   return vk_errorf(&pipeline->device->vk, VK_ERROR_UNKNOWN,
                    "Unable to load NIR");
}

static const struct vk_ycbcr_conversion_state *
lookup_ycbcr_conversion(const void *_sets_layout, uint32_t set,
                        uint32_t binding, uint32_t array_index)
{
   const struct anv_pipeline_sets_layout *sets_layout = _sets_layout;

   assert(set < MAX_SETS);
   assert(binding < sets_layout->set_layouts[set]->binding_count);
   const struct anv_descriptor_set_binding_layout *bind_layout =
      &sets_layout->set_layouts[set]->binding[binding];

   if (bind_layout->samplers == NULL)
      return NULL;

   array_index = MIN2(array_index, bind_layout->array_size - 1);

   const struct anv_descriptor_set_layout_sampler *sampler =
      &bind_layout->samplers[array_index];

   return sampler->has_ycbcr_conversion ?
          &sampler->ycbcr_conversion_state : NULL;
}

static void
shared_type_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size = glsl_type_is_boolean(type)
      ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length,
   *align = comp_size * (length == 3 ? 4 : length);
}

static void
anv_fixup_subgroup_size(struct anv_device *device, struct shader_info *info)
{
   switch (info->stage) {
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_TASK:
   case MESA_SHADER_MESH:
      break;
   default:
      return;
   }

   unsigned local_size = info->workgroup_size[0] *
                         info->workgroup_size[1] *
                         info->workgroup_size[2];

   /* Games don't always request full subgroups when they should,
    * which can cause bugs, as they may expect bigger size of the
    * subgroup than we choose for the execution.
    */
   if (device->physical->instance->assume_full_subgroups &&
       info->uses_wide_subgroup_intrinsics &&
       info->subgroup_size == SUBGROUP_SIZE_API_CONSTANT &&
       local_size &&
       local_size % BRW_SUBGROUP_SIZE == 0)
      info->subgroup_size = SUBGROUP_SIZE_FULL_SUBGROUPS;

   if (device->physical->instance->assume_full_subgroups_with_barrier &&
       info->stage == MESA_SHADER_COMPUTE &&
       device->info->verx10 <= 125 &&
       info->uses_control_barrier &&
       info->subgroup_size == SUBGROUP_SIZE_VARYING &&
       local_size &&
       local_size % BRW_SUBGROUP_SIZE == 0)
      info->subgroup_size = SUBGROUP_SIZE_FULL_SUBGROUPS;

   /* Similarly, sometimes games rely on the implicit synchronization of
    * the shared memory accesses, and choosing smaller subgroups than the game
    * expects will cause bugs. */
   if (device->physical->instance->assume_full_subgroups_with_shared_memory &&
       info->shared_size > 0 &&
       info->subgroup_size == SUBGROUP_SIZE_VARYING &&
       local_size &&
       local_size % BRW_SUBGROUP_SIZE == 0)
      info->subgroup_size = SUBGROUP_SIZE_FULL_SUBGROUPS;

   /* If the client requests that we dispatch full subgroups but doesn't
    * allow us to pick a subgroup size, we have to smash it to the API
    * value of 32.  Performance will likely be terrible in this case but
    * there's nothing we can do about that.  The client should have chosen
    * a size.
    */
   if (info->subgroup_size == SUBGROUP_SIZE_FULL_SUBGROUPS)
      info->subgroup_size =
         device->physical->instance->assume_full_subgroups != 0 ?
         device->physical->instance->assume_full_subgroups : BRW_SUBGROUP_SIZE;

   /* Cooperative matrix extension requires that all invocations in a subgroup
    * be active. As a result, when the application does not request a specific
    * subgroup size, we must use SIMD32.
    */
   if (info->stage == MESA_SHADER_COMPUTE && info->cs.has_cooperative_matrix &&
       info->subgroup_size < SUBGROUP_SIZE_REQUIRE_8) {
      info->subgroup_size = BRW_SUBGROUP_SIZE;
   }
}

/* #define DEBUG_PRINTF_EXAMPLE 0 */

#if DEBUG_PRINTF_EXAMPLE
static bool
print_ubo_load(nir_builder *b,
               nir_intrinsic_instr *intrin,
               UNUSED void *cb_data)
{
   if (intrin->intrinsic != nir_intrinsic_load_uniform)
      return false;

   b->cursor = nir_after_instr(&intrin->instr);
   nir_printf_fmt(b, 64,
                  "uniform<= pos=%02.2fx%02.2f offset=0x%08x val=0x%08x\n",
                  nir_channel(b, nir_load_frag_coord(b), 0),
                  nir_channel(b, nir_load_frag_coord(b), 1),
                  intrin->src[0].ssa,
                  &intrin->def);
   return true;
}
#endif

static bool
accept_64bit_atomic_cb(const nir_intrinsic_instr *intrin, const void *data)
{
   return (intrin->intrinsic == nir_intrinsic_image_atomic ||
           intrin->intrinsic == nir_intrinsic_image_atomic_swap ||
           intrin->intrinsic == nir_intrinsic_image_deref_atomic ||
           intrin->intrinsic == nir_intrinsic_image_deref_atomic_swap) &&
          intrin->def.bit_size == 64;
}

static bool
lower_non_tg4_non_uniform_offsets(const nir_tex_instr *tex,
                                  unsigned index, void *data)
{
   /* HW cannot deal with divergent surfaces/samplers */
   if (tex->src[index].src_type == nir_tex_src_texture_offset ||
       tex->src[index].src_type == nir_tex_src_texture_handle ||
       tex->src[index].src_type == nir_tex_src_sampler_offset ||
       tex->src[index].src_type == nir_tex_src_sampler_handle)
      return true;

   if (tex->src[index].src_type == nir_tex_src_offset) {
      /* HW can deal with TG4 divergent offsets only */
      return tex->op != nir_texop_tg4;
   }

   return false;
}

static nir_def *
build_tcs_input_vertices(nir_builder *b, nir_instr *instr, void *data)
{
   return anv_load_driver_uniform(b, 1, gfx.tcs_input_vertices);
}

static void
anv_pipeline_lower_nir(struct anv_pipeline *pipeline,
                       void *mem_ctx,
                       struct anv_pipeline_stage *stage,
                       struct anv_pipeline_sets_layout *layout,
                       uint32_t view_mask,
                       bool use_primitive_replication)
{
   const struct anv_physical_device *pdevice = pipeline->device->physical;
   const struct brw_compiler *compiler = pdevice->compiler;

   struct brw_stage_prog_data *prog_data = &stage->prog_data.base;
   nir_shader *nir = stage->nir;

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, nir_lower_wpos_center);
      NIR_PASS(_, nir, nir_lower_input_attachments,
               &(nir_input_attachment_options) {
                   .use_fragcoord_sysval = true,
                   .use_layer_id_sysval = true,
               });
   }

   if (gl_shader_stage_is_mesh(nir->info.stage)) {
      nir_lower_compute_system_values_options options = {
            .lower_workgroup_id_to_index = true,
            /* nir_lower_idiv generates expensive code */
            .shortcut_1d_workgroup_id = compiler->devinfo->verx10 >= 125,
      };

      NIR_PASS(_, nir, nir_lower_compute_system_values, &options);
   }

   NIR_PASS(_, nir, nir_vk_lower_ycbcr_tex, lookup_ycbcr_conversion, layout);

   if (pipeline->type == ANV_PIPELINE_GRAPHICS ||
       pipeline->type == ANV_PIPELINE_GRAPHICS_LIB) {
      NIR_PASS(_, nir, anv_nir_lower_multiview, view_mask,
               use_primitive_replication);
   }

   if (nir->info.stage == MESA_SHADER_COMPUTE && nir->info.cs.has_cooperative_matrix) {
      anv_fixup_subgroup_size(pipeline->device, &nir->info);
      NIR_PASS(_, nir, brw_nir_lower_cmat, nir->info.subgroup_size);
      NIR_PASS(_, nir, nir_lower_indirect_derefs, nir_var_function_temp, 16);
   }

   /* The patch control points are delivered through a push constant when
    * dynamic.
    */
   if (nir->info.stage == MESA_SHADER_TESS_CTRL) {
      NIR_PASS(_, nir, intel_nir_lower_patch_vertices_in,
               stage->key.tcs.input_vertices,
               build_tcs_input_vertices, NULL);
   }

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   /* Apply lowering for 64bit atomics pre-Xe2 */
   const bool lower_64bit_atomics = compiler->devinfo->ver < 20;
   if (lower_64bit_atomics) {
      /* Ensure robustness, do this before brw_nir_lower_storage_image so that
       * added image size intrinsics for bounds checkings are properly lowered
       * for cube images.
       */
      NIR_PASS(_, nir, nir_lower_robust_access,
               accept_64bit_atomic_cb, NULL);
   }

   NIR_PASS(_, nir, brw_nir_lower_storage_image, compiler,
            &(struct brw_nir_lower_storage_image_opts) {
               /* Anv only supports Gfx9+ which has better defined typed
                * read behavior.
                */
               .lower_loads = true,
               .lower_stores_64bit = true,
               .lower_loads_without_formats =
                  pdevice->instance->emulate_read_without_format,
            });

   if (lower_64bit_atomics) {
      /* Switch from image to global */
      NIR_PASS(_, nir, nir_lower_image_atomics_to_global,
               accept_64bit_atomic_cb, NULL);

      /* Detile for global */
      NIR_PASS(_, nir, brw_nir_lower_texel_address, compiler->devinfo,
               pdevice->isl_dev.shader_tiling);
   }

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
            nir_address_format_64bit_global);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);

   NIR_PASS(_, nir, brw_nir_lower_ray_queries, &pdevice->info);

   stage->push_desc_info.used_descriptors =
      anv_nir_compute_used_push_descriptors(
         nir, layout->set_layouts, layout->num_sets);

   struct anv_pipeline_push_map push_map = {};

   /* Apply the actual pipeline layout to UBOs, SSBOs, and textures */
   NIR_PASS(_, nir, anv_nir_apply_pipeline_layout,
              pdevice, stage->key.base.robust_flags,
              layout->set_layouts, layout->num_sets,
              layout->independent_sets ? NULL : layout->dynamic_offset_start,
              &stage->bind_map, &push_map, mem_ctx);

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,
            anv_nir_ubo_addr_format(pdevice, stage->key.base.robust_flags));
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
            anv_nir_ssbo_addr_format(pdevice, stage->key.base.robust_flags));

   /* First run copy-prop to get rid of all of the vec() that address
    * calculations often create and then constant-fold so that, when we
    * get to anv_nir_lower_ubo_loads, we can detect constant offsets.
    */
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dce);
   } while (progress);

   NIR_PASS(_, nir, anv_nir_lower_ubo_loads);

   enum nir_lower_non_uniform_access_type lower_non_uniform_access_types =
      nir_lower_non_uniform_texture_access |
      nir_lower_non_uniform_image_access |
      nir_lower_non_uniform_get_ssbo_size |
      nir_lower_non_uniform_texture_offset_access;

   /* For textures, images, sampler, NonUniform decoration is required but not
    * for offsets, so we rely on divergence information for this. Offsets used
    * to be constants until KHR_maintenance8.
    */
   if (pipeline->device->vk.enabled_features.maintenance8) {
      nir_foreach_function_impl(impl, nir)
         nir_metadata_require(impl, nir_metadata_divergence);
   }

   /* In practice, most shaders do not have non-uniform-qualified
    * accesses (see
    * https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/17558#note_1475069)
    * thus a cheaper and likely to fail check is run first.
    */
   if (nir_has_non_uniform_access(nir, lower_non_uniform_access_types)) {
      NIR_PASS(_, nir, nir_opt_non_uniform_access);

      /* We don't support non-uniform UBOs and non-uniform SSBO access is
      * handled naturally by falling back to A64 messages.
      */
      NIR_PASS(_, nir, nir_lower_non_uniform_access,
               &(nir_lower_non_uniform_access_options) {
                  .types = lower_non_uniform_access_types,
                  .tex_src_callback = lower_non_tg4_non_uniform_offsets,
                  .callback = NULL,
               });

      NIR_PASS(_, nir, intel_nir_lower_non_uniform_resource_intel);
      NIR_PASS(_, nir, intel_nir_cleanup_resource_intel);
      NIR_PASS(_, nir, nir_opt_dce);
   }

   NIR_PASS(_, nir, anv_nir_update_resource_intel_block);

   NIR_PASS(_, nir, anv_nir_compute_push_layout,
              pdevice, stage->key.base.robust_flags,
              anv_graphics_pipeline_stage_fragment_dynamic(stage),
              anv_graphics_pipeline_stage_mesh_dynamic(stage),
              prog_data, &stage->bind_map, &push_map, mem_ctx);

   NIR_PASS(_, nir, anv_nir_lower_resource_intel, pdevice,
               stage->bind_map.layout_type);

   if (gl_shader_stage_uses_workgroup(nir->info.stage)) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types,
               nir_var_mem_shared, shared_type_info);

      NIR_PASS(_, nir, nir_lower_explicit_io,
               nir_var_mem_shared, nir_address_format_32bit_offset);

      if (nir->info.zero_initialize_shared_memory &&
          nir->info.shared_size > 0) {
         /* The effective Shared Local Memory size is at least 1024 bytes and
          * is always rounded to a power of two, so it is OK to align the size
          * used by the shader to chunk_size -- which does simplify the logic.
          */
         const unsigned chunk_size = 16;
         const unsigned shared_size = ALIGN(nir->info.shared_size, chunk_size);
         assert(shared_size <=
                intel_compute_slm_calculate_size(compiler->devinfo->ver, nir->info.shared_size));

         NIR_PASS(_, nir, nir_zero_initialize_shared_memory,
                  shared_size, chunk_size);
      }
   }

   if (gl_shader_stage_is_compute(nir->info.stage) ||
       gl_shader_stage_is_mesh(nir->info.stage)) {
      NIR_PASS(_, nir, brw_nir_lower_cs_intrinsics, compiler->devinfo,
               &stage->prog_data.cs);
   }

   stage->push_desc_info.push_set_buffer =
      anv_nir_loads_push_desc_buffer(nir,
                                     layout->set_layouts,
                                     layout->num_sets,
                                     &stage->bind_map);
   stage->push_desc_info.fully_promoted_ubo_descriptors =
      anv_nir_push_desc_ubo_fully_promoted(nir,
                                           layout->set_layouts,
                                           layout->num_sets,
                                           &stage->bind_map);

#if DEBUG_PRINTF_EXAMPLE
   if (stage->stage == MESA_SHADER_FRAGMENT) {
      nir_shader_intrinsics_pass(nir, print_ubo_load,
                                 nir_metadata_none,
                                 NULL);
   }
#endif

   stage->nir = nir;
}

static void
anv_pipeline_link_vs(const struct brw_compiler *compiler,
                     struct anv_pipeline_stage *vs_stage,
                     struct anv_pipeline_stage *next_stage)
{
   if (next_stage)
      brw_nir_link_shaders(compiler, vs_stage->nir, next_stage->nir);
}

static void
anv_pipeline_compile_vs(const struct brw_compiler *compiler,
                        void *mem_ctx,
                        struct anv_graphics_base_pipeline *pipeline,
                        struct anv_pipeline_stage *vs_stage,
                        uint32_t view_mask,
                        char **error_str)
{
   /* When using Primitive Replication for multiview, each view gets its own
    * position slot.
    */
   uint32_t pos_slots =
      (vs_stage->nir->info.per_view_outputs & VARYING_BIT_POS) ?
      MAX2(1, util_bitcount(view_mask)) : 1;

   /* Only position is allowed to be per-view */
   assert(!(vs_stage->nir->info.per_view_outputs & ~VARYING_BIT_POS));

   brw_compute_vue_map(compiler->devinfo,
                       &vs_stage->prog_data.vs.base.vue_map,
                       vs_stage->nir->info.outputs_written,
                       vs_stage->key.base.vue_layout,
                       pos_slots);

   vs_stage->num_stats = 1;

   struct brw_compile_vs_params params = {
      .base = {
         .nir = vs_stage->nir,
         .stats = vs_stage->stats,
         .log_data = pipeline->base.device,
         .mem_ctx = mem_ctx,
         .source_hash = vs_stage->source_hash,
      },
      .key = &vs_stage->key.vs,
      .prog_data = &vs_stage->prog_data.vs,
   };

   vs_stage->code = brw_compile_vs(compiler, &params);
   *error_str = params.base.error_str;
}

static void
merge_tess_info(struct shader_info *tes_info,
                const struct shader_info *tcs_info)
{
   /* The Vulkan 1.0.38 spec, section 21.1 Tessellator says:
    *
    *    "PointMode. Controls generation of points rather than triangles
    *     or lines. This functionality defaults to disabled, and is
    *     enabled if either shader stage includes the execution mode.
    *
    * and about Triangles, Quads, IsoLines, VertexOrderCw, VertexOrderCcw,
    * PointMode, SpacingEqual, SpacingFractionalEven, SpacingFractionalOdd,
    * and OutputVertices, it says:
    *
    *    "One mode must be set in at least one of the tessellation
    *     shader stages."
    *
    * So, the fields can be set in either the TCS or TES, but they must
    * agree if set in both.  Our backend looks at TES, so bitwise-or in
    * the values from the TCS.
    */
   assert(tcs_info->tess.tcs_vertices_out == 0 ||
          tes_info->tess.tcs_vertices_out == 0 ||
          tcs_info->tess.tcs_vertices_out == tes_info->tess.tcs_vertices_out);
   tes_info->tess.tcs_vertices_out |= tcs_info->tess.tcs_vertices_out;

   assert(tcs_info->tess.spacing == TESS_SPACING_UNSPECIFIED ||
          tes_info->tess.spacing == TESS_SPACING_UNSPECIFIED ||
          tcs_info->tess.spacing == tes_info->tess.spacing);
   tes_info->tess.spacing |= tcs_info->tess.spacing;

   assert(tcs_info->tess._primitive_mode == 0 ||
          tes_info->tess._primitive_mode == 0 ||
          tcs_info->tess._primitive_mode == tes_info->tess._primitive_mode);
   tes_info->tess._primitive_mode |= tcs_info->tess._primitive_mode;
   tes_info->tess.ccw |= tcs_info->tess.ccw;
   tes_info->tess.point_mode |= tcs_info->tess.point_mode;
}

static void
anv_pipeline_link_tcs(const struct brw_compiler *compiler,
                      struct anv_pipeline_stage *tcs_stage,
                      struct anv_pipeline_stage *tes_stage)
{
   assert(tes_stage && tes_stage->stage == MESA_SHADER_TESS_EVAL);

   brw_nir_link_shaders(compiler, tcs_stage->nir, tes_stage->nir);

   nir_lower_patch_vertices(tes_stage->nir,
                            tcs_stage->nir->info.tess.tcs_vertices_out,
                            NULL);

   /* Copy TCS info into the TES info */
   merge_tess_info(&tes_stage->nir->info, &tcs_stage->nir->info);

   /* Whacking the key after cache lookup is a bit sketchy, but all of
    * this comes from the SPIR-V, which is part of the hash used for the
    * pipeline cache.  So it should be safe.
    */
   tcs_stage->key.tcs._tes_primitive_mode =
      tes_stage->nir->info.tess._primitive_mode;
}

static void
anv_pipeline_compile_tcs(const struct brw_compiler *compiler,
                         void *mem_ctx,
                         struct anv_device *device,
                         struct anv_pipeline_stage *tcs_stage,
                         struct anv_pipeline_stage *prev_stage,
                         char **error_str)
{
   tcs_stage->key.tcs.outputs_written =
      tcs_stage->nir->info.outputs_written;
   tcs_stage->key.tcs.patch_outputs_written =
      tcs_stage->nir->info.patch_outputs_written;

   tcs_stage->num_stats = 1;

   struct brw_compile_tcs_params params = {
      .base = {
         .nir = tcs_stage->nir,
         .stats = tcs_stage->stats,
         .log_data = device,
         .mem_ctx = mem_ctx,
         .source_hash = tcs_stage->source_hash,
      },
      .key = &tcs_stage->key.tcs,
      .prog_data = &tcs_stage->prog_data.tcs,
   };

   tcs_stage->code = brw_compile_tcs(compiler, &params);
   *error_str = params.base.error_str;
}

static void
anv_pipeline_link_tes(const struct brw_compiler *compiler,
                      struct anv_pipeline_stage *tes_stage,
                      struct anv_pipeline_stage *next_stage)
{
   if (next_stage)
      brw_nir_link_shaders(compiler, tes_stage->nir, next_stage->nir);
}

static void
anv_pipeline_compile_tes(const struct brw_compiler *compiler,
                         void *mem_ctx,
                         struct anv_device *device,
                         struct anv_pipeline_stage *tes_stage,
                         struct anv_pipeline_stage *tcs_stage,
                         char **error_str)
{
   tes_stage->key.tes.inputs_read =
      tcs_stage->nir->info.outputs_written;
   tes_stage->key.tes.patch_inputs_read =
      tcs_stage->nir->info.patch_outputs_written;

   tes_stage->num_stats = 1;

   struct brw_compile_tes_params params = {
      .base = {
         .nir = tes_stage->nir,
         .stats = tes_stage->stats,
         .log_data = device,
         .mem_ctx = mem_ctx,
         .source_hash = tes_stage->source_hash,
      },
      .key = &tes_stage->key.tes,
      .prog_data = &tes_stage->prog_data.tes,
      .input_vue_map = &tcs_stage->prog_data.tcs.base.vue_map,
   };

   tes_stage->code = brw_compile_tes(compiler, &params);
   *error_str = params.base.error_str;
}

static void
anv_pipeline_link_gs(const struct brw_compiler *compiler,
                     struct anv_pipeline_stage *gs_stage,
                     struct anv_pipeline_stage *next_stage)
{
   if (next_stage)
      brw_nir_link_shaders(compiler, gs_stage->nir, next_stage->nir);
}

static void
anv_pipeline_compile_gs(const struct brw_compiler *compiler,
                        void *mem_ctx,
                        struct anv_device *device,
                        struct anv_pipeline_stage *gs_stage,
                        struct anv_pipeline_stage *prev_stage,
                        char **error_str)
{
   brw_compute_vue_map(compiler->devinfo,
                       &gs_stage->prog_data.gs.base.vue_map,
                       gs_stage->nir->info.outputs_written,
                       gs_stage->key.base.vue_layout, 1);

   gs_stage->num_stats = 1;

   struct brw_compile_gs_params params = {
      .base = {
         .nir = gs_stage->nir,
         .stats = gs_stage->stats,
         .log_data = device,
         .mem_ctx = mem_ctx,
         .source_hash = gs_stage->source_hash,
      },
      .key = &gs_stage->key.gs,
      .prog_data = &gs_stage->prog_data.gs,
   };

   gs_stage->code = brw_compile_gs(compiler, &params);
   *error_str = params.base.error_str;
}

static void
anv_pipeline_link_task(const struct brw_compiler *compiler,
                       struct anv_pipeline_stage *task_stage,
                       struct anv_pipeline_stage *next_stage)
{
   assert(next_stage);
   assert(next_stage->stage == MESA_SHADER_MESH);
   brw_nir_link_shaders(compiler, task_stage->nir, next_stage->nir);
}

static void
anv_pipeline_compile_task(const struct brw_compiler *compiler,
                          void *mem_ctx,
                          struct anv_device *device,
                          struct anv_pipeline_stage *task_stage,
                          char **error_str)
{
   task_stage->num_stats = 1;

   struct brw_compile_task_params params = {
      .base = {
         .nir = task_stage->nir,
         .stats = task_stage->stats,
         .log_data = device,
         .mem_ctx = mem_ctx,
         .source_hash = task_stage->source_hash,
      },
      .key = &task_stage->key.task,
      .prog_data = &task_stage->prog_data.task,
   };

   task_stage->code = brw_compile_task(compiler, &params);
   *error_str = params.base.error_str;
}

static void
anv_pipeline_link_mesh(const struct brw_compiler *compiler,
                       struct anv_pipeline_stage *mesh_stage,
                       struct anv_pipeline_stage *next_stage)
{
   if (next_stage) {
      brw_nir_link_shaders(compiler, mesh_stage->nir, next_stage->nir);
   }
}

static nir_def *
mesh_load_provoking_vertex(nir_builder *b, void *data)
{
   return nir_load_inline_data_intel(
      b, 1, 32,
      .base = ANV_INLINE_PARAM_MESH_PROVOKING_VERTEX);
}

static void
anv_pipeline_compile_mesh(const struct brw_compiler *compiler,
                          void *mem_ctx,
                          struct anv_device *device,
                          struct anv_pipeline_stage *mesh_stage,
                          struct anv_pipeline_stage *prev_stage,
                          char **error_str)
{
   mesh_stage->num_stats = 1;

   struct brw_compile_mesh_params params = {
      .base = {
         .nir = mesh_stage->nir,
         .stats = mesh_stage->stats,
         .log_data = device,
         .mem_ctx = mem_ctx,
         .source_hash = mesh_stage->source_hash,
      },
      .key = &mesh_stage->key.mesh,
      .prog_data = &mesh_stage->prog_data.mesh,
      .load_provoking_vertex = mesh_load_provoking_vertex,
   };

   if (prev_stage) {
      assert(prev_stage->stage == MESA_SHADER_TASK);
      params.tue_map = &prev_stage->prog_data.task.map;
   }

   mesh_stage->code = brw_compile_mesh(compiler, &params);
   *error_str = params.base.error_str;
}

static void
anv_pipeline_link_fs(const struct brw_compiler *compiler,
                     struct anv_pipeline_stage *stage,
                     const struct vk_render_pass_state *rp)
{
   /* Initially the valid outputs value is set to all possible render targets
    * valid (see populate_wm_prog_key()), because we're not looking at the
    * shader code yet. Here we look at the output written to get a correct
    * number of render target outputs.
    */
   const uint64_t rt_mask =
      stage->nir->info.outputs_written >> FRAG_RESULT_DATA0;
   stage->key.wm.color_outputs_valid = rt_mask & rp_color_mask(rp);
   stage->key.wm.nr_color_regions =
      util_last_bit(stage->key.wm.color_outputs_valid);

   unsigned num_rt_bindings;
   struct anv_pipeline_binding rt_bindings[MAX_RTS];
   if (stage->key.wm.nr_color_regions > 0) {
      assert(stage->key.wm.nr_color_regions <= MAX_RTS);
      for (unsigned rt = 0; rt < stage->key.wm.nr_color_regions; rt++) {
         if (stage->key.wm.color_outputs_valid & BITFIELD_BIT(rt)) {
            rt_bindings[rt] = (struct anv_pipeline_binding) {
               .set = ANV_DESCRIPTOR_SET_COLOR_ATTACHMENTS,
               .index = rt,
               .binding = UINT32_MAX,

            };
         } else {
            /* Setup a null render target */
            rt_bindings[rt] = (struct anv_pipeline_binding) {
               .set = ANV_DESCRIPTOR_SET_COLOR_ATTACHMENTS,
               .index = ANV_COLOR_OUTPUT_UNUSED,
               .binding = UINT32_MAX,
            };
         }
      }
      num_rt_bindings = stage->key.wm.nr_color_regions;
   } else if (brw_nir_fs_needs_null_rt(
                 compiler->devinfo, stage->nir,
                 stage->key.wm.alpha_to_coverage != INTEL_NEVER)) {
      /* Ensure the shader doesn't discard the writes */
      stage->key.wm.color_outputs_valid = 0x1;
      stage->key.wm.nr_color_regions = 1;
      /* Setup a null render target */
      rt_bindings[0] = (struct anv_pipeline_binding) {
         .set = ANV_DESCRIPTOR_SET_COLOR_ATTACHMENTS,
         .index = ANV_COLOR_OUTPUT_DISABLED,
         .binding = UINT32_MAX,
      };
      num_rt_bindings = 1;
   } else {
      num_rt_bindings = 0;
   }

   assert(num_rt_bindings <= MAX_RTS);
   assert(stage->bind_map.surface_count == 0);
   typed_memcpy(stage->bind_map.surface_to_descriptor,
                rt_bindings, num_rt_bindings);
   stage->bind_map.surface_count += num_rt_bindings;
}

static void
anv_pipeline_compile_fs(const struct brw_compiler *compiler,
                        void *mem_ctx,
                        struct anv_device *device,
                        struct anv_pipeline_stage *fs_stage,
                        struct anv_pipeline_stage *prev_stage,
                        struct anv_graphics_base_pipeline *pipeline,
                        uint32_t view_mask,
                        bool use_primitive_replication,
                        char **error_str)
{
   /* When using Primitive Replication for multiview, each view gets its own
    * position slot.
    */
   uint32_t pos_slots = use_primitive_replication ?
      MAX2(1, util_bitcount(view_mask)) : 1;

   /* If we have a previous stage we can use that to deduce valid slots.
    * Otherwise, rely on inputs of the input shader.
    */
   if (prev_stage) {
      fs_stage->key.wm.input_slots_valid =
         prev_stage->prog_data.vue.vue_map.slots_valid;
   } else {
      struct intel_vue_map prev_vue_map;
      brw_compute_vue_map(compiler->devinfo,
                          &prev_vue_map,
                          fs_stage->nir->info.inputs_read,
                          fs_stage->key.base.vue_layout,
                          pos_slots);

      fs_stage->key.wm.input_slots_valid = prev_vue_map.slots_valid;
   }

   struct brw_compile_fs_params params = {
      .base = {
         .nir = fs_stage->nir,
         .stats = fs_stage->stats,
         .log_data = device,
         .mem_ctx = mem_ctx,
         .source_hash = fs_stage->source_hash,
      },
      .key = &fs_stage->key.wm,
      .prog_data = &fs_stage->prog_data.wm,

      .allow_spilling = true,
      .max_polygons = UCHAR_MAX,
   };

   if (prev_stage && prev_stage->stage == MESA_SHADER_MESH) {
      params.mue_map = &prev_stage->prog_data.mesh.map;
      /* TODO(mesh): Slots valid, do we even use/rely on it? */
   }

   fs_stage->code = brw_compile_fs(compiler, &params);
   *error_str = params.base.error_str;

   fs_stage->num_stats = (uint32_t)!!fs_stage->prog_data.wm.dispatch_multi +
                         (uint32_t)fs_stage->prog_data.wm.dispatch_8 +
                         (uint32_t)fs_stage->prog_data.wm.dispatch_16 +
                         (uint32_t)fs_stage->prog_data.wm.dispatch_32;
   assert(fs_stage->num_stats <= ARRAY_SIZE(fs_stage->stats));

   for (unsigned i = 0; i < ARRAY_SIZE(fs_stage->bind_map.push_ranges); i++) {
      if (fs_stage->bind_map.push_ranges[i].set == ANV_DESCRIPTOR_SET_PER_PRIM_PADDING) {
         fs_stage->bind_map.push_ranges[i].length = MAX2(
            fs_stage->prog_data.wm.num_per_primitive_inputs / 2,
            fs_stage->bind_map.push_ranges[i].length);
         break;
      }
   }
}

static void
anv_pipeline_add_executable(struct anv_pipeline *pipeline,
                            struct anv_pipeline_stage *stage,
                            struct brw_compile_stats *stats,
                            uint32_t code_offset)
{
   char *nir = NULL;
   if (stage->nir &&
       (pipeline->flags &
        VK_PIPELINE_CREATE_2_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR)) {
      nir = nir_shader_as_str(stage->nir, pipeline->mem_ctx);
   }

   char *disasm = NULL;
   if (stage->code &&
       (pipeline->flags &
        VK_PIPELINE_CREATE_2_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR)) {
      char *stream_data = NULL;
      size_t stream_size = 0;
      FILE *stream = open_memstream(&stream_data, &stream_size);

      uint32_t push_size = 0;
      for (unsigned i = 0; i < 4; i++)
         push_size += stage->bind_map.push_ranges[i].length;
      if (push_size > 0) {
         fprintf(stream, "Push constant ranges:\n");
         for (unsigned i = 0; i < 4; i++) {
            if (stage->bind_map.push_ranges[i].length == 0)
               continue;

            fprintf(stream, "    RANGE%d (%dB): ", i,
                    stage->bind_map.push_ranges[i].length * 32);

            switch (stage->bind_map.push_ranges[i].set) {
            case ANV_DESCRIPTOR_SET_NULL:
               fprintf(stream, "NULL");
               break;

            case ANV_DESCRIPTOR_SET_PUSH_CONSTANTS:
               fprintf(stream, "Vulkan push constants and API params");
               break;

            case ANV_DESCRIPTOR_SET_DESCRIPTORS_BUFFER:
               fprintf(stream, "Descriptor buffer (desc buffer) for set %d (start=%dB)",
                       stage->bind_map.push_ranges[i].index,
                       stage->bind_map.push_ranges[i].start * 32);
               break;

            case ANV_DESCRIPTOR_SET_DESCRIPTORS:
               fprintf(stream, "Descriptor buffer for set %d (start=%dB)",
                       stage->bind_map.push_ranges[i].index,
                       stage->bind_map.push_ranges[i].start * 32);
               break;

            case ANV_DESCRIPTOR_SET_COLOR_ATTACHMENTS:
               UNREACHABLE("Color attachments can't be pushed");

            default:
               fprintf(stream, "UBO (set=%d binding=%d start=%dB)",
                       stage->bind_map.push_ranges[i].set,
                       stage->bind_map.push_ranges[i].index,
                       stage->bind_map.push_ranges[i].start * 32);
               break;
            }
            fprintf(stream, "\n");
         }
         fprintf(stream, "\n");
      }

      /* Creating this is far cheaper than it looks.  It's perfectly fine to
       * do it for every binary.
       */
      brw_disassemble_with_errors(&pipeline->device->physical->compiler->isa,
                                  stage->code, code_offset,
                                  &stage->bin->kernel.offset, stream);

      fclose(stream);

      /* Copy it to a ralloc'd thing */
      disasm = ralloc_size(pipeline->mem_ctx, stream_size + 1);
      memcpy(disasm, stream_data, stream_size);
      disasm[stream_size] = 0;

      free(stream_data);
   }

   if (INTEL_DEBUG(DEBUG_SHADERS_LINENO) && stage->code) {
      if (!intel_shader_dump_filter ||
          (intel_shader_dump_filter && intel_shader_dump_filter == stage->source_hash)) {
         brw_disassemble_with_lineno(&pipeline->device->physical->compiler->isa,
                                     stage->stage, (int)stats->dispatch_width,
                                     stage->source_hash, stage->code, code_offset,
                                     stage->bin->kernel.offset, stderr);
      }
   }

   const struct anv_pipeline_executable exe = {
      .stage = stage->stage,
      .stats = *stats,
      .nir = nir,
      .disasm = disasm,
   };
   util_dynarray_append(&pipeline->executables,
                        struct anv_pipeline_executable, exe);
}

static void
anv_pipeline_add_executables(struct anv_pipeline *pipeline,
                             struct anv_pipeline_stage *stage)
{
   if (stage->stage == MESA_SHADER_FRAGMENT) {
      /* We pull the prog data and stats out of the anv_shader_bin because
       * the anv_pipeline_stage may not be fully populated if we successfully
       * looked up the shader in a cache.
       */
      const struct brw_wm_prog_data *wm_prog_data =
         (const struct brw_wm_prog_data *)stage->bin->prog_data;
      struct brw_compile_stats *stats = stage->bin->stats;

      if (wm_prog_data->dispatch_8 ||
          wm_prog_data->dispatch_multi) {
         anv_pipeline_add_executable(pipeline, stage, stats++, 0);
      }

      if (wm_prog_data->dispatch_16) {
         anv_pipeline_add_executable(pipeline, stage, stats++,
                                     wm_prog_data->prog_offset_16);
      }

      if (wm_prog_data->dispatch_32) {
         anv_pipeline_add_executable(pipeline, stage, stats++,
                                     wm_prog_data->prog_offset_32);
      }
   } else {
      anv_pipeline_add_executable(pipeline, stage, stage->bin->stats, 0);
   }
}

static void
anv_pipeline_account_shader(struct anv_pipeline *pipeline,
                            struct anv_shader_bin *shader)
{
   pipeline->scratch_size = MAX2(pipeline->scratch_size,
                                 shader->prog_data->total_scratch);

   pipeline->ray_queries = MAX2(pipeline->ray_queries,
                                shader->prog_data->ray_queries);

   if (shader->push_desc_info.push_set_buffer) {
      pipeline->use_push_descriptor_buffer |=
         mesa_to_vk_shader_stage(shader->stage);
   }
   if (shader->push_desc_info.used_descriptors &
       ~shader->push_desc_info.fully_promoted_ubo_descriptors)
      pipeline->use_push_descriptor |= mesa_to_vk_shader_stage(shader->stage);
}

/* This function return true if a shader should not be looked at because of
 * fast linking. Instead we should use the shader binaries provided by
 * libraries.
 */
static bool
anv_graphics_pipeline_skip_shader_compile(struct anv_graphics_base_pipeline *pipeline,
                                          struct anv_pipeline_stage *stages,
                                          bool link_optimize,
                                          gl_shader_stage stage)
{
   /* Always skip non active stages */
   if (!anv_pipeline_base_has_stage(pipeline, stage))
      return true;

   /* When link optimizing, consider all stages */
   if (link_optimize)
      return false;

   /* Otherwise check if the stage was specified through
    * VkGraphicsPipelineCreateInfo
    */
   assert(stages[stage].info != NULL || stages[stage].imported.bin != NULL);
   return stages[stage].info == NULL;
}

static void
anv_graphics_pipeline_init_keys(struct anv_graphics_base_pipeline *pipeline,
                                const struct vk_graphics_pipeline_state *state,
                                struct anv_pipeline_stage *stages)
{
   struct anv_device *device = pipeline->base.device;
   enum intel_vue_layout vue_layout;

   if ((pipeline->base.flags & VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT) ||
       !device->vk.enabled_extensions.EXT_graphics_pipeline_library) {
      vue_layout = INTEL_VUE_LAYOUT_FIXED;
   } else {
      vue_layout =
         (pipeline->base.type == ANV_PIPELINE_GRAPHICS_LIB &&
          device->vk.enabled_extensions.EXT_mesh_shader) ?
         INTEL_VUE_LAYOUT_SEPARATE_MESH : INTEL_VUE_LAYOUT_SEPARATE;
   }

   for (uint32_t s = 0; s < ANV_GRAPHICS_SHADER_STAGE_COUNT; s++) {
      if (!anv_pipeline_base_has_stage(pipeline, s))
         continue;

      int64_t stage_start = os_time_get_nano();

      const struct anv_device *device = pipeline->base.device;
      switch (stages[s].stage) {
      case MESA_SHADER_VERTEX:
         populate_vs_prog_key(&stages[s], device, vue_layout);
         break;
      case MESA_SHADER_TESS_CTRL:
         populate_tcs_prog_key(&stages[s],
                               device,
                               BITSET_TEST(state->dynamic,
                                           MESA_VK_DYNAMIC_TS_PATCH_CONTROL_POINTS) ?
                               0 : state->ts->patch_control_points,
                               vue_layout);
         break;
      case MESA_SHADER_TESS_EVAL:
         populate_tes_prog_key(&stages[s], device, vue_layout);
         break;
      case MESA_SHADER_GEOMETRY:
         populate_gs_prog_key(&stages[s], device, vue_layout);
         break;
      case MESA_SHADER_FRAGMENT: {
         /* Assume rasterization enabled in any of the following case :
          *
          *    - We're a pipeline library without pre-rasterization information
          *
          *    - Rasterization is not disabled in the non dynamic state
          *
          *    - Rasterization disable is dynamic
          */
         const bool raster_enabled =
            state->rs == NULL ||
            !state->rs->rasterizer_discard_enable ||
            BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE);
         enum intel_sometimes is_mesh = INTEL_NEVER;
         if (device->vk.enabled_extensions.EXT_mesh_shader) {
            if (anv_pipeline_base_has_stage(pipeline, MESA_SHADER_VERTEX))
               is_mesh = INTEL_NEVER;
            else if (anv_pipeline_base_has_stage(pipeline, MESA_SHADER_MESH))
               is_mesh = INTEL_ALWAYS;
            else {
               assert(pipeline->base.type == ANV_PIPELINE_GRAPHICS_LIB);
               is_mesh = INTEL_SOMETIMES;
            }
         }
         populate_wm_prog_key(&stages[s],
                              pipeline,
                              state->dynamic,
                              raster_enabled ? state->ms : NULL,
                              raster_enabled ? state->rs : NULL,
                              state->fsr, state->rp, is_mesh,
                              vue_layout);
         break;
      }

      case MESA_SHADER_TASK:
         populate_task_prog_key(&stages[s], device);
         break;

      case MESA_SHADER_MESH: {
         populate_mesh_prog_key(&stages[s], device, vue_layout);
         break;
      }

      default:
         UNREACHABLE("Invalid graphics shader stage");
      }

      stages[s].feedback.duration += os_time_get_nano() - stage_start;
      stages[s].feedback.flags |= VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;
   }
}

static void
anv_graphics_lib_retain_shaders(struct anv_graphics_base_pipeline *pipeline,
                                struct anv_pipeline_stage *stages,
                                bool will_compile)
{
   /* There isn't much point in retaining NIR shaders on final pipelines. */
   assert(pipeline->base.type == ANV_PIPELINE_GRAPHICS_LIB);

   struct anv_graphics_lib_pipeline *lib = (struct anv_graphics_lib_pipeline *) pipeline;

   for (int s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
      if (!anv_pipeline_base_has_stage(pipeline, s))
         continue;

      memcpy(lib->retained_shaders[s].shader_sha1, stages[s].shader_sha1,
             sizeof(stages[s].shader_sha1));

      lib->retained_shaders[s].subgroup_size_type = stages[s].subgroup_size_type;

      nir_shader *nir = stages[s].nir != NULL ? stages[s].nir : stages[s].imported.nir;
      assert(nir != NULL);

      if (!will_compile) {
         lib->retained_shaders[s].nir = nir;
      } else {
         lib->retained_shaders[s].nir =
            nir_shader_clone(pipeline->base.mem_ctx, nir);
      }
   }
}

static bool
anv_graphics_pipeline_load_cached_shaders(struct anv_graphics_base_pipeline *pipeline,
                                          struct vk_pipeline_cache *cache,
                                          struct anv_pipeline_stage *stages,
                                          bool link_optimize,
                                          VkPipelineCreationFeedback *pipeline_feedback)
{
   struct anv_device *device = pipeline->base.device;
   unsigned cache_hits = 0, found = 0, imported = 0;

   for (unsigned s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
      if (!anv_pipeline_base_has_stage(pipeline, s))
         continue;

      int64_t stage_start = os_time_get_nano();

      bool cache_hit;
      stages[s].bin =
         anv_device_search_for_kernel(device, cache, &stages[s].cache_key,
                                      sizeof(stages[s].cache_key), &cache_hit);
      if (stages[s].bin) {
         found++;
         pipeline->shaders[s] = stages[s].bin;
      }

      if (cache_hit) {
         cache_hits++;
         stages[s].feedback.flags |=
            VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      }
      stages[s].feedback.duration += os_time_get_nano() - stage_start;
   }

   /* When not link optimizing, lookup the missing shader in the imported
    * libraries.
    */
   if (!link_optimize) {
      for (unsigned s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
         if (!anv_pipeline_base_has_stage(pipeline, s))
            continue;

         if (pipeline->shaders[s] != NULL)
            continue;

         if (stages[s].imported.bin == NULL)
            continue;

         stages[s].bin = stages[s].imported.bin;
         pipeline->shaders[s] = anv_shader_bin_ref(stages[s].imported.bin);
         imported++;
      }
   }

   if ((found + imported) == __builtin_popcount(pipeline->base.active_stages)) {
      if (cache_hits == found && found != 0) {
         pipeline_feedback->flags |=
            VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      }
      /* We found all our shaders in the cache.  We're done. */
      for (unsigned s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
         if (pipeline->shaders[s] == NULL)
            continue;

         /* Only add the executables when we're not importing or doing link
          * optimizations. The imported executables are added earlier. Link
          * optimization can produce different binaries.
          */
         if (stages[s].imported.bin == NULL || link_optimize)
            anv_pipeline_add_executables(&pipeline->base, &stages[s]);
      }
      return true;
   } else if (found > 0) {
      /* We found some but not all of our shaders. This shouldn't happen most
       * of the time but it can if we have a partially populated pipeline
       * cache.
       */
      assert(found < __builtin_popcount(pipeline->base.active_stages));

      /* With GPL, this might well happen if the app does an optimized
       * link.
       */
      if (!pipeline->base.device->vk.enabled_extensions.EXT_graphics_pipeline_library) {
         vk_perf(VK_LOG_OBJS(cache ? &cache->base :
                             &pipeline->base.device->vk.base),
                 "Found a partial pipeline in the cache.  This is "
                 "most likely caused by an incomplete pipeline cache "
                 "import or export");
      }

      /* We're going to have to recompile anyway, so just throw away our
       * references to the shaders in the cache.  We'll get them out of the
       * cache again as part of the compilation process.
       */
      for (unsigned s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
         stages[s].feedback.flags = 0;
         if (pipeline->shaders[s]) {
            anv_shader_bin_unref(device, pipeline->shaders[s]);
            pipeline->shaders[s] = NULL;
         }
      }
   }

   return false;
}

static const gl_shader_stage graphics_shader_order[] = {
   MESA_SHADER_VERTEX,
   MESA_SHADER_TESS_CTRL,
   MESA_SHADER_TESS_EVAL,
   MESA_SHADER_GEOMETRY,

   MESA_SHADER_TASK,
   MESA_SHADER_MESH,

   MESA_SHADER_FRAGMENT,
};

/* This function loads NIR only for stages specified in
 * VkGraphicsPipelineCreateInfo::pStages[]
 */
static VkResult
anv_graphics_pipeline_load_nir(struct anv_graphics_base_pipeline *pipeline,
                               struct vk_pipeline_cache *cache,
                               struct anv_pipeline_stage *stages,
                               void *mem_ctx,
                               bool need_clone)
{
   for (unsigned s = 0; s < ANV_GRAPHICS_SHADER_STAGE_COUNT; s++) {
      if (!anv_pipeline_base_has_stage(pipeline, s))
         continue;

      int64_t stage_start = os_time_get_nano();

      assert(stages[s].stage == s);

      /* Only use the create NIR from the pStages[] element if we don't have
       * an imported library for the same stage.
       */
      if (stages[s].imported.bin == NULL) {
         VkResult result = anv_pipeline_stage_get_nir(&pipeline->base, cache,
                                                      mem_ctx, &stages[s]);
         if (result != VK_SUCCESS)
            return result;
      } else {
         stages[s].nir = need_clone ?
                         nir_shader_clone(mem_ctx, stages[s].imported.nir) :
                         stages[s].imported.nir;
      }

      stages[s].feedback.duration += os_time_get_nano() - stage_start;
   }

   return VK_SUCCESS;
}

static void
anv_pipeline_nir_preprocess(struct anv_pipeline *pipeline,
                            struct anv_pipeline_stage *stage)
{
   struct anv_device *device = pipeline->device;
   const struct brw_compiler *compiler = device->physical->compiler;

   const struct nir_lower_sysvals_to_varyings_options sysvals_to_varyings = {
      .point_coord = true,
   };
   NIR_PASS(_, stage->nir, nir_lower_sysvals_to_varyings, &sysvals_to_varyings);

   const nir_opt_access_options opt_access_options = {
      .is_vulkan = true,
   };
   NIR_PASS(_, stage->nir, nir_opt_access, &opt_access_options);

   /* Use a separate-shader linking model for pipeline libraries, we do cross
    * stage linking otherwise.
    */
   stage->nir->info.separate_shader =
      stage->key.base.vue_layout != INTEL_VUE_LAYOUT_FIXED;

   struct brw_nir_compiler_opts opts = {
      .softfp64 = device->fp64_nir,
      /* Assume robustness with EXT_pipeline_robustness because this can be
       * turned on/off per pipeline and we have no visibility on this here.
       */
      .robust_image_access = device->vk.enabled_features.robustImageAccess ||
                             device->vk.enabled_features.robustImageAccess2 ||
                             device->vk.enabled_extensions.EXT_pipeline_robustness,
      .input_vertices = stage->nir->info.stage == MESA_SHADER_TESS_CTRL ?
                        stage->key.tcs.input_vertices : 0,
   };
   brw_preprocess_nir(compiler, stage->nir, &opts);

   NIR_PASS(_, stage->nir, nir_opt_barrier_modes);
   NIR_PASS(_, stage->nir, nir_opt_acquire_release_barriers,
                           SCOPE_QUEUE_FAMILY);

   nir_shader_gather_info(stage->nir, nir_shader_get_entrypoint(stage->nir));
}

static void
anv_fill_pipeline_creation_feedback(const struct anv_graphics_base_pipeline *pipeline,
                                    VkPipelineCreationFeedback *pipeline_feedback,
                                    const VkGraphicsPipelineCreateInfo *info,
                                    struct anv_pipeline_stage *stages)
{
   const VkPipelineCreationFeedbackCreateInfo *create_feedback =
      vk_find_struct_const(info->pNext, PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
   if (create_feedback) {
      *create_feedback->pPipelineCreationFeedback = *pipeline_feedback;

      /* VkPipelineCreationFeedbackCreateInfo:
       *
       *    "An implementation must set or clear the
       *     VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT in
       *     VkPipelineCreationFeedback::flags for pPipelineCreationFeedback
       *     and every element of pPipelineStageCreationFeedbacks."
       *
       */
      for (uint32_t i = 0; i < create_feedback->pipelineStageCreationFeedbackCount; i++) {
         create_feedback->pPipelineStageCreationFeedbacks[i].flags &=
            ~VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;
      }
      /* This part is not really specified in the Vulkan spec at the moment.
       * We're kind of guessing what the CTS wants. We might need to update
       * when https://gitlab.khronos.org/vulkan/vulkan/-/issues/3115 is
       * clarified.
       */
      for (uint32_t s = 0; s < ANV_GRAPHICS_SHADER_STAGE_COUNT; s++) {
         if (!anv_pipeline_base_has_stage(pipeline, s))
            continue;

         if (stages[s].feedback_idx < create_feedback->pipelineStageCreationFeedbackCount) {
            create_feedback->pPipelineStageCreationFeedbacks[
               stages[s].feedback_idx] = stages[s].feedback;
         }
      }
   }
}

static uint32_t
anv_graphics_pipeline_imported_shader_count(struct anv_pipeline_stage *stages)
{
   uint32_t count = 0;
   for (uint32_t s = 0; s < ANV_GRAPHICS_SHADER_STAGE_COUNT; s++) {
      if (stages[s].imported.bin != NULL)
         count++;
   }
   return count;
}

static VkResult
anv_graphics_pipeline_compile(struct anv_graphics_base_pipeline *pipeline,
                              struct anv_pipeline_stage *stages,
                              struct vk_pipeline_cache *cache,
                              VkPipelineCreationFeedback *pipeline_feedback,
                              const VkGraphicsPipelineCreateInfo *info,
                              const struct vk_graphics_pipeline_state *state)
{
   int64_t pipeline_start = os_time_get_nano();

   struct anv_device *device = pipeline->base.device;
   const struct intel_device_info *devinfo = device->info;
   const struct brw_compiler *compiler = device->physical->compiler;

   /* Setup the shaders given in this VkGraphicsPipelineCreateInfo::pStages[].
    * Other shaders imported from libraries should have been added by
    * anv_graphics_pipeline_import_lib().
    */
   uint32_t shader_count = anv_graphics_pipeline_imported_shader_count(stages);
   for (uint32_t i = 0; i < info->stageCount; i++) {
      gl_shader_stage stage = vk_to_mesa_shader_stage(info->pStages[i].stage);

      /* If a pipeline library is loaded in this stage, we should ignore the
       * pStages[] entry of the same stage.
       */
      if (stages[stage].imported.bin != NULL)
         continue;

      stages[stage].stage = stage;
      stages[stage].pipeline_flags = pipeline->base.flags;
      stages[stage].pipeline_pNext = info->pNext;
      stages[stage].info = &info->pStages[i];
      stages[stage].feedback_idx = shader_count++;

      anv_stage_write_shader_hash(&stages[stage], device);
   }

   /* Prepare shader keys for all shaders in pipeline->base.active_stages
    * (this includes libraries) before generating the hash for cache look up.
    *
    * We're doing this because the spec states that :
    *
    *    "When an implementation is looking up a pipeline in a pipeline cache,
    *     if that pipeline is being created using linked libraries,
    *     implementations should always return an equivalent pipeline created
    *     with VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT if available,
    *     whether or not that bit was specified."
    *
    * So even if the application does not request link optimization, we have
    * to do our cache lookup with the entire set of shader sha1s so that we
    * can find what would be the best optimized pipeline in the case as if we
    * had compiled all the shaders together and known the full graphics state.
    */
   anv_graphics_pipeline_init_keys(pipeline, state, stages);

   uint32_t view_mask = state->rp ? state->rp->view_mask : 0;

   unsigned char sha1[20];
   anv_pipeline_hash_graphics(pipeline, stages, view_mask, sha1);

   for (unsigned s = 0; s < ANV_GRAPHICS_SHADER_STAGE_COUNT; s++) {
      if (!anv_pipeline_base_has_stage(pipeline, s))
         continue;

      stages[s].cache_key.stage = s;
      memcpy(stages[s].cache_key.sha1, sha1, sizeof(sha1));
   }

   const bool retain_shaders =
      pipeline->base.flags & VK_PIPELINE_CREATE_2_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
   const bool link_optimize =
      pipeline->base.flags & VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT;

   VkResult result = VK_SUCCESS;
   const bool skip_cache_lookup =
      (pipeline->base.flags & VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR);

   if (!skip_cache_lookup) {
      bool found_all_shaders =
         anv_graphics_pipeline_load_cached_shaders(pipeline, cache, stages,
                                                   link_optimize,
                                                   pipeline_feedback);

      if (found_all_shaders) {
         /* If we need to retain shaders, we need to also load from the NIR
          * cache.
          */
         if (pipeline->base.type == ANV_PIPELINE_GRAPHICS_LIB && retain_shaders) {
            result = anv_graphics_pipeline_load_nir(pipeline, cache,
                                                    stages,
                                                    pipeline->base.mem_ctx,
                                                    false /* need_clone */);
            if (result != VK_SUCCESS) {
               vk_perf(VK_LOG_OBJS(cache ? &cache->base :
                                   &pipeline->base.device->vk.base),
                       "Found all ISA shaders in the cache but not all NIR shaders.");
            } else {
               anv_graphics_lib_retain_shaders(pipeline, stages, false /* will_compile */);
            }
         }

         if (result == VK_SUCCESS)
            goto done;

         for (unsigned s = 0; s < ANV_GRAPHICS_SHADER_STAGE_COUNT; s++) {
            if (!anv_pipeline_base_has_stage(pipeline, s))
               continue;

            if (stages[s].nir) {
               ralloc_free(stages[s].nir);
               stages[s].nir = NULL;
            }

            assert(pipeline->shaders[s] != NULL);
            anv_shader_bin_unref(device, pipeline->shaders[s]);
            pipeline->shaders[s] = NULL;
         }
      }
   }

   if (pipeline->base.flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
      return VK_PIPELINE_COMPILE_REQUIRED;

   void *tmp_ctx = ralloc_context(NULL);

   result = anv_graphics_pipeline_load_nir(pipeline, cache, stages,
                                           tmp_ctx, link_optimize /* need_clone */);
   if (result != VK_SUCCESS)
      goto fail;

   /* Retain shaders now if asked, this only applies to libraries */
   if (pipeline->base.type == ANV_PIPELINE_GRAPHICS_LIB && retain_shaders)
      anv_graphics_lib_retain_shaders(pipeline, stages, true /* will_compile */);

   /* The following steps will be executed for shaders we need to compile :
    *
    *    - specified through VkGraphicsPipelineCreateInfo::pStages[]
    *
    *    - or compiled from libraries with retained shaders (libraries
    *      compiled with CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT) if the
    *      pipeline has the CREATE_LINK_TIME_OPTIMIZATION_BIT flag.
    */

   /* Preprocess all NIR shaders. */
   for (int s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
      if (anv_graphics_pipeline_skip_shader_compile(pipeline, stages,
                                                    link_optimize, s))
         continue;

      anv_stage_allocate_bind_map_tables(&pipeline->base, &stages[s], tmp_ctx);

      anv_pipeline_nir_preprocess(&pipeline->base, &stages[s]);
   }

   /* Walk backwards to link */
   struct anv_pipeline_stage *next_stage = NULL;
   for (int i = ARRAY_SIZE(graphics_shader_order) - 1; i >= 0; i--) {
      gl_shader_stage s = graphics_shader_order[i];
      if (anv_graphics_pipeline_skip_shader_compile(pipeline, stages,
                                                    link_optimize, s))
         continue;

      struct anv_pipeline_stage *stage = &stages[s];

      switch (s) {
      case MESA_SHADER_VERTEX:
         anv_pipeline_link_vs(compiler, stage, next_stage);
         break;
      case MESA_SHADER_TESS_CTRL:
         anv_pipeline_link_tcs(compiler, stage, next_stage);
         break;
      case MESA_SHADER_TESS_EVAL:
         anv_pipeline_link_tes(compiler, stage, next_stage);
         break;
      case MESA_SHADER_GEOMETRY:
         anv_pipeline_link_gs(compiler, stage, next_stage);
         break;
      case MESA_SHADER_TASK:
         anv_pipeline_link_task(compiler, stage, next_stage);
         break;
      case MESA_SHADER_MESH:
         anv_pipeline_link_mesh(compiler, stage, next_stage);
         break;
      case MESA_SHADER_FRAGMENT:
         anv_pipeline_link_fs(compiler, stage, state->rp);
         break;
      default:
         UNREACHABLE("Invalid graphics shader stage");
      }

      next_stage = stage;
   }

   bool use_primitive_replication = false;
   if (devinfo->ver >= 12 && view_mask != 0) {
      /* For some pipelines HW Primitive Replication can be used instead of
       * instancing to implement Multiview.  This depend on how viewIndex is
       * used in all the active shaders, so this check can't be done per
       * individual shaders.
       */
      nir_shader *shaders[ANV_GRAPHICS_SHADER_STAGE_COUNT] = {};
      for (unsigned s = 0; s < ARRAY_SIZE(shaders); s++)
         shaders[s] = stages[s].nir;

      use_primitive_replication =
         anv_check_for_primitive_replication(device,
                                             pipeline->base.active_stages,
                                             shaders, view_mask);
   }

   struct anv_pipeline_stage *prev_stage = NULL;
   for (unsigned i = 0; i < ARRAY_SIZE(graphics_shader_order); i++) {
      gl_shader_stage s = graphics_shader_order[i];
      if (anv_graphics_pipeline_skip_shader_compile(pipeline, stages,
                                                    link_optimize, s))
         continue;

      struct anv_pipeline_stage *stage = &stages[s];

      int64_t stage_start = os_time_get_nano();

      anv_pipeline_lower_nir(&pipeline->base, tmp_ctx, stage,
                             &pipeline->base.layout, view_mask,
                             use_primitive_replication);

      struct shader_info *cur_info = &stage->nir->info;

      if (prev_stage && compiler->nir_options[s]->unify_interfaces) {
         struct shader_info *prev_info = &prev_stage->nir->info;

         prev_info->outputs_written |= cur_info->inputs_read &
                  ~(VARYING_BIT_TESS_LEVEL_INNER | VARYING_BIT_TESS_LEVEL_OUTER);
         cur_info->inputs_read |= prev_info->outputs_written &
                  ~(VARYING_BIT_TESS_LEVEL_INNER | VARYING_BIT_TESS_LEVEL_OUTER);
         prev_info->patch_outputs_written |= cur_info->patch_inputs_read;
         cur_info->patch_inputs_read |= prev_info->patch_outputs_written;
      }

      anv_fixup_subgroup_size(device, cur_info);

      stage->feedback.duration += os_time_get_nano() - stage_start;

      prev_stage = stage;
   }

   /* In the case the platform can write the primitive variable shading rate
    * and KHR_fragment_shading_rate is enabled :
    *    - there can be a fragment shader but we don't have it yet
    *    - the fragment shader needs fragment shading rate
    *
    * figure out the last geometry stage that should write the primitive
    * shading rate, and ensure it is marked as used there. The backend will
    * write a default value if the shader doesn't actually write it.
    *
    * We iterate backwards in the stage and stop on the first shader that can
    * set the value.
    *
    * Don't apply this to MESH stages, as this is a per primitive thing.
    */
   if (devinfo->has_coarse_pixel_primitive_and_cb &&
       device->vk.enabled_extensions.KHR_fragment_shading_rate &&
       pipeline_has_coarse_pixel(state->dynamic, state->ms, state->fsr) &&
       (!stages[MESA_SHADER_FRAGMENT].info ||
        stages[MESA_SHADER_FRAGMENT].key.wm.coarse_pixel) &&
       stages[MESA_SHADER_MESH].nir == NULL) {
      struct anv_pipeline_stage *last_psr = NULL;

      for (unsigned i = 0; i < ARRAY_SIZE(graphics_shader_order); i++) {
         gl_shader_stage s =
            graphics_shader_order[ARRAY_SIZE(graphics_shader_order) - i - 1];

         if (anv_graphics_pipeline_skip_shader_compile(pipeline, stages,
                                                       link_optimize, s) ||
             !gl_shader_stage_can_set_fragment_shading_rate(s))
            continue;

         last_psr = &stages[s];
         break;
      }

      /* Only set primitive shading rate if there is a pre-rasterization
       * shader in this pipeline/pipeline-library.
       */
      if (last_psr)
         last_psr->nir->info.outputs_written |= VARYING_BIT_PRIMITIVE_SHADING_RATE;
   }

   prev_stage = NULL;
   for (unsigned i = 0; i < ARRAY_SIZE(graphics_shader_order); i++) {
      gl_shader_stage s = graphics_shader_order[i];
      struct anv_pipeline_stage *stage = &stages[s];

      if (anv_graphics_pipeline_skip_shader_compile(pipeline, stages, link_optimize, s))
         continue;

      int64_t stage_start = os_time_get_nano();

      void *stage_ctx = ralloc_context(NULL);
      char *error_str = NULL;

      switch (s) {
      case MESA_SHADER_VERTEX:
         anv_pipeline_compile_vs(compiler, stage_ctx, pipeline,
                                 stage, view_mask, &error_str);
         break;
      case MESA_SHADER_TESS_CTRL:
         anv_pipeline_compile_tcs(compiler, stage_ctx, device,
                                  stage, prev_stage, &error_str);
         break;
      case MESA_SHADER_TESS_EVAL:
         anv_pipeline_compile_tes(compiler, stage_ctx, device,
                                  stage, prev_stage, &error_str);
         break;
      case MESA_SHADER_GEOMETRY:
         anv_pipeline_compile_gs(compiler, stage_ctx, device,
                                 stage, prev_stage, &error_str);
         break;
      case MESA_SHADER_TASK:
         anv_pipeline_compile_task(compiler, stage_ctx, device,
                                   stage, &error_str);
         break;
      case MESA_SHADER_MESH:
         anv_pipeline_compile_mesh(compiler, stage_ctx, device,
                                   stage, prev_stage, &error_str);
         break;
      case MESA_SHADER_FRAGMENT:
         anv_pipeline_compile_fs(compiler, stage_ctx, device,
                                 stage, prev_stage, pipeline,
                                 view_mask, use_primitive_replication,
                                 &error_str);
         break;
      default:
         UNREACHABLE("Invalid graphics shader stage");
      }
      if (stage->code == NULL) {
         if (error_str)
            result = vk_errorf(pipeline, VK_ERROR_UNKNOWN, "%s", error_str);
         else
            result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         ralloc_free(stage_ctx);
         goto fail;
      }

      anv_nir_validate_push_layout(device->physical, &stage->prog_data.base,
                                   &stage->bind_map);

      struct anv_shader_upload_params upload_params = {
         .stage               = s,
         .key_data            = &stage->cache_key,
         .key_size            = sizeof(stage->cache_key),
         .kernel_data         = stage->code,
         .kernel_size         = stage->prog_data.base.program_size,
         .prog_data           = &stage->prog_data.base,
         .prog_data_size      = brw_prog_data_size(s),
         .stats               = stage->stats,
         .num_stats           = stage->num_stats,
         .xfb_info            = stage->nir->xfb_info,
         .bind_map            = &stage->bind_map,
         .push_desc_info      = &stage->push_desc_info,
      };

      stage->bin =
         anv_device_upload_kernel(device, cache, &upload_params);
      if (!stage->bin) {
         ralloc_free(stage_ctx);
         result = vk_error(pipeline, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto fail;
      }

      anv_pipeline_add_executables(&pipeline->base, stage);
      pipeline->shaders[s] = stage->bin;

      ralloc_free(stage_ctx);

      stage->feedback.duration += os_time_get_nano() - stage_start;

      prev_stage = stage;
   }

   /* Finally add the imported shaders that were not compiled as part of this
    * step.
    */
   for (unsigned s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
      if (!anv_pipeline_base_has_stage(pipeline, s))
         continue;

      if (pipeline->shaders[s] != NULL)
         continue;

      /* We should have recompiled everything with link optimization. */
      assert(!link_optimize);

      struct anv_pipeline_stage *stage = &stages[s];

      pipeline->shaders[s] = anv_shader_bin_ref(stage->imported.bin);
   }

   ralloc_free(tmp_ctx);

done:

   /* Write the feedback index into the pipeline */
   for (unsigned s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
      if (!anv_pipeline_base_has_stage(pipeline, s))
         continue;

      struct anv_pipeline_stage *stage = &stages[s];
      pipeline->feedback_index[s] = stage->feedback_idx;
      pipeline->robust_flags[s] = stage->robust_flags;

      anv_pipeline_account_shader(&pipeline->base, pipeline->shaders[s]);
   }

   pipeline_feedback->duration = os_time_get_nano() - pipeline_start;

   return VK_SUCCESS;

fail:
   ralloc_free(tmp_ctx);

   for (unsigned s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
      if (pipeline->shaders[s])
         anv_shader_bin_unref(device, pipeline->shaders[s]);
   }

   return result;
}

static VkResult
anv_pipeline_compile_cs(struct anv_compute_pipeline *pipeline,
                        struct vk_pipeline_cache *cache,
                        const VkComputePipelineCreateInfo *info)
{
   ASSERTED const VkPipelineShaderStageCreateInfo *sinfo = &info->stage;
   assert(sinfo->stage == VK_SHADER_STAGE_COMPUTE_BIT);

   VkPipelineCreationFeedback pipeline_feedback = {
      .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
   };
   int64_t pipeline_start = os_time_get_nano();

   struct anv_device *device = pipeline->base.device;
   const struct brw_compiler *compiler = device->physical->compiler;

   struct anv_pipeline_stage stage = {
      .stage = MESA_SHADER_COMPUTE,
      .info = &info->stage,
      .pipeline_flags = pipeline->base.flags,
      .pipeline_pNext = info->pNext,
      .cache_key = {
         .stage = MESA_SHADER_COMPUTE,
      },
      .feedback = {
         .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
      },
   };
   anv_stage_write_shader_hash(&stage, device);

   populate_cs_prog_key(&stage, device);

   const bool skip_cache_lookup =
      (pipeline->base.flags & VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR);

   anv_pipeline_hash_compute(pipeline, &stage, stage.cache_key.sha1);

   bool cache_hit = false;
   if (!skip_cache_lookup) {
      stage.bin = anv_device_search_for_kernel(device, cache,
                                               &stage.cache_key,
                                               sizeof(stage.cache_key),
                                               &cache_hit);
   }

   if (stage.bin == NULL &&
       (pipeline->base.flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT))
      return VK_PIPELINE_COMPILE_REQUIRED;

   void *mem_ctx = ralloc_context(NULL);
   if (stage.bin == NULL) {
      int64_t stage_start = os_time_get_nano();

      anv_stage_allocate_bind_map_tables(&pipeline->base, &stage, mem_ctx);

      VkResult result = anv_pipeline_stage_get_nir(&pipeline->base, cache,
                                                   mem_ctx, &stage);
      if (result != VK_SUCCESS) {
         ralloc_free(mem_ctx);
         return result;
      }

      anv_pipeline_nir_preprocess(&pipeline->base, &stage);

      anv_pipeline_lower_nir(&pipeline->base, mem_ctx, &stage,
                             &pipeline->base.layout, 0 /* view_mask */,
                             false /* use_primitive_replication */);

      anv_fixup_subgroup_size(device, &stage.nir->info);

      stage.num_stats = 1;

      struct brw_compile_cs_params params = {
         .base = {
            .nir = stage.nir,
            .stats = stage.stats,
            .log_data = device,
            .mem_ctx = mem_ctx,
            .source_hash = stage.source_hash,
         },
         .key = &stage.key.cs,
         .prog_data = &stage.prog_data.cs,
      };

      stage.code = brw_compile_cs(compiler, &params);
      if (stage.code == NULL) {
         VkResult result;

         if (params.base.error_str)
            result = vk_errorf(pipeline, VK_ERROR_UNKNOWN, "%s", params.base.error_str);
         else
            result = vk_error(pipeline, VK_ERROR_OUT_OF_HOST_MEMORY);

         ralloc_free(mem_ctx);
         return result;
      }

      anv_nir_validate_push_layout(device->physical, &stage.prog_data.base,
                                   &stage.bind_map);

      struct anv_shader_upload_params upload_params = {
         .stage               = MESA_SHADER_COMPUTE,
         .key_data            = &stage.cache_key,
         .key_size            = sizeof(stage.cache_key),
         .kernel_data         = stage.code,
         .kernel_size         = stage.prog_data.base.program_size,
         .prog_data           = &stage.prog_data.base,
         .prog_data_size      = sizeof(stage.prog_data.cs),
         .stats               = stage.stats,
         .num_stats           = stage.num_stats,
         .bind_map            = &stage.bind_map,
         .push_desc_info      = &stage.push_desc_info,
      };

      stage.bin = anv_device_upload_kernel(device, cache, &upload_params);
      if (!stage.bin) {
         ralloc_free(mem_ctx);
         return vk_error(pipeline, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      stage.feedback.duration = os_time_get_nano() - stage_start;
   }

   anv_pipeline_account_shader(&pipeline->base, stage.bin);
   anv_pipeline_add_executables(&pipeline->base, &stage);

   ralloc_free(mem_ctx);

   if (cache_hit) {
      stage.feedback.flags |=
         VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      pipeline_feedback.flags |=
         VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
   }
   pipeline_feedback.duration = os_time_get_nano() - pipeline_start;

   const VkPipelineCreationFeedbackCreateInfo *create_feedback =
      vk_find_struct_const(info->pNext, PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
   if (create_feedback) {
      *create_feedback->pPipelineCreationFeedback = pipeline_feedback;

      if (create_feedback->pipelineStageCreationFeedbackCount) {
         assert(create_feedback->pipelineStageCreationFeedbackCount == 1);
         create_feedback->pPipelineStageCreationFeedbacks[0] = stage.feedback;
      }
   }

   pipeline->cs = stage.bin;

   return VK_SUCCESS;
}

static VkResult
anv_compute_pipeline_create(struct anv_device *device,
                            struct vk_pipeline_cache *cache,
                            const VkComputePipelineCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipeline)
{
   struct anv_compute_pipeline *pipeline;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);

   pipeline = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_pipeline_init(&pipeline->base, device,
                              ANV_PIPELINE_COMPUTE,
                              vk_compute_pipeline_create_flags(pCreateInfo),
                              pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      return result;
   }

   ANV_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   anv_pipeline_init_layout(&pipeline->base, pipeline_layout);

   pipeline->base.active_stages = VK_SHADER_STAGE_COMPUTE_BIT;

   anv_batch_set_storage(&pipeline->base.batch, ANV_NULL_ADDRESS,
                         pipeline->batch_data, sizeof(pipeline->batch_data));

   result = anv_pipeline_compile_cs(pipeline, cache, pCreateInfo);
   if (result != VK_SUCCESS) {
      anv_pipeline_finish(&pipeline->base, device);
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      return result;
   }

   anv_genX(device->info, compute_pipeline_emit)(pipeline);

   ANV_RMV(compute_pipeline_create, device, pipeline, false);

   *pPipeline = anv_pipeline_to_handle(&pipeline->base);

   return pipeline->base.batch.status;
}

VkResult anv_CreateComputePipelines(
    VkDevice                                    _device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    count,
    const VkComputePipelineCreateInfo*          pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(vk_pipeline_cache, pipeline_cache, pipelineCache);

   VkResult result = VK_SUCCESS;

   unsigned i;
   for (i = 0; i < count; i++) {
      const VkPipelineCreateFlags2KHR flags =
         vk_compute_pipeline_create_flags(&pCreateInfos[i]);
      VkResult res = anv_compute_pipeline_create(device, pipeline_cache,
                                                 &pCreateInfos[i],
                                                 pAllocator, &pPipelines[i]);

      if (res != VK_SUCCESS) {
         result = res;
         if (flags & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR)
            break;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   for (; i < count; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}

static uint32_t
get_vs_input_elements(const struct brw_vs_prog_data *vs_prog_data)
{
   /* Pull inputs_read out of the VS prog data */
   const uint64_t inputs_read = vs_prog_data->inputs_read;
   const uint64_t double_inputs_read =
      vs_prog_data->double_inputs_read & inputs_read;
   assert((inputs_read & ((1 << VERT_ATTRIB_GENERIC0) - 1)) == 0);
   const uint32_t elements = inputs_read >> VERT_ATTRIB_GENERIC0;
   const uint32_t elements_double = double_inputs_read >> VERT_ATTRIB_GENERIC0;

   return __builtin_popcount(elements) -
          __builtin_popcount(elements_double) / 2;
}

static void
anv_graphics_pipeline_emit(struct anv_graphics_pipeline *pipeline,
                           const struct vk_graphics_pipeline_state *state)
{
   pipeline->view_mask = state->rp->view_mask;

   if (anv_pipeline_is_primitive(pipeline)) {
      const struct brw_vs_prog_data *vs_prog_data =
         get_pipeline_vs_prog_data(pipeline);
      const struct brw_wm_prog_data *wm_prog_data =
         get_pipeline_wm_prog_data(pipeline);

      /* The total number of vertex elements we need to program. We might need
       * a couple more to implement some of the draw parameters.
       */
      pipeline->svgs_count =
         (vs_prog_data->uses_vertexid ||
          vs_prog_data->uses_instanceid ||
          vs_prog_data->uses_firstvertex ||
          vs_prog_data->uses_baseinstance) + vs_prog_data->uses_drawid;

      pipeline->vs_input_elements = get_vs_input_elements(vs_prog_data);

      pipeline->vertex_input_elems =
         (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VI) ?
          0 : pipeline->vs_input_elements) + pipeline->svgs_count;

      /* Our implementation of VK_KHR_multiview uses instancing to draw the
       * different views when primitive replication cannot be used.  If the
       * client asks for instancing, we need to multiply by the client's
       * instance count at draw time and instance divisor in the vertex
       * bindings by the number of views ensure that we repeat the client's
       * per-instance data once for each view.
       */
      const bool uses_primitive_replication =
         anv_pipeline_get_last_vue_prog_data(pipeline)->vue_map.num_pos_slots > 1;
      pipeline->instance_multiplier = 1;
      if (pipeline->view_mask && !uses_primitive_replication)
         pipeline->instance_multiplier = util_bitcount(pipeline->view_mask);

      pipeline->vs_source_hash = vs_prog_data->base.base.source_hash;
      pipeline->fs_source_hash = wm_prog_data ? wm_prog_data->base.source_hash : 0;
   } else {
      assert(anv_pipeline_is_mesh(pipeline));
      /* TODO(mesh): Mesh vs. Multiview with Instancing. */
   }

   if (pipeline->base.shaders[MESA_SHADER_FRAGMENT] && state->ms) {
      pipeline->sample_shading_enable = state->ms->sample_shading_enable;
      pipeline->min_sample_shading = state->ms->min_sample_shading;
   }

   /* Mark all color output as unused by default */
   memset(pipeline->color_output_mapping,
          ANV_COLOR_OUTPUT_UNUSED,
          sizeof(pipeline->color_output_mapping));

   if (anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      /* Count the number of color attachments in the binding table */
      const struct anv_pipeline_bind_map *bind_map =
         &pipeline->base.shaders[MESA_SHADER_FRAGMENT]->bind_map;

      if (state->cal != NULL) {
         /* Build a map of fragment color output to attachment */
         uint8_t rt_to_att[MAX_RTS];
         memset(rt_to_att, ANV_COLOR_OUTPUT_DISABLED, MAX_RTS);
         for (uint32_t i = 0; i < MAX_RTS; i++) {
            if (state->cal->color_map[i] != MESA_VK_ATTACHMENT_UNUSED)
               rt_to_att[state->cal->color_map[i]] = i;
         }

         /* For each fragment shader output if not unused apply the remapping
          * to pipeline->color_output_mapping
          */
         unsigned i;
         for (i = 0; i < MIN2(bind_map->surface_count, MAX_RTS); i++) {
            if (bind_map->surface_to_descriptor[i].set !=
                ANV_DESCRIPTOR_SET_COLOR_ATTACHMENTS)
               break;

            uint32_t index = bind_map->surface_to_descriptor[i].index;
            if (index >= MAX_RTS) {
               assert(index <= 0xff);
               pipeline->color_output_mapping[i] = index;
            } else {
               pipeline->color_output_mapping[i] = rt_to_att[i];
            }
         }
         pipeline->num_color_outputs = i;
      }
   }

   const struct anv_device *device = pipeline->base.base.device;
   const struct intel_device_info *devinfo = device->info;
   anv_genX(devinfo, graphics_pipeline_emit)(pipeline, state);
}

static void
anv_graphics_pipeline_import_layout(struct anv_graphics_base_pipeline *pipeline,
                                    struct vk_pipeline_layout *pipeline_layout)
{
   const bool independent_layouts =
      (pipeline_layout->create_flags &
       VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) != 0;
   pipeline->base.layout.independent_sets |= independent_layouts;

   for (uint32_t s = 0; s < pipeline_layout->set_count; s++) {
      if (pipeline_layout->set_layouts[s] == NULL)
         continue;

      struct anv_descriptor_set_layout *set_layout =
         (struct anv_descriptor_set_layout *) pipeline_layout->set_layouts[s];
      anv_pipeline_sets_layout_add(&pipeline->base.layout, s, set_layout);
   }
}

static void
anv_graphics_pipeline_import_sets_layout(struct anv_graphics_base_pipeline *pipeline,
                                         const struct anv_pipeline_sets_layout *sets_layout)
{
   pipeline->base.layout.independent_sets |= sets_layout->independent_sets;

   for (uint32_t s = 0; s < sets_layout->num_sets; s++) {
      if (sets_layout->set_layouts[s] == NULL)
         continue;

      anv_pipeline_sets_layout_add(&pipeline->base.layout, s,
                                   sets_layout->set_layouts[s]);
   }
}

static void
anv_graphics_pipeline_import_lib(struct anv_graphics_base_pipeline *pipeline,
                                 bool link_optimize,
                                 bool retain_shaders,
                                 struct anv_pipeline_stage *stages,
                                 struct anv_graphics_lib_pipeline *lib)
{
   anv_graphics_pipeline_import_sets_layout(pipeline, &lib->base.base.layout);

   /* We can't have shaders specified twice through libraries. */
   assert((pipeline->base.active_stages & lib->base.base.active_stages) == 0);

   /* VK_EXT_graphics_pipeline_library:
    *
    *    "To perform link time optimizations,
    *     VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT must
    *     be specified on all pipeline libraries that are being linked
    *     together. Implementations should retain any additional information
    *     needed to perform optimizations at the final link step when this bit
    *     is present."
    */
   assert(!link_optimize || lib->retain_shaders);

   pipeline->base.active_stages |= lib->base.base.active_stages;

   uint32_t shader_count = anv_graphics_pipeline_imported_shader_count(stages);
   for (uint32_t s = 0; s < ARRAY_SIZE(lib->base.shaders); s++) {
      if (lib->base.shaders[s] == NULL)
         continue;

      stages[s].stage = s;
      stages[s].pipeline_flags = pipeline->base.flags;
      stages[s].feedback_idx = shader_count + lib->base.feedback_index[s];
      stages[s].robust_flags = lib->base.robust_flags[s];

      /* Always import the shader sha1, this will be used for cache lookup. */
      memcpy(stages[s].shader_sha1, lib->retained_shaders[s].shader_sha1,
             sizeof(stages[s].shader_sha1));
      stages[s].source_hash = lib->base.shaders[s]->prog_data->source_hash;

      stages[s].subgroup_size_type = lib->retained_shaders[s].subgroup_size_type;
      stages[s].imported.nir = lib->retained_shaders[s].nir;
      stages[s].imported.bin = lib->base.shaders[s];
   }

   /* When not link optimizing, import the executables (shader descriptions
    * for VK_KHR_pipeline_executable_properties). With link optimization there
    * is a chance it'll produce different binaries, so we'll add the optimized
    * version later.
    */
   if (!link_optimize) {
      util_dynarray_foreach(&lib->base.base.executables,
                            struct anv_pipeline_executable, exe) {
         util_dynarray_append(&pipeline->base.executables,
                              struct anv_pipeline_executable, *exe);
      }
   }
}

static void
anv_graphics_lib_validate_shaders(struct anv_graphics_lib_pipeline *lib,
                                  bool retained_shaders)
{
   for (uint32_t s = 0; s < ARRAY_SIZE(lib->retained_shaders); s++) {
      if (anv_pipeline_base_has_stage(&lib->base, s)) {
         assert(!retained_shaders || lib->retained_shaders[s].nir != NULL);
         assert(lib->base.shaders[s] != NULL);
      }
   }
}

static VkResult
anv_graphics_lib_pipeline_create(struct anv_device *device,
                                 struct vk_pipeline_cache *cache,
                                 const VkGraphicsPipelineCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkPipeline *pPipeline)
{
   struct anv_pipeline_stage stages[ANV_GRAPHICS_SHADER_STAGE_COUNT] = {};
   VkPipelineCreationFeedback pipeline_feedback = {
      .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
   };
   int64_t pipeline_start = os_time_get_nano();

   struct anv_graphics_lib_pipeline *pipeline;
   VkResult result;

   const VkPipelineCreateFlags2KHR flags =
      vk_graphics_pipeline_create_flags(pCreateInfo);
   assert(flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR);

   const VkPipelineLibraryCreateInfoKHR *libs_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           PIPELINE_LIBRARY_CREATE_INFO_KHR);

   pipeline = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_pipeline_init(&pipeline->base.base, device,
                              ANV_PIPELINE_GRAPHICS_LIB, flags,
                              pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      if (result == VK_PIPELINE_COMPILE_REQUIRED)
         *pPipeline = VK_NULL_HANDLE;
      return result;
   }

   /* Capture the retain state before we compile/load any shader. */
   pipeline->retain_shaders =
      (flags & VK_PIPELINE_CREATE_2_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT) != 0;

   /* If we have libraries, import them first. */
   if (libs_info) {
      for (uint32_t i = 0; i < libs_info->libraryCount; i++) {
         ANV_FROM_HANDLE(anv_pipeline, pipeline_lib, libs_info->pLibraries[i]);
         struct anv_graphics_lib_pipeline *gfx_pipeline_lib =
            anv_pipeline_to_graphics_lib(pipeline_lib);

         vk_graphics_pipeline_state_merge(&pipeline->state, &gfx_pipeline_lib->state);
         anv_graphics_pipeline_import_lib(&pipeline->base,
                                          false /* link_optimize */,
                                          pipeline->retain_shaders,
                                          stages, gfx_pipeline_lib);
      }
   }

   result = vk_graphics_pipeline_state_fill(&device->vk,
                                            &pipeline->state, pCreateInfo,
                                            NULL /* driver_rp */,
                                            0 /* driver_rp_flags */,
                                            &pipeline->all_state, NULL, 0, NULL);
   if (result != VK_SUCCESS) {
      anv_pipeline_finish(&pipeline->base.base, device);
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      return result;
   }

   pipeline->base.base.active_stages = pipeline->state.shader_stages;

   /* After we've imported all the libraries' layouts, import the pipeline
    * layout and hash the whole lot.
    */
   ANV_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   if (pipeline_layout != NULL)
      anv_graphics_pipeline_import_layout(&pipeline->base, pipeline_layout);

   anv_pipeline_sets_layout_hash(&pipeline->base.base.layout);

   /* Compile shaders. We can skip this if there are no active stage in that
    * pipeline.
    */
   if (pipeline->base.base.active_stages != 0) {
      result = anv_graphics_pipeline_compile(&pipeline->base, stages,
                                             cache, &pipeline_feedback,
                                             pCreateInfo, &pipeline->state);
      if (result != VK_SUCCESS) {
         anv_pipeline_finish(&pipeline->base.base, device);
         vk_free2(&device->vk.alloc, pAllocator, pipeline);
         return result;
      }
   }

   pipeline_feedback.duration = os_time_get_nano() - pipeline_start;

   anv_fill_pipeline_creation_feedback(&pipeline->base, &pipeline_feedback,
                                       pCreateInfo, stages);

   anv_graphics_lib_validate_shaders(
      pipeline,
      flags & VK_PIPELINE_CREATE_2_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT);

   *pPipeline = anv_pipeline_to_handle(&pipeline->base.base);

   return VK_SUCCESS;
}

static VkResult
anv_graphics_pipeline_create(struct anv_device *device,
                             struct vk_pipeline_cache *cache,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipeline)
{
   struct anv_pipeline_stage stages[ANV_GRAPHICS_SHADER_STAGE_COUNT] = {};
   VkPipelineCreationFeedback pipeline_feedback = {
      .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
   };
   int64_t pipeline_start = os_time_get_nano();

   struct anv_graphics_pipeline *pipeline;
   VkResult result;

   const VkPipelineCreateFlags2KHR flags =
      vk_graphics_pipeline_create_flags(pCreateInfo);
   assert((flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) == 0);

   const VkPipelineLibraryCreateInfoKHR *libs_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           PIPELINE_LIBRARY_CREATE_INFO_KHR);

   pipeline = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Initialize some information required by shaders */
   result = anv_pipeline_init(&pipeline->base.base, device,
                              ANV_PIPELINE_GRAPHICS, flags,
                              pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      return result;
   }

   const bool link_optimize =
      (flags & VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT) != 0;

   struct vk_graphics_pipeline_all_state all;
   struct vk_graphics_pipeline_state state = { };

   /* If we have libraries, import them first. */
   if (libs_info) {
      for (uint32_t i = 0; i < libs_info->libraryCount; i++) {
         ANV_FROM_HANDLE(anv_pipeline, pipeline_lib, libs_info->pLibraries[i]);
         struct anv_graphics_lib_pipeline *gfx_pipeline_lib =
            anv_pipeline_to_graphics_lib(pipeline_lib);

         /* If we have link time optimization, all libraries must be created
          * with
          * VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT.
          */
         assert(!link_optimize || gfx_pipeline_lib->retain_shaders);

         vk_graphics_pipeline_state_merge(&state, &gfx_pipeline_lib->state);
         anv_graphics_pipeline_import_lib(&pipeline->base,
                                          link_optimize,
                                          false,
                                          stages,
                                          gfx_pipeline_lib);
      }
   }

   result = vk_graphics_pipeline_state_fill(&device->vk, &state, pCreateInfo,
                                            NULL /* driver_rp */,
                                            0 /* driver_rp_flags */,
                                            &all, NULL, 0, NULL);
   if (result != VK_SUCCESS) {
      anv_pipeline_finish(&pipeline->base.base, device);
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      return result;
   }

   pipeline->dynamic_state.vi = &pipeline->vertex_input;
   pipeline->dynamic_state.ms.sample_locations = &pipeline->base.sample_locations;
   vk_dynamic_graphics_state_fill(&pipeline->dynamic_state, &state);

   pipeline->base.base.active_stages = state.shader_stages;

   /* Sanity check on the shaders */
   assert(pipeline->base.base.active_stages & VK_SHADER_STAGE_VERTEX_BIT ||
          pipeline->base.base.active_stages & VK_SHADER_STAGE_MESH_BIT_EXT);

   if (anv_pipeline_is_mesh(pipeline)) {
      assert(device->physical->vk.supported_extensions.EXT_mesh_shader);
   }

   /* After we've imported all the libraries' layouts, import the pipeline
    * layout and hash the whole lot.
    */
   ANV_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   if (pipeline_layout != NULL)
      anv_graphics_pipeline_import_layout(&pipeline->base, pipeline_layout);

   anv_pipeline_sets_layout_hash(&pipeline->base.base.layout);

   /* Compile shaders, all required information should be have been copied in
    * the previous step. We can skip this if there are no active stage in that
    * pipeline.
    */
   result = anv_graphics_pipeline_compile(&pipeline->base, stages,
                                          cache, &pipeline_feedback,
                                          pCreateInfo, &state);
   if (result != VK_SUCCESS) {
      anv_pipeline_finish(&pipeline->base.base, device);
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      return result;
   }

   /* Prepare a batch for the commands and emit all the non dynamic ones.
    */
   anv_batch_set_storage(&pipeline->base.base.batch, ANV_NULL_ADDRESS,
                         pipeline->batch_data, sizeof(pipeline->batch_data));

   if (pipeline->base.base.active_stages & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
      pipeline->base.base.active_stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;

   if (anv_pipeline_is_mesh(pipeline))
      assert(device->physical->vk.supported_extensions.EXT_mesh_shader);

   anv_graphics_pipeline_emit(pipeline, &state);

   pipeline_feedback.duration = os_time_get_nano() - pipeline_start;

   anv_fill_pipeline_creation_feedback(&pipeline->base, &pipeline_feedback,
                                       pCreateInfo, stages);

   ANV_RMV(graphics_pipeline_create, device, pipeline, false);

   *pPipeline = anv_pipeline_to_handle(&pipeline->base.base);

   return pipeline->base.base.batch.status;
}

VkResult anv_CreateGraphicsPipelines(
    VkDevice                                    _device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    count,
    const VkGraphicsPipelineCreateInfo*         pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(vk_pipeline_cache, pipeline_cache, pipelineCache);

   VkResult result = VK_SUCCESS;

   unsigned i;
   for (i = 0; i < count; i++) {
      assert(pCreateInfos[i].sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);

      const VkPipelineCreateFlags2KHR flags =
         vk_graphics_pipeline_create_flags(&pCreateInfos[i]);
      VkResult res;
      if (flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) {
         res = anv_graphics_lib_pipeline_create(device, pipeline_cache,
                                                &pCreateInfos[i],
                                                pAllocator,
                                                &pPipelines[i]);
      } else {
         res = anv_graphics_pipeline_create(device,
                                            pipeline_cache,
                                            &pCreateInfos[i],
                                            pAllocator, &pPipelines[i]);
      }

      if (res != VK_SUCCESS) {
         result = res;
         if (flags & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR)
            break;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   for (; i < count; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}

static bool
should_remat_cb(nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   return nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_resource_intel;
}

static VkResult
compile_upload_rt_shader(struct anv_ray_tracing_pipeline *pipeline,
                         struct vk_pipeline_cache *cache,
                         nir_shader *nir,
                         struct anv_pipeline_stage *stage,
                         void *mem_ctx)
{
   const struct brw_compiler *compiler =
      pipeline->base.device->physical->compiler;
   const struct intel_device_info *devinfo = compiler->devinfo;

   struct brw_nir_lower_shader_calls_state lowering_state = {
      .devinfo = devinfo,
      .key = &stage->key.bs,
   };

   nir_shader **resume_shaders = NULL;
   uint32_t num_resume_shaders = 0;
   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      const nir_lower_shader_calls_options opts = {
         .address_format = nir_address_format_64bit_global,
         .stack_alignment = BRW_BTD_STACK_ALIGN,
         .localized_loads = true,
         .vectorizer_callback = brw_nir_should_vectorize_mem,
         .vectorizer_data = NULL,
         .should_remat_callback = should_remat_cb,
      };

      NIR_PASS(_, nir, brw_nir_lower_rt_intrinsics_pre_trace);

      NIR_PASS(_, nir, nir_lower_shader_calls, &opts,
               &resume_shaders, &num_resume_shaders, mem_ctx);
      NIR_PASS(_, nir, brw_nir_lower_shader_calls, &lowering_state);
      NIR_PASS(_, nir, brw_nir_lower_rt_intrinsics, &stage->key.base, devinfo);
   }

   for (unsigned i = 0; i < num_resume_shaders; i++) {
      NIR_PASS(_,resume_shaders[i], brw_nir_lower_shader_calls, &lowering_state);
      NIR_PASS(_, resume_shaders[i], brw_nir_lower_rt_intrinsics, &stage->key.base, devinfo);
   }

   struct brw_compile_bs_params params = {
      .base = {
         .nir = nir,
         .stats = stage->stats,
         .log_data = pipeline->base.device,
         .mem_ctx = mem_ctx,
         .source_hash = stage->source_hash,
      },
      .key = &stage->key.bs,
      .prog_data = &stage->prog_data.bs,
      .num_resume_shaders = num_resume_shaders,
      .resume_shaders = resume_shaders,
   };

   stage->code = brw_compile_bs(compiler, &params);
   if (stage->code == NULL) {
      VkResult result;

      if (params.base.error_str)
         result = vk_errorf(pipeline, VK_ERROR_UNKNOWN, "%s", params.base.error_str);
      else
         result = vk_error(pipeline, VK_ERROR_OUT_OF_HOST_MEMORY);

      return result;
   }

   struct anv_shader_upload_params upload_params = {
      .stage               = stage->stage,
      .key_data            = &stage->cache_key,
      .key_size            = sizeof(stage->cache_key),
      .kernel_data         = stage->code,
      .kernel_size         = stage->prog_data.base.program_size,
      .prog_data           = &stage->prog_data.base,
      .prog_data_size      = brw_prog_data_size(stage->stage),
      .stats               = stage->stats,
      .num_stats           = 1,
      .bind_map            = &stage->bind_map,
      .push_desc_info      = &stage->push_desc_info,
   };

   stage->bin =
      anv_device_upload_kernel(pipeline->base.device, cache, &upload_params);
   if (stage->bin == NULL)
      return vk_error(pipeline, VK_ERROR_OUT_OF_HOST_MEMORY);

   anv_pipeline_add_executables(&pipeline->base, stage);

   return VK_SUCCESS;
}

static bool
is_rt_stack_size_dynamic(const VkRayTracingPipelineCreateInfoKHR *info)
{
   if (info->pDynamicState == NULL)
      return false;

   for (unsigned i = 0; i < info->pDynamicState->dynamicStateCount; i++) {
      if (info->pDynamicState->pDynamicStates[i] ==
          VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR)
         return true;
   }

   return false;
}

static void
anv_pipeline_compute_ray_tracing_stacks(struct anv_ray_tracing_pipeline *pipeline,
                                        const VkRayTracingPipelineCreateInfoKHR *info,
                                        uint32_t *stack_max)
{
   if (is_rt_stack_size_dynamic(info)) {
      pipeline->stack_size = 0; /* 0 means dynamic */
   } else {
      /* From the Vulkan spec:
       *
       *    "If the stack size is not set explicitly, the stack size for a
       *    pipeline is:
       *
       *       rayGenStackMax +
       *       min(1, maxPipelineRayRecursionDepth) ×
       *       max(closestHitStackMax, missStackMax,
       *           intersectionStackMax + anyHitStackMax) +
       *       max(0, maxPipelineRayRecursionDepth-1) ×
       *       max(closestHitStackMax, missStackMax) +
       *       2 × callableStackMax"
       */
      pipeline->stack_size =
         stack_max[MESA_SHADER_RAYGEN] +
         MIN2(1, info->maxPipelineRayRecursionDepth) *
         MAX4(stack_max[MESA_SHADER_CLOSEST_HIT],
              stack_max[MESA_SHADER_MISS],
              stack_max[MESA_SHADER_INTERSECTION],
              stack_max[MESA_SHADER_ANY_HIT]) +
         MAX2(0, (int)info->maxPipelineRayRecursionDepth - 1) *
         MAX2(stack_max[MESA_SHADER_CLOSEST_HIT],
              stack_max[MESA_SHADER_MISS]) +
         2 * stack_max[MESA_SHADER_CALLABLE];

      /* This is an extremely unlikely case but we need to set it to some
       * non-zero value so that we don't accidentally think it's dynamic.
       * Our minimum stack size is 2KB anyway so we could set to any small
       * value we like.
       */
      if (pipeline->stack_size == 0)
         pipeline->stack_size = 1;
   }
}

static enum brw_rt_ray_flags
anv_pipeline_get_pipeline_ray_flags(VkPipelineCreateFlags2KHR flags)
{
   uint32_t ray_flags = 0;

   const bool rt_skip_triangles =
      flags & VK_PIPELINE_CREATE_2_RAY_TRACING_SKIP_TRIANGLES_BIT_KHR;
   const bool rt_skip_aabbs =
      flags & VK_PIPELINE_CREATE_2_RAY_TRACING_SKIP_AABBS_BIT_KHR;
   assert(!(rt_skip_triangles && rt_skip_aabbs));

   if (rt_skip_triangles)
      ray_flags |= BRW_RT_RAY_FLAG_SKIP_TRIANGLES;
   else if (rt_skip_aabbs)
      ray_flags |= BRW_RT_RAY_FLAG_SKIP_AABBS;

   return ray_flags;
}

static struct anv_pipeline_stage *
anv_pipeline_init_ray_tracing_stages(struct anv_ray_tracing_pipeline *pipeline,
                                     const VkRayTracingPipelineCreateInfoKHR *info,
                                     void *tmp_pipeline_ctx)
{
   struct anv_device *device = pipeline->base.device;
   /* Create enough stage entries for all shader modules plus potential
    * combinaisons in the groups.
    */
   struct anv_pipeline_stage *stages =
      rzalloc_array(tmp_pipeline_ctx, struct anv_pipeline_stage, info->stageCount);

   enum brw_rt_ray_flags ray_flags =
      anv_pipeline_get_pipeline_ray_flags(pipeline->base.flags);

   for (uint32_t i = 0; i < info->stageCount; i++) {
      const VkPipelineShaderStageCreateInfo *sinfo = &info->pStages[i];
      if (vk_pipeline_shader_stage_is_null(sinfo))
         continue;

      int64_t stage_start = os_time_get_nano();

      stages[i] = (struct anv_pipeline_stage) {
         .stage = vk_to_mesa_shader_stage(sinfo->stage),
         .pipeline_flags = pipeline->base.flags,
         .pipeline_pNext = info->pNext,
         .info = sinfo,
         .cache_key = {
            .stage = vk_to_mesa_shader_stage(sinfo->stage),
         },
         .feedback = {
            .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
         },
      };

      anv_stage_allocate_bind_map_tables(&pipeline->base, &stages[i],
                                         tmp_pipeline_ctx);

      pipeline->base.active_stages |= sinfo->stage;

      anv_stage_write_shader_hash(&stages[i], device);

      populate_bs_prog_key(&stages[i],
                           pipeline->base.device,
                           ray_flags);

      if (stages[i].stage != MESA_SHADER_INTERSECTION) {
         anv_pipeline_hash_ray_tracing_shader(pipeline, &stages[i],
                                              stages[i].cache_key.sha1);
      }

      stages[i].feedback.duration += os_time_get_nano() - stage_start;
   }

   for (uint32_t i = 0; i < info->groupCount; i++) {
      const VkRayTracingShaderGroupCreateInfoKHR *ginfo = &info->pGroups[i];

      if (ginfo->type != VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR)
         continue;

      int64_t stage_start = os_time_get_nano();

      uint32_t intersection_idx = ginfo->intersectionShader;
      assert(intersection_idx < info->stageCount);

      uint32_t any_hit_idx = ginfo->anyHitShader;
      if (any_hit_idx != VK_SHADER_UNUSED_KHR) {
         assert(any_hit_idx < info->stageCount);
         anv_pipeline_hash_ray_tracing_combined_shader(pipeline,
                                                       &stages[intersection_idx],
                                                       &stages[any_hit_idx],
                                                       stages[intersection_idx].cache_key.sha1);
      } else {
         anv_pipeline_hash_ray_tracing_shader(pipeline,
                                              &stages[intersection_idx],
                                              stages[intersection_idx].cache_key.sha1);
      }

      stages[intersection_idx].feedback.duration += os_time_get_nano() - stage_start;
   }

   return stages;
}

static bool
anv_ray_tracing_pipeline_load_cached_shaders(struct anv_ray_tracing_pipeline *pipeline,
                                             struct vk_pipeline_cache *cache,
                                             const VkRayTracingPipelineCreateInfoKHR *info,
                                             struct anv_pipeline_stage *stages,
                                             VkPipelineCreationFeedback *pipeline_feedback)
{
   uint32_t shaders = 0, found = 0, cache_hits = 0;
   for (uint32_t i = 0; i < info->stageCount; i++) {
      if (stages[i].info == NULL)
         continue;

      shaders++;

      int64_t stage_start = os_time_get_nano();

      bool cache_hit;
      stages[i].bin = anv_device_search_for_kernel(pipeline->base.device, cache,
                                                   &stages[i].cache_key,
                                                   sizeof(stages[i].cache_key),
                                                   &cache_hit);
      if (cache_hit) {
         cache_hits++;
         stages[i].feedback.flags |=
            VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      }

      if (stages[i].bin != NULL) {
         found++;
         anv_pipeline_add_executables(&pipeline->base, &stages[i]);
      }

      stages[i].feedback.duration += os_time_get_nano() - stage_start;
   }

   if (cache_hits == shaders) {
      pipeline_feedback->flags |=
         VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
   }

   return found == shaders;
}

static VkResult
anv_pipeline_compile_ray_tracing(struct anv_ray_tracing_pipeline *pipeline,
                                 void *tmp_pipeline_ctx,
                                 struct anv_pipeline_stage *stages,
                                 struct vk_pipeline_cache *cache,
                                 const VkRayTracingPipelineCreateInfoKHR *info)
{
   const struct intel_device_info *devinfo = pipeline->base.device->info;
   VkResult result;

   VkPipelineCreationFeedback pipeline_feedback = {
      .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
   };
   int64_t pipeline_start = os_time_get_nano();

   const bool skip_cache_lookup =
      (pipeline->base.flags & VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR);

   if (!skip_cache_lookup &&
       anv_ray_tracing_pipeline_load_cached_shaders(pipeline, cache, info, stages,
                                                    &pipeline_feedback)) {
      goto done;
   }

   if (pipeline->base.flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
      return VK_PIPELINE_COMPILE_REQUIRED;

   for (uint32_t i = 0; i < info->stageCount; i++) {
      if (stages[i].info == NULL)
         continue;

      /* Intersection and any-hit need to fetch the nir always,
       * so that they can be handled correctly below in the group section.
       * For the other stages, if we found them in the cache, skip this part.
       */
      if (!(stages[i].stage == MESA_SHADER_INTERSECTION ||
            stages[i].stage == MESA_SHADER_ANY_HIT) &&
          stages[i].bin != NULL)
         continue;

      int64_t stage_start = os_time_get_nano();

      VkResult result = anv_pipeline_stage_get_nir(&pipeline->base, cache,
                                                   tmp_pipeline_ctx,
                                                   &stages[i]);
      if (result != VK_SUCCESS)
         return result;

      anv_pipeline_nir_preprocess(&pipeline->base, &stages[i]);

      anv_pipeline_lower_nir(&pipeline->base, tmp_pipeline_ctx, &stages[i],
                             &pipeline->base.layout, 0 /* view_mask */,
                             false /* use_primitive_replication */);

      stages[i].feedback.duration += os_time_get_nano() - stage_start;
   }

   for (uint32_t i = 0; i < info->stageCount; i++) {
      if (stages[i].info == NULL)
         continue;

      /* Shader found in cache already. */
      if (stages[i].bin != NULL)
         continue;

      /* We handle intersection shaders as part of the group */
      if (stages[i].stage == MESA_SHADER_INTERSECTION)
         continue;

      int64_t stage_start = os_time_get_nano();

      void *tmp_stage_ctx = ralloc_context(tmp_pipeline_ctx);

      nir_shader *nir = nir_shader_clone(tmp_stage_ctx, stages[i].nir);
      switch (stages[i].stage) {
      case MESA_SHADER_RAYGEN:
         brw_nir_lower_raygen(nir, devinfo);
         break;

      case MESA_SHADER_ANY_HIT:
         brw_nir_lower_any_hit(nir, devinfo);
         break;

      case MESA_SHADER_CLOSEST_HIT:
         brw_nir_lower_closest_hit(nir, devinfo);
         break;

      case MESA_SHADER_MISS:
         brw_nir_lower_miss(nir, devinfo);
         break;

      case MESA_SHADER_INTERSECTION:
         UNREACHABLE("These are handled later");

      case MESA_SHADER_CALLABLE:
         brw_nir_lower_callable(nir, devinfo);
         break;

      default:
         UNREACHABLE("Invalid ray-tracing shader stage");
      }

      result = compile_upload_rt_shader(pipeline, cache, nir, &stages[i],
                                        tmp_stage_ctx);
      if (result != VK_SUCCESS) {
         ralloc_free(tmp_stage_ctx);
         return result;
      }

      ralloc_free(tmp_stage_ctx);

      stages[i].feedback.duration += os_time_get_nano() - stage_start;
   }

 done:
   for (uint32_t i = 0; i < info->groupCount; i++) {
      const VkRayTracingShaderGroupCreateInfoKHR *ginfo = &info->pGroups[i];
      struct anv_rt_shader_group *group = &pipeline->groups[i];
      group->type = ginfo->type;
      switch (ginfo->type) {
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR:
         assert(ginfo->generalShader < info->stageCount);
         group->general = stages[ginfo->generalShader].bin;
         break;

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:
         if (ginfo->anyHitShader < info->stageCount)
            group->any_hit = stages[ginfo->anyHitShader].bin;

         if (ginfo->closestHitShader < info->stageCount)
            group->closest_hit = stages[ginfo->closestHitShader].bin;
         break;

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR: {
         if (ginfo->closestHitShader < info->stageCount)
            group->closest_hit = stages[ginfo->closestHitShader].bin;

         uint32_t intersection_idx = info->pGroups[i].intersectionShader;
         assert(intersection_idx < info->stageCount);

         /* Only compile this stage if not already found in the cache. */
         if (stages[intersection_idx].bin == NULL) {
            /* The any-hit and intersection shader have to be combined */
            uint32_t any_hit_idx = info->pGroups[i].anyHitShader;
            const nir_shader *any_hit = NULL;
            if (any_hit_idx < info->stageCount)
               any_hit = stages[any_hit_idx].nir;

            void *tmp_group_ctx = ralloc_context(tmp_pipeline_ctx);
            nir_shader *intersection =
               nir_shader_clone(tmp_group_ctx, stages[intersection_idx].nir);

            brw_nir_lower_combined_intersection_any_hit(intersection, any_hit,
                                                        devinfo);

            result = compile_upload_rt_shader(pipeline, cache,
                                              intersection,
                                              &stages[intersection_idx],
                                              tmp_group_ctx);
            ralloc_free(tmp_group_ctx);
            if (result != VK_SUCCESS)
               return result;
         }

         group->intersection = stages[intersection_idx].bin;
         break;
      }

      default:
         UNREACHABLE("Invalid ray tracing shader group type");
      }
   }

   pipeline_feedback.duration = os_time_get_nano() - pipeline_start;

   const VkPipelineCreationFeedbackCreateInfo *create_feedback =
      vk_find_struct_const(info->pNext, PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
   if (create_feedback) {
      *create_feedback->pPipelineCreationFeedback = pipeline_feedback;

      uint32_t stage_count = create_feedback->pipelineStageCreationFeedbackCount;
      assert(stage_count == 0 || info->stageCount == stage_count);
      for (uint32_t i = 0; i < stage_count; i++) {
         gl_shader_stage s = vk_to_mesa_shader_stage(info->pStages[i].stage);
         create_feedback->pPipelineStageCreationFeedbacks[i] = stages[s].feedback;
      }
   }

   return VK_SUCCESS;
}

VkResult
anv_device_init_rt_shaders(struct anv_device *device)
{
   if (!device->vk.enabled_extensions.KHR_ray_tracing_pipeline)
      return VK_SUCCESS;

   bool cache_hit;

   struct anv_push_descriptor_info empty_push_desc_info = {};
   struct anv_pipeline_bind_map empty_bind_map = {};
   struct brw_rt_trampoline {
      char name[16];
      struct brw_cs_prog_key key;
   } trampoline_key = {
      .name = "rt-trampoline",
   };
   device->rt_trampoline =
      anv_device_search_for_kernel(device, device->internal_cache,
                                   &trampoline_key, sizeof(trampoline_key),
                                   &cache_hit);
   if (device->rt_trampoline == NULL) {

      void *tmp_ctx = ralloc_context(NULL);
      nir_shader *trampoline_nir =
         brw_nir_create_raygen_trampoline(device->physical->compiler, tmp_ctx);

      if (device->info->ver >= 20)
         trampoline_nir->info.subgroup_size = SUBGROUP_SIZE_REQUIRE_16;
      else
         trampoline_nir->info.subgroup_size = SUBGROUP_SIZE_REQUIRE_8;

      struct brw_cs_prog_data trampoline_prog_data = {
         .uses_btd_stack_ids = true,
      };
      struct brw_compile_cs_params params = {
         .base = {
            .nir = trampoline_nir,
            .log_data = device,
            .mem_ctx = tmp_ctx,
         },
         .key = &trampoline_key.key,
         .prog_data = &trampoline_prog_data,
      };
      const unsigned *tramp_data =
         brw_compile_cs(device->physical->compiler, &params);

      struct anv_shader_upload_params upload_params = {
         .stage               = MESA_SHADER_COMPUTE,
         .key_data            = &trampoline_key,
         .key_size            = sizeof(trampoline_key),
         .kernel_data         = tramp_data,
         .kernel_size         = trampoline_prog_data.base.program_size,
         .prog_data           = &trampoline_prog_data.base,
         .prog_data_size      = sizeof(trampoline_prog_data),
         .bind_map            = &empty_bind_map,
         .push_desc_info      = &empty_push_desc_info,
      };

      device->rt_trampoline =
         anv_device_upload_kernel(device, device->internal_cache,
                                  &upload_params);

      ralloc_free(tmp_ctx);

      if (device->rt_trampoline == NULL)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, device->rt_trampoline);

   struct brw_rt_trivial_return {
      char name[16];
      struct brw_bs_prog_key key;
   } return_key = {
      .name = "rt-trivial-ret",
   };
   device->rt_trivial_return =
      anv_device_search_for_kernel(device, device->internal_cache,
                                   &return_key, sizeof(return_key),
                                   &cache_hit);
   if (device->rt_trivial_return == NULL) {
      void *tmp_ctx = ralloc_context(NULL);
      nir_shader *trivial_return_nir =
         brw_nir_create_trivial_return_shader(device->physical->compiler, tmp_ctx);

      NIR_PASS(_, trivial_return_nir, brw_nir_lower_rt_intrinsics,
                 &return_key.key.base, device->info);

      struct brw_bs_prog_data return_prog_data = { 0, };
      struct brw_compile_bs_params params = {
         .base = {
            .nir = trivial_return_nir,
            .log_data = device,
            .mem_ctx = tmp_ctx,
         },
         .key = &return_key.key,
         .prog_data = &return_prog_data,
      };
      const unsigned *return_data =
         brw_compile_bs(device->physical->compiler, &params);

      struct anv_shader_upload_params upload_params = {
         .stage               = MESA_SHADER_CALLABLE,
         .key_data            = &return_key,
         .key_size            = sizeof(return_key),
         .kernel_data         = return_data,
         .kernel_size         = return_prog_data.base.program_size,
         .prog_data           = &return_prog_data.base,
         .prog_data_size      = sizeof(return_prog_data),
         .bind_map            = &empty_bind_map,
         .push_desc_info      = &empty_push_desc_info,
      };

      device->rt_trivial_return =
         anv_device_upload_kernel(device, device->internal_cache,
                                  &upload_params);

      ralloc_free(tmp_ctx);

      if (device->rt_trivial_return == NULL)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, device->rt_trivial_return);

   struct brw_rt_null_ahs {
      char name[16];
      struct brw_bs_prog_key key;
   } null_return_key = {
      .name = "rt-null-ahs",
   };
   device->rt_null_ahs =
      anv_device_search_for_kernel(device, device->internal_cache,
                                   &null_return_key, sizeof(null_return_key),
                                   &cache_hit);
   if (device->rt_null_ahs == NULL) {
      void *tmp_ctx = ralloc_context(NULL);
      nir_shader *null_ahs_nir =
         brw_nir_create_null_ahs_shader(device->physical->compiler, tmp_ctx);

      NIR_PASS(_, null_ahs_nir, brw_nir_lower_rt_intrinsics,
                 &null_return_key.key.base, device->info);

      struct brw_bs_prog_data return_prog_data = { 0, };
      struct brw_compile_bs_params params = {
         .base = {
            .nir = null_ahs_nir,
            .log_data = device,
            .mem_ctx = tmp_ctx,
         },
         .key = &null_return_key.key,
         .prog_data = &return_prog_data,
      };
      const unsigned *return_data =
         brw_compile_bs(device->physical->compiler, &params);

      struct anv_shader_upload_params upload_params = {
         .stage               = MESA_SHADER_CALLABLE,
         .key_data            = &null_return_key,
         .key_size            = sizeof(null_return_key),
         .kernel_data         = return_data,
         .kernel_size         = return_prog_data.base.program_size,
         .prog_data           = &return_prog_data.base,
         .prog_data_size      = sizeof(return_prog_data),
         .bind_map            = &empty_bind_map,
         .push_desc_info      = &empty_push_desc_info,
      };

      device->rt_null_ahs =
         anv_device_upload_kernel(device, device->internal_cache,
                                  &upload_params);

      ralloc_free(tmp_ctx);

      if (device->rt_null_ahs == NULL)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, device->rt_null_ahs);

   return VK_SUCCESS;
}

void
anv_device_finish_rt_shaders(struct anv_device *device)
{
   if (!device->vk.enabled_extensions.KHR_ray_tracing_pipeline)
      return;
}

static void
anv_ray_tracing_pipeline_init(struct anv_ray_tracing_pipeline *pipeline,
                              struct anv_device *device,
                              struct vk_pipeline_cache *cache,
                              const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                              const VkAllocationCallbacks *alloc)
{
   util_dynarray_init(&pipeline->shaders, pipeline->base.mem_ctx);

   ANV_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   anv_pipeline_init_layout(&pipeline->base, pipeline_layout);
}

static void
assert_rt_stage_index_valid(const VkRayTracingPipelineCreateInfoKHR* pCreateInfo,
                            uint32_t stage_idx,
                            VkShaderStageFlags valid_stages)
{
   if (stage_idx == VK_SHADER_UNUSED_KHR)
      return;

   assert(stage_idx <= pCreateInfo->stageCount);
   assert(util_bitcount(pCreateInfo->pStages[stage_idx].stage) == 1);
   assert(pCreateInfo->pStages[stage_idx].stage & valid_stages);
}

static VkResult
anv_ray_tracing_pipeline_create(
    VkDevice                                    _device,
    struct vk_pipeline_cache *                  cache,
    const VkRayTracingPipelineCreateInfoKHR*    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR);

   uint32_t group_count = pCreateInfo->groupCount;
   if (pCreateInfo->pLibraryInfo) {
      for (uint32_t l = 0; l < pCreateInfo->pLibraryInfo->libraryCount; l++) {
         ANV_FROM_HANDLE(anv_pipeline, library,
                         pCreateInfo->pLibraryInfo->pLibraries[l]);
         struct anv_ray_tracing_pipeline *rt_library =
            anv_pipeline_to_ray_tracing(library);
         group_count += rt_library->group_count;
      }
   }

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct anv_ray_tracing_pipeline, pipeline, 1);
   VK_MULTIALLOC_DECL(&ma, struct anv_rt_shader_group, groups, group_count);
   if (!vk_multialloc_zalloc2(&ma, &device->vk.alloc, pAllocator,
                              VK_SYSTEM_ALLOCATION_SCOPE_DEVICE))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_pipeline_init(&pipeline->base, device,
                              ANV_PIPELINE_RAY_TRACING,
                              vk_rt_pipeline_create_flags(pCreateInfo),
                              pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      return result;
   }

   pipeline->group_count = group_count;
   pipeline->groups = groups;

   ASSERTED const VkShaderStageFlags ray_tracing_stages =
      VK_SHADER_STAGE_RAYGEN_BIT_KHR |
      VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
      VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
      VK_SHADER_STAGE_MISS_BIT_KHR |
      VK_SHADER_STAGE_INTERSECTION_BIT_KHR |
      VK_SHADER_STAGE_CALLABLE_BIT_KHR;

   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++)
      assert((pCreateInfo->pStages[i].stage & ~ray_tracing_stages) == 0);

   for (uint32_t i = 0; i < pCreateInfo->groupCount; i++) {
      const VkRayTracingShaderGroupCreateInfoKHR *ginfo =
         &pCreateInfo->pGroups[i];
      assert_rt_stage_index_valid(pCreateInfo, ginfo->generalShader,
                                  VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                  VK_SHADER_STAGE_MISS_BIT_KHR |
                                  VK_SHADER_STAGE_CALLABLE_BIT_KHR);
      assert_rt_stage_index_valid(pCreateInfo, ginfo->closestHitShader,
                                  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
      assert_rt_stage_index_valid(pCreateInfo, ginfo->anyHitShader,
                                  VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
      assert_rt_stage_index_valid(pCreateInfo, ginfo->intersectionShader,
                                  VK_SHADER_STAGE_INTERSECTION_BIT_KHR);
      switch (ginfo->type) {
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR:
         assert(ginfo->generalShader < pCreateInfo->stageCount);
         assert(ginfo->anyHitShader == VK_SHADER_UNUSED_KHR);
         assert(ginfo->closestHitShader == VK_SHADER_UNUSED_KHR);
         assert(ginfo->intersectionShader == VK_SHADER_UNUSED_KHR);
         break;

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:
         assert(ginfo->generalShader == VK_SHADER_UNUSED_KHR);
         assert(ginfo->intersectionShader == VK_SHADER_UNUSED_KHR);
         break;

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR:
         assert(ginfo->generalShader == VK_SHADER_UNUSED_KHR);
         break;

      default:
         UNREACHABLE("Invalid ray-tracing shader group type");
      }
   }

   anv_ray_tracing_pipeline_init(pipeline, device, cache,
                                 pCreateInfo, pAllocator);

   void *tmp_ctx = ralloc_context(NULL);

   struct anv_pipeline_stage *stages =
      anv_pipeline_init_ray_tracing_stages(pipeline, pCreateInfo, tmp_ctx);

   result = anv_pipeline_compile_ray_tracing(pipeline, tmp_ctx, stages,
                                             cache, pCreateInfo);
   if (result != VK_SUCCESS) {
      for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
         if (stages[i].bin != NULL)
            anv_shader_bin_unref(device, stages[i].bin);
      }
      ralloc_free(tmp_ctx);
      anv_pipeline_finish(&pipeline->base, device);
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      return result;
   }

   /* Compute the size of the scratch BO (for register spilling) by taking the
    * max of all the shaders in the pipeline. Also add the shaders to the list
    * of executables.
    */
   uint32_t stack_max[MESA_VULKAN_SHADER_STAGES] = {};
   for (uint32_t s = 0; s < pCreateInfo->stageCount; s++) {
      util_dynarray_append(&pipeline->shaders,
                           struct anv_shader_bin *,
                           stages[s].bin);

      uint32_t stack_size =
         brw_bs_prog_data_const(stages[s].bin->prog_data)->max_stack_size;
      stack_max[stages[s].stage] = MAX2(stack_max[stages[s].stage], stack_size);

      anv_pipeline_account_shader(&pipeline->base, stages[s].bin);
   }

   anv_pipeline_compute_ray_tracing_stacks(pipeline, pCreateInfo, stack_max);

   if (pCreateInfo->pLibraryInfo) {
      uint32_t g = pCreateInfo->groupCount;
      for (uint32_t l = 0; l < pCreateInfo->pLibraryInfo->libraryCount; l++) {
         ANV_FROM_HANDLE(anv_pipeline, library,
                         pCreateInfo->pLibraryInfo->pLibraries[l]);
         struct anv_ray_tracing_pipeline *rt_library =
            anv_pipeline_to_ray_tracing(library);
         for (uint32_t lg = 0; lg < rt_library->group_count; lg++) {
            pipeline->groups[g] = rt_library->groups[lg];
            pipeline->groups[g].imported = true;
            g++;
         }

         /* Account for shaders in the library. */
         util_dynarray_foreach(&rt_library->shaders,
                               struct anv_shader_bin *, shader) {
            util_dynarray_append(&pipeline->shaders,
                                 struct anv_shader_bin *,
                                 anv_shader_bin_ref(*shader));
            anv_pipeline_account_shader(&pipeline->base, *shader);
         }

         /* Add the library shaders to this pipeline's executables. */
         util_dynarray_foreach(&rt_library->base.executables,
                               struct anv_pipeline_executable, exe) {
            util_dynarray_append(&pipeline->base.executables,
                                 struct anv_pipeline_executable, *exe);
         }

         pipeline->base.active_stages |= rt_library->base.active_stages;
      }
   }

   anv_genX(device->info, ray_tracing_pipeline_emit)(pipeline);

   ralloc_free(tmp_ctx);

   ANV_RMV(rt_pipeline_create, device, pipeline, false);

   *pPipeline = anv_pipeline_to_handle(&pipeline->base);

   return pipeline->base.batch.status;
}

VkResult
anv_CreateRayTracingPipelinesKHR(
    VkDevice                                    _device,
    VkDeferredOperationKHR                      deferredOperation,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR*    pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines)
{
   ANV_FROM_HANDLE(vk_pipeline_cache, pipeline_cache, pipelineCache);

   VkResult result = VK_SUCCESS;

   unsigned i;
   for (i = 0; i < createInfoCount; i++) {
      const VkPipelineCreateFlags2KHR flags =
         vk_rt_pipeline_create_flags(&pCreateInfos[i]);
      VkResult res = anv_ray_tracing_pipeline_create(_device, pipeline_cache,
                                                     &pCreateInfos[i],
                                                     pAllocator, &pPipelines[i]);

      if (res != VK_SUCCESS) {
         result = res;
         if (flags & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR)
            break;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   for (; i < createInfoCount; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}

VkResult anv_GetPipelineExecutablePropertiesKHR(
    VkDevice                                    device,
    const VkPipelineInfoKHR*                    pPipelineInfo,
    uint32_t*                                   pExecutableCount,
    VkPipelineExecutablePropertiesKHR*          pProperties)
{
   ANV_FROM_HANDLE(anv_pipeline, pipeline, pPipelineInfo->pipeline);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutablePropertiesKHR, out,
                          pProperties, pExecutableCount);

   util_dynarray_foreach (&pipeline->executables, struct anv_pipeline_executable, exe) {
      vk_outarray_append_typed(VkPipelineExecutablePropertiesKHR, &out, props) {
         gl_shader_stage stage = exe->stage;
         props->stages = mesa_to_vk_shader_stage(stage);

         unsigned simd_width = exe->stats.dispatch_width;
         if (stage == MESA_SHADER_FRAGMENT) {
            if (exe->stats.max_polygons > 1)
               VK_PRINT_STR(props->name, "SIMD%dx%d %s",
                         exe->stats.max_polygons,
                         simd_width / exe->stats.max_polygons,
                         _mesa_shader_stage_to_string(stage));
            else
               VK_PRINT_STR(props->name, "%s%d %s",
                         simd_width ? "SIMD" : "vec",
                         simd_width ? simd_width : 4,
                         _mesa_shader_stage_to_string(stage));
         } else {
            VK_COPY_STR(props->name, _mesa_shader_stage_to_string(stage));
         }
         VK_PRINT_STR(props->description, "%s%d %s shader",
                   simd_width ? "SIMD" : "vec",
                   simd_width ? simd_width : 4,
                   _mesa_shader_stage_to_string(stage));

         /* The compiler gives us a dispatch width of 0 for vec4 but Vulkan
          * wants a subgroup size of 1.
          */
         props->subgroupSize = MAX2(simd_width, 1);
      }
   }

   return vk_outarray_status(&out);
}

static const struct anv_pipeline_executable *
anv_pipeline_get_executable(struct anv_pipeline *pipeline, uint32_t index)
{
   assert(index < util_dynarray_num_elements(&pipeline->executables,
                                             struct anv_pipeline_executable));
   return util_dynarray_element(
      &pipeline->executables, struct anv_pipeline_executable, index);
}

VkResult anv_GetPipelineExecutableStatisticsKHR(
    VkDevice                                    device,
    const VkPipelineExecutableInfoKHR*          pExecutableInfo,
    uint32_t*                                   pStatisticCount,
    VkPipelineExecutableStatisticKHR*           pStatistics)
{
   ANV_FROM_HANDLE(anv_pipeline, pipeline, pExecutableInfo->pipeline);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutableStatisticKHR, out,
                          pStatistics, pStatisticCount);

   const struct anv_pipeline_executable *exe =
      anv_pipeline_get_executable(pipeline, pExecutableInfo->executableIndex);

   const struct brw_stage_prog_data *prog_data;
   switch (pipeline->type) {
   case ANV_PIPELINE_GRAPHICS:
   case ANV_PIPELINE_GRAPHICS_LIB: {
      prog_data = anv_pipeline_to_graphics_base(pipeline)->shaders[exe->stage]->prog_data;
      break;
   }
   case ANV_PIPELINE_COMPUTE: {
      prog_data = anv_pipeline_to_compute(pipeline)->cs->prog_data;
      break;
   }
   case ANV_PIPELINE_RAY_TRACING: {
      struct anv_shader_bin **shader =
         util_dynarray_element(&anv_pipeline_to_ray_tracing(pipeline)->shaders,
                               struct anv_shader_bin *,
                               pExecutableInfo->executableIndex);
      prog_data = (*shader)->prog_data;
      break;
   }
   default:
      UNREACHABLE("invalid pipeline type");
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Instruction Count");
      VK_COPY_STR(stat->description,
                "Number of GEN instructions in the final generated "
                "shader executable.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = exe->stats.instructions;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "SEND Count");
      VK_COPY_STR(stat->description,
                "Number of instructions in the final generated shader "
                "executable which access external units such as the "
                "constant cache or the sampler.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = exe->stats.sends;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Loop Count");
      VK_COPY_STR(stat->description,
                "Number of loops (not unrolled) in the final generated "
                "shader executable.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = exe->stats.loops;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Cycle Count");
      VK_COPY_STR(stat->description,
                "Estimate of the number of EU cycles required to execute "
                "the final generated executable.  This is an estimate only "
                "and may vary greatly from actual run-time performance.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = exe->stats.cycles;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Spill Count");
      VK_COPY_STR(stat->description,
                "Number of scratch spill operations.  This gives a rough "
                "estimate of the cost incurred due to spilling temporary "
                "values to memory.  If this is non-zero, you may want to "
                "adjust your shader to reduce register pressure.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = exe->stats.spills;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Fill Count");
      VK_COPY_STR(stat->description,
                "Number of scratch fill operations.  This gives a rough "
                "estimate of the cost incurred due to spilling temporary "
                "values to memory.  If this is non-zero, you may want to "
                "adjust your shader to reduce register pressure.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = exe->stats.fills;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Scratch Memory Size");
      VK_COPY_STR(stat->description,
                "Number of bytes of scratch memory required by the "
                "generated shader executable.  If this is non-zero, you "
                "may want to adjust your shader to reduce register "
                "pressure.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = prog_data->total_scratch;
   }

   if (pipeline->device->info->ver >= 30) {
      vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
         VK_COPY_STR(stat->name, "GRF registers");
         VK_COPY_STR(stat->description,
                   "Number of GRF registers required by the shader.");
         stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
         stat->value.u64 = prog_data->grf_used;
      }
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Max dispatch width");
      VK_COPY_STR(stat->description,
                "Largest SIMD dispatch width.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      /* Report the max dispatch width only on the smallest SIMD variant */
      if (exe->stage != MESA_SHADER_FRAGMENT || exe->stats.dispatch_width == 8)
         stat->value.u64 = exe->stats.max_dispatch_width;
      else
         stat->value.u64 = 0;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Max live registers");
      VK_COPY_STR(stat->description,
                "Maximum number of registers used across the entire shader.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = exe->stats.max_live_registers;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Workgroup Memory Size");
      VK_COPY_STR(stat->description,
                "Number of bytes of workgroup shared memory used by this "
                "shader including any padding.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      if (gl_shader_stage_uses_workgroup(exe->stage))
         stat->value.u64 = prog_data->total_shared;
      else
         stat->value.u64 = 0;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      uint32_t hash = pipeline->type == ANV_PIPELINE_COMPUTE ?
                      anv_pipeline_to_compute(pipeline)->cs->prog_data->source_hash :
                      (pipeline->type == ANV_PIPELINE_GRAPHICS_LIB ||
                       pipeline->type == ANV_PIPELINE_GRAPHICS) ?
                      anv_pipeline_to_graphics_base(pipeline)->shaders[
                         exe->stage]->prog_data->source_hash:
                      0 /* No source hash for ray tracing */;
      VK_COPY_STR(stat->name, "Source hash");
      VK_PRINT_STR(stat->description,
                "hash = 0x%08x. Hash generated from shader source.", hash);
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = hash;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      VK_COPY_STR(stat->name, "Non SSA regs after NIR");
      VK_COPY_STR(stat->description, "Non SSA regs after NIR translation to BRW.");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = exe->stats.non_ssa_registers_after_nir;
   }

   return vk_outarray_status(&out);
}

static bool
write_ir_text(VkPipelineExecutableInternalRepresentationKHR* ir,
              const char *data)
{
   ir->isText = VK_TRUE;

   size_t data_len = strlen(data) + 1;

   if (ir->pData == NULL) {
      ir->dataSize = data_len;
      return true;
   }

   strncpy(ir->pData, data, ir->dataSize);
   if (ir->dataSize < data_len)
      return false;

   ir->dataSize = data_len;
   return true;
}

VkResult anv_GetPipelineExecutableInternalRepresentationsKHR(
    VkDevice                                    device,
    const VkPipelineExecutableInfoKHR*          pExecutableInfo,
    uint32_t*                                   pInternalRepresentationCount,
    VkPipelineExecutableInternalRepresentationKHR* pInternalRepresentations)
{
   ANV_FROM_HANDLE(anv_pipeline, pipeline, pExecutableInfo->pipeline);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutableInternalRepresentationKHR, out,
                          pInternalRepresentations, pInternalRepresentationCount);
   bool incomplete_text = false;

   const struct anv_pipeline_executable *exe =
      anv_pipeline_get_executable(pipeline, pExecutableInfo->executableIndex);

   if (exe->nir) {
      vk_outarray_append_typed(VkPipelineExecutableInternalRepresentationKHR, &out, ir) {
         VK_COPY_STR(ir->name, "Final NIR");
         VK_COPY_STR(ir->description,
                   "Final NIR before going into the back-end compiler");

         if (!write_ir_text(ir, exe->nir))
            incomplete_text = true;
      }
   }

   if (exe->disasm) {
      vk_outarray_append_typed(VkPipelineExecutableInternalRepresentationKHR, &out, ir) {
         VK_COPY_STR(ir->name, "GEN Assembly");
         VK_COPY_STR(ir->description,
                   "Final GEN assembly for the generated shader binary");

         if (!write_ir_text(ir, exe->disasm))
            incomplete_text = true;
      }
   }

   return incomplete_text ? VK_INCOMPLETE : vk_outarray_status(&out);
}

VkResult
anv_GetRayTracingShaderGroupHandlesKHR(
    VkDevice                                    _device,
    VkPipeline                                  _pipeline,
    uint32_t                                    firstGroup,
    uint32_t                                    groupCount,
    size_t                                      dataSize,
    void*                                       pData)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline, pipeline, _pipeline);

   if (pipeline->type != ANV_PIPELINE_RAY_TRACING)
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);

   struct anv_ray_tracing_pipeline *rt_pipeline =
      anv_pipeline_to_ray_tracing(pipeline);

   assert(firstGroup + groupCount <= rt_pipeline->group_count);
   for (uint32_t i = 0; i < groupCount; i++) {
      struct anv_rt_shader_group *group = &rt_pipeline->groups[firstGroup + i];
      memcpy(pData, group->handle, sizeof(group->handle));
      pData += sizeof(group->handle);
   }

   return VK_SUCCESS;
}

VkResult
anv_GetRayTracingCaptureReplayShaderGroupHandlesKHR(
    VkDevice                                    _device,
    VkPipeline                                  pipeline,
    uint32_t                                    firstGroup,
    uint32_t                                    groupCount,
    size_t                                      dataSize,
    void*                                       pData)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   UNREACHABLE("Unimplemented");
   return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
}

VkDeviceSize
anv_GetRayTracingShaderGroupStackSizeKHR(
    VkDevice                                    device,
    VkPipeline                                  _pipeline,
    uint32_t                                    group,
    VkShaderGroupShaderKHR                      groupShader)
{
   ANV_FROM_HANDLE(anv_pipeline, pipeline, _pipeline);
   assert(pipeline->type == ANV_PIPELINE_RAY_TRACING);

   struct anv_ray_tracing_pipeline *rt_pipeline =
      anv_pipeline_to_ray_tracing(pipeline);

   assert(group < rt_pipeline->group_count);

   struct anv_shader_bin *bin;
   switch (groupShader) {
   case VK_SHADER_GROUP_SHADER_GENERAL_KHR:
      bin = rt_pipeline->groups[group].general;
      break;

   case VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR:
      bin = rt_pipeline->groups[group].closest_hit;
      break;

   case VK_SHADER_GROUP_SHADER_ANY_HIT_KHR:
      bin = rt_pipeline->groups[group].any_hit;
      break;

   case VK_SHADER_GROUP_SHADER_INTERSECTION_KHR:
      bin = rt_pipeline->groups[group].intersection;
      break;

   default:
      UNREACHABLE("Invalid VkShaderGroupShader enum");
   }

   if (bin == NULL)
      return 0;

   return brw_bs_prog_data_const(bin->prog_data)->max_stack_size;
}
