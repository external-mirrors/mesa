/*
 * Copyright © 2018 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include "common/ac_shader_util.h"
#include "common/sid.h"

#include <array>

namespace aco {

namespace {

const std::array<const char*, num_reduce_ops> reduce_ops = []()
{
   std::array<const char*, num_reduce_ops> ret{};
   ret[iadd8] = "iadd8";
   ret[iadd16] = "iadd16";
   ret[iadd32] = "iadd32";
   ret[iadd64] = "iadd64";
   ret[imul8] = "imul8";
   ret[imul16] = "imul16";
   ret[imul32] = "imul32";
   ret[imul64] = "imul64";
   ret[fadd16] = "fadd16";
   ret[fadd32] = "fadd32";
   ret[fadd64] = "fadd64";
   ret[fmul16] = "fmul16";
   ret[fmul32] = "fmul32";
   ret[fmul64] = "fmul64";
   ret[imin8] = "imin8";
   ret[imin16] = "imin16";
   ret[imin32] = "imin32";
   ret[imin64] = "imin64";
   ret[imax8] = "imax8";
   ret[imax16] = "imax16";
   ret[imax32] = "imax32";
   ret[imax64] = "imax64";
   ret[umin8] = "umin8";
   ret[umin16] = "umin16";
   ret[umin32] = "umin32";
   ret[umin64] = "umin64";
   ret[umax8] = "umax8";
   ret[umax16] = "umax16";
   ret[umax32] = "umax32";
   ret[umax64] = "umax64";
   ret[fmin16] = "fmin16";
   ret[fmin32] = "fmin32";
   ret[fmin64] = "fmin64";
   ret[fmax16] = "fmax16";
   ret[fmax32] = "fmax32";
   ret[fmax64] = "fmax64";
   ret[iand8] = "iand8";
   ret[iand16] = "iand16";
   ret[iand32] = "iand32";
   ret[iand64] = "iand64";
   ret[ior8] = "ior8";
   ret[ior16] = "ior16";
   ret[ior32] = "ior32";
   ret[ior64] = "ior64";
   ret[ixor8] = "ixor8";
   ret[ixor16] = "ixor16";
   ret[ixor32] = "ixor32";
   ret[ixor64] = "ixor64";
   return ret;
}();

static void
print_reg_class(const RegClass rc, FILE* output)
{
   if (rc.is_subdword()) {
      fprintf(output, " v%ub: ", rc.bytes());
   } else if (rc.type() == RegType::sgpr) {
      fprintf(output, " s%u: ", rc.size());
   } else if (rc.is_linear()) {
      fprintf(output, " lv%u: ", rc.size());
   } else {
      fprintf(output, " v%u: ", rc.size());
   }
}

void
print_physReg(PhysReg reg, unsigned bytes, FILE* output, unsigned flags)
{
   if (reg == 106) {
      fprintf(output, bytes > 4 ? "vcc" : "vcc_lo");
   } else if (reg == 107) {
      fprintf(output, "vcc_hi");
   } else if (reg == 124) {
      fprintf(output, "m0");
   } else if (reg == 125) {
      fprintf(output, "null");
   } else if (reg == 126) {
      fprintf(output, bytes > 4 ? "exec" : "exec_lo");
   } else if (reg == 127) {
      fprintf(output, "exec_hi");
   } else if (reg == 253) {
      fprintf(output, "scc");
   } else {
      bool is_vgpr = reg / 256;
      unsigned r = reg % 256;
      unsigned size = DIV_ROUND_UP(bytes, 4);
      if (size == 1 && (flags & print_no_ssa)) {
         fprintf(output, "%c%d", is_vgpr ? 'v' : 's', r);
      } else {
         fprintf(output, "%c[%d", is_vgpr ? 'v' : 's', r);
         if (size > 1)
            fprintf(output, "-%d]", r + size - 1);
         else
            fprintf(output, "]");
      }
      if (reg.byte() || bytes % 4)
         fprintf(output, "[%d:%d]", reg.byte() * 8, (reg.byte() + bytes) * 8);
   }
}

static void
print_constant(uint8_t reg, FILE* output)
{
   if (reg >= 128 && reg <= 192) {
      fprintf(output, "%d", reg - 128);
      return;
   } else if (reg >= 192 && reg <= 208) {
      fprintf(output, "%d", 192 - reg);
      return;
   }

   switch (reg) {
   case 240: fprintf(output, "0.5"); break;
   case 241: fprintf(output, "-0.5"); break;
   case 242: fprintf(output, "1.0"); break;
   case 243: fprintf(output, "-1.0"); break;
   case 244: fprintf(output, "2.0"); break;
   case 245: fprintf(output, "-2.0"); break;
   case 246: fprintf(output, "4.0"); break;
   case 247: fprintf(output, "-4.0"); break;
   case 248: fprintf(output, "1/(2*PI)"); break;
   }
}

static void
print_definition(const Definition* definition, FILE* output, unsigned flags)
{
   if (!(flags & print_no_ssa))
      print_reg_class(definition->regClass(), output);
   if (definition->isPrecise())
      fprintf(output, "(precise)");
   if (definition->isInfPreserve() || definition->isNaNPreserve() || definition->isSZPreserve()) {
      fprintf(output, "(");
      if (definition->isSZPreserve())
         fprintf(output, "Sz");
      if (definition->isInfPreserve())
         fprintf(output, "Inf");
      if (definition->isNaNPreserve())
         fprintf(output, "NaN");
      fprintf(output, "Preserve)");
   }
   if (definition->isNUW())
      fprintf(output, "(nuw)");
   if (definition->isNoCSE())
      fprintf(output, "(noCSE)");
   if ((flags & print_kill) && definition->isKill())
      fprintf(output, "(kill)");
   if (!(flags & print_no_ssa))
      fprintf(output, "%%%d%s", definition->tempId(), definition->isFixed() ? ":" : "");

   if (definition->isFixed())
      print_physReg(definition->physReg(), definition->bytes(), output, flags);
}

static void
print_storage(storage_class storage, FILE* output)
{
   fprintf(output, " storage:");
   int printed = 0;
   if (storage & storage_buffer)
      printed += fprintf(output, "%sbuffer", printed ? "," : "");
   if (storage & storage_gds)
      printed += fprintf(output, "%sgds", printed ? "," : "");
   if (storage & storage_image)
      printed += fprintf(output, "%simage", printed ? "," : "");
   if (storage & storage_shared)
      printed += fprintf(output, "%sshared", printed ? "," : "");
   if (storage & storage_task_payload)
      printed += fprintf(output, "%stask_payload", printed ? "," : "");
   if (storage & storage_vmem_output)
      printed += fprintf(output, "%svmem_output", printed ? "," : "");
   if (storage & storage_scratch)
      printed += fprintf(output, "%sscratch", printed ? "," : "");
   if (storage & storage_vgpr_spill)
      printed += fprintf(output, "%svgpr_spill", printed ? "," : "");
}

static void
print_semantics(memory_semantics sem, FILE* output)
{
   fprintf(output, " semantics:");
   int printed = 0;
   if (sem & semantic_acquire)
      printed += fprintf(output, "%sacquire", printed ? "," : "");
   if (sem & semantic_release)
      printed += fprintf(output, "%srelease", printed ? "," : "");
   if (sem & semantic_volatile)
      printed += fprintf(output, "%svolatile", printed ? "," : "");
   if (sem & semantic_private)
      printed += fprintf(output, "%sprivate", printed ? "," : "");
   if (sem & semantic_can_reorder)
      printed += fprintf(output, "%sreorder", printed ? "," : "");
   if (sem & semantic_atomic)
      printed += fprintf(output, "%satomic", printed ? "," : "");
   if (sem & semantic_rmw)
      printed += fprintf(output, "%srmw", printed ? "," : "");
}

static void
print_scope(sync_scope scope, FILE* output, const char* prefix = "scope")
{
   fprintf(output, " %s:", prefix);
   switch (scope) {
   case scope_invocation: fprintf(output, "invocation"); break;
   case scope_subgroup: fprintf(output, "subgroup"); break;
   case scope_workgroup: fprintf(output, "workgroup"); break;
   case scope_queuefamily: fprintf(output, "queuefamily"); break;
   case scope_device: fprintf(output, "device"); break;
   }
}

static void
print_sync(memory_sync_info sync, FILE* output)
{
   if (sync.storage)
      print_storage(sync.storage, output);
   if (sync.semantics)
      print_semantics(sync.semantics, output);
   if (sync.scope != scope_invocation)
      print_scope(sync.scope, output);
}

template <typename T>
static void
print_cache_flags(enum amd_gfx_level gfx_level, const T& instr, FILE* output)
{
   if (gfx_level >= GFX12) {
      if (instr_info.is_atomic[(unsigned)instr.opcode]) {
         if (instr.cache.gfx12.temporal_hint & gfx12_atomic_return)
            fprintf(output, " atomic_return");
         if (instr.cache.gfx12.temporal_hint & gfx12_atomic_non_temporal)
            fprintf(output, " non_temporal");
         if (instr.cache.gfx12.temporal_hint & gfx12_atomic_accum_deferred_scope)
            fprintf(output, " accum_deferred_scope");
      } else if (instr.definitions.empty()) {
         switch (instr.cache.gfx12.temporal_hint) {
         case gfx12_load_regular_temporal: break;
         case gfx12_load_non_temporal: fprintf(output, " non_temporal"); break;
         case gfx12_load_high_temporal: fprintf(output, " high_temporal"); break;
         case gfx12_load_last_use_discard: fprintf(output, " last_use_discard"); break;
         case gfx12_load_near_non_temporal_far_regular_temporal:
            fprintf(output, " near_non_temporal_far_regular_temporal");
            break;
         case gfx12_load_near_regular_temporal_far_non_temporal:
            fprintf(output, " near_regular_temporal_far_non_temporal");
            break;
         case gfx12_load_near_non_temporal_far_high_temporal:
            fprintf(output, " near_non_temporal_far_high_temporal");
            break;
         case gfx12_load_reserved: fprintf(output, " reserved"); break;
         default: fprintf(output, "tmp:%u", (unsigned)instr.cache.gfx12.temporal_hint);
         }
      } else {
         switch (instr.cache.gfx12.temporal_hint) {
         case gfx12_store_regular_temporal: break;
         case gfx12_store_non_temporal: fprintf(output, " non_temporal"); break;
         case gfx12_store_high_temporal: fprintf(output, " high_temporal"); break;
         case gfx12_store_high_temporal_stay_dirty:
            fprintf(output, " high_temporal_stay_dirty");
            break;
         case gfx12_store_near_non_temporal_far_regular_temporal:
            fprintf(output, " near_non_temporal_far_regular_temporal");
            break;
         case gfx12_store_near_regular_temporal_far_non_temporal:
            fprintf(output, " near_regular_temporal_far_non_temporal");
            break;
         case gfx12_store_near_non_temporal_far_high_temporal:
            fprintf(output, " near_non_temporal_far_high_temporal");
            break;
         case gfx12_store_near_non_temporal_far_writeback:
            fprintf(output, " near_non_temporal_far_writeback");
            break;
         default: fprintf(output, "tmp:%u", (unsigned)instr.cache.gfx12.temporal_hint);
         }
      }
      switch (instr.cache.gfx12.scope) {
      case gfx12_scope_cu: break;
      case gfx12_scope_se: fprintf(output, " se"); break;
      case gfx12_scope_device: fprintf(output, " device"); break;
      case gfx12_scope_memory: fprintf(output, " memory"); break;
      }
      if (instr.cache.gfx12.swizzled)
         fprintf(output, " swizzled");
   } else {
      if (instr.cache.value & ac_glc)
         fprintf(output, " glc");
      if (instr.cache.value & ac_slc)
         fprintf(output, " slc");
      if (instr.cache.value & ac_dlc)
         fprintf(output, " dlc");
      if (instr.cache.value & ac_swizzled)
         fprintf(output, " swizzled");
   }
}

static void
print_instr_format_specific(enum amd_gfx_level gfx_level, const Instruction* instr, FILE* output)
{
   switch (instr->format) {
   case Format::SOPK: {
      const SALU_instruction& sopk = instr->salu();
      fprintf(output, " imm:%d", sopk.imm & 0x8000 ? (sopk.imm - 65536) : sopk.imm);
      break;
   }
   case Format::SOPP: {
      uint16_t imm = instr->salu().imm;
      switch (instr->opcode) {
      case aco_opcode::s_waitcnt:
      case aco_opcode::s_wait_loadcnt_dscnt:
      case aco_opcode::s_wait_storecnt_dscnt: {
         wait_imm unpacked;
         unpacked.unpack(gfx_level, instr);
         const char* names[wait_type_num];
         names[wait_type_exp] = "expcnt";
         names[wait_type_vm] = gfx_level >= GFX12 ? "loadcnt" : "vmcnt";
         names[wait_type_lgkm] = gfx_level >= GFX12 ? "dscnt" : "lgkmcnt";
         names[wait_type_vs] = gfx_level >= GFX12 ? "storecnt" : "vscnt";
         names[wait_type_sample] = "samplecnt";
         names[wait_type_bvh] = "bvhcnt";
         names[wait_type_km] = "kmcnt";
         for (unsigned i = 0; i < wait_type_num; i++) {
            if (unpacked[i] != wait_imm::unset_counter)
               fprintf(output, " %s(%d)", names[i], unpacked[i]);
         }
         break;
      }
      case aco_opcode::s_wait_expcnt:
      case aco_opcode::s_wait_dscnt:
      case aco_opcode::s_wait_loadcnt:
      case aco_opcode::s_wait_storecnt:
      case aco_opcode::s_wait_samplecnt:
      case aco_opcode::s_wait_bvhcnt:
      case aco_opcode::s_wait_kmcnt:
      case aco_opcode::s_setprio: {
         fprintf(output, " imm:%u", imm);
         break;
      }
      case aco_opcode::s_waitcnt_depctr: {
         depctr_wait wait = parse_depctr_wait(instr);
         if (wait.va_vdst != 0xf)
            fprintf(output, " va_vdst(%d)", wait.va_vdst);
         if (wait.va_sdst != 0x7)
            fprintf(output, " va_sdst(%d)", wait.va_sdst);
         if (wait.va_ssrc != 0x1)
            fprintf(output, " va_ssrc(%d)", wait.va_ssrc);
         if (wait.hold_cnt != 0x1)
            fprintf(output, " holt_cnt(%d)", wait.hold_cnt);
         if (wait.vm_vsrc != 0x7)
            fprintf(output, " vm_vsrc(%d)", wait.vm_vsrc);
         if (wait.va_vcc != 0x1)
            fprintf(output, " va_vcc(%d)", wait.va_vcc);
         if (wait.sa_sdst != 0x1)
            fprintf(output, " sa_sdst(%d)", wait.sa_sdst);
         break;
      }
      case aco_opcode::s_delay_alu: {
         unsigned delay[2] = {imm & 0xfu, (imm >> 7) & 0xfu};
         unsigned skip = (imm >> 4) & 0x7;
         for (unsigned i = 0; i < 2; i++) {
            if (i == 1 && skip) {
               if (skip == 1)
                  fprintf(output, " next");
               else
                  fprintf(output, " skip_%u", skip - 1);
            }

            alu_delay_wait wait = (alu_delay_wait)delay[i];
            if (wait >= alu_delay_wait::VALU_DEP_1 && wait <= alu_delay_wait::VALU_DEP_4)
               fprintf(output, " valu_dep_%u", delay[i]);
            else if (wait >= alu_delay_wait::TRANS32_DEP_1 && wait <= alu_delay_wait::TRANS32_DEP_3)
               fprintf(output, " trans32_dep_%u",
                       delay[i] - (unsigned)alu_delay_wait::TRANS32_DEP_1 + 1);
            else if (wait == alu_delay_wait::FMA_ACCUM_CYCLE_1)
               fprintf(output, " fma_accum_cycle_1");
            else if (wait >= alu_delay_wait::SALU_CYCLE_1 && wait <= alu_delay_wait::SALU_CYCLE_3)
               fprintf(output, " salu_cycle_%u",
                       delay[i] - (unsigned)alu_delay_wait::SALU_CYCLE_1 + 1);
         }
         break;
      }
      case aco_opcode::s_endpgm:
      case aco_opcode::s_endpgm_saved:
      case aco_opcode::s_endpgm_ordered_ps_done:
      case aco_opcode::s_wakeup:
      case aco_opcode::s_barrier:
      case aco_opcode::s_icache_inv:
      case aco_opcode::s_ttracedata:
      case aco_opcode::s_set_gpr_idx_off: {
         break;
      }
      case aco_opcode::s_sendmsg: {
         unsigned id = imm & sendmsg_id_mask;
         static_assert(sendmsg_gs == sendmsg_hs_tessfactor);
         static_assert(sendmsg_gs_done == sendmsg_dealloc_vgprs);
         switch (id) {
         case sendmsg_none: fprintf(output, " sendmsg(MSG_NONE)"); break;
         case sendmsg_gs:
            if (gfx_level >= GFX11)
               fprintf(output, " sendmsg(hs_tessfactor)");
            else
               fprintf(output, " sendmsg(gs%s%s, %u)", imm & 0x10 ? ", cut" : "",
                       imm & 0x20 ? ", emit" : "", imm >> 8);
            break;
         case sendmsg_gs_done:
            if (gfx_level >= GFX11)
               fprintf(output, " sendmsg(dealloc_vgprs)");
            else
               fprintf(output, " sendmsg(gs_done%s%s, %u)", imm & 0x10 ? ", cut" : "",
                       imm & 0x20 ? ", emit" : "", imm >> 8);
            break;
         case sendmsg_save_wave: fprintf(output, " sendmsg(save_wave)"); break;
         case sendmsg_stall_wave_gen: fprintf(output, " sendmsg(stall_wave_gen)"); break;
         case sendmsg_halt_waves: fprintf(output, " sendmsg(halt_waves)"); break;
         case sendmsg_ordered_ps_done: fprintf(output, " sendmsg(ordered_ps_done)"); break;
         case sendmsg_early_prim_dealloc: fprintf(output, " sendmsg(early_prim_dealloc)"); break;
         case sendmsg_gs_alloc_req: fprintf(output, " sendmsg(gs_alloc_req)"); break;
         case sendmsg_get_doorbell: fprintf(output, " sendmsg(get_doorbell)"); break;
         case sendmsg_get_ddid: fprintf(output, " sendmsg(get_ddid)"); break;
         default: fprintf(output, " imm:%u", imm);
         }
         break;
      }
      case aco_opcode::s_wait_event: {
         if (is_wait_export_ready(gfx_level, instr))
            fprintf(output, " wait_export_ready");
         break;
      }
      default: {
         if (instr_info.classes[(int)instr->opcode] == instr_class::branch)
            fprintf(output, " block:BB%d", imm);
         else if (imm)
            fprintf(output, " imm:%u", imm);
         break;
      }
      }
      break;
   }
   case Format::SOP1: {
      if (instr->opcode == aco_opcode::s_sendmsg_rtn_b32 ||
          instr->opcode == aco_opcode::s_sendmsg_rtn_b64) {
         unsigned id = instr->operands[0].constantValue();
         switch (id) {
         case sendmsg_rtn_get_doorbell: fprintf(output, " sendmsg(rtn_get_doorbell)"); break;
         case sendmsg_rtn_get_ddid: fprintf(output, " sendmsg(rtn_get_ddid)"); break;
         case sendmsg_rtn_get_tma: fprintf(output, " sendmsg(rtn_get_tma)"); break;
         case sendmsg_rtn_get_realtime: fprintf(output, " sendmsg(rtn_get_realtime)"); break;
         case sendmsg_rtn_save_wave: fprintf(output, " sendmsg(rtn_save_wave)"); break;
         case sendmsg_rtn_get_tba: fprintf(output, " sendmsg(rtn_get_tba)"); break;
         default: break;
         }
         break;
      }
      break;
   }
   case Format::SMEM: {
      const SMEM_instruction& smem = instr->smem();
      print_cache_flags(gfx_level, smem, output);
      print_sync(smem.sync, output);
      break;
   }
   case Format::VINTERP_INREG: {
      const VINTERP_inreg_instruction& vinterp = instr->vinterp_inreg();
      if (vinterp.wait_exp != 7)
         fprintf(output, " wait_exp:%u", vinterp.wait_exp);
      break;
   }
   case Format::VINTRP: {
      const VINTRP_instruction& vintrp = instr->vintrp();
      fprintf(output, " attr%d.%c", vintrp.attribute, "xyzw"[vintrp.component]);
      if (vintrp.high_16bits)
         fprintf(output, " high");
      break;
   }
   case Format::DS: {
      const DS_instruction& ds = instr->ds();
      if (ds.offset0)
         fprintf(output, " offset0:%u", ds.offset0);
      if (ds.offset1)
         fprintf(output, " offset1:%u", ds.offset1);
      if (ds.gds)
         fprintf(output, " gds");
      print_sync(ds.sync, output);
      break;
   }
   case Format::LDSDIR: {
      const LDSDIR_instruction& ldsdir = instr->ldsdir();
      if (instr->opcode == aco_opcode::lds_param_load)
         fprintf(output, " attr%u.%c", ldsdir.attr, "xyzw"[ldsdir.attr_chan]);
      if (ldsdir.wait_vdst != 15)
         fprintf(output, " wait_vdst:%u", ldsdir.wait_vdst);
      if (ldsdir.wait_vsrc != 1)
         fprintf(output, " wait_vsrc:%u", ldsdir.wait_vsrc);
      print_sync(ldsdir.sync, output);
      break;
   }
   case Format::MUBUF: {
      const MUBUF_instruction& mubuf = instr->mubuf();
      if (mubuf.offset)
         fprintf(output, " offset:%u", mubuf.offset);
      if (mubuf.offen)
         fprintf(output, " offen");
      if (mubuf.idxen)
         fprintf(output, " idxen");
      if (mubuf.addr64)
         fprintf(output, " addr64");
      print_cache_flags(gfx_level, mubuf, output);
      if (mubuf.tfe)
         fprintf(output, " tfe");
      if (mubuf.lds)
         fprintf(output, " lds");
      if (mubuf.disable_wqm)
         fprintf(output, " disable_wqm");
      print_sync(mubuf.sync, output);
      break;
   }
   case Format::MIMG: {
      const MIMG_instruction& mimg = instr->mimg();
      unsigned identity_dmask = 0xf;
      if (!instr->definitions.empty()) {
         unsigned num_channels = instr->definitions[0].bytes() / (mimg.d16 ? 2 : 4);
         identity_dmask = (1 << num_channels) - 1;
      }
      if ((mimg.dmask & identity_dmask) != identity_dmask)
         fprintf(output, " dmask:%s%s%s%s", mimg.dmask & 0x1 ? "x" : "",
                 mimg.dmask & 0x2 ? "y" : "", mimg.dmask & 0x4 ? "z" : "",
                 mimg.dmask & 0x8 ? "w" : "");
      switch (mimg.dim) {
      case ac_image_1d: fprintf(output, " 1d"); break;
      case ac_image_2d: fprintf(output, " 2d"); break;
      case ac_image_3d: fprintf(output, " 3d"); break;
      case ac_image_cube: fprintf(output, " cube"); break;
      case ac_image_1darray: fprintf(output, " 1darray"); break;
      case ac_image_2darray: fprintf(output, " 2darray"); break;
      case ac_image_2dmsaa: fprintf(output, " 2dmsaa"); break;
      case ac_image_2darraymsaa: fprintf(output, " 2darraymsaa"); break;
      }
      if (mimg.unrm)
         fprintf(output, " unrm");
      print_cache_flags(gfx_level, mimg, output);
      if (mimg.tfe)
         fprintf(output, " tfe");
      if (mimg.da)
         fprintf(output, " da");
      if (mimg.lwe)
         fprintf(output, " lwe");
      if (mimg.r128)
         fprintf(output, " r128");
      if (mimg.a16)
         fprintf(output, " a16");
      if (mimg.d16)
         fprintf(output, " d16");
      if (mimg.disable_wqm)
         fprintf(output, " disable_wqm");
      print_sync(mimg.sync, output);
      break;
   }
   case Format::EXP: {
      const Export_instruction& exp = instr->exp();
      unsigned identity_mask = exp.compressed ? 0x5 : 0xf;
      if ((exp.enabled_mask & identity_mask) != identity_mask)
         fprintf(output, " en:%c%c%c%c", exp.enabled_mask & 0x1 ? 'r' : '*',
                 exp.enabled_mask & 0x2 ? 'g' : '*', exp.enabled_mask & 0x4 ? 'b' : '*',
                 exp.enabled_mask & 0x8 ? 'a' : '*');
      if (exp.compressed)
         fprintf(output, " compr");
      if (exp.done)
         fprintf(output, " done");
      if (exp.valid_mask)
         fprintf(output, " vm");

      if (exp.dest <= V_008DFC_SQ_EXP_MRT + 7)
         fprintf(output, " mrt%d", exp.dest - V_008DFC_SQ_EXP_MRT);
      else if (exp.dest == V_008DFC_SQ_EXP_MRTZ)
         fprintf(output, " mrtz");
      else if (exp.dest == V_008DFC_SQ_EXP_NULL)
         fprintf(output, " null");
      else if (exp.dest >= V_008DFC_SQ_EXP_POS && exp.dest <= V_008DFC_SQ_EXP_POS + 3)
         fprintf(output, " pos%d", exp.dest - V_008DFC_SQ_EXP_POS);
      else if (exp.dest >= V_008DFC_SQ_EXP_PARAM && exp.dest <= V_008DFC_SQ_EXP_PARAM + 31)
         fprintf(output, " param%d", exp.dest - V_008DFC_SQ_EXP_PARAM);
      break;
   }
   case Format::PSEUDO_BRANCH: {
      const Pseudo_branch_instruction& branch = instr->branch();
      /* Note: BB0 cannot be a branch target */
      if (branch.target[0] != 0)
         fprintf(output, " BB%d", branch.target[0]);
      if (branch.target[1] != 0)
         fprintf(output, ", BB%d", branch.target[1]);
      if (branch.rarely_taken)
         fprintf(output, " rarely_taken");
      if (branch.never_taken)
         fprintf(output, " never_taken");
      break;
   }
   case Format::PSEUDO_REDUCTION: {
      const Pseudo_reduction_instruction& reduce = instr->reduction();
      fprintf(output, " op:%s", reduce_ops[reduce.reduce_op]);
      if (reduce.cluster_size)
         fprintf(output, " cluster_size:%u", reduce.cluster_size);
      break;
   }
   case Format::PSEUDO_BARRIER: {
      const Pseudo_barrier_instruction& barrier = instr->barrier();
      print_sync(barrier.sync, output);
      print_scope(barrier.exec_scope, output, "exec_scope");
      break;
   }
   case Format::FLAT:
   case Format::GLOBAL:
   case Format::SCRATCH: {
      const FLAT_instruction& flat = instr->flatlike();
      if (flat.offset)
         fprintf(output, " offset:%d", flat.offset);
      print_cache_flags(gfx_level, flat, output);
      if (flat.lds)
         fprintf(output, " lds");
      if (flat.nv)
         fprintf(output, " nv");
      if (flat.disable_wqm)
         fprintf(output, " disable_wqm");
      if (flat.may_use_lds)
         fprintf(output, " may_use_lds");
      print_sync(flat.sync, output);
      break;
   }
   case Format::MTBUF: {
      const MTBUF_instruction& mtbuf = instr->mtbuf();
      fprintf(output, " dfmt:");
      switch (mtbuf.dfmt) {
      case V_008F0C_BUF_DATA_FORMAT_8: fprintf(output, "8"); break;
      case V_008F0C_BUF_DATA_FORMAT_16: fprintf(output, "16"); break;
      case V_008F0C_BUF_DATA_FORMAT_8_8: fprintf(output, "8_8"); break;
      case V_008F0C_BUF_DATA_FORMAT_32: fprintf(output, "32"); break;
      case V_008F0C_BUF_DATA_FORMAT_16_16: fprintf(output, "16_16"); break;
      case V_008F0C_BUF_DATA_FORMAT_10_11_11: fprintf(output, "10_11_11"); break;
      case V_008F0C_BUF_DATA_FORMAT_11_11_10: fprintf(output, "11_11_10"); break;
      case V_008F0C_BUF_DATA_FORMAT_10_10_10_2: fprintf(output, "10_10_10_2"); break;
      case V_008F0C_BUF_DATA_FORMAT_2_10_10_10: fprintf(output, "2_10_10_10"); break;
      case V_008F0C_BUF_DATA_FORMAT_8_8_8_8: fprintf(output, "8_8_8_8"); break;
      case V_008F0C_BUF_DATA_FORMAT_32_32: fprintf(output, "32_32"); break;
      case V_008F0C_BUF_DATA_FORMAT_16_16_16_16: fprintf(output, "16_16_16_16"); break;
      case V_008F0C_BUF_DATA_FORMAT_32_32_32: fprintf(output, "32_32_32"); break;
      case V_008F0C_BUF_DATA_FORMAT_32_32_32_32: fprintf(output, "32_32_32_32"); break;
      case V_008F0C_BUF_DATA_FORMAT_RESERVED_15: fprintf(output, "reserved15"); break;
      }
      fprintf(output, " nfmt:");
      switch (mtbuf.nfmt) {
      case V_008F0C_BUF_NUM_FORMAT_UNORM: fprintf(output, "unorm"); break;
      case V_008F0C_BUF_NUM_FORMAT_SNORM: fprintf(output, "snorm"); break;
      case V_008F0C_BUF_NUM_FORMAT_USCALED: fprintf(output, "uscaled"); break;
      case V_008F0C_BUF_NUM_FORMAT_SSCALED: fprintf(output, "sscaled"); break;
      case V_008F0C_BUF_NUM_FORMAT_UINT: fprintf(output, "uint"); break;
      case V_008F0C_BUF_NUM_FORMAT_SINT: fprintf(output, "sint"); break;
      case V_008F0C_BUF_NUM_FORMAT_SNORM_OGL: fprintf(output, "snorm"); break;
      case V_008F0C_BUF_NUM_FORMAT_FLOAT: fprintf(output, "float"); break;
      }
      if (mtbuf.offset)
         fprintf(output, " offset:%u", mtbuf.offset);
      if (mtbuf.offen)
         fprintf(output, " offen");
      if (mtbuf.idxen)
         fprintf(output, " idxen");
      print_cache_flags(gfx_level, mtbuf, output);
      if (mtbuf.tfe)
         fprintf(output, " tfe");
      if (mtbuf.disable_wqm)
         fprintf(output, " disable_wqm");
      print_sync(mtbuf.sync, output);
      break;
   }
   default: {
      break;
   }
   }
   if (instr->isVALU()) {
      const VALU_instruction& valu = instr->valu();
      switch (valu.omod) {
      case 1: fprintf(output, " *2"); break;
      case 2: fprintf(output, " *4"); break;
      case 3: fprintf(output, " *0.5"); break;
      }
      if (valu.clamp)
         fprintf(output, " clamp");
      if (valu.opsel & (1 << 3))
         fprintf(output, " opsel_hi");
   }

   bool bound_ctrl = false, fetch_inactive = false;

   if (instr->opcode == aco_opcode::v_permlane16_b32 ||
       instr->opcode == aco_opcode::v_permlanex16_b32) {
      fetch_inactive = instr->valu().opsel[0];
      bound_ctrl = instr->valu().opsel[1];
   } else if (instr->isDPP16()) {
      const DPP16_instruction& dpp = instr->dpp16();
      if (dpp.dpp_ctrl <= 0xff) {
         fprintf(output, " quad_perm:[%d,%d,%d,%d]", dpp.dpp_ctrl & 0x3, (dpp.dpp_ctrl >> 2) & 0x3,
                 (dpp.dpp_ctrl >> 4) & 0x3, (dpp.dpp_ctrl >> 6) & 0x3);
      } else if (dpp.dpp_ctrl >= 0x101 && dpp.dpp_ctrl <= 0x10f) {
         fprintf(output, " row_shl:%d", dpp.dpp_ctrl & 0xf);
      } else if (dpp.dpp_ctrl >= 0x111 && dpp.dpp_ctrl <= 0x11f) {
         fprintf(output, " row_shr:%d", dpp.dpp_ctrl & 0xf);
      } else if (dpp.dpp_ctrl >= 0x121 && dpp.dpp_ctrl <= 0x12f) {
         fprintf(output, " row_ror:%d", dpp.dpp_ctrl & 0xf);
      } else if (dpp.dpp_ctrl == dpp_wf_sl1) {
         fprintf(output, " wave_shl:1");
      } else if (dpp.dpp_ctrl == dpp_wf_rl1) {
         fprintf(output, " wave_rol:1");
      } else if (dpp.dpp_ctrl == dpp_wf_sr1) {
         fprintf(output, " wave_shr:1");
      } else if (dpp.dpp_ctrl == dpp_wf_rr1) {
         fprintf(output, " wave_ror:1");
      } else if (dpp.dpp_ctrl == dpp_row_mirror) {
         fprintf(output, " row_mirror");
      } else if (dpp.dpp_ctrl == dpp_row_half_mirror) {
         fprintf(output, " row_half_mirror");
      } else if (dpp.dpp_ctrl == dpp_row_bcast15) {
         fprintf(output, " row_bcast:15");
      } else if (dpp.dpp_ctrl == dpp_row_bcast31) {
         fprintf(output, " row_bcast:31");
      } else if (dpp.dpp_ctrl >= dpp_row_share(0) && dpp.dpp_ctrl <= dpp_row_share(15)) {
         fprintf(output, " row_share:%d", dpp.dpp_ctrl & 0xf);
      } else if (dpp.dpp_ctrl >= dpp_row_xmask(0) && dpp.dpp_ctrl <= dpp_row_xmask(15)) {
         fprintf(output, " row_xmask:%d", dpp.dpp_ctrl & 0xf);
      } else {
         fprintf(output, " dpp_ctrl:0x%.3x", dpp.dpp_ctrl);
      }
      if (dpp.row_mask != 0xf)
         fprintf(output, " row_mask:0x%.1x", dpp.row_mask);
      if (dpp.bank_mask != 0xf)
         fprintf(output, " bank_mask:0x%.1x", dpp.bank_mask);
      bound_ctrl = dpp.bound_ctrl;
      fetch_inactive = dpp.fetch_inactive;
   } else if (instr->isDPP8()) {
      const DPP8_instruction& dpp = instr->dpp8();
      fprintf(output, " dpp8:[");
      for (unsigned i = 0; i < 8; i++)
         fprintf(output, "%s%u", i ? "," : "", (dpp.lane_sel >> (i * 3)) & 0x7);
      fprintf(output, "]");
      fetch_inactive = dpp.fetch_inactive;
   } else if (instr->isSDWA()) {
      const SDWA_instruction& sdwa = instr->sdwa();
      if (!instr->isVOPC()) {
         char sext = sdwa.dst_sel.sign_extend() ? 's' : 'u';
         unsigned offset = sdwa.dst_sel.offset();
         if (instr->definitions[0].isFixed())
            offset += instr->definitions[0].physReg().byte();
         switch (sdwa.dst_sel.size()) {
         case 1: fprintf(output, " dst_sel:%cbyte%u", sext, offset); break;
         case 2: fprintf(output, " dst_sel:%cword%u", sext, offset >> 1); break;
         case 4: fprintf(output, " dst_sel:dword"); break;
         default: break;
         }
         if (instr->definitions[0].bytes() < 4)
            fprintf(output, " dst_preserve");
      }
      for (unsigned i = 0; i < std::min<unsigned>(2, instr->operands.size()); i++) {
         char sext = sdwa.sel[i].sign_extend() ? 's' : 'u';
         unsigned offset = sdwa.sel[i].offset();
         if (instr->operands[i].isFixed())
            offset += instr->operands[i].physReg().byte();
         switch (sdwa.sel[i].size()) {
         case 1: fprintf(output, " src%d_sel:%cbyte%u", i, sext, offset); break;
         case 2: fprintf(output, " src%d_sel:%cword%u", i, sext, offset >> 1); break;
         case 4: fprintf(output, " src%d_sel:dword", i); break;
         default: break;
         }
      }
   }

   if (bound_ctrl)
      fprintf(output, " bound_ctrl:1");
   if (fetch_inactive)
      fprintf(output, " fi");
}

