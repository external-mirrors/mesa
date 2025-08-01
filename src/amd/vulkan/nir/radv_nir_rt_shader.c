/*
 * Copyright © 2021 Google
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir/nir.h"
#include "nir/nir_builder.h"

#include "bvh/bvh.h"
#include "meta/radv_meta.h"
#include "nir/radv_meta_nir.h"
#include "nir/radv_nir.h"
#include "nir/radv_nir_rt_common.h"
#include "ac_nir.h"
#include "radv_pipeline_cache.h"
#include "radv_pipeline_rt.h"
#include "radv_shader.h"

#include "vk_pipeline.h"

/* Traversal stack size. This stack is put in LDS and experimentally 16 entries results in best
 * performance. */
#define MAX_STACK_ENTRY_COUNT 16

#define RADV_RT_SWITCH_NULL_CHECK_THRESHOLD 3

/* Minimum number of inlined shaders to use binary search to select which shader to run. */
#define INLINED_SHADER_BSEARCH_THRESHOLD 16

struct radv_rt_case_data {
   struct radv_device *device;
   struct radv_ray_tracing_pipeline *pipeline;
   struct rt_variables *vars;
};

typedef void (*radv_get_group_info)(struct radv_ray_tracing_group *, uint32_t *, uint32_t *,
                                    struct radv_rt_case_data *);
typedef void (*radv_insert_shader_case)(nir_builder *, nir_def *, struct radv_ray_tracing_group *,
                                        struct radv_rt_case_data *);

struct inlined_shader_case {
   struct radv_ray_tracing_group *group;
   uint32_t call_idx;
};

static int
compare_inlined_shader_case(const void *a, const void *b)
{
   const struct inlined_shader_case *visit_a = a;
   const struct inlined_shader_case *visit_b = b;
   return visit_a->call_idx > visit_b->call_idx ? 1 : visit_a->call_idx < visit_b->call_idx ? -1 : 0;
}

static void
insert_inlined_range(nir_builder *b, nir_def *sbt_idx, radv_insert_shader_case shader_case,
                     struct radv_rt_case_data *data, struct inlined_shader_case *cases, uint32_t length)
{
   if (length >= INLINED_SHADER_BSEARCH_THRESHOLD) {
      nir_push_if(b, nir_ige_imm(b, sbt_idx, cases[length / 2].call_idx));
      {
         insert_inlined_range(b, sbt_idx, shader_case, data, cases + (length / 2), length - (length / 2));
      }
      nir_push_else(b, NULL);
      {
         insert_inlined_range(b, sbt_idx, shader_case, data, cases, length / 2);
      }
      nir_pop_if(b, NULL);
   } else {
      for (uint32_t i = 0; i < length; ++i)
         shader_case(b, sbt_idx, cases[i].group, data);
   }
}

static void
radv_visit_inlined_shaders(nir_builder *b, nir_def *sbt_idx, bool can_have_null_shaders, struct radv_rt_case_data *data,
                           radv_get_group_info group_info, radv_insert_shader_case shader_case)
{
   struct inlined_shader_case *cases = calloc(data->pipeline->group_count, sizeof(struct inlined_shader_case));
   uint32_t case_count = 0;

   for (unsigned i = 0; i < data->pipeline->group_count; i++) {
      struct radv_ray_tracing_group *group = &data->pipeline->groups[i];

      uint32_t shader_index = VK_SHADER_UNUSED_KHR;
      uint32_t handle_index = VK_SHADER_UNUSED_KHR;
      group_info(group, &shader_index, &handle_index, data);
      if (shader_index == VK_SHADER_UNUSED_KHR)
         continue;

      /* Avoid emitting stages with the same shaders/handles multiple times. */
      bool duplicate = false;
      for (unsigned j = 0; j < i; j++) {
         uint32_t other_shader_index = VK_SHADER_UNUSED_KHR;
         uint32_t other_handle_index = VK_SHADER_UNUSED_KHR;
         group_info(&data->pipeline->groups[j], &other_shader_index, &other_handle_index, data);

         if (handle_index == other_handle_index) {
            duplicate = true;
            break;
         }
      }

      if (!duplicate) {
         cases[case_count++] = (struct inlined_shader_case){
            .group = group,
            .call_idx = handle_index,
         };
      }
   }

   qsort(cases, case_count, sizeof(struct inlined_shader_case), compare_inlined_shader_case);

   /* Do not emit 'if (sbt_idx != 0) { ... }' is there are only a few cases. */
   can_have_null_shaders &= case_count >= RADV_RT_SWITCH_NULL_CHECK_THRESHOLD;

   if (can_have_null_shaders)
      nir_push_if(b, nir_ine_imm(b, sbt_idx, 0));

   insert_inlined_range(b, sbt_idx, shader_case, data, cases, case_count);

   if (can_have_null_shaders)
      nir_pop_if(b, NULL);

   free(cases);
}

static bool
lower_rt_derefs(nir_shader *shader)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   bool progress = false;

   nir_builder b = nir_builder_at(nir_before_impl(impl));

   nir_def *arg_offset = nir_load_rt_arg_scratch_offset_amd(&b);

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_deref)
            continue;

         nir_deref_instr *deref = nir_instr_as_deref(instr);
         if (!nir_deref_mode_is(deref, nir_var_shader_call_data))
            continue;

         deref->modes = nir_var_function_temp;
         progress = true;

         if (deref->deref_type == nir_deref_type_var) {
            b.cursor = nir_before_instr(&deref->instr);
            nir_deref_instr *replacement =
               nir_build_deref_cast(&b, arg_offset, nir_var_function_temp, deref->var->type, 0);
            nir_def_replace(&deref->def, &replacement->def);
         }
      }
   }

   return nir_progress(progress, impl, nir_metadata_control_flow);
}

/*
 * Global variables for an RT pipeline
 */
struct rt_variables {
   struct radv_device *device;
   const VkPipelineCreateFlags2 flags;
   bool monolithic;

   /* idx of the next shader to run in the next iteration of the main loop.
    * During traversal, idx is used to store the SBT index and will contain
    * the correct resume index upon returning.
    */
   nir_variable *idx;
   nir_variable *shader_addr;
   nir_variable *traversal_addr;

   /* scratch offset of the argument area relative to stack_ptr */
   nir_variable *arg;
   uint32_t payload_offset;

   nir_variable *stack_ptr;

   nir_variable *ahit_isec_count;

   nir_variable *launch_sizes[3];
   nir_variable *launch_ids[3];

   /* global address of the SBT entry used for the shader */
   nir_variable *shader_record_ptr;

   /* trace_ray arguments */
   nir_variable *accel_struct;
   nir_variable *cull_mask_and_flags;
   nir_variable *sbt_offset;
   nir_variable *sbt_stride;
   nir_variable *miss_index;
   nir_variable *origin;
   nir_variable *tmin;
   nir_variable *direction;
   nir_variable *tmax;

   /* Properties of the primitive currently being visited. */
   nir_variable *primitive_addr;
   nir_variable *primitive_id;
   nir_variable *geometry_id_and_flags;
   nir_variable *instance_addr;
   nir_variable *hit_kind;
   nir_variable *opaque;

   /* Output variables for intersection & anyhit shaders. */
   nir_variable *ahit_accept;
   nir_variable *ahit_terminate;
   nir_variable *terminated;

   unsigned stack_size;
};

static struct rt_variables
create_rt_variables(nir_shader *shader, struct radv_device *device, const VkPipelineCreateFlags2 flags, bool monolithic)
{
   struct rt_variables vars = {
      .device = device,
      .flags = flags,
      .monolithic = monolithic,
   };
   vars.idx = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "idx");
   vars.shader_addr = nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "shader_addr");
   vars.traversal_addr = nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "traversal_addr");
   vars.arg = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "arg");
   vars.stack_ptr = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "stack_ptr");
   vars.shader_record_ptr = nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "shader_record_ptr");

   vars.launch_sizes[0] = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "launch_size_x");
   vars.launch_sizes[1] = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "launch_size_y");
   vars.launch_sizes[2] = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "launch_size_z");

   vars.launch_ids[0] = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "launch_id_x");
   vars.launch_ids[1] = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "launch_id_y");
   vars.launch_ids[2] = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "launch_id_z");

   if (device->rra_trace.ray_history_addr)
      vars.ahit_isec_count = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "ahit_isec_count");

   const struct glsl_type *vec3_type = glsl_vector_type(GLSL_TYPE_FLOAT, 3);
   vars.accel_struct = nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "accel_struct");
   vars.cull_mask_and_flags = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "cull_mask_and_flags");
   vars.sbt_offset = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "sbt_offset");
   vars.sbt_stride = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "sbt_stride");
   vars.miss_index = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "miss_index");
   vars.origin = nir_variable_create(shader, nir_var_shader_temp, vec3_type, "ray_origin");
   vars.tmin = nir_variable_create(shader, nir_var_shader_temp, glsl_float_type(), "ray_tmin");
   vars.direction = nir_variable_create(shader, nir_var_shader_temp, vec3_type, "ray_direction");
   vars.tmax = nir_variable_create(shader, nir_var_shader_temp, glsl_float_type(), "ray_tmax");

   vars.primitive_addr = nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "primitive_addr");
   vars.primitive_id = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "primitive_id");
   vars.geometry_id_and_flags =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "geometry_id_and_flags");
   vars.instance_addr = nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "instance_addr");
   vars.hit_kind = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "hit_kind");
   vars.opaque = nir_variable_create(shader, nir_var_shader_temp, glsl_bool_type(), "opaque");

   vars.ahit_accept = nir_variable_create(shader, nir_var_shader_temp, glsl_bool_type(), "ahit_accept");
   vars.ahit_terminate = nir_variable_create(shader, nir_var_shader_temp, glsl_bool_type(), "ahit_terminate");
   vars.terminated = nir_variable_create(shader, nir_var_shader_temp, glsl_bool_type(), "terminated");

   return vars;
}

/*
 * Remap all the variables between the two rt_variables struct for inlining.
 */
static void
map_rt_variables(struct hash_table *var_remap, struct rt_variables *src, const struct rt_variables *dst)
{
   _mesa_hash_table_insert(var_remap, src->idx, dst->idx);
   _mesa_hash_table_insert(var_remap, src->shader_addr, dst->shader_addr);
   _mesa_hash_table_insert(var_remap, src->traversal_addr, dst->traversal_addr);
   _mesa_hash_table_insert(var_remap, src->arg, dst->arg);
   _mesa_hash_table_insert(var_remap, src->stack_ptr, dst->stack_ptr);
   _mesa_hash_table_insert(var_remap, src->shader_record_ptr, dst->shader_record_ptr);

   for (uint32_t i = 0; i < ARRAY_SIZE(src->launch_sizes); i++)
      _mesa_hash_table_insert(var_remap, src->launch_sizes[i], dst->launch_sizes[i]);

   for (uint32_t i = 0; i < ARRAY_SIZE(src->launch_ids); i++)
      _mesa_hash_table_insert(var_remap, src->launch_ids[i], dst->launch_ids[i]);

   if (dst->ahit_isec_count)
      _mesa_hash_table_insert(var_remap, src->ahit_isec_count, dst->ahit_isec_count);

   _mesa_hash_table_insert(var_remap, src->accel_struct, dst->accel_struct);
   _mesa_hash_table_insert(var_remap, src->cull_mask_and_flags, dst->cull_mask_and_flags);
   _mesa_hash_table_insert(var_remap, src->sbt_offset, dst->sbt_offset);
   _mesa_hash_table_insert(var_remap, src->sbt_stride, dst->sbt_stride);
   _mesa_hash_table_insert(var_remap, src->miss_index, dst->miss_index);
   _mesa_hash_table_insert(var_remap, src->origin, dst->origin);
   _mesa_hash_table_insert(var_remap, src->tmin, dst->tmin);
   _mesa_hash_table_insert(var_remap, src->direction, dst->direction);
   _mesa_hash_table_insert(var_remap, src->tmax, dst->tmax);

   _mesa_hash_table_insert(var_remap, src->primitive_addr, dst->primitive_addr);
   _mesa_hash_table_insert(var_remap, src->primitive_id, dst->primitive_id);
   _mesa_hash_table_insert(var_remap, src->geometry_id_and_flags, dst->geometry_id_and_flags);
   _mesa_hash_table_insert(var_remap, src->instance_addr, dst->instance_addr);
   _mesa_hash_table_insert(var_remap, src->hit_kind, dst->hit_kind);
   _mesa_hash_table_insert(var_remap, src->opaque, dst->opaque);
   _mesa_hash_table_insert(var_remap, src->ahit_accept, dst->ahit_accept);
   _mesa_hash_table_insert(var_remap, src->ahit_terminate, dst->ahit_terminate);
   _mesa_hash_table_insert(var_remap, src->terminated, dst->terminated);
}

