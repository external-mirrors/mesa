/*
 * Copyright © 2023 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_gpu_info.h"
#include "ac_nir.h"
#include "nir.h"
#include "nir_builder.h"
#include "nir_deref.h"
#include "radv_constants.h"
#include "radv_nir.h"
#include "radv_shader.h"
#include "radv_shader_args.h"

typedef struct {
   const struct radv_shader_args *args;
   const struct radv_shader_info *info;
   const struct radv_graphics_state_key *gfx_state;
   const struct radeon_info *gpu_info;
} lower_vs_inputs_state;

static nir_def *
lower_load_vs_input_from_prolog(nir_builder *b, nir_intrinsic_instr *intrin, lower_vs_inputs_state *s)
{
   nir_src *offset_src = nir_get_io_offset_src(intrin);
   assert(nir_src_is_const(*offset_src));

   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   const unsigned base_offset = nir_src_as_uint(*offset_src);
   const unsigned location = io_sem.location + base_offset - VERT_ATTRIB_GENERIC0;
   const unsigned component = nir_intrinsic_component(intrin);
   const unsigned bit_size = intrin->def.bit_size;
   const unsigned num_components = intrin->def.num_components;

   /* 64-bit inputs: they occupy twice as many 32-bit components.
    * 16-bit inputs: they occupy a 32-bit component (not packed).
    */
   const unsigned arg_bit_size = MAX2(bit_size, 32);

   unsigned num_input_args = 1;
   nir_def *input_args[2] = {ac_nir_load_arg(b, &s->args->ac, s->args->vs_inputs[location]), NULL};
   if (component * 32 + arg_bit_size * num_components > 128) {
      assert(bit_size == 64);

      num_input_args++;
      input_args[1] = ac_nir_load_arg(b, &s->args->ac, s->args->vs_inputs[location + 1]);
   }

   nir_def *extracted = nir_extract_bits(b, input_args, num_input_args, component * 32, num_components, arg_bit_size);

   if (bit_size < arg_bit_size) {
      assert(bit_size == 16);

      if (nir_alu_type_get_base_type(nir_intrinsic_dest_type(intrin)) == nir_type_float)
         return nir_f2f16(b, extracted);
      else
         return nir_u2u16(b, extracted);
   }

   return extracted;
}

static nir_def *
calc_vs_input_index_instance_rate(nir_builder *b, unsigned location, lower_vs_inputs_state *s)
{
   const uint32_t divisor = s->gfx_state->vi.instance_rate_divisors[location];
   nir_def *start_instance = nir_load_base_instance(b);

   if (divisor == 0)
      return start_instance;

   nir_def *instance_id = nir_udiv_imm(b, nir_load_instance_id(b), divisor);
   return nir_iadd(b, start_instance, instance_id);
}

static nir_def *
calc_vs_input_index(nir_builder *b, unsigned location, lower_vs_inputs_state *s)
{
   if (s->gfx_state->vi.instance_rate_inputs & BITFIELD_BIT(location))
      return calc_vs_input_index_instance_rate(b, location, s);

   return nir_iadd(b, nir_load_first_vertex(b), nir_load_vertex_id_zero_base(b));
}

static bool
can_use_untyped_load(const struct util_format_description *f, const unsigned bit_size)
{
   /* All components must have same size and type. */
   if (!f->is_array)
      return false;

   const struct util_format_channel_description *c = &f->channel[0];
   return c->size == bit_size && bit_size >= 32;
}

static nir_def *
oob_input_load_value(nir_builder *b, const unsigned channel_idx, const unsigned bit_size, const bool is_float)
{
   /* 22.1.1. Attribute Location and Component Assignment of Vulkan 1.3 specification:
    * For 64-bit data types, no default attribute values are provided. Input variables
    * must not use more components than provided by the attribute.
    */
   if (bit_size == 64)
      return nir_undef(b, 1, bit_size);

   if (channel_idx == 3) {
      if (is_float)
         return nir_imm_floatN_t(b, 1.0, bit_size);
      else
         return nir_imm_intN_t(b, 1, bit_size);
   }

   return nir_imm_intN_t(b, 0, bit_size);
}

