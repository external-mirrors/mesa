/*
 * Copyright © 2021 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <vector>

namespace aco {
namespace {

constexpr const size_t max_reg_cnt = 512;
constexpr const size_t max_sgpr_cnt = 128;
constexpr const size_t min_vgpr = 256;
constexpr const size_t max_vgpr_cnt = 256;

struct Idx {
   bool operator==(const Idx& other) const { return block == other.block && instr == other.instr; }
   bool operator!=(const Idx& other) const { return !operator==(other); }

   bool found() const { return block != UINT32_MAX; }

   uint32_t block;
   uint32_t instr;
};

/** Indicates that a register was not yet written in the shader. */
Idx not_written_yet{UINT32_MAX, 0};

/** Indicates that an operand is constant or undefined, not written by any instruction. */
Idx const_or_undef{UINT32_MAX, 2};

/** Indicates that a register was overwritten by different instructions in previous blocks. */
Idx overwritten_untrackable{UINT32_MAX, 3};

/** Indicates that there isn't a clear single writer, for example due to subdword operations. */
Idx overwritten_unknown_instr{UINT32_MAX, 4};

struct pr_opt_ctx {
   using Idx_array = std::array<Idx, max_reg_cnt>;

   Program* program;
   Block* current_block;
   uint32_t current_instr_idx;
   std::vector<uint16_t> uses;
   std::unique_ptr<Idx_array[]> instr_idx_by_regs;

   pr_opt_ctx(Program* p)
       : program(p), current_block(nullptr), current_instr_idx(0), uses(dead_code_analysis(p)),
         instr_idx_by_regs(std::unique_ptr<Idx_array[]>{new Idx_array[p->blocks.size()]})
   {}

   ALWAYS_INLINE void reset_block_regs(const Block::edge_vec& preds, const unsigned block_index,
                                       const unsigned min_reg, const unsigned num_regs)
   {
      const unsigned num_preds = preds.size();
      const unsigned first_pred = preds[0];

      /* Copy information from the first predecessor. */
      memcpy(&instr_idx_by_regs[block_index][min_reg], &instr_idx_by_regs[first_pred][min_reg],
             num_regs * sizeof(Idx));

      /* Mark overwritten if it doesn't match with other predecessors. */
      const unsigned until_reg = min_reg + num_regs;
      for (unsigned i = 1; i < num_preds; ++i) {
         unsigned pred = preds[i];
         for (unsigned reg = min_reg; reg < until_reg; ++reg) {
            Idx& idx = instr_idx_by_regs[block_index][reg];
            if (idx == overwritten_untrackable)
               continue;

            if (idx != instr_idx_by_regs[pred][reg])
               idx = overwritten_untrackable;
         }
      }
   }

   void reset_block(Block* block)
   {
      current_block = block;
      current_instr_idx = 0;

      if (block->linear_preds.empty()) {
         std::fill(instr_idx_by_regs[block->index].begin(), instr_idx_by_regs[block->index].end(),
                   not_written_yet);
      } else if (block->kind & block_kind_loop_header) {
         /* Instructions inside the loop may overwrite registers of temporaries that are
          * not live inside the loop, but we can't detect that because we haven't processed
          * the blocks in the loop yet. As a workaround, mark all registers as untrackable.
          * TODO: Consider improving this in the future.
          */
         std::fill(instr_idx_by_regs[block->index].begin(), instr_idx_by_regs[block->index].end(),
                   overwritten_untrackable);
      } else {
         reset_block_regs(block->linear_preds, block->index, 0, max_sgpr_cnt);
         reset_block_regs(block->linear_preds, block->index, 251, 3);

         if (!block->logical_preds.empty()) {
            /* We assume that VGPRs are only read by blocks which have a logical predecessor,
             * ie. any block that reads any VGPR has at least 1 logical predecessor.
             */
            reset_block_regs(block->logical_preds, block->index, min_vgpr, max_vgpr_cnt);
         } else {
            /* If a block has no logical predecessors, it is not part of the
             * logical CFG and therefore it also won't have any logical successors.
             * Such a block does not write any VGPRs ever.
             */
            assert(block->logical_succs.empty());
         }
      }
   }

   Instruction* get(Idx idx) { return program->blocks[idx.block].instructions[idx.instr].get(); }
};

void
save_reg_writes(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   for (const Definition& def : instr->definitions) {
      assert(def.regClass().type() != RegType::sgpr || def.physReg().reg() <= 255);
      assert(def.regClass().type() != RegType::vgpr || def.physReg().reg() >= 256);

      unsigned dw_size = DIV_ROUND_UP(def.bytes(), 4u);
      unsigned r = def.physReg().reg();
      Idx idx{ctx.current_block->index, ctx.current_instr_idx};

      if (def.regClass().is_subdword())
         idx = overwritten_unknown_instr;

      assert((r + dw_size) <= max_reg_cnt);
      assert(def.size() == dw_size || def.regClass().is_subdword());
      std::fill(ctx.instr_idx_by_regs[ctx.current_block->index].begin() + r,
                ctx.instr_idx_by_regs[ctx.current_block->index].begin() + r + dw_size, idx);
   }
   if (instr->isPseudo() && instr->pseudo().needs_scratch_reg) {
      ctx.instr_idx_by_regs[ctx.current_block->index][instr->pseudo().scratch_sgpr] =
         overwritten_unknown_instr;
   }
}

Idx
last_writer_idx(pr_opt_ctx& ctx, PhysReg physReg, RegClass rc)
{
   /* Verify that all of the operand's registers are written by the same instruction. */
   assert(physReg.reg() < max_reg_cnt);
   Idx instr_idx = ctx.instr_idx_by_regs[ctx.current_block->index][physReg.reg()];
   unsigned dw_size = DIV_ROUND_UP(rc.bytes(), 4u);
   unsigned r = physReg.reg();
   bool all_same =
      std::all_of(ctx.instr_idx_by_regs[ctx.current_block->index].begin() + r,
                  ctx.instr_idx_by_regs[ctx.current_block->index].begin() + r + dw_size,
                  [instr_idx](Idx i) { return i == instr_idx; });

   return all_same ? instr_idx : overwritten_untrackable;
}

Idx
last_writer_idx(pr_opt_ctx& ctx, const Operand& op)
{
   if (op.isConstant() || op.isUndefined())
      return const_or_undef;

   return last_writer_idx(ctx, op.physReg(), op.regClass());
}

