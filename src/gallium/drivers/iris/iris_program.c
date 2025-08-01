/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iris_program.c
 *
 * This file contains the driver interface for compiling shaders.
 *
 * See iris_program_cache.c for the in-memory program cache where the
 * compiled shaders are stored.
 */

#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_atomic.h"
#include "util/u_upload_mgr.h"
#include "util/u_debug.h"
#include "util/u_async_debug.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_serialize.h"
#include "intel/compiler/brw_compiler.h"
#include "intel/compiler/brw_nir.h"
#include "intel/compiler/intel_nir.h"
#include "intel/compiler/brw_prim.h"
#ifdef INTEL_USE_ELK
#include "intel/compiler/elk/elk_compiler.h"
#include "intel/compiler/elk/elk_nir.h"
#include "intel/compiler/elk/elk_prim.h"
#endif
#include "iris_context.h"
#include "iris_pipe.h"
#include "nir/tgsi_to_nir.h"

static inline enum intel_vue_layout
vue_layout(bool separate_shader)
{
   return separate_shader ? INTEL_VUE_LAYOUT_SEPARATE : INTEL_VUE_LAYOUT_FIXED;
}

#define KEY_INIT(prefix)                                   \
   .prefix.program_string_id = ish->program_id,            \
   .prefix.limit_trig_input_range =                        \
      screen->driconf.limit_trig_input_range
#define BRW_KEY_INIT(base_key, _vue_layout) \
   .base.program_string_id = (base_key).program_string_id,     \
   .base.limit_trig_input_range = (base_key).limit_trig_input_range, \
   .base.vue_layout = _vue_layout

#ifdef INTEL_USE_ELK
#define ELK_KEY_INIT(gen, prog_id, limit_trig_input)       \
   .base.program_string_id = prog_id,                      \
   .base.limit_trig_input_range = limit_trig_input
#endif

struct iris_threaded_compile_job {
   struct iris_screen *screen;
   struct u_upload_mgr *uploader;
   struct util_debug_callback *dbg;
   struct iris_uncompiled_shader *ish;
   struct iris_compiled_shader *shader;
};

static unsigned
get_new_program_id(struct iris_screen *screen)
{
   return p_atomic_inc_return(&screen->program_id);
}

static void
iris_apply_brw_wm_prog_data(struct iris_compiled_shader *shader,
                            const struct brw_wm_prog_data *brw)
{
   assert(shader->stage == MESA_SHADER_FRAGMENT);
   struct iris_fs_data *iris = &shader->fs;

   STATIC_ASSERT(ARRAY_SIZE(iris->urb_setup) == ARRAY_SIZE(brw->urb_setup));
   STATIC_ASSERT(ARRAY_SIZE(iris->urb_setup_attribs) == ARRAY_SIZE(brw->urb_setup_attribs));
   memcpy(iris->urb_setup, brw->urb_setup, sizeof(iris->urb_setup));
   memcpy(iris->urb_setup_attribs, brw->urb_setup_attribs, brw->urb_setup_attribs_count);
   iris->urb_setup_attribs_count = brw->urb_setup_attribs_count;

   iris->num_varying_inputs   = brw->num_varying_inputs;
   iris->msaa_flags_param     = brw->msaa_flags_param;
   iris->flat_inputs          = brw->flat_inputs;
   iris->inputs               = brw->inputs;
   iris->computed_depth_mode  = brw->computed_depth_mode;
   iris->max_polygons         = brw->max_polygons;
   iris->dispatch_multi       = brw->dispatch_multi;
   iris->computed_stencil     = brw->computed_stencil;
   iris->early_fragment_tests = brw->early_fragment_tests;
   iris->post_depth_coverage  = brw->post_depth_coverage;
   iris->inner_coverage       = brw->inner_coverage;
   iris->dispatch_8           = brw->dispatch_8;
   iris->dispatch_16          = brw->dispatch_16;
   iris->dispatch_32          = brw->dispatch_32;
   iris->dual_src_blend       = brw->dual_src_blend;
   iris->uses_pos_offset      = brw->uses_pos_offset;
   iris->uses_omask           = brw->uses_omask;
   iris->uses_kill            = brw->uses_kill;
   iris->uses_src_depth       = brw->uses_src_depth;
   iris->uses_src_w           = brw->uses_src_w;
   iris->uses_sample_mask     = brw->uses_sample_mask;
   iris->uses_vmask           = brw->uses_vmask;
   iris->has_side_effects     = brw->has_side_effects;
   iris->pulls_bary           = brw->pulls_bary;

   iris->uses_sample_offsets        = brw->uses_sample_offsets;
   iris->uses_npc_bary_coefficients = brw->uses_npc_bary_coefficients;
   iris->uses_pc_bary_coefficients  = brw->uses_pc_bary_coefficients;
   iris->uses_depth_w_coefficients  = brw->uses_depth_w_coefficients;

   iris->uses_nonperspective_interp_modes = brw->uses_nonperspective_interp_modes;

   iris->is_per_sample = brw_wm_prog_data_is_persample(brw, 0);
}

static void
iris_apply_brw_cs_prog_data(struct iris_compiled_shader *shader,
                            const struct brw_cs_prog_data *brw)
{
   assert(shader->stage == MESA_SHADER_COMPUTE);
   struct iris_cs_data *iris = &shader->cs;

   iris->push.cross_thread.dwords = brw->push.cross_thread.dwords;
   iris->push.cross_thread.regs   = brw->push.cross_thread.regs;
   iris->push.cross_thread.size   = brw->push.cross_thread.size;

   iris->push.per_thread.dwords = brw->push.per_thread.dwords;
   iris->push.per_thread.regs   = brw->push.per_thread.regs;
   iris->push.per_thread.size   = brw->push.per_thread.size;

   iris->local_size[0]  = brw->local_size[0];
   iris->local_size[1]  = brw->local_size[1];
   iris->local_size[2]  = brw->local_size[2];
   iris->prog_offset[0] = brw->prog_offset[0];
   iris->prog_offset[1] = brw->prog_offset[1];
   iris->prog_offset[2] = brw->prog_offset[2];

   iris->generate_local_id = brw->generate_local_id;
   iris->walk_order        = brw->walk_order;
   iris->uses_barrier      = brw->uses_barrier;
   iris->uses_sampler      = brw->uses_sampler;
   iris->prog_mask         = brw->prog_mask;

   iris->first_param_is_builtin_subgroup_id =
      brw->base.nr_params > 0 &&
      brw->base.param[0] == BRW_PARAM_BUILTIN_SUBGROUP_ID;
}

static void
iris_apply_brw_vue_prog_data(const struct brw_vue_prog_data *brw,
                             struct iris_vue_data *iris)
{
   memcpy(&iris->vue_map, &brw->vue_map, sizeof(struct intel_vue_map));

   iris->urb_read_length     = brw->urb_read_length;
   iris->cull_distance_mask  = brw->cull_distance_mask;
   iris->urb_entry_size      = brw->urb_entry_size;
   iris->dispatch_mode       = brw->dispatch_mode;
   iris->include_vue_handles = brw->include_vue_handles;
}

static void
iris_apply_brw_vs_prog_data(struct iris_compiled_shader *shader,
                            const struct brw_vs_prog_data *brw)
{
   assert(shader->stage == MESA_SHADER_VERTEX);
   struct iris_vs_data *iris = &shader->vs;

   iris_apply_brw_vue_prog_data(&brw->base, &iris->base);

   iris->uses_vertexid     = brw->uses_vertexid;
   iris->uses_instanceid   = brw->uses_instanceid;
   iris->uses_firstvertex  = brw->uses_firstvertex;
   iris->uses_baseinstance = brw->uses_baseinstance;
   iris->uses_drawid       = brw->uses_drawid;
}

static void
iris_apply_brw_tcs_prog_data(struct iris_compiled_shader *shader,
                             const struct brw_tcs_prog_data *brw)
{
   assert(shader->stage == MESA_SHADER_TESS_CTRL);
   struct iris_tcs_data *iris = &shader->tcs;

   iris_apply_brw_vue_prog_data(&brw->base, &iris->base);

   iris->instances             = brw->instances;
   iris->patch_count_threshold = brw->patch_count_threshold;
   iris->include_primitive_id  = brw->include_primitive_id;
}

static void
iris_apply_brw_tes_prog_data(struct iris_compiled_shader *shader,
                             const struct brw_tes_prog_data *brw)
{
   assert(shader->stage == MESA_SHADER_TESS_EVAL);
   struct iris_tes_data *iris = &shader->tes;

   iris_apply_brw_vue_prog_data(&brw->base, &iris->base);

   iris->partitioning         = brw->partitioning;
   iris->output_topology      = brw->output_topology;
   iris->domain               = brw->domain;
   iris->include_primitive_id = brw->include_primitive_id;
}

static void
iris_apply_brw_gs_prog_data(struct iris_compiled_shader *shader,
                            const struct brw_gs_prog_data *brw)
{
   assert(shader->stage == MESA_SHADER_GEOMETRY);
   struct iris_gs_data *iris = &shader->gs;

   iris_apply_brw_vue_prog_data(&brw->base, &iris->base);

   iris->vertices_in                     = brw->vertices_in;
   iris->output_vertex_size_hwords       = brw->output_vertex_size_hwords;
   iris->output_topology                 = brw->output_topology;
   iris->control_data_header_size_hwords = brw->control_data_header_size_hwords;
   iris->control_data_format             = brw->control_data_format;
   iris->static_vertex_count             = brw->static_vertex_count;
   iris->invocations                     = brw->invocations;
   iris->include_primitive_id            = brw->include_primitive_id;
}

void
iris_apply_brw_prog_data(struct iris_compiled_shader *shader,
                         struct brw_stage_prog_data *brw)
{
   STATIC_ASSERT(ARRAY_SIZE(brw->ubo_ranges) == ARRAY_SIZE(shader->ubo_ranges));
   for (int i = 0; i < ARRAY_SIZE(shader->ubo_ranges); i++) {
      shader->ubo_ranges[i].block  = brw->ubo_ranges[i].block;
      shader->ubo_ranges[i].start  = brw->ubo_ranges[i].start;
      shader->ubo_ranges[i].length = brw->ubo_ranges[i].length;
   }

   shader->nr_params              = brw->nr_params;
   shader->total_scratch          = brw->total_scratch;
   shader->total_shared           = brw->total_shared;
   shader->program_size           = brw->program_size;
   shader->const_data_offset      = brw->const_data_offset;
   shader->dispatch_grf_start_reg = brw->dispatch_grf_start_reg;
   shader->has_ubo_pull           = brw->has_ubo_pull;
   shader->use_alt_mode           = brw->use_alt_mode;

   switch (shader->stage) {
   case MESA_SHADER_FRAGMENT:
      iris_apply_brw_wm_prog_data(shader, brw_wm_prog_data_const(brw));
      break;
   case MESA_SHADER_COMPUTE:
      iris_apply_brw_cs_prog_data(shader, brw_cs_prog_data_const(brw));
      break;
   case MESA_SHADER_VERTEX:
      iris_apply_brw_vs_prog_data(shader, brw_vs_prog_data_const(brw));
      break;
   case MESA_SHADER_TESS_CTRL:
      iris_apply_brw_tcs_prog_data(shader, brw_tcs_prog_data_const(brw));
      break;
   case MESA_SHADER_TESS_EVAL:
      iris_apply_brw_tes_prog_data(shader, brw_tes_prog_data_const(brw));
      break;
   case MESA_SHADER_GEOMETRY:
      iris_apply_brw_gs_prog_data(shader, brw_gs_prog_data_const(brw));
      break;
   default:
      UNREACHABLE("invalid shader stage");
   }

   shader->brw_prog_data = brw;

   ralloc_steal(shader, shader->brw_prog_data);
   ralloc_steal(shader->brw_prog_data, (void *)brw->relocs);
   ralloc_steal(shader->brw_prog_data, brw->param);
}

#ifdef INTEL_USE_ELK

static void
iris_apply_elk_wm_prog_data(struct iris_compiled_shader *shader,
                            const struct elk_wm_prog_data *elk)
{
   assert(shader->stage == MESA_SHADER_FRAGMENT);
   struct iris_fs_data *iris = &shader->fs;

   STATIC_ASSERT(ARRAY_SIZE(iris->urb_setup) == ARRAY_SIZE(elk->urb_setup));
   STATIC_ASSERT(ARRAY_SIZE(iris->urb_setup_attribs) == ARRAY_SIZE(elk->urb_setup_attribs));
   memcpy(iris->urb_setup, elk->urb_setup, sizeof(iris->urb_setup));
   memcpy(iris->urb_setup_attribs, elk->urb_setup_attribs, elk->urb_setup_attribs_count);
   iris->urb_setup_attribs_count = elk->urb_setup_attribs_count;

   iris->num_varying_inputs   = elk->num_varying_inputs;
   iris->msaa_flags_param     = elk->msaa_flags_param;
   iris->flat_inputs          = elk->flat_inputs;
   iris->inputs               = elk->inputs;
   iris->computed_depth_mode  = elk->computed_depth_mode;
   iris->max_polygons         = 1;
   iris->dispatch_multi       = 0;
   iris->computed_stencil     = elk->computed_stencil;
   iris->early_fragment_tests = elk->early_fragment_tests;
   iris->post_depth_coverage  = elk->post_depth_coverage;
   iris->inner_coverage       = elk->inner_coverage;
   iris->dispatch_8           = elk->dispatch_8;
   iris->dispatch_16          = elk->dispatch_16;
   iris->dispatch_32          = elk->dispatch_32;
   iris->dual_src_blend       = elk->dual_src_blend;
   iris->uses_pos_offset      = elk->uses_pos_offset;
   iris->uses_omask           = elk->uses_omask;
   iris->uses_kill            = elk->uses_kill;
   iris->uses_src_depth       = elk->uses_src_depth;
   iris->uses_src_w           = elk->uses_src_w;
   iris->uses_sample_mask     = elk->uses_sample_mask;
   iris->uses_vmask           = elk->uses_vmask;
   iris->pulls_bary           = elk->pulls_bary;
   iris->has_side_effects     = elk->has_side_effects;

   iris->uses_nonperspective_interp_modes = elk->uses_nonperspective_interp_modes;

   iris->is_per_sample = elk_wm_prog_data_is_persample(elk, 0);
}

static void
iris_apply_elk_cs_prog_data(struct iris_compiled_shader *shader,
                            const struct elk_cs_prog_data *elk)
{
   assert(shader->stage == MESA_SHADER_COMPUTE);
   struct iris_cs_data *iris = &shader->cs;

   iris->push.cross_thread.dwords = elk->push.cross_thread.dwords;
   iris->push.cross_thread.regs   = elk->push.cross_thread.regs;
   iris->push.cross_thread.size   = elk->push.cross_thread.size;

   iris->push.per_thread.dwords = elk->push.per_thread.dwords;
   iris->push.per_thread.regs   = elk->push.per_thread.regs;
   iris->push.per_thread.size   = elk->push.per_thread.size;

   iris->local_size[0]  = elk->local_size[0];
   iris->local_size[1]  = elk->local_size[1];
   iris->local_size[2]  = elk->local_size[2];
   iris->prog_offset[0] = elk->prog_offset[0];
   iris->prog_offset[1] = elk->prog_offset[1];
   iris->prog_offset[2] = elk->prog_offset[2];

   iris->uses_barrier      = elk->uses_barrier;
   iris->prog_mask         = elk->prog_mask;

   iris->first_param_is_builtin_subgroup_id =
      elk->base.nr_params > 0 &&
      elk->base.param[0] == ELK_PARAM_BUILTIN_SUBGROUP_ID;
}

static void
iris_apply_elk_vue_prog_data(const struct elk_vue_prog_data *elk,
                             struct iris_vue_data *iris)
{
   memcpy(&iris->vue_map, &elk->vue_map, sizeof(struct intel_vue_map));

   iris->urb_read_length     = elk->urb_read_length;
   iris->cull_distance_mask  = elk->cull_distance_mask;
   iris->urb_entry_size      = elk->urb_entry_size;
   iris->dispatch_mode       = elk->dispatch_mode;
   iris->include_vue_handles = elk->include_vue_handles;
}

static void
iris_apply_elk_vs_prog_data(struct iris_compiled_shader *shader,
                            const struct elk_vs_prog_data *elk)
{
   assert(shader->stage == MESA_SHADER_VERTEX);
   struct iris_vs_data *iris = &shader->vs;

   iris_apply_elk_vue_prog_data(&elk->base, &iris->base);

   iris->uses_vertexid     = elk->uses_vertexid;
   iris->uses_instanceid   = elk->uses_instanceid;
   iris->uses_firstvertex  = elk->uses_firstvertex;
   iris->uses_baseinstance = elk->uses_baseinstance;
   iris->uses_drawid       = elk->uses_drawid;
}

static void
iris_apply_elk_tcs_prog_data(struct iris_compiled_shader *shader,
                             const struct elk_tcs_prog_data *elk)
{
   assert(shader->stage == MESA_SHADER_TESS_CTRL);
   struct iris_tcs_data *iris = &shader->tcs;

   iris_apply_elk_vue_prog_data(&elk->base, &iris->base);

   iris->instances             = elk->instances;
   iris->patch_count_threshold = elk->patch_count_threshold;
   iris->include_primitive_id  = elk->include_primitive_id;
}

