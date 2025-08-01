/*
 * Copyright (C) 2020 Collabora Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#ifndef __BIFROST_COMPILER_H
#define __BIFROST_COMPILER_H

#include "compiler/nir/nir.h"
#include "panfrost/util/pan_ir.h"
#include "util/half_float.h"
#include "util/shader_stats.h"
#include "util/u_math.h"
#include "util/u_worklist.h"
#include "bi_opcodes.h"
#include "bifrost.h"
#include "valhall_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Swizzles across bytes in a 32-bit word. Expresses swz in the XML directly.
 * To express widen, use the correpsonding replicated form, i.e. H01 = identity
 * for widen = none, H00 for widen = h0, B1111 for widen = b1. For lane, also
 * use the replicated form (interpretation is governed by the opcode). For
 * 8-bit lanes with two channels, use replicated forms for replicated forms.
 * For 8-bit lanes with four channels using matching form.
 */

enum bi_swizzle {
   /* 16-bit swizzles, ordered sequentially for fast compute */
   BI_SWIZZLE_H00 = 0,
   BI_SWIZZLE_H01 = 1,
   BI_SWIZZLE_H10 = 2,
   BI_SWIZZLE_H11 = 3,

   /* 8-bit swizzle equivalents */
   BI_SWIZZLE_B0101 = BI_SWIZZLE_H00,
   BI_SWIZZLE_B0123 = BI_SWIZZLE_H01,
   BI_SWIZZLE_B2301 = BI_SWIZZLE_H10,
   BI_SWIZZLE_B2323 = BI_SWIZZLE_H11,

   /* 8-bit replication swizzles, ordered sequentially for fast compute */
   BI_SWIZZLE_B0000 = 4, /* single channel (replicate) */
   BI_SWIZZLE_B1111 = 5,
   BI_SWIZZLE_B2222 = 6,
   BI_SWIZZLE_B3333 = 7,

   /* remaining 8-bit swizzles in arbitrary order */
   BI_SWIZZLE_B0011 = 8,  /* +SWZ.v4i8 */
   BI_SWIZZLE_B2233 = 9,  /* +SWZ.v4i8 */
   BI_SWIZZLE_B1032 = 10, /* +SWZ.v4i8 */
   BI_SWIZZLE_B3210 = 11, /* +SWZ.v4i8 */

   /* 8-bit swizzles that only exist in HW as 8-bit half swizzles */
   BI_SWIZZLE_B0022 = 12,
   BI_SWIZZLE_B1100 = 13,
   BI_SWIZZLE_B2200 = 14,
   BI_SWIZZLE_B3300 = 15,
   BI_SWIZZLE_B2211 = 16,
   BI_SWIZZLE_B3311 = 17,
   BI_SWIZZLE_B1122 = 18,
   BI_SWIZZLE_B3322 = 19,
   BI_SWIZZLE_B0033 = 20,
   BI_SWIZZLE_B1133 = 21,
   BI_SWIZZLE_B1123 = 22,

   /* 16-bit single-lane, values ordered sequentially */
   BI_SWIZZLE_H0 = BI_SWIZZLE_H00,
   BI_SWIZZLE_H1 = BI_SWIZZLE_H11,

   /* 8-bit single-lane, values order sequentially */
   BI_SWIZZLE_B0 = BI_SWIZZLE_B0000,
   BI_SWIZZLE_B1 = BI_SWIZZLE_B1111,
   BI_SWIZZLE_B2 = BI_SWIZZLE_B2222,
   BI_SWIZZLE_B3 = BI_SWIZZLE_B3333,

   /* 8-bit half-swizzle
    *
    * Values for replication are sequential. Other half-swizzles have
    * arbitrary value ordering. */
   BI_SWIZZLE_B00 = BI_SWIZZLE_B0000,
   BI_SWIZZLE_B10 = BI_SWIZZLE_B1100,
   BI_SWIZZLE_B20 = BI_SWIZZLE_B2200,
   BI_SWIZZLE_B30 = BI_SWIZZLE_B3300,
   BI_SWIZZLE_B01 = BI_SWIZZLE_B0011,
   BI_SWIZZLE_B11 = BI_SWIZZLE_B1111,
   BI_SWIZZLE_B21 = BI_SWIZZLE_B2211,
   BI_SWIZZLE_B31 = BI_SWIZZLE_B3311,
   BI_SWIZZLE_B02 = BI_SWIZZLE_B0022,
   BI_SWIZZLE_B12 = BI_SWIZZLE_B1122,
   BI_SWIZZLE_B22 = BI_SWIZZLE_B2222,
   BI_SWIZZLE_B32 = BI_SWIZZLE_B3322,
   BI_SWIZZLE_B03 = BI_SWIZZLE_B0033,
   BI_SWIZZLE_B13 = BI_SWIZZLE_B1133,
   BI_SWIZZLE_B23 = BI_SWIZZLE_B2233,
   BI_SWIZZLE_B33 = BI_SWIZZLE_B3333,
};

/* Given a packed i16vec2/i8vec4 constant, apply a swizzle. Useful for constant
 * folding and Valhall constant optimization. */

static inline uint32_t
bi_apply_swizzle(uint32_t value, enum bi_swizzle swz)
{
   const uint8_t *b = (const uint8_t *)&value;

#define B(b0, b1, b2, b3)                                                      \
   case BI_SWIZZLE_B##b0##b1##b2##b3:                                          \
      return (b[b0] | ((uint32_t)b[b1] << 8) | ((uint32_t)b[b2] << 16) |       \
              ((uint32_t)b[b3] << 24));

   switch (swz) {
      B(0, 1, 0, 1);
      B(0, 1, 2, 3);
      B(2, 3, 0, 1);
      B(2, 3, 2, 3);
      B(0, 0, 0, 0);
      B(1, 1, 1, 1);
      B(2, 2, 2, 2);
      B(3, 3, 3, 3);
      B(0, 0, 1, 1);
      B(2, 2, 3, 3);
      B(1, 0, 3, 2);
      B(3, 2, 1, 0);
      B(0, 0, 2, 2);
      B(1, 1, 0, 0);
      B(2, 2, 0, 0);
      B(3, 3, 0, 0);
      B(2, 2, 1, 1);
      B(3, 3, 1, 1);
      B(1, 1, 2, 2);
      B(3, 3, 2, 2);
      B(0, 0, 3, 3);
      B(1, 1, 3, 3);
      B(1, 1, 2, 3);
   }

#undef H
#undef B

   UNREACHABLE("Invalid swizzle");
}