/**
 * Check whether a register has been overwritten since the given location.
 * This is an important part of checking whether certain optimizations are
 * valid.
 * Note that the decision is made based on registers and not on SSA IDs.
 */
bool
is_overwritten_since(pr_opt_ctx& ctx, PhysReg reg, RegClass rc, const Idx& since_idx,
                     bool inclusive = false)
{
   /* If we didn't find an instruction, assume that the register is overwritten. */
   if (!since_idx.found())
      return true;

   /* TODO: We currently can't keep track of subdword registers. */
   if (rc.is_subdword())
      return true;

   unsigned begin_reg = reg.reg();
   unsigned end_reg = begin_reg + rc.size();
   unsigned current_block_idx = ctx.current_block->index;

   for (unsigned r = begin_reg; r < end_reg; ++r) {
      Idx& i = ctx.instr_idx_by_regs[current_block_idx][r];
      if (i == overwritten_untrackable && current_block_idx > since_idx.block)
         return true;
      else if (i == overwritten_untrackable || i == not_written_yet)
         continue;
      else if (i == overwritten_unknown_instr)
         return true;

      assert(i.found());

      bool since_instr = inclusive ? i.instr >= since_idx.instr : i.instr > since_idx.instr;
      if (i.block > since_idx.block || (i.block == since_idx.block && since_instr))
         return true;
   }

   return false;
}

bool
is_overwritten_since(pr_opt_ctx& ctx, const Definition& def, const Idx& idx, bool inclusive = false)
{
   return is_overwritten_since(ctx, def.physReg(), def.regClass(), idx, inclusive);
}

bool
is_overwritten_since(pr_opt_ctx& ctx, const Operand& op, const Idx& idx, bool inclusive = false)
{
   if (op.isConstant())
      return false;

   return is_overwritten_since(ctx, op.physReg(), op.regClass(), idx, inclusive);
}

void
try_apply_branch_vcc(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* We are looking for the following pattern:
    *
    * vcc = ...                      ; last_vcc_wr
    * sX, scc = s_and_bXX vcc, exec  ; op0_instr
    * (...vcc and exec must not be overwritten inbetween...)
    * s_cbranch_XX scc               ; instr
    *
    * If possible, the above is optimized into:
    *
    * vcc = ...                      ; last_vcc_wr
    * s_cbranch_XX vcc               ; instr modified to use vcc
    */

   /* Don't try to optimize this on GFX6-7 because SMEM may corrupt the vccz bit. */
   if (ctx.program->gfx_level < GFX8)
      return;

   if (instr->format != Format::PSEUDO_BRANCH || instr->operands.size() == 0 ||
       instr->operands[0].physReg() != scc)
      return;

   Idx op0_instr_idx = last_writer_idx(ctx, instr->operands[0]);
   Idx last_vcc_wr_idx = last_writer_idx(ctx, vcc, ctx.program->lane_mask);

   /* We need to make sure:
    * - the instructions that wrote the operand register and VCC are both found
    * - the operand register used by the branch, and VCC were both written in the current block
    * - EXEC hasn't been overwritten since the last VCC write
    * - VCC hasn't been overwritten since the operand register was written
    *   (ie. the last VCC writer precedes the op0 writer)
    */
   if (!op0_instr_idx.found() || !last_vcc_wr_idx.found() ||
       op0_instr_idx.block != ctx.current_block->index ||
       last_vcc_wr_idx.block != ctx.current_block->index ||
       is_overwritten_since(ctx, exec, ctx.program->lane_mask, last_vcc_wr_idx) ||
       is_overwritten_since(ctx, vcc, ctx.program->lane_mask, op0_instr_idx))
      return;

   Instruction* op0_instr = ctx.get(op0_instr_idx);
   Instruction* last_vcc_wr = ctx.get(last_vcc_wr_idx);

   if ((op0_instr->opcode != aco_opcode::s_and_b64 /* wave64 */ &&
        op0_instr->opcode != aco_opcode::s_and_b32 /* wave32 */) ||
       op0_instr->operands[0].physReg() != vcc || op0_instr->operands[1].physReg() != exec ||
       !last_vcc_wr->isVOPC())
      return;

   assert(last_vcc_wr->definitions[0].tempId() == op0_instr->operands[0].tempId());

   /* Reduce the uses of the SCC def */
   ctx.uses[instr->operands[0].tempId()]--;
   /* Use VCC instead of SCC in the branch */
   instr->operands[0] = op0_instr->operands[0];
}