void
print_vopd_instr(enum amd_gfx_level gfx_level, const Instruction* instr, FILE* output,
                 unsigned flags)
{
   unsigned opy_start = get_vopd_opy_start(instr);

   if (!instr->definitions.empty()) {
      print_definition(&instr->definitions[0], output, flags);
      fprintf(output, " = ");
   }
   fprintf(output, "%s", instr_info.name[(int)instr->opcode]);
   for (unsigned i = 0; i < MIN2(instr->operands.size(), opy_start); ++i) {
      fprintf(output, i ? ", " : " ");
      aco_print_operand(&instr->operands[i], output, flags);
   }

   fprintf(output, " ::");

   if (instr->definitions.size() > 1) {
      print_definition(&instr->definitions[1], output, flags);
      fprintf(output, " = ");
   }
   fprintf(output, "%s", instr_info.name[(int)instr->vopd().opy]);
   for (unsigned i = opy_start; i < instr->operands.size(); ++i) {
      fprintf(output, i > opy_start ? ", " : " ");
      aco_print_operand(&instr->operands[i], output, flags);
   }
}

static void
print_block_kind(uint16_t kind, FILE* output)
{
   if (kind & block_kind_uniform)
      fprintf(output, "uniform, ");
   if (kind & block_kind_top_level)
      fprintf(output, "top-level, ");
   if (kind & block_kind_loop_preheader)
      fprintf(output, "loop-preheader, ");
   if (kind & block_kind_loop_header)
      fprintf(output, "loop-header, ");
   if (kind & block_kind_loop_exit)
      fprintf(output, "loop-exit, ");
   if (kind & block_kind_continue)
      fprintf(output, "continue, ");
   if (kind & block_kind_break)
      fprintf(output, "break, ");
   if (kind & block_kind_branch)
      fprintf(output, "branch, ");
   if (kind & block_kind_merge)
      fprintf(output, "merge, ");
   if (kind & block_kind_invert)
      fprintf(output, "invert, ");
   if (kind & block_kind_discard_early_exit)
      fprintf(output, "discard_early_exit, ");
   if (kind & block_kind_uses_discard)
      fprintf(output, "discard, ");
   if (kind & block_kind_resume)
      fprintf(output, "resume, ");
   if (kind & block_kind_export_end)
      fprintf(output, "export_end, ");
   if (kind & block_kind_end_with_regs)
      fprintf(output, "end_with_regs, ");
}