static void
iris_apply_elk_tes_prog_data(struct iris_compiled_shader *shader,
                             const struct elk_tes_prog_data *elk)
{
   assert(shader->stage == MESA_SHADER_TESS_EVAL);
   struct iris_tes_data *iris = &shader->tes;

   iris_apply_elk_vue_prog_data(&elk->base, &iris->base);

   iris->partitioning         = elk->partitioning;
   iris->output_topology      = elk->output_topology;
   iris->domain               = elk->domain;
   iris->include_primitive_id = elk->include_primitive_id;
}

static void
iris_apply_elk_gs_prog_data(struct iris_compiled_shader *shader,
                            const struct elk_gs_prog_data *elk)
{
   assert(shader->stage == MESA_SHADER_GEOMETRY);
   struct iris_gs_data *iris = &shader->gs;

   iris_apply_elk_vue_prog_data(&elk->base, &iris->base);

   iris->vertices_in                     = elk->vertices_in;
   iris->output_vertex_size_hwords       = elk->output_vertex_size_hwords;
   iris->output_topology                 = elk->output_topology;
   iris->control_data_header_size_hwords = elk->control_data_header_size_hwords;
   iris->control_data_format             = elk->control_data_format;
   iris->static_vertex_count             = elk->static_vertex_count;
   iris->invocations                     = elk->invocations;
   iris->include_primitive_id            = elk->include_primitive_id;
}

void
iris_apply_elk_prog_data(struct iris_compiled_shader *shader,
                         struct elk_stage_prog_data *elk)
{
   STATIC_ASSERT(ARRAY_SIZE(elk->ubo_ranges) == ARRAY_SIZE(shader->ubo_ranges));
   for (int i = 0; i < ARRAY_SIZE(shader->ubo_ranges); i++) {
      shader->ubo_ranges[i].block  = elk->ubo_ranges[i].block;
      shader->ubo_ranges[i].start  = elk->ubo_ranges[i].start;
      shader->ubo_ranges[i].length = elk->ubo_ranges[i].length;
   }

   shader->nr_params              = elk->nr_params;
   shader->total_scratch          = elk->total_scratch;
   shader->total_shared           = elk->total_shared;
   shader->program_size           = elk->program_size;
   shader->const_data_offset      = elk->const_data_offset;
   shader->dispatch_grf_start_reg = elk->dispatch_grf_start_reg;
   shader->has_ubo_pull           = elk->has_ubo_pull;
   shader->use_alt_mode           = elk->use_alt_mode;

   switch (shader->stage) {
   case MESA_SHADER_FRAGMENT:
      iris_apply_elk_wm_prog_data(shader, elk_wm_prog_data_const(elk));
      break;
   case MESA_SHADER_COMPUTE:
      iris_apply_elk_cs_prog_data(shader, elk_cs_prog_data_const(elk));
      break;
   case MESA_SHADER_VERTEX:
      iris_apply_elk_vs_prog_data(shader, elk_vs_prog_data_const(elk));
      break;
   case MESA_SHADER_TESS_CTRL:
      iris_apply_elk_tcs_prog_data(shader, elk_tcs_prog_data_const(elk));
      break;
   case MESA_SHADER_TESS_EVAL:
      iris_apply_elk_tes_prog_data(shader, elk_tes_prog_data_const(elk));
      break;
   case MESA_SHADER_GEOMETRY:
      iris_apply_elk_gs_prog_data(shader, elk_gs_prog_data_const(elk));
      break;
   default:
      UNREACHABLE("invalid shader stage");
   }

   shader->elk_prog_data = elk;

   ralloc_steal(shader, shader->elk_prog_data);
   ralloc_steal(shader->elk_prog_data, (void *)elk->relocs);
   ralloc_steal(shader->elk_prog_data, elk->param);
}

#endif

void
iris_finalize_program(struct iris_compiled_shader *shader,
                      uint32_t *streamout,
                      uint32_t *system_values,
                      unsigned num_system_values,
                      unsigned num_cbufs,
                      const struct iris_binding_table *bt)
{
   /* There can be only one. */
#ifdef INTEL_USE_ELK
   assert((shader->brw_prog_data == NULL) != (shader->elk_prog_data == NULL));
#else
   assert(shader->brw_prog_data);
#endif

   shader->streamout = streamout;
   shader->system_values = system_values;
   shader->num_system_values = num_system_values;
   shader->num_cbufs = num_cbufs;
   shader->bt = *bt;

   ralloc_steal(shader, shader->streamout);
   ralloc_steal(shader, shader->system_values);
}

static struct brw_vs_prog_key
iris_to_brw_vs_key(const struct iris_screen *screen,
                   const struct iris_vs_prog_key *key)
{
   return (struct brw_vs_prog_key) {
      BRW_KEY_INIT(key->vue.base, key->vue.layout),
   };
}

static struct brw_tcs_prog_key
iris_to_brw_tcs_key(const struct iris_screen *screen,
                    const struct iris_tcs_prog_key *key)
{
   return (struct brw_tcs_prog_key) {
      BRW_KEY_INIT(key->vue.base, key->vue.layout),
      ._tes_primitive_mode = key->_tes_primitive_mode,
      .input_vertices = key->input_vertices,
      .patch_outputs_written = key->patch_outputs_written,
      .outputs_written = key->outputs_written,
   };
}

static struct brw_tes_prog_key
iris_to_brw_tes_key(const struct iris_screen *screen,
                    const struct iris_tes_prog_key *key)
{
   return (struct brw_tes_prog_key) {
      BRW_KEY_INIT(key->vue.base, key->vue.layout),
      .patch_inputs_read = key->patch_inputs_read,
      .inputs_read = key->inputs_read,
   };
}

static struct brw_gs_prog_key
iris_to_brw_gs_key(const struct iris_screen *screen,
                   const struct iris_gs_prog_key *key)
{
   return (struct brw_gs_prog_key) {
      BRW_KEY_INIT(key->vue.base, key->vue.layout),
   };
}

static struct brw_wm_prog_key
iris_to_brw_fs_key(const struct iris_screen *screen,
                   const struct iris_fs_prog_key *key)
{
   return (struct brw_wm_prog_key) {
      BRW_KEY_INIT(key->base, key->vue_layout),
      .nr_color_regions = key->nr_color_regions,
      .flat_shade = key->flat_shade,
      .alpha_test_replicate_alpha = key->alpha_test_replicate_alpha,
      .alpha_to_coverage = key->alpha_to_coverage ? INTEL_ALWAYS : INTEL_NEVER,
      .clamp_fragment_color = key->clamp_fragment_color,
      .persample_interp = key->persample_interp ? INTEL_ALWAYS : INTEL_NEVER,
      .multisample_fbo = key->multisample_fbo ? INTEL_ALWAYS : INTEL_NEVER,
      .force_dual_color_blend = key->force_dual_color_blend,
      .coherent_fb_fetch = key->coherent_fb_fetch,
      .color_outputs_valid = key->color_outputs_valid,
      .input_slots_valid = key->input_slots_valid,
      .ignore_sample_mask_out = !key->multisample_fbo,
      .null_push_constant_tbimr_workaround =
         screen->devinfo->needs_null_push_constant_tbimr_workaround,
   };
}

static struct brw_cs_prog_key
iris_to_brw_cs_key(const struct iris_screen *screen,
                   const struct iris_cs_prog_key *key)
{
   return (struct brw_cs_prog_key) {
      BRW_KEY_INIT(key->base, INTEL_VUE_LAYOUT_SEPARATE),
   };
}

#ifdef INTEL_USE_ELK

static struct elk_vs_prog_key
iris_to_elk_vs_key(const struct iris_screen *screen,
                   const struct iris_vs_prog_key *key)
{
   return (struct elk_vs_prog_key) {
      ELK_KEY_INIT(screen->devinfo->ver, key->vue.base.program_string_id,
                   key->vue.base.limit_trig_input_range),

      /* Don't tell the backend about our clip plane constants, we've
       * already lowered them in NIR and don't want it doing it again.
       */
      .nr_userclip_plane_consts = 0,
   };
}

static struct elk_tcs_prog_key
iris_to_elk_tcs_key(const struct iris_screen *screen,
                    const struct iris_tcs_prog_key *key)
{
   return (struct elk_tcs_prog_key) {
      ELK_KEY_INIT(screen->devinfo->ver, key->vue.base.program_string_id,
                   key->vue.base.limit_trig_input_range),
      ._tes_primitive_mode = key->_tes_primitive_mode,
      .input_vertices = key->input_vertices,
      .patch_outputs_written = key->patch_outputs_written,
      .outputs_written = key->outputs_written,
      .quads_workaround = key->quads_workaround,
   };
}

static struct elk_tes_prog_key
iris_to_elk_tes_key(const struct iris_screen *screen,
                    const struct iris_tes_prog_key *key)
{
   return (struct elk_tes_prog_key) {
      ELK_KEY_INIT(screen->devinfo->ver, key->vue.base.program_string_id,
                   key->vue.base.limit_trig_input_range),
      .patch_inputs_read = key->patch_inputs_read,
      .inputs_read = key->inputs_read,
   };
}

static struct elk_gs_prog_key
iris_to_elk_gs_key(const struct iris_screen *screen,
                   const struct iris_gs_prog_key *key)
{
   return (struct elk_gs_prog_key) {
      ELK_KEY_INIT(screen->devinfo->ver, key->vue.base.program_string_id,
                   key->vue.base.limit_trig_input_range),
   };
}

static struct elk_wm_prog_key
iris_to_elk_fs_key(const struct iris_screen *screen,
                   const struct iris_fs_prog_key *key)
{
   return (struct elk_wm_prog_key) {
      ELK_KEY_INIT(screen->devinfo->ver, key->base.program_string_id,
                   key->base.limit_trig_input_range),
      .nr_color_regions = key->nr_color_regions,
      .flat_shade = key->flat_shade,
      .alpha_test_replicate_alpha = key->alpha_test_replicate_alpha,
      .alpha_to_coverage = key->alpha_to_coverage ? ELK_ALWAYS : ELK_NEVER,
      .clamp_fragment_color = key->clamp_fragment_color,
      .persample_interp = key->persample_interp ? ELK_ALWAYS : ELK_NEVER,
      .multisample_fbo = key->multisample_fbo ? ELK_ALWAYS : ELK_NEVER,
      .force_dual_color_blend = key->force_dual_color_blend,
      .coherent_fb_fetch = key->coherent_fb_fetch,
      .color_outputs_valid = key->color_outputs_valid,
      .input_slots_valid = key->input_slots_valid,
      .ignore_sample_mask_out = !key->multisample_fbo,
   };
}

static struct elk_cs_prog_key
iris_to_elk_cs_key(const struct iris_screen *screen,
                   const struct iris_cs_prog_key *key)
{
   return (struct elk_cs_prog_key) {
      ELK_KEY_INIT(screen->devinfo->ver, key->base.program_string_id,
                   key->base.limit_trig_input_range),
   };
}

#endif

static void *
upload_state(struct u_upload_mgr *uploader,
             struct iris_state_ref *ref,
             unsigned size,
             unsigned alignment)
{
   void *p = NULL;
   u_upload_alloc(uploader, 0, size, alignment, &ref->offset, &ref->res, &p);
   return p;
}

void
iris_upload_ubo_ssbo_surf_state(struct iris_context *ice,
                                struct pipe_shader_buffer *buf,
                                struct iris_state_ref *surf_state,
                                isl_surf_usage_flags_t usage)
{
   struct pipe_context *ctx = &ice->ctx;
   struct iris_screen *screen = (struct iris_screen *) ctx->screen;
   bool ssbo = usage & ISL_SURF_USAGE_STORAGE_BIT;

   void *map =
      upload_state(ice->state.surface_uploader, surf_state,
                   screen->isl_dev.ss.size, 64);
   if (!unlikely(map)) {
      surf_state->res = NULL;
      return;
   }

   struct iris_resource *res = (void *) buf->buffer;
   struct iris_bo *surf_bo = iris_resource_bo(surf_state->res);
   surf_state->offset += iris_bo_offset_from_base_address(surf_bo);

   const bool dataport = ssbo || !iris_indirect_ubos_use_sampler(screen);

   isl_buffer_fill_state(&screen->isl_dev, map,
                         .address = res->bo->address + res->offset +
                                    buf->buffer_offset,
                         .size_B = buf->buffer_size - res->offset,
                         .format = dataport ? ISL_FORMAT_RAW
                                            : ISL_FORMAT_R32G32B32A32_FLOAT,
                         .swizzle = ISL_SWIZZLE_IDENTITY,
                         .stride_B = 1,
                         .usage = usage,
                         .mocs = iris_mocs(res->bo, &screen->isl_dev, usage));
}

static nir_def *
get_aoa_deref_offset(nir_builder *b,
                     nir_deref_instr *deref,
                     unsigned elem_size)
{
   unsigned array_size = elem_size;
   nir_def *offset = nir_imm_int(b, 0);

   while (deref->deref_type != nir_deref_type_var) {
      assert(deref->deref_type == nir_deref_type_array);

      /* This level's element size is the previous level's array size */
      nir_def *index = deref->arr.index.ssa;
      assert(deref->arr.index.ssa);
      offset = nir_iadd(b, offset,
                           nir_imul_imm(b, index, array_size));

      deref = nir_deref_instr_parent(deref);
      assert(glsl_type_is_array(deref->type));
      array_size *= glsl_get_length(deref->type);
   }

   /* Accessing an invalid surface index with the dataport can result in a
    * hang.  According to the spec "if the index used to select an individual
    * element is negative or greater than or equal to the size of the array,
    * the results of the operation are undefined but may not lead to
    * termination" -- which is one of the possible outcomes of the hang.
    * Clamp the index to prevent access outside of the array bounds.
    */
   return nir_umin(b, offset, nir_imm_int(b, array_size - elem_size));
}

static bool
iris_lower_storage_image_derefs_instr(nir_builder *b,
                                      nir_intrinsic_instr *intrin,
                                      UNUSED void *_)
{
   switch (intrin->intrinsic) {
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
   case nir_intrinsic_image_deref_load_raw_intel:
   case nir_intrinsic_image_deref_store_raw_intel: {
      nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
      nir_variable *var = nir_deref_instr_get_variable(deref);

      b->cursor = nir_before_instr(&intrin->instr);
      nir_def *index =
         nir_iadd_imm(b, get_aoa_deref_offset(b, deref, 1),
                      var->data.driver_location);
      nir_rewrite_image_intrinsic(intrin, index, false);
      return true;
   }

   default:
      return false;
   }
}

static bool
iris_lower_storage_image_derefs(nir_shader *nir)
{
   return nir_shader_intrinsics_pass(nir, iris_lower_storage_image_derefs_instr,
                              nir_metadata_control_flow,
                              NULL);
}

static bool
iris_uses_image_atomic(const nir_shader *shader)
{
   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_image_deref_atomic:
            case nir_intrinsic_image_deref_atomic_swap:
               UNREACHABLE("Should have been lowered in "
                           "iris_lower_storage_image_derefs");

            case nir_intrinsic_image_atomic:
            case nir_intrinsic_image_atomic_swap:
               return true;

            default:
               break;
            }
         }
      }
   }

   return false;
}

/**
 * Undo nir_lower_passthrough_edgeflags but keep the inputs_read flag.
 */
static bool
iris_fix_edge_flags(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_VERTEX) {
      nir_shader_preserve_all_metadata(nir);
      return false;
   }

   nir_variable *var = nir_find_variable_with_location(nir, nir_var_shader_out,
                                                       VARYING_SLOT_EDGE);
   if (!var) {
      nir_shader_preserve_all_metadata(nir);
      return false;
   }

   var->data.mode = nir_var_shader_temp;
   nir->info.outputs_written &= ~VARYING_BIT_EDGE;
   nir->info.inputs_read &= ~VERT_BIT_EDGEFLAG;
   nir_fixup_deref_modes(nir);

   nir_foreach_function_impl(impl, nir) {
      nir_progress(true, impl,
                   nir_metadata_control_flow | nir_metadata_live_defs | nir_metadata_loop_analysis);
   }

   return true;
}

/**
 * Fix an uncompiled shader's stream output info.
 *
 * Core Gallium stores output->register_index as a "slot" number, where
 * slots are assigned consecutively to all outputs in info->outputs_written.
 * This naive packing of outputs doesn't work for us - we too have slots,
 * but the layout is defined by the VUE map, which we won't have until we
 * compile a specific shader variant.  So, we remap these and simply store
 * VARYING_SLOT_* in our copy's output->register_index fields.
 *
 * We also fix up VARYING_SLOT_{LAYER,VIEWPORT,PSIZ} to select the Y/Z/W
 * components of our VUE header.  See brw_vue_map.c for the layout.
 */
static void
update_so_info(struct pipe_stream_output_info *so_info,
               uint64_t outputs_written)
{
   uint8_t reverse_map[64] = {};
   unsigned slot = 0;
   while (outputs_written) {
      reverse_map[slot++] = u_bit_scan64(&outputs_written);
   }

   for (unsigned i = 0; i < so_info->num_outputs; i++) {
      struct pipe_stream_output *output = &so_info->output[i];

      /* Map Gallium's condensed "slots" back to real VARYING_SLOT_* enums */
      output->register_index = reverse_map[output->register_index];

      /* The VUE header contains three scalar fields packed together:
       * - gl_PointSize is stored in VARYING_SLOT_PSIZ.w
       * - gl_Layer is stored in VARYING_SLOT_PSIZ.y
       * - gl_ViewportIndex is stored in VARYING_SLOT_PSIZ.z
       */
      switch (output->register_index) {
      case VARYING_SLOT_LAYER:
         assert(output->num_components == 1);
         output->register_index = VARYING_SLOT_PSIZ;
         output->start_component = 1;
         break;
      case VARYING_SLOT_VIEWPORT:
         assert(output->num_components == 1);
         output->register_index = VARYING_SLOT_PSIZ;
         output->start_component = 2;
         break;
      case VARYING_SLOT_PSIZ:
         assert(output->num_components == 1);
         output->start_component = 3;
         break;
      }

      //info->outputs_written |= 1ull << output->register_index;
   }
}

