/*
 * Copyright © 2013 Rob Clark <robdclark@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef IR3_H_
#define IR3_H_

#include <stdbool.h>
#include <stdint.h>

#include "compiler/shader_enums.h"

#include "util/bitscan.h"
#include "util/list.h"
#include "util/set.h"
#include "util/u_debug.h"

#include "freedreno_common.h"

#include "instr-a3xx.h"

/* low level intermediate representation of an adreno shader program */

struct ir3_compiler;
struct ir3;
struct ir3_instruction;
struct ir3_block;

struct ir3_info {
   /* Size in bytes of the shader binary, including NIR constants and
    * padding
    */
   uint32_t size;
   /* byte offset from start of the shader to the NIR constant data. */
   uint32_t constant_data_offset;
   /* Size in dwords of the instructions. */
   uint16_t sizedwords;
   uint16_t instrs_count; /* expanded to account for rpt's */
   uint16_t preamble_instrs_count;
   uint16_t nops_count;   /* # of nop instructions, including nopN */
   uint16_t mov_count;
   uint16_t cov_count;
   uint16_t stp_count;
   uint16_t ldp_count;
   /* NOTE: max_reg, etc, does not include registers not touched
    * by the shader (ie. vertex fetched via VFD_DECODE but not
    * touched by shader)
    */
   int8_t max_reg; /* highest GPR # used by shader */
   int8_t max_half_reg;
   int16_t max_const;
   unsigned constlen;
   /* This is the maximum # of waves that can executed at once in one core,
    * assuming that they are all executing this shader.
    */
   int8_t max_waves;
   uint8_t subgroup_size;
   bool double_threadsize;
   bool multi_dword_ldp_stp;
   bool early_preamble;
   bool uses_ray_intersection;

   /* number of sync bits: */
   uint16_t ss, sy;

   /* estimate of number of cycles stalled on (ss) */
   uint16_t sstall;
   /* estimate of number of cycles stalled on (sy) */
   uint16_t systall;

   uint16_t last_baryf; /* instruction # of last varying fetch */

   uint16_t last_helper; /* last instruction to use helper invocations */

   /* Number of instructions of a given category: */
   uint16_t instrs_per_cat[8];
};

struct ir3_merge_set {
   uint16_t preferred_reg;
   uint16_t size;
   uint16_t alignment;

   unsigned interval_start;
   unsigned spill_slot;

   unsigned regs_count;
   struct ir3_register **regs;
};

typedef enum ir3_register_flags {
   IR3_REG_CONST = BIT(0),
   IR3_REG_IMMED = BIT(1),
   IR3_REG_HALF = BIT(2),
   /* Shared registers have the same value for all threads when read.
    * They can only be written when one thread is active (that is, inside
    * a "getone" block).
    */
   IR3_REG_SHARED = BIT(3),
   IR3_REG_RELATIV = BIT(4),
   IR3_REG_R = BIT(5),
   /* Most instructions, it seems, can do float abs/neg but not
    * integer.  The CP pass needs to know what is intended (int or
    * float) in order to do the right thing.  For this reason the
    * abs/neg flags are split out into float and int variants.  In
    * addition, .b (bitwise) operations, the negate is actually a
    * bitwise not, so split that out into a new flag to make it
    * more clear.
    */
   IR3_REG_FNEG = BIT(6),
   IR3_REG_FABS = BIT(7),
   IR3_REG_SNEG = BIT(8),
   IR3_REG_SABS = BIT(9),
   IR3_REG_BNOT = BIT(10),
   /* (ei) flag, end-input?  Set on last bary, presumably to signal
    * that the shader needs no more input:
    *
    * Note: Has different meaning on other instructions like add.s/u
    */
   IR3_REG_EI = BIT(11),
   /* meta-flags, for intermediate stages of IR, ie.
    * before register assignment is done:
    */
   IR3_REG_SSA = BIT(12), /* 'def' is ptr to assigning destination */
   IR3_REG_ARRAY = BIT(13),

   /* Set on a use whenever the SSA value becomes dead after the current
    * instruction.
    */
   IR3_REG_KILL = BIT(14),

   /* Similar to IR3_REG_KILL, except that if there are multiple uses of the
    * same SSA value in a single instruction, this is only set on the first
    * use.
    */
   IR3_REG_FIRST_KILL = BIT(15),

   /* Set when a destination doesn't have any uses and is dead immediately
    * after the instruction. This can happen even after optimizations for
    * corner cases such as destinations of atomic instructions.
    */
   IR3_REG_UNUSED = BIT(16),

   /* "Early-clobber" on a destination means that the destination is
    * (potentially) written before any sources are read and therefore
    * interferes with the sources of the instruction.
    */
   IR3_REG_EARLY_CLOBBER = BIT(17),

   /* If this is the last usage of a specific value in the register, the
    * register cannot be read without being written to first after this.
    * This maps to the "(last)" attribute on source GPRs in shader
    * instructions which was introduced in A7XX.
    *
    * Note: This effectively has the same semantics as IR3_REG_KILL but
    * is tracked after register assignment. Additionally, this doesn't
    * cover any const or shared registers.
    */
   IR3_REG_LAST_USE = BIT(18),

   /* Predicate register (p0.c). Cannot be combined with half or shared. */
   IR3_REG_PREDICATE = BIT(19),

   /* Render target dst. Only used by alias.rt. */
   IR3_REG_RT = BIT(20),

   /* Register that is initialized using alias.tex (or will be once the
    * alias.tex instructions are inserted). Before alias.tex is inserted, alias
    * registers may contain things that are normally not allowed by the owning
    * instruction (e.g., consts or immediates) because they will be replaced by
    * GPRs later.
    * Note that if wrmask > 1, this will be set if any of the registers is an
    * alias, even though not all of them may be. We currently have no way to
    * tell which ones are actual aliases.
    */
   IR3_REG_ALIAS = BIT(21),

   /* Alias registers allow us to allocate non-consecutive registers and remap
    * them to consecutive ones using alias.tex. We implement this by adding the
    * sources of collects directly to the sources of their users. This way, RA
    * treats them as scalar registers and we can remap them to consecutive
    * registers afterwards. This flag is used to keep track of the scalar
    * sources that should be remapped together. Every source of such an "alias
    * group" will have the IR3_REG_ALIAS set, while the first one will also have
    * IR3_REG_FIRST_ALIAS set.
    */
   IR3_REG_FIRST_ALIAS = BIT(22),

   /* Set for registers that should be ignored by all passes. For example, the
    * dummy src and dst of prefetch sam/ldc/resinfo.
    */
   IR3_REG_DUMMY = BIT(23),
} ir3_register_flags;

struct ir3_register {
   BITMASK_ENUM(ir3_register_flags) flags;

   unsigned name;

   /* used for cat5 instructions, but also for internal/IR level
    * tracking of what registers are read/written by an instruction.
    * wrmask may be a bad name since it is used to represent both
    * src and dst that touch multiple adjacent registers.
    */
   unsigned wrmask : 16; /* up to vec16 */

   /* for relative addressing, 32bits for array size is too small,
    * but otoh we don't need to deal with disjoint sets, so instead
    * use a simple size field (number of scalar components).
    *
    * Note the size field isn't important for relative const (since
    * we don't have to do register allocation for constants).
    */
   unsigned size : 16;

   /* normal registers:
    * the component is in the low two bits of the reg #, so
    * rN.x becomes: (N << 2) | x
    */
   uint16_t num;
   union {
      /* immediate: */
      int32_t iim_val;
      uint32_t uim_val;
      float fim_val;
      /* relative: */
      struct {
         uint16_t id;
         int16_t offset;
         uint16_t base;
      } array;
   };

   /* For IR3_REG_SSA, dst registers contain pointer back to the instruction
    * containing this register.
    */
   struct ir3_instruction *instr;

   /* For IR3_REG_SSA, src registers contain ptr back to assigning
    * instruction.
    *
    * For IR3_REG_ARRAY, the pointer is back to the last dependent
    * array access (although the net effect is the same, it points
    * back to a previous instruction that we depend on).
    */
   struct ir3_register *def;

   /* Pointer to another register in the instruction that must share the same
    * physical register. Each destination can be tied with one source, and
    * they must have "tied" pointing to each other.
    */
   struct ir3_register *tied;

   unsigned spill_slot, next_use;

   unsigned merge_set_offset;
   struct ir3_merge_set *merge_set;
   unsigned interval_start, interval_end;
};

/*
 * Stupid/simple growable array implementation:
 */
#define DECLARE_ARRAY(type, name)                                              \
   unsigned name##_count, name##_sz;                                           \
   type *name;

