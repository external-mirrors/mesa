/*
 * Copyright © 2019 Google, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "util/dag.h"
#include "util/u_math.h"

#include "ir3.h"
#include "ir3_compiler.h"
#include "ir3_context.h"

#if MESA_DEBUG
#define SCHED_DEBUG (ir3_shader_debug & IR3_DBG_SCHEDMSGS)
#else
#define SCHED_DEBUG 0
#endif
#define d(fmt, ...)                                                            \
   do {                                                                        \
      if (SCHED_DEBUG) {                                                       \
         mesa_logi("PSCHED: " fmt, ##__VA_ARGS__);                             \
      }                                                                        \
   } while (0)

#define di(instr, fmt, ...)                                                    \
   do {                                                                        \
      if (SCHED_DEBUG) {                                                       \
         struct log_stream *stream = mesa_log_streami();                       \
         mesa_log_stream_printf(stream, "PSCHED: " fmt ": ", ##__VA_ARGS__);   \
         ir3_print_instr_stream(stream, instr);                                \
         mesa_log_stream_destroy(stream);                                      \
      }                                                                        \
   } while (0)

#define SCHED_DEBUG_DUMP_DEPTH 1

/*
 * Post RA Instruction Scheduling
 */

struct ir3_postsched_ctx {
   struct ir3 *ir;

   struct ir3_shader_variant *v;

   void *mem_ctx;
   struct ir3_block *block; /* the current block */
   struct dag *dag;

   struct list_head unscheduled_list; /* unscheduled instructions */
};

struct ir3_postsched_node {
   struct dag_node dag; /* must be first for util_dynarray_foreach */
   struct ir3_instruction *instr;
   bool partially_evaluated_path;

   /* The number of nops that need to be inserted if this instruction were
    * scheduled now. This is recalculated for all DAG heads whenever a new
    * instruction needs to be selected based on the current legalize state.
    */
   unsigned delay;

   bool has_sy_src, has_ss_src;

   unsigned max_delay;
};

struct ir3_postsched_block_data {
   struct ir3_legalize_state legalize_state;
   unsigned sy_delay;
   unsigned ss_delay;
};

#define foreach_sched_node(__n, __list)                                        \
   list_for_each_entry (struct ir3_postsched_node, __n, __list, dag.link)

static bool
has_sy_src(struct ir3_instruction *instr)
{
   struct ir3_postsched_node *node = instr->data;
   return !!(node->instr->flags & IR3_INSTR_SY);
}

static bool
has_ss_src(struct ir3_instruction *instr)
{
   struct ir3_postsched_node *node = instr->data;
   return !!(node->instr->flags & IR3_INSTR_SS);
}

#ifndef NDEBUG
static void
sched_dag_validate_cb(const struct dag_node *node, void *data)
{
   struct ir3_postsched_node *n = (struct ir3_postsched_node *)node;

   ir3_print_instr(n->instr);
}
#endif

static void
schedule(struct ir3_postsched_ctx *ctx, struct ir3_instruction *instr)
{
   assert(ctx->block == instr->block);

   /* remove from unscheduled_list:
    */
   list_delinit(&instr->node);

   di(instr, "schedule");

   struct ir3_postsched_node *n = instr->data;

   list_addtail(&instr->node, &instr->block->instr_list);

   dag_prune_head(ctx->dag, &n->dag);

   struct ir3_postsched_block_data *bd = ctx->block->data;
   bd->legalize_state.cycle += n->delay;
   ir3_update_legalize_state(&bd->legalize_state, ctx->v->compiler, instr);

   if (is_meta(instr) && (instr->opc != OPC_META_TEX_PREFETCH))
      return;

   if (is_ss_producer(instr)) {
      bd->ss_delay = soft_ss_delay(instr);
   } else if (has_ss_src(instr)) {
      bd->ss_delay = 0;
   } else if (bd->ss_delay > 0) {
      bd->ss_delay--;
   }

   if (is_sy_producer(instr)) {
      bd->sy_delay = soft_sy_delay(instr, ctx->block->shader);
   } else if (has_sy_src(instr)) {
      bd->sy_delay = 0;
   } else if (bd->sy_delay > 0) {
      bd->sy_delay--;
   }
}