static void
setup_vec4_image_sysval(uint32_t *sysvals, uint32_t idx,
                        unsigned offset, unsigned n)
{
#ifdef INTEL_USE_ELK
   assert(offset % sizeof(uint32_t) == 0);

   for (unsigned i = 0; i < n; ++i)
      sysvals[i] = ELK_PARAM_IMAGE(idx, offset / sizeof(uint32_t) + i);

   for (unsigned i = n; i < 4; ++i)
      sysvals[i] = ELK_PARAM_BUILTIN_ZERO;
#else
   UNREACHABLE("no elk support");
#endif
}

/**
 * Associate NIR uniform variables with the prog_data->param[] mechanism
 * used by the backend.  Also, decide which UBOs we'd like to push in an
 * ideal situation (though the backend can reduce this).
 */
static void
iris_setup_uniforms(ASSERTED const struct intel_device_info *devinfo,
                    void *mem_ctx,
                    nir_shader *nir,
                    uint32_t **out_system_values,
                    unsigned *out_num_system_values,
                    unsigned *out_num_cbufs)
{
   const unsigned IRIS_MAX_SYSTEM_VALUES =
      PIPE_MAX_SHADER_IMAGES * ISL_IMAGE_PARAM_SIZE;
   unsigned *system_values =
      rzalloc_array(mem_ctx, unsigned, IRIS_MAX_SYSTEM_VALUES);
   unsigned num_system_values = 0;

   unsigned patch_vert_idx = -1;
   unsigned tess_outer_default_idx = -1;
   unsigned tess_inner_default_idx = -1;
   unsigned ucp_idx[IRIS_MAX_CLIP_PLANES];
   unsigned img_idx[PIPE_MAX_SHADER_IMAGES];
   unsigned variable_group_size_idx = -1;
   unsigned work_dim_idx = -1;
   memset(ucp_idx, -1, sizeof(ucp_idx));
   memset(img_idx, -1, sizeof(img_idx));

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   nir_builder b = nir_builder_at(nir_before_impl(impl));

   nir_def *temp_ubo_name = nir_undef(&b, 1, 32);

   /* Turn system value intrinsics into uniforms */
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         nir_def *offset;

         switch (intrin->intrinsic) {
         case nir_intrinsic_load_base_workgroup_id: {
            /* GL doesn't have a concept of base workgroup */
            b.cursor = nir_instr_remove(&intrin->instr);
            nir_def_rewrite_uses(&intrin->def,
                                     nir_imm_zero(&b, 3, 32));
            continue;
         }
         case nir_intrinsic_load_constant: {
            unsigned load_size = intrin->def.num_components *
                                 intrin->def.bit_size / 8;
            unsigned load_align = intrin->def.bit_size / 8;

            /* This one is special because it reads from the shader constant
             * data and not cbuf0 which gallium uploads for us.
             */
            b.cursor = nir_instr_remove(&intrin->instr);

            nir_def *offset =
               nir_iadd_imm(&b, intrin->src[0].ssa,
                                nir_intrinsic_base(intrin));

            assert(load_size < b.shader->constant_data_size);
            unsigned max_offset = b.shader->constant_data_size - load_size;
            offset = nir_umin(&b, offset, nir_imm_int(&b, max_offset));

            /* Constant data lives in buffers within IRIS_MEMZONE_SHADER
             * and cannot cross that 4GB boundary, so we can do the address
             * calculation with 32-bit adds.  Also, we can ignore the high
             * bits because IRIS_MEMZONE_SHADER is in the [0, 4GB) range.
             */
            assert(IRIS_MEMZONE_SHADER_START >> 32 == 0ull);

            nir_def *const_data_addr =
               nir_iadd(&b, nir_load_reloc_const_intel(&b, BRW_SHADER_RELOC_CONST_DATA_ADDR_LOW), offset);

            nir_def *data =
               nir_load_global_constant(&b, nir_u2u64(&b, const_data_addr),
                                        load_align,
                                        intrin->def.num_components,
                                        intrin->def.bit_size);

            nir_def_rewrite_uses(&intrin->def,
                                     data);
            continue;
         }
         case nir_intrinsic_load_user_clip_plane: {
            unsigned ucp = nir_intrinsic_ucp_id(intrin);

            if (ucp_idx[ucp] == -1) {
               ucp_idx[ucp] = num_system_values;
               num_system_values += 4;
            }

            for (int i = 0; i < 4; i++) {
               system_values[ucp_idx[ucp] + i] =
                  BRW_PARAM_BUILTIN_CLIP_PLANE(ucp, i);
            }

            b.cursor = nir_before_instr(instr);
            offset = nir_imm_int(&b, ucp_idx[ucp] * sizeof(uint32_t));
            break;
         }
         case nir_intrinsic_load_patch_vertices_in:
            if (patch_vert_idx == -1)
               patch_vert_idx = num_system_values++;

            system_values[patch_vert_idx] =
               BRW_PARAM_BUILTIN_PATCH_VERTICES_IN;

            b.cursor = nir_before_instr(instr);
            offset = nir_imm_int(&b, patch_vert_idx * sizeof(uint32_t));
            break;
         case nir_intrinsic_load_tess_level_outer_default:
            if (tess_outer_default_idx == -1) {
               tess_outer_default_idx = num_system_values;
               num_system_values += 4;
            }

            for (int i = 0; i < 4; i++) {
               system_values[tess_outer_default_idx + i] =
                  BRW_PARAM_BUILTIN_TESS_LEVEL_OUTER_X + i;
            }

            b.cursor = nir_before_instr(instr);
            offset = nir_imm_int(&b, tess_outer_default_idx * sizeof(uint32_t));
            break;
         case nir_intrinsic_load_tess_level_inner_default:
            if (tess_inner_default_idx == -1) {
               tess_inner_default_idx = num_system_values;
               num_system_values += 2;
            }

            for (int i = 0; i < 2; i++) {
               system_values[tess_inner_default_idx + i] =
                  BRW_PARAM_BUILTIN_TESS_LEVEL_INNER_X + i;
            }

            b.cursor = nir_before_instr(instr);
            offset = nir_imm_int(&b, tess_inner_default_idx * sizeof(uint32_t));
            break;
         case nir_intrinsic_image_deref_load_param_intel: {
            assert(devinfo->ver < 9);
            nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
            nir_variable *var = nir_deref_instr_get_variable(deref);

            if (img_idx[var->data.binding] == -1) {
               /* GL only allows arrays of arrays of images. */
               assert(glsl_type_is_image(glsl_without_array(var->type)));
               unsigned num_images = MAX2(1, glsl_get_aoa_size(var->type));

               for (int i = 0; i < num_images; i++) {
                  const unsigned img = var->data.binding + i;

                  img_idx[img] = num_system_values;
                  num_system_values += ISL_IMAGE_PARAM_SIZE;

                  uint32_t *img_sv = &system_values[img_idx[img]];

                  setup_vec4_image_sysval(
                     img_sv + ISL_IMAGE_PARAM_OFFSET_OFFSET, img,
                     offsetof(struct isl_image_param, offset), 2);
                  setup_vec4_image_sysval(
                     img_sv + ISL_IMAGE_PARAM_SIZE_OFFSET, img,
                     offsetof(struct isl_image_param, size), 3);
                  setup_vec4_image_sysval(
                     img_sv + ISL_IMAGE_PARAM_STRIDE_OFFSET, img,
                     offsetof(struct isl_image_param, stride), 4);
                  setup_vec4_image_sysval(
                     img_sv + ISL_IMAGE_PARAM_TILING_OFFSET, img,
                     offsetof(struct isl_image_param, tiling), 3);
                  setup_vec4_image_sysval(
                     img_sv + ISL_IMAGE_PARAM_SWIZZLING_OFFSET, img,
                     offsetof(struct isl_image_param, swizzling), 2);
               }
            }

            b.cursor = nir_before_instr(instr);
            offset = nir_iadd_imm(&b,
               get_aoa_deref_offset(&b, deref, ISL_IMAGE_PARAM_SIZE * 4),
               img_idx[var->data.binding] * 4 +
               nir_intrinsic_base(intrin) * 16);
            break;
         }
         case nir_intrinsic_load_workgroup_size: {
            assert(nir->info.workgroup_size_variable);
            if (variable_group_size_idx == -1) {
               variable_group_size_idx = num_system_values;
               num_system_values += 3;
               for (int i = 0; i < 3; i++) {
                  system_values[variable_group_size_idx + i] =
                     BRW_PARAM_BUILTIN_WORK_GROUP_SIZE_X + i;
               }
            }

            b.cursor = nir_before_instr(instr);
            offset = nir_imm_int(&b, variable_group_size_idx * sizeof(uint32_t));
            break;
         }
         case nir_intrinsic_load_work_dim: {
            if (work_dim_idx == -1) {
               work_dim_idx = num_system_values++;
               system_values[work_dim_idx] = BRW_PARAM_BUILTIN_WORK_DIM;
            }
            b.cursor = nir_before_instr(instr);
            offset = nir_imm_int(&b, work_dim_idx * sizeof(uint32_t));
            break;
         }
         case nir_intrinsic_load_kernel_input: {
            b.cursor = nir_before_instr(instr);
            offset = nir_iadd_imm(&b, intrin->src[0].ssa,
                                      nir_intrinsic_base(intrin));
            break;
         }
         default:
            continue;
         }

         nir_def *load =
            nir_load_ubo(&b, intrin->def.num_components, intrin->def.bit_size,
                         temp_ubo_name, offset,
                         .align_mul = 4,
                         .align_offset = 0,
                         .range_base = 0,
                         .range = ~0);

         nir_def_rewrite_uses(&intrin->def,
                                  load);
         nir_instr_remove(instr);
      }
   }

   nir_validate_shader(nir, "before remapping");

   /* Uniforms are stored in constant buffer 0, the
    * user-facing UBOs are indexed by one.  So if any constant buffer is
    * needed, the constant buffer 0 will be needed, so account for it.
    */
   unsigned num_cbufs = nir->info.num_ubos;
   if (num_cbufs || nir->num_uniforms)
      num_cbufs++;

   /* Place the new params in a new cbuf. */
   if (num_system_values > 0) {
      unsigned sysval_cbuf_index = num_cbufs;
      num_cbufs++;

      system_values = reralloc(mem_ctx, system_values, unsigned,
                               num_system_values);

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);

            if (load->intrinsic != nir_intrinsic_load_ubo)
               continue;

            b.cursor = nir_before_instr(instr);

            if (load->src[0].ssa == temp_ubo_name) {
               nir_def *imm = nir_imm_int(&b, sysval_cbuf_index);
               nir_src_rewrite(&load->src[0], imm);
            }
         }
      }

      /* We need to fold the new iadds for brw_nir_analyze_ubo_ranges */
      nir_opt_constant_folding(nir);
   } else {
      ralloc_free(system_values);
      system_values = NULL;
   }

   assert(num_cbufs < PIPE_MAX_CONSTANT_BUFFERS);
   nir_validate_shader(nir, "after remap");

   /* We don't use params[] but gallium leaves num_uniforms set.  We use this
    * to detect when cbuf0 exists but we don't need it anymore when we get
    * here.  Instead, zero it out so that the back-end doesn't get confused
    * when nr_params * 4 != num_uniforms != nr_params * 4.
    */
   nir->num_uniforms = 0;

   *out_system_values = system_values;
   *out_num_system_values = num_system_values;
   *out_num_cbufs = num_cbufs;
}

static const char *surface_group_names[] = {
   [IRIS_SURFACE_GROUP_RENDER_TARGET]      = "render target",
   [IRIS_SURFACE_GROUP_RENDER_TARGET_READ] = "non-coherent render target read",
   [IRIS_SURFACE_GROUP_CS_WORK_GROUPS]     = "CS work groups",
   [IRIS_SURFACE_GROUP_TEXTURE_LOW64]      = "texture",
   [IRIS_SURFACE_GROUP_TEXTURE_HIGH64]     = "texture",
   [IRIS_SURFACE_GROUP_UBO]                = "ubo",
   [IRIS_SURFACE_GROUP_SSBO]               = "ssbo",
   [IRIS_SURFACE_GROUP_IMAGE]              = "image",
};

static void
iris_print_binding_table(FILE *fp, const char *name,
                         const struct iris_binding_table *bt)
{
   STATIC_ASSERT(ARRAY_SIZE(surface_group_names) == IRIS_SURFACE_GROUP_COUNT);

   uint32_t total = 0;
   uint32_t compacted = 0;

   for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++) {
      uint32_t size = bt->sizes[i];
      total += size;
      if (size)
         compacted += util_bitcount64(bt->used_mask[i]);
   }

   if (total == 0) {
      fprintf(fp, "Binding table for %s is empty\n\n", name);
      return;
   }

   if (total != compacted) {
      fprintf(fp, "Binding table for %s "
              "(compacted to %u entries from %u entries)\n",
              name, compacted, total);
   } else {
      fprintf(fp, "Binding table for %s (%u entries)\n", name, total);
   }

   uint32_t entry = 0;
   for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++) {
      uint64_t mask = bt->used_mask[i];
      while (mask) {
         int index = u_bit_scan64(&mask);
         fprintf(fp, "  [%u] %s #%d\n", entry++, surface_group_names[i], index);
      }
   }
   fprintf(fp, "\n");
}

enum {
   /* Max elements in a surface group. */
   SURFACE_GROUP_MAX_ELEMENTS = 64,
};

/**
 * Map a <group, index> pair to a binding table index.
 *
 * For example: <UBO, 5> => binding table index 12
 */
uint32_t
iris_group_index_to_bti(const struct iris_binding_table *bt,
                        enum iris_surface_group group, uint32_t index)
{
   assert(index < bt->sizes[group]);
   uint64_t mask = bt->used_mask[group];
   uint64_t bit = 1ull << index;
   if (bit & mask) {
      return bt->offsets[group] + util_bitcount64((bit - 1) & mask);
   } else {
      return IRIS_SURFACE_NOT_USED;
   }
}

/**
 * Map a binding table index back to a <group, index> pair.
 *
 * For example: binding table index 12 => <UBO, 5>
 */
uint32_t
iris_bti_to_group_index(const struct iris_binding_table *bt,
                        enum iris_surface_group group, uint32_t bti)
{
   uint64_t used_mask = bt->used_mask[group];
   assert(bti >= bt->offsets[group]);

   uint32_t c = bti - bt->offsets[group];
   while (used_mask) {
      int i = u_bit_scan64(&used_mask);
      if (c == 0)
         return i;
      c--;
   }

   return IRIS_SURFACE_NOT_USED;
}

static void
rewrite_src_with_bti(nir_builder *b, struct iris_binding_table *bt,
                     nir_instr *instr, nir_src *src,
                     enum iris_surface_group group)
{
   assert(bt->sizes[group] > 0);

   b->cursor = nir_before_instr(instr);
   nir_def *bti;
   if (nir_src_is_const(*src)) {
      uint32_t index = nir_src_as_uint(*src);
      bti = nir_imm_intN_t(b, iris_group_index_to_bti(bt, group, index),
                           src->ssa->bit_size);
   } else {
      /* Indirect usage makes all the surfaces of the group to be available,
       * so we can just add the base.
       */
      assert(bt->used_mask[group] == BITFIELD64_MASK(bt->sizes[group]));
      bti = nir_iadd_imm(b, src->ssa, bt->offsets[group]);
   }
   nir_src_rewrite(src, bti);
}

static void
mark_used_with_src(struct iris_binding_table *bt, nir_src *src,
                   enum iris_surface_group group)
{
   assert(bt->sizes[group] > 0);

   if (nir_src_is_const(*src)) {
      uint64_t index = nir_src_as_uint(*src);
      assert(index < bt->sizes[group]);
      bt->used_mask[group] |= 1ull << index;
   } else {
      /* There's an indirect usage, we need all the surfaces. */
      bt->used_mask[group] = BITFIELD64_MASK(bt->sizes[group]);
   }
}

static bool
skip_compacting_binding_tables(void)
{
   static int skip = -1;
   if (skip < 0)
      skip = debug_get_bool_option("INTEL_DISABLE_COMPACT_BINDING_TABLE", false);
   return skip;
}

/**
 * Set up the binding table indices and apply to the shader.
 */