enum bi_index_type {
   BI_INDEX_NULL = 0,
   BI_INDEX_NORMAL = 1,
   BI_INDEX_REGISTER = 2,
   BI_INDEX_CONSTANT = 3,
   BI_INDEX_PASS = 4,
   BI_INDEX_FAU = 5
};

typedef struct {
   uint32_t value;

   /* modifiers, should only be set if applicable for a given instruction.
    * For *IDP.v4i8, abs plays the role of sign. For bitwise ops where
    * applicable, neg plays the role of not */
   bool abs : 1;
   bool neg : 1;

   /* The last use of a value, should be purged from the register cache.
    * Set by liveness analysis. */
   bool discard : 1;

   /* For a source, the swizzle. For a destination, acts a bit like a
    * write mask. Identity for the full 32-bit, H00 for only caring about
    * the lower half, other values unused. */
   enum bi_swizzle swizzle : 5;
   uint32_t offset         : 3;
   enum bi_index_type type : 3;

   /* Last use of an SSA value; similar to discard, but applies to the
    * SSA analysis and does not have any HW restrictions (discard gets
    * sent to the hardware eventually. */
   bool kill_ssa : 1;

   /* Register class */
   bool memory : 1;

   /* Must be zeroed so we can hash the whole 64-bits at a time */
   unsigned padding : (32 - 16);
} bi_index;

static inline bi_index
bi_get_index(unsigned value)
{
   return (bi_index){
      .value = value,
      .swizzle = BI_SWIZZLE_H01,
      .type = BI_INDEX_NORMAL,
   };
}

enum ra_class {
   /* General purpose register */
   RA_GPR,

   /* Memory, used to assign stack slots */
   RA_MEM,

   /* Keep last */
   RA_CLASSES,
};

static inline enum ra_class
ra_class_for_index(bi_index idx)
{
   return idx.memory ? RA_MEM : RA_GPR;
}

static inline bi_index
bi_register(unsigned reg)
{
   assert(reg < 64);

   return (bi_index){
      .value = reg,
      .swizzle = BI_SWIZZLE_H01,
      .type = BI_INDEX_REGISTER,
   };
}

static inline bi_index
bi_imm_u32(uint32_t imm)
{
   return (bi_index){
      .value = imm,
      .swizzle = BI_SWIZZLE_H01,
      .type = BI_INDEX_CONSTANT,
   };
}

static inline bi_index
bi_imm_f32(float imm)
{
   return bi_imm_u32(fui(imm));
}

static inline bi_index
bi_null()
{
   return (bi_index){.type = BI_INDEX_NULL};
}

static inline bi_index
bi_zero()
{
   return bi_imm_u32(0);
}

static inline bi_index
bi_passthrough(enum bifrost_packed_src value)
{
   return (bi_index){
      .value = value,
      .swizzle = BI_SWIZZLE_H01,
      .type = BI_INDEX_PASS,
   };
}

/* Helps construct swizzles */
static inline bi_index
bi_swz_16(bi_index idx, bool x, bool y)
{
   assert(idx.swizzle == BI_SWIZZLE_H01);
   idx.swizzle = (enum bi_swizzle)(BI_SWIZZLE_H00 | (x << 1) | y);
   return idx;
}

static inline bi_index
bi_half(bi_index idx, bool upper)
{
   return bi_swz_16(idx, upper, upper);
}

static inline bi_index
bi_byte(bi_index idx, unsigned lane)
{
   assert(idx.swizzle == BI_SWIZZLE_B0123);
   assert(lane < 4);
   idx.swizzle = (enum bi_swizzle)(BI_SWIZZLE_B0 + lane);
   return idx;
}

static inline bi_index
bi_abs(bi_index idx)
{
   idx.abs = true;
   return idx;
}

static inline bi_index
bi_neg(bi_index idx)
{
   idx.neg ^= true;
   return idx;
}

static inline bi_index
bi_discard(bi_index idx)
{
   idx.discard = true;
   return idx;
}

/* Additive identity in IEEE 754 arithmetic */
static inline bi_index
bi_negzero()
{
   return bi_neg(bi_zero());
}

/* Replaces an index, preserving any modifiers */

static inline bi_index
bi_replace_index(bi_index old, bi_index replacement)
{
   replacement.abs = old.abs;
   replacement.neg = old.neg;
   replacement.swizzle = old.swizzle;
   replacement.discard = false; /* needs liveness analysis to set */
   return replacement;
}

/* Remove any modifiers. This has the property:
 *
 *     replace_index(x, strip_index(x)) = x
 *
 * This ensures it is suitable to use when lowering sources to moves */

static inline bi_index
bi_strip_index(bi_index index)
{
   index.abs = index.neg = false;
   index.swizzle = BI_SWIZZLE_H01;
   return index;
}

/* For bitwise instructions */
#define bi_not(x) bi_neg(x)

static inline bi_index
bi_imm_u8(uint8_t imm)
{
   return bi_byte(bi_imm_u32(imm), 0);
}

static inline bi_index
bi_imm_u16(uint16_t imm)
{
   return bi_half(bi_imm_u32(imm), false);
}

static inline bi_index
bi_imm_uintN(uint32_t imm, unsigned sz)
{
   assert(sz == 8 || sz == 16 || sz == 32);
   return (sz == 8)    ? bi_imm_u8(imm)
          : (sz == 16) ? bi_imm_u16(imm)
                       : bi_imm_u32(imm);
}

static inline bi_index
bi_imm_f16(float imm)
{
   return bi_imm_u16(_mesa_float_to_half(imm));
}

static inline bool
bi_is_null(bi_index idx)
{
   return idx.type == BI_INDEX_NULL;
}

static inline bool
bi_is_ssa(bi_index idx)
{
   return idx.type == BI_INDEX_NORMAL;
}

static inline bool
bi_is_zero(const bi_index idx)
{
   return idx.type == BI_INDEX_CONSTANT && idx.value == 0;
}

/* Compares equivalence as references. Does not compare offsets, swizzles, or
 * modifiers. In other words, this forms bi_index equivalence classes by
 * partitioning memory. E.g. -abs(foo[1].yx) == foo.xy but foo != bar */

static inline bool
bi_is_equiv(bi_index left, bi_index right)
{
   return (left.type == right.type) && (left.value == right.value);
}

/* A stronger equivalence relation that requires the indices access the
 * same offset, useful for RA/scheduling to see what registers will
 * correspond to */

static inline bool
bi_is_word_equiv(bi_index left, bi_index right)
{
   return bi_is_equiv(left, right) && left.offset == right.offset;
}

/* An even stronger equivalence that checks if indices correspond to the
 * right value when evaluated
 */