static unsigned
node_delay(struct ir3_postsched_ctx *ctx, struct ir3_postsched_node *n)
{
   return n->delay;
}

static unsigned
node_delay_soft(struct ir3_postsched_ctx *ctx, struct ir3_postsched_node *n)
{
   unsigned delay = node_delay(ctx, n);
   struct ir3_postsched_block_data *bd = n->instr->block->data;

   /* This takes into account that as when we schedule multiple tex or sfu, the
    * first user has to wait for all of them to complete.
    */
   if (has_ss_src(n->instr))
      delay = MAX2(delay, bd->ss_delay);
   if (has_sy_src(n->instr))
      delay = MAX2(delay, bd->sy_delay);

   return delay;
}

static void
dump_node(struct ir3_postsched_ctx *ctx, struct ir3_postsched_node *n,
          int level)
{
   if (level > SCHED_DEBUG_DUMP_DEPTH)
      return;

   di(n->instr, "%*s%smaxdel=%d, node_delay=%d,node_delay_soft=%d, %d parents ",
      level * 2, "", (level > 0 ? "-> " : ""), n->max_delay, node_delay(ctx, n),
      node_delay_soft(ctx, n), n->dag.parent_count);

   util_dynarray_foreach (&n->dag.edges, struct dag_edge, edge) {
      struct ir3_postsched_node *child =
         (struct ir3_postsched_node *)edge->child;

      dump_node(ctx, child, level + 1);
   }
}

static void
dump_state(struct ir3_postsched_ctx *ctx)
{
   if (!SCHED_DEBUG)
      return;

   foreach_sched_node (n, &ctx->dag->heads) {
      dump_node(ctx, n, 0);
   }
}

