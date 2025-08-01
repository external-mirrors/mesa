/*
 * Copyright © 2015-2018 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "ir3_context.h"
#include "ir3_compiler.h"
#include "ir3_image.h"
#include "ir3_nir.h"
#include "ir3_shader.h"
#include "nir.h"
#include "nir_intrinsics_indices.h"
#include "util/u_math.h"

struct ir3_context *
ir3_context_init(struct ir3_compiler *compiler, struct ir3_shader *shader,
                 struct ir3_shader_variant *so)
{
   MESA_TRACE_FUNC();

   struct ir3_context *ctx = rzalloc(NULL, struct ir3_context);

   if (compiler->gen == 4) {
      if (so->type == MESA_SHADER_VERTEX) {
         ctx->astc_srgb = so->key.vastc_srgb;
         memcpy(ctx->sampler_swizzles, so->key.vsampler_swizzles, sizeof(ctx->sampler_swizzles));
      } else if (so->type == MESA_SHADER_FRAGMENT ||
            so->type == MESA_SHADER_COMPUTE) {
         ctx->astc_srgb = so->key.fastc_srgb;
         memcpy(ctx->sampler_swizzles, so->key.fsampler_swizzles, sizeof(ctx->sampler_swizzles));
      }
   } else if (compiler->gen == 3) {
      if (so->type == MESA_SHADER_VERTEX) {
         ctx->samples = so->key.vsamples;
      } else if (so->type == MESA_SHADER_FRAGMENT) {
         ctx->samples = so->key.fsamples;
      }
   }

   if (compiler->gen >= 6) {
      ctx->funcs = &ir3_a6xx_funcs;
   } else if (compiler->gen >= 4) {
      ctx->funcs = &ir3_a4xx_funcs;
   }

   ctx->compiler = compiler;
   ctx->so = so;
   ctx->def_ht =
      _mesa_hash_table_create(ctx, _mesa_hash_pointer, _mesa_key_pointer_equal);
   ctx->block_ht =
      _mesa_hash_table_create(ctx, _mesa_hash_pointer, _mesa_key_pointer_equal);
   ctx->continue_block_ht =
      _mesa_hash_table_create(ctx, _mesa_hash_pointer, _mesa_key_pointer_equal);
   ctx->sel_cond_conversions =
      _mesa_hash_table_create(ctx, _mesa_hash_pointer, _mesa_key_pointer_equal);
   ctx->predicate_conversions = _mesa_pointer_hash_table_create(ctx);

   /* TODO: maybe generate some sort of bitmask of what key
    * lowers vs what shader has (ie. no need to lower
    * texture clamp lowering if no texture sample instrs)..
    * although should be done further up the stack to avoid
    * creating duplicate variants..
    */

   ctx->s = nir_shader_clone(ctx, shader->nir);
   ir3_nir_lower_variant(so, &shader->options.nir_options, ctx->s);

   bool progress = false;
   bool needs_late_alg = false;

   /* We want to lower nir_op_imul as late as possible, to catch also
    * those generated by earlier passes (e.g,
    * nir_lower_locals_to_regs).  However, we want a final swing of a
    * few passes to have a chance at optimizing the result.
    */
   NIR_PASS(progress, ctx->s, ir3_nir_lower_imul);
   while (progress) {
      progress = false;
      NIR_PASS(progress, ctx->s, nir_opt_algebraic);
      NIR_PASS(progress, ctx->s, nir_opt_copy_prop_vars);
      NIR_PASS(progress, ctx->s, nir_opt_dead_write_vars);
      NIR_PASS(progress, ctx->s, nir_opt_dce);
      NIR_PASS(progress, ctx->s, nir_opt_constant_folding);
      needs_late_alg = true;
   }

   /* nir_opt_algebraic() above would have unfused our ffmas, re-fuse them. */
   if (needs_late_alg) {
      NIR_PASS(progress, ctx->s, nir_opt_algebraic_late);
      NIR_PASS(progress, ctx->s, nir_opt_dce);
   }

   /* This must run after the last nir_opt_algebraic or it gets undone. */
   if (compiler->has_branch_and_or)
      NIR_PASS(_, ctx->s, ir3_nir_opt_branch_and_or_not);

   if (compiler->has_bitwise_triops) {
      bool triops_progress = false;
      NIR_PASS(triops_progress, ctx->s, ir3_nir_opt_triops_bitwise);

      if (triops_progress) {
         NIR_PASS(_, ctx->s, nir_opt_dce);
      }
   }

   /* Enable the texture pre-fetch feature only a4xx onwards.  But
    * only enable it on generations that have been tested:
    */
   if ((so->type == MESA_SHADER_FRAGMENT) && compiler->has_fs_tex_prefetch) {
      NIR_PASS(_, ctx->s, ir3_nir_lower_tex_prefetch, &so->prefetch_bary_type);
   }

   bool vectorized = false;
   NIR_PASS(vectorized, ctx->s, nir_opt_vectorize, ir3_nir_vectorize_filter,
            NULL);

   if (vectorized) {
      NIR_PASS(_, ctx->s, nir_opt_undef);
      NIR_PASS(_, ctx->s, nir_copy_prop);
      NIR_PASS(_, ctx->s, nir_opt_dce);

      /* nir_opt_vectorize could replace swizzled movs with vectorized movs in a
       * different block. If this happens with swizzled movs in a then block, it
       * could leave this block empty. ir3 assumes only the else block can be
       * empty (e.g., when lowering predicates) so make sure ifs are in that
       * canonical form again.
       */
      NIR_PASS(_, ctx->s, nir_opt_if, 0);
   }

   NIR_PASS(progress, ctx->s, nir_convert_to_lcssa, true, true);

   /* This has to go at the absolute end to make sure that all SSA defs are
    * correctly marked.
    */
   nir_divergence_analysis(ctx->s);

   /* Super crude heuristic to limit # of tex prefetch in small
    * shaders.  This completely ignores loops.. but that's really
    * not the worst of it's problems.  (A frag shader that has
    * loops is probably going to be big enough to not trigger a
    * lower threshold.)
    *
    *   1) probably want to do this in terms of ir3 instructions
    *   2) probably really want to decide this after scheduling
    *      (or at least pre-RA sched) so we have a rough idea about
    *      nops, and don't count things that get cp'd away
    *   3) blob seems to use higher thresholds with a mix of more
    *      SFU instructions.  Which partly makes sense, more SFU
    *      instructions probably means you want to get the real
    *      shader started sooner, but that considers where in the
    *      shader the SFU instructions are, which blob doesn't seem
    *      to do.
    *
    * This uses more conservative thresholds assuming a more alu
    * than sfu heavy instruction mix.
    */
   if (so->type == MESA_SHADER_FRAGMENT) {
      nir_function_impl *fxn = nir_shader_get_entrypoint(ctx->s);

      unsigned instruction_count = 0;
      nir_foreach_block (block, fxn) {
         nir_foreach_instr (instr, block) {
            /* Vectorized ALU instructions expand to one scalar instruction per
             * component.
             */
            if (instr->type == nir_instr_type_alu)
               instruction_count += nir_instr_as_alu(instr)->def.num_components;
            else
               instruction_count++;
         }
      }

      if (instruction_count < 50) {
         ctx->prefetch_limit = 2;
      } else if (instruction_count < 70) {
         ctx->prefetch_limit = 3;
      } else {
         ctx->prefetch_limit = IR3_MAX_SAMPLER_PREFETCH;
      }
   }

   if (shader_debug_enabled(so->type, ctx->s->info.internal)) {
      mesa_logi("NIR (final form) for %s shader %s:", ir3_shader_stage(so),
                so->name);
      nir_log_shaderi(ctx->s);
   }

   ir3_ibo_mapping_init(&so->image_mapping, ctx->s->info.num_textures);

   /* Implement the "dual_color_blend_by_location" workaround for Unigine Heaven
    * and Unigine Valley, by remapping FRAG_RESULT_DATA1 to be the 2nd color
    * channel of FRAG_RESULT_DATA0.
    */
   if ((so->type == MESA_SHADER_FRAGMENT) && so->key.force_dual_color_blend) {
      nir_variable *var = nir_find_variable_with_location(
         ctx->s, nir_var_shader_out, FRAG_RESULT_DATA1);
      if (var) {
         var->data.location = FRAG_RESULT_DATA0;
         var->data.index = 1;
         nir_shader_gather_info(ctx->s, nir_shader_get_entrypoint(ctx->s));
         so->dual_src_blend = true;
      }
   }

   return ctx;
}