static void
print_stage(Stage stage, FILE* output)
{
   fprintf(output, "ACO shader stage: SW (");

   u_foreach_bit (s, (uint32_t)stage.sw) {
      switch ((SWStage)(1 << s)) {
      case SWStage::VS: fprintf(output, "VS"); break;
      case SWStage::GS: fprintf(output, "GS"); break;
      case SWStage::TCS: fprintf(output, "TCS"); break;
      case SWStage::TES: fprintf(output, "TES"); break;
      case SWStage::FS: fprintf(output, "FS"); break;
      case SWStage::CS: fprintf(output, "CS"); break;
      case SWStage::TS: fprintf(output, "TS"); break;
      case SWStage::MS: fprintf(output, "MS"); break;
      case SWStage::RT: fprintf(output, "RT"); break;
      default: UNREACHABLE("invalid SW stage");
      }
      if (stage.num_sw_stages() > 1)
         fprintf(output, "+");
   }

   fprintf(output, "), HW (");

   switch (stage.hw) {
   case AC_HW_LOCAL_SHADER: fprintf(output, "LOCAL_SHADER"); break;
   case AC_HW_HULL_SHADER: fprintf(output, "HULL_SHADER"); break;
   case AC_HW_EXPORT_SHADER: fprintf(output, "EXPORT_SHADER"); break;
   case AC_HW_LEGACY_GEOMETRY_SHADER: fprintf(output, "LEGACY_GEOMETRY_SHADER"); break;
   case AC_HW_VERTEX_SHADER: fprintf(output, "VERTEX_SHADER"); break;
   case AC_HW_NEXT_GEN_GEOMETRY_SHADER: fprintf(output, "NEXT_GEN_GEOMETRY_SHADER"); break;
   case AC_HW_PIXEL_SHADER: fprintf(output, "PIXEL_SHADER"); break;
   case AC_HW_COMPUTE_SHADER: fprintf(output, "COMPUTE_SHADER"); break;
   default: UNREACHABLE("invalid HW stage");
   }

   fprintf(output, ")\n");
}