#define array_insert(ctx, arr, ...)                                            \
   do {                                                                        \
      if (arr##_count == arr##_sz) {                                           \
         arr##_sz = MAX2(2 * arr##_sz, 16);                                    \
         arr = reralloc_size(ctx, arr, arr##_sz * sizeof(arr[0]));             \
      }                                                                        \
      arr[arr##_count++] = __VA_ARGS__;                                        \
   } while (0)

typedef enum {
   REDUCE_OP_ADD_U,
   REDUCE_OP_ADD_F,
   REDUCE_OP_MUL_U,
   REDUCE_OP_MUL_F,
   REDUCE_OP_MIN_U,
   REDUCE_OP_MIN_S,
   REDUCE_OP_MIN_F,
   REDUCE_OP_MAX_U,
   REDUCE_OP_MAX_S,
   REDUCE_OP_MAX_F,
   REDUCE_OP_AND_B,
   REDUCE_OP_OR_B,
   REDUCE_OP_XOR_B,
} reduce_op_t;

typedef enum {
   ALIAS_TEX = 0,
   ALIAS_RT = 1,
   ALIAS_MEM = 2,
} ir3_alias_scope;

typedef enum {
   SHFL_XOR = 1,
   SHFL_UP = 2,
   SHFL_DOWN = 3,
   SHFL_RUP = 6,
   SHFL_RDOWN = 7,
} ir3_shfl_mode;

typedef enum ir3_instruction_flags {
   /* (sy) flag is set on first instruction, and after sample
    * instructions (probably just on RAW hazard).
    */
   IR3_INSTR_SY = BIT(0),
   /* (ss) flag is set on first instruction, and first instruction
    * to depend on the result of "long" instructions (RAW hazard):
    *
    *   rcp, rsq, log2, exp2, sin, cos, sqrt
    *
    * It seems to synchronize until all in-flight instructions are
    * completed, for example:
    *
    *   rsq hr1.w, hr1.w
    *   add.f hr2.z, (neg)hr2.z, hc0.y
    *   mul.f hr2.w, (neg)hr2.y, (neg)hr2.y
    *   rsq hr2.x, hr2.x
    *   (rpt1)nop
    *   mad.f16 hr2.w, hr2.z, hr2.z, hr2.w
    *   nop
    *   mad.f16 hr2.w, (neg)hr0.w, (neg)hr0.w, hr2.w
    *   (ss)(rpt2)mul.f hr1.x, (r)hr1.x, hr1.w
    *   (rpt2)mul.f hr0.x, (neg)(r)hr0.x, hr2.x
    *
    * The last mul.f does not have (ss) set, presumably because the
    * (ss) on the previous instruction does the job.
    *
    * The blob driver also seems to set it on WAR hazards, although
    * not really clear if this is needed or just blob compiler being
    * sloppy.  So far I haven't found a case where removing the (ss)
    * causes problems for WAR hazard, but I could just be getting
    * lucky:
    *
    *   rcp r1.y, r3.y
    *   (ss)(rpt2)mad.f32 r3.y, (r)c9.x, r1.x, (r)r3.z
    *
    */
   IR3_INSTR_SS = BIT(1),
   /* (jp) flag is set on jump targets:
    */
   IR3_INSTR_JP = BIT(2),
   /* (eq) flag kills helper invocations when they are no longer needed */
   IR3_INSTR_EQ = BIT(3),
   IR3_INSTR_UL = BIT(4),
   IR3_INSTR_3D = BIT(5),
   IR3_INSTR_A = BIT(6),
   IR3_INSTR_O = BIT(7),
   IR3_INSTR_P = BIT(8),
   IR3_INSTR_S = BIT(9),
   IR3_INSTR_S2EN = BIT(10),
   IR3_INSTR_SAT = BIT(11),
   /* (cat5/cat6) Bindless */
   IR3_INSTR_B = BIT(12),
   /* (cat5/cat6) nonuniform */
   IR3_INSTR_NONUNIF = BIT(13),
   /* (cat5-only) Get some parts of the encoding from a1.x */
   IR3_INSTR_A1EN = BIT(14),
   /* uniform destination for ldc, which must be set if and only if it has a
    * shared reg destination
    */
   IR3_INSTR_U = BIT(15),
   /* meta-flags, for intermediate stages of IR, ie.
    * before register assignment is done:
    */
   IR3_INSTR_MARK = BIT(16),

   /* Used by shared register allocation when creating spill/reload instructions
    * to inform validation that this is created by RA. This also may be set on
    * an instruction where a spill has been folded into it.
    */
   IR3_INSTR_SHARED_SPILL = IR3_INSTR_MARK,

   IR3_INSTR_UNUSED = BIT(17),

   /* Used to indicate that a mov comes from a lowered READ_FIRST/READ_COND
    * and may broadcast a helper invocation's value from a vector register to a
    * shared register that may be read by other invocations. This factors into
    * (eq) calculations.
    */
   IR3_INSTR_NEEDS_HELPERS = BIT(18),

   /* isam.v */
   IR3_INSTR_V = BIT(19),

   /* isam.1d. Note that .1d is an active-low bit. */
   IR3_INSTR_INV_1D = BIT(20),

   /* isam.v/ldib.b/stib.b can optionally use an immediate offset with one of
    * their sources.
    */
   IR3_INSTR_IMM_OFFSET = BIT(21),
} ir3_instruction_flags;

struct ir3_instruction {
   struct ir3_block *block;
   opc_t opc;
   BITMASK_ENUM(ir3_instruction_flags) flags;
   uint8_t repeat;
   uint8_t nop;
#if MESA_DEBUG
   unsigned srcs_max, dsts_max;
#endif
   unsigned srcs_count, dsts_count;
   struct ir3_register **dsts;
   struct ir3_register **srcs;
   union {
      struct {
         char inv1, inv2;
         int immed;
         struct ir3_block *target;
         const char *target_label;
         unsigned idx; /* for brac.N */
      } cat0;
      struct {
         type_t src_type, dst_type;
         round_t round;
         reduce_op_t reduce_op;
      } cat1;
      struct {
         enum {
            IR3_COND_LT = 0,
            IR3_COND_LE = 1,
            IR3_COND_GT = 2,
            IR3_COND_GE = 3,
            IR3_COND_EQ = 4,
            IR3_COND_NE = 5,
         } condition;
      } cat2;
      struct {
         enum {
            IR3_SRC_UNSIGNED = 0,
            IR3_SRC_MIXED = 1,
         } signedness;
         enum {
            IR3_SRC_PACKED_LOW = 0,
            IR3_SRC_PACKED_HIGH = 1,
         } packed;
         bool swapped;
      } cat3;
      struct {
         unsigned samp, tex;
         unsigned tex_base : 3;
         unsigned cluster_size : 4;
         type_t type;
      } cat5;
      struct {
         type_t type;
         /* TODO remove dst_offset and handle as a ir3_register
          * which might be IMMED, similar to how src_offset is
          * handled.
          */
         int dst_offset;
         int iim_val;       /* for ldgb/stgb, # of components */
         unsigned d    : 3; /* for ldc, component offset */
         bool typed    : 1;
         unsigned base : 3;
         ir3_shfl_mode shfl_mode : 3;
      } cat6;
      struct {
         unsigned w : 1; /* write */
         unsigned r : 1; /* read */
         unsigned l : 1; /* local */
         unsigned g : 1; /* global */

         ir3_alias_scope alias_scope;
         unsigned alias_table_size_minus_one;
         bool alias_type_float;
      } cat7;
      /* for meta-instructions, just used to hold extra data
       * before instruction scheduling, etc
       */
      struct {
         int off; /* component/offset */
      } split;
      struct {
         /* Per-source index back to the entry in the
          * ir3_shader_variant::outputs table.
          */
         unsigned *outidxs;
      } end;
      struct {
         /* used to temporarily hold reference to nir_phi_instr
          * until we resolve the phi srcs
          */
         void *nphi;
         unsigned comp;
      } phi;
      struct {
         unsigned samp, tex;
         unsigned input_offset;
         unsigned samp_base : 3;
         unsigned tex_base  : 3;
      } prefetch;
      struct {
         /* maps back to entry in ir3_shader_variant::inputs table: */
         int inidx;
         /* for sysvals, identifies the sysval type.  Mostly so we can
          * identify the special cases where a sysval should not be DCE'd
          * (currently, just pre-fs texture fetch)
          */
         gl_system_value sysval;
      } input;
      struct {
         unsigned src_base, src_size;
         unsigned dst_base;
      } push_consts;
      struct {
         uint64_t value;
      } raw;
   };

   /* For assigning jump offsets, we need instruction's position: */
   uint32_t ip;

   /* used for per-pass extra instruction data.
    *
    * TODO we should remove the per-pass data like this and 'use_count'
    * and do something similar to what RA does w/ ir3_ra_instr_data..
    * ie. use the ir3_count_instructions pass, and then use instr->ip
    * to index into a table of pass-private data.
    */
   void *data;

   /**
    * Valid if pass calls ir3_find_ssa_uses().. see foreach_ssa_use()
    */
   struct set *uses;

   int use_count; /* currently just updated/used by cp */

   /* an instruction can reference at most one address register amongst
    * it's src/dst registers.  Beyond that, you need to insert mov's.
    *
    * NOTE: do not write this directly, use ir3_instr_set_address()
    */
   struct ir3_register *address;

   /* Tracking for additional dependent instructions.  Used to handle
    * barriers, WAR hazards for arrays/SSBOs/etc.
    */
   DECLARE_ARRAY(struct ir3_instruction *, deps);

   /*
    * From PoV of instruction scheduling, not execution (ie. ignores global/
    * local distinction):
    *                            shared  image  atomic  SSBO  everything
    *   barrier()/            -   R/W     R/W    R/W     R/W       X
    *     groupMemoryBarrier()
    *     memoryBarrier()
    *     (but only images declared coherent?)
    *   memoryBarrierAtomic() -                  R/W
    *   memoryBarrierBuffer() -                          R/W
    *   memoryBarrierImage()  -           R/W
    *   memoryBarrierShared() -   R/W
    *
    * TODO I think for SSBO/image/shared, in cases where we can determine
    * which variable is accessed, we don't need to care about accesses to
    * different variables (unless declared coherent??)
    */
   enum {
      IR3_BARRIER_EVERYTHING = 1 << 0,
      IR3_BARRIER_SHARED_R = 1 << 1,
      IR3_BARRIER_SHARED_W = 1 << 2,
      IR3_BARRIER_IMAGE_R = 1 << 3,
      IR3_BARRIER_IMAGE_W = 1 << 4,
      IR3_BARRIER_BUFFER_R = 1 << 5,
      IR3_BARRIER_BUFFER_W = 1 << 6,
      IR3_BARRIER_ARRAY_R = 1 << 7,
      IR3_BARRIER_ARRAY_W = 1 << 8,
      IR3_BARRIER_PRIVATE_R = 1 << 9,
      IR3_BARRIER_PRIVATE_W = 1 << 10,
      IR3_BARRIER_CONST_W = 1 << 11,
      IR3_BARRIER_ACTIVE_FIBERS_R = 1 << 12,
      IR3_BARRIER_ACTIVE_FIBERS_W = 1 << 13,
   } barrier_class,
      barrier_conflict;

   /* Entry in ir3_block's instruction list: */
   struct list_head node;

   /* List of this instruction's repeat group. Vectorized NIR instructions are
    * emitted as multiple scalar instructions that are linked together using
    * this field. After RA, the ir3_combine_rpt pass iterates these groups and,
    * if the register assignment allows it, merges them into a (rptN)
    * instruction.
    *
    * NOTE: this is not a typical list as there is no empty list head. The list
    * head is stored in the first instruction of the repeat group so also refers
    * to a list entry. In order to distinguish the list's first entry, we use
    * serialno: instructions in a repeat group are always emitted consecutively
    * so the first will have the lowest serialno.
    *
    * As this is not a typical list, we have to be careful with using the
    * existing list helper. For example, using list_length on the first
    * instruction will yield one less than the number of instructions in its
    * group.
    */
   struct list_head rpt_node;

   uint32_t serialno;

   // TODO only computerator/assembler:
   int line;
};

/* Represents repeat groups in return values and arguments of the rpt builder
 * API functions.
 */
struct ir3_instruction_rpt {
   struct ir3_instruction *rpts[4];
};

struct ir3 {
   struct ir3_compiler *compiler;
   gl_shader_stage type;

   DECLARE_ARRAY(struct ir3_instruction *, inputs);

   /* Track bary.f (and ldlv) instructions.. this is needed in
    * scheduling to ensure that all varying fetches happen before
    * any potential kill instructions.  The hw gets grumpy if all
    * threads in a group are killed before the last bary.f gets
    * a chance to signal end of input (ei).
    */
   DECLARE_ARRAY(struct ir3_instruction *, baryfs);

   /* Track all indirect instructions (read and write).  To avoid
    * deadlock scenario where an address register gets scheduled,
    * but other dependent src instructions cannot be scheduled due
    * to dependency on a *different* address register value, the
    * scheduler needs to ensure that all dependencies other than
    * the instruction other than the address register are scheduled
    * before the one that writes the address register.  Having a
    * convenient list of instructions that reference some address
    * register simplifies this.
    */
   DECLARE_ARRAY(struct ir3_instruction *, a0_users);

   /* same for a1.x: */
   DECLARE_ARRAY(struct ir3_instruction *, a1_users);

   /* Track texture sample instructions which need texture state
    * patched in (for astc-srgb workaround):
    */
   DECLARE_ARRAY(struct ir3_instruction *, astc_srgb);

   /* Track tg4 instructions which need texture state patched in (for tg4
    * swizzling workaround):
    */
   DECLARE_ARRAY(struct ir3_instruction *, tg4);

   /* List of blocks: */
   struct list_head block_list;

   /* List of ir3_array's: */
   struct list_head array_list;

#if MESA_DEBUG
   unsigned block_count;
#endif
   unsigned instr_count;
};

struct ir3_array {
   struct list_head node;
   unsigned length;
   unsigned id;

   struct nir_def *r;

   /* To avoid array write's from getting DCE'd, keep track of the
    * most recent write.  Any array access depends on the most
    * recent write.  This way, nothing depends on writes after the
    * last read.  But all the writes that happen before that have
    * something depending on them
    */
   struct ir3_register *last_write;

   /* extra stuff used in RA pass: */
   unsigned base; /* base vreg name */
   unsigned reg;  /* base physical reg */
   uint16_t start_ip, end_ip;

   /* Indicates if half-precision */
   bool half;

   bool unused;
};

struct ir3_array *ir3_lookup_array(struct ir3 *ir, unsigned id);

struct ir3_block {
   struct list_head node;
   struct ir3 *shader;

   const struct nir_block *nblock;

   struct list_head instr_list; /* list of ir3_instruction */

   /* each block has either one or two successors.. in case of two
    * successors, 'condition' decides which one to follow.  A block preceding
    * an if/else has two successors.
    *
    * In some cases the path that the machine actually takes through the
    * program may not match the per-thread view of the CFG. In particular
    * this is the case for if/else, where the machine jumps from the end of
    * the if to the beginning of the else and switches active lanes. While
    * most things only care about the per-thread view, we need to use the
    * "physical" view when allocating shared registers. "successors" contains
    * the per-thread successors, and "physical_successors" contains the
    * physical successors which includes the fallthrough edge from the if to
    * the else.
    */
   struct ir3_block *successors[2];

   bool divergent_condition;

   DECLARE_ARRAY(struct ir3_block *, predecessors);
   DECLARE_ARRAY(struct ir3_block *, physical_predecessors);
   DECLARE_ARRAY(struct ir3_block *, physical_successors);

   uint16_t start_ip, end_ip;

   bool reconvergence_point;

   bool in_early_preamble;

   /* Track instructions which do not write a register but other-
    * wise must not be discarded (such as kill, stg, etc)
    */
   DECLARE_ARRAY(struct ir3_instruction *, keeps);

   /* used for per-pass extra block data.  Mainly used right
    * now in RA step to track livein/liveout.
    */
   void *data;

   uint32_t index;

   struct ir3_block *imm_dom;
   DECLARE_ARRAY(struct ir3_block *, dom_children);

   uint32_t dom_pre_index;
   uint32_t dom_post_index;

   uint32_t loop_depth;

#if MESA_DEBUG
   uint32_t serialno;
#endif
};

enum ir3_cursor_option {
   IR3_CURSOR_BEFORE_BLOCK,
   IR3_CURSOR_AFTER_BLOCK,
   IR3_CURSOR_BEFORE_INSTR,
   IR3_CURSOR_AFTER_INSTR,
};

struct ir3_cursor {
   enum ir3_cursor_option option;
   union {
      struct ir3_block *block;
      struct ir3_instruction *instr;
   };
};

struct ir3_builder {
   struct ir3_cursor cursor;
};

static inline uint32_t
block_id(struct ir3_block *block)
{
#if MESA_DEBUG
   return block->serialno;
#else
   return (uint32_t)(unsigned long)block;
#endif
}

static inline struct ir3_block *
ir3_start_block(struct ir3 *ir)
{
   return list_first_entry(&ir->block_list, struct ir3_block, node);
}

static inline struct ir3_block *
ir3_end_block(struct ir3 *ir)
{
   return list_last_entry(&ir->block_list, struct ir3_block, node);
}

struct ir3_instruction *ir3_find_end(struct ir3 *ir);

struct ir3_instruction *ir3_block_get_terminator(struct ir3_block *block);

struct ir3_instruction *ir3_block_take_terminator(struct ir3_block *block);

struct ir3_instruction *
ir3_block_get_last_non_terminator(struct ir3_block *block);

struct ir3_instruction *ir3_block_get_last_phi(struct ir3_block *block);
struct ir3_instruction *ir3_block_get_first_instr(struct ir3_block *block);

static inline struct ir3_block *
ir3_after_preamble(struct ir3 *ir)
{
   struct ir3_block *block = ir3_start_block(ir);
   /* The preamble will have a usually-empty else branch, and we want to skip
    * that to get to the block after the preamble.
    */
   struct ir3_instruction *terminator = ir3_block_get_terminator(block);
   if (terminator && (terminator->opc == OPC_SHPS))
      return block->successors[1]->successors[0];
   else
      return block;
}

static inline bool
ir3_has_preamble(struct ir3 *ir)
{
   return ir3_start_block(ir) != ir3_after_preamble(ir);
}

struct ir3_instruction *ir3_find_shpe(struct ir3 *ir);

/* Create an empty preamble and return shpe. */
struct ir3_instruction *ir3_create_empty_preamble(struct ir3 *ir);

void ir3_block_add_predecessor(struct ir3_block *block, struct ir3_block *pred);
void ir3_block_link_physical(struct ir3_block *pred, struct ir3_block *succ);
void ir3_block_remove_predecessor(struct ir3_block *block,
                                  struct ir3_block *pred);
unsigned ir3_block_get_pred_index(struct ir3_block *block,
                                  struct ir3_block *pred);

void ir3_calc_dominance(struct ir3 *ir);
bool ir3_block_dominates(struct ir3_block *a, struct ir3_block *b);

struct ir3_shader_variant;

struct ir3 *ir3_create(struct ir3_compiler *compiler,
                       struct ir3_shader_variant *v);
void ir3_destroy(struct ir3 *shader);

void ir3_collect_info(struct ir3_shader_variant *v);
void *ir3_alloc(struct ir3 *shader, int sz);

unsigned ir3_get_reg_dependent_max_waves(const struct ir3_compiler *compiler,
                                         unsigned reg_count,
                                         bool double_threadsize);

unsigned ir3_get_reg_independent_max_waves(struct ir3_shader_variant *v,
                                           bool double_threadsize);

bool ir3_should_double_threadsize(struct ir3_shader_variant *v,
                                  unsigned regs_count);

struct ir3_block *ir3_block_create(struct ir3 *shader);

struct ir3_instruction *ir3_build_instr(struct ir3_builder *builder, opc_t opc,
                                        int ndst, int nsrc);
struct ir3_instruction *ir3_instr_create_at(struct ir3_cursor cursor, opc_t opc,
                                            int ndst, int nsrc);
struct ir3_instruction *ir3_instr_create(struct ir3_block *block, opc_t opc,
                                         int ndst, int nsrc);
struct ir3_instruction *ir3_instr_create_at_end(struct ir3_block *block,
                                                opc_t opc, int ndst, int nsrc);
struct ir3_instruction *ir3_instr_clone(struct ir3_instruction *instr);
void ir3_instr_add_dep(struct ir3_instruction *instr,
                       struct ir3_instruction *dep);
const char *ir3_instr_name(struct ir3_instruction *instr);
void ir3_instr_remove(struct ir3_instruction *instr);

void ir3_instr_create_rpt(struct ir3_instruction **instrs, unsigned n);
bool ir3_instr_is_rpt(const struct ir3_instruction *instr);
bool ir3_instr_is_first_rpt(const struct ir3_instruction *instr);
struct ir3_instruction *ir3_instr_prev_rpt(const struct ir3_instruction *instr);
struct ir3_instruction *ir3_instr_first_rpt(struct ir3_instruction *instr);
unsigned ir3_instr_rpt_length(const struct ir3_instruction *instr);

struct ir3_register *ir3_src_create(struct ir3_instruction *instr, int num,
                                    int flags);
struct ir3_register *ir3_dst_create(struct ir3_instruction *instr, int num,
                                    int flags);
struct ir3_register *ir3_reg_clone(struct ir3 *shader,
                                   struct ir3_register *reg);

static inline void
ir3_reg_tie(struct ir3_register *dst, struct ir3_register *src)
{
   assert(!dst->tied && !src->tied);
   dst->tied = src;
   src->tied = dst;
}

void ir3_reg_set_last_array(struct ir3_instruction *instr,
                            struct ir3_register *reg,
                            struct ir3_register *last_write);

void ir3_instr_set_address(struct ir3_instruction *instr,
                           struct ir3_instruction *addr);

struct ir3_instruction *ir3_create_addr1(struct ir3_builder *build,
                                         unsigned const_val);

static inline bool
ir3_instr_check_mark(struct ir3_instruction *instr)
{
   if (instr->flags & IR3_INSTR_MARK)
      return true; /* already visited */
   instr->flags |= IR3_INSTR_MARK;
   return false;
}

void ir3_block_clear_mark(struct ir3_block *block);
void ir3_clear_mark(struct ir3 *shader);

unsigned ir3_count_instructions(struct ir3 *ir);
unsigned ir3_count_instructions_sched(struct ir3 *ir);
unsigned ir3_count_instructions_ra(struct ir3 *ir);

/**
 * Move 'instr' to just before 'after'
 */
static inline void
ir3_instr_move_before(struct ir3_instruction *instr,
                      struct ir3_instruction *after)
{
   list_delinit(&instr->node);
   list_addtail(&instr->node, &after->node);
}

/**
 * Move 'instr' to just after 'before':
 */
static inline void
ir3_instr_move_after(struct ir3_instruction *instr,
                     struct ir3_instruction *before)
{
   list_delinit(&instr->node);
   list_add(&instr->node, &before->node);
}

/**
 * Move 'instr' to the beginning of the block:
 */
static inline void
ir3_instr_move_before_block(struct ir3_instruction *instr,
                            struct ir3_block *block)
{
   list_delinit(&instr->node);
   list_add(&instr->node, &block->instr_list);
}

typedef bool (*use_filter_cb)(struct ir3_instruction *use, unsigned src_n);

void ir3_find_ssa_uses(struct ir3 *ir, void *mem_ctx, bool falsedeps);
void ir3_find_ssa_uses_for(struct ir3 *ir, void *mem_ctx, use_filter_cb filter);

void ir3_set_dst_type(struct ir3_instruction *instr, bool half);
void ir3_fixup_src_type(struct ir3_instruction *instr);

int ir3_flut(struct ir3_register *src_reg);

bool ir3_valid_flags(struct ir3_instruction *instr, unsigned n, unsigned flags);

bool ir3_valid_immediate(struct ir3_instruction *instr, int32_t immed);

/**
 * Given an instruction whose result we want to test for nonzero, return a
 * potentially different instruction for which the result would be the same.
 * This might be one of its sources if instr doesn't change the nonzero-ness.
 */
struct ir3_instruction *
ir3_get_cond_for_nonzero_compare(struct ir3_instruction *instr);

bool ir3_supports_rpt(struct ir3_compiler *compiler, unsigned opc);

#include "util/set.h"
#define foreach_ssa_use(__use, __instr)                                        \
   for (struct ir3_instruction *__use = (void *)~0; __use && (__instr)->uses;  \
        __use = NULL)                                                          \
      set_foreach ((__instr)->uses, __entry)                                   \
         if ((__use = (void *)__entry->key))

static inline uint32_t
reg_num(const struct ir3_register *reg)
{
   return reg->num >> 2;
}

static inline uint32_t
reg_comp(const struct ir3_register *reg)
{
   return reg->num & 0x3;
}

static inline bool
is_flow(struct ir3_instruction *instr)
{
   return (opc_cat(instr->opc) == 0);
}

static inline bool
is_terminator(struct ir3_instruction *instr)
{
   switch (instr->opc) {
   case OPC_BR:
   case OPC_JUMP:
   case OPC_BANY:
   case OPC_BALL:
   case OPC_BRAA:
   case OPC_BRAO:
   case OPC_SHPS:
   case OPC_SHPE:
   case OPC_GETONE:
   case OPC_GETLAST:
   case OPC_PREDT:
   case OPC_PREDF:
      return true;
   default:
      return false;
   }
}

static inline bool
is_kill_or_demote(struct ir3_instruction *instr)
{
   return instr->opc == OPC_KILL || instr->opc == OPC_DEMOTE;
}

static inline bool
is_nop(struct ir3_instruction *instr)
{
   return instr->opc == OPC_NOP;
}

static inline bool
is_same_type_reg(struct ir3_register *dst, struct ir3_register *src)
{
   unsigned dst_type = (dst->flags & IR3_REG_HALF);
   unsigned src_type = (src->flags & IR3_REG_HALF);

   /* Treat shared->normal copies and normal->shared copies as same-type. */
   return dst_type == src_type;
}

/* Is it a non-transformative (ie. not type changing) mov?  This can
 * also include absneg.s/absneg.f, which for the most part can be
 * treated as a mov (single src argument).
 */
static inline bool
is_same_type_mov(struct ir3_instruction *instr)
{
   struct ir3_register *dst;

   switch (instr->opc) {
   case OPC_MOV:
      if (instr->cat1.src_type != instr->cat1.dst_type)
         return false;
      /* If the type of dest reg and src reg are different,
       * it shouldn't be considered as same type mov
       */
      if (!is_same_type_reg(instr->dsts[0], instr->srcs[0]))
         return false;
      break;
   case OPC_ABSNEG_F:
   case OPC_ABSNEG_S:
      if (instr->flags & IR3_INSTR_SAT)
         return false;
      /* If the type of dest reg and src reg are different,
       * it shouldn't be considered as same type mov
       */
      if (!is_same_type_reg(instr->dsts[0], instr->srcs[0]))
         return false;
      break;
   default:
      return false;
   }

   dst = instr->dsts[0];

   /* mov's that write to a0 or p0.x are special: */
   if (dst->flags & IR3_REG_PREDICATE)
      return false;
   if (reg_num(dst) == REG_A0)
      return false;

   if (dst->flags & (IR3_REG_RELATIV | IR3_REG_ARRAY))
      return false;

   return true;
}

/* A move from const, which changes size but not type, can also be
 * folded into dest instruction in some cases.
 */
static inline bool
is_const_mov(struct ir3_instruction *instr)
{
   if (instr->opc != OPC_MOV)
      return false;

   if (!(instr->srcs[0]->flags & IR3_REG_CONST))
      return false;

   type_t src_type = instr->cat1.src_type;
   type_t dst_type = instr->cat1.dst_type;

   /* Allow a narrowing move, but not a widening one.  A narrowing
    * move from full c1.x can be folded into a hc1.x use in an ALU
    * instruction because it is doing the same thing as constant-
    * demotion.  If CONSTANT_DEMOTION_ENABLE wasn't set, we'd need
    * return false in all cases.
    */
   if ((type_size(dst_type) > type_size(src_type)) ||
       (type_size(dst_type) == 8))
      return false;

   return (type_float(src_type) && type_float(dst_type)) ||
          (type_uint(src_type) && type_uint(dst_type)) ||
          (type_sint(src_type) && type_sint(dst_type));
}

static inline bool
is_subgroup_cond_mov_macro(struct ir3_instruction *instr)
{
   switch (instr->opc) {
   case OPC_BALLOT_MACRO:
   case OPC_ANY_MACRO:
   case OPC_ALL_MACRO:
   case OPC_ELECT_MACRO:
   case OPC_READ_COND_MACRO:
   case OPC_READ_GETLAST_MACRO:
   case OPC_READ_FIRST_MACRO:
   case OPC_SCAN_MACRO:
   case OPC_SCAN_CLUSTERS_MACRO:
      return true;
   default:
      return false;
   }
}

enum ir3_subreg_move {
   IR3_SUBREG_MOVE_NONE,
   IR3_SUBREG_MOVE_LOWER,
   IR3_SUBREG_MOVE_UPPER,
};

enum ir3_subreg_move ir3_is_subreg_move(struct ir3_instruction *instr);

static inline bool
is_alu(struct ir3_instruction *instr)
{
   return (1 <= opc_cat(instr->opc)) && (opc_cat(instr->opc) <= 3);
}

static inline bool
is_sfu(struct ir3_instruction *instr)
{
   return (opc_cat(instr->opc) == 4) || instr->opc == OPC_GETFIBERID;
}

static inline bool
is_tex(struct ir3_instruction *instr)
{
   return (opc_cat(instr->opc) == 5) && instr->opc != OPC_TCINV;
}

static inline bool
is_tex_shuffle(struct ir3_instruction *instr)
{
   switch (instr->opc) {
   case OPC_BRCST_ACTIVE:
   case OPC_QUAD_SHUFFLE_BRCST:
   case OPC_QUAD_SHUFFLE_HORIZ:
   case OPC_QUAD_SHUFFLE_VERT:
   case OPC_QUAD_SHUFFLE_DIAG:
      return true;
   default:
      return false;
   }
}

static inline bool
is_tex_or_prefetch(struct ir3_instruction *instr)
{
   return is_tex(instr) || (instr->opc == OPC_META_TEX_PREFETCH);
}

static inline bool
is_mem(struct ir3_instruction *instr)
{
   return (opc_cat(instr->opc) == 6) && instr->opc != OPC_GETFIBERID;
}

static inline bool
is_barrier(struct ir3_instruction *instr)
{
   return (opc_cat(instr->opc) == 7) && instr->opc != OPC_ALIAS;
}

static inline bool
is_half(struct ir3_instruction *instr)
{
   return !!(instr->dsts[0]->flags & IR3_REG_HALF);
}

static inline bool
is_shared(struct ir3_instruction *instr)
{
   return !!(instr->dsts[0]->flags & IR3_REG_SHARED);
}

static inline bool
has_dummy_dst(struct ir3_instruction *instr)
{
   return !!(instr->dsts[0]->flags & IR3_REG_DUMMY);
}

static inline bool
is_store(struct ir3_instruction *instr)
{
   /* these instructions, the "destination" register is
    * actually a source, the address to store to.
    */
   switch (instr->opc) {
   case OPC_STG:
   case OPC_STG_A:
   case OPC_STGB:
   case OPC_STIB:
   case OPC_STP:
   case OPC_STL:
   case OPC_STLW:
   case OPC_L2G:
   case OPC_G2L:
      return true;
   default:
      return false;
   }
}

static inline bool
is_load(struct ir3_instruction *instr)
{
   switch (instr->opc) {
   case OPC_LDG:
   case OPC_LDG_A:
   case OPC_LDGB:
   case OPC_LDIB:
   case OPC_LDL:
   case OPC_LDP:
   case OPC_L2G:
   case OPC_LDLW:
   case OPC_LDLV:
   case OPC_RAY_INTERSECTION:
      /* probably some others too.. */
      return true;
   case OPC_LDC:
      return !has_dummy_dst(instr);
   default:
      return false;
   }
}

static inline bool
is_input(struct ir3_instruction *instr)
{
   /* in some cases, ldlv is used to fetch varying without
    * interpolation.. fortunately inloc is the first src
    * register in either case
    */
   switch (instr->opc) {
   case OPC_LDLV:
   case OPC_BARY_F:
   case OPC_FLAT_B:
      return true;
   default:
      return false;
   }
}

/* Whether non-helper invocations can read the value of helper invocations. We
 * cannot insert (eq) before these instructions.
 */
static inline bool
uses_helpers(struct ir3_instruction *instr)
{
   switch (instr->opc) {
   /* These require helper invocations to be present */
   case OPC_SAMB:
   case OPC_GETLOD:
   case OPC_DSX:
   case OPC_DSY:
   case OPC_DSXPP_1:
   case OPC_DSYPP_1:
   case OPC_DSXPP_MACRO:
   case OPC_DSYPP_MACRO:
   case OPC_QUAD_SHUFFLE_BRCST:
   case OPC_QUAD_SHUFFLE_HORIZ:
   case OPC_QUAD_SHUFFLE_VERT:
   case OPC_QUAD_SHUFFLE_DIAG:
   case OPC_META_TEX_PREFETCH:
      return true;

   /* sam requires helper invocations except for dummy prefetch instructions */
   case OPC_SAM:
      return !has_dummy_dst(instr);

   /* Subgroup operations don't require helper invocations to be present, but
    * will use helper invocations if they are present.
    */
   case OPC_BALLOT_MACRO:
   case OPC_ANY_MACRO:
   case OPC_ALL_MACRO:
   case OPC_READ_FIRST_MACRO:
   case OPC_READ_COND_MACRO:
   case OPC_MOVMSK:
   case OPC_BRCST_ACTIVE:
      return true;

   /* Catch lowered READ_FIRST/READ_COND. For elect, don't include the getone
    * in the preamble because it doesn't actually matter which fiber is
    * selected.
    */
   case OPC_MOV:
   case OPC_ELECT_MACRO:
      return instr->flags & IR3_INSTR_NEEDS_HELPERS;

   default:
      return false;
   }
}

static inline bool
is_bool(struct ir3_instruction *instr)
{
   switch (instr->opc) {
   case OPC_CMPS_F:
   case OPC_CMPS_S:
   case OPC_CMPS_U:
      return true;
   default:
      return false;
   }
}

static inline opc_t
cat3_half_opc(opc_t opc)
{
   switch (opc) {
   case OPC_MAD_F32:
      return OPC_MAD_F16;
   case OPC_SEL_B32:
      return OPC_SEL_B16;
   case OPC_SEL_S32:
      return OPC_SEL_S16;
   case OPC_SEL_F32:
      return OPC_SEL_F16;
   case OPC_SAD_S32:
      return OPC_SAD_S16;
   default:
      return opc;
   }
}

static inline opc_t
cat3_full_opc(opc_t opc)
{
   switch (opc) {
   case OPC_MAD_F16:
      return OPC_MAD_F32;
   case OPC_SEL_B16:
      return OPC_SEL_B32;
   case OPC_SEL_S16:
      return OPC_SEL_S32;
   case OPC_SEL_F16:
      return OPC_SEL_F32;
   case OPC_SAD_S16:
      return OPC_SAD_S32;
   default:
      return opc;
   }
}

static inline opc_t
cat4_half_opc(opc_t opc)
{
   switch (opc) {
   case OPC_RSQ:
      return OPC_HRSQ;
   case OPC_LOG2:
      return OPC_HLOG2;
   case OPC_EXP2:
      return OPC_HEXP2;
   default:
      return opc;
   }
}

static inline opc_t
cat4_full_opc(opc_t opc)
{
   switch (opc) {
   case OPC_HRSQ:
      return OPC_RSQ;
   case OPC_HLOG2:
      return OPC_LOG2;
   case OPC_HEXP2:
      return OPC_EXP2;
   default:
      return opc;
   }
}

static inline bool
is_meta(struct ir3_instruction *instr)
{
   return (opc_cat(instr->opc) == OPC_META);
}

static inline unsigned
reg_elems(const struct ir3_register *reg)
{
   if (reg->flags & IR3_REG_ARRAY)
      return reg->size;
   else
      return util_last_bit(reg->wrmask);
}

static inline unsigned
reg_elem_size(const struct ir3_register *reg)
{
   return (reg->flags & IR3_REG_HALF) ? 1 : 2;
}

static inline unsigned
reg_size(const struct ir3_register *reg)
{
   return reg_elems(reg) * reg_elem_size(reg);
}

/* Post-RA, we don't have arrays any more, so we have to be a bit careful here
 * and have to handle relative accesses specially.
 */

static inline unsigned
post_ra_reg_elems(struct ir3_register *reg)
{
   if (reg->flags & IR3_REG_RELATIV)
      return reg->size;
   return reg_elems(reg);
}

static inline unsigned
post_ra_reg_num(struct ir3_register *reg)
{
   if (reg->flags & IR3_REG_RELATIV)
      return reg->array.base;
   return reg->num;
}

static inline unsigned
dest_regs(struct ir3_instruction *instr)
{
   if (instr->dsts_count == 0)
      return 0;

   assert(instr->dsts_count == 1);
   return util_last_bit(instr->dsts[0]->wrmask);
}

static inline bool
is_reg_gpr(const struct ir3_register *reg)
{
   if (reg->flags &
       (IR3_REG_CONST | IR3_REG_IMMED | IR3_REG_PREDICATE | IR3_REG_RT)) {
      return false;
   }
   if (reg_num(reg) == REG_A0)
      return false;
   if (!(reg->flags & (IR3_REG_SSA | IR3_REG_RELATIV)) &&
       reg->num == INVALID_REG)
      return false;
   return true;
}

static inline bool
is_reg_a0(const struct ir3_register *reg)
{
   if (reg->flags & (IR3_REG_CONST | IR3_REG_IMMED))
      return false;
   return reg->num == regid(REG_A0, 0);
}

/* is dst a normal temp register: */
static inline bool
is_dest_gpr(const struct ir3_register *dst)
{
   if (dst->wrmask == 0)
      return false;
   return is_reg_gpr(dst);
}

static inline bool
writes_gpr(struct ir3_instruction *instr)
{
   if (dest_regs(instr) == 0)
      return false;
   return is_dest_gpr(instr->dsts[0]);
}

static inline bool
writes_addr0(struct ir3_instruction *instr)
{
   /* Note: only the first dest can write to a0.x */
   if (instr->dsts_count > 0) {
      struct ir3_register *dst = instr->dsts[0];
      return dst->num == regid(REG_A0, 0);
   }
   return false;
}

static inline bool
writes_addr1(struct ir3_instruction *instr)
{
   /* Note: only the first dest can write to a1.x */
   if (instr->dsts_count > 0) {
      struct ir3_register *dst = instr->dsts[0];
      return dst->num == regid(REG_A0, 1);
   }
   return false;
}

static inline bool
reads_addr0(struct ir3_instruction *instr)
{
   return instr->address && instr->address->num == regid(REG_A0, 0);
}

static inline bool
reads_addr1(struct ir3_instruction *instr)
{
   return instr->address && instr->address->num == regid(REG_A0, 1);
}

static inline bool
writes_pred(struct ir3_instruction *instr)
{
   /* Note: only the first dest can write to p0 */
   if (instr->dsts_count > 0) {
      struct ir3_register *dst = instr->dsts[0];
      return !!(dst->flags & IR3_REG_PREDICATE);
   }
   return false;
}

/* r0.x - r47.w are "normal" registers. r48.x - r55.w are shared registers.
 * Everything above those are non-GPR registers like a0.x and p0.x that aren't
 * assigned by RA.
 */
#define GPR_REG_SIZE (4 * 48)
#define SHARED_REG_START GPR_REG_SIZE
#define SHARED_REG_SIZE (4 * 8)
#define NONGPR_REG_START (SHARED_REG_START + SHARED_REG_SIZE)
#define NONGPR_REG_SIZE (4 * 8)

enum ir3_reg_file {
   IR3_FILE_FULL,
   IR3_FILE_HALF,
   IR3_FILE_SHARED,
   IR3_FILE_NONGPR,
};

/* Return a file + offset that can be used for determining if two registers
 * alias. The register is only really used for its flags, the num is taken from
 * the parameter. Registers overlap if they are in the same file and have an
 * overlapping offset. The offset is multiplied by 2 for full registers to
 * handle aliasing half and full registers, that is it's in units of half-regs.
 */
static inline unsigned
ir3_reg_file_offset(const struct ir3_register *reg, unsigned num,
                    bool mergedregs, enum ir3_reg_file *file)
{
   assert(!(reg->flags & (IR3_REG_IMMED | IR3_REG_CONST)));
   unsigned size = reg_elem_size(reg);
   if (!is_reg_gpr(reg)) {
      *file = IR3_FILE_NONGPR;
      return (num - NONGPR_REG_START) * size;
   } else if (reg->flags & IR3_REG_SHARED) {
      *file = IR3_FILE_SHARED;
      return (num - SHARED_REG_START) * size;
   } else if (mergedregs || !(reg->flags & IR3_REG_HALF)) {
      *file = IR3_FILE_FULL;
      return num * size;
   } else {
      *file = IR3_FILE_HALF;
      return num;
   }
}

/* returns defining instruction for reg */
/* TODO better name */
static inline struct ir3_instruction *
ssa(struct ir3_register *reg)
{
   if ((reg->flags & (IR3_REG_SSA | IR3_REG_ARRAY)) && reg->def)
      return reg->def->instr;
   return NULL;
}

static inline bool
conflicts(struct ir3_register *a, struct ir3_register *b)
{
   return (a && b) && (a->def != b->def);
}

static inline bool
reg_is_addr1(struct ir3_register *r)
{
   if (r->flags & (IR3_REG_CONST | IR3_REG_IMMED))
      return false;
   return r->num == regid(REG_A0, 1);
}

static inline type_t
half_type(type_t type)
{
   switch (type) {
   case TYPE_F32:
      return TYPE_F16;
   case TYPE_U32:
   case TYPE_U8_32:
      return TYPE_U16;
   case TYPE_S32:
      return TYPE_S16;
   case TYPE_F16:
   case TYPE_U16:
   case TYPE_S16:
      return type;
   case TYPE_U8:
      return type;
   default:
      assert(0);
      return (type_t)~0;
   }
}

static inline type_t
full_type(type_t type)
{
   switch (type) {
   case TYPE_F16:
      return TYPE_F32;
   case TYPE_U8:
   case TYPE_U8_32:
   case TYPE_U16:
      return TYPE_U32;
   case TYPE_S16:
      return TYPE_S32;
   case TYPE_F32:
   case TYPE_U32:
   case TYPE_S32:
      return type;
   default:
      assert(0);
      return (type_t)~0;
   }
}

/* some cat2 instructions (ie. those which are not float) can embed an
 * immediate:
 */
static inline bool
ir3_cat2_int(opc_t opc)
{
   switch (opc) {
   case OPC_ADD_U:
   case OPC_ADD_S:
   case OPC_SUB_U:
   case OPC_SUB_S:
   case OPC_CMPS_U:
   case OPC_CMPS_S:
   case OPC_MIN_U:
   case OPC_MIN_S:
   case OPC_MAX_U:
   case OPC_MAX_S:
   case OPC_CMPV_U:
   case OPC_CMPV_S:
   case OPC_MUL_U24:
   case OPC_MUL_S24:
   case OPC_MULL_U:
   case OPC_CLZ_S:
   case OPC_ABSNEG_S:
   case OPC_AND_B:
   case OPC_OR_B:
   case OPC_NOT_B:
   case OPC_XOR_B:
   case OPC_BFREV_B:
   case OPC_CLZ_B:
   case OPC_SHL_B:
   case OPC_SHR_B:
   case OPC_ASHR_B:
   case OPC_MGEN_B:
   case OPC_GETBIT_B:
   case OPC_CBITS_B:
   case OPC_BARY_F:
   case OPC_FLAT_B:
      return true;

   default:
      return false;
   }
}

/* map cat2 instruction to valid abs/neg flags: */
static inline unsigned
ir3_cat2_absneg(opc_t opc)
{
   switch (opc) {
   case OPC_ADD_F:
   case OPC_MIN_F:
   case OPC_MAX_F:
   case OPC_MUL_F:
   case OPC_SIGN_F:
   case OPC_CMPS_F:
   case OPC_ABSNEG_F:
   case OPC_CMPV_F:
   case OPC_FLOOR_F:
   case OPC_CEIL_F:
   case OPC_RNDNE_F:
   case OPC_RNDAZ_F:
   case OPC_TRUNC_F:
   case OPC_BARY_F:
      return IR3_REG_FABS | IR3_REG_FNEG;

   case OPC_ADD_U:
   case OPC_ADD_S:
   case OPC_SUB_U:
   case OPC_SUB_S:
   case OPC_CMPS_U:
   case OPC_CMPS_S:
   case OPC_MIN_U:
   case OPC_MIN_S:
   case OPC_MAX_U:
   case OPC_MAX_S:
   case OPC_CMPV_U:
   case OPC_CMPV_S:
   case OPC_MUL_U24:
   case OPC_MUL_S24:
   case OPC_MULL_U:
   case OPC_CLZ_S:
      return 0;

   case OPC_ABSNEG_S:
      return IR3_REG_SABS | IR3_REG_SNEG;

   case OPC_AND_B:
   case OPC_OR_B:
   case OPC_NOT_B:
   case OPC_XOR_B:
   case OPC_BFREV_B:
   case OPC_CLZ_B:
   case OPC_SHL_B:
   case OPC_SHR_B:
   case OPC_ASHR_B:
   case OPC_MGEN_B:
   case OPC_GETBIT_B:
   case OPC_CBITS_B:
      return IR3_REG_BNOT;

   default:
      return 0;
   }
}

/* map cat3 instructions to valid abs/neg flags: */
static inline unsigned
ir3_cat3_absneg(opc_t opc, unsigned src_n)
{
   switch (opc) {
   case OPC_MAD_F16:
   case OPC_MAD_F32:
   case OPC_SEL_F16:
   case OPC_SEL_F32:
      return IR3_REG_FNEG;

   case OPC_SAD_S16:
   case OPC_SAD_S32:
      return src_n == 1 ? IR3_REG_SNEG : 0;

   case OPC_MAD_U16:
   case OPC_MADSH_U16:
   case OPC_MAD_S16:
   case OPC_MADSH_M16:
   case OPC_MAD_U24:
   case OPC_MAD_S24:
   case OPC_SEL_S16:
   case OPC_SEL_S32:
      /* neg *may* work on 3rd src.. */

   case OPC_SEL_B16:
   case OPC_SEL_B32:

   case OPC_SHRM:
   case OPC_SHLM:
   case OPC_SHRG:
   case OPC_SHLG:
   case OPC_ANDG:
   case OPC_WMM:
   case OPC_WMM_ACCU:

   default:
      return 0;
   }
}

/* Return the type (float, int, or uint) the op uses when converting from the
 * internal result of the op (which is assumed to be the same size as the
 * sources) to the destination when they are not the same size. If F32 it does
 * a floating-point conversion, if U32 it does a truncation/zero-extension, if
 * S32 it does a truncation/sign-extension. "can_fold" will be false if it
 * doesn't do anything sensible or is unknown.
 */
static inline type_t
ir3_output_conv_type(struct ir3_instruction *instr, bool *can_fold)
{
   *can_fold = true;
   switch (instr->opc) {
   case OPC_ADD_F:
   case OPC_MUL_F:
   case OPC_BARY_F:
   case OPC_MAD_F32:
   case OPC_MAD_F16:
   case OPC_WMM:
   case OPC_WMM_ACCU:
      return TYPE_F32;

   case OPC_ADD_U:
   case OPC_SUB_U:
   case OPC_MIN_U:
   case OPC_MAX_U:
   case OPC_AND_B:
   case OPC_OR_B:
   case OPC_NOT_B:
   case OPC_XOR_B:
   case OPC_MUL_U24:
   case OPC_MULL_U:
   case OPC_SHL_B:
   case OPC_SHR_B:
   case OPC_ASHR_B:
   case OPC_MAD_U24:
   case OPC_SHRM:
   case OPC_SHLM:
   case OPC_SHRG:
   case OPC_SHLG:
   case OPC_ANDG:
   /* Comparison ops zero-extend/truncate their results, so consider them as
    * unsigned here.
    */
   case OPC_CMPS_F:
   case OPC_CMPV_F:
   case OPC_CMPS_U:
   case OPC_CMPS_S:
      return TYPE_U32;

   case OPC_ADD_S:
   case OPC_SUB_S:
   case OPC_MIN_S:
   case OPC_MAX_S:
   case OPC_ABSNEG_S:
   case OPC_MUL_S24:
   case OPC_MAD_S24:
      return TYPE_S32;

   case OPC_MOVS:
      return full_type(instr->cat1.src_type);

   /* We assume that any move->move folding that could be done was done by
    * NIR.
    */
   case OPC_MOV:
   default:
      *can_fold = false;
      return TYPE_U32;
   }
}

/* Return the src and dst types for the conversion which is already folded
 * into the op. We can assume that instr has folded in a conversion from
 * ir3_output_conv_src_type() to ir3_output_conv_dst_type(). Only makes sense
 * to call if ir3_output_conv_type() returns can_fold = true.
 */
static inline type_t
ir3_output_conv_src_type(struct ir3_instruction *instr, type_t base_type)
{
   switch (instr->opc) {
   case OPC_CMPS_F:
   case OPC_CMPV_F:
   case OPC_CMPS_U:
   case OPC_CMPS_S:
      /* Comparisons only return 0/1 and the size of the comparison sources
       * is irrelevant, never consider them as having an output conversion
       * by returning a type with the dest size here:
       */
      return (instr->dsts[0]->flags & IR3_REG_HALF) ? half_type(base_type)
                                                    : full_type(base_type);

   case OPC_BARY_F:
      /* bary.f doesn't have an explicit source, but we can assume here that
       * the varying data it reads is in fp32.
       *
       * This may be fp16 on older gen's depending on some register
       * settings, but it's probably not worth plumbing that through for a
       * small improvement that NIR would hopefully handle for us anyway.
       */
      return TYPE_F32;

   case OPC_FLAT_B:
      /* Treat the input data as u32 if not interpolating. */
      return TYPE_U32;

   default:
      return (instr->srcs[0]->flags & IR3_REG_HALF) ? half_type(base_type)
                                                    : full_type(base_type);
   }
}

static inline type_t
ir3_output_conv_dst_type(struct ir3_instruction *instr, type_t base_type)
{
   return (instr->dsts[0]->flags & IR3_REG_HALF) ? half_type(base_type)
                                                 : full_type(base_type);
}

/* Some instructions have signed/unsigned variants which are identical except
 * for whether the folded conversion sign-extends or zero-extends, and we can
 * fold in a mismatching move by rewriting the opcode. Return the opcode to
 * switch signedness, and whether one exists.
 */
static inline opc_t
ir3_try_swap_signedness(opc_t opc, bool *can_swap)
{
   switch (opc) {
#define PAIR(u, s)                                                             \
   case OPC_##u:                                                               \
      return OPC_##s;                                                          \
   case OPC_##s:                                                               \
      return OPC_##u;
      PAIR(ADD_U, ADD_S)
      PAIR(SUB_U, SUB_S)
      /* Note: these are only identical when the sources are half, but that's
       * the only case we call this function for anyway.
       */
      PAIR(MUL_U24, MUL_S24)

   default:
      *can_swap = false;
      return opc;
   }
}

#define MASK(n) ((1 << (n)) - 1)

/* iterator for an instructions's sources (reg), also returns src #: */
#define foreach_src_n(__srcreg, __n, __instr)                                  \
   if ((__instr)->srcs_count)                                                  \
      for (struct ir3_register *__srcreg = (struct ir3_register *)~0; __srcreg;\
           __srcreg = NULL)                                                    \
         for (unsigned __cnt = (__instr)->srcs_count, __n = 0; __n < __cnt;    \
              __n++)                                                           \
            if ((__srcreg = (__instr)->srcs[__n]))

/* iterator for an instructions's sources (reg): */
#define foreach_src(__srcreg, __instr) foreach_src_n (__srcreg, __i, __instr)

#define foreach_src_if(__srcreg, __instr, __filter)                            \
   foreach_src (__srcreg, __instr)                                             \
      if (__filter(__srcreg))

/* Is this either the first src in an alias group (see IR3_REG_FIRST_ALIAS) or a
 * normal src.
 */
static inline bool
ir3_src_is_first_in_group(struct ir3_register *src)
{
   return (src->flags & IR3_REG_FIRST_ALIAS) || !(src->flags & IR3_REG_ALIAS);
}

/* Iterator for an instruction's sources taking alias groups into account.
 * __src_n will hold the original source index (i.e., the index before expanding
 * collects to alias groups) while __alias_n the index within the current
 * group. Thus, the actual source index is __src_n + __alias_n.
 */
#define foreach_src_with_alias_n(__srcreg, __src_n, __alias_n, __instr)        \
   for (unsigned __src_n = -1, __alias_n = -1, __e = 0; !__e; __e = 1)         \
      foreach_src (__srcreg, __instr)                                          \
         if (__src_n += ir3_src_is_first_in_group(__srcreg) ? 1 : 0,           \
             __alias_n =                                                       \
                ir3_src_is_first_in_group(__srcreg) ? 0 : __alias_n + 1,       \
             true)

/* Iterator for all the sources in the alias group (see IR3_REG_FIRST_ALIAS)
 * starting at source index __start. __alias_n is the offset of the source
 * from the start of the alias group.
 */
#define foreach_src_in_alias_group_n(__alias, __alias_n, __instr, __start)     \
   for (struct ir3_register *__alias = __instr->srcs[__start];                 \
        __alias && (__alias->flags & IR3_REG_FIRST_ALIAS); __alias = NULL)     \
      for (unsigned __i = __start, __alias_n = 0;                              \
           __i < __instr->srcs_count &&                                        \
           (__i == __start || !ir3_src_is_first_in_group(__instr->srcs[__i])); \
           __i++, __alias_n++)                                                 \
         if ((__alias = __instr->srcs[__i]))

#define foreach_src_in_alias_group(__alias, __instr, __start)                  \
   foreach_src_in_alias_group_n (__alias, __alias_n, __instr, __start)

/* iterator for an instructions's destinations (reg), also returns dst #: */
#define foreach_dst_n(__dstreg, __n, __instr)                                  \
   if ((__instr)->dsts_count)                                                  \
      for (struct ir3_register *__dstreg = (struct ir3_register *)~0; __dstreg;\
           __dstreg = NULL)                                                    \
         for (unsigned __cnt = (__instr)->dsts_count, __n = 0; __n < __cnt;    \
              __n++)                                                           \
            if ((__dstreg = (__instr)->dsts[__n]))

/* iterator for an instructions's destinations (reg): */
#define foreach_dst(__dstreg, __instr) foreach_dst_n (__dstreg, __i, __instr)

#define foreach_dst_if(__dstreg, __instr, __filter)                            \
   foreach_dst (__dstreg, __instr)                                             \
      if (__filter(__dstreg))

static inline unsigned
__ssa_src_cnt(struct ir3_instruction *instr)
{
   return instr->srcs_count + instr->deps_count;
}

static inline bool
__is_false_dep(struct ir3_instruction *instr, unsigned n)
{
   if (n >= instr->srcs_count)
      return true;
   return false;
}

static inline struct ir3_instruction **
__ssa_srcp_n(struct ir3_instruction *instr, unsigned n)
{
   if (__is_false_dep(instr, n))
      return &instr->deps[n - instr->srcs_count];
   if (ssa(instr->srcs[n]))
      return &instr->srcs[n]->def->instr;
   return NULL;
}

#define foreach_ssa_srcp_n(__srcp, __n, __instr)                               \
   for (struct ir3_instruction **__srcp = (void *)~0; __srcp; __srcp = NULL)   \
      for (unsigned __cnt = __ssa_src_cnt(__instr), __n = 0; __n < __cnt;      \
           __n++)                                                              \
         if ((__srcp = __ssa_srcp_n(__instr, __n)))

#define foreach_ssa_srcp(__srcp, __instr)                                      \
   foreach_ssa_srcp_n (__srcp, __i, __instr)

/* iterator for an instruction's SSA sources (instr), also returns src #: */
#define foreach_ssa_src_n(__srcinst, __n, __instr)                             \
   for (struct ir3_instruction *__srcinst = (void *)~0; __srcinst;             \
        __srcinst = NULL)                                                      \
      foreach_ssa_srcp_n (__srcp, __n, __instr)                                \
         if ((__srcinst = *__srcp))

/* iterator for an instruction's SSA sources (instr): */
#define foreach_ssa_src(__srcinst, __instr)                                    \
   foreach_ssa_src_n (__srcinst, __i, __instr)

/* iterators for shader inputs: */
#define foreach_input_n(__ininstr, __cnt, __ir)                                \
   for (struct ir3_instruction *__ininstr = (void *)~0; __ininstr;             \
        __ininstr = NULL)                                                      \
      for (unsigned __cnt = 0; __cnt < (__ir)->inputs_count; __cnt++)          \
         if ((__ininstr = (__ir)->inputs[__cnt]))
#define foreach_input(__ininstr, __ir) foreach_input_n (__ininstr, __i, __ir)

/* iterators for instructions: */
#define foreach_instr(__instr, __list)                                         \
   list_for_each_entry (struct ir3_instruction, __instr, __list, node)
#define foreach_instr_from(__instr, __start, __list)                           \
   list_for_each_entry_from(struct ir3_instruction, __instr, &(__start)->node, \
                            __list, node)
#define foreach_instr_rev(__instr, __list)                                     \
   list_for_each_entry_rev (struct ir3_instruction, __instr, __list, node)
#define foreach_instr_safe(__instr, __list)                                    \
   list_for_each_entry_safe (struct ir3_instruction, __instr, __list, node)
#define foreach_instr_from_safe(__instr, __start, __list)                      \
   list_for_each_entry_from_safe(struct ir3_instruction, __instr, __start,     \
                                 __list, node)

/* Iterate over all instructions in a repeat group. */
#define foreach_instr_rpt(__rpt, __instr)                                      \
   if (assert(ir3_instr_is_first_rpt(__instr)), true)                          \
      for (struct ir3_instruction *__rpt = __instr, *__first = __instr;        \
           __first || __rpt != __instr;                                        \
           __first = NULL, __rpt =                                             \
                              list_entry(__rpt->rpt_node.next,                 \
                                         struct ir3_instruction, rpt_node))

/* Iterate over all instructions except the first one in a repeat group. */
#define foreach_instr_rpt_excl(__rpt, __instr)                                 \
   if (assert(ir3_instr_is_first_rpt(__instr)), true)                          \
      list_for_each_entry (struct ir3_instruction, __rpt, &__instr->rpt_node,  \
                           rpt_node)

#define foreach_instr_rpt_excl_safe(__rpt, __instr)                            \
   if (assert(ir3_instr_is_first_rpt(__instr)), true)                          \
      list_for_each_entry_safe (struct ir3_instruction, __rpt,                 \
                                &__instr->rpt_node, rpt_node)

/* iterators for blocks: */
#define foreach_block(__block, __list)                                         \
   list_for_each_entry (struct ir3_block, __block, __list, node)
#define foreach_block_safe(__block, __list)                                    \
   list_for_each_entry_safe (struct ir3_block, __block, __list, node)
#define foreach_block_rev(__block, __list)                                     \
   list_for_each_entry_rev (struct ir3_block, __block, __list, node)

/* iterators for arrays: */
#define foreach_array(__array, __list)                                         \
   list_for_each_entry (struct ir3_array, __array, __list, node)
#define foreach_array_safe(__array, __list)                                    \
   list_for_each_entry_safe (struct ir3_array, __array, __list, node)

#define IR3_PASS(ir, pass, ...)                                                \
   ({                                                                          \
      bool progress = pass(ir, ##__VA_ARGS__);                                 \
      if (progress) {                                                          \
         ir3_debug_print(ir, "AFTER: " #pass);                                 \
         ir3_validate(ir);                                                     \
      }                                                                        \
      progress;                                                                \
   })

/* validate: */
void ir3_validate(struct ir3 *ir);

/* dump: */
void ir3_print(struct ir3 *ir);
void ir3_print_instr(struct ir3_instruction *instr);

struct log_stream;
void ir3_print_instr_stream(struct log_stream *stream, struct ir3_instruction *instr);

/* delay calculation: */
unsigned ir3_src_read_delay(struct ir3_compiler *compiler,
                            struct ir3_instruction *instr, unsigned src_n);
int ir3_delayslots(struct ir3_compiler *compiler,
                   struct ir3_instruction *assigner,
                   struct ir3_instruction *consumer, unsigned n, bool soft);
unsigned ir3_delayslots_with_repeat(struct ir3_compiler *compiler,
                                    struct ir3_instruction *assigner,
                                    struct ir3_instruction *consumer,
                                    unsigned assigner_n, unsigned consumer_n);

/* estimated (ss)/(sy) delay calculation */

static inline bool
is_local_mem_load(struct ir3_instruction *instr)
{
   return instr->opc == OPC_LDL || instr->opc == OPC_LDLV ||
      instr->opc == OPC_LDLW;
}

bool is_scalar_alu(struct ir3_instruction *instr,
                   const struct ir3_compiler *compiler);

/* Does this instruction sometimes need (ss) to wait for its result? */
static inline bool
is_ss_producer(struct ir3_instruction *instr)
{
   foreach_dst (dst, instr) {
      if (dst->flags & IR3_REG_SHARED)
         return true;
   }

   if (instr->block->in_early_preamble && writes_addr1(instr))
      return true;

   return is_sfu(instr) || is_local_mem_load(instr) || instr->opc == OPC_SHFL;
}

static inline bool
needs_ss(const struct ir3_compiler *compiler, struct ir3_instruction *producer,
         struct ir3_instruction *consumer)
{
   if (is_scalar_alu(producer, compiler) &&
       is_scalar_alu(consumer, compiler) &&
       (producer->dsts[0]->flags & IR3_REG_HALF) ==
       (consumer->srcs[0]->flags & IR3_REG_HALF))
      return false;

   return is_ss_producer(producer);
}

static inline bool
supports_ss(struct ir3_instruction *instr)
{
   return opc_cat(instr->opc) < 5 || instr->opc == OPC_ALIAS;
}

/* The soft delay for approximating the cost of (ss). */
static inline unsigned
soft_ss_delay(struct ir3_instruction *instr)
{
   /* On a6xx, it takes the number of delay slots to get a SFU result back (ie.
    * using nop's instead of (ss) is:
    *
    *     8 - single warp
    *     9 - two warps
    *    10 - four warps
    *
    * and so on. Not quite sure where it tapers out (ie. how many warps share an
    * SFU unit). But 10 seems like a reasonable # to choose:
    */
   if (is_sfu(instr) || is_local_mem_load(instr))
      return 10;

   /* The blob adds 6 nops between shared producers and consumers, and before we
    * used (ss) this was sufficient in most cases.
    */
   return 6;
}

static inline bool
is_sy_producer(struct ir3_instruction *instr)
{
   return is_tex_or_prefetch(instr) ||
      (is_load(instr) && !is_local_mem_load(instr)) ||
      is_atomic(instr->opc);
}

static inline unsigned
soft_sy_delay(struct ir3_instruction *instr, struct ir3 *shader)
{
   /* TODO: this is just an optimistic guess, we can do better post-RA.
    */
   bool double_wavesize =
      shader->type == MESA_SHADER_FRAGMENT ||
      shader->type == MESA_SHADER_COMPUTE;

   unsigned components = reg_elems(instr->dsts[0]);

   /* These numbers come from counting the number of delay slots to get
    * cat5/cat6 results back using nops instead of (sy). Note that these numbers
    * are with the result preloaded to cache by loading it before in the same
    * shader - uncached results are much larger.
    *
    * Note: most ALU instructions can't complete at the full doubled rate, so
    * they take 2 cycles. The only exception is fp16 instructions with no
    * built-in conversions. Therefore divide the latency by 2.
    *
    * TODO: Handle this properly in the scheduler and remove this.
    */
   if (instr->opc == OPC_LDC) {
      if (double_wavesize)
         return (21 + 8 * components) / 2;
      else
         return 18 + 4 * components;
   } else if (is_tex_or_prefetch(instr)) {
      if (double_wavesize) {
         switch (components) {
         case 1: return 58 / 2;
         case 2: return 60 / 2;
         case 3: return 77 / 2;
         case 4: return 79 / 2;
         default: UNREACHABLE("bad number of components");
         }
      } else {
         switch (components) {
         case 1: return 51;
         case 2: return 53;
         case 3: return 62;
         case 4: return 64;
         default: UNREACHABLE("bad number of components");
         }
      }
   } else {
      /* TODO: measure other cat6 opcodes like ldg */
      if (double_wavesize)
         return (172 + components) / 2;
      else
         return 109 + components;
   }
}

/* Some instructions don't immediately consume their sources so may introduce a
 * WAR hazard.
 */
static inline bool
is_war_hazard_producer(struct ir3_instruction *instr)
{
   return is_tex(instr) || is_mem(instr) || is_ss_producer(instr) ||
          instr->opc == OPC_STC;
}

bool ir3_cleanup_rpt(struct ir3 *ir, struct ir3_shader_variant *v);
bool ir3_merge_rpt(struct ir3 *ir, struct ir3_shader_variant *v);
bool ir3_opt_predicates(struct ir3 *ir, struct ir3_shader_variant *v);
bool ir3_create_alias_tex_regs(struct ir3 *ir);
bool ir3_insert_alias_tex(struct ir3 *ir);
bool ir3_create_alias_rt(struct ir3 *ir, struct ir3_shader_variant *v);

/* unreachable block elimination: */
bool ir3_remove_unreachable(struct ir3 *ir);

/* calculate reconvergence information: */
void ir3_calc_reconvergence(struct ir3_shader_variant *so);

/* lower invalid shared phis after calculating reconvergence information: */
bool ir3_lower_shared_phis(struct ir3 *ir);

/* dead code elimination: */
struct ir3_shader_variant;
bool ir3_dce(struct ir3 *ir, struct ir3_shader_variant *so);

/* fp16 conversion folding */
bool ir3_cf(struct ir3 *ir, struct ir3_shader_variant *so);

/* shared mov folding */
bool ir3_shared_fold(struct ir3 *ir);

/* copy-propagate: */
bool ir3_cp(struct ir3 *ir, struct ir3_shader_variant *so,
            bool lower_imm_to_const);

/* common subexpression elimination: */
bool ir3_cse(struct ir3 *ir);

/* Make arrays SSA */
bool ir3_array_to_ssa(struct ir3 *ir);

/* Initialize immediates lowered to consts by ir3_cp in the preamble. */
bool ir3_imm_const_to_preamble(struct ir3 *ir, struct ir3_shader_variant *so);

/* scheduling: */
bool ir3_sched_add_deps(struct ir3 *ir);
int ir3_sched(struct ir3 *ir);

struct ir3_context;
bool ir3_postsched(struct ir3 *ir, struct ir3_shader_variant *v);

/* register assignment: */
int ir3_ra(struct ir3_shader_variant *v);
void ir3_ra_predicates(struct ir3_shader_variant *v);

/* lower subgroup ops: */
bool ir3_lower_subgroups(struct ir3 *ir);

/* legalize: */
bool ir3_legalize(struct ir3 *ir, struct ir3_shader_variant *so, int *max_bary);
bool ir3_legalize_relative(struct ir3 *ir);

static inline bool
ir3_has_latency_to_hide(struct ir3 *ir)
{
   /* VS/GS/TCS/TESS  co-exist with frag shader invocations, but we don't
    * know the nature of the fragment shader.  Just assume it will have
    * latency to hide:
    */
   if (ir->type != MESA_SHADER_FRAGMENT)
      return true;

   foreach_block (block, &ir->block_list) {
      foreach_instr (instr, &block->instr_list) {
         if (is_tex_or_prefetch(instr))
            return true;

         if (is_load(instr)) {
            switch (instr->opc) {
            case OPC_LDLV:
            case OPC_LDL:
            case OPC_LDLW:
               break;
            default:
               return true;
            }
         }
      }
   }

   return false;
}

/**
 * Move 'instr' to after the last phi node at the beginning of the block:
 */
static inline void
ir3_instr_move_after_phis(struct ir3_instruction *instr,
                          struct ir3_block *block)
{
   struct ir3_instruction *last_phi = ir3_block_get_last_phi(block);
   if (last_phi)
      ir3_instr_move_after(instr, last_phi);
   else
      ir3_instr_move_before_block(instr, block);
}

static inline struct ir3_block *
ir3_cursor_current_block(struct ir3_cursor cursor)
{
   switch (cursor.option) {
   case IR3_CURSOR_BEFORE_BLOCK:
   case IR3_CURSOR_AFTER_BLOCK:
      return cursor.block;
   case IR3_CURSOR_BEFORE_INSTR:
   case IR3_CURSOR_AFTER_INSTR:
      return cursor.instr->block;
   }

   UNREACHABLE("illegal cursor option");
}

static inline struct ir3_cursor
ir3_before_block(struct ir3_block *block)
{
   assert(block);
   struct ir3_cursor cursor;
   cursor.option = IR3_CURSOR_BEFORE_BLOCK;
   cursor.block = block;
   return cursor;
}

static inline struct ir3_cursor
ir3_after_block(struct ir3_block *block)
{
   assert(block);
   struct ir3_cursor cursor;
   cursor.option = IR3_CURSOR_AFTER_BLOCK;
   cursor.block = block;
   return cursor;
}

static inline struct ir3_cursor
ir3_before_instr(struct ir3_instruction *instr)
{
   assert(instr);
   struct ir3_cursor cursor;
   cursor.option = IR3_CURSOR_BEFORE_INSTR;
   cursor.instr = instr;
   return cursor;
}

static inline struct ir3_cursor
ir3_after_instr(struct ir3_instruction *instr)
{
   assert(instr);
   struct ir3_cursor cursor;
   cursor.option = IR3_CURSOR_AFTER_INSTR;
   cursor.instr = instr;
   return cursor;
}

static inline struct ir3_cursor
ir3_before_terminator(struct ir3_block *block)
{
   assert(block);
   struct ir3_instruction *terminator = ir3_block_get_terminator(block);

   if (terminator)
      return ir3_before_instr(terminator);
   return ir3_after_block(block);
}

static inline struct ir3_cursor
ir3_after_phis(struct ir3_block *block)
{
   assert(block);

   foreach_instr (instr, &block->instr_list) {
      if (instr->opc != OPC_META_PHI)
         return ir3_before_instr(instr);
   }

   return ir3_after_block(block);
}

static inline struct ir3_cursor
ir3_after_instr_and_phis(struct ir3_instruction *instr)
{
   if (instr->opc == OPC_META_PHI) {
      return ir3_after_phis(instr->block);
   } else {
      return ir3_after_instr(instr);
   }
}

static inline struct ir3_builder
ir3_builder_at(struct ir3_cursor cursor)
{
   struct ir3_builder builder;
   builder.cursor = cursor;
   return builder;
}


/* ************************************************************************* */
/* instruction helpers */

/* creates SSA src of correct type (ie. half vs full precision) */
static inline struct ir3_register *
__ssa_src(struct ir3_instruction *instr, struct ir3_instruction *src,
          unsigned flags)
{
   struct ir3_register *reg;
   flags |= src->dsts[0]->flags & (IR3_REG_HALF | IR3_REG_SHARED);
   reg = ir3_src_create(instr, INVALID_REG, IR3_REG_SSA | flags);
   reg->def = src->dsts[0];
   reg->wrmask = src->dsts[0]->wrmask;
   return reg;
}

static inline struct ir3_register *
__ssa_dst(struct ir3_instruction *instr)
{
   struct ir3_register *reg = ir3_dst_create(instr, INVALID_REG, IR3_REG_SSA);
   reg->instr = instr;
   return reg;
}

static BITMASK_ENUM(ir3_register_flags)
type_flags(type_t type)
{
   if (type_size(type) < 32)
      return IR3_REG_HALF;
   return (ir3_register_flags)0;
}

static inline struct ir3_instruction *
create_immed_typed_shared(struct ir3_builder *build, uint32_t val, type_t type,
                          bool shared)
{
   struct ir3_instruction *mov;
   ir3_register_flags flags = type_flags(type);

   mov = ir3_build_instr(build, OPC_MOV, 1, 1);
   mov->cat1.src_type = type;
   mov->cat1.dst_type = type;
   __ssa_dst(mov)->flags |= flags | (shared ? IR3_REG_SHARED : 0);
   ir3_src_create(mov, 0, IR3_REG_IMMED | flags)->uim_val = val;

   return mov;
}

static inline struct ir3_instruction *
create_immed_typed(struct ir3_builder *build, uint32_t val, type_t type)
{
   return create_immed_typed_shared(build, val, type, false);
}

static inline struct ir3_instruction *
create_immed_shared(struct ir3_builder *build, uint32_t val, bool shared)
{
   return create_immed_typed_shared(build, val, TYPE_U32, shared);
}

static inline struct ir3_instruction *
create_immed(struct ir3_builder *build, uint32_t val)
{
   return create_immed_shared(build, val, false);
}

static inline struct ir3_instruction *
create_uniform_typed(struct ir3_builder *build, unsigned n, type_t type)
{
   struct ir3_instruction *mov;
   ir3_register_flags flags = type_flags(type);

   mov = ir3_build_instr(build, OPC_MOV, 1, 1);
   mov->cat1.src_type = type;
   mov->cat1.dst_type = type;
   __ssa_dst(mov)->flags |= flags;
   ir3_src_create(mov, n, IR3_REG_CONST | flags);

   return mov;
}

static inline struct ir3_instruction *
create_uniform(struct ir3_builder *build, unsigned n)
{
   return create_uniform_typed(build, n, TYPE_F32);
}

static inline struct ir3_instruction *
create_uniform_indirect(struct ir3_builder *build, int n, type_t type,
                        struct ir3_instruction *address)
{
   struct ir3_instruction *mov;

   mov = ir3_build_instr(build, OPC_MOV, 1, 1);
   mov->cat1.src_type = type;
   mov->cat1.dst_type = type;
   __ssa_dst(mov);
   ir3_src_create(mov, 0, IR3_REG_CONST | IR3_REG_RELATIV)->array.offset = n;

   ir3_instr_set_address(mov, address);

   return mov;
}

static inline struct ir3_instruction *
ir3_MOV(struct ir3_builder *build, struct ir3_instruction *src, type_t type)
{
   struct ir3_instruction *instr = ir3_build_instr(build, OPC_MOV, 1, 1);
   ir3_register_flags flags = type_flags(type) | (src->dsts[0]->flags & IR3_REG_SHARED);

   __ssa_dst(instr)->flags |= flags;
   if (src->dsts[0]->flags & IR3_REG_ARRAY) {
      struct ir3_register *src_reg = __ssa_src(instr, src, IR3_REG_ARRAY);
      src_reg->array = src->dsts[0]->array;
   } else {
      __ssa_src(instr, src, 0);
   }
   assert(!(src->dsts[0]->flags & IR3_REG_RELATIV));
   instr->cat1.src_type = type;
   instr->cat1.dst_type = type;
   return instr;
}

static inline struct ir3_instruction_rpt
ir3_MOV_rpt(struct ir3_builder *build, unsigned nrpt,
            struct ir3_instruction_rpt src, type_t type)
{
   struct ir3_instruction_rpt dst;
   assert(nrpt <= ARRAY_SIZE(dst.rpts));

   for (unsigned rpt = 0; rpt < nrpt; ++rpt)
      dst.rpts[rpt] = ir3_MOV(build, src.rpts[rpt], type);

   ir3_instr_create_rpt(dst.rpts, nrpt);
   return dst;
}

static inline struct ir3_instruction *
ir3_COV(struct ir3_builder *build, struct ir3_instruction *src, type_t src_type,
        type_t dst_type)
{
   struct ir3_instruction *instr = ir3_build_instr(build, OPC_MOV, 1, 1);
   ir3_register_flags dst_flags = type_flags(dst_type) | (src->dsts[0]->flags & IR3_REG_SHARED);
   ASSERTED ir3_register_flags src_flags = type_flags(src_type);

   assert((src->dsts[0]->flags & IR3_REG_HALF) == src_flags);

   __ssa_dst(instr)->flags |= dst_flags;
   __ssa_src(instr, src, 0);
   instr->cat1.src_type = src_type;
   instr->cat1.dst_type = dst_type;
   assert(!(src->dsts[0]->flags & IR3_REG_ARRAY));
   return instr;
}

static inline struct ir3_instruction_rpt
ir3_COV_rpt(struct ir3_builder *build, unsigned nrpt,
            struct ir3_instruction_rpt src, type_t src_type, type_t dst_type)
{
   struct ir3_instruction_rpt dst;

   for (unsigned rpt = 0; rpt < nrpt; ++rpt)
      dst.rpts[rpt] = ir3_COV(build, src.rpts[rpt], src_type, dst_type);

   ir3_instr_create_rpt(dst.rpts, nrpt);
   return dst;
}

static inline struct ir3_instruction *
ir3_MOVS(struct ir3_builder *build, struct ir3_instruction *src,
         struct ir3_instruction *invocation, type_t type)
{
   bool use_a0 = writes_addr0(invocation);
   struct ir3_instruction *instr =
      ir3_build_instr(build, OPC_MOVS, 1, use_a0 ? 1 : 2);
   ir3_register_flags flags = type_flags(type);

   __ssa_dst(instr)->flags |= flags | IR3_REG_SHARED;
   __ssa_src(instr, src, 0);

   if (use_a0) {
      ir3_instr_set_address(instr, invocation);
   } else {
      __ssa_src(instr, invocation, 0);
   }

   instr->cat1.src_type = type;
   instr->cat1.dst_type = type;
   return instr;
}

static inline struct ir3_instruction *
ir3_MOVMSK(struct ir3_builder *build, unsigned components)
{
   struct ir3_instruction *instr = ir3_build_instr(build, OPC_MOVMSK, 1, 0);

   struct ir3_register *dst = __ssa_dst(instr);
   dst->flags |= IR3_REG_SHARED;
   dst->wrmask = (1 << components) - 1;
   instr->repeat = components - 1;
   return instr;
}

static inline struct ir3_instruction *
ir3_BALLOT_MACRO(struct ir3_builder *build, struct ir3_instruction *src,
                 unsigned components)
{
   struct ir3_instruction *instr =
      ir3_build_instr(build, OPC_BALLOT_MACRO, 1, 1);

   struct ir3_register *dst = __ssa_dst(instr);
   dst->flags |= IR3_REG_SHARED;
   dst->wrmask = (1 << components) - 1;

   __ssa_src(instr, src, 0);

   return instr;
}

struct ir3_instruction *ir3_create_collect(struct ir3_builder *build,
                                           struct ir3_instruction *const *arr,
                                           unsigned arrsz);

#define ir3_collect(build, ...)                                                \
   ({                                                                          \
      struct ir3_instruction *__arr[] = {__VA_ARGS__};                         \
      ir3_create_collect(build, __arr, ARRAY_SIZE(__arr));                     \
   })

void ir3_split_dest(struct ir3_builder *build, struct ir3_instruction **dst,
                    struct ir3_instruction *src, unsigned base, unsigned n);
struct ir3_instruction *ir3_split_off_scalar(struct ir3_builder *build,
                                             struct ir3_instruction *src,
                                             unsigned bit_size);

static inline struct ir3_instruction *
ir3_64b(struct ir3_builder *build, struct ir3_instruction *lo,
        struct ir3_instruction *hi)
{
   assert((lo->dsts[0]->flags & IR3_REG_SHARED) ==
          (hi->dsts[0]->flags & IR3_REG_SHARED));
   return ir3_collect(build, lo, hi);
}

static inline struct ir3_instruction *
ir3_64b_immed(struct ir3_builder *build, uint64_t val)
{
   return ir3_64b(build, create_immed(build, (uint32_t)val),
                  create_immed(build, val >> 32));
}

static inline struct ir3_instruction *
ir3_64b_get_lo(struct ir3_instruction *instr)
{
   assert(instr->opc == OPC_META_COLLECT && instr->srcs_count == 2);
   return instr->srcs[0]->def->instr;
}

static inline struct ir3_instruction *
ir3_64b_get_hi(struct ir3_instruction *instr)
{
   assert(instr->opc == OPC_META_COLLECT && instr->srcs_count == 2);
   return instr->srcs[1]->def->instr;
}

struct ir3_instruction *ir3_store_const(struct ir3_shader_variant *so,
                                        struct ir3_builder *build,
                                        struct ir3_instruction *src,
                                        unsigned dst);

/* clang-format off */
#define __INSTR0(flag, name, opc)                                              \
static inline struct ir3_instruction *ir3_##name(struct ir3_builder *build)    \
{                                                                              \
   struct ir3_instruction *instr = ir3_build_instr(build, opc, 1, 0);          \
   instr->flags |= flag;                                                       \
   return instr;                                                               \
}
/* clang-format on */
#define INSTR0F(f, name) __INSTR0(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR0(name)     __INSTR0((ir3_instruction_flags)0, name, OPC_##name)

/* clang-format off */
#define __INSTR1(flag, dst_count, name, opc, scalar_alu)                       \
static inline struct ir3_instruction *ir3_##name(                              \
   struct ir3_builder *build, struct ir3_instruction *a, unsigned aflags)      \
{                                                                              \
   struct ir3_instruction *instr =                                             \
      ir3_build_instr(build, opc, dst_count, 1);                               \
   unsigned dst_flag = scalar_alu ? (a->dsts[0]->flags & IR3_REG_SHARED) : 0;  \
   for (unsigned i = 0; i < dst_count; i++)                                    \
      __ssa_dst(instr)->flags |= dst_flag;                                     \
   __ssa_src(instr, a, aflags);                                                \
   instr->flags |= flag;                                                       \
   return instr;                                                               \
}                                                                              \
static inline struct ir3_instruction_rpt ir3_##name##_rpt(                     \
   struct ir3_builder *build, unsigned nrpt,                                   \
   struct ir3_instruction_rpt a, unsigned aflags)                              \
{                                                                              \
   struct ir3_instruction_rpt dst;                                             \
   assert(nrpt <= ARRAY_SIZE(dst.rpts));                                       \
   for (unsigned rpt = 0; rpt < nrpt; rpt++)                                   \
      dst.rpts[rpt] = ir3_##name(build, a.rpts[rpt], aflags);                  \
   ir3_instr_create_rpt(dst.rpts, nrpt);                                       \
   return dst;                                                                 \
}

/* clang-format on */
#define INSTR1F(f, name)  __INSTR1(IR3_INSTR_##f, 1, name##_##f, OPC_##name,   \
                                   false)
#define INSTR1(name)      __INSTR1((ir3_instruction_flags)0, 1, name, OPC_##name, false)
#define INSTR1S(name)     __INSTR1((ir3_instruction_flags)0, 1, name, OPC_##name, true)
#define INSTR1NODST(name) __INSTR1((ir3_instruction_flags)0, 0, name, OPC_##name, false)

/* clang-format off */
#define __INSTR2(flag, dst_count, name, opc, scalar_alu)                       \
static inline struct ir3_instruction *ir3_##name(                              \
   struct ir3_builder *build, struct ir3_instruction *a, unsigned aflags,      \
   struct ir3_instruction *b, unsigned bflags)                                 \
{                                                                              \
   struct ir3_instruction *instr = ir3_build_instr(build, opc, dst_count, 2);  \
   unsigned dst_flag = scalar_alu ? (a->dsts[0]->flags & b->dsts[0]->flags &   \
                                     IR3_REG_SHARED) : 0;                      \
   for (unsigned i = 0; i < dst_count; i++)                                    \
      __ssa_dst(instr)->flags |= dst_flag;                                     \
   __ssa_src(instr, a, aflags);                                                \
   __ssa_src(instr, b, bflags);                                                \
   instr->flags |= flag;                                                       \
   return instr;                                                               \
}                                                                              \
static inline struct ir3_instruction_rpt ir3_##name##_rpt(                     \
   struct ir3_builder *build, unsigned nrpt,                                   \
   struct ir3_instruction_rpt a, unsigned aflags,                              \
   struct ir3_instruction_rpt b, unsigned bflags)                              \
{                                                                              \
   struct ir3_instruction_rpt dst;                                             \
   assert(nrpt <= ARRAY_SIZE(dst.rpts));                                       \
   for (unsigned rpt = 0; rpt < nrpt; rpt++) {                                 \
      dst.rpts[rpt] = ir3_##name(build, a.rpts[rpt], aflags,                   \
                                 b.rpts[rpt], bflags);                         \
   }                                                                           \
   ir3_instr_create_rpt(dst.rpts, nrpt);                                       \
   return dst;                                                                 \
}
/* clang-format on */
#define INSTR2F(f, name)   __INSTR2(IR3_INSTR_##f, 1, name##_##f, OPC_##name,  \
                                    false)
