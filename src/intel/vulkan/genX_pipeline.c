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

#include "anv_private.h"

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"
#include "genxml/genX_rt_pack.h"

#include "common/intel_compute_slm.h"
#include "common/intel_common.h"
#include "common/intel_genX_state_brw.h"
#include "common/intel_l3_config.h"
#include "common/intel_sample_positions.h"
#include "nir/nir_xfb_info.h"
#include "vk_util.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_render_pass.h"

static inline struct anv_batch *
anv_gfx_pipeline_add(struct anv_graphics_pipeline *pipeline,
                     struct anv_gfx_state_ptr *ptr,
                     uint32_t n_dwords)
{
   struct anv_batch *batch = &pipeline->base.base.batch;

   assert(ptr->len == 0 ||
          (batch->next - batch->start) / 4 == (ptr->offset + ptr->len));
   if (ptr->len == 0)
      ptr->offset = (batch->next - batch->start) / 4;
   ptr->len += n_dwords;

   return batch;
}

#define anv_pipeline_emit_tmp(pipeline, field, cmd, name)               \
   for (struct cmd name = { __anv_cmd_header(cmd) },                    \
           *_dst = (void *) field;                                      \
        __builtin_expect(_dst != NULL, 1);                              \
        ({ __anv_cmd_pack(cmd)(&(pipeline)->base.base.batch,            \
                               _dst, &name);                            \
           VG(VALGRIND_CHECK_MEM_IS_DEFINED(_dst, __anv_cmd_length(cmd) * 4)); \
           _dst = NULL;                                                 \
        }))

#define anv_pipeline_emit(pipeline, state, cmd, name)                   \
   for (struct cmd name = { __anv_cmd_header(cmd) },                    \
           *_dst = anv_batch_emit_dwords(                               \
              anv_gfx_pipeline_add(pipeline,                            \
                                   &(pipeline)->state,                  \
                                   __anv_cmd_length(cmd)),              \
              __anv_cmd_length(cmd));                                   \
        __builtin_expect(_dst != NULL, 1);                              \
        ({ __anv_cmd_pack(cmd)(&(pipeline)->base.base.batch,            \
                               _dst, &name);                            \
           VG(VALGRIND_CHECK_MEM_IS_DEFINED(_dst, __anv_cmd_length(cmd) * 4)); \
           _dst = NULL;                                                 \
        }))

#define anv_pipeline_emit_merge(pipeline, state, dwords, cmd, name) \
   for (struct cmd name = { 0 },                                        \
           *_dst = anv_batch_emit_dwords(                               \
              anv_gfx_pipeline_add(pipeline,                            \
                                   &(pipeline)->state,                  \
                                   __anv_cmd_length(cmd)),              \
              __anv_cmd_length(cmd));                                   \
        __builtin_expect(_dst != NULL, 1);                              \
        ({ uint32_t _partial[__anv_cmd_length(cmd)];                    \
           assert((pipeline)->state.len == __anv_cmd_length(cmd));      \
           __anv_cmd_pack(cmd)(&(pipeline)->base.base.batch,            \
                               _partial, &name);                        \
           for (uint32_t i = 0; i < __anv_cmd_length(cmd); i++) {       \
              ((uint32_t *)_dst)[i] = _partial[i] | dwords[i];          \
           }                                                            \
           VG(VALGRIND_CHECK_MEM_IS_DEFINED(_dst, __anv_cmd_length(cmd) * 4)); \
           _dst = NULL;                                                 \
         }))

#define anv_pipeline_emitn(pipeline, state, n, cmd, ...) ({             \
   void *__dst = anv_batch_emit_dwords(                                 \
      anv_gfx_pipeline_add(pipeline, &(pipeline)->state, n), n);        \
   if (__dst) {                                                         \
      struct cmd __template = {                                         \
         __anv_cmd_header(cmd),                                         \
         .DWordLength = n - __anv_cmd_length_bias(cmd),                 \
         __VA_ARGS__                                                    \
      };                                                                \
      __anv_cmd_pack(cmd)(&pipeline->base.base.batch,                   \
                          __dst, &__template);                          \
   }                                                                    \
   __dst;                                                               \
   })

#define pipeline_needs_protected(pipeline) \
   ((pipeline)->device->vk.enabled_features.protectedMemory)

static uint32_t
vertex_element_comp_control(enum isl_format format, unsigned comp)
{
   uint8_t bits;
   switch (comp) {
   case 0: bits = isl_format_layouts[format].channels.r.bits; break;
   case 1: bits = isl_format_layouts[format].channels.g.bits; break;
   case 2: bits = isl_format_layouts[format].channels.b.bits; break;
   case 3: bits = isl_format_layouts[format].channels.a.bits; break;
   default: UNREACHABLE("Invalid component");
   }

   /*
    * Take in account hardware restrictions when dealing with 64-bit floats.
    *
    * From Broadwell spec, command reference structures, page 586:
    *  "When SourceElementFormat is set to one of the *64*_PASSTHRU formats,
    *   64-bit components are stored * in the URB without any conversion. In
    *   this case, vertex elements must be written as 128 or 256 bits, with
    *   VFCOMP_STORE_0 being used to pad the output as required. E.g., if
    *   R64_PASSTHRU is used to copy a 64-bit Red component into the URB,
    *   Component 1 must be specified as VFCOMP_STORE_0 (with Components 2,3
    *   set to VFCOMP_NOSTORE) in order to output a 128-bit vertex element, or
    *   Components 1-3 must be specified as VFCOMP_STORE_0 in order to output
    *   a 256-bit vertex element. Likewise, use of R64G64B64_PASSTHRU requires
    *   Component 3 to be specified as VFCOMP_STORE_0 in order to output a
    *   256-bit vertex element."
    */
   if (bits) {
      return VFCOMP_STORE_SRC;
   } else if (comp >= 2 &&
              !isl_format_layouts[format].channels.b.bits &&
              isl_format_layouts[format].channels.r.type == ISL_RAW) {
      /* When emitting 64-bit attributes, we need to write either 128 or 256
       * bit chunks, using VFCOMP_NOSTORE when not writing the chunk, and
       * VFCOMP_STORE_0 to pad the written chunk */
      return VFCOMP_NOSTORE;
   } else if (comp < 3 ||
              isl_format_layouts[format].channels.r.type == ISL_RAW) {
      /* Note we need to pad with value 0, not 1, due hardware restrictions
       * (see comment above) */
      return VFCOMP_STORE_0;
   } else if (isl_format_layouts[format].channels.r.type == ISL_UINT ||
            isl_format_layouts[format].channels.r.type == ISL_SINT) {
      assert(comp == 3);
      return VFCOMP_STORE_1_INT;
   } else {
      assert(comp == 3);
      return VFCOMP_STORE_1_FP;
   }
}

static void
emit_ves_vf_instancing(struct anv_batch *batch,
                       uint32_t *vertex_element_dws,
                       struct anv_graphics_pipeline *pipeline,
                       const struct vk_vertex_input_state *vi,
                       bool emit_in_pipeline)
{
   const struct anv_device *device = pipeline->base.base.device;
   const struct brw_vs_prog_data *vs_prog_data =
      get_pipeline_vs_prog_data(pipeline);
   const uint64_t inputs_read = vs_prog_data->inputs_read;
   const uint64_t double_inputs_read =
      vs_prog_data->double_inputs_read & inputs_read;
   assert((inputs_read & ((1 << VERT_ATTRIB_GENERIC0) - 1)) == 0);
   const uint32_t elements = inputs_read >> VERT_ATTRIB_GENERIC0;
   const uint32_t elements_double = double_inputs_read >> VERT_ATTRIB_GENERIC0;

   for (uint32_t i = 0; i < pipeline->vs_input_elements; i++) {
      /* The SKL docs for VERTEX_ELEMENT_STATE say:
       *
       *    "All elements must be valid from Element[0] to the last valid
       *    element. (I.e. if Element[2] is valid then Element[1] and
       *    Element[0] must also be valid)."
       *
       * The SKL docs for 3D_Vertex_Component_Control say:
       *
       *    "Don't store this component. (Not valid for Component 0, but can
       *    be used for Component 1-3)."
       *
       * So we can't just leave a vertex element blank and hope for the best.
       * We have to tell the VF hardware to put something in it; so we just
       * store a bunch of zero.
       *
       * TODO: Compact vertex elements so we never end up with holes.
       */
      struct GENX(VERTEX_ELEMENT_STATE) element = {
         .Valid = true,
         .Component0Control = VFCOMP_STORE_0,
         .Component1Control = VFCOMP_STORE_0,
         .Component2Control = VFCOMP_STORE_0,
         .Component3Control = VFCOMP_STORE_0,
      };
      GENX(VERTEX_ELEMENT_STATE_pack)(NULL,
                                      &vertex_element_dws[i * 2],
                                      &element);
   }

   u_foreach_bit(a, vi->attributes_valid) {
      enum isl_format format = anv_get_vbo_format(device->physical,
                                                  vi->attributes[a].format,
                                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                                  VK_IMAGE_TILING_LINEAR);
      assume(format < ISL_NUM_FORMATS);

      uint32_t binding = vi->attributes[a].binding;
      assert(binding < get_max_vbs(device->info));

      if ((elements & (1 << a)) == 0)
         continue; /* Binding unused */

      uint32_t slot =
         __builtin_popcount(elements & ((1 << a) - 1)) -
         DIV_ROUND_UP(__builtin_popcount(elements_double &
                                        ((1 << a) -1)), 2);

      struct GENX(VERTEX_ELEMENT_STATE) element = {
         .VertexBufferIndex = vi->attributes[a].binding,
         .Valid = true,
         .SourceElementFormat = format,
         .EdgeFlagEnable = false,
         .SourceElementOffset = vi->attributes[a].offset,
         .Component0Control = vertex_element_comp_control(format, 0),
         .Component1Control = vertex_element_comp_control(format, 1),
         .Component2Control = vertex_element_comp_control(format, 2),
         .Component3Control = vertex_element_comp_control(format, 3),
      };
      GENX(VERTEX_ELEMENT_STATE_pack)(NULL,
                                      &vertex_element_dws[slot * 2],
                                      &element);

      /* On Broadwell and later, we have a separate VF_INSTANCING packet
       * that controls instancing.  On Haswell and prior, that's part of
       * VERTEX_BUFFER_STATE which we emit later.
       */
      if (emit_in_pipeline) {
         anv_pipeline_emit(pipeline, final.vf_instancing, GENX(3DSTATE_VF_INSTANCING), vfi) {
            bool per_instance = vi->bindings[binding].input_rate ==
               VK_VERTEX_INPUT_RATE_INSTANCE;
            uint32_t divisor = vi->bindings[binding].divisor *
               pipeline->instance_multiplier;

            vfi.InstancingEnable = per_instance;
            vfi.VertexElementIndex = slot;
            vfi.InstanceDataStepRate = per_instance ? divisor : 1;
         }
      } else {
         anv_batch_emit(batch, GENX(3DSTATE_VF_INSTANCING), vfi) {
            bool per_instance = vi->bindings[binding].input_rate ==
               VK_VERTEX_INPUT_RATE_INSTANCE;
            uint32_t divisor = vi->bindings[binding].divisor *
               pipeline->instance_multiplier;

            vfi.InstancingEnable = per_instance;
            vfi.VertexElementIndex = slot;
            vfi.InstanceDataStepRate = per_instance ? divisor : 1;
         }
      }
   }
}