void
try_optimize_to_scc_zero_cmp(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* We are looking for the following pattern:
    *
    * s_bfe_u32 s0, s3, 0x40018  ; outputs SGPR and SCC if the SGPR != 0
    * s_cmp_eq_i32 s0, 0         ; comparison between the SGPR and 0
    *
    * If possible, the above is optimized into:
    *
    * s_bfe_u32 s0, s3, 0x40018  ; original instruction
    * s_cmp_eq_i32 scc, 0         ; comparison between the scc and 0
    *
    * This can then be further optimized by try_optimize_scc_nocompare.
    *
    * Alternatively, if scc is overwritten between the first instruction and the comparison,
    * try to pull down the original instruction to replace the cmp entirely.
    */

   if (!instr->isSOPC() ||
       (instr->opcode != aco_opcode::s_cmp_eq_u32 && instr->opcode != aco_opcode::s_cmp_eq_i32 &&
        instr->opcode != aco_opcode::s_cmp_lg_u32 && instr->opcode != aco_opcode::s_cmp_lg_i32 &&
        instr->opcode != aco_opcode::s_cmp_eq_u64 && instr->opcode != aco_opcode::s_cmp_lg_u64) ||
       (!instr->operands[0].constantEquals(0) && !instr->operands[1].constantEquals(0)) ||
       (!instr->operands[0].isTemp() && !instr->operands[1].isTemp()))
      return;

   /* Make sure the constant is always in operand 1 */
   if (instr->operands[0].isConstant())
      std::swap(instr->operands[0], instr->operands[1]);

   /* Find the writer instruction of Operand 0. */
   Idx wr_idx = last_writer_idx(ctx, instr->operands[0]);
   if (!wr_idx.found())
      return;

   Instruction* wr_instr = ctx.get(wr_idx);
   if (!wr_instr->isSALU() || wr_instr->definitions.size() < 2 ||
       wr_instr->definitions[1].physReg() != scc)
      return;

   /* Look for instructions which set SCC := (D != 0) */
   switch (wr_instr->opcode) {
   case aco_opcode::s_bfe_i32:
   case aco_opcode::s_bfe_i64:
   case aco_opcode::s_bfe_u32:
   case aco_opcode::s_bfe_u64:
   case aco_opcode::s_and_b32:
   case aco_opcode::s_and_b64:
   case aco_opcode::s_andn2_b32:
   case aco_opcode::s_andn2_b64:
   case aco_opcode::s_or_b32:
   case aco_opcode::s_or_b64:
   case aco_opcode::s_orn2_b32:
   case aco_opcode::s_orn2_b64:
   case aco_opcode::s_xor_b32:
   case aco_opcode::s_xor_b64:
   case aco_opcode::s_not_b32:
   case aco_opcode::s_not_b64:
   case aco_opcode::s_nor_b32:
   case aco_opcode::s_nor_b64:
   case aco_opcode::s_xnor_b32:
   case aco_opcode::s_xnor_b64:
   case aco_opcode::s_nand_b32:
   case aco_opcode::s_nand_b64:
   case aco_opcode::s_lshl_b32:
   case aco_opcode::s_lshl_b64:
   case aco_opcode::s_lshr_b32:
   case aco_opcode::s_lshr_b64:
   case aco_opcode::s_ashr_i32:
   case aco_opcode::s_ashr_i64:
   case aco_opcode::s_abs_i32:
   case aco_opcode::s_absdiff_i32: break;
   default: return;
   }

   /* Check whether both SCC and Operand 0 are written by the same instruction. */
   Idx sccwr_idx = last_writer_idx(ctx, scc, s1);
   if (wr_idx != sccwr_idx) {
      /* Check whether the current instruction is the only user of its first operand. */
      if (ctx.uses[wr_instr->definitions[1].tempId()] ||
          ctx.uses[wr_instr->definitions[0].tempId()] > 1)
         return;

      /* Check whether the operands of the writer are overwritten. */
      for (const Operand& op : wr_instr->operands) {
         if (is_overwritten_since(ctx, op, wr_idx))
            return;
      }

      aco_opcode pulled_opcode = wr_instr->opcode;
      if (instr->opcode == aco_opcode::s_cmp_eq_u32 || instr->opcode == aco_opcode::s_cmp_eq_i32 ||
          instr->opcode == aco_opcode::s_cmp_eq_u64) {
         /* When s_cmp_eq is used, it effectively inverts the SCC def.
          * However, we can't simply invert the opcodes here because that
          * would change the meaning of the program.
          */
         return;
      }

      Definition scc_def = instr->definitions[0];
      ctx.uses[wr_instr->definitions[0].tempId()]--;

      /* Copy the writer instruction, but use SCC from the current instr.
       * This means that the original instruction will be eliminated.
       */
      if (wr_instr->format == Format::SOP2) {
         instr.reset(create_instruction(pulled_opcode, Format::SOP2, 2, 2));
         instr->operands[1] = wr_instr->operands[1];
      } else if (wr_instr->format == Format::SOP1) {
         instr.reset(create_instruction(pulled_opcode, Format::SOP1, 1, 2));
      }
      instr->definitions[0] = wr_instr->definitions[0];
      instr->definitions[1] = scc_def;
      instr->operands[0] = wr_instr->operands[0];
      return;
   }

   /* Use the SCC def from wr_instr */
   ctx.uses[instr->operands[0].tempId()]--;
   instr->operands[0] = Operand(wr_instr->definitions[1].getTemp());
   instr->operands[0].setFixed(scc);
   ctx.uses[instr->operands[0].tempId()]++;

   /* Set the opcode and operand to 32-bit */
   instr->operands[1] = Operand::zero();
   instr->opcode =
      (instr->opcode == aco_opcode::s_cmp_eq_u32 || instr->opcode == aco_opcode::s_cmp_eq_i32 ||
       instr->opcode == aco_opcode::s_cmp_eq_u64)
         ? aco_opcode::s_cmp_eq_u32
         : aco_opcode::s_cmp_lg_u32;
}

void
try_optimize_scc_nocompare(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* If we have this pattern:
    * s_cmp_eq_i32 scc, 0 ; comparison between scc and 0
    * s_cbranch_scc0 BB3  ; use the result of the comparison, eg. branch or cselect
    *
    * Turn it into:
    * <>                  ; removed s_cmp
    * s_cbranch_scc1 BB3  ; inverted branch
    */

   int scc_op_idx = -1;
   for (unsigned i = 0; i < instr->operands.size(); i++) {
      if (instr->operands[i].isTemp() && instr->operands[i].physReg() == scc) {
         scc_op_idx = i;
         break;
      }
   }

   if (scc_op_idx < 0)
      return;

   Idx wr_idx = last_writer_idx(ctx, instr->operands[scc_op_idx]);
   if (!wr_idx.found())
      return;

   Instruction* wr_instr = ctx.get(wr_idx);

   /* Check if we found the pattern above. */
   if (wr_instr->opcode != aco_opcode::s_cmp_eq_u32 && wr_instr->opcode != aco_opcode::s_cmp_lg_u32)
      return;
   if (wr_instr->operands[0].physReg() != scc || !wr_instr->operands[0].isTemp())
      return;
   if (!wr_instr->operands[1].constantEquals(0))
      return;

   if (wr_instr->opcode == aco_opcode::s_cmp_eq_u32) {
      /* The optimization can be unsafe when there are other users. */
      if (ctx.uses[instr->operands[scc_op_idx].tempId()] > 1)
         return;

      /* Flip the meaning of the instruction to correctly use the SCC. */
      if (instr->format == Format::PSEUDO_BRANCH) {
         instr->opcode = instr->opcode == aco_opcode::p_cbranch_z ? aco_opcode::p_cbranch_nz
                                                                  : aco_opcode::p_cbranch_z;
      } else if (instr->opcode == aco_opcode::s_cselect_b32 ||
                 instr->opcode == aco_opcode::s_cselect_b64) {
         std::swap(instr->operands[0], instr->operands[1]);
      } else if (instr->opcode == aco_opcode::s_cmovk_i32 ||
                 instr->opcode == aco_opcode::s_mul_i32) {
         /* Convert to s_cselect_b32 and swap the operands. */
         Instruction* cselect = create_instruction(aco_opcode::s_cselect_b32, Format::SOP2, 3, 1);
         cselect->definitions[0] = instr->definitions[0];
         cselect->operands[2] = instr->operands[scc_op_idx];
         if (instr->opcode == aco_opcode::s_cmovk_i32) {
            cselect->operands[0] = instr->operands[0];
            cselect->operands[1] = Operand::c32((int32_t)(int16_t)instr->salu().imm);
         } else if (instr->opcode == aco_opcode::s_mul_i32) {
            cselect->operands[0] = Operand::c32(0);
            cselect->operands[1] = instr->operands[!scc_op_idx];
         } else {
            UNREACHABLE("invalid op");
         }
         scc_op_idx = 2;
         instr.reset(cselect);
      } else {
         return;
      }
   }

   /* Use the SCC def from the original instruction, not the comparison */
   ctx.uses[instr->operands[scc_op_idx].tempId()]--;
   if (ctx.uses[instr->operands[scc_op_idx].tempId()])
      ctx.uses[wr_instr->operands[0].tempId()]++;
   instr->operands[scc_op_idx] = wr_instr->operands[0];
}