#define INSTR2(name)       __INSTR2((ir3_instruction_flags)0, 1, name, OPC_##name, false)
#define INSTR2S(name)      __INSTR2((ir3_instruction_flags)0, 1, name, OPC_##name, true)
#define INSTR2NODST(name)  __INSTR2((ir3_instruction_flags)0, 0, name, OPC_##name, false)

/* clang-format off */
#define __INSTR3(flag, dst_count, name, opc, scalar_alu)                       \
static inline struct ir3_instruction *ir3_##name(                              \
   struct ir3_builder *build, struct ir3_instruction *a, unsigned aflags,      \
   struct ir3_instruction *b, unsigned bflags, struct ir3_instruction *c,      \
   unsigned cflags)                                                            \
{                                                                              \
   struct ir3_instruction *instr =                                             \
      ir3_build_instr(build, opc, dst_count, 3);                               \
   unsigned dst_flag = scalar_alu ? (a->dsts[0]->flags & b->dsts[0]->flags &   \
                                     c->dsts[0]->flags & IR3_REG_SHARED) : 0;  \
   for (unsigned i = 0; i < dst_count; i++)                                    \
      __ssa_dst(instr)->flags |= dst_flag;                                     \
   __ssa_src(instr, a, aflags);                                                \
   __ssa_src(instr, b, bflags);                                                \
   __ssa_src(instr, c, cflags);                                                \
   instr->flags |= flag;                                                       \
   return instr;                                                               \
}                                                                              \
static inline struct ir3_instruction_rpt ir3_##name##_rpt(                     \
   struct ir3_builder *build, unsigned nrpt,                                   \
   struct ir3_instruction_rpt a, unsigned aflags,                              \
   struct ir3_instruction_rpt b, unsigned bflags,                              \
   struct ir3_instruction_rpt c, unsigned cflags)                              \
{                                                                              \
   struct ir3_instruction_rpt dst;                                             \
   assert(nrpt <= ARRAY_SIZE(dst.rpts));                                       \
   for (unsigned rpt = 0; rpt < nrpt; rpt++) {                                 \
      dst.rpts[rpt] = ir3_##name(build, a.rpts[rpt], aflags,                   \
                                 b.rpts[rpt], bflags,                          \
                                 c.rpts[rpt], cflags);                         \
   }                                                                           \
   ir3_instr_create_rpt(dst.rpts, nrpt);                                       \
   return dst;                                                                 \
}
/* clang-format on */
#define INSTR3F(f, name)  __INSTR3(IR3_INSTR_##f, 1, name##_##f, OPC_##name,   \
                                   false)