void
genX(batch_emit_pipeline_vertex_input)(struct anv_batch *batch,
                                       struct anv_device *device,
                                       struct anv_graphics_pipeline *pipeline,
                                       const struct vk_vertex_input_state *vi)
{
   const uint32_t ve_count =
      pipeline->vs_input_elements + pipeline->svgs_count;
   const uint32_t num_dwords = 1 + 2 * MAX2(1, ve_count);
   uint32_t *p = anv_batch_emitn(batch, num_dwords,
                                 GENX(3DSTATE_VERTEX_ELEMENTS));
   if (p == NULL)
      return;

   if (ve_count == 0) {
      memcpy(p + 1, device->physical->empty_vs_input,
             sizeof(device->physical->empty_vs_input));
   } else if (ve_count == pipeline->vertex_input_elems) {
      /* MESA_VK_DYNAMIC_VI is not dynamic for this pipeline, so everything is
       * in pipeline->vertex_input_data and we can just memcpy
       */
      memcpy(p + 1, pipeline->vertex_input_data, 4 * 2 * ve_count);
      anv_batch_emit_pipeline_state(batch, pipeline, final.vf_instancing);
   } else {
      assert(pipeline->final.vf_instancing.len == 0);
      /* Use dyn->vi to emit the dynamic VERTEX_ELEMENT_STATE input. */
      emit_ves_vf_instancing(batch, p + 1, pipeline, vi,
                             false /* emit_in_pipeline */);
      /* Then append the VERTEX_ELEMENT_STATE for the draw parameters */
      memcpy(p + 1 + 2 * pipeline->vs_input_elements,
             pipeline->vertex_input_data,
             4 * 2 * pipeline->vertex_input_elems);
   }
}

static void
emit_vertex_input(struct anv_graphics_pipeline *pipeline,
                  const struct vk_graphics_pipeline_state *state,
                  const struct vk_vertex_input_state *vi)
{
   /* Only pack the VERTEX_ELEMENT_STATE if not dynamic so we can just memcpy
    * everything in gfx8_cmd_buffer.c
    */
   if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VI)) {
      emit_ves_vf_instancing(NULL,
                             pipeline->vertex_input_data,
                             pipeline, vi, true /* emit_in_pipeline */);
   }

   const struct brw_vs_prog_data *vs_prog_data = get_pipeline_vs_prog_data(pipeline);
   const bool needs_svgs_elem = pipeline->svgs_count > 1 ||
                                !vs_prog_data->uses_drawid;
   const uint32_t id_slot = pipeline->vs_input_elements;
   const uint32_t drawid_slot = id_slot + needs_svgs_elem;
   if (pipeline->svgs_count > 0) {
      assert(pipeline->vertex_input_elems >= pipeline->svgs_count);
      uint32_t slot_offset =
         pipeline->vertex_input_elems - pipeline->svgs_count;

      if (needs_svgs_elem) {
#if GFX_VER < 11
         /* From the Broadwell PRM for the 3D_Vertex_Component_Control enum:
          *    "Within a VERTEX_ELEMENT_STATE structure, if a Component
          *    Control field is set to something other than VFCOMP_STORE_SRC,
          *    no higher-numbered Component Control fields may be set to
          *    VFCOMP_STORE_SRC"
          *
          * This means, that if we have BaseInstance, we need BaseVertex as
          * well.  Just do all or nothing.
          */
         uint32_t base_ctrl = (vs_prog_data->uses_firstvertex ||
                               vs_prog_data->uses_baseinstance) ?
                              VFCOMP_STORE_SRC : VFCOMP_STORE_0;
#endif

         struct GENX(VERTEX_ELEMENT_STATE) element = {
            .VertexBufferIndex = ANV_SVGS_VB_INDEX,
            .Valid = true,
            .SourceElementFormat = ISL_FORMAT_R32G32_UINT,
#if GFX_VER >= 11
            /* On gen11, these are taken care of by extra parameter slots */
            .Component0Control = VFCOMP_STORE_0,
            .Component1Control = VFCOMP_STORE_0,
#else
            .Component0Control = base_ctrl,
            .Component1Control = base_ctrl,
#endif
            .Component2Control = VFCOMP_STORE_0,
            .Component3Control = VFCOMP_STORE_0,
         };
         GENX(VERTEX_ELEMENT_STATE_pack)(NULL,
                                         &pipeline->vertex_input_data[slot_offset * 2],
                                         &element);
         slot_offset++;

         anv_pipeline_emit(pipeline, final.vf_sgvs_instancing,
                           GENX(3DSTATE_VF_INSTANCING), vfi) {
            vfi.VertexElementIndex = id_slot;
         }
      }

      if (vs_prog_data->uses_drawid) {
         struct GENX(VERTEX_ELEMENT_STATE) element = {
            .VertexBufferIndex = ANV_DRAWID_VB_INDEX,
            .Valid = true,
            .SourceElementFormat = ISL_FORMAT_R32_UINT,
#if GFX_VER >= 11
            /* On gen11, this is taken care of by extra parameter slots */
            .Component0Control = VFCOMP_STORE_0,
#else
            .Component0Control = VFCOMP_STORE_SRC,
#endif
            .Component1Control = VFCOMP_STORE_0,
            .Component2Control = VFCOMP_STORE_0,
            .Component3Control = VFCOMP_STORE_0,
         };
         GENX(VERTEX_ELEMENT_STATE_pack)(NULL,
                                         &pipeline->vertex_input_data[slot_offset * 2],
                                         &element);
         slot_offset++;

         anv_pipeline_emit(pipeline, final.vf_sgvs_instancing,
                           GENX(3DSTATE_VF_INSTANCING), vfi) {
            vfi.VertexElementIndex = drawid_slot;
         }
      }
   }

   anv_pipeline_emit(pipeline, final.vf_sgvs, GENX(3DSTATE_VF_SGVS), sgvs) {
      sgvs.VertexIDEnable              = vs_prog_data->uses_vertexid;
      sgvs.VertexIDComponentNumber     = 2;
      sgvs.VertexIDElementOffset       = id_slot;
      sgvs.InstanceIDEnable            = vs_prog_data->uses_instanceid;
      sgvs.InstanceIDComponentNumber   = 3;
      sgvs.InstanceIDElementOffset     = id_slot;
   }

#if GFX_VER >= 11
   anv_pipeline_emit(pipeline, final.vf_sgvs_2, GENX(3DSTATE_VF_SGVS_2), sgvs) {
      /* gl_BaseVertex */
      sgvs.XP0Enable                   = vs_prog_data->uses_firstvertex;
      sgvs.XP0SourceSelect             = XP0_PARAMETER;
      sgvs.XP0ComponentNumber          = 0;
      sgvs.XP0ElementOffset            = id_slot;

      /* gl_BaseInstance */
      sgvs.XP1Enable                   = vs_prog_data->uses_baseinstance;
      sgvs.XP1SourceSelect             = StartingInstanceLocation;
      sgvs.XP1ComponentNumber          = 1;
      sgvs.XP1ElementOffset            = id_slot;

      /* gl_DrawID */
      sgvs.XP2Enable                   = vs_prog_data->uses_drawid;
      sgvs.XP2ComponentNumber          = 0;
      sgvs.XP2ElementOffset            = drawid_slot;
   }
#endif

   if (pipeline->base.base.device->physical->instance->vf_component_packing) {
      anv_pipeline_emit(pipeline, final.vf_component_packing,
                        GENX(3DSTATE_VF_COMPONENT_PACKING), vfc) {
         vfc.VertexElementEnablesDW[0] = vs_prog_data->vf_component_packing[0];
         vfc.VertexElementEnablesDW[1] = vs_prog_data->vf_component_packing[1];
         vfc.VertexElementEnablesDW[2] = vs_prog_data->vf_component_packing[2];
         vfc.VertexElementEnablesDW[3] = vs_prog_data->vf_component_packing[3];
      }
   }
}

static bool
sbe_primitive_id_override(struct anv_graphics_pipeline *pipeline)
{
   const struct brw_wm_prog_data *wm_prog_data =
      get_pipeline_wm_prog_data(pipeline);
   if (!wm_prog_data)
      return false;

   if (anv_pipeline_is_mesh(pipeline)) {
      const struct brw_mesh_prog_data *mesh_prog_data =
         get_pipeline_mesh_prog_data(pipeline);
      const struct brw_mue_map *mue = &mesh_prog_data->map;
      return (wm_prog_data->inputs & VARYING_BIT_PRIMITIVE_ID) &&
              mue->per_primitive_offsets[VARYING_SLOT_PRIMITIVE_ID] == -1;
   }

   const struct intel_vue_map *fs_input_map =
      &anv_pipeline_get_last_vue_prog_data(pipeline)->vue_map;

   return (wm_prog_data->inputs & VARYING_BIT_PRIMITIVE_ID) &&
          (fs_input_map->slots_valid & VARYING_BIT_PRIMITIVE_ID) == 0;
}