/*
 * Create a copy of the global rt variables where the primitive/instance related variables are
 * independent.This is needed as we need to keep the old values of the global variables around
 * in case e.g. an anyhit shader reject the collision. So there are inner variables that get copied
 * to the outer variables once we commit to a better hit.
 */
static struct rt_variables
create_inner_vars(nir_builder *b, const struct rt_variables *vars)
{
   struct rt_variables inner_vars = *vars;
   inner_vars.idx = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_idx");
   inner_vars.shader_record_ptr =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint64_t_type(), "inner_shader_record_ptr");
   inner_vars.primitive_addr =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint64_t_type(), "inner_primitive_addr");
   inner_vars.primitive_id =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_primitive_id");
   inner_vars.geometry_id_and_flags =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_geometry_id_and_flags");
   inner_vars.tmax = nir_variable_create(b->shader, nir_var_shader_temp, glsl_float_type(), "inner_tmax");
   inner_vars.instance_addr =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint64_t_type(), "inner_instance_addr");
   inner_vars.hit_kind = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_hit_kind");

   return inner_vars;
}

static void
insert_rt_return(nir_builder *b, const struct rt_variables *vars)
{
   nir_store_var(b, vars->stack_ptr, nir_iadd_imm(b, nir_load_var(b, vars->stack_ptr), -16), 1);
   nir_store_var(b, vars->shader_addr, nir_load_scratch(b, 1, 64, nir_load_var(b, vars->stack_ptr), .align_mul = 16),
                 1);
}

enum sbt_type {
   SBT_RAYGEN = offsetof(VkTraceRaysIndirectCommand2KHR, raygenShaderRecordAddress),
   SBT_MISS = offsetof(VkTraceRaysIndirectCommand2KHR, missShaderBindingTableAddress),
   SBT_HIT = offsetof(VkTraceRaysIndirectCommand2KHR, hitShaderBindingTableAddress),
   SBT_CALLABLE = offsetof(VkTraceRaysIndirectCommand2KHR, callableShaderBindingTableAddress),
};

enum sbt_entry {
   SBT_RECURSIVE_PTR = offsetof(struct radv_pipeline_group_handle, recursive_shader_ptr),
   SBT_GENERAL_IDX = offsetof(struct radv_pipeline_group_handle, general_index),
   SBT_CLOSEST_HIT_IDX = offsetof(struct radv_pipeline_group_handle, closest_hit_index),
   SBT_INTERSECTION_IDX = offsetof(struct radv_pipeline_group_handle, intersection_index),
   SBT_ANY_HIT_IDX = offsetof(struct radv_pipeline_group_handle, any_hit_index),
};

static void
load_sbt_entry(nir_builder *b, const struct rt_variables *vars, nir_def *idx, enum sbt_type binding,
               enum sbt_entry offset)
{
   nir_def *desc_base_addr = nir_load_sbt_base_amd(b);

   nir_def *desc = nir_pack_64_2x32(b, nir_load_smem_amd(b, 2, desc_base_addr, nir_imm_int(b, binding)));

   nir_def *stride_offset = nir_imm_int(b, binding + (binding == SBT_RAYGEN ? 8 : 16));
   nir_def *stride = nir_load_smem_amd(b, 1, desc_base_addr, stride_offset);

   nir_def *addr = nir_iadd(b, desc, nir_u2u64(b, nir_iadd_imm(b, nir_imul(b, idx, stride), offset)));

   if (offset == SBT_RECURSIVE_PTR) {
      nir_store_var(b, vars->shader_addr, nir_build_load_global(b, 1, 64, addr), 1);
   } else {
      nir_store_var(b, vars->idx, nir_build_load_global(b, 1, 32, addr), 1);
   }

   nir_def *record_addr = nir_iadd_imm(b, addr, RADV_RT_HANDLE_SIZE - offset);
   nir_store_var(b, vars->shader_record_ptr, record_addr, 1);
}

struct radv_rt_shader_info {
   bool uses_launch_id;
   bool uses_launch_size;
};

struct radv_lower_rt_instruction_data {
   struct rt_variables *vars;
   bool late_lowering;

   struct radv_rt_shader_info *out_info;
};

static bool
radv_lower_rt_instruction(nir_builder *b, nir_instr *instr, void *_data)
{
   if (instr->type == nir_instr_type_jump) {
      nir_jump_instr *jump = nir_instr_as_jump(instr);
      if (jump->type == nir_jump_halt) {
         jump->type = nir_jump_return;
         return true;
      }
      return false;
   } else if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   struct radv_lower_rt_instruction_data *data = _data;
   struct rt_variables *vars = data->vars;

   b->cursor = nir_before_instr(&intr->instr);

   nir_def *ret = NULL;
   switch (intr->intrinsic) {
   case nir_intrinsic_rt_execute_callable: {
      uint32_t size = align(nir_intrinsic_stack_size(intr), 16);
      nir_def *ret_ptr = nir_load_resume_shader_address_amd(b, nir_intrinsic_call_idx(intr));
      ret_ptr = nir_ior_imm(b, ret_ptr, radv_get_rt_priority(b->shader->info.stage));

      nir_store_var(b, vars->stack_ptr, nir_iadd_imm_nuw(b, nir_load_var(b, vars->stack_ptr), size), 1);
      nir_store_scratch(b, ret_ptr, nir_load_var(b, vars->stack_ptr), .align_mul = 16);

      nir_store_var(b, vars->stack_ptr, nir_iadd_imm_nuw(b, nir_load_var(b, vars->stack_ptr), 16), 1);
      load_sbt_entry(b, vars, intr->src[0].ssa, SBT_CALLABLE, SBT_RECURSIVE_PTR);

      nir_store_var(b, vars->arg, nir_iadd_imm(b, intr->src[1].ssa, -size - 16), 1);

      vars->stack_size = MAX2(vars->stack_size, size + 16);
      break;
   }
   case nir_intrinsic_rt_trace_ray: {
      uint32_t size = align(nir_intrinsic_stack_size(intr), 16);
      nir_def *ret_ptr = nir_load_resume_shader_address_amd(b, nir_intrinsic_call_idx(intr));
      ret_ptr = nir_ior_imm(b, ret_ptr, radv_get_rt_priority(b->shader->info.stage));

      nir_store_var(b, vars->stack_ptr, nir_iadd_imm_nuw(b, nir_load_var(b, vars->stack_ptr), size), 1);
      nir_store_scratch(b, ret_ptr, nir_load_var(b, vars->stack_ptr), .align_mul = 16);

      nir_store_var(b, vars->stack_ptr, nir_iadd_imm_nuw(b, nir_load_var(b, vars->stack_ptr), 16), 1);

      nir_store_var(b, vars->shader_addr, nir_load_var(b, vars->traversal_addr), 1);
      nir_store_var(b, vars->arg, nir_iadd_imm(b, intr->src[10].ssa, -size - 16), 1);

      vars->stack_size = MAX2(vars->stack_size, size + 16);

      /* Per the SPIR-V extension spec we have to ignore some bits for some arguments. */
      nir_store_var(b, vars->accel_struct, intr->src[0].ssa, 0x1);
      nir_store_var(b, vars->cull_mask_and_flags, nir_ior(b, nir_ishl_imm(b, intr->src[2].ssa, 24), intr->src[1].ssa),
                    0x1);
      nir_store_var(b, vars->sbt_offset, nir_iand_imm(b, intr->src[3].ssa, 0xf), 0x1);
      nir_store_var(b, vars->sbt_stride, nir_iand_imm(b, intr->src[4].ssa, 0xf), 0x1);
      nir_store_var(b, vars->miss_index, nir_iand_imm(b, intr->src[5].ssa, 0xffff), 0x1);
      nir_store_var(b, vars->origin, intr->src[6].ssa, 0x7);
      nir_store_var(b, vars->tmin, intr->src[7].ssa, 0x1);
      nir_store_var(b, vars->direction, intr->src[8].ssa, 0x7);
      nir_store_var(b, vars->tmax, intr->src[9].ssa, 0x1);
      break;
   }
   case nir_intrinsic_rt_resume: {
      uint32_t size = align(nir_intrinsic_stack_size(intr), 16);

      nir_store_var(b, vars->stack_ptr, nir_iadd_imm(b, nir_load_var(b, vars->stack_ptr), -size), 1);
      break;
   }
   case nir_intrinsic_rt_return_amd: {
      if (b->shader->info.stage == MESA_SHADER_RAYGEN) {
         nir_terminate(b);
         break;
      }
      insert_rt_return(b, vars);
      break;
   }
   case nir_intrinsic_load_scratch: {
      if (data->late_lowering)
         nir_src_rewrite(&intr->src[0], nir_iadd_nuw(b, nir_load_var(b, vars->stack_ptr), intr->src[0].ssa));
      return true;
   }
   case nir_intrinsic_store_scratch: {
      if (data->late_lowering)
         nir_src_rewrite(&intr->src[1], nir_iadd_nuw(b, nir_load_var(b, vars->stack_ptr), intr->src[1].ssa));
      return true;
   }
   case nir_intrinsic_load_rt_arg_scratch_offset_amd: {
      ret = nir_load_var(b, vars->arg);
      break;
   }
   case nir_intrinsic_load_shader_record_ptr: {
      ret = nir_load_var(b, vars->shader_record_ptr);
      break;
   }
   case nir_intrinsic_load_ray_launch_size: {
      if (data->out_info)
         data->out_info->uses_launch_size = true;

      if (!data->late_lowering)
         return false;

      ret = nir_vec3(b, nir_load_var(b, vars->launch_sizes[0]), nir_load_var(b, vars->launch_sizes[1]),
                     nir_load_var(b, vars->launch_sizes[2]));
      break;
   };
   case nir_intrinsic_load_ray_launch_id: {
      if (data->out_info)
         data->out_info->uses_launch_id = true;

      if (!data->late_lowering)
         return false;

      ret = nir_vec3(b, nir_load_var(b, vars->launch_ids[0]), nir_load_var(b, vars->launch_ids[1]),
                     nir_load_var(b, vars->launch_ids[2]));
      break;
   }
   case nir_intrinsic_load_ray_t_min: {
      ret = nir_load_var(b, vars->tmin);
      break;
   }
   case nir_intrinsic_load_ray_t_max: {
      ret = nir_load_var(b, vars->tmax);
      break;
   }
   case nir_intrinsic_load_ray_world_origin: {
      ret = nir_load_var(b, vars->origin);
      break;
   }
   case nir_intrinsic_load_ray_world_direction: {
      ret = nir_load_var(b, vars->direction);
      break;
   }
   case nir_intrinsic_load_ray_instance_custom_index: {
      ret = radv_load_custom_instance(vars->device, b, nir_load_var(b, vars->instance_addr));
      break;
   }
   case nir_intrinsic_load_primitive_id: {
      ret = nir_load_var(b, vars->primitive_id);
      break;
   }
   case nir_intrinsic_load_ray_geometry_index: {
      ret = nir_load_var(b, vars->geometry_id_and_flags);
      ret = nir_iand_imm(b, ret, 0xFFFFFFF);
      break;
   }
   case nir_intrinsic_load_instance_id: {
      ret = radv_load_instance_id(vars->device, b, nir_load_var(b, vars->instance_addr));
      break;
   }
   case nir_intrinsic_load_ray_flags: {
      ret = nir_iand_imm(b, nir_load_var(b, vars->cull_mask_and_flags), 0xFFFFFF);
      break;
   }
   case nir_intrinsic_load_ray_hit_kind: {
      ret = nir_load_var(b, vars->hit_kind);
      break;
   }
   case nir_intrinsic_load_ray_world_to_object: {
      unsigned c = nir_intrinsic_column(intr);
      nir_def *instance_node_addr = nir_load_var(b, vars->instance_addr);
      nir_def *wto_matrix[3];
      radv_load_wto_matrix(vars->device, b, instance_node_addr, wto_matrix);

      nir_def *vals[3];
      for (unsigned i = 0; i < 3; ++i)
         vals[i] = nir_channel(b, wto_matrix[i], c);

      ret = nir_vec(b, vals, 3);
      break;
   }
   case nir_intrinsic_load_ray_object_to_world: {
      unsigned c = nir_intrinsic_column(intr);
      nir_def *otw_matrix[3];
      radv_load_otw_matrix(vars->device, b, nir_load_var(b, vars->instance_addr), otw_matrix);
      ret = nir_vec3(b, nir_channel(b, otw_matrix[0], c), nir_channel(b, otw_matrix[1], c),
                     nir_channel(b, otw_matrix[2], c));
      break;
   }
   case nir_intrinsic_load_ray_object_origin: {
      nir_def *wto_matrix[3];
      radv_load_wto_matrix(vars->device, b, nir_load_var(b, vars->instance_addr), wto_matrix);
      ret = nir_build_vec3_mat_mult(b, nir_load_var(b, vars->origin), wto_matrix, true);
      break;
   }
   case nir_intrinsic_load_ray_object_direction: {
      nir_def *wto_matrix[3];
      radv_load_wto_matrix(vars->device, b, nir_load_var(b, vars->instance_addr), wto_matrix);
      ret = nir_build_vec3_mat_mult(b, nir_load_var(b, vars->direction), wto_matrix, false);
      break;
   }
   case nir_intrinsic_load_intersection_opaque_amd: {
      ret = nir_load_var(b, vars->opaque);
      break;
   }
   case nir_intrinsic_load_cull_mask: {
      ret = nir_ushr_imm(b, nir_load_var(b, vars->cull_mask_and_flags), 24);
      break;
   }
   case nir_intrinsic_ignore_ray_intersection: {
      nir_store_var(b, vars->ahit_accept, nir_imm_false(b), 0x1);

      /* The if is a workaround to avoid having to fix up control flow manually */
      nir_push_if(b, nir_imm_true(b));
      nir_jump(b, nir_jump_return);
      nir_pop_if(b, NULL);
      break;
   }
   case nir_intrinsic_terminate_ray: {
      nir_store_var(b, vars->ahit_accept, nir_imm_true(b), 0x1);
      nir_store_var(b, vars->ahit_terminate, nir_imm_true(b), 0x1);

      /* The if is a workaround to avoid having to fix up control flow manually */
      nir_push_if(b, nir_imm_true(b));
      nir_jump(b, nir_jump_return);
      nir_pop_if(b, NULL);
      break;
   }
   case nir_intrinsic_report_ray_intersection: {
      nir_def *in_range = nir_iand(b, nir_fge(b, nir_load_var(b, vars->tmax), intr->src[0].ssa),
                                   nir_fge(b, intr->src[0].ssa, nir_load_var(b, vars->tmin)));
      nir_def *terminated = nir_load_var(b, vars->terminated);
      nir_push_if(b, nir_iand(b, in_range, nir_inot(b, terminated)));
      {
         nir_store_var(b, vars->ahit_accept, nir_imm_true(b), 0x1);
         nir_store_var(b, vars->tmax, intr->src[0].ssa, 1);
         nir_store_var(b, vars->hit_kind, intr->src[1].ssa, 1);
         nir_def *terminate_on_first_hit =
            nir_test_mask(b, nir_load_var(b, vars->cull_mask_and_flags), SpvRayFlagsTerminateOnFirstHitKHRMask);
         nir_store_var(b, vars->terminated, nir_ior(b, terminate_on_first_hit, nir_load_var(b, vars->ahit_terminate)),
                       1);
      }
      nir_pop_if(b, NULL);
      break;
   }
   case nir_intrinsic_load_sbt_offset_amd: {
      ret = nir_load_var(b, vars->sbt_offset);
      break;
   }
   case nir_intrinsic_load_sbt_stride_amd: {
      ret = nir_load_var(b, vars->sbt_stride);
      break;
   }
   case nir_intrinsic_load_accel_struct_amd: {
      ret = nir_load_var(b, vars->accel_struct);
      break;
   }
   case nir_intrinsic_load_cull_mask_and_flags_amd: {
      ret = nir_load_var(b, vars->cull_mask_and_flags);
      break;
   }
   case nir_intrinsic_execute_closest_hit_amd: {
      nir_store_var(b, vars->tmax, intr->src[1].ssa, 0x1);
      nir_store_var(b, vars->primitive_addr, intr->src[2].ssa, 0x1);
      nir_store_var(b, vars->primitive_id, intr->src[3].ssa, 0x1);
      nir_store_var(b, vars->instance_addr, intr->src[4].ssa, 0x1);
      nir_store_var(b, vars->geometry_id_and_flags, intr->src[5].ssa, 0x1);
      nir_store_var(b, vars->hit_kind, intr->src[6].ssa, 0x1);
      load_sbt_entry(b, vars, intr->src[0].ssa, SBT_HIT, SBT_RECURSIVE_PTR);

      nir_def *should_return =
         nir_test_mask(b, nir_load_var(b, vars->cull_mask_and_flags), SpvRayFlagsSkipClosestHitShaderKHRMask);

      if (!(vars->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_NO_NULL_CLOSEST_HIT_SHADERS_BIT_KHR)) {
         should_return = nir_ior(b, should_return, nir_ieq_imm(b, nir_load_var(b, vars->shader_addr), 0));
      }

      /* should_return is set if we had a hit but we won't be calling the closest hit
       * shader and hence need to return immediately to the calling shader. */
      nir_push_if(b, should_return);
      insert_rt_return(b, vars);
      nir_pop_if(b, NULL);
      break;
   }
   case nir_intrinsic_execute_miss_amd: {
      nir_store_var(b, vars->tmax, intr->src[0].ssa, 0x1);
      nir_def *undef = nir_undef(b, 1, 32);
      nir_store_var(b, vars->primitive_id, undef, 0x1);
      nir_store_var(b, vars->instance_addr, nir_undef(b, 1, 64), 0x1);
      nir_store_var(b, vars->geometry_id_and_flags, undef, 0x1);
      nir_store_var(b, vars->hit_kind, undef, 0x1);
      nir_def *miss_index = nir_load_var(b, vars->miss_index);
      load_sbt_entry(b, vars, miss_index, SBT_MISS, SBT_RECURSIVE_PTR);

      if (!(vars->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_NO_NULL_MISS_SHADERS_BIT_KHR)) {
         /* In case of a NULL miss shader, do nothing and just return. */
         nir_push_if(b, nir_ieq_imm(b, nir_load_var(b, vars->shader_addr), 0));
         insert_rt_return(b, vars);
         nir_pop_if(b, NULL);
      }

      break;
   }
   case nir_intrinsic_load_ray_triangle_vertex_positions: {
      nir_def *primitive_addr = nir_load_var(b, vars->primitive_addr);
      ret = radv_load_vertex_position(vars->device, b, primitive_addr, nir_intrinsic_column(intr));
      break;
   }
   default:
      return false;
   }

   if (ret)
      nir_def_rewrite_uses(&intr->def, ret);
   nir_instr_remove(&intr->instr);

   return true;
}