static unsigned
count_format_bytes(const struct util_format_description *f, const unsigned first_channel, const unsigned num_channels)
{
   if (!num_channels)
      return 0;

   const unsigned last_channel = first_channel + num_channels - 1;
   assert(last_channel < f->nr_channels);
   unsigned bits = 0;
   for (unsigned i = first_channel; i <= last_channel; ++i) {
      bits += f->channel[i].size;
   }

   assert(bits % 8 == 0);
   return bits / 8;
}

static bool
format_needs_swizzle(const struct util_format_description *f)
{
   for (unsigned i = 0; i < f->nr_channels; ++i) {
      if (f->swizzle[i] != PIPE_SWIZZLE_X + i)
         return true;
   }

   return false;
}

static unsigned
first_used_swizzled_channel(const struct util_format_description *f, const unsigned mask, const bool backwards)
{
   unsigned first_used = backwards ? 0 : f->nr_channels;
   const unsigned it_mask = mask & BITFIELD_MASK(f->nr_channels);

   u_foreach_bit (b, it_mask) {
      assert(f->swizzle[b] != PIPE_SWIZZLE_0 && f->swizzle[b] != PIPE_SWIZZLE_1);
      const unsigned c = f->swizzle[b] - PIPE_SWIZZLE_X;
      first_used = backwards ? MAX2(first_used, c) : MIN2(first_used, c);
   }

   return first_used;
}

static nir_def *
adjust_vertex_fetch_alpha(nir_builder *b, enum ac_vs_input_alpha_adjust alpha_adjust, nir_def *alpha)
{
   if (alpha_adjust == AC_ALPHA_ADJUST_SSCALED)
      alpha = nir_f2u32(b, alpha);

   /* For the integer-like cases, do a natural sign extension.
    *
    * For the SNORM case, the values are 0.0, 0.333, 0.666, 1.0 and happen to contain 0, 1, 2, 3 as
    * the two LSBs of the exponent.
    */
   unsigned offset = alpha_adjust == AC_ALPHA_ADJUST_SNORM ? 23u : 0u;

   alpha = nir_ibfe_imm(b, alpha, offset, 2u);

   /* Convert back to the right type. */
   if (alpha_adjust == AC_ALPHA_ADJUST_SNORM) {
      alpha = nir_i2f32(b, alpha);
      alpha = nir_fmax(b, alpha, nir_imm_float(b, -1.0f));
   } else if (alpha_adjust == AC_ALPHA_ADJUST_SSCALED) {
      alpha = nir_i2f32(b, alpha);
   }

   return alpha;
}