static inline bool
bi_is_value_equiv(bi_index left, bi_index right)
{
   if (left.type == BI_INDEX_CONSTANT && right.type == BI_INDEX_CONSTANT) {
      return (bi_apply_swizzle(left.value, left.swizzle) ==
              bi_apply_swizzle(right.value, right.swizzle)) &&
             (left.abs == right.abs) && (left.neg == right.neg);
   } else {
      return (left.value == right.value) && (left.abs == right.abs) &&
             (left.neg == right.neg) && (left.swizzle == right.swizzle) &&
             (left.offset == right.offset) && (left.type == right.type);
   }
}

#define BI_MAX_VEC   8
#define BI_MAX_DESTS 4
#define BI_MAX_SRCS  8

typedef struct {
   /* Must be first */
   struct list_head link;
   bi_index *dest;
   bi_index *src;

   enum bi_opcode op;
   uint8_t nr_srcs;
   uint8_t nr_dests;

   union {
      /* For a branch */
      struct bi_block *branch_target;

      /* For a phi node that hasn't been translated yet. This is only
       * used during NIR->BIR
       */
      nir_phi_instr *phi;
   };

   /* These don't fit neatly with anything else.. */
   enum bi_register_format register_format;
   enum bi_vecsize vecsize;

   /* Flow control associated with a Valhall instruction */
   uint8_t flow;

   /* Valhall-only property to relax waits on read-only resources */
   bool wait_resource;

   /* Slot associated with a message-passing instruction */
   uint8_t slot;

   /* Can we spill the value written here? Used to prevent
    * useless double fills */
   bool no_spill;

   /* On Bifrost: A value of bi_table to override the table, inducing a
    * DTSEL_IMM pair if nonzero.
    *
    * On Valhall: the table index to use for resource instructions.
    *
    * These two interpretations are equivalent if you squint a bit.
    */
   unsigned table;

   /* Everything after this MUST NOT be accessed directly, since
    * interpretation depends on opcodes */

   /* Destination modifiers */
   union {
      enum bi_clamp clamp;
      bool saturate;
      bool not_result;
      unsigned dest_mod;
   };

   /* Immediates. All seen alone in an instruction, except for varying/texture
    * which are specified jointly for VARTEX */
   union {
      uint32_t shift;
      uint32_t fill;
      uint32_t index;
      uint32_t attribute_index;

      struct {
         uint32_t varying_index;
         uint32_t sampler_index;
         uint32_t texture_index;
      };

      /* TEXC, ATOM_CX: # of staging registers used */
      struct {
         uint32_t sr_count;
         uint32_t sr_count_2;

         union {
            /* Atomics effectively require all three */
            int32_t byte_offset;

            /* BLEND requires all three */
            int32_t branch_offset;
         };
      };
   };

   /* Modifiers specific to particular instructions are thrown in a union */
   union {
      enum bi_adj adj;           /* FEXP_TABLE.u4 */
      enum bi_atom_opc atom_opc; /* atomics */
      enum bi_func func;         /* FPOW_SC_DET */
      enum bi_function function; /* LD_VAR_FLAT */
      enum bi_mux mux;           /* MUX */
      enum bi_sem sem;           /* FMAX, FMIN */
      enum bi_source source;     /* LD_GCLK */
      bool scale;                /* VN_ASST2, FSINCOS_OFFSET */
      bool offset;               /* FSIN_TABLE, FOCS_TABLE */
      bool mask;                 /* CLZ */
      bool threads;              /* IMULD, IMOV_FMA */
      bool combine;              /* BRANCHC */
      bool format;               /* LEA_TEX */
      bool z_stencil;            /* LD_TILE */
      bool scheduling_barrier;   /* NOP */

      struct {
         enum bi_special special;   /* FADD_RSCALE, FMA_RSCALE */
         enum bi_round round;       /* FMA, converts, FADD, _RSCALE, etc */
         bool ftz;                  /* Flush-to-zero for F16_TO_F32 and FLUSH */
         enum va_nan_mode nan_mode; /* NaN flush mode, for FLUSH */
         bool flush_inf;            /* Flush infinity to finite, for FLUSH */
      };

      struct {
         enum bi_result_type result_type; /* FCMP, ICMP */
         enum bi_cmpf cmpf;               /* CSEL, FCMP, ICMP, BRANCH */
      };

      struct {
         enum bi_stack_mode stack_mode; /* JUMP_EX */
         bool test_mode;
      };

      struct {
         enum bi_seg seg;       /* LOAD, STORE, SEG_ADD, SEG_SUB */
         bool preserve_null;    /* SEG_ADD, SEG_SUB */
         enum bi_extend extend; /* LOAD, IMUL */
      };

      struct {
         enum bi_sample sample;             /* VAR_TEX, LD_VAR */
         enum bi_update update;             /* VAR_TEX, LD_VAR */
         enum bi_varying_name varying_name; /* LD_VAR_SPECIAL */
         bool skip;                         /* VAR_TEX, TEXS, TEXC */
         bool lod_mode; /* VAR_TEX, TEXS, implicitly for TEXC */
         enum bi_source_format source_format; /* LD_VAR_BUF */

         /* Used for valhall texturing */
         bool shadow;
         bool wide_indices;
         bool texel_offset;
         bool array_enable;
         bool integer_coordinates;
         bool derivative_enable;
         bool force_delta_enable;
         bool lod_bias_disable;
         bool lod_clamp_disable;
         enum bi_fetch_component fetch_component;
         enum bi_va_lod_mode va_lod_mode;
         enum bi_dimension dimension;
         enum bi_write_mask write_mask;
      };

      /* Maximum size, for hashing */
      unsigned flags[14];

      struct {
         enum bi_subgroup subgroup;               /* WMASK, CLPER */
         enum bi_inactive_result inactive_result; /* CLPER */
         enum bi_lane_op lane_op;                 /* CLPER */
      };

      struct {
         bool z;       /* ZS_EMIT */
         bool stencil; /* ZS_EMIT */
      };

      struct {
         bool h; /* VN_ASST1.f16 */
         bool l; /* VN_ASST1.f16 */
      };

      struct {
         bool bytes2; /* RROT_DOUBLE, FRSHIFT_DOUBLE */
         bool result_word;
         bool arithmetic; /* ARSHIFT_OR */
      };

      struct {
         bool sqrt; /* FREXPM */
         bool log;  /* FREXPM */
      };

      struct {
         enum bi_mode mode;           /* FLOG_TABLE */
         enum bi_precision precision; /* FLOG_TABLE */
         bool divzero;                /* FRSQ_APPROX, FRSQ */
      };
   };
} bi_instr;