void
ir3_context_free(struct ir3_context *ctx)
{
   ralloc_free(ctx);
}

/*
 * Misc helpers
 */

/* allocate a n element value array (to be populated by caller) and
 * insert in def_ht
 */
struct ir3_instruction **
ir3_get_dst_ssa(struct ir3_context *ctx, nir_def *dst, unsigned n)
{
   struct ir3_instruction **value =
      ralloc_array(ctx->def_ht, struct ir3_instruction *, n);
   _mesa_hash_table_insert(ctx->def_ht, dst, value);
   return value;
}

struct ir3_instruction **
ir3_get_def(struct ir3_context *ctx, nir_def *def, unsigned n)
{
   struct ir3_instruction **value = ir3_get_dst_ssa(ctx, def, n);

   compile_assert(ctx, !ctx->last_dst);
   ctx->last_dst = value;
   ctx->last_dst_n = n;

   return value;
}

struct ir3_instruction *const *
ir3_get_src_maybe_shared(struct ir3_context *ctx, nir_src *src)
{
   struct hash_entry *entry;
   entry = _mesa_hash_table_search(ctx->def_ht, src->ssa);
   compile_assert(ctx, entry);
   return entry->data;
}

static struct ir3_instruction *
get_shared(struct ir3_builder *build, struct ir3_instruction *src, bool shared)
{
   if (!!(src->dsts[0]->flags & IR3_REG_SHARED) != shared) {
      if (src->opc == OPC_META_COLLECT) {
         /* We can't mov the result of a collect so mov its sources and create a
          * new collect.
          */
         struct ir3_instruction *new_srcs[src->srcs_count];

         for (unsigned i = 0; i < src->srcs_count; i++) {
            new_srcs[i] = get_shared(build, src->srcs[i]->def->instr, shared);
         }

         return ir3_create_collect(build, new_srcs, src->srcs_count);
      }

      struct ir3_instruction *mov =
         ir3_MOV(build, src,
                 (src->dsts[0]->flags & IR3_REG_HALF) ? TYPE_U16 : TYPE_U32);
      mov->dsts[0]->flags &= ~IR3_REG_SHARED;
      mov->dsts[0]->flags |= COND(shared, IR3_REG_SHARED);
      return mov;
   }

   return src;
}

