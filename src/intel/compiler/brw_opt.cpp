/*
 * Copyright © 2010 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_eu.h"
#include "brw_shader.h"
#include "brw_builder.h"

#include "dev/intel_debug.h"

void
brw_optimize(brw_shader &s)
{
   const nir_shader *nir = s.nir;

   s.debug_optimizer(nir, "start", 0, 0);

   /* Start by validating the shader we currently have. */
   brw_validate(s);

   bool progress = false;
   int iteration = 0;
   int pass_num = 0;

#define OPT(pass, ...) ({                                               \
      pass_num++;                                                       \
      bool this_progress = pass(s, ##__VA_ARGS__);                      \
                                                                        \
      if (this_progress)                                                \
         s.debug_optimizer(nir, #pass, iteration, pass_num);            \
                                                                        \
      brw_validate(s);                                                  \
                                                                        \
      progress = progress || this_progress;                             \
      this_progress;                                                    \
   })

   if (s.compiler->lower_dpas)
      OPT(brw_lower_dpas);

   OPT(brw_opt_split_virtual_grfs);

   /* Before anything else, eliminate dead code.  The results of some NIR
    * instructions may effectively be calculated twice.  Once when the
    * instruction is encountered, and again when the user of that result is
    * encountered.  Wipe those away before algebraic optimizations and
    * especially copy propagation can mix things up.
    */
   OPT(brw_opt_dead_code_eliminate);

   OPT(brw_opt_remove_extra_rounding_modes);

   OPT(brw_opt_eliminate_find_live_channel);

   /* Add load_reg instructions before the main optimization loop to get more
    * defs available in those passes. Do it after the preceeding few pre-loop
    * passes so that it hopefully has less work to do. Having it here versus
    * before the call to opt_dce made some difference, but it was mostly
    * noise.
    */
   OPT(brw_insert_load_reg);

   /* Track how much non-SSA at this point. */
   {
      const brw_def_analysis &defs = s.def_analysis.require();
      s.shader_stats.non_ssa_registers_after_nir =
         defs.count() - defs.ssa_count();
   }

   do {
      progress = false;
      pass_num = 0;
      iteration++;

      OPT(brw_opt_algebraic);
      OPT(brw_opt_cse_defs);
      OPT(brw_opt_copy_propagation_defs);
      OPT(brw_opt_cmod_propagation);
      OPT(brw_opt_dead_code_eliminate);
      OPT(brw_opt_saturate_propagation);
      OPT(brw_opt_register_coalesce);

      OPT(brw_opt_compact_virtual_grfs);
   } while (progress);

   brw_shader_phase_update(s, BRW_SHADER_PHASE_AFTER_OPT_LOOP);

   progress = false;
   pass_num = 0;

   if (OPT(brw_opt_combine_convergent_txf))
      OPT(brw_opt_copy_propagation_defs);

   if (OPT(brw_lower_load_reg)) {
      OPT(brw_opt_copy_propagation);
      OPT(brw_opt_register_coalesce);
      OPT(brw_opt_dead_code_eliminate);
   }

   if (OPT(brw_lower_pack)) {
      OPT(brw_opt_register_coalesce);
      OPT(brw_opt_dead_code_eliminate);
   }

   OPT(brw_lower_subgroup_ops);
   OPT(brw_lower_csel);
   OPT(brw_lower_simd_width);
   OPT(brw_lower_scalar_fp64_MAD);
   OPT(brw_lower_barycentrics);
   OPT(brw_lower_logical_sends);

   brw_shader_phase_update(s, BRW_SHADER_PHASE_AFTER_EARLY_LOWERING);

   /* After logical SEND lowering. */

   if (!OPT(brw_opt_copy_propagation_defs))
      OPT(brw_opt_copy_propagation);

   /* Identify trailing zeros LOAD_PAYLOAD of sampler messages.
    * Do this before splitting SENDs.
    */
   if (OPT(brw_opt_zero_samples)) {
      if (!OPT(brw_opt_copy_propagation_defs)) {
         OPT(brw_opt_copy_propagation);
      }
   }

   if (s.devinfo->ver >= 30)
      OPT(brw_opt_send_to_send_gather);

   OPT(brw_opt_split_sends);
   OPT(brw_workaround_nomask_control_flow);

   if (progress) {
      /* Do both forms of copy propagation because it is important to
       * eliminate as many cases of load_payload-of-load_payload as possible.
       */
      OPT(brw_opt_copy_propagation_defs);
      OPT(brw_opt_copy_propagation);

      /* Run after logical send lowering to give it a chance to CSE the
       * LOAD_PAYLOAD instructions created to construct the payloads of
       * e.g. texturing messages in cases where it wasn't possible to CSE the
       * whole logical instruction.
       */
      OPT(brw_opt_cse_defs);
      OPT(brw_opt_register_coalesce);
      OPT(brw_opt_dead_code_eliminate);
   }

   OPT(brw_opt_remove_redundant_halts);

   if (OPT(brw_lower_load_payload)) {
      OPT(brw_opt_split_virtual_grfs);

      OPT(brw_opt_register_coalesce);
      OPT(brw_lower_simd_width);
      OPT(brw_opt_dead_code_eliminate);
   }

   brw_shader_phase_update(s, BRW_SHADER_PHASE_AFTER_MIDDLE_LOWERING);

   OPT(brw_opt_combine_constants);
   if (OPT(brw_lower_integer_multiplication)) {
      /* If lower_integer_multiplication made progress, it may have produced
       * some 32x32-bit MULs in the process of lowering 64-bit MULs.  Run it
       * one more time to clean those up if they exist.
       */
      OPT(brw_lower_integer_multiplication);
   }
   OPT(brw_lower_sub_sat);

   progress = false;
   OPT(brw_lower_derivatives);
   OPT(brw_lower_regioning);

   /* Try both copy propagation passes.  The defs one will likely not be
    * able to handle everything at this point.
    */
   const bool cp1 = OPT(brw_opt_copy_propagation_defs);
   const bool cp2 = OPT(brw_opt_copy_propagation);
   if (cp1 || cp2)
      OPT(brw_opt_combine_constants);

   OPT(brw_opt_dead_code_eliminate);
   OPT(brw_opt_register_coalesce);

   /* Logical sends and load_payload can have large VGRFs that cannot be
    * split. Once all of the lowering passes and optimization passes that
    * might eliminate any of those instructions have completed, try to split
    * larger VGRFs one last time.
    *
    * Register allocation can only handle VGRFs up to a certain size, so this
    * is the last opportunity to prevent later failures due to VGRFs that are
    * too large.
    */
   OPT(brw_opt_split_virtual_grfs);

   if (progress)
      OPT(brw_lower_simd_width);

   if (s.devinfo->ver >= 30)
      OPT(brw_opt_send_gather_to_send);

   OPT(brw_lower_uniform_pull_constant_loads);

   /* Do this before brw_lower_send_descriptors. */
   OPT(brw_workaround_memory_fence_before_eot);

   if (OPT(brw_lower_send_descriptors)) {
      /* No need for standard copy_propagation since
       * brw_opt_address_reg_load will only optimize defs.
       */
      OPT(brw_opt_copy_propagation_defs);
      OPT(brw_opt_algebraic);
      OPT(brw_opt_address_reg_load);
      OPT(brw_opt_dead_code_eliminate);
   }

   OPT(brw_lower_sends_overlapping_payload);

   OPT(brw_lower_indirect_mov);

   OPT(brw_lower_alu_restrictions);

   OPT(brw_lower_find_live_channel);

   OPT(brw_lower_load_subgroup_invocation);

   brw_shader_phase_update(s, BRW_SHADER_PHASE_AFTER_LATE_LOWERING);
}

