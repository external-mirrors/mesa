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

#ifndef ANV_NIR_H
#define ANV_NIR_H

#include "nir/nir.h"
#include "anv_private.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_pipeline_robustness_state;

#define anv_drv_const_offset(field) \
   (offsetof(struct anv_push_constants, field))
#define anv_drv_const_size(field) \
   (sizeof(((struct anv_push_constants *)0)->field))

#define anv_load_driver_uniform(b, components, field)                   \
   nir_load_push_constant(b, components,                                \
                          anv_drv_const_size(field) * 8,                \
                          nir_imm_int(b, 0),                            \
                          .base = anv_drv_const_offset(field),          \
                          .range = components * anv_drv_const_size(field))
/* Use load_uniform for indexed values since load_push_constant requires that
 * the offset source is dynamically uniform in the subgroup which we cannot
 * guarantee.
 */
#define anv_load_driver_uniform_indexed(b, components, field, idx)      \
   nir_load_uniform(b, components,                                      \
                    anv_drv_const_size(field[0]) * 8,                   \
                    nir_imul_imm(b, idx,                                \
                                 anv_drv_const_size(field[0])),         \
                    .base = anv_drv_const_offset(field),                \
                    .range = anv_drv_const_size(field))

/* This map is represent a mapping where the key is the NIR
 * nir_intrinsic_resource_intel::block index. It allows mapping bindless UBOs
 * accesses to descriptor entry.
 *
 * This map only temporary lives between the anv_nir_apply_pipeline_layout()
 * and anv_nir_compute_push_layout() passes.
 */
struct anv_pipeline_push_map {
   uint32_t                     block_count;
   struct anv_pipeline_binding *block_to_descriptor;
};

enum brw_robustness_flags
anv_get_robust_flags(const struct vk_pipeline_robustness_state *rstate);

bool anv_check_for_primitive_replication(struct anv_device *device,
                                         VkShaderStageFlags stages,
                                         nir_shader **shaders,
                                         uint32_t view_mask);

bool anv_nir_lower_multiview(nir_shader *shader, uint32_t view_mask,
                             bool use_primitive_replication);

bool anv_nir_lower_ycbcr_textures(nir_shader *shader,
                                  const struct anv_pipeline_sets_layout *layout);

static inline nir_address_format
anv_nir_ssbo_addr_format(const struct anv_physical_device *pdevice,
                         enum brw_robustness_flags robust_flags)
{
   if (robust_flags & BRW_ROBUSTNESS_SSBO)
      return nir_address_format_64bit_bounded_global;
   else
      return nir_address_format_64bit_global_32bit_offset;
}

static inline nir_address_format
anv_nir_ubo_addr_format(const struct anv_physical_device *pdevice,
                        enum brw_robustness_flags robust_flags)
{
   if (robust_flags & BRW_ROBUSTNESS_UBO)
      return nir_address_format_64bit_bounded_global;
   else
      return nir_address_format_64bit_global_32bit_offset;
}

bool anv_nir_lower_ubo_loads(nir_shader *shader);

bool anv_nir_apply_pipeline_layout(nir_shader *shader,
                                   const struct anv_physical_device *pdevice,
                                   enum brw_robustness_flags robust_flags,
                                   struct anv_descriptor_set_layout * const *set_layouts,
                                   uint32_t set_count,
                                   const uint32_t *dynamic_offset_start,
                                   struct anv_pipeline_bind_map *map,
                                   struct anv_pipeline_push_map *push_map,
                                   void *push_map_mem_ctx);

bool anv_nir_compute_push_layout(nir_shader *nir,
                                 const struct anv_physical_device *pdevice,
                                 enum brw_robustness_flags robust_flags,
                                 bool fragment_dynamic,
                                 bool mesh_dynamic,
                                 struct brw_stage_prog_data *prog_data,
                                 struct anv_pipeline_bind_map *map,
                                 const struct anv_pipeline_push_map *push_map,
                                 void *mem_ctx);

void anv_nir_validate_push_layout(const struct anv_physical_device *pdevice,
                                  struct brw_stage_prog_data *prog_data,
                                  struct anv_pipeline_bind_map *map);

bool anv_nir_update_resource_intel_block(nir_shader *shader);

bool anv_nir_lower_resource_intel(nir_shader *shader,
                                  const struct anv_physical_device *device,
                                  enum anv_descriptor_set_layout_type desc_type);

bool anv_nir_add_base_work_group_id(nir_shader *shader);

uint32_t anv_nir_compute_used_push_descriptors(nir_shader *shader,
                                               struct anv_descriptor_set_layout * const *set_layouts,
                                               uint32_t set_count);

uint8_t anv_nir_loads_push_desc_buffer(nir_shader *nir,
                                       struct anv_descriptor_set_layout * const *set_layouts,
                                       uint32_t set_count,
                                       const struct anv_pipeline_bind_map *bind_map);

uint32_t anv_nir_push_desc_ubo_fully_promoted(nir_shader *nir,
                                              struct anv_descriptor_set_layout * const *set_layouts,
                                              uint32_t set_count,
                                              const struct anv_pipeline_bind_map *bind_map);

void anv_apply_per_prim_attr_wa(struct nir_shader *ms_nir,
                                struct nir_shader *fs_nir,
                                struct anv_device *device);

#ifdef __cplusplus
}
#endif

#endif /* ANV_NIR_H */