struct ir3_instruction *const *
ir3_get_src_shared(struct ir3_context *ctx, nir_src *src, bool shared)
{
   unsigned num_components = nir_src_num_components(*src);
   struct ir3_instruction *const *value = ir3_get_src_maybe_shared(ctx, src);
   bool mismatch = false;
   for (unsigned i = 0; i < nir_src_num_components(*src); i++) {
      if (!!(value[i]->dsts[0]->flags & IR3_REG_SHARED) != shared) {
         mismatch = true;
         break;
      }
   }

   if (!mismatch)
      return value;

   struct ir3_instruction **new_value =
      ralloc_array(ctx, struct ir3_instruction *, num_components);
   for (unsigned i = 0; i < num_components; i++)
      new_value[i] = get_shared(&ctx->build, value[i], shared);

   return new_value;
}

void
ir3_put_def(struct ir3_context *ctx, nir_def *def)
{
   unsigned bit_size = ir3_bitsize(ctx, def->bit_size);

   if (bit_size <= 16) {
      for (unsigned i = 0; i < ctx->last_dst_n; i++) {
         struct ir3_instruction *dst = ctx->last_dst[i];
         ir3_set_dst_type(dst, true);
         ir3_fixup_src_type(dst);
         if (dst->opc == OPC_META_SPLIT) {
            ir3_set_dst_type(ssa(dst->srcs[0]), true);
            ir3_fixup_src_type(ssa(dst->srcs[0]));
            dst->srcs[0]->flags |= IR3_REG_HALF;
         }
      }
   }

   ctx->last_dst = NULL;
   ctx->last_dst_n = 0;
}

NORETURN void
ir3_context_error(struct ir3_context *ctx, const char *format, ...)
{
   struct hash_table *errors = NULL;
   va_list ap;
   va_start(ap, format);
   if (ctx->cur_instr) {
      errors = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                       _mesa_key_pointer_equal);
      char *msg = ralloc_vasprintf(errors, format, ap);
      _mesa_hash_table_insert(errors, ctx->cur_instr, msg);
   } else {
      mesa_loge_v(format, ap);
   }
   va_end(ap);
   nir_log_shader_annotated(ctx->s, errors);
   ralloc_free(errors);
   ctx->error = true;
   UNREACHABLE("");
}