static void
emit_3dstate_sbe(struct anv_graphics_pipeline *pipeline)
{
   const struct brw_wm_prog_data *wm_prog_data = get_pipeline_wm_prog_data(pipeline);
   const struct brw_mesh_prog_data *mesh_prog_data =
      get_pipeline_mesh_prog_data(pipeline);
   UNUSED const struct anv_device *device = pipeline->base.base.device;

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      anv_pipeline_emit(pipeline, final.sbe, GENX(3DSTATE_SBE), sbe);
      anv_pipeline_emit(pipeline, final.sbe_swiz, GENX(3DSTATE_SBE_SWIZ), sbe);
#if GFX_VERx10 >= 125
      if (device->vk.enabled_extensions.EXT_mesh_shader)
         anv_pipeline_emit(pipeline, final.sbe_mesh, GENX(3DSTATE_SBE_MESH), sbe);
#endif
      return;
   }

   const struct intel_vue_map *vue_map =
      anv_pipeline_is_mesh(pipeline) ?
      &get_pipeline_mesh_prog_data(pipeline)->map.vue_map :
      &anv_pipeline_get_last_vue_prog_data(pipeline)->vue_map;

   anv_pipeline_emit(pipeline, final.sbe, GENX(3DSTATE_SBE), sbe) {
   anv_pipeline_emit(pipeline, final.sbe_swiz, GENX(3DSTATE_SBE_SWIZ), swiz) {
      int max_source_attr = 0;
      uint32_t vertex_read_offset, vertex_read_length, vertex_varyings, flat_inputs;
      brw_compute_sbe_per_vertex_urb_read(
         vue_map, mesh_prog_data != NULL,
         mesh_prog_data ? mesh_prog_data->map.wa_18019110168_active : false,
         wm_prog_data,
         &vertex_read_offset, &vertex_read_length, &vertex_varyings,
         &pipeline->primitive_id_index,
         &flat_inputs);

      pipeline->first_vue_slot = vertex_read_offset * 2;

      sbe.AttributeSwizzleEnable = anv_pipeline_is_primitive(pipeline);
      sbe.PointSpriteTextureCoordinateOrigin = UPPERLEFT;
      sbe.ConstantInterpolationEnable = flat_inputs;
      sbe.NumberofSFOutputAttributes = vertex_varyings;
#if GFX_VERx10 >= 200
      sbe.VertexAttributesBypass = wm_prog_data->vertex_attributes_bypass;
#endif

      for (unsigned i = 0; i < 32; i++)
         sbe.AttributeActiveComponentFormat[i] = ACF_XYZW;

      /* As far as we can test, some of the fields in 3DSTATE_SBE & all of
       * 3DSTATE_SBE_SWIZ has no effect when the pipeline is using Mesh so
       * don't bother filling those fields.
       */
      if (anv_pipeline_is_primitive(pipeline)) {
         for (uint8_t idx = 0; idx < wm_prog_data->urb_setup_attribs_count; idx++) {
            uint8_t attr = wm_prog_data->urb_setup_attribs[idx];
            int input_index = wm_prog_data->urb_setup[attr];

            assert(0 <= input_index);

            if (attr == VARYING_SLOT_PNTC) {
               sbe.PointSpriteTextureCoordinateEnable = 1 << input_index;
               continue;
            }

            const int slot = vue_map->varying_to_slot[attr];

            if (slot == -1) {
               /* This attribute does not exist in the VUE--that means that
                * the vertex shader did not write to it. It could be that it's
                * a regular varying read by the fragment shader but not
                * written by the vertex shader or it's gl_PrimitiveID. In the
                * first case the value is undefined, in the second it needs to
                * be gl_PrimitiveID.
                */
               swiz.Attribute[input_index].ConstantSource = PRIM_ID;
               swiz.Attribute[input_index].ComponentOverrideX = true;
               swiz.Attribute[input_index].ComponentOverrideY = true;
               swiz.Attribute[input_index].ComponentOverrideZ = true;
               swiz.Attribute[input_index].ComponentOverrideW = true;
               continue;
            }

            /* We have to subtract two slots to account for the URB entry
             * output read offset in the VS and GS stages.
             */
            const int source_attr = slot - 2 * vertex_read_offset;
            assert(source_attr >= 0 && source_attr < 32);
            max_source_attr = MAX2(max_source_attr, source_attr);
            /* The hardware can only do overrides on 16 overrides at a time,
             * and the other up to 16 have to be lined up so that the input
             * index = the output index. We'll need to do some tweaking to
             * make sure that's the case.
             */
            if (input_index < 16)
               swiz.Attribute[input_index].SourceAttribute = source_attr;
            else
               assert(source_attr == input_index);
         }

         sbe.VertexURBEntryReadOffset = vertex_read_offset;
         sbe.VertexURBEntryReadLength = vertex_read_length;
         sbe.ForceVertexURBEntryReadOffset = true;
         sbe.ForceVertexURBEntryReadLength = true;
      }

      /* Ask the hardware to supply PrimitiveID if the fragment shader reads
       * it but a previous stage didn't write one.
       */
      if (sbe_primitive_id_override(pipeline)) {
         sbe.PrimitiveIDOverrideAttributeSelect =
            wm_prog_data->urb_setup[VARYING_SLOT_PRIMITIVE_ID];
         sbe.PrimitiveIDOverrideComponentX = true;
         sbe.PrimitiveIDOverrideComponentY = true;
         sbe.PrimitiveIDOverrideComponentZ = true;
         sbe.PrimitiveIDOverrideComponentW = true;
      }

#if GFX_VERx10 >= 125
      if (device->vk.enabled_extensions.EXT_mesh_shader) {
         anv_pipeline_emit(pipeline, final.sbe_mesh,
                           GENX(3DSTATE_SBE_MESH), sbe_mesh) {
            if (mesh_prog_data == NULL)
               continue;

            sbe_mesh.PerVertexURBEntryOutputReadOffset = vertex_read_offset;
            sbe_mesh.PerVertexURBEntryOutputReadLength = vertex_read_length;

            uint32_t prim_read_offset, prim_read_length;
            brw_compute_sbe_per_primitive_urb_read(wm_prog_data->per_primitive_inputs,
                                                   wm_prog_data->num_per_primitive_inputs,
                                                   &mesh_prog_data->map,
                                                   &prim_read_offset,
                                                   &prim_read_length);

            sbe_mesh.PerPrimitiveURBEntryOutputReadOffset = prim_read_offset;
            sbe_mesh.PerPrimitiveURBEntryOutputReadLength = prim_read_length;
         }
      }
#endif
   }
   }
}

static void
emit_rs_state(struct anv_graphics_pipeline *pipeline)
{
   anv_pipeline_emit(pipeline, partial.sf, GENX(3DSTATE_SF), sf) {
      sf.ViewportTransformEnable = true;
      sf.StatisticsEnable = true;
      sf.VertexSubPixelPrecisionSelect = _8Bit;
      sf.AALineDistanceMode = true;

      const struct intel_vue_map *vue_map =
         anv_pipeline_is_primitive(pipeline) ?
         &anv_pipeline_get_last_vue_prog_data(pipeline)->vue_map :
         &get_pipeline_mesh_prog_data(pipeline)->map.vue_map;
      if (vue_map->slots_valid & VARYING_BIT_PSIZ) {
         sf.PointWidthSource = Vertex;
      } else {
         sf.PointWidthSource = State;
         sf.PointWidth = 1.0;
      }
   }
}

static void
emit_3dstate_clip(struct anv_graphics_pipeline *pipeline,
                  const struct vk_input_assembly_state *ia,
                  const struct vk_viewport_state *vp,
                  const struct vk_rasterization_state *rs)
{
   const struct brw_wm_prog_data *wm_prog_data =
      get_pipeline_wm_prog_data(pipeline);
   (void) wm_prog_data;

   anv_pipeline_emit(pipeline, partial.clip, GENX(3DSTATE_CLIP), clip) {
      clip.ClipEnable               = true;
      clip.StatisticsEnable         = true;
      clip.EarlyCullEnable          = true;
      clip.GuardbandClipTestEnable  = true;

      clip.VertexSubPixelPrecisionSelect = _8Bit;
      clip.ClipMode = CLIPMODE_NORMAL;

      clip.MinimumPointWidth = 0.125;
      clip.MaximumPointWidth = 255.875;

      /* TODO(mesh): Multiview. */
      if (anv_pipeline_is_primitive(pipeline)) {
         const struct brw_vue_prog_data *last =
            anv_pipeline_get_last_vue_prog_data(pipeline);

         /* From the Vulkan 1.0.45 spec:
          *
          *    "If the last active vertex processing stage shader entry point's
          *    interface does not include a variable decorated with Layer, then
          *    the first layer is used."
          */
         clip.ForceZeroRTAIndexEnable =
            !(last->vue_map.slots_valid & VARYING_BIT_LAYER);

      } else if (anv_pipeline_is_mesh(pipeline)) {
         const struct brw_mesh_prog_data *mesh_prog_data =
            get_pipeline_mesh_prog_data(pipeline);

         clip.ForceZeroRTAIndexEnable =
            mesh_prog_data->map.per_primitive_offsets[VARYING_SLOT_LAYER] < 0;
      }

      clip.NonPerspectiveBarycentricEnable = wm_prog_data ?
         wm_prog_data->uses_nonperspective_interp_modes : 0;
   }

#if GFX_VERx10 >= 125
   const struct anv_device *device = pipeline->base.base.device;
   if (device->vk.enabled_extensions.EXT_mesh_shader) {
      anv_pipeline_emit(pipeline, final.clip_mesh,
                        GENX(3DSTATE_CLIP_MESH), clip_mesh) {
         if (!anv_pipeline_is_mesh(pipeline))
            continue;

         const struct brw_mesh_prog_data *mesh_prog_data =
            get_pipeline_mesh_prog_data(pipeline);
         clip_mesh.PrimitiveHeaderEnable = mesh_prog_data->map.has_per_primitive_header;
         clip_mesh.UserClipDistanceClipTestEnableBitmask = mesh_prog_data->clip_distance_mask;
         clip_mesh.UserClipDistanceCullTestEnableBitmask = mesh_prog_data->cull_distance_mask;
      }
   }
#endif
}

static void
emit_3dstate_streamout(struct anv_graphics_pipeline *pipeline,
                       const struct vk_rasterization_state *rs)
{
   const struct brw_vue_prog_data *prog_data =
      anv_pipeline_get_last_vue_prog_data(pipeline);
   const struct intel_vue_map *vue_map = &prog_data->vue_map;

   nir_xfb_info *xfb_info;
   if (anv_pipeline_has_stage(pipeline, MESA_SHADER_GEOMETRY))
      xfb_info = pipeline->base.shaders[MESA_SHADER_GEOMETRY]->xfb_info;
   else if (anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL))
      xfb_info = pipeline->base.shaders[MESA_SHADER_TESS_EVAL]->xfb_info;
   else
      xfb_info = pipeline->base.shaders[MESA_SHADER_VERTEX]->xfb_info;

   if (xfb_info) {
      struct GENX(SO_DECL) so_decl[MAX_XFB_STREAMS][128];
      int next_offset[MAX_XFB_BUFFERS] = {0, 0, 0, 0};
      int decls[MAX_XFB_STREAMS] = {0, 0, 0, 0};

      memset(so_decl, 0, sizeof(so_decl));

      for (unsigned i = 0; i < xfb_info->output_count; i++) {
         const nir_xfb_output_info *output = &xfb_info->outputs[i];
         unsigned buffer = output->buffer;
         unsigned stream = xfb_info->buffer_to_stream[buffer];

         /* Our hardware is unusual in that it requires us to program SO_DECLs
          * for fake "hole" components, rather than simply taking the offset
          * for each real varying.  Each hole can have size 1, 2, 3, or 4; we
          * program as many size = 4 holes as we can, then a final hole to
          * accommodate the final 1, 2, or 3 remaining.
          */
         int hole_dwords = (output->offset - next_offset[buffer]) / 4;
         while (hole_dwords > 0) {
            so_decl[stream][decls[stream]++] = (struct GENX(SO_DECL)) {
               .HoleFlag = 1,
               .OutputBufferSlot = buffer,
               .ComponentMask = (1 << MIN2(hole_dwords, 4)) - 1,
            };
            hole_dwords -= 4;
         }

         int varying = output->location;
         uint8_t component_mask = output->component_mask;
         /* VARYING_SLOT_PSIZ contains four scalar fields packed together:
          * - VARYING_SLOT_PRIMITIVE_SHADING_RATE in VARYING_SLOT_PSIZ.x
          * - VARYING_SLOT_LAYER                  in VARYING_SLOT_PSIZ.y
          * - VARYING_SLOT_VIEWPORT               in VARYING_SLOT_PSIZ.z
          * - VARYING_SLOT_PSIZ                   in VARYING_SLOT_PSIZ.w
          */
         if (varying == VARYING_SLOT_PRIMITIVE_SHADING_RATE) {
            varying = VARYING_SLOT_PSIZ;
            component_mask = 1 << 0; // SO_DECL_COMPMASK_X
         } else if (varying == VARYING_SLOT_LAYER) {
            varying = VARYING_SLOT_PSIZ;
            component_mask = 1 << 1; // SO_DECL_COMPMASK_Y
         } else if (varying == VARYING_SLOT_VIEWPORT) {
            varying = VARYING_SLOT_PSIZ;
            component_mask = 1 << 2; // SO_DECL_COMPMASK_Z
         } else if (varying == VARYING_SLOT_PSIZ) {
            component_mask = 1 << 3; // SO_DECL_COMPMASK_W
         }

         next_offset[buffer] = output->offset +
                               __builtin_popcount(component_mask) * 4;

         const int slot = vue_map->varying_to_slot[varying];
         if (slot < 0) {
            /* This can happen if the shader never writes to the varying.
             * Insert a hole instead of actual varying data.
             */
            so_decl[stream][decls[stream]++] = (struct GENX(SO_DECL)) {
               .HoleFlag = true,
               .OutputBufferSlot = buffer,
               .ComponentMask = component_mask,
            };
         } else {
            so_decl[stream][decls[stream]++] = (struct GENX(SO_DECL)) {
               .OutputBufferSlot = buffer,
               .RegisterIndex = slot,
               .ComponentMask = component_mask,
            };
         }
      }

      int max_decls = 0;
      for (unsigned s = 0; s < MAX_XFB_STREAMS; s++)
         max_decls = MAX2(max_decls, decls[s]);

      uint8_t sbs[MAX_XFB_STREAMS] = { };
      for (unsigned b = 0; b < MAX_XFB_BUFFERS; b++) {
         if (xfb_info->buffers_written & (1 << b))
            sbs[xfb_info->buffer_to_stream[b]] |= 1 << b;
      }

      uint32_t *dw = anv_pipeline_emitn(pipeline, final.so_decl_list,
                                        3 + 2 * max_decls,
                                        GENX(3DSTATE_SO_DECL_LIST),
                                        .StreamtoBufferSelects0 = sbs[0],
                                        .StreamtoBufferSelects1 = sbs[1],
                                        .StreamtoBufferSelects2 = sbs[2],
                                        .StreamtoBufferSelects3 = sbs[3],
                                        .NumEntries0 = decls[0],
                                        .NumEntries1 = decls[1],
                                        .NumEntries2 = decls[2],
                                        .NumEntries3 = decls[3]);

      for (int i = 0; i < max_decls; i++) {
         GENX(SO_DECL_ENTRY_pack)(NULL, dw + 3 + i * 2,
            &(struct GENX(SO_DECL_ENTRY)) {
               .Stream0Decl = so_decl[0][i],
               .Stream1Decl = so_decl[1][i],
               .Stream2Decl = so_decl[2][i],
               .Stream3Decl = so_decl[3][i],
            });
      }
   }

   anv_pipeline_emit(pipeline, partial.so, GENX(3DSTATE_STREAMOUT), so) {
      if (xfb_info) {
         pipeline->uses_xfb = true;

         so.SOFunctionEnable = true;
         so.SOStatisticsEnable = true;

         so.Buffer0SurfacePitch = xfb_info->buffers[0].stride;
         so.Buffer1SurfacePitch = xfb_info->buffers[1].stride;
         so.Buffer2SurfacePitch = xfb_info->buffers[2].stride;
         so.Buffer3SurfacePitch = xfb_info->buffers[3].stride;

         int urb_entry_read_offset = 0;
         int urb_entry_read_length =
            (prog_data->vue_map.num_slots + 1) / 2 - urb_entry_read_offset;

         /* We always read the whole vertex. This could be reduced at some
          * point by reading less and offsetting the register index in the
          * SO_DECLs.
          */
         so.Stream0VertexReadOffset = urb_entry_read_offset;
         so.Stream0VertexReadLength = urb_entry_read_length - 1;
         so.Stream1VertexReadOffset = urb_entry_read_offset;
         so.Stream1VertexReadLength = urb_entry_read_length - 1;
         so.Stream2VertexReadOffset = urb_entry_read_offset;
         so.Stream2VertexReadLength = urb_entry_read_length - 1;
         so.Stream3VertexReadOffset = urb_entry_read_offset;
         so.Stream3VertexReadLength = urb_entry_read_length - 1;
      }
   }
}