static bool
is_scc_copy(const Instruction* instr)
{
   return instr->opcode == aco_opcode::p_parallelcopy && instr->operands.size() == 1 &&
          instr->operands[0].isTemp() && instr->operands[0].physReg().reg() == scc;
}

void
save_scc_copy_producer(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (!is_scc_copy(instr.get()))
      return;

   Idx wr_idx = last_writer_idx(ctx, instr->operands[0]);
   if (wr_idx.found() && wr_idx.block == ctx.current_block->index)
      instr->pass_flags = wr_idx.instr;
   else
      instr->pass_flags = UINT32_MAX;
}

void
try_eliminate_scc_copy(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* Try to eliminate an SCC copy by duplicating the instruction that produced the SCC. */

   if (instr->opcode != aco_opcode::p_parallelcopy || instr->definitions.size() != 1 ||
       instr->definitions[0].physReg().reg() != scc)
      return;

   /* Find the instruction that copied SCC into an SGPR. */
   Idx wr_idx = last_writer_idx(ctx, instr->operands[0]);
   if (!wr_idx.found())
      return;

   const Instruction* wr_instr = ctx.get(wr_idx);
   if (!is_scc_copy(wr_instr) || wr_instr->pass_flags == UINT32_MAX)
      return;

   Idx producer_idx = {wr_idx.block, wr_instr->pass_flags};
   Instruction* producer_instr = ctx.get(producer_idx);

   if (!producer_instr || !producer_instr->isSALU())
      return;

   /* Verify that the operands of the producer instruction haven't been overwritten. */
   for (const Operand& op : producer_instr->operands) {
      if (is_overwritten_since(ctx, op, producer_idx, true))
         return;
   }

   /* Verify that the definitions (except SCC) of the producer haven't been overwritten. */
   for (const Definition& def : producer_instr->definitions) {
      if (def.physReg().reg() == scc)
         continue;
      if (is_overwritten_since(ctx, def, producer_idx))
         return;
   }

   /* Duplicate the original producer of the SCC */
   Definition scc_def = instr->definitions[0];
   instr.reset(create_instruction(producer_instr->opcode, producer_instr->format,
                                  producer_instr->operands.size(),
                                  producer_instr->definitions.size()));
   instr->salu().imm = producer_instr->salu().imm;

   /* The copy is no longer needed. */
   if (--ctx.uses[wr_instr->definitions[0].tempId()] == 0)
      ctx.uses[wr_instr->operands[0].tempId()]--;

   /* Copy the operands of the original producer. */
   for (unsigned i = 0; i < producer_instr->operands.size(); ++i) {
      instr->operands[i] = producer_instr->operands[i];
      if (producer_instr->operands[i].isTemp() && !is_dead(ctx.uses, producer_instr))
         ctx.uses[producer_instr->operands[i].tempId()]++;
   }

   /* Copy the definitions of the original producer,
    * but mark them as non-temp to keep SSA quasi-intact.
    */
   for (unsigned i = 0; i < producer_instr->definitions.size(); ++i)
      instr->definitions[i] = Definition(producer_instr->definitions[i].physReg(),
                                         producer_instr->definitions[i].regClass());
   instr->definitions.back() = scc_def; /* Keep temporary ID. */
}