static nir_def *
lower_load_vs_input(nir_builder *b, nir_intrinsic_instr *intrin, lower_vs_inputs_state *s)
{
   nir_src *offset_src = nir_get_io_offset_src(intrin);
   assert(nir_src_is_const(*offset_src));

   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   const unsigned base_offset = nir_src_as_uint(*offset_src);
   const unsigned location = io_sem.location + base_offset - VERT_ATTRIB_GENERIC0;
   const unsigned bit_size = intrin->def.bit_size;
   const unsigned dest_num_components = intrin->def.num_components;

   if (!(s->gfx_state->vi.attributes_valid & (1 << location))) {
      /* Return early for unassigned attribute reads. */
      return nir_imm_zero(b, intrin->def.num_components, intrin->def.bit_size);
   }

   /* Convert the component offset to bit_size units.
    * (Intrinsic component offset is in 32-bit units.)
    *
    * Small bitsize inputs consume the same space as 32-bit inputs,
    * but 64-bit inputs consume twice as many.
    * 64-bit variables must not have a component of 1 or 3.
    * (See VK spec 15.1.5 "Component Assignment")
    */
   const unsigned component = nir_intrinsic_component(intrin) / (MAX2(32, bit_size) / 32);

   /* Bitmask of components in bit_size units
    * of the current input load that are actually used.
    */
   const unsigned dest_use_mask = nir_def_components_read(&intrin->def) << component;

   /* If the input is entirely unused, just replace it with undef.
    * This is just in case we debug this pass without running DCE first.
    */
   if (!dest_use_mask)
      return nir_undef(b, dest_num_components, bit_size);

   const uint32_t attrib_binding = s->gfx_state->vi.vertex_attribute_bindings[location];
   const uint32_t attrib_offset = s->gfx_state->vi.vertex_attribute_offsets[location];
   const uint32_t attrib_stride = s->gfx_state->vi.vertex_attribute_strides[location];
   const enum pipe_format attrib_format = s->gfx_state->vi.vertex_attribute_formats[location];
   const struct util_format_description *f = util_format_description(attrib_format);
   const struct ac_vtx_format_info *vtx_info =
      ac_get_vtx_format_info(s->gpu_info->gfx_level, s->gpu_info->family, attrib_format);
   const unsigned binding_index = s->info->vs.use_per_attribute_vb_descs ? location : attrib_binding;
   const unsigned desc_index = util_bitcount(s->info->vs.vb_desc_usage_mask & BITFIELD_MASK(binding_index));

   nir_def *vertex_buffers_arg = ac_nir_load_arg(b, &s->args->ac, s->args->ac.vertex_buffers);
   nir_def *vertex_buffers = nir_pack_64_2x32_split(b, vertex_buffers_arg, nir_imm_int(b, s->gpu_info->address32_hi));
   nir_def *descriptor = nir_load_smem_amd(b, 4, vertex_buffers, nir_imm_int(b, desc_index * 16),
                                           .access = ACCESS_CAN_SPECULATE);
   nir_def *base_index = calc_vs_input_index(b, location, s);
   nir_def *zero = nir_imm_int(b, 0);

   /* We currently implement swizzling for all formats in shaders.
    * Note, it is possible to specify swizzling in the DST_SEL fields of descriptors,
    * but we don't use that because typed loads using the MTBUF instruction format
    * don't support DST_SEL, so it's simpler to just handle it all in shaders.
    */
   const bool needs_swizzle = format_needs_swizzle(f);

   /* We need to adjust the alpha channel as loaded by the HW,
    * for example sign extension and normalization may be necessary.
    */
   const enum ac_vs_input_alpha_adjust alpha_adjust = vtx_info->alpha_adjust;

   /* Try to shrink the load format by skipping unused components from the start.
    * Beneficial because the backend may be able to emit fewer HW instructions.
    * Only possible with array formats.
    */
   const unsigned first_used_channel = first_used_swizzled_channel(f, dest_use_mask, false);
   const unsigned skipped_start = f->is_array ? first_used_channel : 0;

   /* Number of channels we actually use and load.
    * Don't shrink the format here because this might allow the backend to
    * emit fewer (but larger than needed) HW instructions.
    */
   const unsigned first_trailing_unused_channel = first_used_swizzled_channel(f, dest_use_mask, true) + 1;
   const unsigned max_loaded_channels = MIN2(first_trailing_unused_channel, f->nr_channels);
   const unsigned fetch_num_channels =
      first_used_channel >= max_loaded_channels ? 0 : max_loaded_channels - skipped_start;

   /* Load VS inputs from VRAM.
    *
    * For the vast majority of cases this will only create 1x load_(typed)_buffer_amd
    * intrinsic and the backend is responsible for further splitting that
    * to as many HW instructions as needed based on alignment.
    *
    * Take care to prevent loaded components from failing the range check,
    * by emitting several load intrinsics with different index sources.
    * This is necessary because the backend can't further roll the const offset
    * into the index source of MUBUF / MTBUF instructions.
    */
   nir_def *loads[NIR_MAX_VEC_COMPONENTS] = {0};
   unsigned num_loads = 0;
   for (unsigned x = 0, channels; x < fetch_num_channels; x += channels) {
      channels = fetch_num_channels - x;
      const unsigned start = skipped_start + x;
      enum pipe_format fetch_format = attrib_format;
      nir_def *index = base_index;

      /* Add excess constant offset to the index. */
      unsigned const_off = attrib_offset + count_format_bytes(f, 0, start);
      if (attrib_stride && const_off >= attrib_stride) {
         index = nir_iadd_imm(b, base_index, const_off / attrib_stride);
         const_off %= attrib_stride;
      }

      /* Reduce the number of loaded channels until we can pass the range check.
       * Only for array formats. VK spec mandates proper alignment for packed formats.
       * Note, NONE seems to occur in real use and is considered an array format.
       */
      if (f->is_array && fetch_format != PIPE_FORMAT_NONE) {
         while (channels > 1 && attrib_stride && (const_off + count_format_bytes(f, start, channels)) > attrib_stride) {
            channels--;
         }

         /* Keep the fetch format as large as possible to let the backend emit
          * larger load instructions when it deems them beneficial.
          */
         fetch_format = util_format_get_array(f->channel[0].type, f->channel[0].size, f->nr_channels - start,
                                              f->is_unorm || f->is_snorm, f->channel[0].pure_integer);
      }

      assert(f->is_array || channels == fetch_num_channels);

      unsigned align_mul = MAX2(1, s->gfx_state->vi.vertex_binding_align[attrib_binding]);
      unsigned align_offset = const_off % align_mul;

      /* The alignment might be lower than the minimum if it's unknown. */
      const unsigned min_channel_align = vtx_info->chan_byte_size ? vtx_info->chan_byte_size : vtx_info->element_size;
      if (nir_combined_align(align_mul, align_offset) < min_channel_align) {
         align_mul = min_channel_align;
         align_offset = 0;
      }

      /* Prefer using untyped buffer loads if possible, to avoid potential alignment issues.
       * Typed loads can cause GPU hangs when used with improper alignment.
       */
      if (can_use_untyped_load(f, bit_size)) {
         loads[num_loads++] = nir_load_buffer_amd(b, channels, bit_size, descriptor, zero, zero, index,
                                                  .base = const_off, .memory_modes = nir_var_shader_in,
                                                  .align_mul = align_mul, .align_offset = align_offset,
                                                  .access = ACCESS_CAN_REORDER | ACCESS_CAN_SPECULATE);
      } else {
         loads[num_loads++] = nir_load_typed_buffer_amd(
            b, channels, bit_size, descriptor, zero, zero, index, .base = const_off, .format = fetch_format,
            .align_mul = align_mul, .align_offset = align_offset, .memory_modes = nir_var_shader_in,
            .access = ACCESS_CAN_REORDER | ACCESS_CAN_SPECULATE);
      }
   }

   nir_def *load = loads[0];

   /* Extract the channels we actually need when we couldn't skip starting
    * components or had to emit more than one load intrinsic.
    */
   if (num_loads > 0 && (first_used_channel > skipped_start || num_loads != 1))
      load = nir_extract_bits(b, loads, num_loads, (first_used_channel - skipped_start) * bit_size,
                              max_loaded_channels - first_used_channel, bit_size);

   /* Return early if possible to avoid generating unnecessary IR. */
   if (num_loads > 0 && first_used_channel == component && load->num_components == dest_num_components &&
       !needs_swizzle && alpha_adjust == AC_ALPHA_ADJUST_NONE)
      return load;

   /* Fill unused and OOB components.
    * Apply swizzle and alpha adjust according to the format.
    */
   const nir_alu_type dst_type = nir_alu_type_get_base_type(nir_intrinsic_dest_type(intrin));
   nir_def *channels[NIR_MAX_VEC_COMPONENTS] = {0};
   for (unsigned i = 0; i < dest_num_components; ++i) {
      const unsigned c = i + component;

      if (!(dest_use_mask & BITFIELD_BIT(c))) {
         /* Fill unused channels with zero. */
         channels[i] = nir_imm_zero(b, 1, bit_size);
         continue;
      }

      const unsigned sw = f->swizzle[c];
      assert(sw >= first_used_channel);
      const unsigned loaded_channel = sw - first_used_channel;

      if (load && loaded_channel < load->num_components) {
         /* Use channels that were loaded from VRAM. */
         channels[i] = nir_channel(b, load, loaded_channel);

         if (alpha_adjust != AC_ALPHA_ADJUST_NONE && c == 3)
            channels[i] = adjust_vertex_fetch_alpha(b, alpha_adjust, channels[i]);
      } else {
         /* Handle input loads that are larger than their format. */
         channels[i] = oob_input_load_value(b, c, bit_size, dst_type == nir_type_float);
      }
   }

   return nir_vec(b, channels, dest_num_components);
}

