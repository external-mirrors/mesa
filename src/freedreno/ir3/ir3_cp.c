/*
 * Copyright © 2014 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <math.h>
#include "util/half_float.h"
#include "util/u_math.h"

#include "ir3.h"
#include "ir3_compiler.h"
#include "ir3_shader.h"

#define swap(a, b)                                                             \
   do {                                                                        \
      __typeof(a) __tmp = (a);                                                 \
      (a) = (b);                                                               \
      (b) = __tmp;                                                             \
   } while (0)

/*
 * Copy Propagate:
 */

struct ir3_cp_ctx {
   struct ir3 *shader;
   struct ir3_shader_variant *so;
   bool progress;
   bool lower_imm_to_const;
};

/* is it a type preserving mov, with ok flags?
 *
 * @instr: the mov to consider removing
 * @dst_instr: the instruction consuming the mov (instr)
 *
 * TODO maybe drop allow_flags since this is only false when dst is
 * NULL (ie. outputs)
 */
static bool
is_eligible_mov(struct ir3_instruction *instr,
                struct ir3_instruction *dst_instr, bool allow_flags)
{
   if (is_same_type_mov(instr)) {
      struct ir3_register *dst = instr->dsts[0];
      struct ir3_register *src = instr->srcs[0];
      struct ir3_instruction *src_instr = ssa(src);

      /* only if mov src is SSA (not const/immed): */
      if (!src_instr)
         return false;

      /* no indirect: */
      if (dst->flags & IR3_REG_RELATIV)
         return false;
      if (src->flags & IR3_REG_RELATIV)
         return false;

      if (src->flags & IR3_REG_ARRAY)
         return false;

      if (!allow_flags)
         if (src->flags & (IR3_REG_FABS | IR3_REG_FNEG | IR3_REG_SABS |
                           IR3_REG_SNEG | IR3_REG_BNOT))
            return false;

      return true;
   }
   return false;
}

/* propagate register flags from src to dst.. negates need special
 * handling to cancel each other out.
 */
static void
combine_flags(unsigned *dstflags, struct ir3_instruction *src)
{
   unsigned srcflags = src->srcs[0]->flags;

   /* if what we are combining into already has (abs) flags,
    * we can drop (neg) from src:
    */
   if (*dstflags & IR3_REG_FABS)
      srcflags &= ~IR3_REG_FNEG;
   if (*dstflags & IR3_REG_SABS)
      srcflags &= ~IR3_REG_SNEG;

   if (srcflags & IR3_REG_FABS)
      *dstflags |= IR3_REG_FABS;
   if (srcflags & IR3_REG_SABS)
      *dstflags |= IR3_REG_SABS;
   if (srcflags & IR3_REG_FNEG)
      *dstflags ^= IR3_REG_FNEG;
   if (srcflags & IR3_REG_SNEG)
      *dstflags ^= IR3_REG_SNEG;
   if (srcflags & IR3_REG_BNOT)
      *dstflags ^= IR3_REG_BNOT;

   *dstflags &= ~(IR3_REG_SSA | IR3_REG_SHARED);
   *dstflags |= srcflags & IR3_REG_SSA;
   *dstflags |= srcflags & IR3_REG_CONST;
   *dstflags |= srcflags & IR3_REG_IMMED;
   *dstflags |= srcflags & IR3_REG_RELATIV;
   *dstflags |= srcflags & IR3_REG_ARRAY;
   *dstflags |= srcflags & IR3_REG_SHARED;

   /* if src of the src is boolean we can drop the (abs) since we know
    * the source value is already a postitive integer.  This cleans
    * up the absnegs that get inserted when converting between nir and
    * native boolean (see ir3_b2n/n2b)
    */
   struct ir3_instruction *srcsrc = ssa(src->srcs[0]);
   if (srcsrc && is_bool(srcsrc))
      *dstflags &= ~IR3_REG_SABS;
}

/* Tries lowering an immediate register argument to a const buffer access by
 * adding to the list of immediates to be pushed to the const buffer when
 * switching to this shader.
 */
static bool
lower_immed(struct ir3_cp_ctx *ctx, struct ir3_instruction *instr, unsigned n,
            struct ir3_register *reg, unsigned new_flags)
{
   if (!ctx->lower_imm_to_const)
      return false;

   if (!(new_flags & IR3_REG_IMMED))
      return false;

   new_flags &= ~IR3_REG_IMMED;
   new_flags |= IR3_REG_CONST;