/* find instruction to schedule: */
static struct ir3_instruction *
choose_instr(struct ir3_postsched_ctx *ctx)
{
   struct ir3_postsched_node *chosen = NULL;

   struct ir3_postsched_block_data *bd = ctx->block->data;

   /* Needed sync flags and nop delays potentially change after scheduling an
    * instruction. Update them for all schedulable instructions.
    */
   foreach_sched_node (n, &ctx->dag->heads) {
      enum ir3_instruction_flags sync_flags = ir3_required_sync_flags(
         &bd->legalize_state, ctx->v->compiler, n->instr);
      n->instr->flags &= ~(IR3_INSTR_SS | IR3_INSTR_SY);
      n->instr->flags |= sync_flags;
      n->delay =
         ir3_required_delay(&bd->legalize_state, ctx->v->compiler, n->instr);
   }

   dump_state(ctx);

   foreach_sched_node (n, &ctx->dag->heads) {
      if (!is_meta(n->instr))
         continue;

      if (!chosen || (chosen->max_delay < n->max_delay))
         chosen = n;
   }

   if (chosen) {
      di(chosen->instr, "prio: chose (meta)");
      return chosen->instr;
   }

   /* Try to schedule inputs with a higher priority, if possible, as
    * the last bary.f unlocks varying storage to unblock more VS
    * warps.
    */
   foreach_sched_node (n, &ctx->dag->heads) {
      if (!is_input(n->instr))
         continue;

      if (!chosen || (chosen->max_delay < n->max_delay))
         chosen = n;
   }

   if (chosen) {
      di(chosen->instr, "prio: chose (input)");
      return chosen->instr;
   }

   /* Next prioritize discards: */
   foreach_sched_node (n, &ctx->dag->heads) {
      unsigned d = node_delay(ctx, n);

      if (d > 0)
         continue;

      if (!is_kill_or_demote(n->instr))
         continue;

      if (!chosen || (chosen->max_delay < n->max_delay))
         chosen = n;
   }

   if (chosen) {
      di(chosen->instr, "csp: chose (kill, hard ready)");
      return chosen->instr;
   }

   /* Next prioritize expensive instructions: */
   foreach_sched_node (n, &ctx->dag->heads) {
      unsigned d = node_delay_soft(ctx, n);

      if (d > 0)
         continue;

      if (!(is_ss_producer(n->instr) || is_sy_producer(n->instr)))
         continue;

      if (!chosen || (chosen->max_delay < n->max_delay))
         chosen = n;
   }

   if (chosen) {
      di(chosen->instr, "csp: chose (sfu/tex, soft ready)");
      return chosen->instr;
   }

   /* Next try to find a ready leader w/ soft delay (ie. including extra
    * delay for things like tex fetch which can be synchronized w/ sync
    * bit (but we probably do want to schedule some other instructions
    * while we wait). We also allow a small amount of nops, to prefer now-nops
    * over future-nops up to a point, as that gives better results.
    */
   unsigned chosen_delay = 0;
   foreach_sched_node (n, &ctx->dag->heads) {
      unsigned d = node_delay_soft(ctx, n);

      if (d > 3)
         continue;

      if (!chosen || d < chosen_delay) {
         chosen = n;
         chosen_delay = d;
         continue;
      }

      if (d > chosen_delay)
         continue;

      if (chosen->max_delay < n->max_delay) {
         chosen = n;
         chosen_delay = d;
      }
   }

   if (chosen) {
      di(chosen->instr, "csp: chose (soft ready)");
      return chosen->instr;
   }

   /* Otherwise choose leader with maximum cost:
    */
   foreach_sched_node (n, &ctx->dag->heads) {
      if (!chosen || chosen->max_delay < n->max_delay)
         chosen = n;
   }

   if (chosen) {
      di(chosen->instr, "csp: chose (leader)");
      return chosen->instr;
   }

   return NULL;
}

struct ir3_postsched_deps_state {
   struct ir3_postsched_ctx *ctx;

   enum { F, R } direction;

   bool merged;

   /* Track the mapping between sched node (instruction) that last
    * wrote a given register (in whichever direction we are iterating
    * the block)
    *
    * Note, this table is twice as big as the # of regs, to deal with
    * half-precision regs.  The approach differs depending on whether
    * the half and full precision register files are "merged" (conflict,
    * ie. a6xx+) in which case we use "regs" for both full precision and half
    * precision dependencies and consider each full precision dep
    * as two half-precision dependencies, vs older separate (non-
    * conflicting) in which case the separate "half_regs" table is used for
    * half-precision deps. See ir3_reg_file_offset().
    */
   struct ir3_postsched_node *regs[2 * GPR_REG_SIZE];
   unsigned dst_n[2 * GPR_REG_SIZE];
   struct ir3_postsched_node *half_regs[GPR_REG_SIZE];
   unsigned half_dst_n[GPR_REG_SIZE];
   struct ir3_postsched_node *shared_regs[2 * SHARED_REG_SIZE];
   unsigned shared_dst_n[2 * SHARED_REG_SIZE];
   struct ir3_postsched_node *nongpr_regs[2 * NONGPR_REG_SIZE];
   unsigned nongpr_dst_n[2 * NONGPR_REG_SIZE];
};

static void
add_dep(struct ir3_postsched_deps_state *state,
        struct ir3_postsched_node *before, struct ir3_postsched_node *after,
        unsigned d)
{
   if (!before || !after)
      return;

   assert(before != after);

   if (state->direction == F) {
      dag_add_edge_max_data(&before->dag, &after->dag, (uintptr_t)d);
   } else {
      dag_add_edge_max_data(&after->dag, &before->dag, 0);
   }
}