void
print_debug_info(const Program* program, const Instruction* instr, FILE* output)
{
   fprintf(output, "// ");

   assert(instr->operands[0].isConstant());
   const auto& info = program->debug_info[instr->operands[0].constantValue()];
   switch (info.type) {
   case ac_shader_debug_info_src_loc:
      if (info.src_loc.spirv_offset)
         fprintf(output, "0x%x ", info.src_loc.spirv_offset);
      fprintf(output, "%s:%u:%u", info.src_loc.file, info.src_loc.line, info.src_loc.column);
      break;
   }

   fprintf(output, "\n");
}

void
aco_print_block(enum amd_gfx_level gfx_level, const Block* block, FILE* output, unsigned flags,
                const Program* program)
{
   if (block->instructions.empty() && block->linear_preds.empty())
      return;

   fprintf(output, "BB%d\n", block->index);
   fprintf(output, "/* logical preds: ");
   for (unsigned pred : block->logical_preds)
      fprintf(output, "BB%d, ", pred);
   fprintf(output, "/ linear preds: ");
   for (unsigned pred : block->linear_preds)
      fprintf(output, "BB%d, ", pred);
   fprintf(output, "/ kind: ");
   print_block_kind(block->kind, output);
   fprintf(output, "*/\n");

   if (flags & print_live_vars) {
      fprintf(output, "\tlive in:");
      for (unsigned id : program->live.live_in[block->index])
         fprintf(output, " %%%d", id);
      fprintf(output, "\n");

      RegisterDemand demand = block->register_demand;
      fprintf(output, "\tdemand: %u vgpr, %u sgpr\n", demand.vgpr, demand.sgpr);
   }

   for (auto const& instr : block->instructions) {
      fprintf(output, "\t");
      if (instr->opcode == aco_opcode::p_debug_info) {
         print_debug_info(program, instr.get(), output);
         continue;
      }
      if (flags & print_live_vars) {
         RegisterDemand demand = instr->register_demand;
         fprintf(output, "(%3u vgpr, %3u sgpr)   ", demand.vgpr, demand.sgpr);
      }
      if (flags & print_perf_info)
         fprintf(output, "(%3u clk)   ", instr->pass_flags);

      aco_print_instr(gfx_level, instr.get(), output, flags);
      fprintf(output, "\n");
   }
}

} /* end namespace */