#define INSTR3(name)      __INSTR3((ir3_instruction_flags)0, 1, name, OPC_##name, false)
#define INSTR3S(name)     __INSTR3((ir3_instruction_flags)0, 1, name, OPC_##name, true)
#define INSTR3NODST(name) __INSTR3((ir3_instruction_flags)0, 0, name, OPC_##name, false)

/* clang-format off */
#define __INSTR4(flag, dst_count, name, opc)                                   \
static inline struct ir3_instruction *ir3_##name(                              \
   struct ir3_builder *build, struct ir3_instruction *a, unsigned aflags,      \
   struct ir3_instruction *b, unsigned bflags, struct ir3_instruction *c,      \
   unsigned cflags, struct ir3_instruction *d, unsigned dflags)                \
{                                                                              \
   struct ir3_instruction *instr =                                             \
      ir3_build_instr(build, opc, dst_count, 4);                               \
   for (unsigned i = 0; i < dst_count; i++)                                    \
      __ssa_dst(instr);                                                        \
   __ssa_src(instr, a, aflags);                                                \
   __ssa_src(instr, b, bflags);                                                \
   __ssa_src(instr, c, cflags);                                                \
   __ssa_src(instr, d, dflags);                                                \
   instr->flags |= flag;                                                       \
   return instr;                                                               \
}
/* clang-format on */
#define INSTR4F(f, name)  __INSTR4(IR3_INSTR_##f, 1, name##_##f, OPC_##name)
#define INSTR4(name)      __INSTR4((ir3_instruction_flags)0, 1, name, OPC_##name)
#define INSTR4NODST(name) __INSTR4((ir3_instruction_flags)0, 0, name, OPC_##name)