static void
add_single_reg_dep(struct ir3_postsched_deps_state *state,
                   struct ir3_postsched_node *node,
                   struct ir3_postsched_node **dep_ptr,
                   unsigned *dst_n_ptr, unsigned num, int src_n,
                   int dst_n)
{
   struct ir3_postsched_node *dep = *dep_ptr;

   unsigned d = 0;
   if (src_n >= 0 && dep && state->direction == F) {
      struct ir3_compiler *compiler = state->ctx->ir->compiler;
      /* get the dst_n this corresponds to */
      unsigned dst_n = *dst_n_ptr;
      d = ir3_delayslots_with_repeat(compiler, dep->instr, node->instr, dst_n, src_n);
      if (is_sy_producer(dep->instr))
         node->has_sy_src = true;
      if (needs_ss(compiler, dep->instr, node->instr))
         node->has_ss_src = true;
   }

   if (src_n >= 0 && dep && state->direction == R) {
      /* If node generates a WAR hazard (because it doesn't consume its sources
       * immediately, dep needs (ss) to sync its dest. Even though this isn't a
       * (ss) source (but rather a dest), the effect is exactly the same so we
       * model it as such.
       */
      if (is_war_hazard_producer(node->instr)) {
         dep->has_ss_src = true;
      }
   }

   add_dep(state, dep, node, d);
   if (src_n < 0) {
      *dep_ptr = node;
      *dst_n_ptr = dst_n;
   }
}

/* This is where we handled full vs half-precision, and potential conflicts
 * between half and full precision that result in additional dependencies.
 * The 'reg' arg is really just to know half vs full precision.
 *
 * If src_n is positive, then this adds a dependency on a source register, and
 * src_n is the index passed into ir3_delayslots() for calculating the delay:
 * it corresponds to node->instr->srcs[src_n]. If src_n is negative, then
 * this is for the destination register corresponding to dst_n.
 */
static void
add_reg_dep(struct ir3_postsched_deps_state *state,
            struct ir3_postsched_node *node, const struct ir3_register *reg,
            unsigned num, int src_n, int dst_n)
{
   struct ir3_postsched_node **regs;
   unsigned *dst_n_ptr;
   enum ir3_reg_file file;
   unsigned size = reg_elem_size(reg);
   unsigned offset = ir3_reg_file_offset(reg, num, state->merged, &file);
   switch (file) {
   case IR3_FILE_FULL:
      assert(offset + size <= ARRAY_SIZE(state->regs));
      regs = state->regs;
      dst_n_ptr = state->dst_n;
      break;
   case IR3_FILE_HALF:
      assert(offset + 1 <= ARRAY_SIZE(state->half_regs));
      regs = state->half_regs;
      dst_n_ptr = state->half_dst_n;
      break;
   case IR3_FILE_SHARED:
      assert(offset + size <= ARRAY_SIZE(state->shared_regs));
      regs = state->shared_regs;
      dst_n_ptr = state->shared_dst_n;
      break;
   case IR3_FILE_NONGPR:
      assert(offset + size <= ARRAY_SIZE(state->nongpr_regs));
      regs = state->nongpr_regs;
      dst_n_ptr = state->nongpr_dst_n;
      break;
   }

   for (unsigned i = 0; i < size; i++)
      add_single_reg_dep(state, node, &regs[offset + i], &dst_n_ptr[offset + i], num, src_n, dst_n);
}

static void
calculate_deps(struct ir3_postsched_deps_state *state,
               struct ir3_postsched_node *node)
{
   /* Add dependencies on instructions that previously (or next,
    * in the reverse direction) wrote any of our src registers:
    */
   foreach_src_n (reg, i, node->instr) {
      if (reg->flags & (IR3_REG_CONST | IR3_REG_IMMED | IR3_REG_DUMMY))
         continue;

      if (reg->flags & IR3_REG_RELATIV) {
         /* mark entire array as read: */
         for (unsigned j = 0; j < reg->size; j++) {
            add_reg_dep(state, node, reg, reg->array.base + j, i, -1);
         }
      } else {
         u_foreach_bit (b, reg->wrmask) {
            add_reg_dep(state, node, reg, reg->num + b, i, -1);
         }
      }
   }