/* This lowers all the RT instructions that we do not want to pass on to the combined shader and
 * that we can implement using the variables from the shader we are going to inline into. */
static bool
lower_rt_instructions(nir_shader *shader, struct rt_variables *vars, bool late_lowering,
                      struct radv_rt_shader_info *out_info)
{
   struct radv_lower_rt_instruction_data data = {
      .vars = vars,
      .late_lowering = late_lowering,
      .out_info = out_info,
   };
   return nir_shader_instructions_pass(shader, radv_lower_rt_instruction, nir_metadata_none, &data);
}

/* Lowers hit attributes to registers or shared memory. If hit_attribs is NULL, attributes are
 * lowered to shared memory. */
static bool
lower_hit_attribs(nir_shader *shader, nir_variable **hit_attribs, uint32_t workgroup_size)
{
   bool progress = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   nir_foreach_variable_with_modes (attrib, shader, nir_var_ray_hit_attrib) {
      attrib->data.mode = nir_var_shader_temp;
      progress = true;
   }

   nir_builder b = nir_builder_create(impl);

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_load_hit_attrib_amd &&
             intrin->intrinsic != nir_intrinsic_store_hit_attrib_amd)
            continue;

         progress = true;
         b.cursor = nir_after_instr(instr);

         nir_def *offset;
         if (!hit_attribs)
            offset = nir_imul_imm(
               &b, nir_iadd_imm(&b, nir_load_local_invocation_index(&b), nir_intrinsic_base(intrin) * workgroup_size),
               sizeof(uint32_t));

         if (intrin->intrinsic == nir_intrinsic_load_hit_attrib_amd) {
            nir_def *ret;
            if (hit_attribs)
               ret = nir_load_var(&b, hit_attribs[nir_intrinsic_base(intrin)]);
            else
               ret = nir_load_shared(&b, 1, 32, offset, .base = 0, .align_mul = 4);
            nir_def_rewrite_uses(nir_instr_def(instr), ret);
         } else {
            if (hit_attribs)
               nir_store_var(&b, hit_attribs[nir_intrinsic_base(intrin)], intrin->src->ssa, 0x1);
            else
               nir_store_shared(&b, intrin->src->ssa, offset, .base = 0, .align_mul = 4);
         }
         nir_instr_remove(instr);
      }
   }

   if (!hit_attribs)
      shader->info.shared_size = MAX2(shader->info.shared_size, workgroup_size * RADV_MAX_HIT_ATTRIB_SIZE);

   return nir_progress(progress, impl, nir_metadata_control_flow);
}

static void
inline_constants(nir_shader *dst, nir_shader *src)
{
   if (!src->constant_data_size)
      return;

   uint32_t old_constant_data_size = dst->constant_data_size;
   uint32_t base_offset = align(dst->constant_data_size, 64);
   dst->constant_data_size = base_offset + src->constant_data_size;
   dst->constant_data = rerzalloc_size(dst, dst->constant_data, old_constant_data_size, dst->constant_data_size);
   memcpy((char *)dst->constant_data + base_offset, src->constant_data, src->constant_data_size);

   if (!base_offset)
      return;

   uint32_t base_align_mul = base_offset ? 1 << (ffs(base_offset) - 1) : NIR_ALIGN_MUL_MAX;
   nir_foreach_block (block, nir_shader_get_entrypoint(src)) {
      nir_foreach_instr (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instr);
         if (intrinsic->intrinsic == nir_intrinsic_load_constant) {
            nir_intrinsic_set_base(intrinsic, base_offset + nir_intrinsic_base(intrinsic));

            uint32_t align_mul = nir_intrinsic_align_mul(intrinsic);
            uint32_t align_offset = nir_intrinsic_align_offset(intrinsic);
            align_mul = MIN2(align_mul, base_align_mul);
            nir_intrinsic_set_align(intrinsic, align_mul, align_offset % align_mul);
         }
      }
   }
}

static void
insert_rt_case(nir_builder *b, nir_shader *shader, struct rt_variables *vars, nir_def *idx, uint32_t call_idx)
{
   struct hash_table *var_remap = _mesa_pointer_hash_table_create(NULL);

   nir_opt_dead_cf(shader);

   struct rt_variables src_vars = create_rt_variables(shader, vars->device, vars->flags, vars->monolithic);
   map_rt_variables(var_remap, &src_vars, vars);

   NIR_PASS(_, shader, lower_rt_instructions, &src_vars, false, NULL);

   NIR_PASS(_, shader, nir_lower_returns);
   NIR_PASS(_, shader, nir_opt_dce);

   inline_constants(b->shader, shader);

   nir_push_if(b, nir_ieq_imm(b, idx, call_idx));
   nir_inline_function_impl(b, nir_shader_get_entrypoint(shader), NULL, var_remap);
   nir_pop_if(b, NULL);

   ralloc_free(var_remap);
}

void
radv_nir_lower_rt_io(nir_shader *nir, bool monolithic, uint32_t payload_offset)
{
   if (!monolithic) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_function_temp | nir_var_shader_call_data,
               glsl_get_natural_size_align_bytes);

      NIR_PASS(_, nir, lower_rt_derefs);

      NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_function_temp, nir_address_format_32bit_offset);
   } else {
      NIR_PASS(_, nir, radv_nir_lower_ray_payload_derefs, payload_offset);
   }
}