/* clang-format off */
#define __INSTR5(flag, name, opc)                                              \
static inline struct ir3_instruction *ir3_##name(                              \
   struct ir3_builder *build, struct ir3_instruction *a, unsigned aflags,      \
   struct ir3_instruction *b, unsigned bflags, struct ir3_instruction *c,      \
   unsigned cflags, struct ir3_instruction *d, unsigned dflags,                \
   struct ir3_instruction *e, unsigned eflags)                                 \
{                                                                              \
   struct ir3_instruction *instr = ir3_build_instr(build, opc, 1, 5);          \
   __ssa_dst(instr);                                                           \
   __ssa_src(instr, a, aflags);                                                \
   __ssa_src(instr, b, bflags);                                                \
   __ssa_src(instr, c, cflags);                                                \
   __ssa_src(instr, d, dflags);                                                \
   __ssa_src(instr, e, eflags);                                                \
   instr->flags |= flag;                                                       \
   return instr;                                                               \
}
/* clang-format on */
#define INSTR5F(f, name) __INSTR5(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR5(name)     __INSTR5((ir3_instruction_flags)0, name, OPC_##name)

/* clang-format off */
#define __INSTR6(flag, dst_count, name, opc)                                   \
static inline struct ir3_instruction *ir3_##name(                              \
   struct ir3_builder *build, struct ir3_instruction *a, unsigned aflags,      \
   struct ir3_instruction *b, unsigned bflags, struct ir3_instruction *c,      \
   unsigned cflags, struct ir3_instruction *d, unsigned dflags,                \
   struct ir3_instruction *e, unsigned eflags, struct ir3_instruction *f,      \
   unsigned fflags)                                                            \
{                                                                              \
   struct ir3_instruction *instr = ir3_build_instr(build, opc, 1, 6);          \
   for (unsigned i = 0; i < dst_count; i++)                                    \
      __ssa_dst(instr);                                                        \
   __ssa_src(instr, a, aflags);                                                \
   __ssa_src(instr, b, bflags);                                                \
   __ssa_src(instr, c, cflags);                                                \
   __ssa_src(instr, d, dflags);                                                \
   __ssa_src(instr, e, eflags);                                                \
   __ssa_src(instr, f, fflags);                                                \
   instr->flags |= flag;                                                       \
   return instr;                                                               \
}
/* clang-format on */
#define INSTR6F(f, name)  __INSTR6(IR3_INSTR_##f, 1, name##_##f, OPC_##name)
#define INSTR6(name)      __INSTR6((ir3_instruction_flags)0, 1, name, OPC_##name)
#define INSTR6NODST(name) __INSTR6((ir3_instruction_flags)0, 0, name, OPC_##name)