void
aco_print_operand(const Operand* operand, FILE* output, unsigned flags)
{
   if (operand->isLiteral() || (operand->isConstant() && operand->bytes() == 1)) {
      if (operand->bytes() == 1)
         fprintf(output, "0x%.2x", operand->constantValue());
      else if (operand->bytes() == 2)
         fprintf(output, "0x%.4x", operand->constantValue());
      else
         fprintf(output, "0x%x", operand->constantValue());
   } else if (operand->isConstant()) {
      print_constant(operand->physReg().reg(), output);
   } else if (operand->isUndefined()) {
      print_reg_class(operand->regClass(), output);
      fprintf(output, "undef");
   } else {
      if (operand->is16bit())
         fprintf(output, "(is16bit)");
      if (operand->is24bit())
         fprintf(output, "(is24bit)");
      if ((flags & print_kill) && operand->isKill()) {
         if (operand->isLateKill())
            fprintf(output, "(lateKill)");
         else
            fprintf(output, "(kill)");
      }

      if (!(flags & print_no_ssa))
         fprintf(output, "%%%d%s", operand->tempId(), operand->isFixed() ? ":" : "");

      if (operand->isFixed())
         print_physReg(operand->physReg(), operand->bytes(), output, flags);
   }
}

void
aco_print_instr(enum amd_gfx_level gfx_level, const Instruction* instr, FILE* output,
                unsigned flags)
{
   if (instr->isVOPD()) {
      print_vopd_instr(gfx_level, instr, output, flags);
      return;
   }

   if (!instr->definitions.empty()) {
      for (unsigned i = 0; i < instr->definitions.size(); ++i) {
         print_definition(&instr->definitions[i], output, flags);
         if (i + 1 != instr->definitions.size())
            fprintf(output, ", ");
      }
      fprintf(output, " = ");
   }
   fprintf(output, "%s", instr_info.name[(int)instr->opcode]);
   if (instr->operands.size()) {
      const unsigned num_operands = instr->operands.size();
      bitarray8 abs = 0;
      bitarray8 neg = 0;
      bitarray8 neg_lo = 0;
      bitarray8 neg_hi = 0;
      bitarray8 opsel = 0;
      bitarray8 f2f32 = 0;
      bitarray8 opsel_lo = 0;
      bitarray8 opsel_hi = -1;

      if (instr->opcode == aco_opcode::v_fma_mix_f32 ||
          instr->opcode == aco_opcode::v_fma_mixlo_f16 ||
          instr->opcode == aco_opcode::v_fma_mixhi_f16) {
         const VALU_instruction& vop3p = instr->valu();
         abs = vop3p.abs;
         neg = vop3p.neg;
         f2f32 = vop3p.opsel_hi;
         opsel = f2f32 & vop3p.opsel_lo;
      } else if (instr->isVOP3P()) {
         const VALU_instruction& vop3p = instr->valu();
         neg = vop3p.neg_lo & vop3p.neg_hi;
         neg_lo = vop3p.neg_lo & ~neg;
         neg_hi = vop3p.neg_hi & ~neg;
         opsel_lo = vop3p.opsel_lo;
         opsel_hi = vop3p.opsel_hi;
      } else if (instr->isVALU() && instr->opcode != aco_opcode::v_permlane16_b32 &&
                 instr->opcode != aco_opcode::v_permlanex16_b32) {
         const VALU_instruction& valu = instr->valu();
         abs = valu.abs;
         neg = valu.neg;
         opsel = valu.opsel;
      }
      bool is_vector_op = false;
      for (unsigned i = 0; i < num_operands; ++i) {
         if (i)
            fprintf(output, ", ");
         else
            fprintf(output, " ");
         if (!is_vector_op && instr->operands[i].isVectorAligned())
            fprintf(output, "(");

         if (i < 3) {
            if (neg[i] && instr->operands[i].isConstant())
               fprintf(output, "neg(");
            else if (neg[i])
               fprintf(output, "-");
            if (abs[i])
               fprintf(output, "|");
            if (opsel[i])
               fprintf(output, "hi(");
            else if (f2f32[i])
               fprintf(output, "lo(");
         }

         aco_print_operand(&instr->operands[i], output, flags);

         if (i < 3) {
            if (f2f32[i] || opsel[i])
               fprintf(output, ")");
            if (abs[i])
               fprintf(output, "|");

            if (opsel_lo[i] || !opsel_hi[i])
               fprintf(output, ".%c%c", opsel_lo[i] ? 'y' : 'x', opsel_hi[i] ? 'y' : 'x');

            if (neg[i] && instr->operands[i].isConstant())
               fprintf(output, ")");
            if (neg_lo[i])
               fprintf(output, "*[-1,1]");
            if (neg_hi[i])
               fprintf(output, "*[1,-1]");
         }

         if (is_vector_op && !instr->operands[i].isVectorAligned())
            fprintf(output, ")");
         is_vector_op = instr->operands[i].isVectorAligned();
      }
   }
   print_instr_format_specific(gfx_level, instr, output);
}