static nir_def *
radv_build_token_begin(nir_builder *b, struct rt_variables *vars, nir_def *hit, enum radv_packed_token_type token_type,
                       nir_def *token_size, uint32_t max_token_size)
{
   struct radv_rra_trace_data *rra_trace = &vars->device->rra_trace;
   assert(rra_trace->ray_history_addr);
   assert(rra_trace->ray_history_buffer_size >= max_token_size);

   nir_def *ray_history_addr = nir_imm_int64(b, rra_trace->ray_history_addr);

   nir_def *launch_id = nir_load_ray_launch_id(b);

   nir_def *trace = nir_imm_true(b);
   for (uint32_t i = 0; i < 3; i++) {
      nir_def *remainder = nir_umod_imm(b, nir_channel(b, launch_id, i), rra_trace->ray_history_resolution_scale);
      trace = nir_iand(b, trace, nir_ieq_imm(b, remainder, 0));
   }
   nir_push_if(b, trace);

   static_assert(offsetof(struct radv_ray_history_header, offset) == 0, "Unexpected offset");
   nir_def *base_offset = nir_global_atomic(b, 32, ray_history_addr, token_size, .atomic_op = nir_atomic_op_iadd);

   /* Abuse the dword alignment of token_size to add an invalid bit to offset. */
   trace = nir_ieq_imm(b, nir_iand_imm(b, base_offset, 1), 0);

   nir_def *in_bounds = nir_ule_imm(b, base_offset, rra_trace->ray_history_buffer_size - max_token_size);
   /* Make sure we don't overwrite the header in case of an overflow. */
   in_bounds = nir_iand(b, in_bounds, nir_uge_imm(b, base_offset, sizeof(struct radv_ray_history_header)));

   nir_push_if(b, nir_iand(b, trace, in_bounds));

   nir_def *dst_addr = nir_iadd(b, ray_history_addr, nir_u2u64(b, base_offset));

   nir_def *launch_size = nir_load_ray_launch_size(b);

   nir_def *launch_id_comps[3];
   nir_def *launch_size_comps[3];
   for (uint32_t i = 0; i < 3; i++) {
      launch_id_comps[i] = nir_udiv_imm(b, nir_channel(b, launch_id, i), rra_trace->ray_history_resolution_scale);
      launch_size_comps[i] = nir_udiv_imm(b, nir_channel(b, launch_size, i), rra_trace->ray_history_resolution_scale);
   }

   nir_def *global_index =
      nir_iadd(b, launch_id_comps[0],
               nir_iadd(b, nir_imul(b, launch_id_comps[1], launch_size_comps[0]),
                        nir_imul(b, launch_id_comps[2], nir_imul(b, launch_size_comps[0], launch_size_comps[1]))));
   nir_def *launch_index_and_hit = nir_bcsel(b, hit, nir_ior_imm(b, global_index, 1u << 29u), global_index);
   nir_build_store_global(b, nir_ior_imm(b, launch_index_and_hit, token_type << 30), dst_addr, .align_mul = 4);

   return nir_iadd_imm(b, dst_addr, 4);
}

static void
radv_build_token_end(nir_builder *b)
{
   nir_pop_if(b, NULL);
   nir_pop_if(b, NULL);
}

static void
radv_build_end_trace_token(nir_builder *b, struct rt_variables *vars, nir_def *tmax, nir_def *hit,
                           nir_def *iteration_instance_count)
{
   nir_def *token_size = nir_bcsel(b, hit, nir_imm_int(b, sizeof(struct radv_packed_end_trace_token)),
                                   nir_imm_int(b, offsetof(struct radv_packed_end_trace_token, primitive_id)));

   nir_def *dst_addr = radv_build_token_begin(b, vars, hit, radv_packed_token_end_trace, token_size,
                                              sizeof(struct radv_packed_end_trace_token));
   {
      nir_build_store_global(b, nir_load_var(b, vars->accel_struct), dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 8);

      nir_def *dispatch_indices =
         nir_load_smem_amd(b, 2, nir_imm_int64(b, vars->device->rra_trace.ray_history_addr),
                           nir_imm_int(b, offsetof(struct radv_ray_history_header, dispatch_index)), .align_mul = 4);
      nir_def *dispatch_index = nir_iadd(b, nir_channel(b, dispatch_indices, 0), nir_channel(b, dispatch_indices, 1));
      nir_def *dispatch_and_flags = nir_iand_imm(b, nir_load_var(b, vars->cull_mask_and_flags), 0xFFFF);
      dispatch_and_flags = nir_ior(b, dispatch_and_flags, dispatch_index);
      nir_build_store_global(b, dispatch_and_flags, dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 4);

      nir_def *shifted_cull_mask = nir_iand_imm(b, nir_load_var(b, vars->cull_mask_and_flags), 0xFF000000);

      nir_def *packed_args = nir_load_var(b, vars->sbt_offset);
      packed_args = nir_ior(b, packed_args, nir_ishl_imm(b, nir_load_var(b, vars->sbt_stride), 4));
      packed_args = nir_ior(b, packed_args, nir_ishl_imm(b, nir_load_var(b, vars->miss_index), 8));
      packed_args = nir_ior(b, packed_args, shifted_cull_mask);
      nir_build_store_global(b, packed_args, dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 4);

      nir_build_store_global(b, nir_load_var(b, vars->origin), dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 12);

      nir_build_store_global(b, nir_load_var(b, vars->tmin), dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 4);

      nir_build_store_global(b, nir_load_var(b, vars->direction), dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 12);

      nir_build_store_global(b, tmax, dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 4);

      nir_build_store_global(b, iteration_instance_count, dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 4);

      nir_build_store_global(b, nir_load_var(b, vars->ahit_isec_count), dst_addr, .align_mul = 4);
      dst_addr = nir_iadd_imm(b, dst_addr, 4);

      nir_push_if(b, hit);
      {
         nir_build_store_global(b, nir_load_var(b, vars->primitive_id), dst_addr, .align_mul = 4);
         dst_addr = nir_iadd_imm(b, dst_addr, 4);

         nir_def *geometry_id = nir_iand_imm(b, nir_load_var(b, vars->geometry_id_and_flags), 0xFFFFFFF);
         nir_build_store_global(b, geometry_id, dst_addr, .align_mul = 4);
         dst_addr = nir_iadd_imm(b, dst_addr, 4);

         nir_def *instance_id_and_hit_kind =
            nir_build_load_global(b, 1, 32,
                                  nir_iadd_imm(b, nir_load_var(b, vars->instance_addr),
                                               offsetof(struct radv_bvh_instance_node, instance_id)));
         instance_id_and_hit_kind =
            nir_ior(b, instance_id_and_hit_kind, nir_ishl_imm(b, nir_load_var(b, vars->hit_kind), 24));
         nir_build_store_global(b, instance_id_and_hit_kind, dst_addr, .align_mul = 4);
         dst_addr = nir_iadd_imm(b, dst_addr, 4);

         nir_build_store_global(b, nir_load_var(b, vars->tmax), dst_addr, .align_mul = 4);
         dst_addr = nir_iadd_imm(b, dst_addr, 4);
      }
      nir_pop_if(b, NULL);
   }
   radv_build_token_end(b);
}

static nir_function_impl *
lower_any_hit_for_intersection(nir_shader *any_hit)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(any_hit);

   /* Any-hit shaders need three parameters */
   assert(impl->function->num_params == 0);
   nir_parameter params[] = {
      {
         /* A pointer to a boolean value for whether or not the hit was
          * accepted.
          */
         .num_components = 1,
         .bit_size = 32,
      },
      {
         /* The hit T value */
         .num_components = 1,
         .bit_size = 32,
      },
      {
         /* The hit kind */
         .num_components = 1,
         .bit_size = 32,
      },
      {
         /* Scratch offset */
         .num_components = 1,
         .bit_size = 32,
      },
   };
   impl->function->num_params = ARRAY_SIZE(params);
   impl->function->params = ralloc_array(any_hit, nir_parameter, ARRAY_SIZE(params));
   memcpy(impl->function->params, params, sizeof(params));

   nir_builder build = nir_builder_at(nir_before_impl(impl));
   nir_builder *b = &build;

   nir_def *commit_ptr = nir_load_param(b, 0);
   nir_def *hit_t = nir_load_param(b, 1);
   nir_def *hit_kind = nir_load_param(b, 2);
   nir_def *scratch_offset = nir_load_param(b, 3);

   nir_deref_instr *commit = nir_build_deref_cast(b, commit_ptr, nir_var_function_temp, glsl_bool_type(), 0);

   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         switch (instr->type) {
         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_ignore_ray_intersection:
               b->cursor = nir_instr_remove(&intrin->instr);
               /* We put the newly emitted code inside a dummy if because it's
                * going to contain a jump instruction and we don't want to
                * deal with that mess here.  It'll get dealt with by our
                * control-flow optimization passes.
                */
               nir_store_deref(b, commit, nir_imm_false(b), 0x1);
               nir_push_if(b, nir_imm_true(b));
               nir_jump(b, nir_jump_return);
               nir_pop_if(b, NULL);
               break;

            case nir_intrinsic_terminate_ray:
               /* The "normal" handling of terminateRay works fine in
                * intersection shaders.
                */
               break;

            case nir_intrinsic_load_ray_t_max:
               nir_def_replace(&intrin->def, hit_t);
               break;

            case nir_intrinsic_load_ray_hit_kind:
               nir_def_replace(&intrin->def, hit_kind);
               break;

            /* We place all any_hit scratch variables after intersection scratch variables.
             * For that reason, we increment the scratch offset by the intersection scratch
             * size. For call_data, we have to subtract the offset again.
             *
             * Note that we don't increase the scratch size as it is already reflected via
             * the any_hit stack_size.
             */
            case nir_intrinsic_load_scratch:
               b->cursor = nir_before_instr(instr);
               nir_src_rewrite(&intrin->src[0], nir_iadd_nuw(b, scratch_offset, intrin->src[0].ssa));
               break;
            case nir_intrinsic_store_scratch:
               b->cursor = nir_before_instr(instr);
               nir_src_rewrite(&intrin->src[1], nir_iadd_nuw(b, scratch_offset, intrin->src[1].ssa));
               break;
            case nir_intrinsic_load_rt_arg_scratch_offset_amd:
               b->cursor = nir_after_instr(instr);
               nir_def *arg_offset = nir_isub(b, &intrin->def, scratch_offset);
               nir_def_rewrite_uses_after(&intrin->def, arg_offset);
               break;

            default:
               break;
            }
            break;
         }
         case nir_instr_type_jump: {
            nir_jump_instr *jump = nir_instr_as_jump(instr);
            if (jump->type == nir_jump_halt) {
               b->cursor = nir_instr_remove(instr);
               nir_jump(b, nir_jump_return);
            }
            break;
         }

         default:
            break;
         }
      }
   }

   nir_validate_shader(any_hit, "after initial any-hit lowering");

   nir_lower_returns_impl(impl);

   nir_validate_shader(any_hit, "after lowering returns");

   return impl;
}

/* Inline the any_hit shader into the intersection shader so we don't have
 * to implement yet another shader call interface here. Neither do any recursion.
 */
static void
nir_lower_intersection_shader(nir_shader *intersection, nir_shader *any_hit)
{
   void *dead_ctx = ralloc_context(intersection);

   nir_function_impl *any_hit_impl = NULL;
   struct hash_table *any_hit_var_remap = NULL;
   if (any_hit) {
      any_hit = nir_shader_clone(dead_ctx, any_hit);
      NIR_PASS(_, any_hit, nir_opt_dce);

      inline_constants(intersection, any_hit);

      any_hit_impl = lower_any_hit_for_intersection(any_hit);
      any_hit_var_remap = _mesa_pointer_hash_table_create(dead_ctx);
   }

   nir_function_impl *impl = nir_shader_get_entrypoint(intersection);

   nir_builder build = nir_builder_create(impl);
   nir_builder *b = &build;

   b->cursor = nir_before_impl(impl);

   nir_variable *commit = nir_local_variable_create(impl, glsl_bool_type(), "ray_commit");
   nir_store_var(b, commit, nir_imm_false(b), 0x1);

   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_report_ray_intersection)
            continue;

         b->cursor = nir_instr_remove(&intrin->instr);
         nir_def *hit_t = intrin->src[0].ssa;
         nir_def *hit_kind = intrin->src[1].ssa;
         nir_def *min_t = nir_load_ray_t_min(b);
         nir_def *max_t = nir_load_ray_t_max(b);

         /* bool commit_tmp = false; */
         nir_variable *commit_tmp = nir_local_variable_create(impl, glsl_bool_type(), "commit_tmp");
         nir_store_var(b, commit_tmp, nir_imm_false(b), 0x1);

         nir_push_if(b, nir_iand(b, nir_fge(b, hit_t, min_t), nir_fge(b, max_t, hit_t)));
         {
            /* Any-hit defaults to commit */
            nir_store_var(b, commit_tmp, nir_imm_true(b), 0x1);

            if (any_hit_impl != NULL) {
               nir_push_if(b, nir_inot(b, nir_load_intersection_opaque_amd(b)));
               {
                  nir_def *params[] = {
                     &nir_build_deref_var(b, commit_tmp)->def,
                     hit_t,
                     hit_kind,
                     nir_imm_int(b, intersection->scratch_size),
                  };
                  nir_inline_function_impl(b, any_hit_impl, params, any_hit_var_remap);
               }
               nir_pop_if(b, NULL);
            }

            nir_push_if(b, nir_load_var(b, commit_tmp));
            {
               nir_report_ray_intersection(b, 1, hit_t, hit_kind);
            }
            nir_pop_if(b, NULL);
         }
         nir_pop_if(b, NULL);

         nir_def *accepted = nir_load_var(b, commit_tmp);
         nir_def_rewrite_uses(&intrin->def, accepted);
      }
   }
   nir_progress(true, impl, nir_metadata_none);

   /* We did some inlining; have to re-index SSA defs */
   nir_index_ssa_defs(impl);

   /* Eliminate the casts introduced for the commit return of the any-hit shader. */
   NIR_PASS(_, intersection, nir_opt_deref);

   ralloc_free(dead_ctx);
}