static inline uint32_t
get_sampler_count(const struct anv_shader_bin *bin)
{
   /* We can potentially have way more than 32 samplers and that's ok.
    * However, the 3DSTATE_XS packets only have 3 bits to specify how
    * many to pre-fetch and all values above 4 are marked reserved.
    */
   return DIV_ROUND_UP(CLAMP(bin->bind_map.sampler_count, 0, 16), 4);
}

static UNUSED struct anv_address
get_scratch_address(struct anv_pipeline *pipeline,
                    gl_shader_stage stage,
                    const struct anv_shader_bin *bin)
{
   return (struct anv_address) {
      .bo = anv_scratch_pool_alloc(pipeline->device,
                                   &pipeline->device->scratch_pool,
                                   stage, bin->prog_data->total_scratch),
      .offset = 0,
   };
}

static UNUSED uint32_t
get_scratch_space(const struct anv_shader_bin *bin)
{
   return ffs(bin->prog_data->total_scratch / 2048);
}

static UNUSED uint32_t
get_scratch_surf(struct anv_pipeline *pipeline,
                 gl_shader_stage stage,
                 const struct anv_shader_bin *bin,
                 bool protected)
{
   if (bin->prog_data->total_scratch == 0)
      return 0;

   struct anv_scratch_pool *pool = protected ?
      &pipeline->device->protected_scratch_pool :
      &pipeline->device->scratch_pool;
   struct anv_bo *bo =
      anv_scratch_pool_alloc(pipeline->device, pool,
                             stage, bin->prog_data->total_scratch);
   anv_reloc_list_add_bo(pipeline->batch.relocs, bo);
   return anv_scratch_pool_get_surf(pipeline->device, pool,
                                    bin->prog_data->total_scratch) >> ANV_SCRATCH_SPACE_SHIFT(GFX_VER);
}

static void
emit_3dstate_vs(struct anv_graphics_pipeline *pipeline)
{
   const struct intel_device_info *devinfo = pipeline->base.base.device->info;
   const struct brw_vs_prog_data *vs_prog_data =
      get_pipeline_vs_prog_data(pipeline);
   const struct anv_shader_bin *vs_bin =
      pipeline->base.shaders[MESA_SHADER_VERTEX];

   assert(anv_pipeline_has_stage(pipeline, MESA_SHADER_VERTEX));

   uint32_t vs_dwords[GENX(3DSTATE_VS_length)];
   anv_pipeline_emit_tmp(pipeline, vs_dwords, GENX(3DSTATE_VS), vs) {
      vs.Enable               = true;
      vs.StatisticsEnable     = true;
      vs.KernelStartPointer   = vs_bin->kernel.offset;
#if GFX_VER < 20
      vs.SIMD8DispatchEnable  =
         vs_prog_data->base.dispatch_mode == DISPATCH_MODE_SIMD8;
#endif

      assert(!vs_prog_data->base.base.use_alt_mode);
#if GFX_VER < 11
      vs.SingleVertexDispatch       = false;
#endif
      vs.VectorMaskEnable           = false;
      /* Wa_1606682166:
       * Incorrect TDL's SSP address shift in SARB for 16:6 & 18:8 modes.
       * Disable the Sampler state prefetch functionality in the SARB by
       * programming 0xB000[30] to '1'.
       */
      vs.SamplerCount               = GFX_VER == 11 ? 0 : get_sampler_count(vs_bin);
      vs.BindingTableEntryCount     = vs_bin->bind_map.surface_count;
      vs.FloatingPointMode          = IEEE754;
      vs.IllegalOpcodeExceptionEnable = false;
      vs.SoftwareExceptionEnable    = false;
      vs.MaximumNumberofThreads     = devinfo->max_vs_threads - 1;

      if (GFX_VER == 9 && devinfo->gt == 4 &&
          anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL)) {
         /* On Sky Lake GT4, we have experienced some hangs related to the VS
          * cache and tessellation.  It is unknown exactly what is happening
          * but the Haswell docs for the "VS Reference Count Full Force Miss
          * Enable" field of the "Thread Mode" register refer to a HSW bug in
          * which the VUE handle reference count would overflow resulting in
          * internal reference counting bugs.  My (Faith's) best guess is that
          * this bug cropped back up on SKL GT4 when we suddenly had more
          * threads in play than any previous gfx9 hardware.
          *
          * What we do know for sure is that setting this bit when
          * tessellation shaders are in use fixes a GPU hang in Batman: Arkham
          * City when playing with DXVK (https://bugs.freedesktop.org/107280).
          * Disabling the vertex cache with tessellation shaders should only
          * have a minor performance impact as the tessellation shaders are
          * likely generating and processing far more geometry than the vertex
          * stage.
          */
         vs.VertexCacheDisable = true;
      }

      vs.VertexURBEntryReadLength      = vs_prog_data->base.urb_read_length;
      vs.VertexURBEntryReadOffset      = 0;
      vs.DispatchGRFStartRegisterForURBData =
         vs_prog_data->base.base.dispatch_grf_start_reg;

      vs.UserClipDistanceClipTestEnableBitmask =
         vs_prog_data->base.clip_distance_mask;
      vs.UserClipDistanceCullTestEnableBitmask =
         vs_prog_data->base.cull_distance_mask;

#if GFX_VERx10 < 125
      vs.PerThreadScratchSpace   = get_scratch_space(vs_bin);
      vs.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base.base, MESA_SHADER_VERTEX, vs_bin);
#endif

#if GFX_VER >= 30
      vs.RegistersPerThread = ptl_register_blocks(vs_prog_data->base.base.grf_used);
#endif
   }

   anv_pipeline_emit_merge(pipeline, final.vs, vs_dwords, GENX(3DSTATE_VS), vs) {
#if GFX_VERx10 >= 125
      vs.ScratchSpaceBuffer = get_scratch_surf(&pipeline->base.base,
                                               MESA_SHADER_VERTEX,
                                               vs_bin, false);
#endif
   }
   if (pipeline_needs_protected(&pipeline->base.base)) {
      anv_pipeline_emit_merge(pipeline, final.vs_protected,
                              vs_dwords, GENX(3DSTATE_VS), vs) {
#if GFX_VERx10 >= 125
         vs.ScratchSpaceBuffer = get_scratch_surf(&pipeline->base.base,
                                                  MESA_SHADER_VERTEX,
                                                  vs_bin, true);
#endif
      }
   }
}

static void
emit_3dstate_hs_ds(struct anv_graphics_pipeline *pipeline,
                   const struct vk_tessellation_state *ts)
{
   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL)) {
      anv_pipeline_emit(pipeline, final.hs, GENX(3DSTATE_HS), hs);
      anv_pipeline_emit(pipeline, final.hs_protected, GENX(3DSTATE_HS), hs);
      anv_pipeline_emit(pipeline, final.ds, GENX(3DSTATE_DS), ds);
      anv_pipeline_emit(pipeline, final.ds_protected, GENX(3DSTATE_DS), ds);
      return;
   }

   const struct intel_device_info *devinfo = pipeline->base.base.device->info;
   const struct anv_shader_bin *tcs_bin =
      pipeline->base.shaders[MESA_SHADER_TESS_CTRL];
   const struct anv_shader_bin *tes_bin =
      pipeline->base.shaders[MESA_SHADER_TESS_EVAL];

   const struct brw_tcs_prog_data *tcs_prog_data =
      get_pipeline_tcs_prog_data(pipeline);
   const struct brw_tes_prog_data *tes_prog_data =
      get_pipeline_tes_prog_data(pipeline);

   uint32_t hs_dwords[GENX(3DSTATE_HS_length)];
   anv_pipeline_emit_tmp(pipeline, hs_dwords, GENX(3DSTATE_HS), hs) {
      hs.Enable = true;
      hs.StatisticsEnable = true;
      hs.KernelStartPointer = tcs_bin->kernel.offset;
      /* Wa_1606682166 */
      hs.SamplerCount = GFX_VER == 11 ? 0 : get_sampler_count(tcs_bin);
      hs.BindingTableEntryCount = tcs_bin->bind_map.surface_count;

#if GFX_VER >= 12
      /* Wa_1604578095:
       *
       *    Hang occurs when the number of max threads is less than 2 times
       *    the number of instance count. The number of max threads must be
       *    more than 2 times the number of instance count.
       */
      assert((devinfo->max_tcs_threads / 2) > tcs_prog_data->instances);
#endif

      hs.MaximumNumberofThreads = devinfo->max_tcs_threads - 1;
      hs.IncludeVertexHandles = true;
      hs.InstanceCount = tcs_prog_data->instances - 1;

      hs.VertexURBEntryReadLength = 0;
      hs.VertexURBEntryReadOffset = 0;
      hs.DispatchGRFStartRegisterForURBData =
         tcs_prog_data->base.base.dispatch_grf_start_reg & 0x1f;
#if GFX_VER >= 12
      hs.DispatchGRFStartRegisterForURBData5 =
         tcs_prog_data->base.base.dispatch_grf_start_reg >> 5;
#endif

#if GFX_VERx10 < 125
      hs.PerThreadScratchSpace = get_scratch_space(tcs_bin);
      hs.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base.base, MESA_SHADER_TESS_CTRL, tcs_bin);