/*
 * Helpers to set opcode and to get properties related to
 * the opcode. In principle this would allow different
 * properties to be used based on the architecture. In practice
 * we've unified the valhall/bifrost descriptions so this
 * isn't necessary now. We may want it for a future architecture
 * though.
 */
static inline void
bi_set_opcode(bi_instr *I, enum bi_opcode opc)
{
   I->op = opc;
}

static inline struct bi_op_props *
bi_get_opcode_props(const bi_instr *I)
{
   return &bi_opcode_props[I->op];
}

static inline bool
bi_is_staging_src(const bi_instr *I, unsigned s)
{
   return (s == 0 || s == 4) && bi_get_opcode_props(I)->sr_read;
}

static inline bool
bi_is_scheduling_barrier(const bi_instr *I)
{
   return I->op == BI_OPCODE_NOP && I->scheduling_barrier;
}

/*
 * Safe helpers to remove destinations/sources at the end of the
 * destination/source array when changing opcodes. Unlike adding
 * sources/destinations, this does not require reallocation.
 */
static inline void
bi_drop_dests(bi_instr *I, unsigned new_count)
{
   assert(new_count < I->nr_dests);

   for (unsigned i = new_count; i < I->nr_dests; ++i)
      I->dest[i] = bi_null();

   I->nr_dests = new_count;
}

static inline void
bi_drop_srcs(bi_instr *I, unsigned new_count)
{
   assert(new_count < I->nr_srcs);

   for (unsigned i = new_count; i < I->nr_srcs; ++i)
      I->src[i] = bi_null();

   I->nr_srcs = new_count;
}

static inline void
bi_replace_src(bi_instr *I, unsigned src_index, bi_index replacement)
{
   I->src[src_index] = bi_replace_index(I->src[src_index], replacement);
}

/* Represents the assignment of slots for a given bi_tuple */

typedef struct {
   /* Register to assign to each slot */
   unsigned slot[4];

   /* Read slots can be disabled */
   bool enabled[2];

   /* Configuration for slots 2/3 */
   struct bifrost_reg_ctrl_23 slot23;

   /* Fast-Access-Uniform RAM index */
   uint8_t fau_idx;

   /* Whether writes are actually for the last instruction */
   bool first_instruction;
} bi_registers;

/* A bi_tuple contains two paired instruction pointers. If a slot is unfilled,
 * leave it NULL; the emitter will fill in a nop. Instructions reference
 * registers via slots which are assigned per tuple.
 */

typedef struct {
   uint8_t fau_idx;
   bi_registers regs;
   bi_instr *fma;
   bi_instr *add;
} bi_tuple;

struct bi_block;

typedef struct {
   struct list_head link;

   /* Link back up for branch calculations */
   struct bi_block *block;

   /* Architectural limit of 8 tuples/clause */
   unsigned tuple_count;
   bi_tuple tuples[8];

   /* For scoreboarding -- the clause ID (this is not globally unique!)
    * and its dependencies in terms of other clauses, computed during
    * scheduling and used when emitting code. Dependencies expressed as a
    * bitfield matching the hardware, except shifted by a clause (the
    * shift back to the ISA's off-by-one encoding is worked out when
    * emitting clauses) */
   unsigned scoreboard_id;
   uint8_t dependencies;

   /* See ISA header for description */
   enum bifrost_flow flow_control;

   /* Can we prefetch the next clause? Usually it makes sense, except for
    * clauses ending in unconditional branches */
   bool next_clause_prefetch;

   /* Assigned data register */
   unsigned staging_register;

   /* Corresponds to the usual bit but shifted by a clause */
   bool staging_barrier;

   /* Constants read by this clause. ISA limit. Must satisfy:
    *
    *      constant_count + tuple_count <= 13
    *
    * Also implicitly constant_count <= tuple_count since a tuple only
    * reads a single constant.
    */
   uint64_t constants[8];
   unsigned constant_count;

   /* Index of a constant to be PC-relative */
   unsigned pcrel_idx;

   /* Branches encode a constant offset relative to the program counter
    * with some magic flags. By convention, if there is a branch, its
    * constant will be last. Set this flag to indicate this is required.
    */
   bool branch_constant;

   /* Unique in a clause */
   enum bifrost_message_type message_type;
   bi_instr *message;

   /* Discard helper threads */
   bool td;

   /* Should flush-to-zero mode be enabled for this clause? */
   bool ftz;
} bi_clause;

#define BI_NUM_SLOTS 8

/* A model for the state of the scoreboard */
struct bi_scoreboard_state {
   /** Bitmap of registers read/written by a slot */
   uint64_t read[BI_NUM_SLOTS];
   uint64_t write[BI_NUM_SLOTS];

   /* Nonregister dependencies present by a slot */
   uint8_t varying : BI_NUM_SLOTS;
   uint8_t memory : BI_NUM_SLOTS;
};

typedef struct bi_block {
   /* Link to next block. Must be first for mir_get_block */
   struct list_head link;

   /* List of instructions emitted for the current block */
   struct list_head instructions;

   /* Index of the block in source order */
   unsigned index;

   /* Control flow graph */
   struct bi_block *successors[2];
   struct util_dynarray predecessors;
   bool unconditional_jumps;
   bool loop_header;

   /* Per 32-bit word live masks for the block indexed by node */
   uint8_t *live_in;
   uint8_t *live_out;

   /* Scalar liveness indexed by SSA index */
   BITSET_WORD *ssa_live_in;
   BITSET_WORD *ssa_live_out;

   /* If true, uses clauses; if false, uses instructions */
   bool scheduled;
   struct list_head clauses; /* list of bi_clause */

   /* Post-RA liveness */
   uint64_t reg_live_in, reg_live_out;

   /* Scoreboard state at the start/end of block */
   struct bi_scoreboard_state scoreboard_in, scoreboard_out;

   /* On Valhall, indicates we need a terminal NOP to implement jumps to
    * the end of the shader.
    */
   bool needs_nop;

   /* Flags available for pass-internal use */
   uint8_t pass_flags;
} bi_block;

static inline unsigned
bi_num_successors(bi_block *block)
{
   STATIC_ASSERT(ARRAY_SIZE(block->successors) == 2);
   assert(block->successors[0] || !block->successors[1]);

   if (block->successors[1])
      return 2;
   else if (block->successors[0])
      return 1;
   else
      return 0;
}

static inline unsigned
bi_num_predecessors(bi_block *block)
{
   return util_dynarray_num_elements(&block->predecessors, bi_block *);
}

static inline bi_block *
bi_start_block(struct list_head *blocks)
{
   bi_block *first = list_first_entry(blocks, bi_block, link);
   assert(bi_num_predecessors(first) == 0);
   return first;
}