   /* And then after we update the state for what this instruction
    * wrote:
    */
   foreach_dst_n (reg, i, node->instr) {
      if (reg->wrmask == 0)
         continue;
      if (reg->flags & (IR3_REG_RT | IR3_REG_DUMMY))
         continue;
      if (reg->flags & IR3_REG_RELATIV) {
         /* mark the entire array as written: */
         for (unsigned j = 0; j < reg->size; j++) {
            add_reg_dep(state, node, reg, reg->array.base + j, -1, i);
         }
      } else {
         assert(reg->wrmask >= 1);
         u_foreach_bit (b, reg->wrmask) {
            add_reg_dep(state, node, reg, reg->num + b, -1, i);
         }
      }
   }
}

static void
calculate_forward_deps(struct ir3_postsched_ctx *ctx)
{
   struct ir3_postsched_deps_state state = {
      .ctx = ctx,
      .direction = F,
      .merged = ctx->v->mergedregs,
   };

   foreach_instr (instr, &ctx->unscheduled_list) {
      calculate_deps(&state, instr->data);
   }
}

static void
calculate_reverse_deps(struct ir3_postsched_ctx *ctx)
{
   struct ir3_postsched_deps_state state = {
      .ctx = ctx,
      .direction = R,
      .merged = ctx->v->mergedregs,
   };

   foreach_instr_rev (instr, &ctx->unscheduled_list) {
      calculate_deps(&state, instr->data);
   }
}

static void
sched_node_init(struct ir3_postsched_ctx *ctx, struct ir3_instruction *instr)
{
   struct ir3_postsched_node *n =
      rzalloc(ctx->mem_ctx, struct ir3_postsched_node);

   dag_init_node(ctx->dag, &n->dag);

   n->instr = instr;
   instr->data = n;
}

static void
sched_dag_max_delay_cb(struct dag_node *node, void *state)
{
   struct ir3_postsched_node *n = (struct ir3_postsched_node *)node;
   struct ir3_postsched_ctx *ctx = state;
   uint32_t max_delay = 0;

   util_dynarray_foreach (&n->dag.edges, struct dag_edge, edge) {
      struct ir3_postsched_node *child =
         (struct ir3_postsched_node *)edge->child;
      unsigned delay = edge->data;
      unsigned sy_delay = 0;
      unsigned ss_delay = 0;

      if (child->has_sy_src && is_sy_producer(n->instr)) {
         sy_delay = soft_sy_delay(n->instr, ctx->block->shader);
      }

      if (child->has_ss_src &&
          needs_ss(ctx->v->compiler, n->instr, child->instr)) {
         ss_delay = soft_ss_delay(n->instr);
      }

      delay = MAX3(delay, sy_delay, ss_delay);
      max_delay = MAX2(child->max_delay + delay, max_delay);
   }

   n->max_delay = MAX2(n->max_delay, max_delay);
}