void
aco_print_program(const Program* program, FILE* output, unsigned flags)
{
   switch (program->progress) {
   case CompilationProgress::after_isel: fprintf(output, "After Instruction Selection:\n"); break;
   case CompilationProgress::after_spilling:
      fprintf(output, "After Spilling:\n");
      flags |= print_kill;
      break;
   case CompilationProgress::after_ra: fprintf(output, "After RA:\n"); break;
   case CompilationProgress::after_lower_to_hw:
      fprintf(output, "After lowering to hw instructions:\n");
      break;
   }

   print_stage(program->stage, output);

   for (Block const& block : program->blocks)
      aco_print_block(program->gfx_level, &block, output, flags, program);

   if (program->constant_data.size()) {
      fprintf(output, "\n/* constant data */\n");
      for (unsigned i = 0; i < program->constant_data.size(); i += 32) {
         fprintf(output, "[%06d] ", i);
         unsigned line_size = std::min<size_t>(program->constant_data.size() - i, 32);
         for (unsigned j = 0; j < line_size; j += 4) {
            unsigned size = std::min<size_t>(program->constant_data.size() - (i + j), 4);
            uint32_t v = 0;
            memcpy(&v, &program->constant_data[i + j], size);
            fprintf(output, " %08x", v);
         }
         fprintf(output, "\n");
      }
   }

   fprintf(output, "\n");
}

} // namespace aco