#endif

#if GFX_VER == 12
      /*  Patch Count threshold specifies the maximum number of patches that
       *  will be accumulated before a thread dispatch is forced.
       */
      hs.PatchCountThreshold = tcs_prog_data->patch_count_threshold;
#endif

#if GFX_VER < 20
      hs.DispatchMode = tcs_prog_data->base.dispatch_mode;
#endif
      hs.IncludePrimitiveID = tcs_prog_data->include_primitive_id;

#if GFX_VER >= 30
      hs.RegistersPerThread = ptl_register_blocks(tcs_prog_data->base.base.grf_used);
#endif
   };

   uint32_t ds_dwords[GENX(3DSTATE_DS_length)];
   anv_pipeline_emit_tmp(pipeline, ds_dwords, GENX(3DSTATE_DS), ds) {
      ds.Enable = true;
      ds.StatisticsEnable = true;
      ds.KernelStartPointer = tes_bin->kernel.offset;
      /* Wa_1606682166 */
      ds.SamplerCount = GFX_VER == 11 ? 0 : get_sampler_count(tes_bin);
      ds.BindingTableEntryCount = tes_bin->bind_map.surface_count;
      ds.MaximumNumberofThreads = devinfo->max_tes_threads - 1;

      ds.ComputeWCoordinateEnable =
         tes_prog_data->domain == INTEL_TESS_DOMAIN_TRI;

      ds.PatchURBEntryReadLength = tes_prog_data->base.urb_read_length;
      ds.PatchURBEntryReadOffset = 0;
      ds.DispatchGRFStartRegisterForURBData =
         tes_prog_data->base.base.dispatch_grf_start_reg;

#if GFX_VER < 11
      ds.DispatchMode =
         tes_prog_data->base.dispatch_mode == DISPATCH_MODE_SIMD8 ?
         DISPATCH_MODE_SIMD8_SINGLE_PATCH :
         DISPATCH_MODE_SIMD4X2;
#else
      assert(tes_prog_data->base.dispatch_mode == INTEL_DISPATCH_MODE_SIMD8);
      ds.DispatchMode = DISPATCH_MODE_SIMD8_SINGLE_PATCH;
#endif

      ds.UserClipDistanceClipTestEnableBitmask =
         tes_prog_data->base.clip_distance_mask;
      ds.UserClipDistanceCullTestEnableBitmask =
         tes_prog_data->base.cull_distance_mask;

#if GFX_VER >= 12
      ds.PrimitiveIDNotRequired = !tes_prog_data->include_primitive_id;
#endif
#if GFX_VERx10 < 125
      ds.PerThreadScratchSpace = get_scratch_space(tes_bin);
      ds.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base.base, MESA_SHADER_TESS_EVAL, tes_bin);
#endif

#if GFX_VER >= 30
      ds.RegistersPerThread = ptl_register_blocks(tes_prog_data->base.base.grf_used);
#endif
   }

   anv_pipeline_emit_merge(pipeline, final.hs, hs_dwords, GENX(3DSTATE_HS), hs) {
#if GFX_VERx10 >= 125
      hs.ScratchSpaceBuffer = get_scratch_surf(&pipeline->base.base,
                                               MESA_SHADER_TESS_CTRL,
                                               tcs_bin, false);
#endif
   }
   anv_pipeline_emit_merge(pipeline, final.ds, ds_dwords, GENX(3DSTATE_DS), ds) {
#if GFX_VERx10 >= 125
      ds.ScratchSpaceBuffer = get_scratch_surf(&pipeline->base.base,
                                               MESA_SHADER_TESS_EVAL,
                                               tes_bin, false);
#endif
   }
   if (pipeline_needs_protected(&pipeline->base.base)) {
      anv_pipeline_emit_merge(pipeline, final.hs_protected,
                              hs_dwords, GENX(3DSTATE_HS), hs) {
#if GFX_VERx10 >= 125
         hs.ScratchSpaceBuffer = get_scratch_surf(&pipeline->base.base,
                                                  MESA_SHADER_TESS_CTRL,
                                                  tcs_bin, true);
#endif
      }
      anv_pipeline_emit_merge(pipeline, final.ds_protected,
                              ds_dwords, GENX(3DSTATE_DS), ds) {
#if GFX_VERx10 >= 125
         ds.ScratchSpaceBuffer = get_scratch_surf(&pipeline->base.base,
                                                  MESA_SHADER_TESS_EVAL,
                                                  tes_bin, true);
#endif
      }
   }
}

static UNUSED bool
geom_or_tess_prim_id_used(struct anv_graphics_pipeline *pipeline)
{
   const struct brw_tcs_prog_data *tcs_prog_data =
      anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_CTRL) ?
      get_pipeline_tcs_prog_data(pipeline) : NULL;
   const struct brw_tes_prog_data *tes_prog_data =
      anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL) ?
      get_pipeline_tes_prog_data(pipeline) : NULL;
   const struct brw_gs_prog_data *gs_prog_data =
      anv_pipeline_has_stage(pipeline, MESA_SHADER_GEOMETRY) ?
      get_pipeline_gs_prog_data(pipeline) : NULL;

   return (tcs_prog_data && tcs_prog_data->include_primitive_id) ||
          (tes_prog_data && tes_prog_data->include_primitive_id) ||
          (gs_prog_data && gs_prog_data->include_primitive_id);
}

static void
emit_3dstate_te(struct anv_graphics_pipeline *pipeline)
{
   anv_pipeline_emit(pipeline, partial.te, GENX(3DSTATE_TE), te) {
      if (anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL)) {
         const struct brw_tes_prog_data *tes_prog_data =
            get_pipeline_tes_prog_data(pipeline);

         te.Partitioning = tes_prog_data->partitioning;
         te.TEDomain = tes_prog_data->domain;
         te.TEEnable = true;
         te.MaximumTessellationFactorOdd = 63.0;
         te.MaximumTessellationFactorNotOdd = 64.0;
#if GFX_VERx10 >= 125
         const struct anv_device *device = pipeline->base.base.device;
         if (intel_needs_workaround(device->info, 22012699309))
            te.TessellationDistributionMode = TEDMODE_RR_STRICT;
         else
            te.TessellationDistributionMode = TEDMODE_RR_FREE;

         if (intel_needs_workaround(device->info, 14015055625)) {
            /* Wa_14015055625:
             *
             * Disable Tessellation Distribution when primitive Id is enabled.
             */
            if (sbe_primitive_id_override(pipeline) ||
                geom_or_tess_prim_id_used(pipeline))
               te.TessellationDistributionMode = TEDMODE_OFF;
         }

         if (!device->physical->instance->enable_te_distribution)
            te.TessellationDistributionMode = TEDMODE_OFF;

#if GFX_VER >= 20
         if (intel_needs_workaround(device->info, 16025857284))
            te.TessellationDistributionLevel = TEDLEVEL_PATCH;
         else
            te.TessellationDistributionLevel = TEDLEVEL_REGION;
#else
         te.TessellationDistributionLevel = TEDLEVEL_PATCH;
#endif
         /* 64_TRIANGLES */
         te.SmallPatchThreshold = 3;
         /* 1K_TRIANGLES */
         te.TargetBlockSize = 8;
         /* 1K_TRIANGLES */
         te.LocalBOPAccumulatorThreshold = 1;
#endif

#if GFX_VER >= 20
         te.NumberOfRegionsPerPatch = 2;
#endif
      }
   }
}

static void
emit_3dstate_gs(struct anv_graphics_pipeline *pipeline)
{
   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_GEOMETRY)) {
      anv_pipeline_emit(pipeline, partial.gs, GENX(3DSTATE_GS), gs);
      anv_pipeline_emit(pipeline, partial.gs_protected, GENX(3DSTATE_GS), gs);
      return;
   }

   const struct intel_device_info *devinfo = pipeline->base.base.device->info;
   const struct anv_shader_bin *gs_bin =
      pipeline->base.shaders[MESA_SHADER_GEOMETRY];
   const struct brw_gs_prog_data *gs_prog_data =
      get_pipeline_gs_prog_data(pipeline);

   uint32_t gs_dwords[GENX(3DSTATE_GS_length)];
   anv_pipeline_emit_tmp(pipeline, gs_dwords, GENX(3DSTATE_GS), gs) {
      gs.Enable                  = true;
      gs.StatisticsEnable        = true;
      gs.KernelStartPointer      = gs_bin->kernel.offset;
#if GFX_VER < 20
      gs.DispatchMode            = gs_prog_data->base.dispatch_mode;
#endif

      gs.SingleProgramFlow       = false;
      gs.VectorMaskEnable        = false;
      /* Wa_1606682166 */
      gs.SamplerCount            = GFX_VER == 11 ? 0 : get_sampler_count(gs_bin);
      gs.BindingTableEntryCount  = gs_bin->bind_map.surface_count;
      gs.IncludeVertexHandles    = gs_prog_data->base.include_vue_handles;
      gs.IncludePrimitiveID      = gs_prog_data->include_primitive_id;

      gs.MaximumNumberofThreads = devinfo->max_gs_threads - 1;

      gs.OutputVertexSize        = gs_prog_data->output_vertex_size_hwords * 2 - 1;
      gs.OutputTopology          = gs_prog_data->output_topology;
      gs.ControlDataFormat       = gs_prog_data->control_data_format;
      gs.ControlDataHeaderSize   = gs_prog_data->control_data_header_size_hwords;
      gs.InstanceControl         = MAX2(gs_prog_data->invocations, 1) - 1;

      gs.ExpectedVertexCount     = gs_prog_data->vertices_in;
      gs.StaticOutput            = gs_prog_data->static_vertex_count >= 0;
      gs.StaticOutputVertexCount = gs_prog_data->static_vertex_count >= 0 ?
         gs_prog_data->static_vertex_count : 0;

      gs.VertexURBEntryReadOffset = 0;
      gs.VertexURBEntryReadLength = gs_prog_data->base.urb_read_length;
      gs.DispatchGRFStartRegisterForURBData =
         gs_prog_data->base.base.dispatch_grf_start_reg;

      gs.UserClipDistanceClipTestEnableBitmask =
         gs_prog_data->base.clip_distance_mask;
      gs.UserClipDistanceCullTestEnableBitmask =
         gs_prog_data->base.cull_distance_mask;

#if GFX_VERx10 < 125
      gs.PerThreadScratchSpace   = get_scratch_space(gs_bin);
      gs.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base.base, MESA_SHADER_GEOMETRY, gs_bin);
#endif

#if GFX_VER >= 30
      gs.RegistersPerThread = ptl_register_blocks(gs_prog_data->base.base.grf_used);
#endif
   }

   anv_pipeline_emit_merge(pipeline, partial.gs, gs_dwords, GENX(3DSTATE_GS), gs) {
#if GFX_VERx10 >= 125
      gs.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base.base, MESA_SHADER_GEOMETRY, gs_bin, false);