static void
sched_dag_init(struct ir3_postsched_ctx *ctx)
{
   ctx->dag = dag_create(ctx->mem_ctx);

   foreach_instr (instr, &ctx->unscheduled_list)
      sched_node_init(ctx, instr);

   calculate_forward_deps(ctx);
   calculate_reverse_deps(ctx);

   /*
    * To avoid expensive texture fetches, etc, from being moved ahead
    * of kills, track the kills we've seen so far, so we can add an
    * extra dependency on them for tex/mem instructions
    */
   struct util_dynarray kills;
   util_dynarray_init(&kills, ctx->mem_ctx);

   /* The last bary.f with the (ei) flag must be scheduled before any kills,
    * or the hw gets angry. Keep track of inputs here so we can add the
    * false dep on the kill instruction.
    */
   struct util_dynarray inputs;
   util_dynarray_init(&inputs, ctx->mem_ctx);

   /*
    * Normal srcs won't be in SSA at this point, those are dealt with in
    * calculate_forward_deps() and calculate_reverse_deps().  But we still
    * have the false-dep information in SSA form, so go ahead and add
    * dependencies for that here:
    */
   foreach_instr (instr, &ctx->unscheduled_list) {
      struct ir3_postsched_node *n = instr->data;

      foreach_ssa_src_n (src, i, instr) {
         if (src->block != instr->block)
            continue;

         /* we can end up with unused false-deps.. just skip them: */
         if (src->flags & IR3_INSTR_UNUSED)
            continue;

         struct ir3_postsched_node *sn = src->data;

         /* don't consider dependencies in other blocks: */
         if (src->block != instr->block)
            continue;

         dag_add_edge_max_data(&sn->dag, &n->dag, 0);
      }

      if (is_input(instr)) {
         util_dynarray_append(&inputs, struct ir3_instruction *, instr);
      } else if (is_kill_or_demote(instr)) {
         util_dynarray_foreach (&inputs, struct ir3_instruction *, instrp) {
            struct ir3_instruction *input = *instrp;
            struct ir3_postsched_node *in = input->data;
            dag_add_edge_max_data(&in->dag, &n->dag, 0);
         }
         util_dynarray_append(&kills, struct ir3_instruction *, instr);
      } else if (is_tex(instr) || is_mem(instr)) {
         util_dynarray_foreach (&kills, struct ir3_instruction *, instrp) {
            struct ir3_instruction *kill = *instrp;
            struct ir3_postsched_node *kn = kill->data;
            dag_add_edge_max_data(&kn->dag, &n->dag, 0);
         }
      }
   }

#ifndef NDEBUG
   dag_validate(ctx->dag, sched_dag_validate_cb, NULL);
#endif

   // TODO do we want to do this after reverse-dependencies?
   dag_traverse_bottom_up(ctx->dag, sched_dag_max_delay_cb, ctx);
}

static void
sched_dag_destroy(struct ir3_postsched_ctx *ctx)
{
   ctx->dag = NULL;
}

static struct ir3_legalize_state *
get_block_legalize_state(struct ir3_block *block)
{
   struct ir3_postsched_block_data *bd = block->data;
   return bd ? &bd->legalize_state : NULL;
}