static unsigned
load_payload_sources_read_for_size(brw_inst *lp, unsigned size_read)
{
   assert(lp->opcode == SHADER_OPCODE_LOAD_PAYLOAD);
   assert(size_read >= lp->header_size * REG_SIZE);

   unsigned i;
   unsigned size = lp->header_size * REG_SIZE;
   for (i = lp->header_size; size < size_read && i < lp->sources; i++)
      size += lp->exec_size * brw_type_size_bytes(lp->src[i].type);

   /* Size read must cover exactly a subset of sources. */
   assert(size == size_read);
   return i;
}

/**
 * Optimize sample messages that have constant zero values for the trailing
 * parameters. We can just reduce the message length for these
 * instructions instead of reserving a register for it. Trailing parameters
 * that aren't sent default to zero anyway. This will cause the dead code
 * eliminator to remove the MOV instruction that would otherwise be emitted to
 * set up the zero value.
 */

bool
brw_opt_zero_samples(brw_shader &s)
{
   bool progress = false;

   foreach_block_and_inst(block, brw_inst, send, s.cfg) {
      if (send->opcode != SHADER_OPCODE_SEND ||
          send->sfid != BRW_SFID_SAMPLER)
         continue;

      /* Wa_14012688258:
       *
       * Don't trim zeros at the end of payload for sample operations
       * in cube and cube arrays.
       */
      if (send->keep_payload_trailing_zeros)
         continue;

      /* This pass works on SENDs before splitting. */
      if (send->ex_mlen > 0)
         continue;

      brw_inst *lp = (brw_inst *) send->prev;

      if (lp->is_head_sentinel() || lp->opcode != SHADER_OPCODE_LOAD_PAYLOAD)
         continue;

      /* How much of the payload are actually read by this SEND. */
      const unsigned params =
         load_payload_sources_read_for_size(lp, send->mlen * REG_SIZE);

      /* We don't want to remove the message header or the first parameter.
       * Removing the first parameter is not allowed, see the Haswell PRM
       * volume 7, page 149:
       *
       *     "Parameter 0 is required except for the sampleinfo message, which
       *      has no parameter 0"
       */
      const unsigned first_param_idx = lp->header_size;
      unsigned zero_size = 0;
      for (unsigned i = params - 1; i > first_param_idx; i--) {
         if (lp->src[i].file != BAD_FILE && !lp->src[i].is_zero())
            break;
         zero_size += lp->exec_size * brw_type_size_bytes(lp->src[i].type) * lp->dst.stride;
      }

      /* Round down to ensure to only consider full registers. */
      const unsigned zero_len = ROUND_DOWN_TO(zero_size / REG_SIZE, reg_unit(s.devinfo));
      if (zero_len > 0) {
         /* Note mlen is in REG_SIZE units. */
         send->mlen -= zero_len;
         progress = true;
      }
   }

   if (progress)
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTION_DETAIL);

   return progress;
}