#endif
   }
   if (pipeline_needs_protected(&pipeline->base.base)) {
      anv_pipeline_emit_merge(pipeline, partial.gs_protected,
                              gs_dwords, GENX(3DSTATE_GS), gs) {
#if GFX_VERx10 >= 125
         gs.ScratchSpaceBuffer = get_scratch_surf(&pipeline->base.base,
                                                  MESA_SHADER_GEOMETRY,
                                                  gs_bin, true);
#endif
      }
   }
}

static void
emit_3dstate_wm(struct anv_graphics_pipeline *pipeline,
                const struct vk_input_assembly_state *ia,
                const struct vk_rasterization_state *rs,
                const struct vk_multisample_state *ms,
                const struct vk_color_blend_state *cb,
                const struct vk_render_pass_state *rp)
{
   const struct brw_wm_prog_data *wm_prog_data =
      get_pipeline_wm_prog_data(pipeline);

   anv_pipeline_emit(pipeline, partial.wm, GENX(3DSTATE_WM), wm) {
      wm.StatisticsEnable                    = true;
      wm.LineEndCapAntialiasingRegionWidth   = _05pixels;
      wm.LineAntialiasingRegionWidth         = _10pixels;
      wm.PointRasterizationRule              = RASTRULE_UPPER_LEFT;

      if (anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
         if (wm_prog_data->early_fragment_tests) {
            wm.EarlyDepthStencilControl         = EDSC_PREPS;
         } else if (wm_prog_data->has_side_effects) {
            wm.EarlyDepthStencilControl         = EDSC_PSEXEC;
         } else {
            wm.EarlyDepthStencilControl         = EDSC_NORMAL;
         }
      }
   }
}

static void
emit_3dstate_ps(struct anv_graphics_pipeline *pipeline,
                const struct vk_multisample_state *ms,
                const struct vk_color_blend_state *cb)
{
   UNUSED const struct intel_device_info *devinfo =
      pipeline->base.base.device->info;
   const struct anv_shader_bin *fs_bin =
      pipeline->base.shaders[MESA_SHADER_FRAGMENT];

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      anv_pipeline_emit(pipeline, partial.ps, GENX(3DSTATE_PS), ps);
      anv_pipeline_emit(pipeline, partial.ps_protected, GENX(3DSTATE_PS), ps);
      return;
   }

   const struct brw_wm_prog_data *wm_prog_data =
      get_pipeline_wm_prog_data(pipeline);

   uint32_t ps_dwords[GENX(3DSTATE_PS_length)];
   anv_pipeline_emit_tmp(pipeline, ps_dwords, GENX(3DSTATE_PS), ps) {
#if GFX_VER == 12
      assert(wm_prog_data->dispatch_multi == 0 ||
             (wm_prog_data->dispatch_multi == 16 && wm_prog_data->max_polygons == 2));
      ps.DualSIMD8DispatchEnable = wm_prog_data->dispatch_multi;
      /* XXX - No major improvement observed from enabling
       *       overlapping subspans, but it could be helpful
       *       in theory when the requirements listed on the
       *       BSpec page for 3DSTATE_PS_BODY are met.
       */
      ps.OverlappingSubspansEnable = false;
#endif

      ps.SingleProgramFlow          = false;
      ps.VectorMaskEnable           = wm_prog_data->uses_vmask;
      /* Wa_1606682166 */
      ps.SamplerCount               = GFX_VER == 11 ? 0 : get_sampler_count(fs_bin);
      ps.BindingTableEntryCount     = fs_bin->bind_map.surface_count;
#if GFX_VER < 20
      ps.PushConstantEnable         =
         wm_prog_data->base.nr_params > 0 ||
         wm_prog_data->base.ubo_ranges[0].length;
#endif

      ps.MaximumNumberofThreadsPerPSD = devinfo->max_threads_per_psd - 1;

#if GFX_VERx10 < 125
      ps.PerThreadScratchSpace   = get_scratch_space(fs_bin);
      ps.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base.base, MESA_SHADER_FRAGMENT, fs_bin);
#endif

#if GFX_VER >= 30
      ps.RegistersPerThread = ptl_register_blocks(wm_prog_data->base.grf_used);
#endif
   }
   anv_pipeline_emit_merge(pipeline, partial.ps, ps_dwords, GENX(3DSTATE_PS), ps) {
#if GFX_VERx10 >= 125
      ps.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base.base, MESA_SHADER_FRAGMENT, fs_bin, false);
#endif
   }
   if (pipeline_needs_protected(&pipeline->base.base)) {
      anv_pipeline_emit_merge(pipeline, partial.ps_protected,
                              ps_dwords, GENX(3DSTATE_PS), ps) {
#if GFX_VERx10 >= 125
         ps.ScratchSpaceBuffer =
            get_scratch_surf(&pipeline->base.base, MESA_SHADER_FRAGMENT, fs_bin, true);
#endif
      }
   }
}

static void
emit_3dstate_ps_extra(struct anv_graphics_pipeline *pipeline,
                      const struct vk_rasterization_state *rs,
                      const struct vk_graphics_pipeline_state *state)
{
   const struct brw_wm_prog_data *wm_prog_data =
      get_pipeline_wm_prog_data(pipeline);

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      anv_pipeline_emit(pipeline, partial.ps_extra, GENX(3DSTATE_PS_EXTRA), ps);
      return;
   }

   anv_pipeline_emit(pipeline, partial.ps_extra, GENX(3DSTATE_PS_EXTRA), ps) {
      ps.PixelShaderValid              = true;
#if GFX_VER < 20
      ps.AttributeEnable               = wm_prog_data->num_varying_inputs > 0;
#endif
      ps.oMaskPresenttoRenderTarget    = wm_prog_data->uses_omask;
      ps.PixelShaderComputedDepthMode  = wm_prog_data->computed_depth_mode;
      ps.PixelShaderUsesSourceDepth    = wm_prog_data->uses_src_depth;
      ps.PixelShaderUsesSourceW        = wm_prog_data->uses_src_w;

      ps.PixelShaderComputesStencil = wm_prog_data->computed_stencil;
#if GFX_VER >= 20
      assert(!wm_prog_data->pulls_bary);
#else
      ps.PixelShaderPullsBary    = wm_prog_data->pulls_bary;
#endif

      ps.InputCoverageMaskState = ICMS_NONE;
      assert(!wm_prog_data->inner_coverage); /* Not available in SPIR-V */
      if (!wm_prog_data->uses_sample_mask)
         ps.InputCoverageMaskState = ICMS_NONE;
      else if (brw_wm_prog_data_is_coarse(wm_prog_data, 0))
         ps.InputCoverageMaskState  = ICMS_NORMAL;
      else if (wm_prog_data->post_depth_coverage)
         ps.InputCoverageMaskState = ICMS_DEPTH_COVERAGE;
      else
         ps.InputCoverageMaskState = ICMS_NORMAL;

#if GFX_VER >= 11
      ps.PixelShaderRequiresSubpixelSampleOffsets =
         wm_prog_data->uses_sample_offsets;
      ps.PixelShaderRequiresNonPerspectiveBaryPlaneCoefficients =
         wm_prog_data->uses_npc_bary_coefficients;
      ps.PixelShaderRequiresPerspectiveBaryPlaneCoefficients =
         wm_prog_data->uses_pc_bary_coefficients;
      ps.PixelShaderRequiresSourceDepthandorWPlaneCoefficients =
         wm_prog_data->uses_depth_w_coefficients;
#endif
   }
}

static void
compute_kill_pixel(struct anv_graphics_pipeline *pipeline,
                   const struct vk_multisample_state *ms,
                   const struct vk_graphics_pipeline_state *state)
{
   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      pipeline->kill_pixel = false;
      return;
   }

   const struct brw_wm_prog_data *wm_prog_data =
      get_pipeline_wm_prog_data(pipeline);

   /* This computes the KillPixel portion of the computation for whether or
    * not we want to enable the PMA fix on gfx8 or gfx9.  It's given by this
    * chunk of the giant formula:
    *
    *    (3DSTATE_PS_EXTRA::PixelShaderKillsPixels ||
    *     3DSTATE_PS_EXTRA::oMask Present to RenderTarget ||
    *     3DSTATE_PS_BLEND::AlphaToCoverageEnable ||
    *     3DSTATE_PS_BLEND::AlphaTestEnable ||
    *     3DSTATE_WM_CHROMAKEY::ChromaKeyKillEnable)
    *
    * 3DSTATE_WM_CHROMAKEY::ChromaKeyKillEnable is always false and so is
    * 3DSTATE_PS_BLEND::AlphaTestEnable since Vulkan doesn't have a concept
    * of an alpha test.
    */
   pipeline->kill_pixel =
      wm_prog_data->uses_kill ||
      wm_prog_data->uses_omask ||
      (ms && ms->alpha_to_coverage_enable);
}

#if GFX_VER >= 12
static void
emit_3dstate_primitive_replication(struct anv_graphics_pipeline *pipeline,
                                   const struct vk_render_pass_state *rp)
{
   if (anv_pipeline_is_mesh(pipeline)) {
      anv_pipeline_emit(pipeline, final.primitive_replication,
                        GENX(3DSTATE_PRIMITIVE_REPLICATION), pr);
      return;
   }

   const int replication_count =
      anv_pipeline_get_last_vue_prog_data(pipeline)->vue_map.num_pos_slots;

   assert(replication_count >= 1);
   if (replication_count == 1) {
      anv_pipeline_emit(pipeline, final.primitive_replication,
                        GENX(3DSTATE_PRIMITIVE_REPLICATION), pr);
      return;
   }

   assert(replication_count == util_bitcount(rp->view_mask));
   assert(replication_count <= MAX_VIEWS_FOR_PRIMITIVE_REPLICATION);

   anv_pipeline_emit(pipeline, final.primitive_replication,
                     GENX(3DSTATE_PRIMITIVE_REPLICATION), pr) {
      pr.ReplicaMask = (1 << replication_count) - 1;
      pr.ReplicationCount = replication_count - 1;

      int i = 0;
      u_foreach_bit(view_index, rp->view_mask) {
         pr.RTAIOffset[i] = view_index;
         i++;
      }
   }
}
#endif