static inline bi_block *
bi_exit_block(struct list_head *blocks)
{
   bi_block *last = list_last_entry(blocks, bi_block, link);
   assert(bi_num_successors(last) == 0);
   return last;
}

static inline void
bi_block_add_successor(bi_block *block, bi_block *successor)
{
   assert(block != NULL && successor != NULL);

   /* Cull impossible edges */
   if (block->unconditional_jumps)
      return;

   for (unsigned i = 0; i < ARRAY_SIZE(block->successors); ++i) {
      if (block->successors[i]) {
         if (block->successors[i] == successor)
            return;
         else
            continue;
      }

      block->successors[i] = successor;
      util_dynarray_append(&successor->predecessors, bi_block *, block);
      return;
   }

   UNREACHABLE("Too many successors");
}

/* Subset of pan_shader_info needed per-variant, in order to support IDVS */
struct bi_shader_info {
   struct pan_ubo_push *push;
   struct bifrost_shader_info *bifrost;
   struct pan_stats stats;
   unsigned tls_size;
   unsigned work_reg_count;
   unsigned push_offset;
   bool has_ld_gclk_instr;
};

/* State of index-driven vertex shading for current shader */
enum bi_idvs_mode {
   /* IDVS not in use */
   BI_IDVS_NONE = 0,

   /* IDVS in use. Compiling a position shader */
   BI_IDVS_POSITION = 1,

   /* IDVS in use. Compiling a varying shader */
   BI_IDVS_VARYING = 2,

   /* IDVS2 in use. Compiling a deferred shader (v12+) */
   BI_IDVS_ALL = 3,
};

#define BI_MAX_REGS 64

typedef struct {
   const struct pan_compile_inputs *inputs;
   nir_shader *nir;
   struct bi_shader_info info;
   gl_shader_stage stage;
   struct list_head blocks; /* list of bi_block */
   uint32_t quirks;
   unsigned arch;
   enum bi_idvs_mode idvs;
   unsigned num_blocks;

   /* Floating point rounding mode controls */
   bool rtz_fp16;
   bool rtz_fp32;
   bool ftz_fp32;

   /* In any graphics shader, whether the "IDVS with memory
    * allocation" flow is used. This affects how varyings are loaded and
    * stored. Ignore for compute.
    */
   bool malloc_idvs;

   /* During NIR->BIR */
   bi_block *current_block;
   bi_block *after_block;
   bi_block *break_block;
   bi_block *continue_block;
   bi_block **indexed_nir_blocks;
   bool emitted_atest;

   /* During NIR->BIR, the coverage bitmap. If this is NULL, the default
    * coverage bitmap should be source from preloaded register r60. This is
    * written by ATEST and ZS_EMIT
    */
   bi_index coverage;

   /* During NIR->BIR, table of preloaded registers, or NULL if never
    * preloaded.
    */
   bi_index preloaded[BI_MAX_REGS];

   /* For creating temporaries */
   unsigned ssa_alloc;
   unsigned reg_alloc;

   /* Mask of UBOs that need to be uploaded */
   uint32_t ubo_mask;

   /* During instruction selection, map from vector bi_index to its scalar
    * components, populated by a split.
    */
   struct hash_table_u64 *allocated_vec;

   /* Beginning of our stack allocation used for spilling, below that is
    * NIR-level scratch.
    */
   unsigned spill_base_B;

   /* Beginning of stack allocation used for parallel copy lowering */
   bool has_spill_pcopy_reserved;
   unsigned spill_pcopy_base;

   /* Stats for shader-db */
   unsigned loop_count;
   unsigned spills;
   unsigned fills;
} bi_context;

static inline enum bi_round
bi_round_mode(bi_context *ctx, unsigned bit_size)
{
   assert(bit_size == 16 || bit_size == 32);
   bool rtz = bit_size == 16 ? ctx->rtz_fp16 : ctx->rtz_fp32;
   return rtz ? BI_ROUND_RTZ : BI_ROUND_NONE;
}

static inline void
bi_remove_instruction(bi_instr *ins)
{
   list_del(&ins->link);
}

enum bir_fau {
   BIR_FAU_ZERO = 0,
   BIR_FAU_LANE_ID = 1,
   BIR_FAU_WARP_ID = 2,
   BIR_FAU_CORE_ID = 3,
   BIR_FAU_FB_EXTENT = 4,
   BIR_FAU_ATEST_PARAM = 5,
   BIR_FAU_SAMPLE_POS_ARRAY = 6,
   BIR_FAU_BLEND_0 = 8,
   /* blend descs 1 - 7 */
   BIR_FAU_TYPE_MASK = 15,

   /* Valhall only */
   BIR_FAU_TLS_PTR = 16,
   BIR_FAU_WLS_PTR = 17,
   BIR_FAU_PROGRAM_COUNTER = 18,

   /* Avalon only */
   BIR_FAU_SHADER_OUTPUT = (1 << 9),

   BIR_FAU_UNIFORM = (1 << 7),
   /* Look up table on Valhall */
   BIR_FAU_IMMEDIATE = (1 << 8),

};

static inline bi_index
bi_fau(enum bir_fau value, bool hi)
{
   return (bi_index){
      .value = value,
      .swizzle = BI_SWIZZLE_H01,
      .offset = hi ? 1u : 0u,
      .type = BI_INDEX_FAU,
   };
}

/*
 * Builder for Valhall LUT entries. Generally, constants are modeled with
 * BI_INDEX_IMMEDIATE in the intermediate representation. This helper is only
 * necessary for passes running after lowering constants, as well as when
 * lowering constants.
 *
 */
static inline bi_index
va_lut(unsigned index)
{
   return bi_fau((enum bir_fau)(BIR_FAU_IMMEDIATE | (index >> 1)), index & 1);
}

/*
 * va_lut_zero is like bi_zero but only works on Valhall. It is intended for
 * use by late passes that run after constants are lowered, specifically
 * register allocation. bi_zero() is preferred where possible.
 */
static inline bi_index
va_zero_lut()
{
   return va_lut(0);
}

static inline bi_index
bi_temp(bi_context *ctx)
{
   return bi_get_index(ctx->ssa_alloc++);
}

static inline bi_index
bi_def_index(nir_def *def)
{
   return bi_get_index(def->index);
}

/* Inline constants automatically, will be lowered out by bi_lower_fau where a
 * constant is not allowed. load_const_to_scalar gaurantees that this makes
 * sense */

static inline bi_index
bi_src_index(nir_src *src)
{
   if (nir_src_is_const(*src) && nir_src_bit_size(*src) <= 32) {
      return bi_imm_u32(nir_src_as_uint(*src));
   } else {
      return bi_def_index(src->ssa);
   }
}