/**
 * Opportunistically split SEND message payloads.
 *
 * Gfx9+ supports "split" SEND messages, which take two payloads that are
 * implicitly concatenated.  If we find a SEND message with a single payload,
 * we can split that payload in two.  This results in smaller contiguous
 * register blocks for us to allocate.  But it can help beyond that, too.
 *
 * We try and split a LOAD_PAYLOAD between sources which change registers.
 * For example, a sampler message often contains a x/y/z coordinate that may
 * already be in a contiguous VGRF, combined with an LOD, shadow comparitor,
 * or array index, which comes from elsewhere.  In this case, the first few
 * sources will be different offsets of the same VGRF, then a later source
 * will be a different VGRF.  So we split there, possibly eliminating the
 * payload concatenation altogether.
 */
bool
brw_opt_split_sends(brw_shader &s)
{
   bool progress = false;

   foreach_block_and_inst(block, brw_inst, send, s.cfg) {
      if (send->opcode != SHADER_OPCODE_SEND ||
          send->mlen <= reg_unit(s.devinfo) || send->ex_mlen > 0 ||
          send->src[2].file != VGRF)
         continue;

      /* Currently don't split sends that reuse a previously used payload. */
      brw_inst *lp = (brw_inst *) send->prev;

      if (lp->is_head_sentinel() || lp->opcode != SHADER_OPCODE_LOAD_PAYLOAD)
         continue;

      if (lp->dst.file != send->src[2].file || lp->dst.nr != send->src[2].nr)
         continue;

      /* Split either after the header (if present), or when consecutive
       * sources switch from one VGRF to a different one.
       */
      unsigned mid = lp->header_size;
      if (mid == 0) {
         for (mid = 1; mid < lp->sources; mid++) {
            if (lp->src[mid].file == BAD_FILE)
               continue;

            if (lp->src[0].file != lp->src[mid].file ||
                lp->src[0].nr != lp->src[mid].nr)
               break;
         }
      }

      /* SEND mlen might be smaller than what LOAD_PAYLOAD provides, so
       * find out how many sources from the payload does it really need.
       */
      const unsigned end =
         load_payload_sources_read_for_size(lp, send->mlen * REG_SIZE);

      /* Nothing to split. */
      if (end <= mid)
         continue;

      const brw_builder ibld(lp);
      brw_inst *lp1 = ibld.LOAD_PAYLOAD(lp->dst, &lp->src[0], mid, lp->header_size);
      brw_inst *lp2 = ibld.LOAD_PAYLOAD(lp->dst, &lp->src[mid], end - mid, 0);

      assert(lp1->size_written % REG_SIZE == 0);
      assert(lp2->size_written % REG_SIZE == 0);
      assert((lp1->size_written + lp2->size_written) / REG_SIZE == send->mlen);

      lp1->dst = retype(brw_allocate_vgrf_units(s, lp1->size_written / REG_SIZE), lp1->dst.type);
      lp2->dst = retype(brw_allocate_vgrf_units(s, lp2->size_written / REG_SIZE), lp2->dst.type);

      send->resize_sources(4);
      send->src[2] = lp1->dst;
      send->src[3] = lp2->dst;
      send->ex_mlen = lp2->size_written / REG_SIZE;
      send->mlen -= send->ex_mlen;

      progress = true;
   }

   if (progress)
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTIONS |
                            BRW_DEPENDENCY_VARIABLES);

   return progress;
}