/* Variables only used internally to ray traversal. This is data that describes
 * the current state of the traversal vs. what we'd give to a shader.  e.g. what
 * is the instance we're currently visiting vs. what is the instance of the
 * closest hit. */
struct rt_traversal_vars {
   nir_variable *origin;
   nir_variable *dir;
   nir_variable *inv_dir;
   nir_variable *sbt_offset_and_flags;
   nir_variable *instance_addr;
   nir_variable *hit;
   nir_variable *bvh_base;
   nir_variable *stack;
   nir_variable *top_stack;
   nir_variable *stack_low_watermark;
   nir_variable *current_node;
   nir_variable *previous_node;
   nir_variable *instance_top_node;
   nir_variable *instance_bottom_node;
};

static struct rt_traversal_vars
init_traversal_vars(nir_builder *b)
{
   const struct glsl_type *vec3_type = glsl_vector_type(GLSL_TYPE_FLOAT, 3);
   struct rt_traversal_vars ret;

   ret.origin = nir_variable_create(b->shader, nir_var_shader_temp, vec3_type, "traversal_origin");
   ret.dir = nir_variable_create(b->shader, nir_var_shader_temp, vec3_type, "traversal_dir");
   ret.inv_dir = nir_variable_create(b->shader, nir_var_shader_temp, vec3_type, "traversal_inv_dir");
   ret.sbt_offset_and_flags =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "traversal_sbt_offset_and_flags");
   ret.instance_addr = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint64_t_type(), "instance_addr");
   ret.hit = nir_variable_create(b->shader, nir_var_shader_temp, glsl_bool_type(), "traversal_hit");
   ret.bvh_base = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint64_t_type(), "traversal_bvh_base");
   ret.stack = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "traversal_stack_ptr");
   ret.top_stack = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "traversal_top_stack_ptr");
   ret.stack_low_watermark =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "traversal_stack_low_watermark");
   ret.current_node = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "current_node;");
   ret.previous_node = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "previous_node");
   ret.instance_top_node = nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "instance_top_node");
   ret.instance_bottom_node =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "instance_bottom_node");
   return ret;
}

struct traversal_data {
   struct radv_device *device;
   struct rt_variables *vars;
   struct rt_traversal_vars *trav_vars;
   nir_variable *barycentrics;

   struct radv_ray_tracing_pipeline *pipeline;
};

static void
radv_ray_tracing_group_ahit_info(struct radv_ray_tracing_group *group, uint32_t *shader_index, uint32_t *handle_index,
                                 struct radv_rt_case_data *data)
{
   if (group->type == VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR) {
      *shader_index = group->any_hit_shader;
      *handle_index = group->handle.any_hit_index;
   }
}

static void
radv_build_ahit_case(nir_builder *b, nir_def *sbt_idx, struct radv_ray_tracing_group *group,
                     struct radv_rt_case_data *data)
{
   nir_shader *nir_stage =
      radv_pipeline_cache_handle_to_nir(data->device, data->pipeline->stages[group->any_hit_shader].nir);
   assert(nir_stage);

   radv_nir_lower_rt_io(nir_stage, data->vars->monolithic, data->vars->payload_offset);

   insert_rt_case(b, nir_stage, data->vars, sbt_idx, group->handle.any_hit_index);
   ralloc_free(nir_stage);
}

static void
radv_ray_tracing_group_isec_info(struct radv_ray_tracing_group *group, uint32_t *shader_index, uint32_t *handle_index,
                                 struct radv_rt_case_data *data)
{
   if (group->type == VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR) {
      *shader_index = group->intersection_shader;
      *handle_index = group->handle.intersection_index;
   }
}

static void
radv_build_isec_case(nir_builder *b, nir_def *sbt_idx, struct radv_ray_tracing_group *group,
                     struct radv_rt_case_data *data)
{
   nir_shader *nir_stage =
      radv_pipeline_cache_handle_to_nir(data->device, data->pipeline->stages[group->intersection_shader].nir);
   assert(nir_stage);

   radv_nir_lower_rt_io(nir_stage, data->vars->monolithic, data->vars->payload_offset);

   nir_shader *any_hit_stage = NULL;
   if (group->any_hit_shader != VK_SHADER_UNUSED_KHR) {
      any_hit_stage =
         radv_pipeline_cache_handle_to_nir(data->device, data->pipeline->stages[group->any_hit_shader].nir);
      assert(any_hit_stage);

      radv_nir_lower_rt_io(any_hit_stage, data->vars->monolithic, data->vars->payload_offset);

      /* reserve stack size for any_hit before it is inlined */
      data->pipeline->stages[group->any_hit_shader].stack_size = any_hit_stage->scratch_size;

      nir_lower_intersection_shader(nir_stage, any_hit_stage);
      ralloc_free(any_hit_stage);
   }

   insert_rt_case(b, nir_stage, data->vars, sbt_idx, group->handle.intersection_index);
   ralloc_free(nir_stage);
}

static void
radv_ray_tracing_group_chit_info(struct radv_ray_tracing_group *group, uint32_t *shader_index, uint32_t *handle_index,
                                 struct radv_rt_case_data *data)
{
   if (group->type != VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR) {
      *shader_index = group->recursive_shader;
      *handle_index = group->handle.closest_hit_index;
   }
}

static void
radv_ray_tracing_group_miss_info(struct radv_ray_tracing_group *group, uint32_t *shader_index, uint32_t *handle_index,
                                 struct radv_rt_case_data *data)
{
   if (group->type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR) {
      if (data->pipeline->stages[group->recursive_shader].stage != MESA_SHADER_MISS)
         return;

      *shader_index = group->recursive_shader;
      *handle_index = group->handle.general_index;
   }
}

static void
radv_build_recursive_case(nir_builder *b, nir_def *sbt_idx, struct radv_ray_tracing_group *group,
                          struct radv_rt_case_data *data)
{
   nir_shader *nir_stage =
      radv_pipeline_cache_handle_to_nir(data->device, data->pipeline->stages[group->recursive_shader].nir);
   assert(nir_stage);

   radv_nir_lower_rt_io(nir_stage, data->vars->monolithic, data->vars->payload_offset);

   insert_rt_case(b, nir_stage, data->vars, sbt_idx, group->handle.general_index);
   ralloc_free(nir_stage);
}