static void
iris_setup_binding_table(const struct intel_device_info *devinfo,
                         struct nir_shader *nir,
                         struct iris_binding_table *bt,
                         unsigned num_render_targets,
                         unsigned num_system_values,
                         unsigned num_cbufs,
                         bool use_null_rt)
{
   const struct shader_info *info = &nir->info;

   memset(bt, 0, sizeof(*bt));

   /* Set the sizes for each surface group.  For some groups, we already know
    * upfront how many will be used, so mark them.
    */
   if (info->stage == MESA_SHADER_FRAGMENT) {
      bt->sizes[IRIS_SURFACE_GROUP_RENDER_TARGET] = num_render_targets;
      /* All render targets used. */
      bt->used_mask[IRIS_SURFACE_GROUP_RENDER_TARGET] =
         BITFIELD64_MASK(num_render_targets);

      /* Setup render target read surface group in order to support non-coherent
       * framebuffer fetch on Gfx8
       */
      if (devinfo->ver == 8 && info->outputs_read) {
         bt->sizes[IRIS_SURFACE_GROUP_RENDER_TARGET_READ] = num_render_targets;
         bt->used_mask[IRIS_SURFACE_GROUP_RENDER_TARGET_READ] =
            BITFIELD64_MASK(num_render_targets);
      }

      bt->use_null_rt = use_null_rt;
   } else if (info->stage == MESA_SHADER_COMPUTE) {
      bt->sizes[IRIS_SURFACE_GROUP_CS_WORK_GROUPS] = 1;
   }

   assert(ARRAY_SIZE(info->textures_used) >= 4);
   int max_tex = BITSET_LAST_BIT(info->textures_used);
   assert(max_tex <= 128);
   bt->sizes[IRIS_SURFACE_GROUP_TEXTURE_LOW64] = MIN2(64, max_tex);
   bt->sizes[IRIS_SURFACE_GROUP_TEXTURE_HIGH64] = MAX2(0, max_tex - 64);
   bt->used_mask[IRIS_SURFACE_GROUP_TEXTURE_LOW64] =
      info->textures_used[0] | ((uint64_t)info->textures_used[1]) << 32;
   bt->used_mask[IRIS_SURFACE_GROUP_TEXTURE_HIGH64] =
      info->textures_used[2] | ((uint64_t)info->textures_used[3]) << 32;
   bt->samplers_used_mask = info->samplers_used[0];

   bt->sizes[IRIS_SURFACE_GROUP_IMAGE] = BITSET_LAST_BIT(info->images_used);

   /* Allocate an extra slot in the UBO section for NIR constants.
    * Binding table compaction will remove it if unnecessary.
    *
    * We don't include them in iris_compiled_shader::num_cbufs because
    * they are uploaded separately from shs->constbuf[], but from a shader
    * point of view, they're another UBO (at the end of the section).
    */
   bt->sizes[IRIS_SURFACE_GROUP_UBO] = num_cbufs + 1;

   bt->sizes[IRIS_SURFACE_GROUP_SSBO] = info->num_ssbos;

   for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++)
      assert(bt->sizes[i] <= SURFACE_GROUP_MAX_ELEMENTS);

   /* Mark surfaces used for the cases we don't have the information available
    * upfront.
    */
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block (block, impl) {
      nir_foreach_instr (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_load_num_workgroups:
            bt->used_mask[IRIS_SURFACE_GROUP_CS_WORK_GROUPS] = 1;
            break;

         case nir_intrinsic_load_output:
            if (devinfo->ver == 8) {
               mark_used_with_src(bt, &intrin->src[0],
                                  IRIS_SURFACE_GROUP_RENDER_TARGET_READ);
            }
            break;

         case nir_intrinsic_image_size:
         case nir_intrinsic_image_load:
         case nir_intrinsic_image_store:
         case nir_intrinsic_image_atomic:
         case nir_intrinsic_image_atomic_swap:
         case nir_intrinsic_image_load_raw_intel:
         case nir_intrinsic_image_store_raw_intel:
            mark_used_with_src(bt, &intrin->src[0], IRIS_SURFACE_GROUP_IMAGE);
            break;

         case nir_intrinsic_load_ubo:
            mark_used_with_src(bt, &intrin->src[0], IRIS_SURFACE_GROUP_UBO);
            break;

         case nir_intrinsic_store_ssbo:
            mark_used_with_src(bt, &intrin->src[1], IRIS_SURFACE_GROUP_SSBO);
            break;

         case nir_intrinsic_get_ssbo_size:
         case nir_intrinsic_ssbo_atomic:
         case nir_intrinsic_ssbo_atomic_swap:
         case nir_intrinsic_load_ssbo:
            mark_used_with_src(bt, &intrin->src[0], IRIS_SURFACE_GROUP_SSBO);
            break;

         default:
            break;
         }
      }
   }

   /* When disable we just mark everything as used. */
   if (unlikely(skip_compacting_binding_tables())) {
      for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++)
         bt->used_mask[i] = BITFIELD64_MASK(bt->sizes[i]);
   }

   /* Calculate the offsets and the binding table size based on the used
    * surfaces.  After this point, the functions to go between "group indices"
    * and binding table indices can be used.
    */
   uint32_t next = 0;
   for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++) {
      if (bt->used_mask[i] != 0) {
         bt->offsets[i] = next;
         next += util_bitcount64(bt->used_mask[i]);
      }
   }
   bt->size_bytes = next * 4;

   if (INTEL_DEBUG(DEBUG_BT)) {
      iris_print_binding_table(stderr, gl_shader_stage_name(info->stage), bt);
   }

   /* Apply the binding table indices.  The backend compiler is not expected
    * to change those, as we haven't set any of the *_start entries in brw
    * binding_table.
    */
   nir_builder b = nir_builder_create(impl);

   nir_foreach_block (block, impl) {
      nir_foreach_instr (instr, block) {
         if (instr->type == nir_instr_type_tex) {
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (tex->texture_index < 64) {
               tex->texture_index =
                  iris_group_index_to_bti(bt, IRIS_SURFACE_GROUP_TEXTURE_LOW64,
                                          tex->texture_index);
            } else {
               tex->texture_index =
                  iris_group_index_to_bti(bt, IRIS_SURFACE_GROUP_TEXTURE_HIGH64,
                                          tex->texture_index - 64);
            }
            continue;
         }

         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_image_size:
         case nir_intrinsic_image_load:
         case nir_intrinsic_image_store:
         case nir_intrinsic_image_atomic:
         case nir_intrinsic_image_atomic_swap:
         case nir_intrinsic_image_load_raw_intel:
         case nir_intrinsic_image_store_raw_intel:
            rewrite_src_with_bti(&b, bt, instr, &intrin->src[0],
                                 IRIS_SURFACE_GROUP_IMAGE);
            break;

         case nir_intrinsic_load_ubo:
            rewrite_src_with_bti(&b, bt, instr, &intrin->src[0],
                                 IRIS_SURFACE_GROUP_UBO);
            break;

         case nir_intrinsic_store_ssbo:
            rewrite_src_with_bti(&b, bt, instr, &intrin->src[1],
                                 IRIS_SURFACE_GROUP_SSBO);
            break;

         case nir_intrinsic_load_output:
            if (devinfo->ver == 8) {
               rewrite_src_with_bti(&b, bt, instr, &intrin->src[0],
                                    IRIS_SURFACE_GROUP_RENDER_TARGET_READ);
            }
            break;

         case nir_intrinsic_get_ssbo_size:
         case nir_intrinsic_ssbo_atomic:
         case nir_intrinsic_ssbo_atomic_swap:
         case nir_intrinsic_load_ssbo:
            rewrite_src_with_bti(&b, bt, instr, &intrin->src[0],
                                 IRIS_SURFACE_GROUP_SSBO);
            break;

         default:
            break;
         }
      }
   }
}

static void
iris_debug_recompile_brw(struct iris_screen *screen,
                         struct util_debug_callback *dbg,
                         struct iris_uncompiled_shader *ish,
                         const struct brw_base_prog_key *key)
{
   if (!ish || list_is_empty(&ish->variants)
            || list_is_singular(&ish->variants))
      return;

   const struct brw_compiler *c = screen->brw;
   const struct shader_info *info = &ish->nir->info;

   brw_shader_perf_log(c, dbg, "Recompiling %s shader for program %s: %s\n",
                       _mesa_shader_stage_to_string(info->stage),
                       info->name ? info->name : "(no identifier)",
                       info->label ? info->label : "");

   struct iris_compiled_shader *shader =
      list_first_entry(&ish->variants, struct iris_compiled_shader, link);
   const void *old_iris_key = &shader->key;

   union brw_any_prog_key old_key;

   switch (info->stage) {
   case MESA_SHADER_VERTEX:
      old_key.vs = iris_to_brw_vs_key(screen, old_iris_key);
      break;
   case MESA_SHADER_TESS_CTRL:
      old_key.tcs = iris_to_brw_tcs_key(screen, old_iris_key);
      break;
   case MESA_SHADER_TESS_EVAL:
      old_key.tes = iris_to_brw_tes_key(screen, old_iris_key);
      break;
   case MESA_SHADER_GEOMETRY:
      old_key.gs = iris_to_brw_gs_key(screen, old_iris_key);
      break;
   case MESA_SHADER_FRAGMENT:
      old_key.wm = iris_to_brw_fs_key(screen, old_iris_key);
      break;
   case MESA_SHADER_COMPUTE:
      old_key.cs = iris_to_brw_cs_key(screen, old_iris_key);
      break;
   default:
      UNREACHABLE("invalid shader stage");
   }

   brw_debug_key_recompile(c, dbg, info->stage, &old_key.base, key);
}

#ifdef INTEL_USE_ELK

static void
iris_debug_recompile_elk(struct iris_screen *screen,
                         struct util_debug_callback *dbg,
                         struct iris_uncompiled_shader *ish,
                         const struct elk_base_prog_key *key)
{
   if (!ish || list_is_empty(&ish->variants)
            || list_is_singular(&ish->variants))
      return;

   const struct elk_compiler *c = screen->elk;
   const struct shader_info *info = &ish->nir->info;

   elk_shader_perf_log(c, dbg, "Recompiling %s shader for program %s: %s\n",
                       _mesa_shader_stage_to_string(info->stage),
                       info->name ? info->name : "(no identifier)",
                       info->label ? info->label : "");

   struct iris_compiled_shader *shader =
      list_first_entry(&ish->variants, struct iris_compiled_shader, link);
   const void *old_iris_key = &shader->key;

   union elk_any_prog_key old_key;

   switch (info->stage) {
   case MESA_SHADER_VERTEX:
      old_key.vs = iris_to_elk_vs_key(screen, old_iris_key);
      break;
   case MESA_SHADER_TESS_CTRL:
      old_key.tcs = iris_to_elk_tcs_key(screen, old_iris_key);
      break;
   case MESA_SHADER_TESS_EVAL:
      old_key.tes = iris_to_elk_tes_key(screen, old_iris_key);
      break;
   case MESA_SHADER_GEOMETRY:
      old_key.gs = iris_to_elk_gs_key(screen, old_iris_key);
      break;
   case MESA_SHADER_FRAGMENT:
      old_key.wm = iris_to_elk_fs_key(screen, old_iris_key);
      break;
   case MESA_SHADER_COMPUTE:
      old_key.cs = iris_to_elk_cs_key(screen, old_iris_key);
      break;
   default:
      UNREACHABLE("invalid shader stage");
   }

   elk_debug_key_recompile(c, dbg, info->stage, &old_key.base, key);
}

#endif

static void
check_urb_size(struct iris_context *ice,
               unsigned needed_size,
               gl_shader_stage stage)
{
   unsigned last_allocated_size = ice->shaders.urb.cfg.size[stage];

   /* If the last URB allocation wasn't large enough for our needs,
    * flag it as needing to be reconfigured.  Otherwise, we can use
    * the existing config.  However, if the URB is constrained, and
    * we can shrink our size for this stage, we may be able to gain
    * extra concurrency by reconfiguring it to be smaller.  Do so.
    */
   if (last_allocated_size < needed_size ||
       (ice->shaders.urb.constrained && last_allocated_size > needed_size)) {
      ice->state.dirty |= IRIS_DIRTY_URB;
   }
}

/**
 * Get the shader for the last enabled geometry stage.
 *
 * This stage is the one which will feed stream output and the rasterizer.
 */
static gl_shader_stage
last_vue_stage(struct iris_context *ice)
{
   if (ice->shaders.uncompiled[MESA_SHADER_GEOMETRY])
      return MESA_SHADER_GEOMETRY;

   if (ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL])
      return MESA_SHADER_TESS_EVAL;

   return MESA_SHADER_VERTEX;
}

/**
 * \param added  Set to \c true if the variant was added to the list (i.e., a
 *               variant matching \c key was not found).  Set to \c false
 *               otherwise.
 */
static inline struct iris_compiled_shader *
find_or_add_variant(const struct iris_screen *screen,
                    struct iris_uncompiled_shader *ish,
                    enum iris_program_cache_id cache_id,
                    const void *key, unsigned key_size,
                    bool *added)
{
   struct list_head *start = ish->variants.next;

   *added = false;

   if (screen->precompile) {
      /* Check the first list entry.  There will always be at least one
       * variant in the list (most likely the precompile variant), and
       * other contexts only append new variants, so we can safely check
       * it without locking, saving that cost in the common case.
       */
      struct iris_compiled_shader *first =
         list_first_entry(&ish->variants, struct iris_compiled_shader, link);

      if (memcmp(&first->key, key, key_size) == 0) {
         util_queue_fence_wait(&first->ready);
         return first;
      }

      /* Skip this one in the loop below */
      start = first->link.next;
   }

   struct iris_compiled_shader *variant = NULL;

   /* If it doesn't match, we have to walk the list; other contexts may be
    * concurrently appending shaders to it, so we need to lock here.
    */
   simple_mtx_lock(&ish->lock);

   list_for_each_entry_from(struct iris_compiled_shader, v, start,
                            &ish->variants, link) {
      if (memcmp(&v->key, key, key_size) == 0) {
         variant = v;
         break;
      }
   }

   gl_shader_stage stage = ish->nir->info.stage;

   if (variant == NULL) {
      variant = iris_create_shader_variant(screen, NULL, stage, cache_id,
                                           key_size, key);

      /* Append our new variant to the shader's variant list. */
      list_addtail(&variant->link, &ish->variants);
      *added = true;

      simple_mtx_unlock(&ish->lock);
   } else {
      simple_mtx_unlock(&ish->lock);

      util_queue_fence_wait(&variant->ready);
   }

   assert(stage == variant->stage);
   return variant;
}

static void
iris_threaded_compile_job_delete(void *_job, UNUSED void *_gdata,
                                 UNUSED int thread_index)
{
   free(_job);
}

static void
iris_schedule_compile(struct iris_screen *screen,
                      struct util_queue_fence *ready_fence,
                      struct util_debug_callback *dbg,
                      struct iris_threaded_compile_job *job,
                      util_queue_execute_func execute)

{
   struct util_async_debug_callback async_debug;

   if (dbg) {
      u_async_debug_init(&async_debug);
      job->dbg = &async_debug.base;
   }

   util_queue_add_job(&screen->shader_compiler_queue, job, ready_fence, execute,
                      iris_threaded_compile_job_delete, 0);

   if (screen->driconf.sync_compile || dbg)
      util_queue_fence_wait(ready_fence);

   if (dbg) {
      u_async_debug_drain(&async_debug, dbg);
      u_async_debug_cleanup(&async_debug);
   }
}

/**
 * Compile a vertex shader, and upload the assembly.
 */
static void
iris_compile_vs(struct iris_screen *screen,
                struct u_upload_mgr *uploader,
                struct util_debug_callback *dbg,
                struct iris_uncompiled_shader *ish,
                struct iris_compiled_shader *shader)
{
   const struct intel_device_info *devinfo = screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   uint32_t *system_values;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);
   const struct iris_vs_prog_key *const key = &shader->key.vs;

   if (key->vue.nr_userclip_plane_consts) {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      /* Check if variables were found. */
      if (nir_lower_clip_vs(nir, (1 << key->vue.nr_userclip_plane_consts) - 1,
                            true, false, NULL)) {
         nir_lower_io_vars_to_temporaries(nir, impl, true, false);
         nir_lower_global_vars_to_local(nir);
         nir_lower_vars_to_ssa(nir);
         nir_shader_gather_info(nir, impl);
      }
   }

   iris_setup_uniforms(devinfo, mem_ctx, nir, &system_values,
                       &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(devinfo, nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs, false);

   const char *error;
   const unsigned *program;
   if (screen->brw) {
      struct brw_vs_prog_data *brw_prog_data =
         rzalloc(mem_ctx, struct brw_vs_prog_data);

      brw_prog_data->base.base.use_alt_mode = nir->info.use_legacy_math_rules;

      brw_nir_analyze_ubo_ranges(screen->brw, nir, brw_prog_data->base.base.ubo_ranges);

      brw_compute_vue_map(devinfo,
                          &brw_prog_data->base.vue_map, nir->info.outputs_written,
                          key->vue.layout, /* pos_slots */ 1);

      struct brw_vs_prog_key brw_key = iris_to_brw_vs_key(screen, key);

      struct brw_compile_vs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &brw_key,
         .prog_data = brw_prog_data,
      };

      program = brw_compile_vs(screen->brw, &params);
      error = params.base.error_str;
      if (program) {
         iris_apply_brw_prog_data(shader, &brw_prog_data->base.base);
         iris_debug_recompile_brw(screen, dbg, ish, &brw_key.base);
      }
   } else {
#ifdef INTEL_USE_ELK
      struct elk_vs_prog_data *elk_prog_data =
         rzalloc(mem_ctx, struct elk_vs_prog_data);

      elk_prog_data->base.base.use_alt_mode = nir->info.use_legacy_math_rules;

      elk_nir_analyze_ubo_ranges(screen->elk, nir, elk_prog_data->base.base.ubo_ranges);

      elk_compute_vue_map(devinfo,
                          &elk_prog_data->base.vue_map, nir->info.outputs_written,
                          nir->info.separate_shader ?
                          INTEL_VUE_LAYOUT_SEPARATE :
                          INTEL_VUE_LAYOUT_FIXED, /* pos_slots */ 1);

      struct elk_vs_prog_key elk_key = iris_to_elk_vs_key(screen, key);

      struct elk_compile_vs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &elk_key,
         .prog_data = elk_prog_data,
      };

      program = elk_compile_vs(screen->elk, &params);
      error = params.base.error_str;
      if (program) {
         iris_debug_recompile_elk(screen, dbg, ish, &elk_key.base);
         iris_apply_elk_prog_data(shader, &elk_prog_data->base.base);
      }