/**
 * Remove redundant or useless halts.
 *
 * For example, we can eliminate halts in the following sequence:
 *
 * halt        (redundant with the next halt)
 * halt        (useless; jumps to the next instruction)
 * halt-target
 */
bool
brw_opt_remove_redundant_halts(brw_shader &s)
{
   bool progress = false;

   unsigned halt_count = 0;
   brw_inst *halt_target = NULL;
   foreach_block_and_inst(block, brw_inst, inst, s.cfg) {
      if (inst->opcode == BRW_OPCODE_HALT)
         halt_count++;

      if (inst->opcode == SHADER_OPCODE_HALT_TARGET) {
         halt_target = inst;
         break;
      }
   }

   if (!halt_target) {
      assert(halt_count == 0);
      return false;
   }

   /* Delete any HALTs immediately before the halt target. */
   for (brw_inst *prev = (brw_inst *) halt_target->prev;
        !prev->is_head_sentinel() && prev->opcode == BRW_OPCODE_HALT;
        prev = (brw_inst *) halt_target->prev) {
      prev->remove();
      halt_count--;
      progress = true;
   }

   if (halt_count == 0) {
      halt_target->remove();
      progress = true;
   }

   if (progress)
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTIONS);

   return progress;
}

/**
 * Eliminate FIND_LIVE_CHANNEL instructions occurring outside any control
 * flow.  We could probably do better here with some form of divergence
 * analysis.
 */
bool
brw_opt_eliminate_find_live_channel(brw_shader &s)
{
   bool progress = false;
   unsigned depth = 0;

   if (!brw_stage_has_packed_dispatch(s.devinfo, s.stage, s.max_polygons,
                                      s.prog_data)) {
      /* The optimization below assumes that channel zero is live on thread
       * dispatch, which may not be the case if the fixed function dispatches
       * threads sparsely.
       */
      return false;
   }

   foreach_block_and_inst_safe(block, brw_inst, inst, s.cfg) {
      switch (inst->opcode) {
      case BRW_OPCODE_IF:
      case BRW_OPCODE_DO:
         depth++;
         break;

      case BRW_OPCODE_ENDIF:
      case BRW_OPCODE_WHILE:
         depth--;
         break;

      case BRW_OPCODE_HALT:
         /* This can potentially make control flow non-uniform until the end
          * of the program.
          */
         goto out;

      case SHADER_OPCODE_FIND_LIVE_CHANNEL:
         if (depth == 0) {
            inst->opcode = BRW_OPCODE_MOV;
            inst->src[0] = brw_imm_ud(0u);
            inst->force_writemask_all = true;

            /* FIND_LIVE_CHANNEL emitted by emit_uniformize will have
             * size_written set by hand to a smaller value. In this case,
             * munge the exec_size to match.
             */
            if (inst->size_written == inst->dst.component_size(8 * reg_unit(s.devinfo)))
               inst->exec_size = 8 * reg_unit(s.devinfo);

            inst->resize_sources(1);
            progress = true;

            /* emit_uniformize() frequently emits FIND_LIVE_CHANNEL paired
             * with a BROADCAST.  Save some work for opt_copy_propagation
             * and opt_algebraic by trivially cleaning up both together.
             */
            assert(!inst->next->is_tail_sentinel());
            brw_inst *bcast = (brw_inst *) inst->next;

            /* Ignore stride when comparing */
            if (bcast->opcode == SHADER_OPCODE_BROADCAST &&
                inst->dst.file == VGRF &&
                inst->dst.file == bcast->src[1].file &&
                inst->dst.nr == bcast->src[1].nr &&
                inst->dst.offset == bcast->src[1].offset) {
               bcast->opcode = BRW_OPCODE_MOV;
               if (!is_uniform(bcast->src[0]))
                  bcast->src[0] = component(bcast->src[0], 0);

               bcast->force_writemask_all = true;
               bcast->exec_size = 8 * reg_unit(s.devinfo);
               assert(bcast->size_written == bcast->dst.component_size(bcast->exec_size));
               bcast->resize_sources(1);
            }
         }
         break;

      default:
         break;
      }
   }

out:
   if (progress)
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTION_DATA_FLOW |
                            BRW_DEPENDENCY_INSTRUCTION_DETAIL);

   return progress;
}