/* Iterators for Bifrost IR */

#define bi_foreach_block(ctx, v)                                               \
   list_for_each_entry(bi_block, v, &ctx->blocks, link)

#define bi_foreach_block_rev(ctx, v)                                           \
   list_for_each_entry_rev(bi_block, v, &ctx->blocks, link)

#define bi_foreach_block_from(ctx, from, v)                                    \
   list_for_each_entry_from(bi_block, v, from, &ctx->blocks, link)

#define bi_foreach_block_from_rev(ctx, from, v)                                \
   list_for_each_entry_from_rev(bi_block, v, from, &ctx->blocks, link)

#define bi_foreach_instr_in_block(block, v)                                    \
   list_for_each_entry(bi_instr, v, &(block)->instructions, link)

#define bi_foreach_instr_in_block_rev(block, v)                                \
   list_for_each_entry_rev(bi_instr, v, &(block)->instructions, link)

#define bi_foreach_instr_in_block_safe(block, v)                               \
   list_for_each_entry_safe(bi_instr, v, &(block)->instructions, link)

#define bi_foreach_instr_in_block_safe_rev(block, v)                           \
   list_for_each_entry_safe_rev(bi_instr, v, &(block)->instructions, link)

#define bi_foreach_instr_in_block_from(block, v, from)                         \
   list_for_each_entry_from(bi_instr, v, from, &(block)->instructions, link)

#define bi_foreach_instr_in_block_from_rev(block, v, from)                     \
   list_for_each_entry_from_rev(bi_instr, v, from, &(block)->instructions, link)

#define bi_foreach_clause_in_block(block, v)                                   \
   list_for_each_entry(bi_clause, v, &(block)->clauses, link)

#define bi_foreach_clause_in_block_rev(block, v)                               \
   list_for_each_entry_rev(bi_clause, v, &(block)->clauses, link)

#define bi_foreach_clause_in_block_safe(block, v)                              \
   list_for_each_entry_safe(bi_clause, v, &(block)->clauses, link)

#define bi_foreach_clause_in_block_from(block, v, from)                        \
   list_for_each_entry_from(bi_clause, v, from, &(block)->clauses, link)

#define bi_foreach_clause_in_block_from_rev(block, v, from)                    \
   list_for_each_entry_from_rev(bi_clause, v, from, &(block)->clauses, link)

#define bi_foreach_instr_global(ctx, v)                                        \
   bi_foreach_block(ctx, v_block)                                              \
      bi_foreach_instr_in_block(v_block, v)

#define bi_foreach_instr_global_rev(ctx, v)                                    \
   bi_foreach_block_rev(ctx, v_block)                                          \
      bi_foreach_instr_in_block_rev(v_block, v)

#define bi_foreach_instr_global_safe(ctx, v)                                   \
   bi_foreach_block(ctx, v_block)                                              \
      bi_foreach_instr_in_block_safe(v_block, v)

#define bi_foreach_instr_global_rev_safe(ctx, v)                               \
   bi_foreach_block_rev(ctx, v_block)                                          \
   bi_foreach_instr_in_block_rev_safe(v_block, v)

#define bi_foreach_instr_in_tuple(tuple, v)                                    \
   for (bi_instr *v = (tuple)->fma ?: (tuple)->add; v != NULL;                 \
        v = (v == (tuple)->add) ? NULL : (tuple)->add)

#define bi_foreach_successor(blk, v)                                           \
   bi_block *v;                                                                \
   bi_block **_v;                                                              \
   for (_v = &blk->successors[0], v = *_v;                                     \
        v != NULL && _v < &blk->successors[2]; _v++, v = *_v)

#define bi_foreach_predecessor(blk, v)                                         \
   util_dynarray_foreach(&(blk)->predecessors, bi_block *, v)

#define bi_foreach_src(ins, v) for (unsigned v = 0; v < ins->nr_srcs; ++v)
#define bi_foreach_src_rev(ins, v) for (signed v = ins->nr_srcs-1; v >= 0; --v)

#define bi_foreach_dest(ins, v) for (unsigned v = 0; v < ins->nr_dests; ++v)
#define bi_foreach_dest_rev(ins, v) for (signed v = ins->nr_dests-1; v >= 0; --v)

#define bi_foreach_ssa_src(ins, v)                                             \
   bi_foreach_src(ins, v)                                                      \
      if (ins->src[v].type == BI_INDEX_NORMAL)

#define bi_foreach_ssa_src_rev(ins, v)                                         \
   bi_foreach_src_rev(ins, v)                                                  \
      if (ins->src[v].type == BI_INDEX_NORMAL)

#define bi_foreach_ssa_dest(ins, v)                                            \
   bi_foreach_dest(ins, v)                                                     \
      if (ins->dest[v].type == BI_INDEX_NORMAL)

#define bi_foreach_instr_and_src_in_tuple(tuple, ins, s)                       \
   bi_foreach_instr_in_tuple(tuple, ins)                                       \
      bi_foreach_src(ins, s)

#define bi_foreach_ssa_dest_rev(ins, v)                                        \
   bi_foreach_dest_rev(ins, v)                                                 \
      if (ins->dest[v].type == BI_INDEX_NORMAL)

/* Phis only come at the start (after else instructions) so we stop as soon as
 * we hit a non-phi
 */
#define bi_foreach_phi_in_block(block, v)                                      \
   bi_foreach_instr_in_block(block, v)                                        \
      if (v->op != BI_OPCODE_PHI)                                              \
         break;                                                                \
      else

#define bi_foreach_phi_in_block_safe(block, v)                                 \
   bi_foreach_instr_in_block_safe(block, v)                                    \
      if (v->op != BI_OPCODE_PHI)                                             \
         break;                                                                \
      else

/*
 * Find the index of a predecessor, used as the implicit order of phi sources.
 */
static inline unsigned
bi_predecessor_index(bi_block *succ, bi_block *pred)
{
   unsigned index = 0;

   bi_foreach_predecessor(succ, x) {
      if (*x == pred)
         return index;

      index++;
   }

   UNREACHABLE("Invalid predecessor");
}

static inline bi_instr *
bi_prev_op(bi_instr *ins)
{
   return list_last_entry(&(ins->link), bi_instr, link);
}

static inline bi_instr *
bi_next_op(bi_instr *ins)
{
   return list_first_entry(&(ins->link), bi_instr, link);
}

static inline bi_block *
bi_next_block(bi_block *block)
{
   return list_first_entry(&(block->link), bi_block, link);
}

static inline bi_block *
bi_entry_block(bi_context *ctx)
{
   return list_first_entry(&ctx->blocks, bi_block, link);
}