#else
      UNREACHABLE("no elk support");
#endif
   }

   if (program == NULL) {
      dbg_printf("Failed to compile vertex shader: %s\n", error);
      ralloc_free(mem_ctx);

      shader->compilation_failed = true;
      util_queue_fence_signal(&shader->ready);

      return;
   }

   shader->compilation_failed = false;

   uint32_t *so_decls =
      screen->vtbl.create_so_decl_list(&ish->stream_output,
                                       &iris_vue_data(shader)->vue_map);

   iris_finalize_program(shader, so_decls,
                         system_values, num_system_values, num_cbufs, &bt);

   iris_upload_shader(screen, ish, shader, NULL, uploader, IRIS_CACHE_VS,
                      sizeof(*key), key, program);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
}

/**
 * Update the current vertex shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_vs(struct iris_context *ice)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_VERTEX];
   struct u_upload_mgr *uploader = ice->shaders.uploader_driver;
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_VERTEX];

   struct iris_vs_prog_key key = {
      KEY_INIT(vue.base),
      .vue.layout = vue_layout(ish->nir->info.separate_shader),
   };
   screen->vtbl.populate_vs_key(ice, &ish->nir->info, last_vue_stage(ice), &key);

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_VS];
   bool added;
   struct iris_compiled_shader *shader =
      find_or_add_variant(screen, ish, IRIS_CACHE_VS, &key, sizeof(key), &added);

   if (added && !iris_disk_cache_retrieve(screen, uploader, ish, shader,
                                          &key, sizeof(key))) {
      iris_compile_vs(screen, uploader, &ice->dbg, ish, shader);
   }

   if (shader->compilation_failed)
      shader = NULL;

   if (old != shader) {
      iris_shader_variant_reference(&ice->shaders.prog[MESA_SHADER_VERTEX],
                                    shader);
      ice->state.dirty |= IRIS_DIRTY_VF_SGVS;
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_VS |
                                IRIS_STAGE_DIRTY_BINDINGS_VS |
                                IRIS_STAGE_DIRTY_CONSTANTS_VS;
      shs->sysvals_need_upload = true;

      unsigned urb_entry_size = shader ?
         iris_vue_data(shader)->urb_entry_size : 0;
      check_urb_size(ice, urb_entry_size, MESA_SHADER_VERTEX);
   }
}

/**
 * Get the shader_info for a given stage, or NULL if the stage is disabled.
 */
const struct shader_info *
iris_get_shader_info(const struct iris_context *ice, gl_shader_stage stage)
{
   const struct iris_uncompiled_shader *ish = ice->shaders.uncompiled[stage];

   if (!ish)
      return NULL;

   const nir_shader *nir = ish->nir;
   return &nir->info;
}

/**
 * Get the union of TCS output and TES input slots.
 *
 * TCS and TES need to agree on a common URB entry layout.  In particular,
 * the data for all patch vertices is stored in a single URB entry (unlike
 * GS which has one entry per input vertex).  This means that per-vertex
 * array indexing needs a stride.
 *
 * SSO requires locations to match, but doesn't require the number of
 * outputs/inputs to match (in fact, the TCS often has extra outputs).
 * So, we need to take the extra step of unifying these on the fly.
 */
static void
get_unified_tess_slots(const struct iris_context *ice,
                       uint64_t *per_vertex_slots,
                       uint32_t *per_patch_slots)
{
   const struct shader_info *tcs =
      iris_get_shader_info(ice, MESA_SHADER_TESS_CTRL);
   const struct shader_info *tes =
      iris_get_shader_info(ice, MESA_SHADER_TESS_EVAL);

   *per_vertex_slots = tes->inputs_read;
   *per_patch_slots = tes->patch_inputs_read;

   if (tcs) {
      *per_vertex_slots |= tcs->outputs_written;
      *per_patch_slots |= tcs->patch_outputs_written;
   }
}

/**
 * Compile a tessellation control shader, and upload the assembly.
 */
static void
iris_compile_tcs(struct iris_screen *screen,
                 struct hash_table *passthrough_ht,
                 struct u_upload_mgr *uploader,
                 struct util_debug_callback *dbg,
                 struct iris_uncompiled_shader *ish,
                 struct iris_compiled_shader *shader)
{
   void *mem_ctx = ralloc_context(NULL);
   const struct intel_device_info *devinfo = screen->devinfo;
   uint32_t *system_values = NULL;
   unsigned num_system_values = 0;
   unsigned num_cbufs = 0;

   nir_shader *nir;

   struct iris_binding_table bt;

   const struct iris_tcs_prog_key *const key = &shader->key.tcs;
   struct brw_tcs_prog_key brw_key = iris_to_brw_tcs_key(screen, key);
#ifdef INTEL_USE_ELK
   struct elk_tcs_prog_key elk_key = iris_to_elk_tcs_key(screen, key);
#endif
   uint32_t source_hash;

   if (ish) {
      nir = nir_shader_clone(mem_ctx, ish->nir);
      source_hash = ish->source_hash;
   } else {
      if (screen->brw) {
         nir = brw_nir_create_passthrough_tcs(mem_ctx, screen->brw, &brw_key);
      } else {
#ifdef INTEL_USE_ELK
         assert(screen->elk);
         nir = elk_nir_create_passthrough_tcs(mem_ctx, screen->elk, &elk_key);
#else
         UNREACHABLE("no elk support");
#endif
      }
      source_hash = *(uint32_t*)nir->info.source_blake3;
   }

   iris_setup_uniforms(devinfo, mem_ctx, nir, &system_values,
                       &num_system_values, &num_cbufs);
   iris_setup_binding_table(devinfo, nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs, false);

   const char *error = NULL;
   const unsigned *program;
   if (screen->brw) {
      struct brw_tcs_prog_data *brw_prog_data =
         rzalloc(mem_ctx, struct brw_tcs_prog_data);
      brw_nir_analyze_ubo_ranges(screen->brw, nir, brw_prog_data->base.base.ubo_ranges);

      struct brw_compile_tcs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = source_hash,
         },
         .key = &brw_key,
         .prog_data = brw_prog_data,
      };

      program = brw_compile_tcs(screen->brw, &params);
      error = params.base.error_str;

      if (program) {
         iris_apply_brw_prog_data(shader, &brw_prog_data->base.base);
         iris_debug_recompile_brw(screen, dbg, ish, &brw_key.base);
      }
   } else {
#ifdef INTEL_USE_ELK
      assert(screen->elk);
      struct elk_tcs_prog_data *elk_prog_data =
         rzalloc(mem_ctx, struct elk_tcs_prog_data);
      elk_nir_analyze_ubo_ranges(screen->elk, nir, elk_prog_data->base.base.ubo_ranges);

      struct elk_compile_tcs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = source_hash,
         },
         .key = &elk_key,
         .prog_data = elk_prog_data,
      };

      program = elk_compile_tcs(screen->elk, &params);
      error = params.base.error_str;

      if (program) {
         iris_debug_recompile_elk(screen, dbg, ish, &elk_key.base);
         iris_apply_elk_prog_data(shader, &elk_prog_data->base.base);
      }
#else
      UNREACHABLE("no elk support");
#endif
   }

   if (program == NULL) {
      dbg_printf("Failed to compile control shader: %s\n", error);
      ralloc_free(mem_ctx);

      shader->compilation_failed = true;
      util_queue_fence_signal(&shader->ready);

      return;
   }

   shader->compilation_failed = false;

   iris_finalize_program(shader, NULL, system_values,
                         num_system_values, num_cbufs, &bt);

   iris_upload_shader(screen, ish, shader, passthrough_ht, uploader,
                      IRIS_CACHE_TCS, sizeof(*key), key, program);

   if (ish)
      iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
}

/**
 * Update the current tessellation control shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_tcs(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_TESS_CTRL];
   struct iris_uncompiled_shader *tcs =
      ice->shaders.uncompiled[MESA_SHADER_TESS_CTRL];
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   struct u_upload_mgr *uploader = ice->shaders.uploader_driver;
   const struct intel_device_info *devinfo = screen->devinfo;

   const struct shader_info *tes_info =
      iris_get_shader_info(ice, MESA_SHADER_TESS_EVAL);
   struct iris_tcs_prog_key key = {
      .vue.base.program_string_id = tcs ? tcs->program_id : 0,
      .vue.layout = vue_layout(tcs ? tcs->nir->info.separate_shader : false),
      ._tes_primitive_mode = tes_info->tess._primitive_mode,
      .input_vertices =
         !tcs || iris_use_tcs_multi_patch(screen) ? ice->state.vertices_per_patch : 0,
      .quads_workaround = devinfo->ver < 9 &&
                          tes_info->tess._primitive_mode == TESS_PRIMITIVE_QUADS &&
                          tes_info->tess.spacing == TESS_SPACING_EQUAL,
   };
   get_unified_tess_slots(ice, &key.outputs_written,
                          &key.patch_outputs_written);
   screen->vtbl.populate_tcs_key(ice, &key);

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_TCS];
   struct iris_compiled_shader *shader;
   bool added = false;

   if (tcs != NULL) {
      shader = find_or_add_variant(screen, tcs, IRIS_CACHE_TCS, &key,
                                   sizeof(key), &added);
   } else {
      /* Look for and possibly create a passthrough TCS */
      shader = iris_find_cached_shader(ice, IRIS_CACHE_TCS, sizeof(key), &key);


      if (shader == NULL) {
         shader = iris_create_shader_variant(screen, ice->shaders.cache,
                                             MESA_SHADER_TESS_CTRL,
                                             IRIS_CACHE_TCS, sizeof(key), &key);
         added = true;
      }

   }

   /* If the shader was not found in (whichever cache), call iris_compile_tcs
    * if either ish is NULL or the shader could not be found in the disk
    * cache.
    */
   if (added &&
       (tcs == NULL || !iris_disk_cache_retrieve(screen, uploader, tcs, shader,
                                                 &key, sizeof(key)))) {
      iris_compile_tcs(screen, ice->shaders.cache, uploader, &ice->dbg, tcs,
                       shader);
   }

   if (shader->compilation_failed)
      shader = NULL;

   if (old != shader) {
      iris_shader_variant_reference(&ice->shaders.prog[MESA_SHADER_TESS_CTRL],
                                    shader);
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_TCS |
                                IRIS_STAGE_DIRTY_BINDINGS_TCS |
                                IRIS_STAGE_DIRTY_CONSTANTS_TCS;
      shs->sysvals_need_upload = true;

      unsigned urb_entry_size = shader ?
         iris_vue_data(shader)->urb_entry_size : 0;
      check_urb_size(ice, urb_entry_size, MESA_SHADER_TESS_CTRL);
   }
}

/**
 * Compile a tessellation evaluation shader, and upload the assembly.
 */
static void
iris_compile_tes(struct iris_screen *screen,
                 struct u_upload_mgr *uploader,
                 struct util_debug_callback *dbg,
                 struct iris_uncompiled_shader *ish,
                 struct iris_compiled_shader *shader)
{
   void *mem_ctx = ralloc_context(NULL);
   uint32_t *system_values;
   const struct intel_device_info *devinfo = screen->devinfo;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);
   const struct iris_tes_prog_key *const key = &shader->key.tes;

   if (key->vue.nr_userclip_plane_consts) {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      nir_lower_clip_vs(nir, (1 << key->vue.nr_userclip_plane_consts) - 1,
                        true, false, NULL);
      nir_lower_io_vars_to_temporaries(nir, impl, true, false);
      nir_lower_global_vars_to_local(nir);
      nir_lower_vars_to_ssa(nir);
      nir_shader_gather_info(nir, impl);
   }

   iris_setup_uniforms(devinfo, mem_ctx, nir, &system_values,
                       &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(devinfo, nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs, false);

   const char *error;
   const unsigned *program;

   if (screen->brw) {
      struct brw_tes_prog_data *brw_prog_data =
         rzalloc(mem_ctx, struct brw_tes_prog_data);

      brw_nir_analyze_ubo_ranges(screen->brw, nir, brw_prog_data->base.base.ubo_ranges);

      struct intel_vue_map input_vue_map;
      brw_compute_tess_vue_map(&input_vue_map, key->inputs_read,
                               key->patch_inputs_read);

      struct brw_tes_prog_key brw_key = iris_to_brw_tes_key(screen, key);

      struct brw_compile_tes_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &brw_key,
         .prog_data = brw_prog_data,
         .input_vue_map = &input_vue_map,
      };

      program = brw_compile_tes(screen->brw, &params);
      error = params.base.error_str;

      if (program) {
         iris_debug_recompile_brw(screen, dbg, ish, &brw_key.base);
         iris_apply_brw_prog_data(shader, &brw_prog_data->base.base);
      }
   } else {
#ifdef INTEL_USE_ELK
      struct elk_tes_prog_data *elk_prog_data =
         rzalloc(mem_ctx, struct elk_tes_prog_data);

      elk_nir_analyze_ubo_ranges(screen->elk, nir, elk_prog_data->base.base.ubo_ranges);

      struct intel_vue_map input_vue_map;
      elk_compute_tess_vue_map(&input_vue_map, key->inputs_read,
                               key->patch_inputs_read);

      struct elk_tes_prog_key elk_key = iris_to_elk_tes_key(screen, key);

      struct elk_compile_tes_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &elk_key,
         .prog_data = elk_prog_data,
         .input_vue_map = &input_vue_map,
      };

      program = elk_compile_tes(screen->elk, &params);
      error = params.base.error_str;

      if (program) {
         iris_debug_recompile_elk(screen, dbg, ish, &elk_key.base);
         iris_apply_elk_prog_data(shader, &elk_prog_data->base.base);
      }
#else
      UNREACHABLE("no elk support");
#endif
   }

   if (program == NULL) {
      dbg_printf("Failed to compile evaluation shader: %s\n", error);
      ralloc_free(mem_ctx);

      shader->compilation_failed = true;
      util_queue_fence_signal(&shader->ready);

      return;
   }

   shader->compilation_failed = false;

   uint32_t *so_decls =
      screen->vtbl.create_so_decl_list(&ish->stream_output,
                                       &iris_vue_data(shader)->vue_map);

   iris_finalize_program(shader, so_decls, system_values,
                         num_system_values, num_cbufs, &bt);

   iris_upload_shader(screen, ish, shader, NULL, uploader, IRIS_CACHE_TES,
                      sizeof(*key), key, program);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
}

/**
 * Update the current tessellation evaluation shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_tes(struct iris_context *ice)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   struct u_upload_mgr *uploader = ice->shaders.uploader_driver;
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_TESS_EVAL];
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL];

   struct iris_tes_prog_key key = {
      KEY_INIT(vue.base),
      .vue.layout = vue_layout(ish->nir->info.separate_shader),
   };
   get_unified_tess_slots(ice, &key.inputs_read, &key.patch_inputs_read);
   screen->vtbl.populate_tes_key(ice, &ish->nir->info, last_vue_stage(ice), &key);

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_TES];
   bool added;
   struct iris_compiled_shader *shader =
      find_or_add_variant(screen, ish, IRIS_CACHE_TES, &key, sizeof(key), &added);

   if (added && !iris_disk_cache_retrieve(screen, uploader, ish, shader,
                                          &key, sizeof(key))) {
      iris_compile_tes(screen, uploader, &ice->dbg, ish, shader);
   }

   if (shader->compilation_failed)
      shader = NULL;

   if (old != shader) {
      iris_shader_variant_reference(&ice->shaders.prog[MESA_SHADER_TESS_EVAL],
                                    shader);
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_TES |
                                IRIS_STAGE_DIRTY_BINDINGS_TES |
                                IRIS_STAGE_DIRTY_CONSTANTS_TES;
      shs->sysvals_need_upload = true;

      unsigned urb_entry_size = shader ?
         iris_vue_data(shader)->urb_entry_size : 0;
      check_urb_size(ice, urb_entry_size, MESA_SHADER_TESS_EVAL);
   }

   /* TODO: Could compare and avoid flagging this. */
   const struct shader_info *tes_info = &ish->nir->info;
   if (BITSET_TEST(tes_info->system_values_read, SYSTEM_VALUE_VERTICES_IN)) {
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_CONSTANTS_TES;
      ice->state.shaders[MESA_SHADER_TESS_EVAL].sysvals_need_upload = true;
   }
}

/**
 * Compile a geometry shader, and upload the assembly.
 */