   if (!ir3_valid_flags(instr, n, new_flags))
      return false;

   reg = ir3_reg_clone(ctx->shader, reg);

   /* Half constant registers seems to handle only 32-bit values
    * within floating-point opcodes. So convert back to 32-bit values.
    */
   bool f_opcode =
      (is_cat2_float(instr->opc) || is_cat3_float(instr->opc)) ? true : false;
   if (f_opcode && (new_flags & IR3_REG_HALF))
      reg->uim_val = fui(_mesa_half_to_float(reg->uim_val));

   /* in some cases, there are restrictions on (abs)/(neg) plus const..
    * so just evaluate those and clear the flags:
    */
   if (new_flags & IR3_REG_SABS) {
      reg->iim_val = abs(reg->iim_val);
      new_flags &= ~IR3_REG_SABS;
   }

   if (new_flags & IR3_REG_FABS) {
      reg->fim_val = fabs(reg->fim_val);
      new_flags &= ~IR3_REG_FABS;
   }

   if (new_flags & IR3_REG_SNEG) {
      reg->iim_val = -reg->iim_val;
      new_flags &= ~IR3_REG_SNEG;
   }

   if (new_flags & IR3_REG_FNEG) {
      reg->fim_val = -reg->fim_val;
      new_flags &= ~IR3_REG_FNEG;
   }

   reg->num = ir3_const_find_imm(ctx->so, reg->uim_val);

   if (reg->num == INVALID_CONST_REG) {
      reg->num = ir3_const_add_imm(ctx->so, reg->uim_val);

      if (reg->num == INVALID_CONST_REG)
         return false;
   }

   reg->flags = new_flags;

   instr->srcs[n] = reg;

   return true;
}

static void
unuse(struct ir3_instruction *instr)
{
   assert(instr->use_count > 0);

   if (--instr->use_count == 0) {
      struct ir3_block *block = instr->block;

      instr->barrier_class = 0;
      instr->barrier_conflict = 0;

      /* we don't want to remove anything in keeps (which could
       * be things like array store's)
       */
      for (unsigned i = 0; i < block->keeps_count; i++) {
         assert(block->keeps[i] != instr);
      }
   }
}

/* Try to swap src n of instr using new_flags with src swap_n. */
static bool
try_swap_two_srcs(struct ir3_instruction *instr, unsigned n, unsigned new_flags,
                  unsigned swap_n)
{
   /* NOTE: pre-swap first two src's before valid_flags(),
    * which might try to dereference the n'th src:
    */
   swap(instr->srcs[swap_n], instr->srcs[n]);

   bool valid_swap =
      /* can we propagate mov if we move 2nd src to first? */
      ir3_valid_flags(instr, swap_n, new_flags) &&
      /* and does first src fit in second slot? */
      ir3_valid_flags(instr, n, instr->srcs[n]->flags);

   if (!valid_swap) {
      /* put things back the way they were: */
      swap(instr->srcs[swap_n], instr->srcs[n]);
   } else {
      /* otherwise leave things swapped */
      instr->cat3.swapped = true;
   }

   return valid_swap;
}

/**
 * Handles the special case of the 2nd src (n == 1) to "normal" mad
 * instructions, which cannot reference a constant.  See if it is
 * possible to swap the 1st and 2nd sources.
 * The same case is handled for sad but since it's 3-src commutative, we can
 * also try to swap the 2nd src with the 3rd. In addition, we can try to swap
 * either the 1st or 3rd srcs with the 2nd which may be useful since only the
 * 2nd src supports (neg).
 */
static bool
try_swap_cat3_two_srcs(struct ir3_instruction *instr, unsigned n,
                       unsigned new_flags)
{
   if (!(is_mad(instr->opc) && n == 1) && !is_sad(instr->opc))
      return false;

   /* If we've already tried, nothing more to gain.. we will only
    * have previously swapped if the original 2nd src was const or
    * immed.  So swapping back won't improve anything and could
    * result in an infinite "progress" loop.
    */
   if (instr->cat3.swapped)
      return false;

   /* cat3 doesn't encode immediate, but we can lower immediate
    * to const if that helps:
    */
   if (new_flags & IR3_REG_IMMED) {
      new_flags &= ~IR3_REG_IMMED;
      new_flags |= IR3_REG_CONST;
   }

   /* If the reason we couldn't fold without swapping is something
    * other than const source, then swapping won't help:
    */
   if (!(new_flags & (IR3_REG_CONST | IR3_REG_SHARED | IR3_REG_SNEG)))
      return false;

   if (n == 1) {
      /* Both mad and sad support swapping srcs 2 and 1. */
      if (try_swap_two_srcs(instr, n, new_flags, 0)) {
         return true;
      }

      /* sad also supports swapping srcs 2 and 3. */
      if (is_sad(instr->opc) && try_swap_two_srcs(instr, n, new_flags, 2)) {
         return true;
      }
   }

   /* sad also supports swapping srcs 1 or 3 with 2. */
   return is_sad(instr->opc) && try_swap_two_srcs(instr, n, new_flags, 1);
}