/* cat0 instructions: */
INSTR0(NOP)
INSTR1NODST(BR)
INSTR1NODST(BALL)
INSTR1NODST(BANY)
INSTR2NODST(BRAA)
INSTR2NODST(BRAO)
INSTR0(JUMP)
INSTR1NODST(KILL)
INSTR1NODST(DEMOTE)
INSTR0(END)
INSTR0(CHSH)
INSTR0(CHMASK)
INSTR1NODST(PREDT)
INSTR1NODST(PREDF)
INSTR0(PREDE)
INSTR0(GETONE)
INSTR0(GETLAST)
INSTR0(SHPS)
INSTR0(SHPE)

/* cat1 macros */
INSTR1(ANY_MACRO)
INSTR1(ALL_MACRO)
INSTR1(READ_FIRST_MACRO)
INSTR2(READ_COND_MACRO)
INSTR1(READ_GETLAST_MACRO)

static inline struct ir3_instruction *
ir3_ELECT_MACRO(struct ir3_builder *build)
{
   struct ir3_instruction *instr =
      ir3_build_instr(build, OPC_ELECT_MACRO, 1, 0);
   __ssa_dst(instr);
   return instr;
}

static inline struct ir3_instruction *
ir3_SHPS_MACRO(struct ir3_builder *build)
{
   struct ir3_instruction *instr = ir3_build_instr(build, OPC_SHPS_MACRO, 1, 0);
   __ssa_dst(instr);
   return instr;
}