static struct ir3_instruction *
create_addr0(struct ir3_builder *build, struct ir3_instruction *src, int align)
{
   struct ir3_instruction *instr, *immed;

   instr = ir3_COV(build, src, TYPE_U32, TYPE_S16);
   bool shared = (src->dsts[0]->flags & IR3_REG_SHARED);

   switch (align) {
   case 1:
      /* src *= 1: */
      break;
   case 2:
      /* src *= 2	=> src <<= 1: */
      immed = create_immed_typed_shared(build, 1, TYPE_S16, shared);
      instr = ir3_SHL_B(build, instr, 0, immed, 0);
      break;
   case 3:
      /* src *= 3: */
      immed = create_immed_typed_shared(build, 3, TYPE_S16, shared);
      instr = ir3_MULL_U(build, instr, 0, immed, 0);
      break;
   case 4:
      /* src *= 4 => src <<= 2: */
      immed = create_immed_typed_shared(build, 2, TYPE_S16, shared);
      instr = ir3_SHL_B(build, instr, 0, immed, 0);
      break;
   default:
      UNREACHABLE("bad align");
      return NULL;
   }

   instr->dsts[0]->flags |= IR3_REG_HALF;

   instr = ir3_MOV(build, instr, TYPE_S16);
   instr->dsts[0]->num = regid(REG_A0, 0);
   instr->dsts[0]->flags &= ~IR3_REG_SHARED;

   return instr;
}

/* caches addr values to avoid generating multiple cov/shl/mova
 * sequences for each use of a given NIR level src as address
 */
struct ir3_instruction *
ir3_get_addr0(struct ir3_context *ctx, struct ir3_instruction *src, int align)
{
   struct ir3_instruction *addr;
   unsigned idx = align - 1;

   compile_assert(ctx, idx < ARRAY_SIZE(ctx->addr0_ht));

   if (!ctx->addr0_ht[idx]) {
      ctx->addr0_ht[idx] = _mesa_hash_table_create(ctx, _mesa_hash_pointer,
                                                   _mesa_key_pointer_equal);
   } else {
      struct hash_entry *entry;
      entry = _mesa_hash_table_search(ctx->addr0_ht[idx], src);
      if (entry)
         return entry->data;
   }

   addr = create_addr0(&ctx->build, src, align);
   _mesa_hash_table_insert(ctx->addr0_ht[idx], src, addr);

   return addr;
}

struct ir3_instruction *
ir3_get_predicate(struct ir3_context *ctx, struct ir3_instruction *src)
{
   src = ir3_get_cond_for_nonzero_compare(src);

   struct hash_entry *src_entry =
      _mesa_hash_table_search(ctx->predicate_conversions, src);
   if (src_entry)
      return src_entry->data;

   struct ir3_builder b = ir3_builder_at(ir3_after_instr_and_phis(src));
   struct ir3_instruction *cond;

   /* NOTE: we use cpms.s.ne x, 0 to move x into a predicate register */
   struct ir3_instruction *zero =
      create_immed_typed_shared(&b, 0, is_half(src) ? TYPE_U16 : TYPE_U32,
                                src->dsts[0]->flags & IR3_REG_SHARED);
   cond = ir3_CMPS_S(&b, src, 0, zero, 0);
   cond->cat2.condition = IR3_COND_NE;

   /* condition always goes in predicate register: */
   cond->dsts[0]->flags |= IR3_REG_PREDICATE;
   cond->dsts[0]->flags &= ~IR3_REG_SHARED;

   _mesa_hash_table_insert(ctx->predicate_conversions, src, cond);
   return cond;
}

/*
 * Array helpers
 */

void
ir3_declare_array(struct ir3_context *ctx, nir_intrinsic_instr *decl)
{
   struct ir3_array *arr = rzalloc(ctx, struct ir3_array);
   arr->id = ++ctx->num_arrays;
   /* NOTE: sometimes we get non array regs, for example for arrays of
    * length 1.  See fs-const-array-of-struct-of-array.shader_test.  So
    * treat a non-array as if it was an array of length 1.
    *
    * It would be nice if there was a nir pass to convert arrays of
    * length 1 to ssa.
    */
   arr->length = nir_intrinsic_num_components(decl) *
                 MAX2(1, nir_intrinsic_num_array_elems(decl));

   compile_assert(ctx, arr->length > 0);
   arr->r = &decl->def;
   arr->half = ir3_bitsize(ctx, nir_intrinsic_bit_size(decl)) <= 16;
   list_addtail(&arr->node, &ctx->ir->array_list);
}

struct ir3_array *
ir3_get_array(struct ir3_context *ctx, nir_def *reg)
{
   foreach_array (arr, &ctx->ir->array_list) {
      if (arr->r == reg)
         return arr;
   }
   ir3_context_error(ctx, "bogus reg: r%d\n", reg->index);
   return NULL;
}