/**
 * Handle cp for a given src register.  This additionally handles
 * the cases of collapsing immedate/const (which replace the src
 * register with a non-ssa src) or collapsing mov's from relative
 * src (which needs to also fixup the address src reference by the
 * instruction).
 */
static bool
reg_cp(struct ir3_cp_ctx *ctx, struct ir3_instruction *instr,
       struct ir3_register *reg, unsigned n)
{
   struct ir3_instruction *src = ssa(reg);

   if (is_eligible_mov(src, instr, true)) {
      /* simple case, no immed/const/relativ, only mov's w/ ssa src: */
      struct ir3_register *src_reg = src->srcs[0];
      unsigned new_flags = reg->flags;

      combine_flags(&new_flags, src);

      if (ir3_valid_flags(instr, n, new_flags)) {
         if (new_flags & IR3_REG_ARRAY) {
            assert(!(reg->flags & IR3_REG_ARRAY));
            reg->array = src_reg->array;
         }
         reg->flags = new_flags;
         reg->def = src_reg->def;

         instr->barrier_class |= src->barrier_class;
         instr->barrier_conflict |= src->barrier_conflict;

         unuse(src);
         reg->def->instr->use_count++;

         return true;
      } else if (try_swap_cat3_two_srcs(instr, n, new_flags)) {
         return true;
      }
   } else if ((is_same_type_mov(src) || is_const_mov(src)) &&
              /* cannot collapse const/immed/etc into control flow: */
              opc_cat(instr->opc) != 0) {
      /* immed/const/etc cases, which require some special handling: */
      struct ir3_register *src_reg = src->srcs[0];
      unsigned new_flags = reg->flags;

      if (src_reg->flags & IR3_REG_ARRAY)
         return false;

      combine_flags(&new_flags, src);

      if (!ir3_valid_flags(instr, n, new_flags)) {
         /* See if lowering an immediate to const would help. */
         if (lower_immed(ctx, instr, n, src_reg, new_flags))
            return true;

         /* special case for "normal" mad instructions, we can
          * try swapping the first two args if that fits better.
          *
          * the "plain" MAD's (ie. the ones that don't shift first
          * src prior to multiply) can swap their first two srcs if
          * src[0] is !CONST and src[1] is CONST:
          */
         if (try_swap_cat3_two_srcs(instr, n, new_flags)) {
            return true;
         } else {
            return false;
         }
      }

      /* Here we handle the special case of mov from
       * CONST and/or RELATIV.  These need to be handled
       * specially, because in the case of move from CONST
       * there is no src ir3_instruction so we need to
       * replace the ir3_register.  And in the case of
       * RELATIV we need to handle the address register
       * dependency.
       */
      if (src_reg->flags & IR3_REG_CONST) {
         /* an instruction cannot reference two different
          * address registers:
          */
         if ((src_reg->flags & IR3_REG_RELATIV) &&
             conflicts(instr->address, reg->def->instr->address))
            return false;

         /* These macros expand to a mov in an if statement */
         if ((src_reg->flags & IR3_REG_RELATIV) &&
             is_subgroup_cond_mov_macro(instr))
            return false;

         /* This seems to be a hw bug, or something where the timings
          * just somehow don't work out.  This restriction may only
          * apply if the first src is also CONST.
          */
         if (ctx->so->compiler->cat3_rel_offset_0_quirk &&
             (opc_cat(instr->opc) == 3) && (n == 2) &&
             (src_reg->flags & IR3_REG_RELATIV) && (src_reg->array.offset == 0))
            return false;

         /* When narrowing constant from 32b to 16b, it seems
          * to work only for float. So we should do this only with
          * float opcodes.
          */
         if (src->cat1.dst_type == TYPE_F16) {
            /* TODO: should we have a way to tell phi/collect to use a
             * float move so that this is legal?
             */
            if (is_meta(instr))
               return false;
            if (instr->opc == OPC_MOV && !type_float(instr->cat1.src_type))
               return false;
            if (!is_cat2_float(instr->opc) && !is_cat3_float(instr->opc))
               return false;
         } else if (src->cat1.dst_type == TYPE_U16 || src->cat1.dst_type == TYPE_S16) {
            /* Since we set CONSTANT_DEMOTION_ENABLE, a float reference of
             * what was a U16 value read from the constbuf would incorrectly
             * do 32f->16f conversion, when we want to read a 16f value.
             */
            if (is_cat2_float(instr->opc) || is_cat3_float(instr->opc))
               return false;
            if (instr->opc == OPC_MOV && type_float(instr->cat1.src_type))
               return false;
         }

         src_reg = ir3_reg_clone(instr->block->shader, src_reg);
         src_reg->flags = new_flags;
         instr->srcs[n] = src_reg;

         if (src_reg->flags & IR3_REG_RELATIV)
            ir3_instr_set_address(instr, reg->def->instr->address->def->instr);

         return true;
      }

      if (src_reg->flags & IR3_REG_IMMED) {
         int32_t iim_val = src_reg->iim_val;

         assert((opc_cat(instr->opc) == 1) ||
                      (opc_cat(instr->opc) == 2) ||
                      (is_cat3_alt(instr->opc) && (n == 0 || n == 2)) ||
                      (opc_cat(instr->opc) == 6) ||
                      is_meta(instr) ||
                      (instr->opc == OPC_ISAM && (n == 1 || n == 2)) ||
                      (is_mad(instr->opc) && (n == 0)));

         if ((opc_cat(instr->opc) == 2) &&
               !ir3_cat2_int(instr->opc)) {
            iim_val = ir3_flut(src_reg);
            if (iim_val < 0) {
               /* Fall back to trying to load the immediate as a const: */
               return lower_immed(ctx, instr, n, src_reg, new_flags);
            }
         }

         if (new_flags & IR3_REG_SABS)
            iim_val = abs(iim_val);

         if (new_flags & IR3_REG_SNEG)
            iim_val = -iim_val;

         if (new_flags & IR3_REG_BNOT)
            iim_val = ~iim_val;

         if (ir3_valid_flags(instr, n, new_flags) &&
             ir3_valid_immediate(instr, iim_val)) {
            new_flags &= ~(IR3_REG_SABS | IR3_REG_SNEG | IR3_REG_BNOT);
            src_reg = ir3_reg_clone(instr->block->shader, src_reg);
            src_reg->flags = new_flags;
            src_reg->iim_val = iim_val;
            instr->srcs[n] = src_reg;

            return true;
         } else {
            /* Fall back to trying to load the immediate as a const: */
            return lower_immed(ctx, instr, n, src_reg, new_flags);
         }
      }
   }