static void
iris_compile_gs(struct iris_screen *screen,
                struct u_upload_mgr *uploader,
                struct util_debug_callback *dbg,
                struct iris_uncompiled_shader *ish,
                struct iris_compiled_shader *shader)
{
   const struct intel_device_info *devinfo = screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   uint32_t *system_values;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);
   const struct iris_gs_prog_key *const key = &shader->key.gs;

   if (key->vue.nr_userclip_plane_consts) {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      nir_lower_clip_gs(nir, (1 << key->vue.nr_userclip_plane_consts) - 1,
                        false, NULL);
      nir_lower_io_vars_to_temporaries(nir, impl, true, false);
      nir_lower_global_vars_to_local(nir);
      nir_lower_vars_to_ssa(nir);
      nir_shader_gather_info(nir, impl);
   }

   iris_setup_uniforms(devinfo, mem_ctx, nir, &system_values,
                       &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(devinfo, nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs, false);

   const char *error;
   const unsigned *program;
   if (screen->brw) {
      struct brw_gs_prog_data *brw_prog_data =
         rzalloc(mem_ctx, struct brw_gs_prog_data);

      brw_nir_analyze_ubo_ranges(screen->brw, nir, brw_prog_data->base.base.ubo_ranges);

      brw_compute_vue_map(devinfo,
                          &brw_prog_data->base.vue_map, nir->info.outputs_written,
                          key->vue.layout, /* pos_slots */ 1);

      struct brw_gs_prog_key brw_key = iris_to_brw_gs_key(screen, key);

      struct brw_compile_gs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &brw_key,
         .prog_data = brw_prog_data,
      };

      program = brw_compile_gs(screen->brw, &params);
      error = params.base.error_str;
      if (program) {
         iris_debug_recompile_brw(screen, dbg, ish, &brw_key.base);
         iris_apply_brw_prog_data(shader, &brw_prog_data->base.base);
      }
   } else {
#ifdef INTEL_USE_ELK
      struct elk_gs_prog_data *elk_prog_data =
         rzalloc(mem_ctx, struct elk_gs_prog_data);

      elk_nir_analyze_ubo_ranges(screen->elk, nir, elk_prog_data->base.base.ubo_ranges);

      elk_compute_vue_map(devinfo,
                          &elk_prog_data->base.vue_map, nir->info.outputs_written,
                          nir->info.separate_shader ?
                          INTEL_VUE_LAYOUT_SEPARATE :
                          INTEL_VUE_LAYOUT_FIXED, /* pos_slots */ 1);

      struct elk_gs_prog_key elk_key = iris_to_elk_gs_key(screen, key);

      struct elk_compile_gs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &elk_key,
         .prog_data = elk_prog_data,
      };

      program = elk_compile_gs(screen->elk, &params);
      error = params.base.error_str;
      if (program) {
         iris_debug_recompile_elk(screen, dbg, ish, &elk_key.base);
         iris_apply_elk_prog_data(shader, &elk_prog_data->base.base);
      }
#else
      UNREACHABLE("no elk support");
#endif
   }

   if (program == NULL) {
      dbg_printf("Failed to compile geometry shader: %s\n", error);
      ralloc_free(mem_ctx);

      shader->compilation_failed = true;
      util_queue_fence_signal(&shader->ready);

      return;
   }

   shader->compilation_failed = false;

   uint32_t *so_decls =
      screen->vtbl.create_so_decl_list(&ish->stream_output,
                                       &iris_vue_data(shader)->vue_map);

   iris_finalize_program(shader, so_decls, system_values,
                         num_system_values, num_cbufs, &bt);

   iris_upload_shader(screen, ish, shader, NULL, uploader, IRIS_CACHE_GS,
                      sizeof(*key), key, program);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
}

/**
 * Update the current geometry shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_gs(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_GEOMETRY];
   struct u_upload_mgr *uploader = ice->shaders.uploader_driver;
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_GEOMETRY];
   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_GS];
   struct iris_compiled_shader *shader = NULL;
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;

   if (ish) {
      struct iris_gs_prog_key key = {
         KEY_INIT(vue.base),
         .vue.layout = vue_layout(ish->nir->info.separate_shader),
      };
      screen->vtbl.populate_gs_key(ice, &ish->nir->info, last_vue_stage(ice), &key);

      bool added;

      shader = find_or_add_variant(screen, ish, IRIS_CACHE_GS, &key,
                                   sizeof(key), &added);

      if (added && !iris_disk_cache_retrieve(screen, uploader, ish, shader,
                                             &key, sizeof(key))) {
         iris_compile_gs(screen, uploader, &ice->dbg, ish, shader);
      }

      if (shader->compilation_failed)
         shader = NULL;
   }

   if (old != shader) {
      iris_shader_variant_reference(&ice->shaders.prog[MESA_SHADER_GEOMETRY],
                                    shader);
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_GS |
                                IRIS_STAGE_DIRTY_BINDINGS_GS |
                                IRIS_STAGE_DIRTY_CONSTANTS_GS;
      shs->sysvals_need_upload = true;

      unsigned urb_entry_size = shader ?
         iris_vue_data(shader)->urb_entry_size : 0;
      check_urb_size(ice, urb_entry_size, MESA_SHADER_GEOMETRY);
   }
}

/**
 * Compile a fragment (pixel) shader, and upload the assembly.
 */
static void
iris_compile_fs(struct iris_screen *screen,
                struct u_upload_mgr *uploader,
                struct util_debug_callback *dbg,
                struct iris_uncompiled_shader *ish,
                struct iris_compiled_shader *shader,
                struct intel_vue_map *vue_map)
{
   void *mem_ctx = ralloc_context(NULL);
   uint32_t *system_values;
   const struct intel_device_info *devinfo = screen->devinfo;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);
   const struct iris_fs_prog_key *const key = &shader->key.fs;

   iris_setup_uniforms(devinfo, mem_ctx, nir, &system_values,
                       &num_system_values, &num_cbufs);

   /* Lower output variables to load_output intrinsics before setting up
    * binding tables, so iris_setup_binding_table can map any load_output
    * intrinsics to IRIS_SURFACE_GROUP_RENDER_TARGET_READ on Gfx8 for
    * non-coherent framebuffer fetches.
    */
   brw_nir_lower_fs_outputs(nir);

   int null_rts = brw_nir_fs_needs_null_rt(devinfo, nir,
                                           key->alpha_to_coverage) ? 1 : 0;

   struct iris_binding_table bt;
   iris_setup_binding_table(devinfo, nir, &bt,
                            MAX2(key->nr_color_regions, null_rts),
                            num_system_values, num_cbufs, null_rts != 0);

   const char *error;
   const unsigned *program;

   if (screen->brw) {
      struct brw_wm_prog_data *brw_prog_data =
         rzalloc(mem_ctx, struct brw_wm_prog_data);

      brw_prog_data->base.use_alt_mode = nir->info.use_legacy_math_rules;

      brw_nir_analyze_ubo_ranges(screen->brw, nir, brw_prog_data->base.ubo_ranges);

      struct brw_wm_prog_key brw_key = iris_to_brw_fs_key(screen, key);

      struct brw_compile_fs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &brw_key,
         .prog_data = brw_prog_data,

         .allow_spilling = true,
         .max_polygons = UCHAR_MAX,
         .vue_map = vue_map,
      };

      program = brw_compile_fs(screen->brw, &params);
      error = params.base.error_str;
      if (program) {
         iris_debug_recompile_brw(screen, dbg, ish, &brw_key.base);
         iris_apply_brw_prog_data(shader, &brw_prog_data->base);
      }
   } else {
#ifdef INTEL_USE_ELK
      struct elk_wm_prog_data *elk_prog_data =
         rzalloc(mem_ctx, struct elk_wm_prog_data);

      elk_prog_data->base.use_alt_mode = nir->info.use_legacy_math_rules;

      elk_nir_analyze_ubo_ranges(screen->elk, nir, elk_prog_data->base.ubo_ranges);

      struct elk_wm_prog_key elk_key = iris_to_elk_fs_key(screen, key);

      struct elk_compile_fs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &elk_key,
         .prog_data = elk_prog_data,

         .allow_spilling = true,
         .max_polygons = UCHAR_MAX,
         .vue_map = vue_map,
      };

      program = elk_compile_fs(screen->elk, &params);
      error = params.base.error_str;
      if (program) {
         iris_debug_recompile_elk(screen, dbg, ish, &elk_key.base);
         iris_apply_elk_prog_data(shader, &elk_prog_data->base);
      }
#else
      UNREACHABLE("no elk support");
#endif
   }

   if (program == NULL) {
      dbg_printf("Failed to compile fragment shader: %s\n", error);
      ralloc_free(mem_ctx);

      shader->compilation_failed = true;
      util_queue_fence_signal(&shader->ready);

      return;
   }

   shader->compilation_failed = false;

   iris_finalize_program(shader, NULL, system_values,
                         num_system_values, num_cbufs, &bt);

   iris_upload_shader(screen, ish, shader, NULL, uploader, IRIS_CACHE_FS,
                      sizeof(*key), key, program);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
}

/**
 * Update the current fragment shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_fs(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_FRAGMENT];
   struct u_upload_mgr *uploader = ice->shaders.uploader_driver;
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_FRAGMENT];
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   struct iris_fs_prog_key key = {
      KEY_INIT(base),
      .vue_layout = vue_layout(ish->nir->info.separate_shader),
   };
   screen->vtbl.populate_fs_key(ice, &ish->nir->info, &key);

   struct intel_vue_map *last_vue_map =
      &iris_vue_data(ice->shaders.last_vue_shader)->vue_map;

   if (ish->nos & (1ull << IRIS_NOS_LAST_VUE_MAP))
      key.input_slots_valid = last_vue_map->slots_valid;

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_FS];
   bool added;
   struct iris_compiled_shader *shader =
      find_or_add_variant(screen, ish, IRIS_CACHE_FS, &key,
                          sizeof(key), &added);

   if (added && !iris_disk_cache_retrieve(screen, uploader, ish, shader,
                                          &key, sizeof(key))) {
      iris_compile_fs(screen, uploader, &ice->dbg, ish, shader, last_vue_map);
   }

   if (shader->compilation_failed)
      shader = NULL;

   if (old != shader) {
      // XXX: only need to flag CLIP if barycentric has NONPERSPECTIVE
      // toggles.  might be able to avoid flagging SBE too.
      iris_shader_variant_reference(&ice->shaders.prog[MESA_SHADER_FRAGMENT],
                                    shader);
      ice->state.dirty |= IRIS_DIRTY_WM |
                          IRIS_DIRTY_CLIP |
                          IRIS_DIRTY_SBE;
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_FS |
                                IRIS_STAGE_DIRTY_BINDINGS_FS |
                                IRIS_STAGE_DIRTY_CONSTANTS_FS;
      shs->sysvals_need_upload = true;
   }
}

/**
 * Update the last enabled stage's VUE map.
 *
 * When the shader feeding the rasterizer's output interface changes, we
 * need to re-emit various packets.
 */
static void
update_last_vue_map(struct iris_context *ice,
                    struct iris_compiled_shader *shader)
{
   const struct intel_vue_map *vue_map = &iris_vue_data(shader)->vue_map;
   const struct intel_vue_map *old_map =
      !ice->shaders.last_vue_shader ? NULL :
      &iris_vue_data(ice->shaders.last_vue_shader)->vue_map;
   const uint64_t changed_slots =
      (old_map ? old_map->slots_valid : 0ull) ^ vue_map->slots_valid;

   if (changed_slots & VARYING_BIT_VIEWPORT) {
      ice->state.num_viewports =
         (vue_map->slots_valid & VARYING_BIT_VIEWPORT) ? IRIS_MAX_VIEWPORTS : 1;
      ice->state.dirty |= IRIS_DIRTY_CLIP |
                          IRIS_DIRTY_SF_CL_VIEWPORT |
                          IRIS_DIRTY_CC_VIEWPORT |
                          IRIS_DIRTY_SCISSOR_RECT;
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_UNCOMPILED_FS |
         ice->state.stage_dirty_for_nos[IRIS_NOS_LAST_VUE_MAP];
   }

   if (changed_slots & VARYING_BIT_LAYER) {
      ice->state.dirty |= IRIS_DIRTY_CLIP;
   }

   if (changed_slots || (old_map && old_map->layout != vue_map->layout)) {
      ice->state.dirty |= IRIS_DIRTY_SBE;
   }

   iris_shader_variant_reference(&ice->shaders.last_vue_shader, shader);
}

static void
iris_update_pull_constant_descriptors(struct iris_context *ice,
                                      gl_shader_stage stage)
{
   struct iris_compiled_shader *shader = ice->shaders.prog[stage];

   if (!shader || !shader->has_ubo_pull)
      return;

   struct iris_shader_state *shs = &ice->state.shaders[stage];
   bool any_new_descriptors =
      shader->num_system_values > 0 && shs->sysvals_need_upload;

   unsigned bound_cbufs = shs->bound_cbufs;

   while (bound_cbufs) {
      const int i = u_bit_scan(&bound_cbufs);
      struct pipe_shader_buffer *cbuf = &shs->constbuf[i];
      struct iris_state_ref *surf_state = &shs->constbuf_surf_state[i];
      if (!surf_state->res && cbuf->buffer) {
         iris_upload_ubo_ssbo_surf_state(ice, cbuf, surf_state,
                                         ISL_SURF_USAGE_CONSTANT_BUFFER_BIT);
         any_new_descriptors = true;
      }
   }

   if (any_new_descriptors)
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_BINDINGS_VS << stage;
}

/**
 * Update the current shader variants for the given state.
 *
 * This should be called on every draw call to ensure that the correct
 * shaders are bound.  It will also flag any dirty state triggered by
 * swapping out those shaders.
 */
void
iris_update_compiled_shaders(struct iris_context *ice)
{
   const uint64_t stage_dirty = ice->state.stage_dirty;

   if (stage_dirty & (IRIS_STAGE_DIRTY_UNCOMPILED_TCS |
                      IRIS_STAGE_DIRTY_UNCOMPILED_TES)) {
       struct iris_uncompiled_shader *tes =
          ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL];
       if (tes) {
          iris_update_compiled_tcs(ice);
          iris_update_compiled_tes(ice);
       } else {
         iris_shader_variant_reference(&ice->shaders.prog[MESA_SHADER_TESS_CTRL], NULL);
         iris_shader_variant_reference(&ice->shaders.prog[MESA_SHADER_TESS_EVAL], NULL);
          ice->state.stage_dirty |=
             IRIS_STAGE_DIRTY_TCS | IRIS_STAGE_DIRTY_TES |
             IRIS_STAGE_DIRTY_BINDINGS_TCS | IRIS_STAGE_DIRTY_BINDINGS_TES |
             IRIS_STAGE_DIRTY_CONSTANTS_TCS | IRIS_STAGE_DIRTY_CONSTANTS_TES;

          if (ice->shaders.urb.constrained)
             ice->state.dirty |= IRIS_DIRTY_URB;
       }
   }

   if (stage_dirty & IRIS_STAGE_DIRTY_UNCOMPILED_VS)
      iris_update_compiled_vs(ice);
   if (stage_dirty & IRIS_STAGE_DIRTY_UNCOMPILED_GS)
      iris_update_compiled_gs(ice);

   if (stage_dirty & (IRIS_STAGE_DIRTY_UNCOMPILED_GS |
                      IRIS_STAGE_DIRTY_UNCOMPILED_TES)) {
      const struct iris_compiled_shader *gs =
         ice->shaders.prog[MESA_SHADER_GEOMETRY];
      const struct iris_compiled_shader *tes =
         ice->shaders.prog[MESA_SHADER_TESS_EVAL];

      bool points_or_lines = false;

      if (gs) {
         const struct iris_gs_data *gs_data = iris_gs_data_const(gs);
         points_or_lines =
            gs_data->output_topology == _3DPRIM_POINTLIST ||
            gs_data->output_topology == _3DPRIM_LINESTRIP;
      } else if (tes) {
         const struct iris_tes_data *tes_data = iris_tes_data_const(tes);
         points_or_lines =
            tes_data->output_topology == INTEL_TESS_OUTPUT_TOPOLOGY_LINE ||
            tes_data->output_topology == INTEL_TESS_OUTPUT_TOPOLOGY_POINT;
      }

      if (ice->shaders.output_topology_is_points_or_lines != points_or_lines) {
         /* Outbound to XY Clip enables */
         ice->shaders.output_topology_is_points_or_lines = points_or_lines;
         ice->state.dirty |= IRIS_DIRTY_CLIP;
      }
   }

   gl_shader_stage last_stage = last_vue_stage(ice);
   struct iris_compiled_shader *shader = ice->shaders.prog[last_stage];
   struct iris_uncompiled_shader *ish = ice->shaders.uncompiled[last_stage];
   update_last_vue_map(ice, shader);
   if (ice->state.streamout != shader->streamout) {
      ice->state.streamout = shader->streamout;
      ice->state.dirty |= IRIS_DIRTY_SO_DECL_LIST | IRIS_DIRTY_STREAMOUT;
   }

   if (ice->state.streamout_active) {
      for (int i = 0; i < PIPE_MAX_SO_BUFFERS; i++) {
         struct iris_stream_output_target *so =
            (void *) ice->state.so_target[i];
         if (so)
            so->stride = ish->stream_output.stride[i] * sizeof(uint32_t);
      }
   }

   if (stage_dirty & IRIS_STAGE_DIRTY_UNCOMPILED_FS)
      iris_update_compiled_fs(ice);

   for (int i = MESA_SHADER_VERTEX; i <= MESA_SHADER_FRAGMENT; i++) {
      if (ice->state.stage_dirty & (IRIS_STAGE_DIRTY_CONSTANTS_VS << i))
         iris_update_pull_constant_descriptors(ice, i);
   }
}

static void
iris_compile_cs(struct iris_screen *screen,
                struct u_upload_mgr *uploader,
                struct util_debug_callback *dbg,
                struct iris_uncompiled_shader *ish,
                struct iris_compiled_shader *shader)
{
   void *mem_ctx = ralloc_context(NULL);
   uint32_t *system_values;
   const struct intel_device_info *devinfo = screen->devinfo;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);
   const struct iris_cs_prog_key *const key = &shader->key.cs;

   if (screen->brw)
      NIR_PASS(_, nir, brw_nir_lower_cs_intrinsics, devinfo, NULL);
   else
#ifdef INTEL_USE_ELK
      NIR_PASS(_, nir, elk_nir_lower_cs_intrinsics, devinfo, NULL);