static bool
lower_vs_input_instr(nir_builder *b, nir_intrinsic_instr *intrin, void *state)
{
   if (intrin->intrinsic != nir_intrinsic_load_input)
      return false;

   lower_vs_inputs_state *s = (lower_vs_inputs_state *)state;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *replacement = NULL;

   if (s->info->vs.dynamic_inputs) {
      replacement = lower_load_vs_input_from_prolog(b, intrin, s);
   } else {
      replacement = lower_load_vs_input(b, intrin, s);
   }

   nir_def_replace(&intrin->def, replacement);
   nir_instr_free(&intrin->instr);

   return true;
}

bool
radv_nir_lower_vs_inputs(nir_shader *shader, const struct radv_shader_stage *vs_stage,
                         const struct radv_graphics_state_key *gfx_state, const struct radeon_info *gpu_info)
{
   assert(shader->info.stage == MESA_SHADER_VERTEX);

   lower_vs_inputs_state state = {
      .info = &vs_stage->info,
      .args = &vs_stage->args,
      .gfx_state = gfx_state,
      .gpu_info = gpu_info,
   };

   return nir_shader_intrinsics_pass(shader, lower_vs_input_instr, nir_metadata_control_flow, &state);
}

static void
type_size_vec4(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   *size = glsl_count_attribute_slots(type, false);
   *align = 1;
}