/**
 * Rounding modes for conversion instructions are included for each
 * conversion, but right now it is a state. So once it is set,
 * we don't need to call it again for subsequent calls.
 *
 * This is useful for vector/matrices conversions, as setting the
 * mode once is enough for the full vector/matrix
 */
bool
brw_opt_remove_extra_rounding_modes(brw_shader &s)
{
   bool progress = false;
   unsigned execution_mode = s.nir->info.float_controls_execution_mode;

   brw_rnd_mode base_mode = BRW_RND_MODE_UNSPECIFIED;
   if ((FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP16 |
        FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP32 |
        FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP64) &
       execution_mode)
      base_mode = BRW_RND_MODE_RTNE;
   if ((FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16 |
        FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP32 |
        FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP64) &
       execution_mode)
      base_mode = BRW_RND_MODE_RTZ;

   foreach_block (block, s.cfg) {
      brw_rnd_mode prev_mode = base_mode;

      foreach_inst_in_block_safe (brw_inst, inst, block) {
         if (inst->opcode == SHADER_OPCODE_RND_MODE) {
            assert(inst->src[0].file == IMM);
            const brw_rnd_mode mode = (brw_rnd_mode) inst->src[0].d;
            if (mode == prev_mode) {
               inst->remove();
               progress = true;
            } else {
               prev_mode = mode;
            }
         }
      }
   }

   if (progress)
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTIONS);

   return progress;
}

bool
brw_opt_send_to_send_gather(brw_shader &s)
{
   const intel_device_info *devinfo = s.devinfo;
   bool progress = false;

   assert(devinfo->ver >= 30);

   const unsigned unit = reg_unit(devinfo);
   assert(unit == 2);

   unsigned count = 0;

   foreach_block_and_inst_safe(block, brw_inst, inst, s.cfg) {
      if (inst->opcode != SHADER_OPCODE_SEND)
         continue;

      /* For 1-2 registers, send-gather offers no benefits over split-send. */
      if (inst->mlen + inst->ex_mlen <= 2 * unit)
         continue;

      assert(inst->mlen % unit == 0);
      assert(inst->ex_mlen % unit == 0);

      struct {
         brw_reg src;
         unsigned phys_len;
      } payload[2] = {
         { inst->src[2], inst->mlen / unit },
         { inst->src[3], inst->ex_mlen / unit },
      };

      const unsigned num_payload_sources = payload[0].phys_len + payload[1].phys_len;

      /* Limited by Src0.Length in the SEND instruction. */
      if (num_payload_sources > 15)
         continue;

      if (INTEL_DEBUG(DEBUG_NO_SEND_GATHER)) {
         count++;
         continue;
      }

      inst->resize_sources(3 + num_payload_sources);
      /* Sources 0 and 1 remain the same.  Source 2 will be filled
       * after register allocation.
       */
      inst->src[2] = {};

      int idx = 3;
      for (unsigned p = 0; p < ARRAY_SIZE(payload); p++) {
         for (unsigned i = 0; i < payload[p].phys_len; i++) {
            inst->src[idx++] = byte_offset(payload[p].src,
                                           i * reg_unit(devinfo) * REG_SIZE);
         }
      }
      assert(idx == inst->sources);

      inst->opcode = SHADER_OPCODE_SEND_GATHER;
      inst->mlen = 0;
      inst->ex_mlen = 0;

      progress = true;
   }

   if (INTEL_DEBUG(DEBUG_NO_SEND_GATHER)) {
      fprintf(stderr, "Ignored %u opportunities to try SEND_GATHER in %s shader.\n",
              count, _mesa_shader_stage_to_string(s.stage));
   }

   if (progress)
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTION_DETAIL |
                            BRW_DEPENDENCY_INSTRUCTION_DATA_FLOW);

   return progress;
}