static void
sched_block(struct ir3_postsched_ctx *ctx, struct ir3_block *block)
{
   ctx->block = block;
   struct ir3_postsched_block_data *bd =
      rzalloc(ctx->mem_ctx, struct ir3_postsched_block_data);
   block->data = bd;

   ir3_init_legalize_state(&bd->legalize_state, ctx->v->compiler);
   ir3_merge_pred_legalize_states(&bd->legalize_state, block,
                                  get_block_legalize_state);

   /* Initialize the ss/sy_delay by taking the maximum from the predecessors.
    * TODO: disable carrying over tex prefetch delays from the preamble for now
    * as this seems to negatively affect nop count and stalls. This should be
    * revisited in the future.
    */
   if (block != ir3_after_preamble(ctx->ir)) {
      for (unsigned i = 0; i < block->predecessors_count; i++) {
         struct ir3_block *pred = block->predecessors[i];
         struct ir3_postsched_block_data *pred_bd = pred->data;

         if (pred_bd) {
            bd->sy_delay = MAX2(bd->sy_delay, pred_bd->sy_delay);
            bd->ss_delay = MAX2(bd->ss_delay, pred_bd->ss_delay);
         }
      }
   }

   /* The terminator has to stay at the end. Instead of trying to set up
    * dependencies to achieve this, it's easier to just remove it now and add it
    * back after scheduling.
    */
   struct ir3_instruction *terminator = ir3_block_take_terminator(block);

   /* move all instructions to the unscheduled list, and
    * empty the block's instruction list (to which we will
    * be inserting).
    */
   list_replace(&block->instr_list, &ctx->unscheduled_list);
   list_inithead(&block->instr_list);

   // TODO once we are using post-sched for everything we can
   // just not stick in NOP's prior to post-sched, and drop this.
   // for now keep this, since it makes post-sched optional:
   foreach_instr_safe (instr, &ctx->unscheduled_list) {
      switch (instr->opc) {
      case OPC_NOP:
         list_delinit(&instr->node);
         break;
      default:
         break;
      }
   }

   sched_dag_init(ctx);

   /* First schedule all meta:input instructions, followed by
    * tex-prefetch.  We want all of the instructions that load
    * values into registers before the shader starts to go
    * before any other instructions.  But in particular we
    * want inputs to come before prefetches.  This is because
    * a FS's bary_ij input may not actually be live in the
    * shader, but it should not be scheduled on top of any
    * other input (but can be overwritten by a tex prefetch)
    */
   foreach_instr_safe (instr, &ctx->unscheduled_list)
      if (instr->opc == OPC_META_INPUT)
         schedule(ctx, instr);

   foreach_instr_safe (instr, &ctx->unscheduled_list)
      if (instr->opc == OPC_META_TEX_PREFETCH)
         schedule(ctx, instr);

   foreach_instr_safe (instr, &ctx->unscheduled_list)
      if (instr->opc == OPC_PUSH_CONSTS_LOAD_MACRO)
         schedule(ctx, instr);

   while (!list_is_empty(&ctx->unscheduled_list)) {
      struct ir3_instruction *instr = choose_instr(ctx);

      unsigned delay = node_delay(ctx, instr->data);
      d("delay=%u", delay);

      assert(delay <= 6);

      schedule(ctx, instr);
   }

   sched_dag_destroy(ctx);

   if (terminator)
      list_addtail(&terminator->node, &block->instr_list);
}

static bool
is_self_mov(struct ir3_instruction *instr)
{
   if (!is_same_type_mov(instr))
      return false;

   if (instr->dsts[0]->num != instr->srcs[0]->num)
      return false;

   if (instr->dsts[0]->flags & IR3_REG_RELATIV)
      return false;

   if (instr->cat1.round != ROUND_ZERO)
      return false;

   if (instr->srcs[0]->flags &
       (IR3_REG_CONST | IR3_REG_IMMED | IR3_REG_RELATIV | IR3_REG_FNEG |
        IR3_REG_FABS | IR3_REG_SNEG | IR3_REG_SABS | IR3_REG_BNOT))
      return false;

   return true;
}

/* sometimes we end up w/ in-place mov's, ie. mov.u32u32 r1.y, r1.y
 * as a result of places were before RA we are not sure that it is
 * safe to eliminate.  We could eliminate these earlier, but sometimes
 * they are tangled up in false-dep's, etc, so it is easier just to
 * let them exist until after RA
 */
static void
cleanup_self_movs(struct ir3 *ir)
{
   foreach_block (block, &ir->block_list) {
      foreach_instr_safe (instr, &block->instr_list) {
         for (unsigned i = 0; i < instr->deps_count; i++) {
            if (instr->deps[i] && is_self_mov(instr->deps[i])) {
               instr->deps[i] = NULL;
            }
         }

         if (is_self_mov(instr))
            list_delinit(&instr->node);
      }
   }
}

bool
ir3_postsched(struct ir3 *ir, struct ir3_shader_variant *v)
{
   struct ir3_postsched_ctx ctx = {
      .ir = ir,
      .v = v,
      .mem_ctx = ralloc_context(NULL),
   };

   cleanup_self_movs(ir);

   foreach_block (block, &ir->block_list) {
      block->data = NULL;
   }

   foreach_block (block, &ir->block_list) {
      sched_block(&ctx, block);
   }

   ralloc_free(ctx.mem_ctx);
   return true;
}