void
try_combine_dpp(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* We are looking for the following pattern:
    *
    * v_mov_dpp vA, vB, ...      ; move instruction with DPP
    * v_xxx vC, vA, ...          ; current instr that uses the result from the move
    *
    * If possible, the above is optimized into:
    *
    * v_xxx_dpp vC, vB, ...      ; current instr modified to use DPP directly
    *
    */

   if (!instr->isVALU() || instr->isDPP())
      return;

   for (unsigned i = 0; i < instr->operands.size(); i++) {
      Idx op_instr_idx = last_writer_idx(ctx, instr->operands[i]);
      if (!op_instr_idx.found())
         continue;

      /* is_overwritten_since only considers active lanes when the register could possibly
       * have been overwritten from inactive lanes. Restrict this optimization to at most
       * one block so that there is no possibility for clobbered inactive lanes.
       */
      if (ctx.current_block->index - op_instr_idx.block > 1)
         continue;

      const Instruction* mov = ctx.get(op_instr_idx);
      if (mov->opcode != aco_opcode::v_mov_b32 || !mov->isDPP())
         continue;

      /* If we aren't going to remove the v_mov_b32, we have to ensure that it doesn't overwrite
       * it's own operand before we use it.
       */
      if (mov->definitions[0].physReg() == mov->operands[0].physReg() &&
          (!mov->definitions[0].tempId() || ctx.uses[mov->definitions[0].tempId()] > 1))
         continue;

      /* Don't propagate DPP if the source register is overwritten since the move. */
      if (is_overwritten_since(ctx, mov->operands[0], op_instr_idx))
         continue;

      bool dpp8 = mov->isDPP8();

      /* Fetch-inactive means exec is ignored, which allows us to combine across exec changes. */
      if (!(dpp8 ? mov->dpp8().fetch_inactive : mov->dpp16().fetch_inactive) &&
          is_overwritten_since(ctx, Operand(exec, ctx.program->lane_mask), op_instr_idx))
         continue;

      /* We won't eliminate the DPP mov if the operand is used twice */
      bool op_used_twice = false;
      for (unsigned j = 0; j < instr->operands.size(); j++)
         op_used_twice |= i != j && instr->operands[i] == instr->operands[j];
      if (op_used_twice)
         continue;

      bool input_mods = can_use_input_modifiers(ctx.program->gfx_level, instr->opcode, i) &&
                        get_operand_type(instr, i).bit_size == 32;
      bool mov_uses_mods = mov->valu().neg[0] || mov->valu().abs[0];
      if (((dpp8 && ctx.program->gfx_level < GFX11) || !input_mods) && mov_uses_mods)
         continue;

      if (i != 0) {
         if (!can_swap_operands(instr, &instr->opcode, 0, i))
            continue;
         instr->valu().swapOperands(0, i);
      }

      if (!can_use_DPP(ctx.program->gfx_level, instr, dpp8))
         continue;

      if (!dpp8) /* anything else doesn't make sense in SSA */
         assert(mov->dpp16().row_mask == 0xf && mov->dpp16().bank_mask == 0xf);

      if (--ctx.uses[mov->definitions[0].tempId()])
         ctx.uses[mov->operands[0].tempId()]++;

      convert_to_DPP(ctx.program->gfx_level, instr, dpp8);

      instr->operands[0] = mov->operands[0];

      if (dpp8) {
         DPP8_instruction* dpp = &instr->dpp8();
         dpp->lane_sel = mov->dpp8().lane_sel;
         dpp->fetch_inactive = mov->dpp8().fetch_inactive;
         if (mov_uses_mods)
            instr->format = asVOP3(instr->format);
      } else {
         DPP16_instruction* dpp = &instr->dpp16();
         dpp->dpp_ctrl = mov->dpp16().dpp_ctrl;
         dpp->bound_ctrl = true;
         dpp->fetch_inactive = mov->dpp16().fetch_inactive;
      }
      instr->valu().neg[0] ^= mov->valu().neg[0] && !instr->valu().abs[0];
      instr->valu().abs[0] |= mov->valu().abs[0];
      return;
   }
}

unsigned
num_encoded_alu_operands(const aco_ptr<Instruction>& instr)
{
   if (instr->isSALU()) {
      if (instr->isSOP2() || instr->isSOPC())
         return 2;
      else if (instr->isSOP1())
         return 1;

      return 0;
   }

   if (instr->isVALU()) {
      if (instr->isVOP1())
         return 1;
      else if (instr->isVOPC() || instr->isVOP2())
         return 2;
      else if (instr->opcode == aco_opcode::v_writelane_b32_e64 ||
               instr->opcode == aco_opcode::v_writelane_b32)
         return 2; /* potentially VOP3, but reads VDST as SRC2 */
      else if (instr->isVOP3() || instr->isVOP3P() || instr->isVINTERP_INREG())
         return instr->operands.size();
   }

   return 0;
}

void
try_reassign_split_vector(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* Any unused split_vector definition can always use the same register
    * as the operand. This avoids creating unnecessary copies.
    */
   if (instr->opcode == aco_opcode::p_split_vector) {
      Operand& op = instr->operands[0];
      if (!op.isTemp() || op.isKill())
         return;

      PhysReg reg = op.physReg();
      for (Definition& def : instr->definitions) {
         if (def.getTemp().type() == op.getTemp().type() && def.isKill())
            def.setFixed(reg);

         reg = reg.advance(def.bytes());
      }

      return;
   }

   /* We are looking for the following pattern:
    *
    * sA, sB = p_split_vector s[X:Y]
    * ... X and Y not overwritten here ...
    * use sA or sB <--- current instruction
    *
    * If possible, we propagate the registers from the p_split_vector
    * operand into the current instruction and the above is optimized into:
    *
    * use sX or sY
    *
    * Thereby, we might violate register assignment rules.
    * This optimization exists because it's too difficult to solve it
    * in RA, and should be removed after we solved this in RA.
    */

   if (!instr->isVALU() && !instr->isSALU())
      return;

   for (unsigned i = 0; i < num_encoded_alu_operands(instr); i++) {
      /* Find the instruction that writes the current operand. */
      const Operand& op = instr->operands[i];
      Idx op_instr_idx = last_writer_idx(ctx, op);
      if (!op_instr_idx.found())
         continue;

      /* Check if the operand is written by p_split_vector. */
      Instruction* split_vec = ctx.get(op_instr_idx);
      if (split_vec->opcode != aco_opcode::p_split_vector &&
          split_vec->opcode != aco_opcode::p_extract_vector)
         continue;

      Operand& split_op = split_vec->operands[0];

      /* Don't do anything if the p_split_vector operand is not a temporary
       * or is killed by the p_split_vector.
       * In this case the definitions likely already reuse the same registers as the operand.
       */
      if (!split_op.isTemp() || split_op.isKill())
         continue;

      /* Only propagate operands of the same type */
      if (split_op.getTemp().type() != op.getTemp().type())
         continue;

      /* Check if the p_split_vector operand's registers are overwritten. */
      if (is_overwritten_since(ctx, split_op, op_instr_idx))
         continue;

      PhysReg reg = split_op.physReg();
      if (split_vec->opcode == aco_opcode::p_extract_vector) {
         reg =
            reg.advance(split_vec->definitions[0].bytes() * split_vec->operands[1].constantValue());
      }
      for (Definition& def : split_vec->definitions) {
         if (def.getTemp() != op.getTemp()) {
            reg = reg.advance(def.bytes());
            continue;
         }

         /* Don't propagate misaligned SGPRs.
          * Note: No ALU instruction can take a variable larger than 64bit.
          */
         if (op.regClass() == s2 && reg.reg() % 2 != 0)
            break;

         /* Sub dword operands might need updates to SDWA/opsel,
          * but we only track full register writes at the moment.
          */
         assert(op.physReg().byte() == reg.byte());

         /* If there is only one use (left), recolor the split_vector definition */
         if (ctx.uses[op.tempId()] == 1)
            def.setFixed(reg);
         else
            ctx.uses[op.tempId()]--;

         /* Use the p_split_vector operand register directly.
          *
          * Note: this might violate register assignment rules to some extend
          *       in case the definition does not get recolored, eventually.
          */
         instr->operands[i].setFixed(reg);
         break;
      }
   }
}