static void
handle_candidate_triangle(nir_builder *b, struct radv_triangle_intersection *intersection,
                          const struct radv_ray_traversal_args *args, const struct radv_ray_flags *ray_flags)
{
   struct traversal_data *data = args->data;

   nir_def *geometry_id = nir_iand_imm(b, intersection->base.geometry_id_and_flags, 0xfffffff);
   nir_def *sbt_idx =
      nir_iadd(b,
               nir_iadd(b, nir_load_var(b, data->vars->sbt_offset),
                        nir_iand_imm(b, nir_load_var(b, data->trav_vars->sbt_offset_and_flags), 0xffffff)),
               nir_imul(b, nir_load_var(b, data->vars->sbt_stride), geometry_id));

   nir_def *hit_kind = nir_bcsel(b, intersection->frontface, nir_imm_int(b, 0xFE), nir_imm_int(b, 0xFF));

   nir_def *prev_barycentrics = nir_load_var(b, data->barycentrics);
   nir_store_var(b, data->barycentrics, intersection->barycentrics, 0x3);

   nir_store_var(b, data->vars->ahit_accept, nir_imm_true(b), 0x1);
   nir_store_var(b, data->vars->ahit_terminate, nir_imm_false(b), 0x1);

   nir_push_if(b, nir_inot(b, intersection->base.opaque));
   {
      struct rt_variables inner_vars = create_inner_vars(b, data->vars);

      nir_store_var(b, inner_vars.primitive_addr, intersection->base.node_addr, 1);
      nir_store_var(b, inner_vars.primitive_id, intersection->base.primitive_id, 1);
      nir_store_var(b, inner_vars.geometry_id_and_flags, intersection->base.geometry_id_and_flags, 1);
      nir_store_var(b, inner_vars.tmax, intersection->t, 0x1);
      nir_store_var(b, inner_vars.instance_addr, nir_load_var(b, data->trav_vars->instance_addr), 0x1);
      nir_store_var(b, inner_vars.hit_kind, hit_kind, 0x1);

      load_sbt_entry(b, &inner_vars, sbt_idx, SBT_HIT, SBT_ANY_HIT_IDX);

      struct radv_rt_case_data case_data = {
         .device = data->device,
         .pipeline = data->pipeline,
         .vars = &inner_vars,
      };

      if (data->vars->ahit_isec_count)
         nir_store_var(b, data->vars->ahit_isec_count, nir_iadd_imm(b, nir_load_var(b, data->vars->ahit_isec_count), 1),
                       0x1);

      radv_visit_inlined_shaders(
         b, nir_load_var(b, inner_vars.idx),
         !(data->vars->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_NO_NULL_ANY_HIT_SHADERS_BIT_KHR), &case_data,
         radv_ray_tracing_group_ahit_info, radv_build_ahit_case);

      nir_push_if(b, nir_inot(b, nir_load_var(b, data->vars->ahit_accept)));
      {
         nir_store_var(b, data->barycentrics, prev_barycentrics, 0x3);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);

   nir_push_if(b, nir_load_var(b, data->vars->ahit_accept));
   {
      nir_store_var(b, data->vars->primitive_addr, intersection->base.node_addr, 1);
      nir_store_var(b, data->vars->primitive_id, intersection->base.primitive_id, 1);
      nir_store_var(b, data->vars->geometry_id_and_flags, intersection->base.geometry_id_and_flags, 1);
      nir_store_var(b, data->vars->tmax, intersection->t, 0x1);
      nir_store_var(b, data->vars->instance_addr, nir_load_var(b, data->trav_vars->instance_addr), 0x1);
      nir_store_var(b, data->vars->hit_kind, hit_kind, 0x1);

      nir_store_var(b, data->vars->idx, sbt_idx, 1);
      nir_store_var(b, data->trav_vars->hit, nir_imm_true(b), 1);

      nir_def *ray_terminated = nir_load_var(b, data->vars->ahit_terminate);
      nir_break_if(b, nir_ior(b, ray_flags->terminate_on_first_hit, ray_terminated));
   }
   nir_pop_if(b, NULL);
}

static void
handle_candidate_aabb(nir_builder *b, struct radv_leaf_intersection *intersection,
                      const struct radv_ray_traversal_args *args)
{
   struct traversal_data *data = args->data;

   nir_def *geometry_id = nir_iand_imm(b, intersection->geometry_id_and_flags, 0xfffffff);
   nir_def *sbt_idx =
      nir_iadd(b,
               nir_iadd(b, nir_load_var(b, data->vars->sbt_offset),
                        nir_iand_imm(b, nir_load_var(b, data->trav_vars->sbt_offset_and_flags), 0xffffff)),
               nir_imul(b, nir_load_var(b, data->vars->sbt_stride), geometry_id));

   struct rt_variables inner_vars = create_inner_vars(b, data->vars);

   /* For AABBs the intersection shader writes the hit kind, and only does it if it is the
    * next closest hit candidate. */
   inner_vars.hit_kind = data->vars->hit_kind;

   nir_store_var(b, inner_vars.primitive_addr, intersection->node_addr, 1);
   nir_store_var(b, inner_vars.primitive_id, intersection->primitive_id, 1);
   nir_store_var(b, inner_vars.geometry_id_and_flags, intersection->geometry_id_and_flags, 1);
   nir_store_var(b, inner_vars.tmax, nir_load_var(b, data->vars->tmax), 0x1);
   nir_store_var(b, inner_vars.instance_addr, nir_load_var(b, data->trav_vars->instance_addr), 0x1);
   nir_store_var(b, inner_vars.opaque, intersection->opaque, 1);

   load_sbt_entry(b, &inner_vars, sbt_idx, SBT_HIT, SBT_INTERSECTION_IDX);

   nir_store_var(b, data->vars->ahit_accept, nir_imm_false(b), 0x1);
   nir_store_var(b, data->vars->ahit_terminate, nir_imm_false(b), 0x1);
   nir_store_var(b, data->vars->terminated, nir_imm_false(b), 0x1);

   if (data->vars->ahit_isec_count)
      nir_store_var(b, data->vars->ahit_isec_count,
                    nir_iadd_imm(b, nir_load_var(b, data->vars->ahit_isec_count), 1 << 16), 0x1);

   struct radv_rt_case_data case_data = {
      .device = data->device,
      .pipeline = data->pipeline,
      .vars = &inner_vars,
   };

   radv_visit_inlined_shaders(
      b, nir_load_var(b, inner_vars.idx),
      !(data->vars->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_NO_NULL_INTERSECTION_SHADERS_BIT_KHR), &case_data,
      radv_ray_tracing_group_isec_info, radv_build_isec_case);

   nir_push_if(b, nir_load_var(b, data->vars->ahit_accept));
   {
      nir_store_var(b, data->vars->primitive_addr, intersection->node_addr, 1);
      nir_store_var(b, data->vars->primitive_id, intersection->primitive_id, 1);
      nir_store_var(b, data->vars->geometry_id_and_flags, intersection->geometry_id_and_flags, 1);
      nir_store_var(b, data->vars->tmax, nir_load_var(b, inner_vars.tmax), 0x1);
      nir_store_var(b, data->vars->instance_addr, nir_load_var(b, data->trav_vars->instance_addr), 0x1);

      nir_store_var(b, data->vars->idx, sbt_idx, 1);
      nir_store_var(b, data->trav_vars->hit, nir_imm_true(b), 1);

      nir_break_if(b, nir_load_var(b, data->vars->terminated));
   }
   nir_pop_if(b, NULL);
}

static void
store_stack_entry(nir_builder *b, nir_def *index, nir_def *value, const struct radv_ray_traversal_args *args)
{
   nir_store_shared(b, value, index, .base = 0, .align_mul = 4);
}

static nir_def *
load_stack_entry(nir_builder *b, nir_def *index, const struct radv_ray_traversal_args *args)
{
   return nir_load_shared(b, 1, 32, index, .base = 0, .align_mul = 4);
}

static void
radv_build_traversal(struct radv_device *device, struct radv_ray_tracing_pipeline *pipeline,
                     const VkRayTracingPipelineCreateInfoKHR *pCreateInfo, bool monolithic, nir_builder *b,
                     struct rt_variables *vars, bool ignore_cull_mask, struct radv_ray_tracing_stage_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   nir_variable *barycentrics =
      nir_variable_create(b->shader, nir_var_ray_hit_attrib, glsl_vector_type(GLSL_TYPE_FLOAT, 2), "barycentrics");
   barycentrics->data.driver_location = 0;

   struct rt_traversal_vars trav_vars = init_traversal_vars(b);

   nir_def *cull_mask_and_flags = nir_load_var(b, vars->cull_mask_and_flags);

   nir_store_var(b, trav_vars.hit, nir_imm_false(b), 1);

   nir_def *accel_struct = nir_load_var(b, vars->accel_struct);

   nir_def *zero = nir_imm_int(b, 0);
   nir_def *bvh_offset;
   nir_push_if(b, nir_ine_imm(b, accel_struct, 0));
   {
      bvh_offset = nir_build_load_global(
         b, 1, 32, nir_iadd_imm(b, accel_struct, offsetof(struct radv_accel_struct_header, bvh_offset)),
         .access = ACCESS_NON_WRITEABLE);
      nir_store_var(b, trav_vars.current_node, nir_imm_int(b, RADV_BVH_ROOT_NODE), 0x1);
   }
   nir_push_else(b, NULL);
   {
      nir_store_var(b, trav_vars.current_node,
                    nir_imm_int(b, radv_use_bvh_stack_rtn(pdev) ? RADV_BVH_STACK_TERMINAL_NODE : RADV_BVH_INVALID_NODE),
                    0x1);
   }
   nir_pop_if(b, NULL);
   bvh_offset = nir_if_phi(b, bvh_offset, zero);

   nir_def *root_bvh_base = nir_iadd(b, accel_struct, nir_u2u64(b, bvh_offset));
   root_bvh_base = build_addr_to_node(device, b, root_bvh_base, cull_mask_and_flags);

   nir_store_var(b, trav_vars.bvh_base, root_bvh_base, 1);

   nir_def *stack_idx = nir_load_local_invocation_index(b);
   uint32_t stack_stride;

   if (radv_use_bvh_stack_rtn(pdev)) {
      stack_idx = radv_build_bvh_stack_rtn_addr(b, pdev, pdev->rt_wave_size, 0, MAX_STACK_ENTRY_COUNT);
      stack_stride = 1;
   } else {
      stack_idx = nir_imul_imm(b, stack_idx, sizeof(uint32_t));
      stack_stride = pdev->rt_wave_size * sizeof(uint32_t);
   }

   nir_def *vec3ones = nir_imm_vec3(b, 1.0, 1.0, 1.0);

   nir_store_var(b, trav_vars.origin, nir_load_var(b, vars->origin), 7);
   nir_store_var(b, trav_vars.dir, nir_load_var(b, vars->direction), 7);
   nir_store_var(b, trav_vars.inv_dir, nir_fdiv(b, vec3ones, nir_load_var(b, trav_vars.dir)), 7);
   nir_store_var(b, trav_vars.sbt_offset_and_flags, nir_imm_int(b, 0), 1);
   nir_store_var(b, trav_vars.instance_addr, nir_imm_int64(b, 0), 1);

   nir_store_var(b, trav_vars.stack, stack_idx, 1);
   nir_store_var(b, trav_vars.stack_low_watermark, nir_load_var(b, trav_vars.stack), 1);
   nir_store_var(b, trav_vars.previous_node, nir_imm_int(b, RADV_BVH_INVALID_NODE), 0x1);
   nir_store_var(b, trav_vars.instance_top_node, nir_imm_int(b, RADV_BVH_INVALID_NODE), 0x1);
   nir_store_var(b, trav_vars.instance_bottom_node, nir_imm_int(b, RADV_BVH_NO_INSTANCE_ROOT), 0x1);

   nir_store_var(b, trav_vars.top_stack, nir_imm_int(b, -1), 1);

   struct radv_ray_traversal_vars trav_vars_args = {
      .tmax = nir_build_deref_var(b, vars->tmax),
      .origin = nir_build_deref_var(b, trav_vars.origin),
      .dir = nir_build_deref_var(b, trav_vars.dir),
      .inv_dir = nir_build_deref_var(b, trav_vars.inv_dir),
      .bvh_base = nir_build_deref_var(b, trav_vars.bvh_base),
      .stack = nir_build_deref_var(b, trav_vars.stack),
      .top_stack = nir_build_deref_var(b, trav_vars.top_stack),
      .stack_low_watermark = nir_build_deref_var(b, trav_vars.stack_low_watermark),
      .current_node = nir_build_deref_var(b, trav_vars.current_node),
      .previous_node = nir_build_deref_var(b, trav_vars.previous_node),
      .instance_top_node = nir_build_deref_var(b, trav_vars.instance_top_node),
      .instance_bottom_node = nir_build_deref_var(b, trav_vars.instance_bottom_node),
      .instance_addr = nir_build_deref_var(b, trav_vars.instance_addr),
      .sbt_offset_and_flags = nir_build_deref_var(b, trav_vars.sbt_offset_and_flags),
   };

   nir_variable *iteration_instance_count = NULL;
   if (vars->device->rra_trace.ray_history_addr) {
      iteration_instance_count =
         nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "iteration_instance_count");
      nir_store_var(b, iteration_instance_count, nir_imm_int(b, 0), 0x1);
      trav_vars_args.iteration_instance_count = nir_build_deref_var(b, iteration_instance_count);

      nir_store_var(b, vars->ahit_isec_count, nir_imm_int(b, 0), 0x1);
   }

   struct traversal_data data = {
      .device = device,
      .vars = vars,
      .trav_vars = &trav_vars,
      .barycentrics = barycentrics,
      .pipeline = pipeline,
   };

   struct radv_ray_traversal_args args = {
      .root_bvh_base = root_bvh_base,
      .flags = cull_mask_and_flags,
      .cull_mask = cull_mask_and_flags,
      .origin = nir_load_var(b, vars->origin),
      .tmin = nir_load_var(b, vars->tmin),
      .dir = nir_load_var(b, vars->direction),
      .vars = trav_vars_args,
      .stack_stride = stack_stride,
      .stack_entries = MAX_STACK_ENTRY_COUNT,
      .stack_base = 0,
      .ignore_cull_mask = ignore_cull_mask,
      .set_flags = info ? info->set_flags : 0,
      .unset_flags = info ? info->unset_flags : 0,
      .stack_store_cb = store_stack_entry,
      .stack_load_cb = load_stack_entry,
      .aabb_cb = (pipeline->base.base.create_flags & VK_PIPELINE_CREATE_2_RAY_TRACING_SKIP_AABBS_BIT_KHR)
                    ? NULL
                    : handle_candidate_aabb,
      .triangle_cb = (pipeline->base.base.create_flags & VK_PIPELINE_CREATE_2_RAY_TRACING_SKIP_TRIANGLES_BIT_KHR)
                        ? NULL
                        : handle_candidate_triangle,
      .use_bvh_stack_rtn = radv_use_bvh_stack_rtn(pdev),
      .data = &data,
   };

   nir_def *original_tmax = nir_load_var(b, vars->tmax);

   if (radv_use_bvh8(pdev))
      radv_build_ray_traversal_gfx12(device, b, &args);
   else
      radv_build_ray_traversal(device, b, &args);

   if (vars->device->rra_trace.ray_history_addr)
      radv_build_end_trace_token(b, vars, original_tmax, nir_load_var(b, trav_vars.hit),
                                 nir_load_var(b, iteration_instance_count));

   nir_progress(true, nir_shader_get_entrypoint(b->shader), nir_metadata_none);
   radv_nir_lower_hit_attrib_derefs(b->shader);

   /* Register storage for hit attributes */
   nir_variable *hit_attribs[RADV_MAX_HIT_ATTRIB_DWORDS];

   if (!monolithic) {
      for (uint32_t i = 0; i < ARRAY_SIZE(hit_attribs); i++)
         hit_attribs[i] =
            nir_local_variable_create(nir_shader_get_entrypoint(b->shader), glsl_uint_type(), "ahit_attrib");

      lower_hit_attribs(b->shader, hit_attribs, pdev->rt_wave_size);
   }

   /* Initialize follow-up shader. */
   nir_push_if(b, nir_load_var(b, trav_vars.hit));
   {
      if (monolithic) {
         load_sbt_entry(b, vars, nir_load_var(b, vars->idx), SBT_HIT, SBT_CLOSEST_HIT_IDX);

         nir_def *should_return =
            nir_test_mask(b, nir_load_var(b, vars->cull_mask_and_flags), SpvRayFlagsSkipClosestHitShaderKHRMask);

         /* should_return is set if we had a hit but we won't be calling the closest hit
          * shader and hence need to return immediately to the calling shader. */
         nir_push_if(b, nir_inot(b, should_return));

         struct radv_rt_case_data case_data = {
            .device = device,
            .pipeline = pipeline,
            .vars = vars,
         };

         radv_visit_inlined_shaders(
            b, nir_load_var(b, vars->idx),
            !(vars->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_NO_NULL_CLOSEST_HIT_SHADERS_BIT_KHR), &case_data,
            radv_ray_tracing_group_chit_info, radv_build_recursive_case);

         nir_pop_if(b, NULL);
      } else {
         for (int i = 0; i < ARRAY_SIZE(hit_attribs); ++i)
            nir_store_hit_attrib_amd(b, nir_load_var(b, hit_attribs[i]), .base = i);

         nir_def *primitive_addr;
         if (info->has_position_fetch)
            primitive_addr = nir_load_var(b, vars->primitive_addr);
         else
            primitive_addr = nir_undef(b, 1, 64);

         nir_execute_closest_hit_amd(b, nir_load_var(b, vars->idx), nir_load_var(b, vars->tmax), primitive_addr,
                                     nir_load_var(b, vars->primitive_id), nir_load_var(b, vars->instance_addr),
                                     nir_load_var(b, vars->geometry_id_and_flags), nir_load_var(b, vars->hit_kind));
      }
   }
   nir_push_else(b, NULL);
   {
      if (monolithic) {
         load_sbt_entry(b, vars, nir_load_var(b, vars->miss_index), SBT_MISS, SBT_GENERAL_IDX);

         struct radv_rt_case_data case_data = {
            .device = device,
            .pipeline = pipeline,
            .vars = vars,
         };

         radv_visit_inlined_shaders(b, nir_load_var(b, vars->idx),
                                    !(vars->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_NO_NULL_MISS_SHADERS_BIT_KHR),
                                    &case_data, radv_ray_tracing_group_miss_info, radv_build_recursive_case);
      } else {
         /* Only load the miss shader if we actually miss. It is valid to not specify an SBT pointer
          * for miss shaders if none of the rays miss. */
         nir_execute_miss_amd(b, nir_load_var(b, vars->tmax));
      }
   }
   nir_pop_if(b, NULL);
}