#else
      UNREACHABLE("no elk support");
#endif

   iris_setup_uniforms(devinfo, mem_ctx, nir,
                       &system_values, &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(devinfo, nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs, false);

   const char *error;
   const unsigned *program;

   if (screen->brw) {
      struct brw_cs_prog_key brw_key = iris_to_brw_cs_key(screen, key);

      struct brw_cs_prog_data *brw_prog_data =
         rzalloc(mem_ctx, struct brw_cs_prog_data);

      struct brw_compile_cs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &brw_key,
         .prog_data = brw_prog_data,
      };

      program = brw_compile_cs(screen->brw, &params);
      error = params.base.error_str;
      if (program) {
         iris_debug_recompile_brw(screen, dbg, ish, &brw_key.base);
         iris_apply_brw_prog_data(shader, &brw_prog_data->base);
      }
   } else {
#ifdef INTEL_USE_ELK
      struct elk_cs_prog_key elk_key = iris_to_elk_cs_key(screen, key);

      struct elk_cs_prog_data *elk_prog_data =
         rzalloc(mem_ctx, struct elk_cs_prog_data);

      struct elk_compile_cs_params params = {
         .base = {
            .mem_ctx = mem_ctx,
            .nir = nir,
            .log_data = dbg,
            .source_hash = ish->source_hash,
         },
         .key = &elk_key,
         .prog_data = elk_prog_data,
      };

      program = elk_compile_cs(screen->elk, &params);
      error = params.base.error_str;
      if (program) {
         iris_debug_recompile_elk(screen, dbg, ish, &elk_key.base);
         iris_apply_elk_prog_data(shader, &elk_prog_data->base);
      }
#else
      UNREACHABLE("no elk support");
#endif
   }

   if (program == NULL) {
      dbg_printf("Failed to compile compute shader: %s\n", error);

      shader->compilation_failed = true;
      util_queue_fence_signal(&shader->ready);

      return;
   }

   shader->compilation_failed = false;

   iris_finalize_program(shader, NULL, system_values,
                         num_system_values, num_cbufs,
                         &bt);

   iris_upload_shader(screen, ish, shader, NULL, uploader, IRIS_CACHE_CS,
                      sizeof(*key), key, program);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
}

static void
iris_update_compiled_cs(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_COMPUTE];
   struct u_upload_mgr *uploader = ice->shaders.uploader_driver;
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_COMPUTE];
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   struct iris_cs_prog_key key = { KEY_INIT(base) };
   screen->vtbl.populate_cs_key(ice, &key);

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_CS];
   bool added;
   struct iris_compiled_shader *shader =
      find_or_add_variant(screen, ish, IRIS_CACHE_CS, &key,
                          sizeof(key), &added);

   if (added && !iris_disk_cache_retrieve(screen, uploader, ish, shader,
                                          &key, sizeof(key))) {
      iris_compile_cs(screen, uploader, &ice->dbg, ish, shader);
   }

   if (shader->compilation_failed)
      shader = NULL;

   if (old != shader) {
      iris_shader_variant_reference(&ice->shaders.prog[MESA_SHADER_COMPUTE],
                                    shader);
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_CS |
                                IRIS_STAGE_DIRTY_BINDINGS_CS |
                                IRIS_STAGE_DIRTY_CONSTANTS_CS;
      shs->sysvals_need_upload = true;
   }
}

void
iris_update_compiled_compute_shader(struct iris_context *ice)
{
   if (ice->state.stage_dirty & IRIS_STAGE_DIRTY_UNCOMPILED_CS)
      iris_update_compiled_cs(ice);

   if (ice->state.stage_dirty & IRIS_STAGE_DIRTY_CONSTANTS_CS)
      iris_update_pull_constant_descriptors(ice, MESA_SHADER_COMPUTE);
}

void
iris_fill_cs_push_const_buffer(struct iris_screen *screen,
                               struct iris_compiled_shader *shader,
                               unsigned threads,
                               uint32_t *dst)
{
   struct iris_cs_data *cs_data = iris_cs_data(shader);
   assert(iris_cs_push_const_total_size(shader, threads) > 0);
   assert(cs_data->push.cross_thread.size == 0);
   assert(cs_data->push.per_thread.dwords == 1);
   assert(cs_data->first_param_is_builtin_subgroup_id);
   for (unsigned t = 0; t < threads; t++)
      dst[8 * t] = t;
}

/**
 * Allocate scratch BOs as needed for the given per-thread size and stage.
 */
struct iris_bo *
iris_get_scratch_space(struct iris_context *ice,
                       unsigned per_thread_scratch,
                       gl_shader_stage stage)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   struct iris_bufmgr *bufmgr = screen->bufmgr;
   const struct intel_device_info *devinfo = screen->devinfo;

   unsigned encoded_size = ffs(per_thread_scratch) - 11;
   assert(encoded_size < ARRAY_SIZE(ice->shaders.scratch_bos));
   assert(per_thread_scratch == 1 << (encoded_size + 10));

   /* On GFX version 12.5, scratch access changed to a surface-based model.
    * Instead of each shader type having its own layout based on IDs passed
    * from the relevant fixed-function unit, all scratch access is based on
    * thread IDs like it always has been for compute.
    */
   if (devinfo->verx10 >= 125)
      stage = MESA_SHADER_COMPUTE;

   struct iris_bo **bop = &ice->shaders.scratch_bos[encoded_size][stage];

   if (!*bop) {
      assert(stage < ARRAY_SIZE(devinfo->max_scratch_ids));
      uint32_t size = per_thread_scratch * devinfo->max_scratch_ids[stage];
      *bop = iris_bo_alloc(bufmgr, "scratch", size, 1024,
                           IRIS_MEMZONE_SHADER, BO_ALLOC_PLAIN);
   }

   return *bop;
}

const struct iris_state_ref *
iris_get_scratch_surf(struct iris_context *ice,
                      unsigned per_thread_scratch)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   ASSERTED const struct intel_device_info *devinfo = screen->devinfo;

   assert(devinfo->verx10 >= 125);

   unsigned encoded_size = ffs(per_thread_scratch) - 11;
   assert(encoded_size < ARRAY_SIZE(ice->shaders.scratch_surfs));
   assert(per_thread_scratch == 1 << (encoded_size + 10));

   struct iris_state_ref *ref = &ice->shaders.scratch_surfs[encoded_size];

   if (ref->res)
      return ref;

   struct iris_bo *scratch_bo =
      iris_get_scratch_space(ice, per_thread_scratch, MESA_SHADER_COMPUTE);

   void *map = upload_state(ice->state.scratch_surface_uploader, ref,
                            screen->isl_dev.ss.size, 64);

   isl_buffer_fill_state(&screen->isl_dev, map,
                         .address = scratch_bo->address,
                         .size_B = scratch_bo->size,
                         .format = ISL_FORMAT_RAW,
                         .swizzle = ISL_SWIZZLE_IDENTITY,
                         .usage = 0,
                         .mocs = iris_mocs(scratch_bo, &screen->isl_dev, 0),
                         .stride_B = per_thread_scratch,
                         .is_scratch = true);

   return ref;
}

/* ------------------------------------------------------------------- */

/**
 * The pipe->create_[stage]_state() driver hooks.
 *
 * Performs basic NIR preprocessing, records any state dependencies, and
 * returns an iris_uncompiled_shader as the Gallium CSO.
 *
 * Actual shader compilation to assembly happens later, at first use.
 */
static void *
iris_create_uncompiled_shader(struct iris_screen *screen,
                              nir_shader *nir,
                              const struct pipe_stream_output_info *so_info)
{
   struct iris_uncompiled_shader *ish =
      calloc(1, sizeof(struct iris_uncompiled_shader));
   if (!ish)
      return NULL;

   pipe_reference_init(&ish->ref, 1);
   list_inithead(&ish->variants);
   simple_mtx_init(&ish->lock, mtx_plain);
   util_queue_fence_init(&ish->ready);

   ish->uses_atomic_load_store = iris_uses_image_atomic(nir);

   ish->program_id = get_new_program_id(screen);
   ish->nir = nir;
   if (so_info) {
      memcpy(&ish->stream_output, so_info, sizeof(*so_info));
      update_so_info(&ish->stream_output, nir->info.outputs_written);
   }

   /* Use lowest dword of source shader blake3 for shader hash. */
   ish->source_hash = *(uint32_t*)nir->info.source_blake3;

   if (screen->disk_cache) {
      /* Serialize the NIR to a binary blob that we can hash for the disk
       * cache.  Drop unnecessary information (like variable names)
       * so the serialized NIR is smaller, and also to let us detect more
       * isomorphic shaders when hashing, increasing cache hits.
       */
      struct blob blob;
      blob_init(&blob);
      nir_serialize(&blob, nir, true);
      _mesa_sha1_compute(blob.data, blob.size, ish->nir_sha1);
      blob_finish(&blob);
   }

   return ish;
}

static void *
iris_create_compute_state(struct pipe_context *ctx,
                          const struct pipe_compute_state *state)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   struct u_upload_mgr *uploader = ice->shaders.uploader_unsync;

   nir_shader *nir;
   switch (state->ir_type) {
   case PIPE_SHADER_IR_NIR:
      nir = (void *)state->prog;
      break;

   default:
      UNREACHABLE("Unsupported IR");
   }

   /* Most of iris doesn't really care about the difference between compute
    * shaders and kernels.  We also tend to hard-code COMPUTE everywhere so
    * it's way easier if we just normalize to COMPUTE here.
    */
   assert(nir->info.stage == MESA_SHADER_COMPUTE ||
          nir->info.stage == MESA_SHADER_KERNEL);
   nir->info.stage = MESA_SHADER_COMPUTE;

   struct iris_uncompiled_shader *ish =
      iris_create_uncompiled_shader(screen, nir, NULL);
   ish->kernel_shared_size = state->static_shared_mem;

   // XXX: disallow more than 64KB of shared variables

   if (screen->precompile) {
      struct iris_cs_prog_key key = { KEY_INIT(base) };

      struct iris_compiled_shader *shader =
         iris_create_shader_variant(screen, NULL, MESA_SHADER_COMPUTE,
                                    IRIS_CACHE_CS, sizeof(key), &key);

      /* Append our new variant to the shader's variant list. */
      list_addtail(&shader->link, &ish->variants);

      if (!iris_disk_cache_retrieve(screen, uploader, ish, shader,
                                    &key, sizeof(key))) {
         iris_compile_cs(screen, uploader, &ice->dbg, ish, shader);
      }
   }

   return ish;
}

static void
iris_get_compute_state_info(struct pipe_context *ctx, void *state,
                            struct pipe_compute_state_object_info *info)
{
   struct iris_screen *screen = (void *) ctx->screen;
   struct iris_uncompiled_shader *ish = state;

   info->max_threads = MIN2(1024, 32 * screen->devinfo->max_cs_workgroup_threads);
   info->private_memory = 0;
   info->preferred_simd_size = 32;
   info->simd_sizes = 8 | 16 | 32;

   list_for_each_entry_safe(struct iris_compiled_shader, shader,
                            &ish->variants, link) {
      info->private_memory = MAX2(info->private_memory,
                                  shader->total_scratch);
   }
}

static uint32_t
iris_get_compute_state_subgroup_size(struct pipe_context *ctx, void *state,
                                     const uint32_t block[3])
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   struct u_upload_mgr *uploader = ice->shaders.uploader_driver;
   struct iris_uncompiled_shader *ish = state;

   struct iris_cs_prog_key key = { KEY_INIT(base) };
   screen->vtbl.populate_cs_key(ice, &key);

   bool added;
   struct iris_compiled_shader *shader =
      find_or_add_variant(screen, ish, IRIS_CACHE_CS, &key,
                          sizeof(key), &added);

   if (added && !iris_disk_cache_retrieve(screen, uploader, ish, shader,
                                          &key, sizeof(key))) {
      iris_compile_cs(screen, uploader, &ice->dbg, ish, shader);
   }

   return iris_get_cs_dispatch_info(screen->devinfo, shader, block).simd_size;
}

static void
iris_compile_shader(void *_job, UNUSED void *_gdata, UNUSED int thread_index)
{
   const struct iris_threaded_compile_job *job =
      (struct iris_threaded_compile_job *) _job;

   struct iris_screen *screen = job->screen;
   struct u_upload_mgr *uploader = job->uploader;
   struct util_debug_callback *dbg = job->dbg;
   struct iris_uncompiled_shader *ish = job->ish;
   struct iris_compiled_shader *shader = job->shader;

   switch (ish->nir->info.stage) {
   case MESA_SHADER_VERTEX:
      iris_compile_vs(screen, uploader, dbg, ish, shader);
      break;
   case MESA_SHADER_TESS_CTRL:
      iris_compile_tcs(screen, NULL, uploader, dbg, ish, shader);
      break;
   case MESA_SHADER_TESS_EVAL:
      iris_compile_tes(screen, uploader, dbg, ish, shader);
      break;
   case MESA_SHADER_GEOMETRY:
      iris_compile_gs(screen, uploader, dbg, ish, shader);
      break;
   case MESA_SHADER_FRAGMENT:
      iris_compile_fs(screen, uploader, dbg, ish, shader, NULL);
      break;

   default:
      UNREACHABLE("Invalid shader stage.");
   }
}

static void *
iris_create_shader_state(struct pipe_context *ctx,
                         const struct pipe_shader_state *state)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   struct nir_shader *nir;

   if (state->type == PIPE_SHADER_IR_TGSI)
      nir = tgsi_to_nir(state->tokens, ctx->screen, false);
   else
      nir = state->ir.nir;

   const struct shader_info *const info = &nir->info;
   struct iris_uncompiled_shader *ish =
      iris_create_uncompiled_shader(screen, nir, &state->stream_output);

   union iris_any_prog_key key;
   unsigned key_size = 0;

   memset(&key, 0, sizeof(key));

   switch (info->stage) {
   case MESA_SHADER_VERTEX:
      /* User clip planes */
      if (info->clip_distance_array_size == 0)
         ish->nos |= (1ull << IRIS_NOS_RASTERIZER);

      key.vs = (struct iris_vs_prog_key) {
         KEY_INIT(vue.base),
         .vue.layout = vue_layout(ish->nir->info.separate_shader),
      };
      key_size = sizeof(key.vs);
      break;

   case MESA_SHADER_TESS_CTRL: {
      key.tcs = (struct iris_tcs_prog_key) {
         KEY_INIT(vue.base),
         .vue.layout = vue_layout(ish->nir->info.separate_shader),
         // XXX: make sure the linker fills this out from the TES...
         ._tes_primitive_mode =
         info->tess._primitive_mode ? info->tess._primitive_mode
                                   : TESS_PRIMITIVE_TRIANGLES,
         .outputs_written = info->outputs_written,
         .patch_outputs_written = info->patch_outputs_written,
      };

      /* MULTI_PATCH mode needs the key to contain the input patch dimensionality.
       * We don't have that information, so we randomly guess that the input
       * and output patches are the same size.  This is a bad guess, but we
       * can't do much better.
       */
      if (iris_use_tcs_multi_patch(screen))
         key.tcs.input_vertices = info->tess.tcs_vertices_out;

      key_size = sizeof(key.tcs);
      break;
   }

   case MESA_SHADER_TESS_EVAL:
      /* User clip planes */
      if (info->clip_distance_array_size == 0)
         ish->nos |= (1ull << IRIS_NOS_RASTERIZER);

      key.tes = (struct iris_tes_prog_key) {
         KEY_INIT(vue.base),
         .vue.layout = vue_layout(ish->nir->info.separate_shader),
         // XXX: not ideal, need TCS output/TES input unification
         .inputs_read = info->inputs_read,
         .patch_inputs_read = info->patch_inputs_read,
      };

      key_size = sizeof(key.tes);
      break;

   case MESA_SHADER_GEOMETRY:
      ish->nos |= (1ull << IRIS_NOS_RASTERIZER);

      key.gs = (struct iris_gs_prog_key) {
         KEY_INIT(vue.base),
         .vue.layout = vue_layout(ish->nir->info.separate_shader),
      };
      key_size = sizeof(key.gs);
      break;

   case MESA_SHADER_FRAGMENT:
      ish->nos |= (1ull << IRIS_NOS_FRAMEBUFFER) |
                  (1ull << IRIS_NOS_DEPTH_STENCIL_ALPHA) |
                  (1ull << IRIS_NOS_RASTERIZER) |
                  (1ull << IRIS_NOS_BLEND);

#ifdef INTEL_USE_ELK
      STATIC_ASSERT(BRW_FS_VARYING_INPUT_MASK == ELK_FS_VARYING_INPUT_MASK);
#endif

      /* The program key needs the VUE map if there are > 16 inputs */
      if (util_bitcount64(info->inputs_read & BRW_FS_VARYING_INPUT_MASK) > 16) {
         ish->nos |= (1ull << IRIS_NOS_LAST_VUE_MAP);
      }

      const uint64_t color_outputs = info->outputs_written &
         ~(BITFIELD64_BIT(FRAG_RESULT_DEPTH) |
           BITFIELD64_BIT(FRAG_RESULT_STENCIL) |
           BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK));

      bool can_rearrange_varyings =
         util_bitcount64(info->inputs_read & BRW_FS_VARYING_INPUT_MASK) <= 16;

      const struct intel_device_info *devinfo = screen->devinfo;

      key.fs = (struct iris_fs_prog_key) {
         KEY_INIT(base),
         .vue_layout = vue_layout(ish->nir->info.separate_shader),
         .nr_color_regions = util_bitcount(color_outputs),
         .coherent_fb_fetch = devinfo->ver >= 9 && devinfo->ver < 20,
         .input_slots_valid =
            can_rearrange_varyings ? 0 : info->inputs_read | VARYING_BIT_POS,
      };

      key_size = sizeof(key.fs);
      break;

   default:
      UNREACHABLE("Invalid shader stage.");
   }

   if (screen->precompile) {
      struct u_upload_mgr *uploader = ice->shaders.uploader_unsync;

      struct iris_compiled_shader *shader =
         iris_create_shader_variant(screen, NULL, info->stage,
                                    (enum iris_program_cache_id) info->stage,
                                    key_size, &key);

      /* Append our new variant to the shader's variant list. */
      list_addtail(&shader->link, &ish->variants);

      if (!iris_disk_cache_retrieve(screen, uploader, ish, shader,
                                    &key, key_size)) {
         assert(!util_queue_fence_is_signalled(&shader->ready));

         struct iris_threaded_compile_job *job = calloc(1, sizeof(*job));

         job->screen = screen;
         job->uploader = uploader;
         job->ish = ish;
         job->shader = shader;

         iris_schedule_compile(screen, &ish->ready, &ice->dbg, job,
                               iris_compile_shader);
      }
   }

   return ish;
}

