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

#pragma once

#include "compiler/nir/nir.h"
#include "elk_compiler.h"
#include "nir_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

int elk_type_size_vec4(const struct glsl_type *type, bool bindless);
int elk_type_size_dvec4(const struct glsl_type *type, bool bindless);

static inline int
elk_type_size_scalar_bytes(const struct glsl_type *type, bool bindless)
{
   return glsl_count_dword_slots(type, bindless) * 4;
}

static inline int
elk_type_size_vec4_bytes(const struct glsl_type *type, bool bindless)
{
   return elk_type_size_vec4(type, bindless) * 16;
}

/* Flags set in the instr->pass_flags field by i965 analysis passes */
enum {
   ELK_NIR_NON_BOOLEAN           = 0x0,

   /* Indicates that the given instruction's destination is a boolean
    * value but that it needs to be resolved before it can be used.
    * On Gen <= 5, CMP instructions return a 32-bit value where the bottom
    * bit represents the actual true/false value of the compare and the top
    * 31 bits are undefined.  In order to use this value, we have to do a
    * "resolve" operation by replacing the value of the CMP with -(x & 1)
    * to sign-extend the bottom bit to 0/~0.
    */
   ELK_NIR_BOOLEAN_NEEDS_RESOLVE = 0x1,

   /* Indicates that the given instruction's destination is a boolean
    * value that has intentionally been left unresolved.  Not all boolean
    * values need to be resolved immediately.  For instance, if we have
    *
    *    CMP r1 r2 r3
    *    CMP r4 r5 r6
    *    AND r7 r1 r4
    *
    * We don't have to resolve the result of the two CMP instructions
    * immediately because the AND still does an AND of the bottom bits.
    * Instead, we can save ourselves instructions by delaying the resolve
    * until after the AND.  The result of the two CMP instructions is left
    * as ELK_NIR_BOOLEAN_UNRESOLVED.
    */
   ELK_NIR_BOOLEAN_UNRESOLVED    = 0x2,

   /* Indicates a that the given instruction's destination is a boolean
    * value that does not need a resolve.  For instance, if you AND two
    * values that are ELK_NIR_BOOLEAN_NEEDS_RESOLVE then we know that both
    * values will be 0/~0 before we get them and the result of the AND is
    * also guaranteed to be 0/~0 and does not need a resolve.
    */
   ELK_NIR_BOOLEAN_NO_RESOLVE    = 0x3,

   /* A mask to mask the boolean status values off of instr->pass_flags */
   ELK_NIR_BOOLEAN_MASK          = 0x3,
};

void elk_nir_analyze_boolean_resolves(nir_shader *nir);

struct elk_nir_compiler_opts {
   /* Soft floating point implementation shader */
   const nir_shader *softfp64;

   /* Whether robust image access is enabled */
   bool robust_image_access;

   /* Input vertices for TCS stage (0 means dynamic) */
   unsigned input_vertices;
};

/* UBO surface index can come in 2 flavors :
 *    - nir_intrinsic_resource_intel
 *    - anything else
 *
 * In the first case, checking that the surface index is const requires
 * checking resource_intel::src[1]. In any other case it's a simple
 * nir_src_is_const().
 *
 * This function should only be called on src[0] of load_ubo intrinsics.
 */
static inline bool
elk_nir_ubo_surface_index_is_pushable(nir_src src)
{
   nir_intrinsic_instr *intrin =
      src.ssa->parent_instr->type == nir_instr_type_intrinsic ?
      nir_def_as_intrinsic(src.ssa) : NULL;

   if (intrin && intrin->intrinsic == nir_intrinsic_resource_intel) {
      return (nir_intrinsic_resource_access_intel(intrin) &
              nir_resource_intel_pushable);
   }

   return nir_src_is_const(src);
}

static inline unsigned
elk_nir_ubo_surface_index_get_push_block(nir_src src)
{
   if (nir_src_is_const(src))
      return nir_src_as_uint(src);

   if (!elk_nir_ubo_surface_index_is_pushable(src))
      return UINT32_MAX;

   assert(src.ssa->parent_instr->type == nir_instr_type_intrinsic);

   nir_intrinsic_instr *intrin = nir_def_as_intrinsic(src.ssa);
   assert(intrin->intrinsic == nir_intrinsic_resource_intel);

   return nir_intrinsic_resource_block_intel(intrin);
}

/* This helper return the binding table index of a surface access (any
 * buffer/image/etc...). It works off the source of one of the intrinsics
 * (load_ubo, load_ssbo, store_ssbo, load_image, store_image, etc...).
 *
 * If the source is constant, then this is the binding table index. If we're
 * going through a resource_intel intel intrinsic, then we need to check
 * src[1] of that intrinsic.
 */