nir_shader *
radv_build_traversal_shader(struct radv_device *device, struct radv_ray_tracing_pipeline *pipeline,
                            const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                            struct radv_ray_tracing_stage_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const VkPipelineCreateFlagBits2 create_flags = vk_rt_pipeline_create_flags(pCreateInfo);

   /* Create the traversal shader as an intersection shader to prevent validation failures due to
    * invalid variable modes.*/
   nir_builder b = radv_meta_nir_init_shader(device, MESA_SHADER_INTERSECTION, "rt_traversal");
   b.shader->info.internal = false;
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = pdev->rt_wave_size == 64 ? 8 : 4;
   b.shader->info.shared_size = pdev->rt_wave_size * MAX_STACK_ENTRY_COUNT * sizeof(uint32_t);
   struct rt_variables vars = create_rt_variables(b.shader, device, create_flags, false);

   if (info->tmin.state == RADV_RT_CONST_ARG_STATE_VALID)
      nir_store_var(&b, vars.tmin, nir_imm_int(&b, info->tmin.value), 0x1);
   else
      nir_store_var(&b, vars.tmin, nir_load_ray_t_min(&b), 0x1);

   if (info->tmax.state == RADV_RT_CONST_ARG_STATE_VALID)
      nir_store_var(&b, vars.tmax, nir_imm_int(&b, info->tmax.value), 0x1);
   else
      nir_store_var(&b, vars.tmax, nir_load_ray_t_max(&b), 0x1);

   if (info->sbt_offset.state == RADV_RT_CONST_ARG_STATE_VALID)
      nir_store_var(&b, vars.sbt_offset, nir_imm_int(&b, info->sbt_offset.value), 0x1);
   else
      nir_store_var(&b, vars.sbt_offset, nir_load_sbt_offset_amd(&b), 0x1);

   if (info->sbt_stride.state == RADV_RT_CONST_ARG_STATE_VALID)
      nir_store_var(&b, vars.sbt_stride, nir_imm_int(&b, info->sbt_stride.value), 0x1);
   else
      nir_store_var(&b, vars.sbt_stride, nir_load_sbt_stride_amd(&b), 0x1);

   /* initialize trace_ray arguments */
   nir_store_var(&b, vars.accel_struct, nir_load_accel_struct_amd(&b), 1);
   nir_store_var(&b, vars.cull_mask_and_flags, nir_load_cull_mask_and_flags_amd(&b), 0x1);
   nir_store_var(&b, vars.origin, nir_load_ray_world_origin(&b), 0x7);
   nir_store_var(&b, vars.direction, nir_load_ray_world_direction(&b), 0x7);
   nir_store_var(&b, vars.arg, nir_load_rt_arg_scratch_offset_amd(&b), 0x1);
   nir_store_var(&b, vars.stack_ptr, nir_imm_int(&b, 0), 0x1);

   radv_build_traversal(device, pipeline, pCreateInfo, false, &b, &vars, false, info);

   /* Deal with all the inline functions. */
   nir_index_ssa_defs(nir_shader_get_entrypoint(b.shader));
   nir_progress(true, nir_shader_get_entrypoint(b.shader), nir_metadata_none);

   /* Lower and cleanup variables */
   NIR_PASS(_, b.shader, nir_lower_global_vars_to_local);
   NIR_PASS(_, b.shader, nir_lower_vars_to_ssa);

   return b.shader;
}

struct lower_rt_instruction_monolithic_state {
   struct radv_device *device;
   struct radv_ray_tracing_pipeline *pipeline;
   const VkRayTracingPipelineCreateInfoKHR *pCreateInfo;

   struct rt_variables *vars;
};

static bool
lower_rt_instruction_monolithic(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   b->cursor = nir_after_instr(instr);

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   struct lower_rt_instruction_monolithic_state *state = data;
   const struct radv_physical_device *pdev = radv_device_physical(state->device);
   struct rt_variables *vars = state->vars;

   switch (intr->intrinsic) {
   case nir_intrinsic_execute_callable:
      /* It's allowed to place OpExecuteCallableKHR in a SPIR-V, even if the RT pipeline doesn't contain
       * any callable shaders. However, it's impossible to execute the instruction in a valid way, so just remove any
       * nir_intrinsic_execute_callable we encounter.
       */
      nir_instr_remove(instr);
      return true;
   case nir_intrinsic_trace_ray: {
      vars->payload_offset = nir_src_as_uint(intr->src[10]);

      nir_src cull_mask = intr->src[2];
      bool ignore_cull_mask = nir_src_is_const(cull_mask) && (nir_src_as_uint(cull_mask) & 0xFF) == 0xFF;

      /* Per the SPIR-V extension spec we have to ignore some bits for some arguments. */
      nir_store_var(b, vars->accel_struct, intr->src[0].ssa, 0x1);
      nir_store_var(b, vars->cull_mask_and_flags, nir_ior(b, nir_ishl_imm(b, cull_mask.ssa, 24), intr->src[1].ssa),
                    0x1);
      nir_store_var(b, vars->sbt_offset, nir_iand_imm(b, intr->src[3].ssa, 0xf), 0x1);
      nir_store_var(b, vars->sbt_stride, nir_iand_imm(b, intr->src[4].ssa, 0xf), 0x1);
      nir_store_var(b, vars->miss_index, nir_iand_imm(b, intr->src[5].ssa, 0xffff), 0x1);
      nir_store_var(b, vars->origin, intr->src[6].ssa, 0x7);
      nir_store_var(b, vars->tmin, intr->src[7].ssa, 0x1);
      nir_store_var(b, vars->direction, intr->src[8].ssa, 0x7);
      nir_store_var(b, vars->tmax, intr->src[9].ssa, 0x1);

      nir_def *stack_ptr = nir_load_var(b, vars->stack_ptr);
      nir_store_var(b, vars->stack_ptr, nir_iadd_imm(b, stack_ptr, b->shader->scratch_size), 0x1);

      radv_build_traversal(state->device, state->pipeline, state->pCreateInfo, true, b, vars, ignore_cull_mask, NULL);
      b->shader->info.shared_size =
         MAX2(b->shader->info.shared_size, pdev->rt_wave_size * MAX_STACK_ENTRY_COUNT * sizeof(uint32_t));

      nir_store_var(b, vars->stack_ptr, stack_ptr, 0x1);

      nir_instr_remove(instr);
      return true;
   }
   case nir_intrinsic_rt_resume:
      UNREACHABLE("nir_intrinsic_rt_resume");
   case nir_intrinsic_rt_return_amd:
      UNREACHABLE("nir_intrinsic_rt_return_amd");
   case nir_intrinsic_execute_closest_hit_amd:
      UNREACHABLE("nir_intrinsic_execute_closest_hit_amd");
   case nir_intrinsic_execute_miss_amd:
      UNREACHABLE("nir_intrinsic_execute_miss_amd");
   default:
      return false;
   }
}

static bool
radv_count_hit_attrib_slots(nir_builder *b, nir_intrinsic_instr *instr, void *data)
{
   uint32_t *count = data;
   if (instr->intrinsic == nir_intrinsic_load_hit_attrib_amd || instr->intrinsic == nir_intrinsic_store_hit_attrib_amd)
      *count = MAX2(*count, nir_intrinsic_base(instr) + 1);

   return false;
}

static void
lower_rt_instructions_monolithic(nir_shader *shader, struct radv_device *device,
                                 struct radv_ray_tracing_pipeline *pipeline,
                                 const VkRayTracingPipelineCreateInfoKHR *pCreateInfo, struct rt_variables *vars)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   struct lower_rt_instruction_monolithic_state state = {
      .device = device,
      .pipeline = pipeline,
      .pCreateInfo = pCreateInfo,
      .vars = vars,
   };

   nir_shader_instructions_pass(shader, lower_rt_instruction_monolithic, nir_metadata_none, &state);
   nir_index_ssa_defs(impl);

   uint32_t hit_attrib_count = 0;
   nir_shader_intrinsics_pass(shader, radv_count_hit_attrib_slots, nir_metadata_all, &hit_attrib_count);

   /* Register storage for hit attributes */
   STACK_ARRAY(nir_variable *, hit_attribs, hit_attrib_count);
   for (uint32_t i = 0; i < hit_attrib_count; i++)
      hit_attribs[i] = nir_local_variable_create(impl, glsl_uint_type(), "ahit_attrib");

   lower_hit_attribs(shader, hit_attribs, 0);
}

/** Select the next shader based on priorities:
 *
 * Detect the priority of the shader stage by the lowest bits in the address (low to high):
 *  - Raygen              - idx 0
 *  - Traversal           - idx 1
 *  - Closest Hit / Miss  - idx 2
 *  - Callable            - idx 3
 *
 *
 * This gives us the following priorities:
 * Raygen       :  Callable  >               >  Traversal  >  Raygen
 * Traversal    :            >  Chit / Miss  >             >  Raygen
 * CHit / Miss  :  Callable  >  Chit / Miss  >  Traversal  >  Raygen
 * Callable     :  Callable  >  Chit / Miss  >             >  Raygen
 */
static nir_def *
select_next_shader(nir_builder *b, nir_def *shader_addr, unsigned wave_size)
{
   gl_shader_stage stage = b->shader->info.stage;
   nir_def *prio = nir_iand_imm(b, shader_addr, radv_rt_priority_mask);
   nir_def *ballot = nir_ballot(b, 1, wave_size, nir_imm_bool(b, true));
   nir_def *ballot_traversal = nir_ballot(b, 1, wave_size, nir_ieq_imm(b, prio, radv_rt_priority_traversal));
   nir_def *ballot_hit_miss = nir_ballot(b, 1, wave_size, nir_ieq_imm(b, prio, radv_rt_priority_hit_miss));
   nir_def *ballot_callable = nir_ballot(b, 1, wave_size, nir_ieq_imm(b, prio, radv_rt_priority_callable));

   if (stage != MESA_SHADER_CALLABLE && stage != MESA_SHADER_INTERSECTION)
      ballot = nir_bcsel(b, nir_ine_imm(b, ballot_traversal, 0), ballot_traversal, ballot);
   if (stage != MESA_SHADER_RAYGEN)
      ballot = nir_bcsel(b, nir_ine_imm(b, ballot_hit_miss, 0), ballot_hit_miss, ballot);
   if (stage != MESA_SHADER_INTERSECTION)
      ballot = nir_bcsel(b, nir_ine_imm(b, ballot_callable, 0), ballot_callable, ballot);

   nir_def *lsb = nir_find_lsb(b, ballot);
   nir_def *next = nir_read_invocation(b, shader_addr, lsb);
   return nir_iand_imm(b, next, ~radv_rt_priority_mask);
}

static void
radv_store_arg(nir_builder *b, const struct radv_shader_args *args, const struct radv_ray_tracing_stage_info *info,
               struct ac_arg arg, nir_def *value)
{
   /* Do not pass unused data to the next stage. */
   if (!info || !BITSET_TEST(info->unused_args, arg.arg_index))
      ac_nir_store_arg(b, &args->ac, arg, value);
}