/**
 * Called when the refcount on the iris_uncompiled_shader reaches 0.
 *
 * Frees the iris_uncompiled_shader.
 *
 * \sa iris_delete_shader_state
 */
void
iris_destroy_shader_state(struct pipe_context *ctx, void *state)
{
   struct iris_uncompiled_shader *ish = state;

   /* No need to take ish->lock; we hold the last reference to ish */
   list_for_each_entry_safe(struct iris_compiled_shader, shader,
                            &ish->variants, link) {
      list_del(&shader->link);

      iris_shader_variant_reference(&shader, NULL);
   }

   simple_mtx_destroy(&ish->lock);
   util_queue_fence_destroy(&ish->ready);

   ralloc_free(ish->nir);
   free(ish);
}

/**
 * The pipe->delete_[stage]_state() driver hooks.
 *
 * \sa iris_destroy_shader_state
 */
static void
iris_delete_shader_state(struct pipe_context *ctx, void *state)
{
   struct iris_uncompiled_shader *ish = state;
   struct iris_context *ice = (void *) ctx;

   const gl_shader_stage stage = ish->nir->info.stage;

   if (ice->shaders.uncompiled[stage] == ish) {
      ice->shaders.uncompiled[stage] = NULL;
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_UNCOMPILED_VS << stage;
   }

   if (pipe_reference(&ish->ref, NULL))
      iris_destroy_shader_state(ctx, state);
}

/**
 * The pipe->bind_[stage]_state() driver hook.
 *
 * Binds an uncompiled shader as the current one for a particular stage.
 * Updates dirty tracking to account for the shader's NOS.
 */
static void
bind_shader_state(struct iris_context *ice,
                  struct iris_uncompiled_shader *ish,
                  gl_shader_stage stage)
{
   uint64_t stage_dirty_bit = IRIS_STAGE_DIRTY_UNCOMPILED_VS << stage;
   const uint64_t nos = ish ? ish->nos : 0;

   const struct shader_info *old_info = iris_get_shader_info(ice, stage);
   const struct shader_info *new_info = ish ? &ish->nir->info : NULL;

   if ((old_info ? BITSET_LAST_BIT(old_info->samplers_used) : 0) !=
       (new_info ? BITSET_LAST_BIT(new_info->samplers_used) : 0)) {
      ice->state.stage_dirty |= IRIS_STAGE_DIRTY_SAMPLER_STATES_VS << stage;
   }

   ice->shaders.uncompiled[stage] = ish;
   ice->state.stage_dirty |= stage_dirty_bit;

   /* Record that CSOs need to mark IRIS_DIRTY_UNCOMPILED_XS when they change
    * (or that they no longer need to do so).
    */
   for (int i = 0; i < IRIS_NOS_COUNT; i++) {
      if (nos & (1 << i))
         ice->state.stage_dirty_for_nos[i] |= stage_dirty_bit;
      else
         ice->state.stage_dirty_for_nos[i] &= ~stage_dirty_bit;
   }
}

static void
iris_bind_vs_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *)ctx;
   struct iris_uncompiled_shader *ish = state;

   if (ish) {
      const struct shader_info *info = &ish->nir->info;
      if (ice->state.window_space_position != info->vs.window_space_position) {
         ice->state.window_space_position = info->vs.window_space_position;

         ice->state.dirty |= IRIS_DIRTY_CLIP |
                             IRIS_DIRTY_RASTER |
                             IRIS_DIRTY_CC_VIEWPORT;
      }

      const bool uses_draw_params =
         BITSET_TEST(info->system_values_read, SYSTEM_VALUE_FIRST_VERTEX) ||
         BITSET_TEST(info->system_values_read, SYSTEM_VALUE_BASE_INSTANCE);
      const bool uses_derived_draw_params =
         BITSET_TEST(info->system_values_read, SYSTEM_VALUE_DRAW_ID) ||
         BITSET_TEST(info->system_values_read, SYSTEM_VALUE_IS_INDEXED_DRAW);
      const bool needs_sgvs_element = uses_draw_params ||
         BITSET_TEST(info->system_values_read, SYSTEM_VALUE_INSTANCE_ID) ||
         BITSET_TEST(info->system_values_read,
                     SYSTEM_VALUE_VERTEX_ID_ZERO_BASE);

      if (ice->state.vs_uses_draw_params != uses_draw_params ||
          ice->state.vs_uses_derived_draw_params != uses_derived_draw_params ||
          ice->state.vs_needs_edge_flag != info->vs.needs_edge_flag ||
          ice->state.vs_needs_sgvs_element != needs_sgvs_element) {
         ice->state.dirty |= IRIS_DIRTY_VERTEX_BUFFERS |
                             IRIS_DIRTY_VERTEX_ELEMENTS;
      }

      ice->state.vs_uses_draw_params = uses_draw_params;
      ice->state.vs_uses_derived_draw_params = uses_derived_draw_params;
      ice->state.vs_needs_sgvs_element = needs_sgvs_element;
      ice->state.vs_needs_edge_flag = info->vs.needs_edge_flag;
   }

   bind_shader_state((void *) ctx, state, MESA_SHADER_VERTEX);
}

static void
iris_bind_tcs_state(struct pipe_context *ctx, void *state)
{
   bind_shader_state((void *) ctx, state, MESA_SHADER_TESS_CTRL);
}

static void
iris_bind_tes_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *)ctx;
   struct iris_screen *screen = (struct iris_screen *) ctx->screen;
   const struct intel_device_info *devinfo = screen->devinfo;

   /* Enabling/disabling optional stages requires a URB reconfiguration. */
   if (!!state != !!ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL])
      ice->state.dirty |= IRIS_DIRTY_URB | (devinfo->verx10 >= 125 ?
                                            IRIS_DIRTY_VFG : 0);

   bind_shader_state((void *) ctx, state, MESA_SHADER_TESS_EVAL);
}

static void
iris_bind_gs_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   /* Enabling/disabling optional stages requires a URB reconfiguration. */
   if (!!state != !!ice->shaders.uncompiled[MESA_SHADER_GEOMETRY])
      ice->state.dirty |= IRIS_DIRTY_URB;

   bind_shader_state((void *) ctx, state, MESA_SHADER_GEOMETRY);
}

static void
iris_bind_fs_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_screen *screen = (struct iris_screen *) ctx->screen;
   const struct intel_device_info *devinfo = screen->devinfo;
   struct iris_uncompiled_shader *old_ish =
      ice->shaders.uncompiled[MESA_SHADER_FRAGMENT];
   struct iris_uncompiled_shader *new_ish = state;

   const unsigned color_bits =
      BITFIELD64_BIT(FRAG_RESULT_COLOR) |
      BITFIELD64_RANGE(FRAG_RESULT_DATA0, IRIS_MAX_DRAW_BUFFERS);

   /* Fragment shader outputs influence HasWriteableRT */
   if (!old_ish || !new_ish ||
       (old_ish->nir->info.outputs_written & color_bits) !=
       (new_ish->nir->info.outputs_written & color_bits))
      ice->state.dirty |= IRIS_DIRTY_PS_BLEND;

   if (devinfo->ver == 8)
      ice->state.dirty |= IRIS_DIRTY_PMA_FIX;

   bind_shader_state((void *) ctx, state, MESA_SHADER_FRAGMENT);
}

static void
iris_bind_cs_state(struct pipe_context *ctx, void *state)
{
   bind_shader_state((void *) ctx, state, MESA_SHADER_COMPUTE);
}

static void
iris_finalize_nir(struct pipe_screen *_screen, struct nir_shader *nir)
{
   struct iris_screen *screen = (struct iris_screen *)_screen;

   NIR_PASS(_, nir, iris_fix_edge_flags);

   if (screen->brw) {
      struct brw_nir_compiler_opts opts = {};
      brw_preprocess_nir(screen->brw, nir, &opts);

      NIR_PASS(_, nir, brw_nir_lower_storage_image,
                 screen->brw,
                 &(struct brw_nir_lower_storage_image_opts) {
                    .lower_loads  = true,
                    .lower_stores = true,
                 });
   } else {
#ifdef INTEL_USE_ELK
      const struct intel_device_info *devinfo = screen->devinfo;
      assert(screen->elk);

      struct elk_nir_compiler_opts opts = {};
      elk_preprocess_nir(screen->elk, nir, &opts);

      NIR_PASS(_, nir, elk_nir_lower_storage_image,
                 &(struct elk_nir_lower_storage_image_opts) {
                    .devinfo        = devinfo,
                    .lower_loads    = true,
                    .lower_stores   = true,

                    /* Iris uploads image params used by
                     * get_size lowering only in Gfx8.
                     */
                    .lower_get_size = true,
                 });
#else
      UNREACHABLE("no elk support");
#endif
   }

   NIR_PASS(_, nir, iris_lower_storage_image_derefs);

   nir_sweep(nir);
}

static void
iris_set_max_shader_compiler_threads(struct pipe_screen *pscreen,
                                     unsigned max_threads)
{
   struct iris_screen *screen = (struct iris_screen *) pscreen;
   util_queue_adjust_num_threads(&screen->shader_compiler_queue, max_threads,
                                 false);
}

static bool
iris_is_parallel_shader_compilation_finished(struct pipe_screen *pscreen,
                                             void *v_shader,
                                             enum pipe_shader_type p_stage)
{
   struct iris_screen *screen = (struct iris_screen *) pscreen;

   /* Threaded compilation is only used for the precompile.  If precompile is
    * disabled, threaded compilation is "done."
    */
   if (!screen->precompile)
      return true;

   struct iris_uncompiled_shader *ish = v_shader;

   /* When precompile is enabled, the first entry is the precompile variant.
    * Check the ready fence of the precompile variant.
    */
   struct iris_compiled_shader *first =
      list_first_entry(&ish->variants, struct iris_compiled_shader, link);

   return util_queue_fence_is_signalled(&first->ready);
}

void
iris_init_screen_program_functions(struct pipe_screen *pscreen)
{
   pscreen->is_parallel_shader_compilation_finished =
      iris_is_parallel_shader_compilation_finished;
   pscreen->set_max_shader_compiler_threads =
      iris_set_max_shader_compiler_threads;
   pscreen->finalize_nir = iris_finalize_nir;
}

void
iris_init_program_functions(struct pipe_context *ctx)
{
   ctx->create_vs_state  = iris_create_shader_state;
   ctx->create_tcs_state = iris_create_shader_state;
   ctx->create_tes_state = iris_create_shader_state;
   ctx->create_gs_state  = iris_create_shader_state;
   ctx->create_fs_state  = iris_create_shader_state;
   ctx->create_compute_state = iris_create_compute_state;

   ctx->delete_vs_state  = iris_delete_shader_state;
   ctx->delete_tcs_state = iris_delete_shader_state;
   ctx->delete_tes_state = iris_delete_shader_state;
   ctx->delete_gs_state  = iris_delete_shader_state;
   ctx->delete_fs_state  = iris_delete_shader_state;
   ctx->delete_compute_state = iris_delete_shader_state;

   ctx->bind_vs_state  = iris_bind_vs_state;
   ctx->bind_tcs_state = iris_bind_tcs_state;
   ctx->bind_tes_state = iris_bind_tes_state;
   ctx->bind_gs_state  = iris_bind_gs_state;
   ctx->bind_fs_state  = iris_bind_fs_state;
   ctx->bind_compute_state = iris_bind_cs_state;

   ctx->get_compute_state_info = iris_get_compute_state_info;
   ctx->get_compute_state_subgroup_size = iris_get_compute_state_subgroup_size;
}

struct intel_cs_dispatch_info
iris_get_cs_dispatch_info(const struct intel_device_info *devinfo,
                          const struct iris_compiled_shader *shader,
                          const uint32_t block[3])
{
   if (shader->brw_prog_data) {
      return brw_cs_get_dispatch_info(devinfo,
                                      brw_cs_prog_data(shader->brw_prog_data),
                                      block);
   } else {
#ifdef INTEL_USE_ELK
      assert(shader->elk_prog_data);
      return elk_cs_get_dispatch_info(devinfo,
                                      elk_cs_prog_data(shader->elk_prog_data),
                                      block);
#else
      UNREACHABLE("no elk support");
#endif
   }
}

unsigned
iris_cs_push_const_total_size(const struct iris_compiled_shader *shader,
                              unsigned threads)
{
   if (shader->brw_prog_data) {
      return brw_cs_push_const_total_size(brw_cs_prog_data(shader->brw_prog_data),
                                          threads);
   } else {
#ifdef INTEL_USE_ELK
      assert(shader->elk_prog_data);
      return elk_cs_push_const_total_size(elk_cs_prog_data(shader->elk_prog_data),
                                          threads);
#else
      UNREACHABLE("no elk support");
#endif
   }
}

uint32_t
iris_fs_barycentric_modes(const struct iris_compiled_shader *shader,
                          enum intel_msaa_flags pushed_msaa_flags)
{
   if (shader->brw_prog_data) {
      return wm_prog_data_barycentric_modes(brw_wm_prog_data(shader->brw_prog_data),
                                            pushed_msaa_flags);
   } else {
#ifdef INTEL_USE_ELK
      assert(shader->elk_prog_data);
      return elk_wm_prog_data_barycentric_modes(elk_wm_prog_data(shader->elk_prog_data),
                                                pushed_msaa_flags);
#else
      UNREACHABLE("no elk support");
#endif
   }
}

bool
iris_use_tcs_multi_patch(struct iris_screen *screen)
{
   return screen->brw && screen->brw->use_tcs_multi_patch;
}

bool
iris_indirect_ubos_use_sampler(struct iris_screen *screen)
{
   if (screen->brw) {
      return screen->brw->indirect_ubos_use_sampler;
   } else {
#ifdef INTEL_USE_ELK
      assert(screen->elk);
      return screen->elk->indirect_ubos_use_sampler;
#else
      UNREACHABLE("no elk support");
#endif
   }
}

static void
iris_shader_debug_log(void *data, unsigned *id, const char *fmt, ...)
{
   struct util_debug_callback *dbg = data;
   va_list args;

   if (!dbg->debug_message)
      return;

   va_start(args, fmt);
   dbg->debug_message(dbg->data, id, UTIL_DEBUG_TYPE_SHADER_INFO, fmt, args);
   va_end(args);
}

static void
iris_shader_perf_log(void *data, unsigned *id, const char *fmt, ...)
{
   struct util_debug_callback *dbg = data;
   va_list args;
   va_start(args, fmt);

   if (INTEL_DEBUG(DEBUG_PERF)) {
      va_list args_copy;
      va_copy(args_copy, args);
      vfprintf(stderr, fmt, args_copy);
      va_end(args_copy);
   }

   if (dbg->debug_message) {
      dbg->debug_message(dbg->data, id, UTIL_DEBUG_TYPE_PERF_INFO, fmt, args);
   }

   va_end(args);
}

const struct nir_shader_compiler_options *
iris_get_compiler_options(struct pipe_screen *pscreen,
                          enum pipe_shader_type pstage)
{
   struct iris_screen *screen = (struct iris_screen *) pscreen;
   gl_shader_stage stage = stage_from_pipe(pstage);

#ifdef INTEL_USE_ELK
   return screen->brw ? screen->brw->nir_options[stage]
                      : screen->elk->nir_options[stage];
#else
   return screen->brw->nir_options[stage];
#endif
}

void
iris_compiler_init(struct iris_screen *screen)
{
   if (screen->devinfo->ver >= 9) {
      STATIC_ASSERT(IRIS_MAX_DRAW_BUFFERS == BRW_MAX_DRAW_BUFFERS);

      screen->brw = brw_compiler_create(screen, screen->devinfo);
      screen->brw->shader_debug_log = iris_shader_debug_log;
      screen->brw->shader_perf_log = iris_shader_perf_log;
   } else {
#ifdef INTEL_USE_ELK
      STATIC_ASSERT(IRIS_MAX_DRAW_BUFFERS == ELK_MAX_DRAW_BUFFERS);
      STATIC_ASSERT(IRIS_MAX_SOL_BINDINGS == ELK_MAX_SOL_BINDINGS);

      screen->elk = elk_compiler_create(screen, screen->devinfo);
      screen->elk->shader_debug_log = iris_shader_debug_log;
      screen->elk->shader_perf_log = iris_shader_perf_log;
      screen->elk->supports_shader_constants = true;
#else
      UNREACHABLE("no elk support");
#endif
   }
}