#if GFX_VERx10 >= 125
static void
emit_task_state(struct anv_graphics_pipeline *pipeline)
{
   assert(anv_pipeline_is_mesh(pipeline));

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_TASK)) {
      anv_pipeline_emit(pipeline, final.task_control,
                        GENX(3DSTATE_TASK_CONTROL), zero);
      anv_pipeline_emit(pipeline, final.task_control_protected,
                        GENX(3DSTATE_TASK_CONTROL), zero);
      anv_pipeline_emit(pipeline, final.task_shader,
                        GENX(3DSTATE_TASK_SHADER), zero);
      anv_pipeline_emit(pipeline, final.task_redistrib,
                        GENX(3DSTATE_TASK_REDISTRIB), zero);
      return;
   }

   const struct anv_shader_bin *task_bin =
      pipeline->base.shaders[MESA_SHADER_TASK];

   uint32_t task_control_dwords[GENX(3DSTATE_TASK_CONTROL_length)];
   anv_pipeline_emit_tmp(pipeline, task_control_dwords, GENX(3DSTATE_TASK_CONTROL), tc) {
      tc.TaskShaderEnable = true;
      tc.StatisticsEnable = true;
      tc.MaximumNumberofThreadGroups = 511;
   }

   anv_pipeline_emit_merge(pipeline, final.task_control,
                           task_control_dwords, GENX(3DSTATE_TASK_CONTROL), tc) {
      tc.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base.base, MESA_SHADER_TASK, task_bin, false);
   }
   if (pipeline_needs_protected(&pipeline->base.base)) {
      anv_pipeline_emit_merge(pipeline, final.task_control_protected,
                              task_control_dwords, GENX(3DSTATE_TASK_CONTROL), tc) {
         tc.ScratchSpaceBuffer =
            get_scratch_surf(&pipeline->base.base, MESA_SHADER_TASK, task_bin, true);
      }
   }

   const struct intel_device_info *devinfo = pipeline->base.base.device->info;
   const struct brw_task_prog_data *task_prog_data =
      get_pipeline_task_prog_data(pipeline);
   const struct intel_cs_dispatch_info task_dispatch =
      brw_cs_get_dispatch_info(devinfo, &task_prog_data->base, NULL);

   anv_pipeline_emit(pipeline, final.task_shader,
                     GENX(3DSTATE_TASK_SHADER), task) {
      task.KernelStartPointer                = task_bin->kernel.offset;
      task.SIMDSize                          = task_dispatch.simd_size / 16;
      task.MessageSIMD                       = task.SIMDSize;
      task.NumberofThreadsinGPGPUThreadGroup = task_dispatch.threads;
      task.ExecutionMask                     = task_dispatch.right_mask;
      task.LocalXMaximum                     = task_dispatch.group_size - 1;
      task.EmitLocalIDX                      = true;

      task.NumberofBarriers                  = task_prog_data->base.uses_barrier;
      task.SharedLocalMemorySize             =
         intel_compute_slm_encode_size(GFX_VER, task_prog_data->base.base.total_shared);
      task.PreferredSLMAllocationSize        =
         intel_compute_preferred_slm_calc_encode_size(devinfo,
                                                      task_prog_data->base.base.total_shared,
                                                      task_dispatch.group_size,
                                                      task_dispatch.simd_size);

      task.EmitInlineParameter = task_prog_data->base.uses_inline_data;
      task.IndirectDataLength = align(task_bin->bind_map.push_ranges[0].length * 32, 64);

      task.XP0Required = task_prog_data->uses_drawid;

#if GFX_VER >= 30
      task.RegistersPerThread = ptl_register_blocks(task_prog_data->base.base.grf_used);
#endif
   }

   /* Recommended values from "Task and Mesh Distribution Programming". */
   anv_pipeline_emit(pipeline, final.task_redistrib,
                     GENX(3DSTATE_TASK_REDISTRIB), redistrib) {
      redistrib.LocalBOTAccumulatorThreshold = MULTIPLIER_1;
      redistrib.SmallTaskThreshold = 1; /* 2^N */
      redistrib.TargetMeshBatchSize = devinfo->num_slices > 2 ? 3 : 5; /* 2^N */
      redistrib.TaskRedistributionLevel = TASKREDISTRIB_BOM;
      redistrib.TaskRedistributionMode = TASKREDISTRIB_RR_STRICT;
   }
}

static void
emit_mesh_state(struct anv_graphics_pipeline *pipeline)
{
   assert(anv_pipeline_is_mesh(pipeline));

   const struct anv_shader_bin *mesh_bin = pipeline->base.shaders[MESA_SHADER_MESH];
   const struct brw_mesh_prog_data *mesh_prog_data =
      get_pipeline_mesh_prog_data(pipeline);

   uint32_t mesh_control_dwords[GENX(3DSTATE_MESH_CONTROL_length)];
   anv_pipeline_emit_tmp(pipeline, mesh_control_dwords, GENX(3DSTATE_MESH_CONTROL), mc) {
      mc.MeshShaderEnable = true;
      mc.StatisticsEnable = true;
      mc.MaximumNumberofThreadGroups = 511;
#if GFX_VER >= 20
      mc.VPandRTAIndexAutostripEnable = mesh_prog_data->autostrip_enable;
#endif
   }

   anv_pipeline_emit_merge(pipeline, final.mesh_control,
                           mesh_control_dwords, GENX(3DSTATE_MESH_CONTROL), mc) {
      mc.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base.base, MESA_SHADER_MESH, mesh_bin, false);
   }
   if (pipeline_needs_protected(&pipeline->base.base)) {
      anv_pipeline_emit_merge(pipeline, final.mesh_control_protected,
                           mesh_control_dwords, GENX(3DSTATE_MESH_CONTROL), mc) {
         mc.ScratchSpaceBuffer =
            get_scratch_surf(&pipeline->base.base, MESA_SHADER_MESH, mesh_bin, true);
      }
   }

   const struct intel_device_info *devinfo = pipeline->base.base.device->info;
   const struct intel_cs_dispatch_info mesh_dispatch =
      brw_cs_get_dispatch_info(devinfo, &mesh_prog_data->base, NULL);

   const unsigned output_topology =
      mesh_prog_data->primitive_type == MESA_PRIM_POINTS ? OUTPUT_POINT :
      mesh_prog_data->primitive_type == MESA_PRIM_LINES  ? OUTPUT_LINE :
                                                             OUTPUT_TRI;

   uint32_t index_format;
   switch (mesh_prog_data->index_format) {
   case BRW_INDEX_FORMAT_U32:
      index_format = INDEX_U32;
      break;
   case BRW_INDEX_FORMAT_U888X:
      index_format = INDEX_U888X;
      break;
   default:
      UNREACHABLE("invalid index format");
   }

   anv_pipeline_emit(pipeline, final.mesh_shader,
                     GENX(3DSTATE_MESH_SHADER), mesh) {
      mesh.KernelStartPointer                = mesh_bin->kernel.offset;
      mesh.SIMDSize                          = mesh_dispatch.simd_size / 16;
      mesh.MessageSIMD                       = mesh.SIMDSize;
      mesh.NumberofThreadsinGPGPUThreadGroup = mesh_dispatch.threads;
      mesh.ExecutionMask                     = mesh_dispatch.right_mask;
      mesh.LocalXMaximum                     = mesh_dispatch.group_size - 1;
      mesh.EmitLocalIDX                      = true;

      mesh.MaximumPrimitiveCount             = MAX2(mesh_prog_data->map.max_primitives, 1) - 1;
      mesh.OutputTopology                    = output_topology;
      mesh.PerVertexDataPitch                = mesh_prog_data->map.per_vertex_stride / 32;
      mesh.PerPrimitiveDataPresent           = mesh_prog_data->map.per_primitive_stride > 0;
      mesh.PerPrimitiveDataPitch             = mesh_prog_data->map.per_primitive_stride / 32;
      mesh.IndexFormat                       = index_format;

      mesh.NumberofBarriers                  = mesh_prog_data->base.uses_barrier;
      mesh.SharedLocalMemorySize             =
         intel_compute_slm_encode_size(GFX_VER, mesh_prog_data->base.base.total_shared);
      mesh.PreferredSLMAllocationSize        =
         intel_compute_preferred_slm_calc_encode_size(devinfo,
                                                      mesh_prog_data->base.base.total_shared,
                                                      mesh_dispatch.group_size,
                                                      mesh_dispatch.simd_size);

      mesh.EmitInlineParameter = mesh_prog_data->base.uses_inline_data;
      mesh.IndirectDataLength = align(mesh_bin->bind_map.push_ranges[0].length * 32, 64);

      mesh.XP0Required = mesh_prog_data->uses_drawid;

#if GFX_VER >= 30
      mesh.RegistersPerThread = ptl_register_blocks(mesh_prog_data->base.base.grf_used);
#endif
   }

   /* Recommended values from "Task and Mesh Distribution Programming". */
   anv_pipeline_emit(pipeline, final.mesh_distrib,
                     GENX(3DSTATE_MESH_DISTRIB), distrib) {
      distrib.DistributionMode = MESH_RR_FREE;
      distrib.TaskDistributionBatchSize = devinfo->num_slices > 2 ? 4 : 9; /* 2^N thread groups */
      distrib.MeshDistributionBatchSize = devinfo->num_slices > 2 ? 3 : 3; /* 2^N thread groups */
   }
}
#endif

void
genX(graphics_pipeline_emit)(struct anv_graphics_pipeline *pipeline,
                             const struct vk_graphics_pipeline_state *state)
{
   emit_rs_state(pipeline);
   compute_kill_pixel(pipeline, state->ms, state);

   emit_3dstate_clip(pipeline, state->ia, state->vp, state->rs);

#if GFX_VER >= 12
   emit_3dstate_primitive_replication(pipeline, state->rp);
#endif

#if GFX_VERx10 >= 125
   bool needs_instance_granularity =
      intel_needs_workaround(pipeline->base.base.device->info, 14019166699) &&
      (sbe_primitive_id_override(pipeline) ||
       geom_or_tess_prim_id_used(pipeline));

   anv_pipeline_emit(pipeline, partial.vfg, GENX(3DSTATE_VFG), vfg) {
      /* Gfx12.5: If 3DSTATE_TE: TE Enable == 1 then RR_STRICT else RR_FREE */
      vfg.DistributionMode =
#if GFX_VER < 20
         !anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL) ? RR_FREE :
#endif
         RR_STRICT;
      vfg.DistributionGranularity = needs_instance_granularity ?
         InstanceLevelGranularity : BatchLevelGranularity;
#if INTEL_WA_14014851047_GFX_VER
      vfg.GranularityThresholdDisable =
         intel_needs_workaround(pipeline->base.base.device->info, 14014851047);
#endif
      /* 192 vertices for TRILIST_ADJ */
      vfg.ListNBatchSizeScale = 0;
      /* Batch size of 384 vertices */
      vfg.List3BatchSizeScale = 2;
      /* Batch size of 128 vertices */
      vfg.List2BatchSizeScale = 1;
      /* Batch size of 128 vertices */
      vfg.List1BatchSizeScale = 2;
      /* Batch size of 256 vertices for STRIP topologies */
      vfg.StripBatchSizeScale = 3;
      /* 192 control points for PATCHLIST_3 */
      vfg.PatchBatchSizeScale = 1;
      /* 192 control points for PATCHLIST_3 */
      vfg.PatchBatchSizeMultiplier = 31;
   }