void
try_convert_fma_to_vop2(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* We convert v_fma_f32 with inline constant to fmamk/fmaak.
    * This is only benefical if it allows more VOPD.
    */
   if (ctx.program->gfx_level < GFX11 || ctx.program->wave_size != 32 ||
       instr->opcode != aco_opcode::v_fma_f32 || instr->usesModifiers())
      return;

   int constant_idx = -1;
   int vgpr_idx = -1;
   for (int i = 0; i < 3; i++) {
      const Operand& op = instr->operands[i];
      if (op.isConstant() && !op.isLiteral())
         constant_idx = i;
      else if (op.isOfType(RegType::vgpr))
         vgpr_idx = i;
      else
         return;
   }

   if (constant_idx < 0 || vgpr_idx < 0)
      return;

   std::swap(instr->operands[constant_idx], instr->operands[2]);
   if (constant_idx == 0 || vgpr_idx == 0)
      std::swap(instr->operands[0], instr->operands[1]);
   instr->operands[2] = Operand::literal32(instr->operands[2].constantValue());
   instr->opcode = constant_idx == 2 ? aco_opcode::v_fmaak_f32 : aco_opcode::v_fmamk_f32;
   instr->format = Format::VOP2;
}

bool
instr_overwrites(Instruction* instr, PhysReg reg, unsigned size)
{
   for (Definition def : instr->definitions) {
      if (def.physReg() + def.size() > reg && reg + size > def.physReg())
         return true;
   }
   if (instr->isPseudo() && instr->pseudo().needs_scratch_reg) {
      PhysReg scratch_reg = instr->pseudo().scratch_sgpr;
      if (scratch_reg >= reg && reg + size > scratch_reg)
         return true;
   }
   return false;
}

bool
try_insert_saveexec_out_of_loop(pr_opt_ctx& ctx, Block* block, Definition saved_exec,
                                unsigned saveexec_pos)
{
   /* This pattern can be created by try_optimize_branching_sequence:
    * BB1: // loop-header
    *    ...                              // nothing that clobbers s[0:1] or writes exec
    *    s[0:1] = p_parallelcopy exec     // we will move this
    *    exec = v_cmpx_...
    *    p_branch_z exec BB3, BB2
    * BB2:
    *    ...
    *    p_branch BB3
    * BB3:
    *    exec = p_parallelcopy s[0:1]     // exec and s[0:1] contain the same mask
    *    ...                              // nothing that clobbers s[0:1] or writes exec
    *    p_branch_nz scc BB1, BB4
    * BB4:
    *    ...
    *
    * If we know that that exec copy in the loop header is only needed in the
    * first iteration, it can be inserted into the preheader by adding a phi:
    *
    * BB1: // loop-header
    *    s[0:1] = p_linear_phi exec, s[0:1]
    *
    * will be lowered to a parallelcopy at the loop preheader.
    */
   if (block->linear_preds.size() != 2)
      return false;

   /* Check if exec is written, or the copy's dst overwritten in the loop header. */
   for (unsigned i = 0; i < saveexec_pos; i++) {
      if (!block->instructions[i])
         continue;
      if (block->instructions[i]->writes_exec())
         return false;
      if (instr_overwrites(block->instructions[i].get(), saved_exec.physReg(), saved_exec.size()))
         return false;
   }

   /* The register(s) must already contain the same value as exec in the continue block. */
   Block* cont = &ctx.program->blocks[block->linear_preds[1]];
   do {
      for (int i = cont->instructions.size() - 1; i >= 0; i--) {
         Instruction* instr = cont->instructions[i].get();
         if (instr->opcode == aco_opcode::p_parallelcopy && instr->definitions.size() == 1 &&
             instr->definitions[0].physReg() == exec &&
             instr->operands[0].physReg() == saved_exec.physReg()) {

            /* Insert after existing phis at the loop header because
             * the first phi might contain a valid scratch reg if needed.
             */
            auto it = std::find_if(block->instructions.begin(), block->instructions.end(),
                                   [](aco_ptr<Instruction>& phi) { return phi && !is_phi(phi); });

            Instruction* phi = create_instruction(aco_opcode::p_linear_phi, Format::PSEUDO, 2, 1);
            phi->definitions[0] = saved_exec;
            phi->operands[0] = Operand(exec, ctx.program->lane_mask);
            phi->operands[1] = instr->operands[0];
            block->instructions.emplace(it, phi);
            return true;
         }

         if (instr->writes_exec())
            return false;
         if (instr_overwrites(instr, saved_exec.physReg(), saved_exec.size()))
            return false;
      }
   } while (cont->linear_preds.size() == 1 && (cont = &ctx.program->blocks[cont->linear_preds[0]]));

   return false;
}

void
fixup_reg_writes(pr_opt_ctx& ctx, unsigned start)
{
   const unsigned current_idx = ctx.current_instr_idx;
   for (unsigned i = start; i < current_idx; i++) {
      ctx.current_instr_idx = i;
      if (ctx.current_block->instructions[i])
         save_reg_writes(ctx, ctx.current_block->instructions[i]);
   }

   ctx.current_instr_idx = current_idx;
}