/* cat2 instructions, most 2 src but some 1 src: */
INSTR2S(ADD_F)
INSTR2S(MIN_F)
INSTR2S(MAX_F)
INSTR2S(MUL_F)
INSTR1S(SIGN_F)
INSTR2S(CMPS_F)
INSTR1S(ABSNEG_F)
INSTR2S(CMPV_F)
INSTR1S(FLOOR_F)
INSTR1S(CEIL_F)
INSTR1S(RNDNE_F)
INSTR1S(RNDAZ_F)
INSTR1S(TRUNC_F)
INSTR2S(ADD_U)
INSTR2S(ADD_S)
INSTR2S(SUB_U)
INSTR2S(SUB_S)
INSTR2S(CMPS_U)
INSTR2S(CMPS_S)
INSTR2S(MIN_U)
INSTR2S(MIN_S)
INSTR2S(MAX_U)
INSTR2S(MAX_S)
INSTR1S(ABSNEG_S)
INSTR2S(AND_B)
INSTR2S(OR_B)
INSTR1S(NOT_B)
INSTR2S(XOR_B)
INSTR2S(CMPV_U)
INSTR2S(CMPV_S)
INSTR2S(MUL_U24)
INSTR2S(MUL_S24)
INSTR2S(MULL_U)
INSTR1S(BFREV_B)
INSTR1S(CLZ_S)
INSTR1S(CLZ_B)
INSTR2S(SHL_B)
INSTR2S(SHR_B)
INSTR2S(ASHR_B)
INSTR2(BARY_F)
INSTR2(FLAT_B)
INSTR2S(MGEN_B)
INSTR2S(GETBIT_B)
INSTR1(SETRM)
INSTR1S(CBITS_B)
INSTR2S(SHB)
INSTR2S(MSAD)