/* BIR manipulation */

bool bi_has_arg(const bi_instr *ins, bi_index arg);
unsigned bi_count_read_registers(const bi_instr *ins, unsigned src);
unsigned bi_count_write_registers(const bi_instr *ins, unsigned dest);
bool bi_is_regfmt_16(enum bi_register_format fmt);
unsigned bi_writemask(const bi_instr *ins, unsigned dest);
bi_clause *bi_next_clause(bi_context *ctx, bi_block *block, bi_clause *clause);
bool bi_side_effects(const bi_instr *I);
bool bi_reconverge_branches(bi_block *block);

bool bi_can_replace_with_csel(bi_instr *I);

void bi_print_instr(const bi_instr *I, FILE *fp);
void bi_print_slots(bi_registers *regs, FILE *fp);
void bi_print_tuple(bi_tuple *tuple, FILE *fp);
void bi_print_clause(bi_clause *clause, FILE *fp);
void bi_print_block(bi_block *block, FILE *fp);
void bi_print_shader(bi_context *ctx, FILE *fp);

/* BIR passes */

bool bi_instr_uses_helpers(bi_instr *I);
bool bi_block_terminates_helpers(bi_block *block);
void bi_analyze_helper_terminate(bi_context *ctx);
void bi_mark_clauses_td(bi_context *ctx);

void bi_analyze_helper_requirements(bi_context *ctx);
void bi_opt_copy_prop(bi_context *ctx);
void bi_opt_dce(bi_context *ctx, bool partial);
void bi_opt_cse(bi_context *ctx);
void bi_opt_mod_prop_forward(bi_context *ctx);
void bi_opt_mod_prop_backward(bi_context *ctx);
void bi_opt_fuse_dual_texture(bi_context *ctx);
void bi_opt_dce_post_ra(bi_context *ctx);
void bi_opt_message_preload(bi_context *ctx);
void bi_opt_push_ubo(bi_context *ctx);
void bi_opt_reorder_push(bi_context *ctx);
void bi_lower_swizzle(bi_context *ctx);
void bi_lower_fau(bi_context *ctx);
void bi_assign_scoreboard(bi_context *ctx);
void bi_register_allocate(bi_context *ctx);
void va_optimize(bi_context *ctx);
void va_lower_split_64bit(bi_context *ctx);

void bi_lower_opt_instructions(bi_context *ctx);

void bi_pressure_schedule(bi_context *ctx);
void bi_schedule(bi_context *ctx);
bool bi_can_fma(bi_instr *ins);
bool bi_can_add(bi_instr *ins);
bool bi_must_message(bi_instr *ins);
bool bi_reads_zero(bi_instr *ins);
bool bi_reads_temps(bi_instr *ins, unsigned src);
bool bi_reads_t(bi_instr *ins, unsigned src);

#ifndef NDEBUG
bool bi_validate_initialization(bi_context *ctx);
void bi_validate(bi_context *ctx, const char *after_str);
#else
static inline bool
bi_validate_initialization(UNUSED bi_context *ctx)
{
   return true;
}
static inline void
bi_validate(UNUSED bi_context *ctx, UNUSED const char *after_str)
{
   return;
}
#endif

uint32_t bi_fold_constant(bi_instr *I, bool *unsupported);
bool bi_opt_constant_fold(bi_context *ctx);

/* Liveness */

void bi_compute_liveness_ssa(bi_context *ctx);
void bi_liveness_ins_update_ssa(BITSET_WORD *live, const bi_instr *ins);

unsigned bi_calc_register_demand(bi_context *ctx);

void bi_postra_liveness(bi_context *ctx);
uint64_t MUST_CHECK bi_postra_liveness_ins(uint64_t live, bi_instr *ins);

/* SSA spilling; returns number of spilled registers */
unsigned bi_spill_ssa(bi_context *ctx, unsigned num_registers, unsigned tls_size);

/* Layout */

signed bi_block_offset(bi_context *ctx, bi_clause *start, bi_block *target);
bool bi_ec0_packed(unsigned tuple_count);

/* Check if there are no more instructions starting with a given block, this
 * needs to recurse in case a shader ends with multiple empty blocks */

static inline bool
bi_is_terminal_block(bi_block *block)
{
   return (block == NULL) || (list_is_empty(&block->instructions) &&
                              bi_is_terminal_block(block->successors[0]) &&
                              bi_is_terminal_block(block->successors[1]));
}

/* Code emit */

/* Returns the size of the final clause */
unsigned bi_pack(bi_context *ctx, struct util_dynarray *emission);
void bi_pack_valhall(bi_context *ctx, struct util_dynarray *emission);

struct bi_packed_tuple {
   uint64_t lo;
   uint64_t hi;
};

uint8_t bi_pack_literal(enum bi_clause_subword literal);

uint8_t bi_pack_upper(enum bi_clause_subword upper,
                      struct bi_packed_tuple *tuples,
                      ASSERTED unsigned tuple_count);
uint64_t bi_pack_tuple_bits(enum bi_clause_subword idx,
                            struct bi_packed_tuple *tuples,
                            ASSERTED unsigned tuple_count, unsigned offset,
                            unsigned nbits);

uint8_t bi_pack_sync(enum bi_clause_subword t1, enum bi_clause_subword t2,
                     enum bi_clause_subword t3, struct bi_packed_tuple *tuples,
                     ASSERTED unsigned tuple_count, bool z);

void bi_pack_format(struct util_dynarray *emission, unsigned index,
                    struct bi_packed_tuple *tuples,
                    ASSERTED unsigned tuple_count, uint64_t header,
                    uint64_t ec0, unsigned m0, bool z);

unsigned bi_pack_fma(bi_instr *I, enum bifrost_packed_src src0,
                     enum bifrost_packed_src src1, enum bifrost_packed_src src2,
                     enum bifrost_packed_src src3);
unsigned bi_pack_add(bi_instr *I, enum bifrost_packed_src src0,
                     enum bifrost_packed_src src1, enum bifrost_packed_src src2,
                     enum bifrost_packed_src src3);

/* Like in NIR, for use with the builder */

enum bi_cursor_option {
   bi_cursor_after_block,
   bi_cursor_before_instr,
   bi_cursor_after_instr
};

typedef struct {
   enum bi_cursor_option option;

   union {
      bi_block *block;
      bi_instr *instr;
   };
} bi_cursor;

static inline bi_cursor
bi_after_block(bi_block *block)
{
   return (bi_cursor){.option = bi_cursor_after_block, .block = block};
}

static inline bi_cursor
bi_before_instr(bi_instr *instr)
{
   return (bi_cursor){.option = bi_cursor_before_instr, .instr = instr};
}