static bool
opt_vs_input_to_const(nir_builder *b, nir_intrinsic_instr *intrin, void *state)
{
   const struct radv_graphics_state_key *gfx_state = state;

   if (intrin->intrinsic != nir_intrinsic_load_deref)
      return false;

   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   if (!nir_deref_mode_is(deref, nir_var_shader_in) || nir_deref_instr_has_indirect(deref))
      return false;

   const nir_variable *var = nir_deref_instr_get_variable(deref);
   const unsigned location =
      var->data.location + nir_deref_instr_get_const_offset(deref, &type_size_vec4) - VERT_ATTRIB_GENERIC0;
   const bool is_integer = glsl_base_type_is_integer(glsl_get_base_type(deref->type));
   const unsigned bit_size = intrin->def.bit_size;
   const unsigned component = var->data.location_frac >> (bit_size == 64 ? 1 : 0);

   b->cursor = nir_after_instr(&intrin->instr);

   nir_def *res = &intrin->def;

   if (gfx_state->vi.attributes_valid & (1 << location)) {
      const enum pipe_format attrib_format = gfx_state->vi.vertex_attribute_formats[location];
      const struct util_format_description *f = util_format_description(attrib_format);

      for (unsigned i = 0; i < intrin->def.num_components; i++) {
         const unsigned c = i + component;
         if (f->swizzle[c] >= f->nr_channels) {
            /* Handle input loads that are larger than their format. */
            nir_def *channel = oob_input_load_value(b, c, bit_size, !is_integer);
            res = nir_vector_insert_imm(b, res, channel, i);
         }
      }
   } else {
      /* Use (0,0,0,0) for unassigned attribute reads. */
      res = nir_imm_zero(b, intrin->def.num_components, intrin->def.bit_size);
   }

   if (res != &intrin->def) {
      nir_def_rewrite_uses_after(&intrin->def, res);
      return true;
   } else {
      return false;
   }
}

bool
radv_nir_optimize_vs_inputs_to_const(nir_shader *shader, const struct radv_graphics_state_key *gfx_state)
{
   assert(shader->info.stage == MESA_SHADER_VERTEX);
   return nir_shader_intrinsics_pass(shader, opt_vs_input_to_const, nir_metadata_control_flow, (void *)gfx_state);
}