/* cat3 instructions: */
INSTR3(MAD_U16)
INSTR3(MADSH_U16)
INSTR3(MAD_S16)
INSTR3(MADSH_M16)
INSTR3(MAD_U24)
INSTR3(MAD_S24)
INSTR3(MAD_F16)
INSTR3(MAD_F32)
INSTR3(DP2ACC)
INSTR3(DP4ACC)
/* NOTE: SEL_B32 checks for zero vs nonzero */
INSTR3S(SEL_B16)
INSTR3S(SEL_B32)
INSTR3S(SEL_S16)
INSTR3S(SEL_S32)
INSTR3S(SEL_F16)
INSTR3S(SEL_F32)
INSTR3(SAD_S16)
INSTR3(SAD_S32)
INSTR3S(SHRM)
INSTR3S(SHLM)
INSTR3S(SHRG)
INSTR3S(SHLG)
INSTR3S(ANDG)

/* cat4 instructions: */
INSTR1S(RCP)
INSTR1S(RSQ)
INSTR1S(HRSQ)
INSTR1S(LOG2)
INSTR1S(HLOG2)
INSTR1S(EXP2)
INSTR1S(HEXP2)
INSTR1S(SIN)
INSTR1S(COS)
INSTR1S(SQRT)

/* cat5 instructions: */
INSTR1(DSX)
INSTR1(DSXPP_MACRO)
INSTR1(DSY)
INSTR1(DSYPP_MACRO)
INSTR1F(3D, DSX)
INSTR1F(3D, DSY)
INSTR1(RGETPOS)

static inline struct ir3_instruction *
ir3_SAM(struct ir3_builder *build, opc_t opc, type_t type, unsigned wrmask,
        ir3_instruction_flags flags, struct ir3_instruction *samp_tex,
        struct ir3_instruction *src0, struct ir3_instruction *src1)
{
   struct ir3_instruction *sam;
   unsigned nreg = 0;

   if (flags & IR3_INSTR_S2EN) {
      nreg++;
   }
   if (src0 || opc == OPC_SAM) {
      nreg++;
   }
   if (src1) {
      nreg++;
   }

   sam = ir3_build_instr(build, opc, 1, nreg);
   sam->flags |= flags;
   __ssa_dst(sam)->wrmask = wrmask;
   if (flags & IR3_INSTR_S2EN) {
      __ssa_src(sam, samp_tex, (flags & IR3_INSTR_B) ? 0 : IR3_REG_HALF);
   }
   if (src0) {
      __ssa_src(sam, src0, 0);
   } else if (opc == OPC_SAM) {
      /* Create a dummy shared source for the coordinate, for the prefetch
       * case. It needs to be shared so that we don't accidentally disable early
       * preamble, and this is what the blob does.
       */
      ir3_src_create(sam, regid(48, 0), IR3_REG_SHARED | IR3_REG_DUMMY);
   }
   if (src1) {
      __ssa_src(sam, src1, 0);
   }
   sam->cat5.type = type;

   return sam;
}

/* brcst.active rx, ry behaves like a conditional move: rx either keeps its
 * value or is set to ry. In order to model this in SSA form, we add an extra
 * argument (the initial value of rx) and tie it to the destination.
 */
static inline struct ir3_instruction *
ir3_BRCST_ACTIVE(struct ir3_builder *build, unsigned cluster_size,
                 struct ir3_instruction *src,
                 struct ir3_instruction *dst_default)
{
   struct ir3_instruction *brcst =
      ir3_build_instr(build, OPC_BRCST_ACTIVE, 1, 2);
   brcst->cat5.cluster_size = cluster_size;
   brcst->cat5.type = TYPE_U32;
   struct ir3_register *brcst_dst = __ssa_dst(brcst);
   __ssa_src(brcst, src, 0);
   struct ir3_register *default_src = __ssa_src(brcst, dst_default, 0);
   ir3_reg_tie(brcst_dst, default_src);
   return brcst;
}

/* cat6 instructions: */
INSTR0(GETFIBERID)
INSTR2(LDLV)
INSTR3(LDG)
INSTR3(LDL)
INSTR3(LDLW)
INSTR3(LDP)
INSTR4NODST(STG)
INSTR3NODST(STL)
INSTR3NODST(STLW)
INSTR3NODST(STP)
INSTR1(RESINFO)
INSTR1(RESFMT)
INSTR2(ATOMIC_ADD)
INSTR2(ATOMIC_SUB)
INSTR2(ATOMIC_XCHG)
INSTR2(ATOMIC_INC)
INSTR2(ATOMIC_DEC)
INSTR2(ATOMIC_CMPXCHG)
INSTR2(ATOMIC_MIN)
INSTR2(ATOMIC_MAX)
INSTR2(ATOMIC_AND)
INSTR2(ATOMIC_OR)
INSTR2(ATOMIC_XOR)
INSTR2(LDC)
INSTR2(QUAD_SHUFFLE_BRCST)
INSTR1(QUAD_SHUFFLE_HORIZ)
INSTR1(QUAD_SHUFFLE_VERT)
INSTR1(QUAD_SHUFFLE_DIAG)
INSTR2NODST(LDC_K)
INSTR2NODST(STC)
INSTR2NODST(STSC)
INSTR2(SHFL)
#ifndef GPU
#elif GPU >= 600
INSTR4NODST(STIB);
INSTR3(LDIB);
INSTR5(LDG_A);
INSTR6NODST(STG_A);
INSTR2(ATOMIC_G_ADD)
INSTR2(ATOMIC_G_SUB)
INSTR2(ATOMIC_G_XCHG)
INSTR2(ATOMIC_G_INC)
INSTR2(ATOMIC_G_DEC)
INSTR2(ATOMIC_G_CMPXCHG)
INSTR2(ATOMIC_G_MIN)
INSTR2(ATOMIC_G_MAX)
INSTR2(ATOMIC_G_AND)
INSTR2(ATOMIC_G_OR)
INSTR2(ATOMIC_G_XOR)
INSTR3(ATOMIC_B_ADD)
INSTR3(ATOMIC_B_SUB)
INSTR3(ATOMIC_B_XCHG)
INSTR3(ATOMIC_B_INC)
INSTR3(ATOMIC_B_DEC)
INSTR3(ATOMIC_B_CMPXCHG)
INSTR3(ATOMIC_B_MIN)
INSTR3(ATOMIC_B_MAX)
INSTR3(ATOMIC_B_AND)
INSTR3(ATOMIC_B_OR)
INSTR3(ATOMIC_B_XOR)
#elif GPU >= 400
INSTR3(LDGB)
#if GPU >= 500
INSTR3(LDIB)
#endif
INSTR4NODST(STGB)
INSTR4NODST(STIB)
INSTR4(ATOMIC_S_ADD)
INSTR4(ATOMIC_S_SUB)
INSTR4(ATOMIC_S_XCHG)
INSTR4(ATOMIC_S_INC)
INSTR4(ATOMIC_S_DEC)
INSTR4(ATOMIC_S_CMPXCHG)
INSTR4(ATOMIC_S_MIN)
INSTR4(ATOMIC_S_MAX)
INSTR4(ATOMIC_S_AND)
INSTR4(ATOMIC_S_OR)
INSTR4(ATOMIC_S_XOR)
#endif
INSTR4NODST(LDG_K)
INSTR5(RAY_INTERSECTION)

/* cat7 instructions: */
INSTR0(BAR)
INSTR0(FENCE)
INSTR0(CCINV)

/* ************************************************************************* */
#include "util/bitset.h"

#define MAX_REG 256

typedef BITSET_DECLARE(fullstate_t, 2 * GPR_REG_SIZE);
typedef BITSET_DECLARE(halfstate_t, GPR_REG_SIZE);
typedef BITSET_DECLARE(sharedstate_t, 2 * SHARED_REG_SIZE);
typedef BITSET_DECLARE(nongprstate_t, 2 * NONGPR_REG_SIZE);

typedef struct {
   bool mergedregs;
   fullstate_t full;
   halfstate_t half;
   sharedstate_t shared;
   nongprstate_t nongpr;
} regmask_t;

static inline BITSET_WORD *
__regmask_file(regmask_t *regmask, enum ir3_reg_file file)
{
   switch (file) {
   case IR3_FILE_FULL:
      return regmask->full;
   case IR3_FILE_HALF:
      return regmask->half;
   case IR3_FILE_SHARED:
      return regmask->shared;
   case IR3_FILE_NONGPR:
      return regmask->nongpr;
   }
   UNREACHABLE("bad file");
}

static inline bool
__regmask_get(regmask_t *regmask, enum ir3_reg_file file, unsigned n, unsigned size)
{
   BITSET_WORD *regs = __regmask_file(regmask, file);
   for (unsigned i = 0; i < size; i++) {
      if (BITSET_TEST(regs, n + i))
         return true;
   }
   return false;
}

static inline void
__regmask_set(regmask_t *regmask, enum ir3_reg_file file, unsigned n, unsigned size)
{
   BITSET_WORD *regs = __regmask_file(regmask, file);
   for (unsigned i = 0; i < size; i++)
      BITSET_SET(regs, n + i);
}

static inline void
__regmask_clear(regmask_t *regmask, enum ir3_reg_file file, unsigned n, unsigned size)
{
   BITSET_WORD *regs = __regmask_file(regmask, file);
   for (unsigned i = 0; i < size; i++)
      BITSET_CLEAR(regs, n + i);
}

static inline void
regmask_init(regmask_t *regmask, bool mergedregs)
{
   memset(regmask, 0, sizeof(*regmask));
   regmask->mergedregs = mergedregs;
}

static inline void
regmask_or(regmask_t *dst, regmask_t *a, regmask_t *b)
{
   assert(dst->mergedregs == a->mergedregs);
   assert(dst->mergedregs == b->mergedregs);

   for (unsigned i = 0; i < ARRAY_SIZE(dst->full); i++)
      dst->full[i] = a->full[i] | b->full[i];
   for (unsigned i = 0; i < ARRAY_SIZE(dst->half); i++)
      dst->half[i] = a->half[i] | b->half[i];
   for (unsigned i = 0; i < ARRAY_SIZE(dst->shared); i++)
      dst->shared[i] = a->shared[i] | b->shared[i];
   for (unsigned i = 0; i < ARRAY_SIZE(dst->nongpr); i++)
      dst->nongpr[i] = a->nongpr[i] | b->nongpr[i];
}

static inline void
regmask_or_shared(regmask_t *dst, regmask_t *a, regmask_t *b)
{
   for (unsigned i = 0; i < ARRAY_SIZE(dst->shared); i++)
      dst->shared[i] = a->shared[i] | b->shared[i];
}

static inline void
regmask_set(regmask_t *regmask, struct ir3_register *reg)
{
   unsigned size = reg_elem_size(reg);
   enum ir3_reg_file file;
   unsigned num = post_ra_reg_num(reg);
   unsigned n = ir3_reg_file_offset(reg, num, regmask->mergedregs, &file);
   if (reg->flags & IR3_REG_RELATIV) {
      __regmask_set(regmask, file, n, size * reg->size);
   } else {
      for (unsigned mask = reg->wrmask; mask; mask >>= 1, n += size)
         if (mask & 1)
            __regmask_set(regmask, file, n, size);
   }
}

static inline void
regmask_clear(regmask_t *regmask, struct ir3_register *reg)
{
   unsigned size = reg_elem_size(reg);
   enum ir3_reg_file file;
   unsigned num = post_ra_reg_num(reg);
   unsigned n = ir3_reg_file_offset(reg, num, regmask->mergedregs, &file);
   if (reg->flags & IR3_REG_RELATIV) {
      __regmask_clear(regmask, file, n, size * reg->size);
   } else {
      for (unsigned mask = reg->wrmask; mask; mask >>= 1, n += size)
         if (mask & 1)
            __regmask_clear(regmask, file, n, size);
   }
}

static inline bool
regmask_get(regmask_t *regmask, struct ir3_register *reg)
{
   unsigned size = reg_elem_size(reg);
   enum ir3_reg_file file;
   unsigned num = post_ra_reg_num(reg);
   unsigned n = ir3_reg_file_offset(reg, num, regmask->mergedregs, &file);
   if (reg->flags & IR3_REG_RELATIV) {
      return __regmask_get(regmask, file, n, size * reg->size);
   } else {
      for (unsigned mask = reg->wrmask; mask; mask >>= 1, n += size)
         if (mask & 1)
            if (__regmask_get(regmask, file, n, size))
               return true;
   }
   return false;
}
/* ************************************************************************* */

struct ir3_nop_state {
   unsigned full_ready[GPR_REG_SIZE];
   unsigned half_ready[GPR_REG_SIZE];
};

struct ir3_legalize_state {
   regmask_t needs_ss;
   regmask_t needs_ss_scalar_full; /* half scalar ALU producer -> full scalar ALU consumer */
   regmask_t needs_ss_scalar_half; /* full scalar ALU producer -> half scalar ALU consumer */
   regmask_t needs_ss_war; /* write after read */
   regmask_t needs_sy_war; /* WAR that can only be resolved using (sy) */
   regmask_t needs_ss_or_sy_war;  /* WAR for sy-producer sources */
   regmask_t needs_ss_scalar_war; /* scalar ALU write -> ALU write */
   regmask_t needs_ss_or_sy_scalar_war;
   regmask_t needs_sy;
   bool needs_ss_for_const;
   bool needs_sy_for_const;

   /* Next instruction needs (ss)/(sy), no matter its dsts/srcs. */
   bool force_ss;
   bool force_sy;

   /* Each of these arrays contains the cycle when the corresponding register
    * becomes "ready" i.e. does not require any more nops. There is a special
    * mechanism to let ALU instructions read compatible (i.e. same halfness)
    * destinations of another ALU instruction with less delay, so this can
    * depend on what type the consuming instruction is, which is why there are
    * multiple arrays. The cycle is counted relative to the start of the block.
    */

   /* When ALU instructions reading the given full/half register will be ready.
    */
   struct ir3_nop_state alu_nop;

   /* When non-ALU (e.g. cat5) instructions reading the given full/half register
    * will be ready.
    */
   struct ir3_nop_state non_alu_nop;

   /* When p0.x-w, a0.x, and a1.x are ready. */
   unsigned pred_ready[4];
   unsigned addr_ready[2];

   unsigned cycle;
};

typedef struct ir3_legalize_state *(*ir3_get_block_legalize_state_cb)(
   struct ir3_block *);

void ir3_init_legalize_state(struct ir3_legalize_state *state,
                             struct ir3_compiler *compiler);
void ir3_merge_pred_legalize_states(struct ir3_legalize_state *state,
                                    struct ir3_block *block,
                                    ir3_get_block_legalize_state_cb get_state);
void ir3_update_legalize_state(struct ir3_legalize_state *state,
                               struct ir3_compiler *compiler,
                               struct ir3_instruction *n);
enum ir3_instruction_flags
ir3_required_sync_flags(struct ir3_legalize_state *state,
                        struct ir3_compiler *compiler,
                        struct ir3_instruction *n);
unsigned ir3_required_delay(struct ir3_legalize_state *state,
                            struct ir3_compiler *compiler,
                            struct ir3_instruction *instr);

#endif /* IR3_H_ */