bool
try_optimize_branching_sequence(pr_opt_ctx& ctx, aco_ptr<Instruction>& exec_copy)
{
   /* Try to optimize the branching sequence at the end of a block.
    *
    * We are looking for blocks that look like this:
    *
    * BB:
    * ... instructions ...
    * s[N:M] = <exec_val instruction>
    * ... other instructions that don't depend on exec ...
    * p_logical_end
    * exec = <exec_copy instruction> s[N:M]
    * p_cbranch exec
    *
    * The main motivation is to eliminate exec_copy.
    * Depending on the context, we try to do the following:
    *
    * 1. Reassign exec_val to write exec directly
    * 2. If possible, eliminate exec_copy
    * 3. When exec_copy also saves the old exec mask, insert a
    *    new copy instruction before exec_val
    * 4. Reassign any instruction that used s[N:M] to use exec
    *
    * This is beneficial for the following reasons:
    *
    * - Fewer instructions in the block when exec_copy can be eliminated
    * - As a result, when exec_val is VOPC this also improves the stalls
    *   due to SALU waiting for VALU. This works best when we can also
    *   remove the branching instruction, in which case the stall
    *   is entirely eliminated.
    * - When exec_copy can't be removed, the reassignment may still be
    *   very slightly beneficial to latency.
    */

   if (!exec_copy->writes_exec())
      return false;

   const aco_opcode and_saveexec = ctx.program->lane_mask == s2 ? aco_opcode::s_and_saveexec_b64
                                                                : aco_opcode::s_and_saveexec_b32;

   const aco_opcode s_and =
      ctx.program->lane_mask == s2 ? aco_opcode::s_and_b64 : aco_opcode::s_and_b32;

   const aco_opcode s_andn2 =
      ctx.program->lane_mask == s2 ? aco_opcode::s_andn2_b64 : aco_opcode::s_andn2_b32;

   if (exec_copy->opcode != and_saveexec && exec_copy->opcode != aco_opcode::p_parallelcopy &&
       (exec_copy->opcode != s_and || exec_copy->operands[1].physReg() != exec) &&
       (exec_copy->opcode != s_andn2 || exec_copy->operands[0].physReg() != exec))
      return false;

   const bool negate = exec_copy->opcode == s_andn2;
   const Operand& exec_copy_op = exec_copy->operands[negate];

   /* The SCC def of s_and/s_and_saveexec must be unused. */
   if (exec_copy->opcode != aco_opcode::p_parallelcopy && !exec_copy->definitions[1].isKill())
      return false;

   Idx exec_val_idx = last_writer_idx(ctx, exec_copy_op);
   if (!exec_val_idx.found() || exec_val_idx.block != ctx.current_block->index)
      return false;

   if (is_overwritten_since(ctx, exec, ctx.program->lane_mask, exec_val_idx)) {
      // TODO: in case nothing needs the previous exec mask, just remove it
      return false;
   }

   Instruction* exec_val = ctx.get(exec_val_idx);

   /* Only allow SALU with multiple definitions. */
   if (!exec_val->isSALU() && exec_val->definitions.size() > 1)
      return false;

   const bool vcmpx_exec_only = ctx.program->gfx_level >= GFX10;

   if (negate && !exec_val->isVOPC())
      return false;

   /* Check if a suitable v_cmpx opcode exists. */
   const aco_opcode v_cmpx_op =
      exec_val->isVOPC()
         ? (negate ? get_vcmpx(get_vcmp_inverse(exec_val->opcode)) : get_vcmpx(exec_val->opcode))
         : aco_opcode::num_opcodes;
   const bool vopc = v_cmpx_op != aco_opcode::num_opcodes;

   /* V_CMPX+DPP returns 0 with reads from disabled lanes, unlike V_CMP+DPP (RDNA3 ISA doc, 7.7) */
   if (vopc && exec_val->isDPP())
      return false;

   /* If s_and_saveexec is used, we'll need to insert a new instruction to save the old exec. */
   bool save_original_exec =
      exec_copy->opcode == and_saveexec && !exec_copy->definitions[0].isKill();

   const Definition exec_wr_def = exec_val->definitions[0];
   const Definition exec_copy_def = exec_copy->definitions[0];

   /* If we need to negate, the instruction has to be otherwise unused. */
   if (negate && ctx.uses[exec_copy_op.tempId()] != 1)
      return false;

   /* The copy can be removed when it kills its operand.
    * v_cmpx also writes the original destination pre GFX10.
    */
   const bool can_remove_copy = exec_copy_op.isKill() || (vopc && !vcmpx_exec_only);

   /* Always allow reassigning when the value is written by (usable) VOPC.
    * Note, VOPC implicitly contains "& exec" because it yields zero on inactive lanes.
    * Additionally, when value is copied as-is, also allow SALU and parallelcopies.
    */
   const bool can_reassign =
      vopc || (exec_copy->opcode == aco_opcode::p_parallelcopy &&
               (exec_val->isSALU() || exec_val->opcode == aco_opcode::p_parallelcopy ||
                exec_val->opcode == aco_opcode::p_create_vector));

   /* The reassignment is not worth it when both the original exec needs to be copied
    * and the new exec copy can't be removed. In this case we'd end up with more instructions.
    */
   if (!can_reassign || (save_original_exec && !can_remove_copy))
      return false;

   /* Ensure that nothing needs a previous exec between exec_val_idx and the current exec write. */
   for (unsigned i = exec_val_idx.instr + 1; i < ctx.current_instr_idx; i++) {
      Instruction* instr = ctx.current_block->instructions[i].get();
      if (instr && needs_exec_mask(instr))
         return false;

      /* If the successor has phis, copies might have to be inserted at p_logical_end. */
      if (instr && instr->opcode == aco_opcode::p_logical_end &&
          ctx.current_block->logical_succs.size() == 1)
         return false;
   }

   /* When exec_val and exec_copy are non-adjacent, check whether there are any
    * instructions inbetween (besides p_logical_end) which may inhibit the optimization.
    */
   if (save_original_exec) {
      if (is_overwritten_since(ctx, exec_copy_def, exec_val_idx))
         return false;

      unsigned prev_wr_idx = ctx.current_instr_idx;
      if (exec_copy_op.physReg() == exec_copy_def.physReg()) {
         /* We'd overwrite the saved original exec */
         if (vopc && !vcmpx_exec_only)
            return false;

         /* Other instructions can use exec directly, so only check exec_val instr */
         prev_wr_idx = exec_val_idx.instr + 1;
      }
      /* Make sure that nothing else needs these registers in-between. */
      for (unsigned i = exec_val_idx.instr; i < prev_wr_idx; i++) {
         if (ctx.current_block->instructions[i]) {
            for (const Operand op : ctx.current_block->instructions[i]->operands) {
               if (op.physReg() + op.size() > exec_copy_def.physReg() &&
                   exec_copy_def.physReg() + exec_copy_def.size() > op.physReg())
                  return false;
            }
         }
      }
   }

   /* Reassign the instruction to write exec directly. */
   if (vopc) {
      /* Add one extra definition for exec and copy the VOP3-specific fields if present. */
      if (!vcmpx_exec_only) {
         if (exec_val->isSDWA()) {
            /* This might work but it needs testing and more code to copy the instruction. */
            return false;
         } else {
            Instruction* tmp =
               create_instruction(v_cmpx_op, exec_val->format, exec_val->operands.size(),
                                  exec_val->definitions.size() + 1);
            std::copy(exec_val->operands.cbegin(), exec_val->operands.cend(),
                      tmp->operands.begin());
            std::copy(exec_val->definitions.cbegin(), exec_val->definitions.cend(),
                      tmp->definitions.begin());

            VALU_instruction& src = exec_val->valu();
            VALU_instruction& dst = tmp->valu();
            dst.opsel = src.opsel;
            dst.omod = src.omod;
            dst.clamp = src.clamp;
            dst.neg = src.neg;
            dst.abs = src.abs;

            ctx.current_block->instructions[exec_val_idx.instr].reset(tmp);
            exec_val = ctx.get(exec_val_idx);
         }
      }

      /* Set v_cmpx opcode. */
      exec_val->opcode = v_cmpx_op;
      exec_val->definitions.back() = Definition(exec, ctx.program->lane_mask);

      /* Change instruction from VOP3 to plain VOPC when possible. */
      if (vcmpx_exec_only && !exec_val->usesModifiers() &&
          (exec_val->operands.size() < 2 || exec_val->operands[1].isOfType(RegType::vgpr)))
         exec_val->format = Format::VOPC;
   } else {
      exec_val->definitions[0] = Definition(exec, ctx.program->lane_mask);
   }
   for (unsigned i = 0; i < ctx.program->lane_mask.size(); i++)
      ctx.instr_idx_by_regs[ctx.current_block->index][exec + i] =
         ctx.instr_idx_by_regs[ctx.current_block->index][exec_copy_op.physReg() + i];

   /* If there are other instructions (besides p_logical_end) between
    * writing the value and copying it to exec, reassign uses
    * of the old definition.
    */
   Temp exec_temp = exec_copy_op.getTemp();
   for (unsigned i = exec_val_idx.instr + 1; i < ctx.current_instr_idx; i++) {
      if (ctx.current_block->instructions[i]) {
         for (Operand& op : ctx.current_block->instructions[i]->operands) {
            if (op.isTemp() && op.getTemp() == exec_temp) {
               op = Operand(exec, op.regClass());
               ctx.uses[exec_temp.id()]--;
            }
         }
      }
   }

   if (can_remove_copy) {
      /* Remove the copy. */
      exec_copy.reset();
      ctx.uses[exec_temp.id()]--;
   } else {
      /* Reassign the copy to write the register of the original value. */
      exec_copy.reset(create_instruction(aco_opcode::p_parallelcopy, Format::PSEUDO, 1, 1));
      exec_copy->definitions[0] = exec_wr_def;
      exec_copy->operands[0] = Operand(exec, ctx.program->lane_mask);
   }

   if (save_original_exec) {
      /* Insert a new instruction that saves the original exec before it is overwritten.
       * Do this last, because inserting in the instructions vector may invalidate the exec_val
       * reference.
       */
      if (ctx.current_block->kind & block_kind_loop_header) {
         if (try_insert_saveexec_out_of_loop(ctx, ctx.current_block, exec_copy_def,
                                             exec_val_idx.instr)) {
            /* We inserted something after the last phi, so fixup indices from the start. */
            fixup_reg_writes(ctx, 0);
            return true;
         }
      }
      Instruction* copy = create_instruction(aco_opcode::p_parallelcopy, Format::PSEUDO, 1, 1);
      copy->definitions[0] = exec_copy_def;
      copy->operands[0] = Operand(exec, ctx.program->lane_mask);
      auto it = std::next(ctx.current_block->instructions.begin(), exec_val_idx.instr);
      ctx.current_block->instructions.emplace(it, copy);

      /* Fixup indices after inserting an instruction. */
      fixup_reg_writes(ctx, exec_val_idx.instr);
      return true;
   }

   return true;
}