   return false;
}

/* Handle special case of eliminating output mov, and similar cases where
 * there isn't a normal "consuming" instruction.  In this case we cannot
 * collapse flags (ie. output mov from const, or w/ abs/neg flags, cannot
 * be eliminated)
 */
static struct ir3_instruction *
eliminate_output_mov(struct ir3_cp_ctx *ctx, struct ir3_instruction *instr)
{
   if (is_eligible_mov(instr, NULL, false)) {
      struct ir3_register *reg = instr->srcs[0];
      if (!(reg->flags & IR3_REG_ARRAY)) {
         struct ir3_instruction *src_instr = ssa(reg);
         assert(src_instr);
         ctx->progress = true;
         return src_instr;
      }
   }
   return instr;
}

/**
 * Find instruction src's which are mov's that can be collapsed, replacing
 * the mov dst with the mov src
 */
static void
instr_cp(struct ir3_cp_ctx *ctx, struct ir3_instruction *instr)
{
   if (instr->srcs_count == 0)
      return;

   if (ir3_instr_check_mark(instr))
      return;

   /* walk down the graph from each src: */
   bool progress;
   do {
      progress = false;
      foreach_src_n (reg, n, instr) {
         struct ir3_instruction *src = ssa(reg);

         if (!src)
            continue;

         instr_cp(ctx, src);

         /* TODO non-indirect access we could figure out which register
          * we actually want and allow cp..
          */
         if ((reg->flags & IR3_REG_ARRAY) && src->opc != OPC_META_PHI)
            continue;

         /* Don't CP absneg into meta instructions, that won't end well: */
         if (is_meta(instr) &&
             (src->opc == OPC_ABSNEG_F || src->opc == OPC_ABSNEG_S))
            continue;

         /* Don't CP mova and mova1 into their users */
         if (writes_addr0(src) || writes_addr1(src))
            continue;

         progress |= reg_cp(ctx, instr, reg, n);
         ctx->progress |= progress;
      }
   } while (progress);

   /* After folding a mov's source we may wind up with a type-converting mov
    * of an immediate. This happens e.g. with texture descriptors, since we
    * narrow the descriptor (which may be a constant) to a half-reg in ir3.
    * By converting the immediate in-place to the destination type, we can
    * turn the mov into a same-type mov so that it can be further propagated.
    */
   if (instr->opc == OPC_MOV && (instr->srcs[0]->flags & IR3_REG_IMMED) &&
       instr->cat1.src_type != instr->cat1.dst_type &&
       /* Only do uint types for now, until we generate other types of
        * mov's during instruction selection.
        */
       full_type(instr->cat1.src_type) == TYPE_U32 &&
       full_type(instr->cat1.dst_type) == TYPE_U32) {
      uint32_t uimm = instr->srcs[0]->uim_val;
      if (instr->cat1.dst_type == TYPE_U16)
         uimm &= 0xffff;
      instr->srcs[0]->uim_val = uimm;
      if (instr->dsts[0]->flags & IR3_REG_HALF)
         instr->srcs[0]->flags |= IR3_REG_HALF;
      else
         instr->srcs[0]->flags &= ~IR3_REG_HALF;
      instr->cat1.src_type = instr->cat1.dst_type;
      ctx->progress = true;
   }

   /* Handle converting a sam.s2en (taking samp/tex idx params via register)
    * into a normal sam (encoding immediate samp/tex idx) if they are
    * immediate. This saves some instructions and regs in the common case
    * where we know samp/tex at compile time. This needs to be done in the
    * frontend for bindless tex, though, so don't replicate it here.
    */
   if (is_tex(instr) && (instr->flags & IR3_INSTR_S2EN) &&
       !(instr->flags & IR3_INSTR_B) &&
       !(ir3_shader_debug & IR3_DBG_FORCES2EN) &&
       !(instr->srcs[0]->flags & IR3_REG_ALIAS)) {
      /* The first src will be a collect, if both of it's
       * two sources are mov from imm, then we can
       */
      struct ir3_instruction *samp_tex = ssa(instr->srcs[0]);

      assert(samp_tex->opc == OPC_META_COLLECT);

      struct ir3_register *tex = samp_tex->srcs[0];
      struct ir3_register *samp = samp_tex->srcs[1];

      if ((samp->flags & IR3_REG_IMMED) && (tex->flags & IR3_REG_IMMED) &&
          (samp->iim_val < 16) && (tex->iim_val < 16)) {
         instr->flags &= ~IR3_INSTR_S2EN;
         instr->cat5.samp = samp->iim_val;
         instr->cat5.tex = tex->iim_val;

         /* shuffle around the regs to remove the first src: */
         instr->srcs_count--;
         for (unsigned i = 0; i < instr->srcs_count; i++) {
            instr->srcs[i] = instr->srcs[i + 1];
         }

         ctx->progress = true;
      }
   }
}