/* If after optimizations, the sources are *still* contiguous in a
 * SEND_GATHER, prefer to use the regular SEND, which would save
 * having to write the ARF scalar register.
 */
bool
brw_opt_send_gather_to_send(brw_shader &s)
{
   const intel_device_info *devinfo = s.devinfo;
   bool progress = false;

   assert(devinfo->ver >= 30);

   const unsigned unit = reg_unit(devinfo);
   assert(unit == 2);

   foreach_block_and_inst_safe(block, brw_inst, inst, s.cfg) {
      if (inst->opcode != SHADER_OPCODE_SEND_GATHER)
         continue;

      assert(inst->sources > 2);
      assert(inst->src[2].file == BAD_FILE);

      const int num_payload_sources = inst->sources - 3;
      assert(num_payload_sources > 0);

      /* Limited by Src0.Length in the SEND instruction. */
      assert(num_payload_sources < 16);

      /* Determine whether the sources are still spread in either one or two
       * spans.  In those cases the regular SEND instruction can be used
       * and there's no need to use SEND_GATHER (which would set ARF scalar register
       * adding an extra instruction).
       */
      const brw_reg *payload = &inst->src[3];
      brw_reg payload1       = payload[0];
      brw_reg payload2       = {};
      int payload1_len       = 0;
      int payload2_len       = 0;

      for (int i = 0; i < num_payload_sources; i++) {
         if (payload[i].file == VGRF &&
             payload[i].nr == payload1.nr &&
             payload[i].offset == payload1_len * REG_SIZE * unit)
            payload1_len++;
         else {
            payload2 = payload[i];
            break;
         }
      }

      if (payload2.file == VGRF) {
         for (int i = payload1_len; i < num_payload_sources; i++) {
            if (payload[i].file == VGRF &&
                payload[i].nr == payload2.nr &&
                payload[i].offset == payload2_len * REG_SIZE * unit)
               payload2_len++;
            else
               break;
         }
      } else {
         payload2 = brw_null_reg();
      }

      if (payload1_len + payload2_len != num_payload_sources)
         continue;

      /* Bspec 57058 (r64705) says
       *
       *    When a source data payload is used in dataport message, that payload
       *    must be specified as Source 1 portion of a Split Send message.
       *
       * But at this point the split point is not guaranteed to respect that.
       *
       * TODO: Pass LSC address length or infer it so valid splits can work.
       */
      if (payload2_len && (inst->sfid == BRW_SFID_UGM ||
                           inst->sfid == BRW_SFID_TGM ||
                           inst->sfid == BRW_SFID_SLM ||
                           inst->sfid == BRW_SFID_URB)) {
         enum lsc_opcode lsc_op = lsc_msg_desc_opcode(devinfo, inst->desc);
         if (lsc_op_num_data_values(lsc_op) > 0)
            continue;
      }

      inst->resize_sources(4);
      inst->opcode  = SHADER_OPCODE_SEND;
      inst->src[2]  = payload1;
      inst->src[3]  = payload2;
      inst->mlen    = payload1_len * unit;
      inst->ex_mlen = payload2_len * unit;

      progress = true;
   }

   if (progress) {
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTION_DETAIL |
                            BRW_DEPENDENCY_INSTRUCTION_DATA_FLOW);
   }

   return progress;
}