static inline bi_cursor
bi_after_instr(bi_instr *instr)
{
   return (bi_cursor){.option = bi_cursor_after_instr, .instr = instr};
}

static inline bi_cursor
bi_after_block_logical(bi_block *block)
{
   if (list_is_empty(&block->instructions))
      return bi_after_block(block);

   bi_instr *last = list_last_entry(&block->instructions, bi_instr, link);
   assert(last != NULL);

   if (last->branch_target)
      return bi_before_instr(last);
   else
      return bi_after_block(block);
}

static inline bi_cursor
bi_before_nonempty_block(bi_block *block)
{
   bi_instr *I = list_first_entry(&block->instructions, bi_instr, link);
   assert(I != NULL);

   return bi_before_instr(I);
}

static inline bi_cursor
bi_before_block(bi_block *block)
{
   if (list_is_empty(&block->instructions))
      return bi_after_block(block);
   else
      return bi_before_nonempty_block(block);
}

/* Invariant: a tuple must be nonempty UNLESS it is the last tuple of a clause,
 * in which case there must exist a nonempty penultimate tuple */

ATTRIBUTE_RETURNS_NONNULL static inline bi_instr *
bi_first_instr_in_tuple(bi_tuple *tuple)
{
   bi_instr *instr = tuple->fma ?: tuple->add;
   assert(instr != NULL);
   return instr;
}

ATTRIBUTE_RETURNS_NONNULL static inline bi_instr *
bi_first_instr_in_clause(bi_clause *clause)
{
   return bi_first_instr_in_tuple(&clause->tuples[0]);
}

ATTRIBUTE_RETURNS_NONNULL static inline bi_instr *
bi_last_instr_in_clause(bi_clause *clause)
{
   bi_tuple tuple = clause->tuples[clause->tuple_count - 1];
   bi_instr *instr = tuple.add ?: tuple.fma;

   if (!instr) {
      assert(clause->tuple_count >= 2);
      tuple = clause->tuples[clause->tuple_count - 2];
      instr = tuple.add ?: tuple.fma;
   }

   assert(instr != NULL);
   return instr;
}

/* Implemented by expanding bi_foreach_instr_in_block_from(_rev) with the start
 * (end) of the clause and adding a condition for the clause boundary */

#define bi_foreach_instr_in_clause(block, clause, pos)                         \
   for (bi_instr *pos =                                                        \
           list_entry(bi_first_instr_in_clause(clause), bi_instr, link);       \
        (&pos->link != &(block)->instructions) &&                              \
        (pos != bi_next_op(bi_last_instr_in_clause(clause)));                  \
        pos = list_entry(pos->link.next, bi_instr, link))

#define bi_foreach_instr_in_clause_rev(block, clause, pos)                     \
   for (bi_instr *pos =                                                        \
           list_entry(bi_last_instr_in_clause(clause), bi_instr, link);        \
        (&pos->link != &(block)->instructions) &&                              \
        pos != bi_prev_op(bi_first_instr_in_clause(clause));                   \
        pos = list_entry(pos->link.prev, bi_instr, link))

static inline bi_cursor
bi_before_clause(bi_clause *clause)
{
   return bi_before_instr(bi_first_instr_in_clause(clause));
}

static inline bi_cursor
bi_before_tuple(bi_tuple *tuple)
{
   return bi_before_instr(bi_first_instr_in_tuple(tuple));
}

static inline bi_cursor
bi_after_clause(bi_clause *clause)
{
   return bi_after_instr(bi_last_instr_in_clause(clause));
}

/* Get a cursor at the start of a function, after any preloads */
static inline bi_cursor
bi_before_function(bi_context *ctx)
{
   bi_block *block = bi_start_block(&ctx->blocks);

   return bi_before_block(block);
}

/* IR builder in terms of cursor infrastructure */

typedef struct {
   bi_context *shader;
   bi_cursor cursor;
} bi_builder;

static inline bi_builder
bi_init_builder(bi_context *ctx, bi_cursor cursor)
{
   return (bi_builder){.shader = ctx, .cursor = cursor};
}

/* insert load/store for spills */
bi_instr *bi_load_tl(bi_builder *b, unsigned bits, bi_index src, unsigned offset);
void bi_store_tl(bi_builder *b, unsigned bits, bi_index src, unsigned offset);

/* Insert an instruction at the cursor and move the cursor */

static inline void
bi_builder_insert(bi_cursor *cursor, bi_instr *I)
{
   switch (cursor->option) {
   case bi_cursor_after_instr:
      list_add(&I->link, &cursor->instr->link);
      cursor->instr = I;
      return;

   case bi_cursor_after_block:
      list_addtail(&I->link, &cursor->block->instructions);
      cursor->option = bi_cursor_after_instr;
      cursor->instr = I;
      return;

   case bi_cursor_before_instr:
      list_addtail(&I->link, &cursor->instr->link);
      cursor->option = bi_cursor_after_instr;
      cursor->instr = I;
      return;
   }

   UNREACHABLE("Invalid cursor option");
}

bi_instr *bi_csel_from_mux(bi_builder *b, const bi_instr *I, bool must_sign);

/* Read back power-efficent garbage, TODO maybe merge with null? */
static inline bi_index
bi_dontcare(bi_builder *b)
{
   if (b->shader->arch >= 9)
      return bi_zero();
   else
      return bi_passthrough(BIFROST_SRC_FAU_HI);
}

#define bi_worklist_init(ctx, w)        u_worklist_init(w, ctx->num_blocks, ctx)
#define bi_worklist_push_head(w, block) u_worklist_push_head(w, block, index)
#define bi_worklist_push_tail(w, block) u_worklist_push_tail(w, block, index)
#define bi_worklist_peek_head(w)        u_worklist_peek_head(w, bi_block, index)
#define bi_worklist_pop_head(w)         u_worklist_pop_head(w, bi_block, index)
#define bi_worklist_peek_tail(w)        u_worklist_peek_tail(w, bi_block, index)
#define bi_worklist_pop_tail(w)         u_worklist_pop_tail(w, bi_block, index)

static inline void
bi_record_use(bi_instr **uses, BITSET_WORD *multiple, bi_instr *I, unsigned s)
{
   unsigned v = I->src[s].value;

   assert(I->src[s].type == BI_INDEX_NORMAL);
   if (uses[v] && uses[v] != I)
      BITSET_SET(multiple, v);
   else
      uses[v] = I;
}

/* NIR passes */

bool bi_lower_divergent_indirects(nir_shader *shader, unsigned lanes);

#ifdef __cplusplus
} /* extern C */
#endif

#endif