#endif

   if (anv_pipeline_is_primitive(pipeline)) {
      emit_vertex_input(pipeline, state, state->vi);

      emit_3dstate_vs(pipeline);
      emit_3dstate_hs_ds(pipeline, state->ts);
      emit_3dstate_te(pipeline);
      emit_3dstate_gs(pipeline);

      emit_3dstate_streamout(pipeline, state->rs);

#if GFX_VERx10 >= 125
      const struct anv_device *device = pipeline->base.base.device;
      /* Disable Mesh. */
      if (device->vk.enabled_extensions.EXT_mesh_shader) {
         anv_pipeline_emit(pipeline, final.mesh_control,
                           GENX(3DSTATE_MESH_CONTROL), zero);
         anv_pipeline_emit(pipeline, final.mesh_control_protected,
                           GENX(3DSTATE_MESH_CONTROL), zero);
         anv_pipeline_emit(pipeline, final.mesh_shader,
                           GENX(3DSTATE_MESH_SHADER), zero);
         anv_pipeline_emit(pipeline, final.mesh_distrib,
                           GENX(3DSTATE_MESH_DISTRIB), zero);
         anv_pipeline_emit(pipeline, final.task_control,
                           GENX(3DSTATE_TASK_CONTROL), zero);
         anv_pipeline_emit(pipeline, final.task_control_protected,
                           GENX(3DSTATE_TASK_CONTROL), zero);
         anv_pipeline_emit(pipeline, final.task_shader,
                           GENX(3DSTATE_TASK_SHADER), zero);
         anv_pipeline_emit(pipeline, final.task_redistrib,
                           GENX(3DSTATE_TASK_REDISTRIB), zero);
      }
#endif
   } else {
      assert(anv_pipeline_is_mesh(pipeline));

      anv_pipeline_emit(pipeline, final.vf_sgvs, GENX(3DSTATE_VF_SGVS), sgvs);
#if GFX_VER >= 11
      anv_pipeline_emit(pipeline, final.vf_sgvs_2, GENX(3DSTATE_VF_SGVS_2), sgvs);
#endif
      if (pipeline->base.base.device->physical->instance->vf_component_packing) {
         anv_pipeline_emit(pipeline, final.vf_component_packing,
                           GENX(3DSTATE_VF_COMPONENT_PACKING), vfc);
      }
      anv_pipeline_emit(pipeline, final.vs, GENX(3DSTATE_VS), vs);
      anv_pipeline_emit(pipeline, final.hs, GENX(3DSTATE_HS), hs);
      anv_pipeline_emit(pipeline, final.ds, GENX(3DSTATE_DS), ds);
      anv_pipeline_emit(pipeline, partial.te, GENX(3DSTATE_TE), te);
      anv_pipeline_emit(pipeline, partial.gs, GENX(3DSTATE_GS), gs);

      anv_pipeline_emit(pipeline, final.vs_protected, GENX(3DSTATE_VS), vs);
      anv_pipeline_emit(pipeline, final.hs_protected, GENX(3DSTATE_HS), hs);
      anv_pipeline_emit(pipeline, final.ds_protected, GENX(3DSTATE_DS), ds);
      anv_pipeline_emit(pipeline, partial.gs_protected, GENX(3DSTATE_GS), gs);

      /* BSpec 46303 forbids both 3DSTATE_MESH_CONTROL.MeshShaderEnable
       * and 3DSTATE_STREAMOUT.SOFunctionEnable to be 1.
       */
      anv_pipeline_emit(pipeline, partial.so, GENX(3DSTATE_STREAMOUT), so);

#if GFX_VERx10 >= 125
      emit_task_state(pipeline);
      emit_mesh_state(pipeline);
#endif
   }

   emit_3dstate_sbe(pipeline);
   emit_3dstate_wm(pipeline, state->ia, state->rs,
                   state->ms, state->cb, state->rp);
   emit_3dstate_ps(pipeline, state->ms, state->cb);
   emit_3dstate_ps_extra(pipeline, state->rs, state);
}

#if GFX_VERx10 >= 125

void
genX(compute_pipeline_emit)(struct anv_compute_pipeline *pipeline)
{
   const struct brw_cs_prog_data *prog_data =
      (const struct brw_cs_prog_data *)pipeline->cs->prog_data;
   const struct intel_device_info *devinfo = pipeline->base.device->info;
   const struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(devinfo, prog_data, NULL);
   const struct anv_shader_bin *shader = pipeline->cs;

   struct GENX(COMPUTE_WALKER) walker =  {
      GENX(COMPUTE_WALKER_header),
#if GFX_VERx10 == 125
      .SystolicModeEnable             = prog_data->uses_systolic,
#endif
      .body = {
         .SIMDSize                       = dispatch.simd_size / 16,
         .MessageSIMD                    = dispatch.simd_size / 16,
         .GenerateLocalID                = prog_data->generate_local_id != 0,
         .EmitLocal                      = prog_data->generate_local_id,
         .WalkOrder                      = prog_data->walk_order,
         .TileLayout                     = prog_data->walk_order == INTEL_WALK_ORDER_YXZ ?
                                           TileY32bpe : Linear,
         .LocalXMaximum                  = prog_data->local_size[0] - 1,
         .LocalYMaximum                  = prog_data->local_size[1] - 1,
         .LocalZMaximum                  = prog_data->local_size[2] - 1,
         .ExecutionMask                  = dispatch.right_mask,
         .PostSync                       = {
            .MOCS                        = anv_mocs(pipeline->base.device, NULL, 0),
         },
         .InterfaceDescriptor            = {
            .KernelStartPointer                = shader->kernel.offset,
            /* Typically set to 0 to avoid prefetching on every thread dispatch. */
            .BindingTableEntryCount            = devinfo->verx10 == 125 ?
            0 : 1 + MIN2(shader->bind_map.surface_count, 30),
            .NumberofThreadsinGPGPUThreadGroup = dispatch.threads,
            .ThreadGroupDispatchSize =
               intel_compute_threads_group_dispatch_size(dispatch.threads),
            .SharedLocalMemorySize             =
            intel_compute_slm_encode_size(GFX_VER, prog_data->base.total_shared),
            .PreferredSLMAllocationSize        =
            intel_compute_preferred_slm_calc_encode_size(devinfo,
                                                         prog_data->base.total_shared,
                                                         dispatch.group_size,
                                                         dispatch.simd_size),
            .NumberOfBarriers                  = prog_data->uses_barrier,
         },
         .EmitInlineParameter            = prog_data->uses_inline_push_addr,
      },
   };

   assert(ARRAY_SIZE(pipeline->gfx125.compute_walker) >= GENX(COMPUTE_WALKER_length));
   GENX(COMPUTE_WALKER_pack)(NULL, pipeline->gfx125.compute_walker, &walker);
}

#else /* #if GFX_VERx10 >= 125 */

void
genX(compute_pipeline_emit)(struct anv_compute_pipeline *pipeline)
{
   struct anv_device *device = pipeline->base.device;
   const struct intel_device_info *devinfo = device->info;
   const struct brw_cs_prog_data *cs_prog_data =
      (struct brw_cs_prog_data *) pipeline->cs->prog_data;

   const struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(devinfo, cs_prog_data, NULL);
   const uint32_t vfe_curbe_allocation =
      ALIGN(cs_prog_data->push.per_thread.regs * dispatch.threads +
            cs_prog_data->push.cross_thread.regs, 2);

   const struct anv_shader_bin *cs_bin = pipeline->cs;

   anv_batch_emit(&pipeline->base.batch, GENX(MEDIA_VFE_STATE), vfe) {
      vfe.StackSize              = 0;
      vfe.MaximumNumberofThreads =
         devinfo->max_cs_threads * devinfo->subslice_total - 1;
      vfe.NumberofURBEntries     = 2;
#if GFX_VER < 11
      vfe.ResetGatewayTimer      = true;
#endif
      vfe.URBEntryAllocationSize = 2;
      vfe.CURBEAllocationSize    = vfe_curbe_allocation;

      if (cs_prog_data->base.total_scratch) {
         /* Broadwell's Per Thread Scratch Space is in the range [0, 11]
          * where 0 = 1k, 1 = 2k, 2 = 4k, ..., 11 = 2M.
          */
         vfe.PerThreadScratchSpace = ffs(cs_prog_data->base.total_scratch) - 11;
         vfe.ScratchSpaceBasePointer =
            get_scratch_address(&pipeline->base, MESA_SHADER_COMPUTE, cs_bin);
      }
   }

   struct GENX(INTERFACE_DESCRIPTOR_DATA) desc = {
      .KernelStartPointer     =
         cs_bin->kernel.offset +
         brw_cs_prog_data_prog_offset(cs_prog_data, dispatch.simd_size),

      /* Wa_1606682166 */
      .SamplerCount           = GFX_VER == 11 ? 0 : get_sampler_count(cs_bin),
      /* We add 1 because the CS indirect parameters buffer isn't accounted
       * for in bind_map.surface_count.
       *
       * Typically set to 0 to avoid prefetching on every thread dispatch.
       */
      .BindingTableEntryCount = devinfo->verx10 == 125 ?
         0 : MIN2(pipeline->cs->bind_map.surface_count, 30),
      .BarrierEnable          = cs_prog_data->uses_barrier,
      .SharedLocalMemorySize  =
         intel_compute_slm_encode_size(GFX_VER, cs_prog_data->base.total_shared),

      .ConstantURBEntryReadOffset = 0,
      .ConstantURBEntryReadLength = cs_prog_data->push.per_thread.regs,
      .CrossThreadConstantDataReadLength =
         cs_prog_data->push.cross_thread.regs,
#if GFX_VER >= 12
      /* TODO: Check if we are missing workarounds and enable mid-thread
       * preemption.
       *
       * We still have issues with mid-thread preemption (it was already
       * disabled by the kernel on gfx11, due to missing workarounds). It's
       * possible that we are just missing some workarounds, and could enable
       * it later, but for now let's disable it to fix a GPU in compute in Car
       * Chase (and possibly more).
       */
      .ThreadPreemptionDisable = true,
#endif

      .NumberofThreadsinGPGPUThreadGroup = dispatch.threads,
   };
   GENX(INTERFACE_DESCRIPTOR_DATA_pack)(NULL,
                                        pipeline->gfx9.interface_descriptor_data,
                                        &desc);

   struct GENX(GPGPU_WALKER) walker = {
      GENX(GPGPU_WALKER_header),
      .SIMDSize                     = dispatch.simd_size / 16,
      .ThreadDepthCounterMaximum    = 0,
      .ThreadHeightCounterMaximum   = 0,
      .ThreadWidthCounterMaximum    = dispatch.threads - 1,
      .RightExecutionMask           = dispatch.right_mask,
      .BottomExecutionMask          = 0xffffffff,
   };
   GENX(GPGPU_WALKER_pack)(NULL, pipeline->gfx9.gpgpu_walker, &walker);
}

#endif /* #if GFX_VERx10 >= 125 */

#if GFX_VERx10 >= 125

void
genX(ray_tracing_pipeline_emit)(struct anv_ray_tracing_pipeline *pipeline)
{
   for (uint32_t i = 0; i < pipeline->group_count; i++) {
      struct anv_rt_shader_group *group = &pipeline->groups[i];

      switch (group->type) {
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR: {
         struct GENX(RT_GENERAL_SBT_HANDLE) sh = {};
         sh.General = anv_shader_bin_get_bsr(group->general, 32);
         GENX(RT_GENERAL_SBT_HANDLE_pack)(NULL, group->handle, &sh);
         break;
      }

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR: {
         struct GENX(RT_TRIANGLES_SBT_HANDLE) sh = {};
         struct anv_device *device = pipeline->base.device;
         if (group->closest_hit)
            sh.ClosestHit = anv_shader_bin_get_bsr(group->closest_hit, 32);
         if (group->any_hit)
            sh.AnyHit = anv_shader_bin_get_bsr(group->any_hit, 24);
         else
            sh.AnyHit = anv_shader_bin_get_bsr(device->rt_null_ahs, 24);
         GENX(RT_TRIANGLES_SBT_HANDLE_pack)(NULL, group->handle, &sh);
         break;
      }

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR: {
         struct GENX(RT_PROCEDURAL_SBT_HANDLE) sh = {};
         if (group->closest_hit)
            sh.ClosestHit = anv_shader_bin_get_bsr(group->closest_hit, 32);
         sh.Intersection = anv_shader_bin_get_bsr(group->intersection, 24);
         GENX(RT_PROCEDURAL_SBT_HANDLE_pack)(NULL, group->handle, &sh);
         break;
      }

      default:
         UNREACHABLE("Invalid shader group type");
      }
   }
}

#else

void
genX(ray_tracing_pipeline_emit)(struct anv_ray_tracing_pipeline *pipeline)
{
   UNREACHABLE("Ray tracing not supported");
}

#endif /* GFX_VERx10 >= 125 */