static inline unsigned
elk_nir_ubo_surface_index_get_bti(nir_src src)
{
   if (nir_src_is_const(src))
      return nir_src_as_uint(src);

   assert(src.ssa->parent_instr->type == nir_instr_type_intrinsic);

   nir_intrinsic_instr *intrin = nir_def_as_intrinsic(src.ssa);
   if (!intrin || intrin->intrinsic != nir_intrinsic_resource_intel)
      return UINT32_MAX;

   /* In practice we could even drop this intrinsic because the bindless
    * access always operate from a base offset coming from a push constant, so
    * they can never be constant.
    */
   if (nir_intrinsic_resource_access_intel(intrin) &
       nir_resource_intel_bindless)
      return UINT32_MAX;

   if (!nir_src_is_const(intrin->src[1]))
      return UINT32_MAX;

   return nir_src_as_uint(intrin->src[1]);
}

void elk_preprocess_nir(const struct elk_compiler *compiler,
                        nir_shader *nir,
                        const struct elk_nir_compiler_opts *opts);

void
elk_nir_link_shaders(const struct elk_compiler *compiler,
                     nir_shader *producer, nir_shader *consumer);

bool elk_nir_lower_cs_intrinsics(nir_shader *nir,
                                 const struct intel_device_info *devinfo,
                                 struct elk_cs_prog_data *prog_data);
bool elk_nir_lower_alpha_to_coverage(nir_shader *shader,
                                     const struct elk_wm_prog_key *key,
                                     const struct elk_wm_prog_data *prog_data);
void elk_nir_lower_vs_inputs(nir_shader *nir,
                             bool edgeflag_is_last,
                             const uint8_t *vs_attrib_wa_flags);
void elk_nir_lower_vue_inputs(nir_shader *nir,
                              const struct intel_vue_map *vue_map);
void elk_nir_lower_tes_inputs(nir_shader *nir, const struct intel_vue_map *vue);
void elk_nir_lower_fs_inputs(nir_shader *nir,
                             const struct intel_device_info *devinfo,
                             const struct elk_wm_prog_key *key);
void elk_nir_lower_vue_outputs(nir_shader *nir);
void elk_nir_lower_tcs_outputs(nir_shader *nir, const struct intel_vue_map *vue,
                               enum tess_primitive_mode tes_primitive_mode);
void elk_nir_lower_fs_outputs(nir_shader *nir);

bool elk_nir_lower_cmat(nir_shader *nir, unsigned subgroup_size);

bool elk_nir_lower_shading_rate_output(nir_shader *nir);

bool elk_nir_lower_sparse_intrinsics(nir_shader *nir);

struct elk_nir_lower_storage_image_opts {
   const struct intel_device_info *devinfo;

   bool lower_loads;
   bool lower_stores;
   bool lower_atomics;
   bool lower_get_size;
};

bool elk_nir_lower_storage_image(nir_shader *nir,
                                 const struct elk_nir_lower_storage_image_opts *opts);

bool elk_nir_lower_mem_access_bit_sizes(nir_shader *shader,
                                        const struct
                                        intel_device_info *devinfo);

void elk_postprocess_nir(nir_shader *nir,
                         const struct elk_compiler *compiler,
                         bool debug_enabled,
                         enum elk_robustness_flags robust_flags);

bool elk_nir_apply_attribute_workarounds(nir_shader *nir,
                                         const uint8_t *attrib_wa_flags);

bool elk_nir_apply_trig_workarounds(nir_shader *nir);

bool elk_nir_limit_trig_input_range_workaround(nir_shader *nir);

void elk_nir_apply_key(nir_shader *nir,
                       const struct elk_compiler *compiler,
                       const struct elk_base_prog_key *key,
                       unsigned max_subgroup_size);

unsigned elk_nir_api_subgroup_size(const nir_shader *nir,
                                   unsigned hw_subgroup_size);

void elk_nir_analyze_ubo_ranges(const struct elk_compiler *compiler,
                                nir_shader *nir,
                                struct elk_ubo_range out_ranges[4]);

void elk_nir_optimize(nir_shader *nir, bool is_scalar,
                      const struct intel_device_info *devinfo);

nir_shader *elk_nir_create_passthrough_tcs(void *mem_ctx,
                                           const struct elk_compiler *compiler,
                                           const struct elk_tcs_prog_key *key);

#define ELK_NIR_FRAG_OUTPUT_INDEX_SHIFT 0
#define ELK_NIR_FRAG_OUTPUT_INDEX_MASK INTEL_MASK(0, 0)
#define ELK_NIR_FRAG_OUTPUT_LOCATION_SHIFT 1
#define ELK_NIR_FRAG_OUTPUT_LOCATION_MASK INTEL_MASK(31, 1)

bool elk_nir_move_interpolation_to_top(nir_shader *nir);
nir_def *elk_nir_load_global_const(nir_builder *b,
                                       nir_intrinsic_instr *load_uniform,
                                       nir_def *base_addr,
                                       unsigned off);

const struct glsl_type *elk_nir_get_var_type(const struct nir_shader *nir,
                                             nir_variable *var);

void elk_nir_adjust_payload(nir_shader *shader);

nir_shader *
elk_nir_from_spirv(void *mem_ctx, const uint32_t *spirv, size_t spirv_size);

#ifdef __cplusplus
}
#endif