void
try_skip_const_branch(pr_opt_ctx& ctx, aco_ptr<Instruction>& branch)
{
   if (branch->opcode != aco_opcode::p_cbranch_z || branch->operands[0].physReg() != exec)
      return;
   if (branch->branch().never_taken)
      return;

   Idx exec_val_idx = last_writer_idx(ctx, branch->operands[0]);
   if (!exec_val_idx.found())
      return;

   Instruction* exec_val = ctx.get(exec_val_idx);
   if ((exec_val->opcode == aco_opcode::p_parallelcopy && exec_val->operands.size() == 1) ||
       exec_val->opcode == aco_opcode::p_create_vector) {
      /* Remove the branch instruction when exec is constant non-zero. */
      bool is_const_val = std::any_of(exec_val->operands.begin(), exec_val->operands.end(),
                                      [](const Operand& op) -> bool
                                      { return op.isConstant() && op.constantValue(); });
      branch->branch().never_taken |= is_const_val;
   }
}

void
process_instruction(pr_opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* Don't try to optimize instructions which are already dead. */
   if (!instr || is_dead(ctx.uses, instr.get())) {
      instr.reset();
      ctx.current_instr_idx++;
      return;
   }
   if (try_optimize_branching_sequence(ctx, instr))
      return;

   try_apply_branch_vcc(ctx, instr);

   try_optimize_to_scc_zero_cmp(ctx, instr);

   try_optimize_scc_nocompare(ctx, instr);

   try_combine_dpp(ctx, instr);

   try_reassign_split_vector(ctx, instr);

   try_convert_fma_to_vop2(ctx, instr);

   try_eliminate_scc_copy(ctx, instr);

   save_scc_copy_producer(ctx, instr);

   save_reg_writes(ctx, instr);

   ctx.current_instr_idx++;
}

} // namespace

void
optimize_postRA(Program* program)
{
   pr_opt_ctx ctx(program);

   /* Forward pass
    * Goes through each instruction exactly once, and can transform
    * instructions or adjust the use counts of temps.
    */
   for (auto& block : program->blocks) {
      ctx.reset_block(&block);

      while (ctx.current_instr_idx < block.instructions.size()) {
         aco_ptr<Instruction>& instr = block.instructions[ctx.current_instr_idx];
         process_instruction(ctx, instr);
      }

      try_skip_const_branch(ctx, block.instructions.back());
   }

   /* Cleanup pass
    * Gets rid of instructions which are manually deleted or
    * no longer have any uses.
    */
   for (auto& block : program->blocks) {
      std::vector<aco_ptr<Instruction>> instructions;
      instructions.reserve(block.instructions.size());

      for (aco_ptr<Instruction>& instr : block.instructions) {
         if (!instr || is_dead(ctx.uses, instr.get()))
            continue;

         instructions.emplace_back(std::move(instr));
      }

      block.instructions = std::move(instructions);
   }
}

} // namespace aco