bool
ir3_cp(struct ir3 *ir, struct ir3_shader_variant *so, bool lower_imm_to_const)
{
   struct ir3_cp_ctx ctx = {
      .shader = ir,
      .so = so,
      .lower_imm_to_const = lower_imm_to_const,
   };

   /* This is a bit annoying, and probably wouldn't be necessary if we
    * tracked a reverse link from producing instruction to consumer.
    * But we need to know when we've eliminated the last consumer of
    * a mov, so we need to do a pass to first count consumers of a
    * mov.
    */
   foreach_block (block, &ir->block_list) {
      foreach_instr (instr, &block->instr_list) {

         /* by the way, we don't account for false-dep's, so the CP
          * pass should always happen before false-dep's are inserted
          */
         assert(instr->deps_count == 0);

         foreach_ssa_src (src, instr) {
            src->use_count++;
         }
      }
   }

   ir3_clear_mark(ir);

   foreach_block (block, &ir->block_list) {
      struct ir3_instruction *terminator = ir3_block_get_terminator(block);
      if (terminator)
         instr_cp(&ctx, terminator);

      for (unsigned i = 0; i < block->keeps_count; i++) {
         instr_cp(&ctx, block->keeps[i]);
         block->keeps[i] = eliminate_output_mov(&ctx, block->keeps[i]);
      }
   }

   return ctx.progress;
}