void
radv_nir_lower_rt_abi(nir_shader *shader, const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                      const struct radv_shader_args *args, const struct radv_shader_info *info, uint32_t *stack_size,
                      bool resume_shader, struct radv_device *device, struct radv_ray_tracing_pipeline *pipeline,
                      bool monolithic, bool has_position_fetch,
                      const struct radv_ray_tracing_stage_info *traversal_info)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   const VkPipelineCreateFlagBits2 create_flags = vk_rt_pipeline_create_flags(pCreateInfo);

   struct rt_variables vars = create_rt_variables(shader, device, create_flags, monolithic);

   if (monolithic)
      lower_rt_instructions_monolithic(shader, device, pipeline, pCreateInfo, &vars);

   struct radv_rt_shader_info rt_info = {0};

   lower_rt_instructions(shader, &vars, true, &rt_info);

   if (stack_size) {
      vars.stack_size = MAX2(vars.stack_size, shader->scratch_size);
      *stack_size = MAX2(*stack_size, vars.stack_size);
   }
   shader->scratch_size = 0;

   NIR_PASS(_, shader, nir_lower_returns);

   nir_cf_list list;
   nir_cf_extract(&list, nir_before_impl(impl), nir_after_impl(impl));

   /* initialize variables */
   nir_builder b = nir_builder_at(nir_before_impl(impl));

   nir_def *descriptor_sets = ac_nir_load_arg(&b, &args->ac, args->descriptor_sets[0]);
   nir_def *push_constants = ac_nir_load_arg(&b, &args->ac, args->ac.push_constants);
   nir_def *sbt_descriptors = ac_nir_load_arg(&b, &args->ac, args->ac.rt.sbt_descriptors);

   nir_def *launch_sizes[3];
   for (uint32_t i = 0; i < ARRAY_SIZE(launch_sizes); i++) {
      launch_sizes[i] = ac_nir_load_arg(&b, &args->ac, args->ac.rt.launch_sizes[i]);
      nir_store_var(&b, vars.launch_sizes[i], launch_sizes[i], 1);
   }

   nir_def *scratch_offset = NULL;
   if (args->ac.scratch_offset.used)
      scratch_offset = ac_nir_load_arg(&b, &args->ac, args->ac.scratch_offset);
   nir_def *ring_offsets = NULL;
   if (args->ac.ring_offsets.used)
      ring_offsets = ac_nir_load_arg(&b, &args->ac, args->ac.ring_offsets);

   nir_def *launch_ids[3];
   for (uint32_t i = 0; i < ARRAY_SIZE(launch_ids); i++) {
      launch_ids[i] = ac_nir_load_arg(&b, &args->ac, args->ac.rt.launch_ids[i]);
      nir_store_var(&b, vars.launch_ids[i], launch_ids[i], 1);
   }

   nir_def *traversal_addr = ac_nir_load_arg(&b, &args->ac, args->ac.rt.traversal_shader_addr);
   nir_store_var(&b, vars.traversal_addr, nir_pack_64_2x32(&b, traversal_addr), 1);

   nir_def *shader_addr = ac_nir_load_arg(&b, &args->ac, args->ac.rt.shader_addr);
   shader_addr = nir_pack_64_2x32(&b, shader_addr);
   nir_store_var(&b, vars.shader_addr, shader_addr, 1);

   nir_store_var(&b, vars.stack_ptr, ac_nir_load_arg(&b, &args->ac, args->ac.rt.dynamic_callable_stack_base), 1);
   nir_def *record_ptr = ac_nir_load_arg(&b, &args->ac, args->ac.rt.shader_record);
   nir_store_var(&b, vars.shader_record_ptr, nir_pack_64_2x32(&b, record_ptr), 1);
   nir_store_var(&b, vars.arg, ac_nir_load_arg(&b, &args->ac, args->ac.rt.payload_offset), 1);

   nir_def *accel_struct = ac_nir_load_arg(&b, &args->ac, args->ac.rt.accel_struct);
   nir_store_var(&b, vars.accel_struct, nir_pack_64_2x32(&b, accel_struct), 1);
   nir_store_var(&b, vars.cull_mask_and_flags, ac_nir_load_arg(&b, &args->ac, args->ac.rt.cull_mask_and_flags), 1);
   nir_store_var(&b, vars.sbt_offset, ac_nir_load_arg(&b, &args->ac, args->ac.rt.sbt_offset), 1);
   nir_store_var(&b, vars.sbt_stride, ac_nir_load_arg(&b, &args->ac, args->ac.rt.sbt_stride), 1);
   nir_store_var(&b, vars.origin, ac_nir_load_arg(&b, &args->ac, args->ac.rt.ray_origin), 0x7);
   nir_store_var(&b, vars.tmin, ac_nir_load_arg(&b, &args->ac, args->ac.rt.ray_tmin), 1);
   nir_store_var(&b, vars.direction, ac_nir_load_arg(&b, &args->ac, args->ac.rt.ray_direction), 0x7);
   nir_store_var(&b, vars.tmax, ac_nir_load_arg(&b, &args->ac, args->ac.rt.ray_tmax), 1);

   if (traversal_info && traversal_info->miss_index.state == RADV_RT_CONST_ARG_STATE_VALID)
      nir_store_var(&b, vars.miss_index, nir_imm_int(&b, traversal_info->miss_index.value), 0x1);
   else
      nir_store_var(&b, vars.miss_index, ac_nir_load_arg(&b, &args->ac, args->ac.rt.miss_index), 0x1);

   nir_def *primitive_addr = ac_nir_load_arg(&b, &args->ac, args->ac.rt.primitive_addr);
   nir_store_var(&b, vars.primitive_addr, nir_pack_64_2x32(&b, primitive_addr), 1);
   nir_store_var(&b, vars.primitive_id, ac_nir_load_arg(&b, &args->ac, args->ac.rt.primitive_id), 1);
   nir_def *instance_addr = ac_nir_load_arg(&b, &args->ac, args->ac.rt.instance_addr);
   nir_store_var(&b, vars.instance_addr, nir_pack_64_2x32(&b, instance_addr), 1);
   nir_store_var(&b, vars.geometry_id_and_flags, ac_nir_load_arg(&b, &args->ac, args->ac.rt.geometry_id_and_flags), 1);
   nir_store_var(&b, vars.hit_kind, ac_nir_load_arg(&b, &args->ac, args->ac.rt.hit_kind), 1);

   /* guard the shader, so that only the correct invocations execute it */
   nir_if *shader_guard = NULL;
   if (shader->info.stage != MESA_SHADER_RAYGEN || resume_shader) {
      nir_def *uniform_shader_addr = ac_nir_load_arg(&b, &args->ac, args->ac.rt.uniform_shader_addr);
      uniform_shader_addr = nir_pack_64_2x32(&b, uniform_shader_addr);
      uniform_shader_addr = nir_ior_imm(&b, uniform_shader_addr, radv_get_rt_priority(shader->info.stage));

      shader_guard = nir_push_if(&b, nir_ieq(&b, uniform_shader_addr, shader_addr));
      shader_guard->control = nir_selection_control_divergent_always_taken;
   }

   nir_cf_reinsert(&list, b.cursor);

   if (shader_guard)
      nir_pop_if(&b, shader_guard);

   b.cursor = nir_after_impl(impl);

   if (monolithic) {
      nir_terminate(&b);
   } else {
      /* select next shader */
      shader_addr = nir_load_var(&b, vars.shader_addr);
      nir_def *next = select_next_shader(&b, shader_addr, info->wave_size);
      ac_nir_store_arg(&b, &args->ac, args->ac.rt.uniform_shader_addr, next);

      ac_nir_store_arg(&b, &args->ac, args->descriptor_sets[0], descriptor_sets);
      ac_nir_store_arg(&b, &args->ac, args->ac.push_constants, push_constants);
      ac_nir_store_arg(&b, &args->ac, args->ac.rt.sbt_descriptors, sbt_descriptors);
      ac_nir_store_arg(&b, &args->ac, args->ac.rt.traversal_shader_addr, traversal_addr);

      for (uint32_t i = 0; i < ARRAY_SIZE(launch_sizes); i++) {
         if (rt_info.uses_launch_size)
            ac_nir_store_arg(&b, &args->ac, args->ac.rt.launch_sizes[i], launch_sizes[i]);
         else
            radv_store_arg(&b, args, traversal_info, args->ac.rt.launch_sizes[i], launch_sizes[i]);
      }

      if (scratch_offset)
         ac_nir_store_arg(&b, &args->ac, args->ac.scratch_offset, scratch_offset);
      if (ring_offsets)
         ac_nir_store_arg(&b, &args->ac, args->ac.ring_offsets, ring_offsets);

      for (uint32_t i = 0; i < ARRAY_SIZE(launch_ids); i++) {
         if (rt_info.uses_launch_id)
            ac_nir_store_arg(&b, &args->ac, args->ac.rt.launch_ids[i], launch_ids[i]);
         else
            radv_store_arg(&b, args, traversal_info, args->ac.rt.launch_ids[i], launch_ids[i]);
      }

      /* store back all variables to registers */
      ac_nir_store_arg(&b, &args->ac, args->ac.rt.dynamic_callable_stack_base, nir_load_var(&b, vars.stack_ptr));
      ac_nir_store_arg(&b, &args->ac, args->ac.rt.shader_addr, shader_addr);
      radv_store_arg(&b, args, traversal_info, args->ac.rt.shader_record, nir_load_var(&b, vars.shader_record_ptr));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.payload_offset, nir_load_var(&b, vars.arg));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.accel_struct, nir_load_var(&b, vars.accel_struct));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.cull_mask_and_flags,
                     nir_load_var(&b, vars.cull_mask_and_flags));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.sbt_offset, nir_load_var(&b, vars.sbt_offset));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.sbt_stride, nir_load_var(&b, vars.sbt_stride));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.miss_index, nir_load_var(&b, vars.miss_index));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.ray_origin, nir_load_var(&b, vars.origin));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.ray_tmin, nir_load_var(&b, vars.tmin));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.ray_direction, nir_load_var(&b, vars.direction));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.ray_tmax, nir_load_var(&b, vars.tmax));

      if (has_position_fetch)
         radv_store_arg(&b, args, traversal_info, args->ac.rt.primitive_addr, nir_load_var(&b, vars.primitive_addr));

      radv_store_arg(&b, args, traversal_info, args->ac.rt.primitive_id, nir_load_var(&b, vars.primitive_id));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.instance_addr, nir_load_var(&b, vars.instance_addr));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.geometry_id_and_flags,
                     nir_load_var(&b, vars.geometry_id_and_flags));
      radv_store_arg(&b, args, traversal_info, args->ac.rt.hit_kind, nir_load_var(&b, vars.hit_kind));
   }

   nir_progress(true, impl, nir_metadata_none);

   /* cleanup passes */
   NIR_PASS(_, shader, nir_lower_global_vars_to_local);
   NIR_PASS(_, shader, nir_lower_vars_to_ssa);
   if (shader->info.stage == MESA_SHADER_CLOSEST_HIT || shader->info.stage == MESA_SHADER_INTERSECTION)
      NIR_PASS(_, shader, lower_hit_attribs, NULL, info->wave_size);
}

static bool
radv_arg_def_is_unused(nir_def *def)
{
   nir_foreach_use (use, def) {
      nir_instr *use_instr = nir_src_parent_instr(use);
      if (use_instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *use_intr = nir_instr_as_intrinsic(use_instr);
         if (use_intr->intrinsic == nir_intrinsic_store_scalar_arg_amd ||
             use_intr->intrinsic == nir_intrinsic_store_vector_arg_amd)
            continue;
      } else if (use_instr->type == nir_instr_type_phi) {
         nir_cf_node *prev_node = nir_cf_node_prev(&use_instr->block->cf_node);
         if (!prev_node)
            return false;

         nir_phi_instr *phi = nir_instr_as_phi(use_instr);
         if (radv_arg_def_is_unused(&phi->def))
            continue;
      }

      return false;
   }

   return true;
}

static bool
radv_gather_unused_args_instr(nir_builder *b, nir_intrinsic_instr *instr, void *data)
{
   if (instr->intrinsic != nir_intrinsic_load_scalar_arg_amd && instr->intrinsic != nir_intrinsic_load_vector_arg_amd)
      return false;

   if (!radv_arg_def_is_unused(&instr->def)) {
      /* This arg is used for more than passing data to the next stage. */
      struct radv_ray_tracing_stage_info *info = data;
      BITSET_CLEAR(info->unused_args, nir_intrinsic_base(instr));
   }

   return false;
}

void
radv_gather_unused_args(struct radv_ray_tracing_stage_info *info, nir_shader *nir)
{
   nir_shader_intrinsics_pass(nir, radv_gather_unused_args_instr, nir_metadata_all, info);
}