/* relative (indirect) if address!=NULL */
struct ir3_instruction *
ir3_create_array_load(struct ir3_context *ctx, struct ir3_array *arr, int n,
                      struct ir3_instruction *address)
{
   struct ir3_block *block = ctx->block;
   struct ir3_instruction *mov;
   struct ir3_register *src;
   unsigned flags = 0;

   mov = ir3_build_instr(&ctx->build, OPC_MOV, 1, 1);
   if (arr->half) {
      mov->cat1.src_type = TYPE_U16;
      mov->cat1.dst_type = TYPE_U16;
      flags |= IR3_REG_HALF;
   } else {
      mov->cat1.src_type = TYPE_U32;
      mov->cat1.dst_type = TYPE_U32;
   }

   mov->barrier_class = IR3_BARRIER_ARRAY_R;
   mov->barrier_conflict = IR3_BARRIER_ARRAY_W;
   __ssa_dst(mov)->flags |= flags;
   src = ir3_src_create(mov, 0,
                        IR3_REG_ARRAY | COND(address, IR3_REG_RELATIV) | flags);
   src->def = (arr->last_write && arr->last_write->instr->block == block)
                 ? arr->last_write
                 : NULL;
   src->size = arr->length;
   src->array.id = arr->id;
   src->array.offset = n;
   src->array.base = INVALID_REG;

   if (address)
      ir3_instr_set_address(mov, address);

   return mov;
}

/* relative (indirect) if address!=NULL */
void
ir3_create_array_store(struct ir3_context *ctx, struct ir3_array *arr, int n,
                       struct ir3_instruction *src,
                       struct ir3_instruction *address)
{
   struct ir3_block *block = ctx->block;
   struct ir3_instruction *mov;
   struct ir3_register *dst;
   unsigned flags = 0;

   mov = ir3_build_instr(&ctx->build, OPC_MOV, 1, 1);
   if (arr->half) {
      mov->cat1.src_type = TYPE_U16;
      mov->cat1.dst_type = TYPE_U16;
      flags |= IR3_REG_HALF;
   } else {
      mov->cat1.src_type = TYPE_U32;
      mov->cat1.dst_type = TYPE_U32;
   }
   mov->barrier_class = IR3_BARRIER_ARRAY_W;
   mov->barrier_conflict = IR3_BARRIER_ARRAY_R | IR3_BARRIER_ARRAY_W;
   dst = ir3_dst_create(
      mov, INVALID_REG,
      IR3_REG_SSA | IR3_REG_ARRAY | flags | COND(address, IR3_REG_RELATIV));
   dst->instr = mov;
   dst->size = arr->length;
   dst->array.id = arr->id;
   dst->array.offset = n;
   dst->array.base = INVALID_REG;
   ir3_src_create(mov, INVALID_REG, IR3_REG_SSA | flags |
                  (src->dsts[0]->flags & IR3_REG_SHARED))->def = src->dsts[0];

   if (arr->last_write && arr->last_write->instr->block == block)
      ir3_reg_set_last_array(mov, dst, arr->last_write);

   if (address)
      ir3_instr_set_address(mov, address);

   arr->last_write = dst;

   /* the array store may only matter to something in an earlier
    * block (ie. loops), but since arrays are not in SSA, depth
    * pass won't know this.. so keep all array stores:
    */
   array_insert(block, block->keeps, mov);
}

void
ir3_lower_imm_offset(struct ir3_context *ctx, nir_intrinsic_instr *intr,
                     nir_src *offset_src, unsigned imm_offset_bits,
                     struct ir3_instruction **offset, unsigned *imm_offset)
{
   nir_const_value *nir_const_offset = nir_src_as_const_value(*offset_src);
   int base = nir_intrinsic_base(intr);
   unsigned imm_offset_bound = (1 << imm_offset_bits);
   assert(base >= 0 && base < imm_offset_bound);

   if (nir_const_offset) {
      /* If both the offset and the base (immed offset) are constants, lower the
       * offset to a multiple of the bound and the immed offset to the
       * remainder. This ensures that the offset register can often be reused
       * among multiple contiguous accesses.
       */
      uint32_t full_offset = base + nir_const_offset->u32;
      *offset = create_immed(&ctx->build,
                             ROUND_DOWN_TO(full_offset, imm_offset_bound));
      *imm_offset = full_offset % imm_offset_bound;
   } else {
      *offset = ir3_get_src(ctx, offset_src)[0];
      *imm_offset = base;
   }
}
