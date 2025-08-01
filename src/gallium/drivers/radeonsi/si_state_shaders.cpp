/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_nir.h"
#include "ac_shader_util.h"
#include "nir.h"
#include "nir_xfb_info.h"
#include "nir_serialize.h"
#include "nir/tgsi_to_nir.h"
#include "si_build_pm4.h"
#include "sid.h"
#include "util/crc32.h"
#include "util/disk_cache.h"
#include "util/hash_table.h"
#include "util/mesa-sha1.h"
#include "util/u_async_debug.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#include "tgsi/tgsi_from_mesa.h"

static void si_update_tess_in_out_patch_vertices(struct si_context *sctx);

unsigned si_determine_wave_size(struct si_screen *sscreen, struct si_shader *shader)
{
   struct si_shader_info *info = &shader->selector->info;
   gl_shader_stage stage = shader->selector->stage;

   struct si_shader_selector *prev_sel = NULL;
   if (stage == MESA_SHADER_TESS_CTRL)
      prev_sel = shader->key.ge.part.tcs.ls;
   else if (stage == MESA_SHADER_GEOMETRY)
      prev_sel = shader->key.ge.part.gs.es;

   if (sscreen->info.gfx_level < GFX10)
      return 64;

   /* Legacy GS only supports Wave64. */
   if ((stage == MESA_SHADER_VERTEX && shader->key.ge.as_es && !shader->key.ge.as_ngg) ||
       (stage == MESA_SHADER_TESS_EVAL && shader->key.ge.as_es && !shader->key.ge.as_ngg) ||
       (stage == MESA_SHADER_GEOMETRY && !shader->key.ge.as_ngg))
      return 64;

   /* For KHR_shader_subgroup which require a constant subgroup size known by user. */
   if (info->base.subgroup_size == SUBGROUP_SIZE_API_CONSTANT ||
       (prev_sel && prev_sel->info.base.subgroup_size == SUBGROUP_SIZE_API_CONSTANT))
      return 64;

   /* Workgroup sizes that are not divisible by 64 use Wave32. */
   if (stage == MESA_SHADER_COMPUTE && !info->base.workgroup_size_variable &&
       (info->base.workgroup_size[0] *
        info->base.workgroup_size[1] *
        info->base.workgroup_size[2]) % 64 != 0)
      return 32;

   /* AMD_DEBUG wave flags override everything else. */
   if (sscreen->shader_debug_flags &
       (stage == MESA_SHADER_COMPUTE ? DBG(W32_CS) :
        stage == MESA_SHADER_FRAGMENT ? DBG(W32_PS) : DBG(W32_GE)))
      return 32;

   if (sscreen->shader_debug_flags &
       (stage == MESA_SHADER_COMPUTE ? DBG(W64_CS) :
        stage == MESA_SHADER_FRAGMENT ? DBG(W64_PS) : DBG(W64_GE)))
      return 64;

   /* Shader profiles. */
   if (info->options & SI_PROFILE_WAVE32)
      return 32;

   if (info->options & SI_PROFILE_GFX10_WAVE64 &&
       (sscreen->info.gfx_level == GFX10 || sscreen->info.gfx_level == GFX10_3))
      return 64;

   /* Gfx10: Pixel shaders without interp instructions don't suffer from reduced interpolation
    * performance in Wave32, so use Wave32. This helps Piano and Voloplosion.
    *
    * Gfx11: Prefer Wave64 to take advantage of doubled VALU performance.
    */
   if (sscreen->info.gfx_level < GFX11 && stage == MESA_SHADER_FRAGMENT && !info->num_inputs)
      return 32;

   /* Gfx10: There are a few very rare cases where VS is better with Wave32, and there are no
    * known cases where Wave64 is better.
    *
    * Wave32 is disabled for GFX10 when culling is active as a workaround for #6457. I don't
    * know why this helps.
    *
    * Gfx11: Prefer Wave64 because it's slightly better than Wave32.
    */
   if (stage <= MESA_SHADER_GEOMETRY &&
       (sscreen->info.gfx_level == GFX10 || sscreen->info.gfx_level == GFX10_3) &&
       !(sscreen->info.gfx_level == GFX10 && si_shader_culling_enabled(shader)))
      return 32;

   /* Divergent loops in Wave64 can end up having too many iterations in one half of the wave
    * while the other half is idling but occupying VGPRs, preventing other waves from launching.
    * Wave32 eliminates the idling half to allow the next wave to start.
    *
    * Gfx11: Wave32 continues to be faster with divergent loops despite worse VALU performance.
    */
   if (info->has_divergent_loop ||
       /* Merged shader has to use same wave size for two shader stages. */
       (prev_sel && prev_sel->info.has_divergent_loop))
      return 32;

   return 64;
}

static bool si_shader_uses_bindless_samplers(struct si_shader_selector *selector)
{
   return selector ? selector->info.uses_bindless_samplers : false;
}

static bool si_shader_uses_bindless_images(struct si_shader_selector *selector)
{
   return selector ? selector->info.uses_bindless_images : false;
}

/* SHADER_CACHE */

/**
 * Return the IR key for the shader cache.
 */
void si_get_ir_cache_key(struct si_shader_selector *sel, bool ngg, bool es,
                         unsigned wave_size, unsigned char ir_sha1_cache_key[20])
{
   struct blob blob = {};
   unsigned ir_size;
   void *ir_binary;

   if (sel->nir_binary) {
      ir_binary = sel->nir_binary;
      ir_size = sel->nir_size;
   } else {
      assert(sel->nir);

      blob_init(&blob);
      /* Keep debug info if NIR debug prints are in use. */
      nir_serialize(&blob, sel->nir, NIR_DEBUG(PRINT) == 0);
      ir_binary = blob.data;
      ir_size = blob.size;
   }

   /* These settings affect the compilation, but they are not derived
    * from the input shader IR.
    */
   unsigned shader_variant_flags = 0;

   if (ngg)
      shader_variant_flags |= 1 << 0;
   /* bit gap */
   if (wave_size == 32)
      shader_variant_flags |= 1 << 2;
   /* bit gap */
   /* use_ngg_culling disables NGG passthrough for non-culling shaders to reduce context
    * rolls, which can be changed with AMD_DEBUG=nonggc or AMD_DEBUG=nggc.
    */
   if (sel->screen->use_ngg_culling)
      shader_variant_flags |= 1 << 4;
   if (sel->screen->record_llvm_ir)
      shader_variant_flags |= 1 << 5;
   if (sel->screen->info.has_image_opcodes)
      shader_variant_flags |= 1 << 6;
   if (sel->screen->options.no_infinite_interp)
      shader_variant_flags |= 1 << 7;
   if (sel->screen->options.clamp_div_by_zero)
      shader_variant_flags |= 1 << 8;
   if ((sel->stage == MESA_SHADER_VERTEX ||
        sel->stage == MESA_SHADER_TESS_EVAL ||
        sel->stage == MESA_SHADER_GEOMETRY) &&
       !es &&
       sel->screen->options.vrs2x2)
      shader_variant_flags |= 1 << 10;
   if (sel->screen->options.inline_uniforms)
      shader_variant_flags |= 1 << 11;
   if (sel->screen->options.clear_lds)
      shader_variant_flags |= 1 << 12;

   struct mesa_sha1 ctx;
   _mesa_sha1_init(&ctx);
   _mesa_sha1_update(&ctx, &shader_variant_flags, 4);
   _mesa_sha1_update(&ctx, ir_binary, ir_size);
   _mesa_sha1_final(&ctx, ir_sha1_cache_key);

   if (ir_binary == blob.data)
      blob_finish(&blob);
}

/** Copy "data" to "ptr" and return the next dword following copied data. */
static uint32_t *write_data(uint32_t *ptr, const void *data, unsigned size)
{
   /* data may be NULL if size == 0 */
   if (size)
      memcpy(ptr, data, size);
   ptr += DIV_ROUND_UP(size, 4);
   return ptr;
}

/** Read data from "ptr". Return the next dword following the data. */
static uint32_t *read_data(uint32_t *ptr, void *data, unsigned size)
{
   memcpy(data, ptr, size);
   ptr += DIV_ROUND_UP(size, 4);
   return ptr;
}

/**
 * Write the size as uint followed by the data. Return the next dword
 * following the copied data.
 */
static uint32_t *write_chunk(uint32_t *ptr, const void *data, unsigned size)
{
   *ptr++ = size;
   return write_data(ptr, data, size);
}

/**
 * Read the size as uint followed by the data. Return both via parameters.
 * Return the next dword following the data.
 */
static uint32_t *read_chunk(uint32_t *ptr, void **data, unsigned *size)
{
   *size = *ptr++;
   assert(*data == NULL);
   if (!*size)
      return ptr;
   *data = malloc(*size);
   return read_data(ptr, *data, *size);
}

struct si_shader_blob_head {
   uint32_t size;
   uint32_t type;
   uint32_t crc32;
};

/**
 * Return the shader binary in a buffer.
 */
static uint32_t *si_get_shader_binary(struct si_shader *shader)
{
   /* There is always a size of data followed by the data itself. */
   unsigned llvm_ir_size =
      shader->binary.llvm_ir_string ? strlen(shader->binary.llvm_ir_string) + 1 : 0;

   /* Refuse to allocate overly large buffers and guard against integer
    * overflow. */
   if (shader->binary.code_size > UINT_MAX / 4 || llvm_ir_size > UINT_MAX / 4 ||
       shader->binary.num_symbols > UINT_MAX / 32)
      return NULL;

   unsigned size = sizeof(struct si_shader_blob_head) +
                   align(sizeof(shader->config), 4) +
                   align(sizeof(shader->info), 4) +
                   4 + 4 + align(shader->binary.code_size, 4) +
                   4 + shader->binary.num_symbols * 8 +
                   4 + align(llvm_ir_size, 4) +
                   4 + align(shader->binary.disasm_size, 4);
   uint32_t *buffer = (uint32_t*)CALLOC(1, size);
   if (!buffer)
      return NULL;

   struct si_shader_blob_head *head = (struct si_shader_blob_head *)buffer;
   head->type = shader->binary.type;
   head->size = size;

   uint32_t *data = buffer + sizeof(*head) / 4;
   uint32_t *ptr = data;

   ptr = write_data(ptr, &shader->config, sizeof(shader->config));
   ptr = write_data(ptr, &shader->info, sizeof(shader->info));
   ptr = write_data(ptr, &shader->binary.exec_size, 4);
   ptr = write_chunk(ptr, shader->binary.code_buffer, shader->binary.code_size);
   ptr = write_chunk(ptr, shader->binary.symbols, shader->binary.num_symbols * 8);
   ptr = write_chunk(ptr, shader->binary.llvm_ir_string, llvm_ir_size);
   ptr = write_chunk(ptr, shader->binary.disasm_string, shader->binary.disasm_size);
   assert((char *)ptr - (char *)buffer == (ptrdiff_t)size);

   /* Compute CRC32. */
   head->crc32 = util_hash_crc32(data, size - sizeof(*head));

   return buffer;
}

static bool si_load_shader_binary(struct si_shader *shader, void *binary)
{
   struct si_shader_blob_head *head = (struct si_shader_blob_head *)binary;
   unsigned chunk_size;
   unsigned code_size;

   uint32_t *ptr = (uint32_t *)binary + sizeof(*head) / 4;
   if (util_hash_crc32(ptr, head->size - sizeof(*head)) != head->crc32) {
      mesa_loge("binary shader has invalid CRC32");
      return false;
   }

   shader->binary.type = (enum si_shader_binary_type)head->type;
   ptr = read_data(ptr, &shader->config, sizeof(shader->config));
   ptr = read_data(ptr, &shader->info, sizeof(shader->info));
   ptr = read_data(ptr, &shader->binary.exec_size, 4);
   ptr = read_chunk(ptr, (void **)&shader->binary.code_buffer, &code_size);
   shader->binary.code_size = code_size;
   ptr = read_chunk(ptr, (void **)&shader->binary.symbols, &chunk_size);
   shader->binary.num_symbols = chunk_size / 8;
   ptr = read_chunk(ptr, (void **)&shader->binary.llvm_ir_string, &chunk_size);
   ptr = read_chunk(ptr, (void **)&shader->binary.disasm_string, &chunk_size);
   shader->binary.disasm_size = chunk_size;

   if (!shader->is_gs_copy_shader &&
       shader->selector->stage == MESA_SHADER_GEOMETRY && !shader->key.ge.as_ngg) {
      shader->gs_copy_shader = CALLOC_STRUCT(si_shader);
      if (!shader->gs_copy_shader)
         return false;

      shader->gs_copy_shader->is_gs_copy_shader = true;

      if (!si_load_shader_binary(shader->gs_copy_shader, (uint8_t*)binary + head->size)) {
         FREE(shader->gs_copy_shader);
         shader->gs_copy_shader = NULL;
         return false;
      }

      util_queue_fence_init(&shader->gs_copy_shader->ready);
      shader->gs_copy_shader->selector = shader->selector;
      shader->gs_copy_shader->is_gs_copy_shader = true;
      shader->gs_copy_shader->wave_size =
         si_determine_wave_size(shader->selector->screen, shader->gs_copy_shader);

      si_shader_binary_upload(shader->selector->screen, shader->gs_copy_shader, 0);
   }

   return true;
}

/**
 * Insert a shader into the cache. It's assumed the shader is not in the cache.
 * Use si_shader_cache_load_shader before calling this.
 */
void si_shader_cache_insert_shader(struct si_screen *sscreen, unsigned char ir_sha1_cache_key[20],
                                   struct si_shader *shader, bool insert_into_disk_cache)
{
   uint32_t *hw_binary;
   struct hash_entry *entry;
   uint8_t key[CACHE_KEY_SIZE];
   bool memory_cache_full = sscreen->shader_cache_size >= sscreen->shader_cache_max_size;

   if (!insert_into_disk_cache && memory_cache_full)
      return;

   entry = _mesa_hash_table_search(sscreen->shader_cache, ir_sha1_cache_key);
   if (entry)
      return; /* already added */

   hw_binary = si_get_shader_binary(shader);
   if (!hw_binary)
      return;

   unsigned size = *hw_binary;

   if (shader->selector->stage == MESA_SHADER_GEOMETRY && !shader->key.ge.as_ngg) {
      uint32_t *gs_copy_binary = si_get_shader_binary(shader->gs_copy_shader);
      if (!gs_copy_binary) {
         FREE(hw_binary);
         return;
      }

      /* Combine both binaries. */
      size += *gs_copy_binary;
      uint32_t *combined_binary = (uint32_t*)MALLOC(size);
      if (!combined_binary) {
         FREE(hw_binary);
         FREE(gs_copy_binary);
         return;
      }

      memcpy(combined_binary, hw_binary, *hw_binary);
      memcpy(combined_binary + *hw_binary / 4, gs_copy_binary, *gs_copy_binary);
      FREE(hw_binary);
      FREE(gs_copy_binary);
      hw_binary = combined_binary;
   }

   if (!memory_cache_full) {
      if (_mesa_hash_table_insert(sscreen->shader_cache,
                                  mem_dup(ir_sha1_cache_key, 20),
                                  hw_binary) == NULL) {
          FREE(hw_binary);
          return;
      }

      sscreen->shader_cache_size += size;
   }

   if (sscreen->disk_shader_cache && insert_into_disk_cache) {
      disk_cache_compute_key(sscreen->disk_shader_cache, ir_sha1_cache_key, 20, key);
      disk_cache_put(sscreen->disk_shader_cache, key, hw_binary, size, NULL);
   }

   if (memory_cache_full)
      FREE(hw_binary);
}

bool si_shader_cache_load_shader(struct si_screen *sscreen, unsigned char ir_sha1_cache_key[20],
                                 struct si_shader *shader)
{
   struct hash_entry *entry = _mesa_hash_table_search(sscreen->shader_cache, ir_sha1_cache_key);

   if (entry) {
      if (si_load_shader_binary(shader, entry->data)) {
         p_atomic_inc(&sscreen->num_memory_shader_cache_hits);
         return true;
      }
   }
   p_atomic_inc(&sscreen->num_memory_shader_cache_misses);

   if (!sscreen->disk_shader_cache)
      return false;

   unsigned char sha1[CACHE_KEY_SIZE];
   disk_cache_compute_key(sscreen->disk_shader_cache, ir_sha1_cache_key, 20, sha1);

   size_t total_size;
   uint32_t *buffer = (uint32_t*)disk_cache_get(sscreen->disk_shader_cache, sha1, &total_size);
   if (buffer) {
      unsigned size = *buffer;
      unsigned gs_copy_binary_size = 0;

      /* The GS copy shader binary is after the GS binary. */
      if (shader->selector->stage == MESA_SHADER_GEOMETRY && !shader->key.ge.as_ngg)
         gs_copy_binary_size = buffer[size / 4];

      if (total_size >= sizeof(uint32_t) && size + gs_copy_binary_size == total_size) {
         if (si_load_shader_binary(shader, buffer)) {
            free(buffer);
            si_shader_cache_insert_shader(sscreen, ir_sha1_cache_key, shader, false);
            p_atomic_inc(&sscreen->num_disk_shader_cache_hits);
            return true;
         }
      } else {
         /* Something has gone wrong discard the item from the cache and
          * rebuild/link from source.
          */
         assert(!"Invalid radeonsi shader disk cache item!");
         disk_cache_remove(sscreen->disk_shader_cache, sha1);
      }
   }

   free(buffer);
   p_atomic_inc(&sscreen->num_disk_shader_cache_misses);
   return false;
}

static uint32_t si_shader_cache_key_hash(const void *key)
{
   /* Take the first dword of SHA1. */
   return *(uint32_t *)key;
}

static bool si_shader_cache_key_equals(const void *a, const void *b)
{
   /* Compare SHA1s. */
   return memcmp(a, b, 20) == 0;
}

static void si_destroy_shader_cache_entry(struct hash_entry *entry)
{
   FREE((void *)entry->key);
   FREE(entry->data);
}

bool si_init_shader_cache(struct si_screen *sscreen)
{
   (void)simple_mtx_init(&sscreen->shader_cache_mutex, mtx_plain);
   sscreen->shader_cache =
      _mesa_hash_table_create(NULL, si_shader_cache_key_hash, si_shader_cache_key_equals);
   sscreen->shader_cache_size = 0;
   /* Maximum size: 64MB on 32 bits, 1GB else */
   sscreen->shader_cache_max_size = ((sizeof(void *) == 4) ? 64 : 1024) * 1024 * 1024;

   return sscreen->shader_cache != NULL;
}

void si_destroy_shader_cache(struct si_screen *sscreen)
{
   if (sscreen->shader_cache)
      _mesa_hash_table_destroy(sscreen->shader_cache, si_destroy_shader_cache_entry);
   simple_mtx_destroy(&sscreen->shader_cache_mutex);
}

/* SHADER STATES */

unsigned si_shader_encode_vgprs(struct si_shader *shader)
{
   assert(shader->selector->screen->info.gfx_level >= GFX10 || shader->wave_size == 64);
   return shader->config.num_vgprs / (shader->wave_size == 32 ? 8 : 4) - 1;
}

unsigned si_shader_encode_sgprs(struct si_shader *shader)
{
   if (shader->selector->screen->info.gfx_level >= GFX10)
      return 0; /* Gfx10+ don't have the SGPRS field and always allocate 128 SGPRs. */

   return shader->config.num_sgprs / 8 - 1;
}

bool si_shader_mem_ordered(struct si_shader *shader)
{
   struct si_screen *sscreen = shader->selector->screen;

   if (sscreen->info.gfx_level < GFX10 || sscreen->info.gfx_level >= GFX12)
      return false;

   /* Return true if both types of VMEM that return something are used. */
   return shader->info.uses_vmem_sampler_or_bvh &&
          (shader->info.uses_vmem_load_other ||
           shader->config.scratch_bytes_per_wave);
}

static void si_set_tesseval_regs(struct si_screen *sscreen, const struct si_shader_selector *tes,
                                 struct si_shader *shader)
{
   const struct si_shader_info *info = &tes->info;
   enum tess_primitive_mode tes_prim_mode = info->base.tess._primitive_mode;
   unsigned tes_spacing = info->base.tess.spacing;
   bool tes_vertex_order_cw = !info->base.tess.ccw;
   bool tes_point_mode = info->base.tess.point_mode;
   unsigned type, partitioning, topology, distribution_mode;

   switch (tes_prim_mode) {
   case TESS_PRIMITIVE_ISOLINES:
      type = V_028B6C_TESS_ISOLINE;
      break;
   case TESS_PRIMITIVE_TRIANGLES:
      type = V_028B6C_TESS_TRIANGLE;
      break;
   case TESS_PRIMITIVE_QUADS:
      type = V_028B6C_TESS_QUAD;
      break;
   default:
      assert(0);
      return;
   }

   switch (tes_spacing) {
   case TESS_SPACING_FRACTIONAL_ODD:
      partitioning = V_028B6C_PART_FRAC_ODD;
      break;
   case TESS_SPACING_FRACTIONAL_EVEN:
      partitioning = V_028B6C_PART_FRAC_EVEN;
      break;
   case TESS_SPACING_EQUAL:
      partitioning = V_028B6C_PART_INTEGER;
      break;
   default:
      assert(0);
      return;
   }

   if (tes_point_mode)
      topology = V_028B6C_OUTPUT_POINT;
   else if (tes_prim_mode == TESS_PRIMITIVE_ISOLINES)
      topology = V_028B6C_OUTPUT_LINE;
   else if (tes_vertex_order_cw)
      /* for some reason, this must be the other way around */
      topology = V_028B6C_OUTPUT_TRIANGLE_CCW;
   else
      topology = V_028B6C_OUTPUT_TRIANGLE_CW;

   if (sscreen->info.has_distributed_tess) {
      if (sscreen->info.family == CHIP_FIJI || sscreen->info.family >= CHIP_POLARIS10)
         distribution_mode = V_028B6C_TRAPEZOIDS;
      else
         distribution_mode = V_028B6C_DONUTS;
   } else
      distribution_mode = V_028B6C_NO_DIST;

   shader->vgt_tf_param = S_028B6C_TYPE(type) | S_028B6C_PARTITIONING(partitioning) |
                          S_028B6C_TOPOLOGY(topology) |
                          S_028B6C_DISTRIBUTION_MODE(distribution_mode);

   if (sscreen->info.gfx_level >= GFX12)
      shader->vgt_tf_param |= S_028AA4_TEMPORAL(gfx12_load_last_use_discard);
}

/* Polaris needs different VTX_REUSE_DEPTH settings depending on
 * whether the "fractional odd" tessellation spacing is used.
 *
 * Possible VGT configurations and which state should set the register:
 *
 *   Reg set in | VGT shader configuration   | Value
 * ------------------------------------------------------
 *     VS as VS | VS                         | 30
 *     VS as ES | ES -> GS -> VS             | 30
 *    TES as VS | LS -> HS -> VS             | 14 or 30
 *    TES as ES | LS -> HS -> ES -> GS -> VS | 14 or 30
 */
static void polaris_set_vgt_vertex_reuse(struct si_screen *sscreen, struct si_shader_selector *sel,
                                         struct si_shader *shader)
{
   if (sscreen->info.family < CHIP_POLARIS10 || sscreen->info.gfx_level >= GFX10)
      return;

   /* VS as VS, or VS as ES: */
   if ((sel->stage == MESA_SHADER_VERTEX &&
        (!shader->key.ge.as_ls && !shader->is_gs_copy_shader)) ||
       /* TES as VS, or TES as ES: */
       sel->stage == MESA_SHADER_TESS_EVAL) {
      unsigned vtx_reuse_depth = 30;

      if (sel->stage == MESA_SHADER_TESS_EVAL &&
          sel->info.base.tess.spacing == TESS_SPACING_FRACTIONAL_ODD)
         vtx_reuse_depth = 14;

      shader->vgt_vertex_reuse_block_cntl = vtx_reuse_depth;
   }
}

static struct si_pm4_state *
si_get_shader_pm4_state(struct si_shader *shader,
                        void (*emit_func)(struct si_context *ctx, unsigned index))
{
   si_pm4_clear_state(&shader->pm4, shader->selector->screen, false);
   shader->pm4.atom.emit = emit_func;
   return &shader->pm4;
}

static unsigned si_get_num_vs_user_sgprs(struct si_shader *shader,
                                         unsigned num_always_on_user_sgprs)
{
   struct si_shader_selector *vs =
      shader->previous_stage_sel ? shader->previous_stage_sel : shader->selector;
   unsigned num_vbos_in_user_sgprs = vs->info.num_vbos_in_user_sgprs;

   /* 1 SGPR is reserved for the vertex buffer pointer. */
   assert(num_always_on_user_sgprs <= SI_SGPR_VS_VB_DESCRIPTOR_FIRST - 1);

   if (num_vbos_in_user_sgprs)
      return SI_SGPR_VS_VB_DESCRIPTOR_FIRST + num_vbos_in_user_sgprs * 4;

   /* Add the pointer to VBO descriptors. */
   return num_always_on_user_sgprs + 1;
}

/* Return VGPR_COMP_CNT for the API vertex shader. This can be hw LS, LSHS, ES, ESGS, VS. */
static unsigned si_get_vs_vgpr_comp_cnt(struct si_screen *sscreen, struct si_shader *shader,
                                        bool legacy_vs_prim_id)
{
   assert(shader->selector->stage == MESA_SHADER_VERTEX ||
          (shader->previous_stage_sel && shader->previous_stage_sel->stage == MESA_SHADER_VERTEX));

   /* GFX6-9   LS    (VertexID, RelAutoIndex,           InstanceID / StepRate0, InstanceID)
    * GFX6-9   ES,VS (VertexID, InstanceID / StepRate0, VSPrimID,               InstanceID)
    * GFX10-11 LS    (VertexID, RelAutoIndex,           UserVGPR1,              UserVGPR2 or InstanceID)
    * GFX10-11 ES,VS (VertexID, UserVGPR1,              UserVGPR2 or VSPrimID,  UserVGPR3 or InstanceID)
    * GFX12    LS,ES (VertexID, InstanceID)
    */
   bool is_ls = shader->selector->stage == MESA_SHADER_TESS_CTRL || shader->key.ge.as_ls;
   unsigned max = 0;

   if (shader->info.uses_instance_id) {
      if (sscreen->info.gfx_level >= GFX12)
         max = MAX2(max, 1);
      else if (sscreen->info.gfx_level >= GFX10)
         max = MAX2(max, 3);
      else if (is_ls)
         max = MAX2(max, 2); /* use (InstanceID / StepRate0) because StepRate0 == 1 */
      else
         max = MAX2(max, 1); /* use (InstanceID / StepRate0) because StepRate0 == 1 */
   }

   if (legacy_vs_prim_id)
      max = MAX2(max, 2); /* VSPrimID */

   /* GFX11: We prefer to compute RelAutoIndex using (WaveID * WaveSize + ThreadID).
    * Older chips didn't have WaveID in LS.
    * GFX12 doesn't have RelAutoIndex.
    */
   if (is_ls && sscreen->info.gfx_level <= GFX10_3)
      max = MAX2(max, 1); /* RelAutoIndex */

   return max;
}

static void si_shader_ls(struct si_screen *sscreen, struct si_shader *shader)
{
   struct si_pm4_state *pm4;
   uint64_t va;

   assert(sscreen->info.gfx_level <= GFX8);

   pm4 = si_get_shader_pm4_state(shader, NULL);
   if (!pm4)
      return;

   va = shader->bo->gpu_address;
   ac_pm4_set_reg(&pm4->base, R_00B520_SPI_SHADER_PGM_LO_LS, va >> 8);

   shader->config.rsrc1 = S_00B528_VGPRS(si_shader_encode_vgprs(shader)) |
                          S_00B528_SGPRS(si_shader_encode_sgprs(shader)) |
                          S_00B528_VGPR_COMP_CNT(si_get_vs_vgpr_comp_cnt(sscreen, shader, false)) |
                          S_00B528_DX10_CLAMP(1) |
                          S_00B528_FLOAT_MODE(shader->config.float_mode);
   shader->config.rsrc2 = S_00B52C_USER_SGPR(si_get_num_vs_user_sgprs(shader, SI_VS_NUM_USER_SGPR)) |
                          S_00B52C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0);
   ac_pm4_finalize(&pm4->base);
}

static void si_shader_hs(struct si_screen *sscreen, struct si_shader *shader)
{
   struct si_pm4_state *pm4 = si_get_shader_pm4_state(shader, NULL);
   if (!pm4)
      return;

   uint64_t va = shader->bo->gpu_address;
   unsigned num_user_sgprs = sscreen->info.gfx_level >= GFX9 ?
                                si_get_num_vs_user_sgprs(shader, GFX9_TCS_NUM_USER_SGPR) :
                                GFX6_TCS_NUM_USER_SGPR;

   if (sscreen->info.gfx_level >= GFX12) {
      ac_pm4_set_reg(&pm4->base, R_00B420_SPI_SHADER_PGM_RSRC4_HS,
                     S_00B420_WAVE_LIMIT(0x3ff) |
                     S_00B420_GLG_FORCE_DISABLE(1) |
                     S_00B420_INST_PREF_SIZE(si_get_shader_prefetch_size(shader)));

      ac_pm4_set_reg(&pm4->base, R_00B424_SPI_SHADER_PGM_LO_LS, va >> 8);
   } else if (sscreen->info.gfx_level >= GFX11) {
      ac_pm4_set_reg_idx3(&pm4->base, R_00B404_SPI_SHADER_PGM_RSRC4_HS,
                          ac_apply_cu_en(S_00B404_INST_PREF_SIZE(si_get_shader_prefetch_size(shader)) |
                                         S_00B404_CU_EN(0xffff),
                                         C_00B404_CU_EN, 16, &sscreen->info));

      ac_pm4_set_reg(&pm4->base, R_00B520_SPI_SHADER_PGM_LO_LS, va >> 8);
   } else if (sscreen->info.gfx_level >= GFX10) {
      ac_pm4_set_reg(&pm4->base, R_00B520_SPI_SHADER_PGM_LO_LS, va >> 8);
   } else if (sscreen->info.gfx_level >= GFX9) {
      ac_pm4_set_reg(&pm4->base, R_00B410_SPI_SHADER_PGM_LO_LS, va >> 8);
   } else {
      ac_pm4_set_reg(&pm4->base, R_00B420_SPI_SHADER_PGM_LO_HS, va >> 8);
      ac_pm4_set_reg(&pm4->base, R_00B424_SPI_SHADER_PGM_HI_HS,
                     S_00B424_MEM_BASE(sscreen->info.address32_hi >> 8));
   }

   ac_pm4_set_reg(&pm4->base, R_00B428_SPI_SHADER_PGM_RSRC1_HS,
                  S_00B428_VGPRS(si_shader_encode_vgprs(shader)) |
                  S_00B428_SGPRS(si_shader_encode_sgprs(shader)) |
                  S_00B428_DX10_CLAMP(sscreen->info.gfx_level < GFX12) |
                  S_00B428_MEM_ORDERED(si_shader_mem_ordered(shader)) |
                  S_00B428_FLOAT_MODE(shader->config.float_mode) |
                  S_00B428_LS_VGPR_COMP_CNT(sscreen->info.gfx_level >= GFX9 ?
                                            si_get_vs_vgpr_comp_cnt(sscreen, shader, false) : 0));

   shader->config.rsrc2 = S_00B42C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0) |
                          S_00B42C_USER_SGPR(num_user_sgprs);

   if (sscreen->info.gfx_level >= GFX10) {
      shader->config.rsrc2 |= S_00B42C_USER_SGPR_MSB_GFX10(num_user_sgprs >> 5) |
                              S_00B42C_SHARED_VGPR_CNT(shader->config.num_shared_vgprs / 8);
   } else if (sscreen->info.gfx_level >= GFX9) {
      shader->config.rsrc2 |= S_00B42C_USER_SGPR_MSB_GFX9(num_user_sgprs >> 5);
   } else {
      shader->config.rsrc2 |= S_00B42C_OC_LDS_EN(1);
   }

   if (sscreen->info.gfx_level <= GFX8)
      ac_pm4_set_reg(&pm4->base, R_00B42C_SPI_SHADER_PGM_RSRC2_HS, shader->config.rsrc2);

   ac_pm4_finalize(&pm4->base);
}

static void si_emit_shader_es(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.es;

   radeon_begin(&sctx->gfx_cs);
   radeon_opt_set_context_reg(R_028AAC_VGT_ESGS_RING_ITEMSIZE,
                              SI_TRACKED_VGT_ESGS_RING_ITEMSIZE,
                              shader->selector->info.esgs_vertex_stride / 4);

   if (shader->selector->stage == MESA_SHADER_TESS_EVAL)
      radeon_opt_set_context_reg(R_028B6C_VGT_TF_PARAM, SI_TRACKED_VGT_TF_PARAM,
                                 shader->vgt_tf_param);

   if (shader->vgt_vertex_reuse_block_cntl)
      radeon_opt_set_context_reg(R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL,
                                 SI_TRACKED_VGT_VERTEX_REUSE_BLOCK_CNTL,
                                 shader->vgt_vertex_reuse_block_cntl);
   radeon_end_update_context_roll();
}

static void si_shader_es(struct si_screen *sscreen, struct si_shader *shader)
{
   struct si_pm4_state *pm4;
   unsigned num_user_sgprs;
   unsigned vgpr_comp_cnt;
   uint64_t va;
   unsigned oc_lds_en;

   assert(sscreen->info.gfx_level <= GFX8);

   pm4 = si_get_shader_pm4_state(shader, si_emit_shader_es);
   if (!pm4)
      return;

   va = shader->bo->gpu_address;

   if (shader->selector->stage == MESA_SHADER_VERTEX) {
      vgpr_comp_cnt = si_get_vs_vgpr_comp_cnt(sscreen, shader, false);
      num_user_sgprs = si_get_num_vs_user_sgprs(shader, SI_VS_NUM_USER_SGPR);
   } else if (shader->selector->stage == MESA_SHADER_TESS_EVAL) {
      vgpr_comp_cnt = shader->selector->info.uses_primid ? 3 : 2;
      num_user_sgprs = SI_TES_NUM_USER_SGPR;
   } else
      UNREACHABLE("invalid shader selector type");

   oc_lds_en = shader->selector->stage == MESA_SHADER_TESS_EVAL ? 1 : 0;

   ac_pm4_set_reg(&pm4->base, R_00B320_SPI_SHADER_PGM_LO_ES, va >> 8);
   ac_pm4_set_reg(&pm4->base, R_00B324_SPI_SHADER_PGM_HI_ES,
                  S_00B324_MEM_BASE(sscreen->info.address32_hi >> 8));
   ac_pm4_set_reg(&pm4->base, R_00B328_SPI_SHADER_PGM_RSRC1_ES,
                  S_00B328_VGPRS(si_shader_encode_vgprs(shader)) |
                  S_00B328_SGPRS(si_shader_encode_sgprs(shader)) |
                  S_00B328_VGPR_COMP_CNT(vgpr_comp_cnt) |
                  S_00B328_DX10_CLAMP(1) |
                  S_00B328_FLOAT_MODE(shader->config.float_mode));
   ac_pm4_set_reg(&pm4->base, R_00B32C_SPI_SHADER_PGM_RSRC2_ES,
                  S_00B32C_USER_SGPR(num_user_sgprs) | S_00B32C_OC_LDS_EN(oc_lds_en) |
                  S_00B32C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0));

   if (shader->selector->stage == MESA_SHADER_TESS_EVAL)
      si_set_tesseval_regs(sscreen, shader->selector, shader);

   polaris_set_vgt_vertex_reuse(sscreen, shader->selector, shader);
   ac_pm4_finalize(&pm4->base);
}

static void gfx9_set_gs_sgpr_num_es_outputs(struct si_context *sctx, unsigned esgs_vertex_stride)
{
   /* The stride must always be odd (e.g. a multiple of 4 + 1) to reduce LDS bank conflicts. */
   assert(!esgs_vertex_stride || esgs_vertex_stride % 4 == 1);
   unsigned num_es_outputs = esgs_vertex_stride / 4;

   /* If there are no ES outputs, GS doesn't use this SGPR field, so only set it if the number
    * is non-zero.
    */
   if (num_es_outputs)
      SET_FIELD(sctx->current_gs_state, GS_STATE_NUM_ES_OUTPUTS, num_es_outputs);
}

static void si_emit_shader_gs(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.gs;

   if (sctx->gfx_level >= GFX9)
      gfx9_set_gs_sgpr_num_es_outputs(sctx, shader->key.ge.part.gs.es->info.esgs_vertex_stride / 4);

   radeon_begin(&sctx->gfx_cs);

   /* R_028A60_VGT_GSVS_RING_OFFSET_1, R_028A64_VGT_GSVS_RING_OFFSET_2
    * R_028A68_VGT_GSVS_RING_OFFSET_3 */
   radeon_opt_set_context_reg3(
      R_028A60_VGT_GSVS_RING_OFFSET_1, SI_TRACKED_VGT_GSVS_RING_OFFSET_1,
      shader->gs.vgt_gsvs_ring_offset_1, shader->gs.vgt_gsvs_ring_offset_2,
      shader->gs.vgt_gsvs_ring_offset_3);

   /* R_028AB0_VGT_GSVS_RING_ITEMSIZE */
   radeon_opt_set_context_reg(R_028AB0_VGT_GSVS_RING_ITEMSIZE,
                              SI_TRACKED_VGT_GSVS_RING_ITEMSIZE,
                              shader->gs.vgt_gsvs_ring_itemsize);

   /* R_028B38_VGT_GS_MAX_VERT_OUT */
   radeon_opt_set_context_reg(R_028B38_VGT_GS_MAX_VERT_OUT, SI_TRACKED_VGT_GS_MAX_VERT_OUT,
                              shader->gs.vgt_gs_max_vert_out);

   /* R_028B5C_VGT_GS_VERT_ITEMSIZE, R_028B60_VGT_GS_VERT_ITEMSIZE_1
    * R_028B64_VGT_GS_VERT_ITEMSIZE_2, R_028B68_VGT_GS_VERT_ITEMSIZE_3 */
   radeon_opt_set_context_reg4(
      R_028B5C_VGT_GS_VERT_ITEMSIZE, SI_TRACKED_VGT_GS_VERT_ITEMSIZE,
      shader->gs.vgt_gs_vert_itemsize, shader->gs.vgt_gs_vert_itemsize_1,
      shader->gs.vgt_gs_vert_itemsize_2, shader->gs.vgt_gs_vert_itemsize_3);

   /* R_028B90_VGT_GS_INSTANCE_CNT */
   radeon_opt_set_context_reg(R_028B90_VGT_GS_INSTANCE_CNT, SI_TRACKED_VGT_GS_INSTANCE_CNT,
                              shader->gs.vgt_gs_instance_cnt);

   if (sctx->gfx_level >= GFX9) {
      /* R_028A44_VGT_GS_ONCHIP_CNTL */
      radeon_opt_set_context_reg(R_028A44_VGT_GS_ONCHIP_CNTL, SI_TRACKED_VGT_GS_ONCHIP_CNTL,
                                 shader->gs.vgt_gs_onchip_cntl);
      /* R_028A94_VGT_GS_MAX_PRIMS_PER_SUBGROUP */
      if (sctx->gfx_level == GFX9) {
         radeon_opt_set_context_reg(R_028A94_VGT_GS_MAX_PRIMS_PER_SUBGROUP,
                                    SI_TRACKED_VGT_GS_MAX_PRIMS_PER_SUBGROUP,
                                    shader->gs.vgt_gs_max_prims_per_subgroup);
      }

      if (shader->key.ge.part.gs.es->stage == MESA_SHADER_TESS_EVAL)
         radeon_opt_set_context_reg(R_028B6C_VGT_TF_PARAM, SI_TRACKED_VGT_TF_PARAM,
                                    shader->vgt_tf_param);
      if (shader->vgt_vertex_reuse_block_cntl)
         radeon_opt_set_context_reg(R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL,
                                    SI_TRACKED_VGT_VERTEX_REUSE_BLOCK_CNTL,
                                    shader->vgt_vertex_reuse_block_cntl);
   }
   radeon_end_update_context_roll();

   /* These don't cause any context rolls. */
   radeon_begin_again(&sctx->gfx_cs);
   if (sctx->gfx_level >= GFX7) {
      if (sctx->screen->info.uses_kernel_cu_mask) {
         radeon_opt_set_sh_reg_idx(R_00B21C_SPI_SHADER_PGM_RSRC3_GS,
                                   SI_TRACKED_SPI_SHADER_PGM_RSRC3_GS,
                                   3, shader->gs.spi_shader_pgm_rsrc3_gs);
      } else {
         radeon_opt_set_sh_reg(R_00B21C_SPI_SHADER_PGM_RSRC3_GS,
                               SI_TRACKED_SPI_SHADER_PGM_RSRC3_GS,
                               shader->gs.spi_shader_pgm_rsrc3_gs);
      }
   }
   if (sctx->gfx_level >= GFX10) {
      if (sctx->screen->info.uses_kernel_cu_mask) {
         radeon_opt_set_sh_reg_idx(R_00B204_SPI_SHADER_PGM_RSRC4_GS,
                                   SI_TRACKED_SPI_SHADER_PGM_RSRC4_GS,
                                   3, shader->gs.spi_shader_pgm_rsrc4_gs);
      } else {
         radeon_opt_set_sh_reg(R_00B204_SPI_SHADER_PGM_RSRC4_GS,
                               SI_TRACKED_SPI_SHADER_PGM_RSRC4_GS,
                               shader->gs.spi_shader_pgm_rsrc4_gs);
      }
   }
   radeon_end();
}

static void si_shader_gs(struct si_screen *sscreen, struct si_shader *shader)
{
   struct si_shader_selector *sel = shader->selector;
   const uint8_t *num_components = shader->info.legacy_gs.num_components_per_stream;
   unsigned gs_num_invocations = sel->info.base.gs.invocations;
   struct si_pm4_state *pm4;
   uint64_t va;
   unsigned max_stream = num_components[3] ? 4 :
                         num_components[2] ? 3 :
                         num_components[1] ? 2 : 1;
   unsigned offset;

   assert(sscreen->info.gfx_level < GFX11); /* gfx11 doesn't have the legacy pipeline */

   pm4 = si_get_shader_pm4_state(shader, si_emit_shader_gs);
   if (!pm4)
      return;

   offset = (uint32_t)num_components[0] * sel->info.base.gs.vertices_out;
   shader->gs.vgt_gsvs_ring_offset_1 = offset;

   if (max_stream >= 2)
      offset += (uint32_t)num_components[1] * sel->info.base.gs.vertices_out;
   shader->gs.vgt_gsvs_ring_offset_2 = offset;

   if (max_stream >= 3)
      offset += (uint32_t)num_components[2] * sel->info.base.gs.vertices_out;
   shader->gs.vgt_gsvs_ring_offset_3 = offset;

   if (max_stream >= 4)
      offset += (uint32_t)num_components[3] * sel->info.base.gs.vertices_out;
   shader->gs.vgt_gsvs_ring_itemsize = offset;

   /* The GSVS_RING_ITEMSIZE register takes 15 bits */
   assert(offset < (1 << 15));

   shader->gs.vgt_gs_max_vert_out = sel->info.base.gs.vertices_out;

   shader->gs.vgt_gs_vert_itemsize = num_components[0];
   shader->gs.vgt_gs_vert_itemsize_1 = (max_stream >= 2) ? num_components[1] : 0;
   shader->gs.vgt_gs_vert_itemsize_2 = (max_stream >= 3) ? num_components[2] : 0;
   shader->gs.vgt_gs_vert_itemsize_3 = (max_stream >= 4) ? num_components[3] : 0;

   shader->gs.vgt_gs_instance_cnt =
      S_028B90_CNT(MIN2(gs_num_invocations, 127)) | S_028B90_ENABLE(gs_num_invocations > 0);

   /* Copy over fields from the GS copy shader to make them easily accessible from GS. */
   shader->pa_cl_vs_out_cntl = shader->gs_copy_shader->pa_cl_vs_out_cntl;

   va = shader->bo->gpu_address;

   if (sscreen->info.gfx_level >= GFX9) {
      unsigned input_prim = sel->info.base.gs.input_primitive;
      gl_shader_stage es_stage = shader->key.ge.part.gs.es->stage;
      unsigned es_vgpr_comp_cnt, gs_vgpr_comp_cnt;

      if (es_stage == MESA_SHADER_VERTEX) {
         es_vgpr_comp_cnt = si_get_vs_vgpr_comp_cnt(sscreen, shader, false);
      } else if (es_stage == MESA_SHADER_TESS_EVAL)
         es_vgpr_comp_cnt = shader->key.ge.part.gs.es->info.uses_primid ? 3 : 2;
      else
         UNREACHABLE("invalid shader selector type");

      /* If offsets 4, 5 are used, GS_VGPR_COMP_CNT is ignored and
       * VGPR[0:4] are always loaded.
       */
      if (sel->info.uses_invocationid)
         gs_vgpr_comp_cnt = 3; /* VGPR3 contains InvocationID. */
      else if (sel->info.uses_primid)
         gs_vgpr_comp_cnt = 2; /* VGPR2 contains PrimitiveID. */
      else if (input_prim >= MESA_PRIM_TRIANGLES)
         gs_vgpr_comp_cnt = 1; /* VGPR1 contains offsets 2, 3 */
      else
         gs_vgpr_comp_cnt = 0; /* VGPR0 contains offsets 0, 1 */

      unsigned num_user_sgprs;
      if (es_stage == MESA_SHADER_VERTEX)
         num_user_sgprs = si_get_num_vs_user_sgprs(shader, GFX9_GS_NUM_USER_SGPR);
      else
         num_user_sgprs = GFX9_GS_NUM_USER_SGPR;

      if (sscreen->info.gfx_level >= GFX10) {
         ac_pm4_set_reg(&pm4->base, R_00B320_SPI_SHADER_PGM_LO_ES, va >> 8);
      } else {
         ac_pm4_set_reg(&pm4->base, R_00B210_SPI_SHADER_PGM_LO_ES, va >> 8);
      }

      uint32_t rsrc1 = S_00B228_VGPRS(si_shader_encode_vgprs(shader)) |
                       S_00B228_SGPRS(si_shader_encode_sgprs(shader)) |
                       S_00B228_DX10_CLAMP(1) |
                       S_00B228_MEM_ORDERED(si_shader_mem_ordered(shader)) |
                       S_00B228_FLOAT_MODE(shader->config.float_mode) |
                       S_00B228_GS_VGPR_COMP_CNT(gs_vgpr_comp_cnt);
      uint32_t rsrc2 = S_00B22C_USER_SGPR(num_user_sgprs) |
                       S_00B22C_ES_VGPR_COMP_CNT(es_vgpr_comp_cnt) |
                       S_00B22C_OC_LDS_EN(es_stage == MESA_SHADER_TESS_EVAL) |
                       S_00B22C_LDS_SIZE(shader->config.lds_size) |
                       S_00B22C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0);

      if (sscreen->info.gfx_level >= GFX10) {
         rsrc2 |= S_00B22C_USER_SGPR_MSB_GFX10(num_user_sgprs >> 5) |
                  S_00B22C_SHARED_VGPR_CNT(shader->config.num_shared_vgprs / 8);
      } else {
         rsrc2 |= S_00B22C_USER_SGPR_MSB_GFX9(num_user_sgprs >> 5);
      }

      ac_pm4_set_reg(&pm4->base, R_00B228_SPI_SHADER_PGM_RSRC1_GS, rsrc1);
      ac_pm4_set_reg(&pm4->base, R_00B22C_SPI_SHADER_PGM_RSRC2_GS, rsrc2);

      shader->gs.spi_shader_pgm_rsrc3_gs =
         ac_apply_cu_en(S_00B21C_CU_EN(0xffff) |
                        S_00B21C_WAVE_LIMIT(0x3F),
                        C_00B21C_CU_EN, 0, &sscreen->info);
      shader->gs.spi_shader_pgm_rsrc4_gs =
         ac_apply_cu_en(S_00B204_CU_EN_GFX10(0xffff) |
                        S_00B204_SPI_SHADER_LATE_ALLOC_GS_GFX10(0),
                        C_00B204_CU_EN_GFX10, 16, &sscreen->info);

      shader->gs.vgt_gs_onchip_cntl =
         S_028A44_ES_VERTS_PER_SUBGRP(shader->gs_info.es_verts_per_subgroup) |
         S_028A44_GS_PRIMS_PER_SUBGRP(shader->gs_info.gs_prims_per_subgroup) |
         S_028A44_GS_INST_PRIMS_IN_SUBGRP(shader->gs_info.gs_inst_prims_in_subgroup);
      shader->gs.vgt_gs_max_prims_per_subgroup =
         S_028A94_MAX_PRIMS_PER_SUBGROUP(shader->gs_info.max_prims_per_subgroup);
      shader->gs.vgt_esgs_ring_itemsize = shader->key.ge.part.gs.es->info.esgs_vertex_stride / 4;

      if (es_stage == MESA_SHADER_TESS_EVAL)
         si_set_tesseval_regs(sscreen, shader->key.ge.part.gs.es, shader);

      polaris_set_vgt_vertex_reuse(sscreen, shader->key.ge.part.gs.es, shader);
   } else {
      shader->gs.spi_shader_pgm_rsrc3_gs =
         ac_apply_cu_en(S_00B21C_CU_EN(0xffff) |
                        S_00B21C_WAVE_LIMIT(0x3F),
                        C_00B21C_CU_EN, 0, &sscreen->info);

      ac_pm4_set_reg(&pm4->base, R_00B220_SPI_SHADER_PGM_LO_GS, va >> 8);
      ac_pm4_set_reg(&pm4->base, R_00B224_SPI_SHADER_PGM_HI_GS,
                     S_00B224_MEM_BASE(sscreen->info.address32_hi >> 8));

      ac_pm4_set_reg(&pm4->base, R_00B228_SPI_SHADER_PGM_RSRC1_GS,
                     S_00B228_VGPRS(si_shader_encode_vgprs(shader)) |
                     S_00B228_SGPRS(si_shader_encode_sgprs(shader)) |
                     S_00B228_DX10_CLAMP(1) |
                     S_00B228_FLOAT_MODE(shader->config.float_mode));
      ac_pm4_set_reg(&pm4->base, R_00B22C_SPI_SHADER_PGM_RSRC2_GS,
                     S_00B22C_USER_SGPR(GFX6_GS_NUM_USER_SGPR) |
                     S_00B22C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0));
   }
   ac_pm4_finalize(&pm4->base);
}

bool gfx10_is_ngg_passthrough(struct si_shader *shader)
{
   struct si_shader_selector *sel = shader->selector;

   /* Never use NGG passthrough if culling is possible even when it's not used by this shader,
    * so that we don't get context rolls when enabling and disabling NGG passthrough.
    */
   if (sel->screen->use_ngg_culling)
      return false;

   /* The definition of NGG passthrough is:
    * - user GS is turned off (no amplification, no GS instancing, and no culling)
    * - VGT_ESGS_RING_ITEMSIZE is ignored (behaving as if it was equal to 1)
    * - vertex indices are packed into 1 VGPR
    * - Navi23 and later chips can optionally skip the gs_alloc_req message
    *
    * NGG passthrough still allows the use of LDS.
    */
   return sel->stage != MESA_SHADER_GEOMETRY && !si_shader_culling_enabled(shader);
}

template <enum si_has_tess HAS_TESS>
static void gfx10_emit_shader_ngg(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.gs;

   if (shader->selector->stage == MESA_SHADER_GEOMETRY)
      gfx9_set_gs_sgpr_num_es_outputs(sctx, shader->ngg.esgs_vertex_stride);

   radeon_begin(&sctx->gfx_cs);
   if (HAS_TESS) {
      radeon_opt_set_context_reg(R_028B6C_VGT_TF_PARAM, SI_TRACKED_VGT_TF_PARAM,
                                 shader->vgt_tf_param);
   }
   radeon_opt_set_context_reg(R_0287FC_GE_MAX_OUTPUT_PER_SUBGROUP,
                              SI_TRACKED_GE_MAX_OUTPUT_PER_SUBGROUP,
                              shader->ngg.ge_max_output_per_subgroup);
   radeon_opt_set_context_reg(R_028B4C_GE_NGG_SUBGRP_CNTL, SI_TRACKED_GE_NGG_SUBGRP_CNTL,
                              shader->ngg.ge_ngg_subgrp_cntl);
   radeon_opt_set_context_reg(R_028A84_VGT_PRIMITIVEID_EN, SI_TRACKED_VGT_PRIMITIVEID_EN,
                              shader->ngg.vgt_primitiveid_en);
   if (sctx->gfx_level < GFX11) {
      radeon_opt_set_context_reg(R_028A44_VGT_GS_ONCHIP_CNTL, SI_TRACKED_VGT_GS_ONCHIP_CNTL,
                                 shader->ngg.vgt_gs_onchip_cntl);
   }
   radeon_opt_set_context_reg(R_028B38_VGT_GS_MAX_VERT_OUT, SI_TRACKED_VGT_GS_MAX_VERT_OUT,
                              shader->ngg.vgt_gs_max_vert_out);
   radeon_opt_set_context_reg(R_028B90_VGT_GS_INSTANCE_CNT, SI_TRACKED_VGT_GS_INSTANCE_CNT,
                              shader->ngg.vgt_gs_instance_cnt);
   radeon_opt_set_context_reg(R_0286C4_SPI_VS_OUT_CONFIG, SI_TRACKED_SPI_VS_OUT_CONFIG,
                              shader->ngg.spi_vs_out_config);
   radeon_opt_set_context_reg(R_02870C_SPI_SHADER_POS_FORMAT,
                              SI_TRACKED_SPI_SHADER_POS_FORMAT,
                              shader->ngg.spi_shader_pos_format);
   radeon_opt_set_context_reg(R_028818_PA_CL_VTE_CNTL, SI_TRACKED_PA_CL_VTE_CNTL,
                              shader->ngg.pa_cl_vte_cntl);
   radeon_end_update_context_roll();

   /* These don't cause a context roll. */
   radeon_begin_again(&sctx->gfx_cs);
   if (sctx->screen->info.uses_kernel_cu_mask) {
      radeon_opt_set_sh_reg_idx(R_00B21C_SPI_SHADER_PGM_RSRC3_GS,
                                SI_TRACKED_SPI_SHADER_PGM_RSRC3_GS,
                                3, shader->ngg.spi_shader_pgm_rsrc3_gs);
      radeon_opt_set_sh_reg_idx(R_00B204_SPI_SHADER_PGM_RSRC4_GS,
                                SI_TRACKED_SPI_SHADER_PGM_RSRC4_GS,
                                3, shader->ngg.spi_shader_pgm_rsrc4_gs);
   } else {
      radeon_opt_set_sh_reg(R_00B21C_SPI_SHADER_PGM_RSRC3_GS,
                            SI_TRACKED_SPI_SHADER_PGM_RSRC3_GS,
                            shader->ngg.spi_shader_pgm_rsrc3_gs);
      radeon_opt_set_sh_reg(R_00B204_SPI_SHADER_PGM_RSRC4_GS,
                            SI_TRACKED_SPI_SHADER_PGM_RSRC4_GS,
                            shader->ngg.spi_shader_pgm_rsrc4_gs);
   }
   radeon_opt_set_uconfig_reg(R_030980_GE_PC_ALLOC, SI_TRACKED_GE_PC_ALLOC,
                              shader->ngg.ge_pc_alloc);
   radeon_end();
}

template <enum si_has_tess HAS_TESS>
static void gfx11_dgpu_emit_shader_ngg(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.gs;

   if (shader->selector->stage == MESA_SHADER_GEOMETRY)
      gfx9_set_gs_sgpr_num_es_outputs(sctx, shader->ngg.esgs_vertex_stride);

   radeon_begin(&sctx->gfx_cs);
   gfx11_begin_packed_context_regs();
   if (HAS_TESS) {
      gfx11_opt_set_context_reg(R_028B6C_VGT_TF_PARAM, SI_TRACKED_VGT_TF_PARAM,
                                shader->vgt_tf_param);
   }
   gfx11_opt_set_context_reg(R_0287FC_GE_MAX_OUTPUT_PER_SUBGROUP,
                             SI_TRACKED_GE_MAX_OUTPUT_PER_SUBGROUP,
                             shader->ngg.ge_max_output_per_subgroup);
   gfx11_opt_set_context_reg(R_028B4C_GE_NGG_SUBGRP_CNTL, SI_TRACKED_GE_NGG_SUBGRP_CNTL,
                             shader->ngg.ge_ngg_subgrp_cntl);
   gfx11_opt_set_context_reg(R_028A84_VGT_PRIMITIVEID_EN, SI_TRACKED_VGT_PRIMITIVEID_EN,
                             shader->ngg.vgt_primitiveid_en);
   gfx11_opt_set_context_reg(R_028B38_VGT_GS_MAX_VERT_OUT, SI_TRACKED_VGT_GS_MAX_VERT_OUT,
                             shader->ngg.vgt_gs_max_vert_out);
   gfx11_opt_set_context_reg(R_028B90_VGT_GS_INSTANCE_CNT, SI_TRACKED_VGT_GS_INSTANCE_CNT,
                             shader->ngg.vgt_gs_instance_cnt);
   gfx11_opt_set_context_reg(R_0286C4_SPI_VS_OUT_CONFIG, SI_TRACKED_SPI_VS_OUT_CONFIG,
                             shader->ngg.spi_vs_out_config);
   gfx11_opt_set_context_reg(R_02870C_SPI_SHADER_POS_FORMAT, SI_TRACKED_SPI_SHADER_POS_FORMAT,
                             shader->ngg.spi_shader_pos_format);
   gfx11_opt_set_context_reg(R_028818_PA_CL_VTE_CNTL, SI_TRACKED_PA_CL_VTE_CNTL,
                             shader->ngg.pa_cl_vte_cntl);
   gfx11_end_packed_context_regs();

   assert(!sctx->screen->info.uses_kernel_cu_mask);
   if (sctx->screen->info.has_set_sh_pairs_packed) {
      gfx11_opt_push_gfx_sh_reg(R_00B21C_SPI_SHADER_PGM_RSRC3_GS,
                                SI_TRACKED_SPI_SHADER_PGM_RSRC3_GS,
                                shader->ngg.spi_shader_pgm_rsrc3_gs);
      gfx11_opt_push_gfx_sh_reg(R_00B204_SPI_SHADER_PGM_RSRC4_GS,
                                SI_TRACKED_SPI_SHADER_PGM_RSRC4_GS,
                                shader->ngg.spi_shader_pgm_rsrc4_gs);
   } else {
      if (sctx->screen->info.uses_kernel_cu_mask) {
         radeon_opt_set_sh_reg_idx(R_00B21C_SPI_SHADER_PGM_RSRC3_GS,
                                   SI_TRACKED_SPI_SHADER_PGM_RSRC3_GS,
                                   3, shader->ngg.spi_shader_pgm_rsrc3_gs);
         radeon_opt_set_sh_reg_idx(R_00B204_SPI_SHADER_PGM_RSRC4_GS,
                                   SI_TRACKED_SPI_SHADER_PGM_RSRC4_GS,
                                   3, shader->ngg.spi_shader_pgm_rsrc4_gs);
      } else {
         radeon_opt_set_sh_reg(R_00B21C_SPI_SHADER_PGM_RSRC3_GS,
                               SI_TRACKED_SPI_SHADER_PGM_RSRC3_GS,
                               shader->ngg.spi_shader_pgm_rsrc3_gs);
         radeon_opt_set_sh_reg(R_00B204_SPI_SHADER_PGM_RSRC4_GS,
                               SI_TRACKED_SPI_SHADER_PGM_RSRC4_GS,
                               shader->ngg.spi_shader_pgm_rsrc4_gs);
      }
   }

   radeon_opt_set_uconfig_reg(R_030980_GE_PC_ALLOC, SI_TRACKED_GE_PC_ALLOC,
                              shader->ngg.ge_pc_alloc);
   radeon_end();
}

template <enum si_has_tess HAS_TESS>
static void gfx12_emit_shader_ngg(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.gs;

   if (shader->selector->stage == MESA_SHADER_GEOMETRY)
      gfx9_set_gs_sgpr_num_es_outputs(sctx, shader->ngg.esgs_vertex_stride);

   radeon_begin(&sctx->gfx_cs);
   gfx12_begin_context_regs();
   if (HAS_TESS) {
      gfx12_opt_set_context_reg(R_028AA4_VGT_TF_PARAM, SI_TRACKED_VGT_TF_PARAM,
                                shader->vgt_tf_param);
   }
   gfx12_opt_set_context_reg(R_0287FC_GE_MAX_OUTPUT_PER_SUBGROUP,
                             SI_TRACKED_GE_MAX_OUTPUT_PER_SUBGROUP,
                             shader->ngg.ge_max_output_per_subgroup);
   gfx12_opt_set_context_reg(R_028B4C_GE_NGG_SUBGRP_CNTL, SI_TRACKED_GE_NGG_SUBGRP_CNTL,
                             shader->ngg.ge_ngg_subgrp_cntl);
   gfx12_opt_set_context_reg(R_028B38_VGT_GS_MAX_VERT_OUT, SI_TRACKED_VGT_GS_MAX_VERT_OUT,
                             shader->ngg.vgt_gs_max_vert_out);
   gfx12_opt_set_context_reg(R_028B3C_VGT_GS_INSTANCE_CNT, SI_TRACKED_VGT_GS_INSTANCE_CNT,
                             shader->ngg.vgt_gs_instance_cnt);
   gfx12_opt_set_context_reg(R_02864C_SPI_SHADER_POS_FORMAT, SI_TRACKED_SPI_SHADER_POS_FORMAT,
                             shader->ngg.spi_shader_pos_format);
   gfx12_opt_set_context_reg(R_028814_PA_CL_VTE_CNTL, SI_TRACKED_PA_CL_VTE_CNTL,
                             shader->ngg.pa_cl_vte_cntl);
   gfx12_end_context_regs();

   radeon_opt_set_uconfig_reg(R_030988_VGT_PRIMITIVEID_EN,
                              SI_TRACKED_VGT_PRIMITIVEID_EN_UCONFIG,
                              shader->ngg.vgt_primitiveid_en);
   radeon_end(); /* don't track context rolls on GFX12 */

   assert(!sctx->screen->info.uses_kernel_cu_mask);
   gfx12_opt_push_gfx_sh_reg(R_00B220_SPI_SHADER_PGM_RSRC4_GS,
                             SI_TRACKED_SPI_SHADER_PGM_RSRC4_GS,
                             shader->ngg.spi_shader_pgm_rsrc4_gs);
}

unsigned si_get_input_prim(const struct si_shader_selector *gs, const union si_shader_key *key,
                           bool return_unknown)
{
   if (gs->stage == MESA_SHADER_GEOMETRY)
      return gs->info.base.gs.input_primitive;

   if (gs->stage == MESA_SHADER_TESS_EVAL) {
      if (gs->info.base.tess.point_mode)
         return MESA_PRIM_POINTS;
      if (gs->info.base.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES)
         return MESA_PRIM_LINES;
      return MESA_PRIM_TRIANGLES;
   }

   assert(gs->stage == MESA_SHADER_VERTEX);

   if (key->ge.opt.ngg_culling & SI_NGG_CULL_VS_LINES)
      return MESA_PRIM_LINES;

   switch (key->ge.opt.ngg_vs_streamout_num_verts_per_prim) {
   case 3:
      return MESA_PRIM_TRIANGLES;
   case 2:
      return MESA_PRIM_LINES;
   case 1:
      return MESA_PRIM_POINTS;
   }

   if (return_unknown)
      return MESA_PRIM_UNKNOWN;
   else
      return MESA_PRIM_TRIANGLES; /* worst case for all callers */
}

/* Return a simplified primitive type, e.g. don't return *_STRIP and *_FAN.
 * This returns MESA_PRIM_UNKNOWN if the primitive type is not known at compile time.
 */
unsigned si_get_output_prim_simplified(const struct si_shader_selector *sel,
                                       const union si_shader_key *key)
{
   if (sel->stage == MESA_SHADER_GEOMETRY) {
      if (util_rast_prim_is_triangles(sel->info.base.gs.output_primitive))
         return MESA_PRIM_TRIANGLES;
      else if (util_prim_is_lines(sel->info.base.gs.output_primitive))
         return MESA_PRIM_LINES;
      else
         return MESA_PRIM_POINTS;
   }

   if (sel->stage == MESA_SHADER_VERTEX && sel->info.base.vs.blit_sgprs_amd)
      return SI_PRIM_RECTANGLE_LIST;

   /* It's the same as the input primitive type for VS and TES. */
   return si_get_input_prim(sel, key, true);
}

unsigned si_get_num_vertices_per_output_prim(struct si_shader *shader)
{
   unsigned prim = si_get_output_prim_simplified(shader->selector, &shader->key);

   switch (prim) {
   case MESA_PRIM_TRIANGLES:
   case SI_PRIM_RECTANGLE_LIST:
      return 3;
   case MESA_PRIM_LINES:
      return 2;
   case MESA_PRIM_POINTS:
      return 1;
   case MESA_PRIM_UNKNOWN:
      return 0;
   default:
      UNREACHABLE("unexpected prim type");
   }
}

static unsigned si_get_vs_out_cntl(const struct si_shader_selector *sel,
                                   const struct si_shader *shader, bool ngg)
{
   unsigned num_clip_distances = util_bitcount(shader->info.clipdist_mask | shader->info.culldist_mask);
   bool writes_psize = sel->info.writes_psize && !shader->key.ge.opt.kill_pointsize;
   bool writes_layer = sel->info.writes_layer && !shader->key.ge.opt.kill_layer;
   bool misc_vec_ena = writes_psize || (sel->info.writes_edgeflag && !ngg) ||
                       writes_layer || sel->info.writes_viewport_index ||
                       sel->screen->options.vrs2x2;

   return S_02881C_VS_OUT_CCDIST0_VEC_ENA(num_clip_distances > 0) |
          S_02881C_VS_OUT_CCDIST1_VEC_ENA(num_clip_distances > 4) |
          S_02881C_USE_VTX_POINT_SIZE(writes_psize) |
          S_02881C_USE_VTX_EDGE_FLAG(sel->info.writes_edgeflag && !ngg) |
          S_02881C_USE_VTX_VRS_RATE(sel->screen->options.vrs2x2) |
          S_02881C_USE_VTX_RENDER_TARGET_INDX(writes_layer) |
          S_02881C_USE_VTX_VIEWPORT_INDX(sel->info.writes_viewport_index) |
          S_02881C_VS_OUT_MISC_VEC_ENA(misc_vec_ena) |
          S_02881C_VS_OUT_MISC_SIDE_BUS_ENA(misc_vec_ena ||
                                            (sel->screen->info.gfx_level >= GFX10_3 &&
                                             shader->info.nr_pos_exports > 1));
}

/* Return the number of allocated param exports. This can be more than the number of param
 * exports in the shader.
 */
unsigned si_shader_num_alloc_param_exports(struct si_shader *shader)
{
   unsigned num_params = shader->info.nr_param_exports;

   /* Since there is no alloc/dealloc mechanism for the 12-bit ordered IDs on GFX12, they can wrap
    * around if there are more than 2^12 workgroups, causing 2 workgroups to get the same
    * ordered ID, which can deadlock the "ordered add" loop.
    *
    * The recommended solution is to use the alloc/dealloc mechanism of the attribute ring to limit
    * the number of workgroups in flight and thus the number of ordered IDs in flight.
    */
   if (shader->selector->screen->info.gfx_level >= GFX12 && shader->info.num_streamout_vec4s)
      num_params = MAX2(num_params, 8);

   return num_params;
}

/**
 * Prepare the PM4 image for \p shader, which will run as a merged ESGS shader
 * in NGG mode.
 */
static void gfx10_shader_ngg(struct si_screen *sscreen, struct si_shader *shader)
{
   const struct si_shader_selector *gs_sel = shader->selector;
   const struct si_shader_info *gs_info = &gs_sel->info;
   const gl_shader_stage gs_stage = shader->selector->stage;
   const struct si_shader_selector *es_sel =
      shader->previous_stage_sel ? shader->previous_stage_sel : shader->selector;
   const struct si_shader_info *es_info = &es_sel->info;
   const gl_shader_stage es_stage = es_sel->stage;
   unsigned num_user_sgprs;
   unsigned es_vgpr_comp_cnt, gs_vgpr_comp_cnt;
   uint64_t va;
   bool window_space = gs_sel->stage == MESA_SHADER_VERTEX ?
                          gs_info->base.vs.window_space_position : 0;
   bool es_enable_prim_id = shader->key.ge.mono.u.vs_export_prim_id || es_info->uses_primid;
   unsigned gs_num_invocations = gs_sel->stage == MESA_SHADER_GEOMETRY ?
                                    CLAMP(gs_info->base.gs.invocations, 1, 32) : 0;
   unsigned input_prim = si_get_input_prim(gs_sel, &shader->key, false);
   unsigned gs_input_verts_per_prim = gs_sel->stage == MESA_SHADER_GEOMETRY ?
                                         mesa_vertices_per_prim(gs_sel->info.base.gs.input_primitive) : 0;

   struct si_pm4_state *pm4 = si_get_shader_pm4_state(shader, NULL);
   if (!pm4)
      return;

   if (sscreen->info.gfx_level >= GFX12) {
      if (es_stage == MESA_SHADER_TESS_EVAL)
         pm4->atom.emit = gfx12_emit_shader_ngg<TESS_ON>;
      else
         pm4->atom.emit = gfx12_emit_shader_ngg<TESS_OFF>;
   } else if (sscreen->info.has_set_context_pairs_packed) {
      if (es_stage == MESA_SHADER_TESS_EVAL)
         pm4->atom.emit = gfx11_dgpu_emit_shader_ngg<TESS_ON>;
      else
         pm4->atom.emit = gfx11_dgpu_emit_shader_ngg<TESS_OFF>;
   } else {
      if (es_stage == MESA_SHADER_TESS_EVAL)
         pm4->atom.emit = gfx10_emit_shader_ngg<TESS_ON>;
      else
         pm4->atom.emit = gfx10_emit_shader_ngg<TESS_OFF>;
   }

   va = shader->bo->gpu_address;

   if (es_stage == MESA_SHADER_VERTEX) {
      es_vgpr_comp_cnt = si_get_vs_vgpr_comp_cnt(sscreen, shader, false);

      if (es_info->base.vs.blit_sgprs_amd) {
         num_user_sgprs =
            SI_SGPR_VS_BLIT_DATA + es_info->base.vs.blit_sgprs_amd;
      } else {
         num_user_sgprs = si_get_num_vs_user_sgprs(shader, GFX9_GS_NUM_USER_SGPR);
      }
   } else {
      assert(es_stage == MESA_SHADER_TESS_EVAL);
      es_vgpr_comp_cnt = es_enable_prim_id ? 3 : 2;
      num_user_sgprs = GFX9_GS_NUM_USER_SGPR;
   }

   /* Primitives with adjancency can only occur without tessellation. */
   assert(gs_input_verts_per_prim <= 3 || es_stage == MESA_SHADER_VERTEX);

   if (sscreen->info.gfx_level >= GFX12) {
      if (gs_input_verts_per_prim >= 4)
         gs_vgpr_comp_cnt = 2; /* VGPR2 contains offsets 3-5 */
      else if ((gs_stage == MESA_SHADER_GEOMETRY && gs_info->uses_primid) ||
               (gs_stage == MESA_SHADER_VERTEX && shader->key.ge.mono.u.vs_export_prim_id))
         gs_vgpr_comp_cnt = 1; /* VGPR1 contains PrimitiveID */
      else
         gs_vgpr_comp_cnt = 0; /* VGPR0 contains offsets 0-2, edgeflags, GS invocation ID. */
   } else {
      /* If offsets 4, 5 are used, GS_VGPR_COMP_CNT is ignored and
       * VGPR[0:4] are always loaded.
       *
       * Vertex shaders always need to load VGPR3, because they need to
       * pass edge flags for decomposed primitives (such as quads) to the PA
       * for the GL_LINE polygon mode to skip rendering lines on inner edges.
       */
      if (gs_info->uses_invocationid ||
          (gfx10_has_variable_edgeflags(shader) && !gfx10_is_ngg_passthrough(shader)))
         gs_vgpr_comp_cnt = 3; /* VGPR3 contains InvocationID, edge flags. */
      else if ((gs_stage == MESA_SHADER_GEOMETRY && gs_info->uses_primid) ||
               (gs_stage == MESA_SHADER_VERTEX && shader->key.ge.mono.u.vs_export_prim_id))
         gs_vgpr_comp_cnt = 2; /* VGPR2 contains PrimitiveID. */
      else if (input_prim >= MESA_PRIM_TRIANGLES && !gfx10_is_ngg_passthrough(shader))
         gs_vgpr_comp_cnt = 1; /* VGPR1 contains offsets 2, 3 */
      else
         gs_vgpr_comp_cnt = 0; /* VGPR0 contains offsets 0, 1 */
   }

   if (sscreen->info.gfx_level >= GFX12) {
      ac_pm4_set_reg(&pm4->base, R_00B224_SPI_SHADER_PGM_LO_ES, va >> 8);
   } else {
      ac_pm4_set_reg(&pm4->base, R_00B320_SPI_SHADER_PGM_LO_ES, va >> 8);
   }

   ac_pm4_set_reg(&pm4->base, R_00B228_SPI_SHADER_PGM_RSRC1_GS,
                  S_00B228_VGPRS(si_shader_encode_vgprs(shader)) |
                  S_00B228_FLOAT_MODE(shader->config.float_mode) |
                  S_00B228_DX10_CLAMP(sscreen->info.gfx_level < GFX12) |
                  S_00B228_MEM_ORDERED(si_shader_mem_ordered(shader)) |
                  S_00B228_GS_VGPR_COMP_CNT(gs_vgpr_comp_cnt));
   ac_pm4_set_reg(&pm4->base, R_00B22C_SPI_SHADER_PGM_RSRC2_GS,
                  S_00B22C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0) |
                  S_00B22C_USER_SGPR(num_user_sgprs) |
                  S_00B22C_ES_VGPR_COMP_CNT(es_vgpr_comp_cnt) |
                  S_00B22C_OC_LDS_EN(es_stage == MESA_SHADER_TESS_EVAL) |
                  S_00B22C_LDS_SIZE(shader->config.lds_size) |
                  S_00B22C_USER_SGPR_MSB_GFX10(num_user_sgprs >> 5) |
                  S_00B22C_SHARED_VGPR_CNT(shader->config.num_shared_vgprs / 8));

   /* Set register values emitted conditionally in gfx10_emit_shader_ngg_*. */
   shader->ngg.spi_shader_pos_format =
      S_02870C_POS0_EXPORT_FORMAT(V_02870C_SPI_SHADER_4COMP) |
      S_02870C_POS1_EXPORT_FORMAT(shader->info.nr_pos_exports > 1 ? V_02870C_SPI_SHADER_4COMP
                                                                  : V_02870C_SPI_SHADER_NONE) |
      S_02870C_POS2_EXPORT_FORMAT(shader->info.nr_pos_exports > 2 ? V_02870C_SPI_SHADER_4COMP
                                                                  : V_02870C_SPI_SHADER_NONE) |
      S_02870C_POS3_EXPORT_FORMAT(shader->info.nr_pos_exports > 3 ? V_02870C_SPI_SHADER_4COMP
                                                                  : V_02870C_SPI_SHADER_NONE);
   shader->ngg.ge_max_output_per_subgroup = S_0287FC_MAX_VERTS_PER_SUBGROUP(shader->ngg.info.max_out_verts);
   shader->ngg.vgt_gs_instance_cnt =
      S_028B90_ENABLE(gs_num_invocations > 1) |
      S_028B90_CNT(gs_num_invocations) |
      S_028B90_EN_MAX_VERT_OUT_PER_GS_INSTANCE(shader->ngg.info.max_vert_out_per_gs_instance);
   shader->pa_cl_vs_out_cntl = si_get_vs_out_cntl(shader->selector, shader, true);

   if (gs_stage == MESA_SHADER_GEOMETRY) {
      shader->ngg.esgs_vertex_stride = es_sel->info.esgs_vertex_stride / 4;
      shader->ngg.vgt_gs_max_vert_out = gs_sel->info.base.gs.vertices_out;
      shader->ngg.ge_ngg_subgrp_cntl = S_028B4C_PRIM_AMP_FACTOR(gs_sel->info.base.gs.vertices_out);
   } else {
      shader->ngg.esgs_vertex_stride = 1;
      shader->ngg.vgt_gs_max_vert_out = 1;
      shader->ngg.ge_ngg_subgrp_cntl = S_028B4C_PRIM_AMP_FACTOR(1);
   }

   if (es_stage == MESA_SHADER_TESS_EVAL)
      si_set_tesseval_regs(sscreen, es_sel, shader);

   shader->ngg.vgt_primitiveid_en =
      S_028A84_NGG_DISABLE_PROVOK_REUSE(shader->key.ge.mono.u.vs_export_prim_id ||
                                        gs_sel->info.writes_primid);

   if (sscreen->info.gfx_level >= GFX12) {
      unsigned num_params = si_shader_num_alloc_param_exports(shader);
      unsigned wave_limit_per_se = 0x3ff;

      /* This tuning adds up to 50% streamout performance. */
      if (shader->info.num_streamout_vec4s) {
         unsigned num_streamout_vec4s = shader->info.num_streamout_vec4s;

         /* TODO: Tested on a pre-production chip. Re-test on the final chip. */
         if (num_streamout_vec4s <= 4)
            wave_limit_per_se = 48;
         else if (num_streamout_vec4s <= 5)
            wave_limit_per_se = 24;
         else if (num_streamout_vec4s <= 6)
            wave_limit_per_se = 20;
         else if (num_streamout_vec4s <= 8)
            wave_limit_per_se = 18;
         else if (num_streamout_vec4s <= 11)
            wave_limit_per_se = 17;
         else if (num_streamout_vec4s <= 12)
            wave_limit_per_se = 16;
         else if (num_streamout_vec4s <= 15)
            wave_limit_per_se = 15;
         else
            wave_limit_per_se = 14;
      }

      shader->ngg.spi_shader_pgm_rsrc4_gs = S_00B220_SPI_SHADER_LATE_ALLOC_GS(127) |
                                            S_00B220_GLG_FORCE_DISABLE(1) |
                                            S_00B220_WAVE_LIMIT(wave_limit_per_se) |
                                            S_00B220_INST_PREF_SIZE(si_get_shader_prefetch_size(shader));
      shader->ngg.spi_vs_out_config = S_00B0C4_VS_EXPORT_COUNT(MAX2(num_params, 1) - 1) |
                                      S_00B0C4_NO_PC_EXPORT(num_params == 0);
   } else {
      unsigned late_alloc_wave64, cu_mask;

      ac_compute_late_alloc(&sscreen->info, true, si_shader_culling_enabled(shader),
                            shader->config.scratch_bytes_per_wave > 0,
                            &late_alloc_wave64, &cu_mask);

      /* Oversubscribe PC. This improves performance when there are too many varyings. */
      unsigned oversub_pc_lines, oversub_pc_factor = 1;

      if (si_shader_culling_enabled(shader)) {
         /* Be more aggressive with NGG culling. */
         if (shader->info.nr_param_exports > 4)
            oversub_pc_factor = 4;
         else if (shader->info.nr_param_exports > 2)
            oversub_pc_factor = 3;
         else
            oversub_pc_factor = 2;
      }
      oversub_pc_lines = late_alloc_wave64 ? (sscreen->info.pc_lines / 4) * oversub_pc_factor : 0;
      shader->ngg.ge_pc_alloc = S_030980_OVERSUB_EN(oversub_pc_lines > 0) |
                                S_030980_NUM_PC_LINES(oversub_pc_lines - 1);
      shader->ngg.vgt_primitiveid_en |= S_028A84_PRIMITIVEID_EN(es_enable_prim_id);
      shader->ngg.spi_shader_pgm_rsrc3_gs =
         ac_apply_cu_en(S_00B21C_CU_EN(cu_mask) |
                        S_00B21C_WAVE_LIMIT(0x3F),
                        C_00B21C_CU_EN, 0, &sscreen->info);
      shader->ngg.spi_shader_pgm_rsrc4_gs = S_00B204_SPI_SHADER_LATE_ALLOC_GS_GFX10(late_alloc_wave64);
      shader->ngg.spi_vs_out_config =
         S_0286C4_VS_EXPORT_COUNT(MAX2(shader->info.nr_param_exports, 1) - 1) |
         S_0286C4_NO_PC_EXPORT(shader->info.nr_param_exports == 0);

      if (sscreen->info.gfx_level >= GFX11) {
         shader->ngg.spi_shader_pgm_rsrc4_gs |=
            ac_apply_cu_en(S_00B204_CU_EN_GFX11(0x1) |
                           S_00B204_INST_PREF_SIZE(si_get_shader_prefetch_size(shader)),
                           C_00B204_CU_EN_GFX11, 16, &sscreen->info);
      } else {
         shader->ngg.spi_shader_pgm_rsrc4_gs |=
            ac_apply_cu_en(S_00B204_CU_EN_GFX10(0xffff),
                           C_00B204_CU_EN_GFX10, 16, &sscreen->info);
      }
   }

   if (sscreen->info.gfx_level >= GFX11) {
      /* This should be <= 252 for performance on Gfx11. 256 works too but is slower. */
      unsigned max_prim_grp_size = sscreen->info.gfx_level >= GFX12 ? 256 : 252;
      unsigned prim_amp_factor = gs_stage == MESA_SHADER_GEOMETRY ?
                                    gs_sel->info.base.gs.vertices_out : 1;

      shader->ge_cntl = S_03096C_PRIMS_PER_SUBGRP(shader->ngg.info.max_gsprims) |
                        S_03096C_VERTS_PER_SUBGRP(shader->ngg.info.hw_max_esverts) |
                        S_03096C_PRIM_GRP_SIZE_GFX11(
                           CLAMP(max_prim_grp_size / MAX2(prim_amp_factor, 1), 1, 256)) |
                        S_03096C_DIS_PG_SIZE_ADJUST_FOR_STRIP(sscreen->info.gfx_level >= GFX12);
   } else {
      shader->ge_cntl = S_03096C_PRIM_GRP_SIZE_GFX10(shader->ngg.info.max_gsprims) |
                        S_03096C_VERT_GRP_SIZE(shader->ngg.info.hw_max_esverts);

      shader->ngg.vgt_gs_onchip_cntl =
         S_028A44_ES_VERTS_PER_SUBGRP(shader->ngg.info.hw_max_esverts) |
         S_028A44_GS_PRIMS_PER_SUBGRP(shader->ngg.info.max_gsprims) |
         S_028A44_GS_INST_PRIMS_IN_SUBGRP(shader->ngg.info.max_gsprims * gs_num_invocations);

      /* On gfx10, the GE only checks against the maximum number of ES verts after
       * allocating a full GS primitive. So we need to ensure that whenever
       * this check passes, there is enough space for a full primitive without
       * vertex reuse. VERT_GRP_SIZE=256 doesn't need this. We should always get 256
       * if we have enough LDS.
       *
       * Tessellation is unaffected because it always sets GE_CNTL.VERT_GRP_SIZE = 0.
       */
      if ((sscreen->info.gfx_level == GFX10) &&
          (es_stage == MESA_SHADER_VERTEX || gs_stage == MESA_SHADER_VERTEX) && /* = no tess */
          shader->ngg.info.hw_max_esverts != 256 &&
          shader->ngg.info.hw_max_esverts > 5) {
         /* This could be based on the input primitive type. 5 is the worst case
          * for primitive types with adjacency.
          */
         shader->ge_cntl &= C_03096C_VERT_GRP_SIZE;
         shader->ge_cntl |= S_03096C_VERT_GRP_SIZE(shader->ngg.info.hw_max_esverts - 5);
      }
   }

   if (window_space) {
      shader->ngg.pa_cl_vte_cntl = S_028818_VTX_XY_FMT(1) | S_028818_VTX_Z_FMT(1);
   } else {
      shader->ngg.pa_cl_vte_cntl = S_028818_VTX_W0_FMT(1) |
                                   S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
                                   S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) |
                                   S_028818_VPORT_Z_SCALE_ENA(1) | S_028818_VPORT_Z_OFFSET_ENA(1);
   }

   if (sscreen->info.gfx_level >= GFX12) {
      shader->ngg.vgt_shader_stages_en =
         S_028A98_GS_EN(gs_stage == MESA_SHADER_GEOMETRY) |
         S_028A98_PRIMGEN_PASSTHRU_NO_MSG(gfx10_is_ngg_passthrough(shader)) |
         S_028A98_GS_W32_EN(shader->wave_size == 32) |
         S_028A98_NGG_WAVE_ID_EN(shader->info.num_streamout_vec4s != 0);
   } else {
      shader->ngg.vgt_shader_stages_en =
         S_028B54_ES_EN(es_stage == MESA_SHADER_TESS_EVAL ?
                           V_028B54_ES_STAGE_DS : V_028B54_ES_STAGE_REAL) |
         S_028B54_GS_EN(gs_stage == MESA_SHADER_GEOMETRY) |
         S_028B54_PRIMGEN_EN(1) |
         S_028B54_PRIMGEN_PASSTHRU_EN(gfx10_is_ngg_passthrough(shader)) |
         S_028B54_PRIMGEN_PASSTHRU_NO_MSG(gfx10_is_ngg_passthrough(shader) &&
                                          sscreen->info.family >= CHIP_NAVI23) |
         S_028B54_NGG_WAVE_ID_EN(shader->info.num_streamout_vec4s != 0) |
         S_028B54_GS_W32_EN(shader->wave_size == 32) |
         S_028B54_MAX_PRIMGRP_IN_WAVE(2);
   }

   ac_pm4_finalize(&pm4->base);
}

static void si_emit_shader_vs(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.vs;

   radeon_begin(&sctx->gfx_cs);
   radeon_opt_set_context_reg(R_028A40_VGT_GS_MODE, SI_TRACKED_VGT_GS_MODE,
                              shader->vs.vgt_gs_mode);
   radeon_opt_set_context_reg(R_028A84_VGT_PRIMITIVEID_EN, SI_TRACKED_VGT_PRIMITIVEID_EN,
                              shader->vs.vgt_primitiveid_en);

   if (sctx->gfx_level <= GFX8) {
      radeon_opt_set_context_reg(R_028AB4_VGT_REUSE_OFF, SI_TRACKED_VGT_REUSE_OFF,
                                 shader->vs.vgt_reuse_off);
   }

   radeon_opt_set_context_reg(R_0286C4_SPI_VS_OUT_CONFIG, SI_TRACKED_SPI_VS_OUT_CONFIG,
                              shader->vs.spi_vs_out_config);

   radeon_opt_set_context_reg(R_02870C_SPI_SHADER_POS_FORMAT,
                              SI_TRACKED_SPI_SHADER_POS_FORMAT,
                              shader->vs.spi_shader_pos_format);

   radeon_opt_set_context_reg(R_028818_PA_CL_VTE_CNTL, SI_TRACKED_PA_CL_VTE_CNTL,
                              shader->vs.pa_cl_vte_cntl);

   if (shader->selector->stage == MESA_SHADER_TESS_EVAL)
      radeon_opt_set_context_reg(R_028B6C_VGT_TF_PARAM, SI_TRACKED_VGT_TF_PARAM,
                                 shader->vgt_tf_param);

   if (shader->vgt_vertex_reuse_block_cntl)
      radeon_opt_set_context_reg(R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL,
                                 SI_TRACKED_VGT_VERTEX_REUSE_BLOCK_CNTL,
                                 shader->vgt_vertex_reuse_block_cntl);

   /* Required programming for tessellation. (legacy pipeline only) */
   if (sctx->gfx_level >= GFX10 && shader->selector->stage == MESA_SHADER_TESS_EVAL) {
      radeon_opt_set_context_reg(R_028A44_VGT_GS_ONCHIP_CNTL,
                                 SI_TRACKED_VGT_GS_ONCHIP_CNTL,
                                 S_028A44_ES_VERTS_PER_SUBGRP(250) |
                                 S_028A44_GS_PRIMS_PER_SUBGRP(126) |
                                 S_028A44_GS_INST_PRIMS_IN_SUBGRP(126));
   }

   radeon_end_update_context_roll();

   /* GE_PC_ALLOC is not a context register, so it doesn't cause a context roll. */
   if (sctx->gfx_level >= GFX10) {
      radeon_begin_again(&sctx->gfx_cs);
      radeon_opt_set_uconfig_reg(R_030980_GE_PC_ALLOC, SI_TRACKED_GE_PC_ALLOC,
                                 shader->vs.ge_pc_alloc);
      radeon_end();
   }
}

/**
 * Compute the state for \p shader, which will run as a vertex shader on the
 * hardware.
 *
 * If \p gs is non-NULL, it points to the geometry shader for which this shader
 * is the copy shader.
 */
static void si_shader_vs(struct si_screen *sscreen, struct si_shader *shader,
                         struct si_shader_selector *gs)
{
   const struct si_shader_info *info = &shader->selector->info;
   struct si_pm4_state *pm4;
   unsigned num_user_sgprs, vgpr_comp_cnt;
   uint64_t va;
   unsigned nparams, oc_lds_en;
   bool window_space = shader->selector->stage == MESA_SHADER_VERTEX ?
                          info->base.vs.window_space_position : 0;
   bool enable_prim_id = shader->key.ge.mono.u.vs_export_prim_id || info->uses_primid;

   assert(sscreen->info.gfx_level < GFX11);

   pm4 = si_get_shader_pm4_state(shader, si_emit_shader_vs);
   if (!pm4)
      return;

   /* We always write VGT_GS_MODE in the VS state, because every switch
    * between different shader pipelines involving a different GS or no
    * GS at all involves a switch of the VS (different GS use different
    * copy shaders). On the other hand, when the API switches from a GS to
    * no GS and then back to the same GS used originally, the GS state is
    * not sent again.
    */
   if (!gs) {
      unsigned mode = V_028A40_GS_OFF;

      /* PrimID needs GS scenario A. */
      if (enable_prim_id)
         mode = V_028A40_GS_SCENARIO_A;

      shader->vs.vgt_gs_mode = S_028A40_MODE(mode);
      shader->vs.vgt_primitiveid_en = enable_prim_id;
   } else {
      shader->vs.vgt_gs_mode =
         ac_vgt_gs_mode(gs->info.base.gs.vertices_out, sscreen->info.gfx_level);
      shader->vs.vgt_primitiveid_en = 0;
   }

   if (sscreen->info.gfx_level <= GFX8) {
      /* Reuse needs to be set off if we write oViewport. */
      shader->vs.vgt_reuse_off = S_028AB4_REUSE_OFF(info->writes_viewport_index);
   }

   va = shader->bo->gpu_address;

   if (gs) {
      vgpr_comp_cnt = 0; /* only VertexID is needed for GS-COPY. */
      num_user_sgprs = SI_GSCOPY_NUM_USER_SGPR;
   } else if (shader->selector->stage == MESA_SHADER_VERTEX) {
      vgpr_comp_cnt = si_get_vs_vgpr_comp_cnt(sscreen, shader, enable_prim_id);

      if (info->base.vs.blit_sgprs_amd) {
         num_user_sgprs = SI_SGPR_VS_BLIT_DATA + info->base.vs.blit_sgprs_amd;
      } else {
         num_user_sgprs = si_get_num_vs_user_sgprs(shader, SI_VS_NUM_USER_SGPR);
      }
   } else if (shader->selector->stage == MESA_SHADER_TESS_EVAL) {
      vgpr_comp_cnt = enable_prim_id ? 3 : 2;
      num_user_sgprs = SI_TES_NUM_USER_SGPR;
   } else
      UNREACHABLE("invalid shader selector type");

   /* VS is required to export at least one param. */
   nparams = MAX2(shader->info.nr_param_exports, 1);
   shader->vs.spi_vs_out_config = S_0286C4_VS_EXPORT_COUNT(nparams - 1);

   if (sscreen->info.gfx_level >= GFX10) {
      shader->vs.spi_vs_out_config |=
         S_0286C4_NO_PC_EXPORT(shader->info.nr_param_exports == 0);
   }

   shader->vs.spi_shader_pos_format =
      S_02870C_POS0_EXPORT_FORMAT(V_02870C_SPI_SHADER_4COMP) |
      S_02870C_POS1_EXPORT_FORMAT(shader->info.nr_pos_exports > 1 ? V_02870C_SPI_SHADER_4COMP
                                                                  : V_02870C_SPI_SHADER_NONE) |
      S_02870C_POS2_EXPORT_FORMAT(shader->info.nr_pos_exports > 2 ? V_02870C_SPI_SHADER_4COMP
                                                                  : V_02870C_SPI_SHADER_NONE) |
      S_02870C_POS3_EXPORT_FORMAT(shader->info.nr_pos_exports > 3 ? V_02870C_SPI_SHADER_4COMP
                                                                  : V_02870C_SPI_SHADER_NONE);
   unsigned late_alloc_wave64, cu_mask;
   ac_compute_late_alloc(&sscreen->info, false, false,
                         shader->config.scratch_bytes_per_wave > 0,
                         &late_alloc_wave64, &cu_mask);

   shader->vs.ge_pc_alloc = S_030980_OVERSUB_EN(late_alloc_wave64 > 0) |
                            S_030980_NUM_PC_LINES(sscreen->info.pc_lines / 4 - 1);
   shader->pa_cl_vs_out_cntl = si_get_vs_out_cntl(shader->selector, shader, false);

   oc_lds_en = shader->selector->stage == MESA_SHADER_TESS_EVAL ? 1 : 0;

   if (sscreen->info.gfx_level >= GFX7) {
      ac_pm4_set_reg_idx3(&pm4->base, R_00B118_SPI_SHADER_PGM_RSRC3_VS,
                          ac_apply_cu_en(S_00B118_CU_EN(cu_mask) |
                                         S_00B118_WAVE_LIMIT(0x3F),
                                         C_00B118_CU_EN, 0, &sscreen->info));
      ac_pm4_set_reg(&pm4->base, R_00B11C_SPI_SHADER_LATE_ALLOC_VS, S_00B11C_LIMIT(late_alloc_wave64));
   }

   ac_pm4_set_reg(&pm4->base, R_00B120_SPI_SHADER_PGM_LO_VS, va >> 8);
   ac_pm4_set_reg(&pm4->base, R_00B124_SPI_SHADER_PGM_HI_VS,
                  S_00B124_MEM_BASE(sscreen->info.address32_hi >> 8));

   uint32_t rsrc1 =
      S_00B128_VGPRS(si_shader_encode_vgprs(shader)) |
      S_00B128_SGPRS(si_shader_encode_sgprs(shader)) |
      S_00B128_VGPR_COMP_CNT(vgpr_comp_cnt) |
      S_00B128_DX10_CLAMP(1) |
      S_00B128_MEM_ORDERED(si_shader_mem_ordered(shader)) |
      S_00B128_FLOAT_MODE(shader->config.float_mode);
   uint32_t rsrc2 = S_00B12C_USER_SGPR(num_user_sgprs) | S_00B12C_OC_LDS_EN(oc_lds_en) |
                    S_00B12C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0);

   if (sscreen->info.gfx_level >= GFX10)
      rsrc2 |= S_00B12C_USER_SGPR_MSB_GFX10(num_user_sgprs >> 5) |
               S_00B12C_SHARED_VGPR_CNT(shader->config.num_shared_vgprs / 8);
   else if (sscreen->info.gfx_level == GFX9)
      rsrc2 |= S_00B12C_USER_SGPR_MSB_GFX9(num_user_sgprs >> 5);

   if (shader->info.num_streamout_vec4s) {
      rsrc2 |= S_00B12C_SO_BASE0_EN(!!shader->selector->info.base.xfb_stride[0]) |
               S_00B12C_SO_BASE1_EN(!!shader->selector->info.base.xfb_stride[1]) |
               S_00B12C_SO_BASE2_EN(!!shader->selector->info.base.xfb_stride[2]) |
               S_00B12C_SO_BASE3_EN(!!shader->selector->info.base.xfb_stride[3]) |
               S_00B12C_SO_EN(1);
   }

   ac_pm4_set_reg(&pm4->base, R_00B128_SPI_SHADER_PGM_RSRC1_VS, rsrc1);
   ac_pm4_set_reg(&pm4->base, R_00B12C_SPI_SHADER_PGM_RSRC2_VS, rsrc2);

   if (window_space)
      shader->vs.pa_cl_vte_cntl = S_028818_VTX_XY_FMT(1) | S_028818_VTX_Z_FMT(1);
   else
      shader->vs.pa_cl_vte_cntl =
         S_028818_VTX_W0_FMT(1) | S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
         S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) |
         S_028818_VPORT_Z_SCALE_ENA(1) | S_028818_VPORT_Z_OFFSET_ENA(1);

   if (shader->selector->stage == MESA_SHADER_TESS_EVAL)
      si_set_tesseval_regs(sscreen, shader->selector, shader);

   polaris_set_vgt_vertex_reuse(sscreen, shader->selector, shader);
   ac_pm4_finalize(&pm4->base);
}

static unsigned si_get_spi_shader_col_format(struct si_shader *shader)
{
   unsigned spi_shader_col_format = shader->key.ps.part.epilog.spi_shader_col_format;
   unsigned value = 0, num_mrts = 0;
   unsigned i, num_targets = (util_last_bit(spi_shader_col_format) + 3) / 4;

   /* Remove holes in spi_shader_col_format. */
   for (i = 0; i < num_targets; i++) {
      unsigned spi_format = (spi_shader_col_format >> (i * 4)) & 0xf;

      if (spi_format) {
         value |= spi_format << (num_mrts * 4);
         num_mrts++;
      }
   }

   return value;
}

static void gfx6_emit_shader_ps(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.ps;

   radeon_begin(&sctx->gfx_cs);
   radeon_opt_set_context_reg2(R_0286CC_SPI_PS_INPUT_ENA, SI_TRACKED_SPI_PS_INPUT_ENA,
                               shader->ps.spi_ps_input_ena,
                               shader->ps.spi_ps_input_addr);
   radeon_opt_set_context_reg(R_0286D8_SPI_PS_IN_CONTROL, SI_TRACKED_SPI_PS_IN_CONTROL,
                              shader->ps.spi_ps_in_control);
   radeon_opt_set_context_reg2(R_028710_SPI_SHADER_Z_FORMAT, SI_TRACKED_SPI_SHADER_Z_FORMAT,
                               shader->ps.spi_shader_z_format,
                               shader->ps.spi_shader_col_format);
   radeon_opt_set_context_reg(R_02823C_CB_SHADER_MASK, SI_TRACKED_CB_SHADER_MASK,
                              shader->ps.cb_shader_mask);
   radeon_end_update_context_roll();
}

static void gfx11_dgpu_emit_shader_ps(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.ps;

   radeon_begin(&sctx->gfx_cs);
   gfx11_begin_packed_context_regs();
   gfx11_opt_set_context_reg(R_0286CC_SPI_PS_INPUT_ENA, SI_TRACKED_SPI_PS_INPUT_ENA,
                             shader->ps.spi_ps_input_ena);
   gfx11_opt_set_context_reg(R_0286D0_SPI_PS_INPUT_ADDR, SI_TRACKED_SPI_PS_INPUT_ADDR,
                             shader->ps.spi_ps_input_addr);
   gfx11_opt_set_context_reg(R_0286D8_SPI_PS_IN_CONTROL, SI_TRACKED_SPI_PS_IN_CONTROL,
                             shader->ps.spi_ps_in_control);
   gfx11_opt_set_context_reg(R_028710_SPI_SHADER_Z_FORMAT, SI_TRACKED_SPI_SHADER_Z_FORMAT,
                             shader->ps.spi_shader_z_format);
   gfx11_opt_set_context_reg(R_028714_SPI_SHADER_COL_FORMAT, SI_TRACKED_SPI_SHADER_COL_FORMAT,
                             shader->ps.spi_shader_col_format);
   gfx11_opt_set_context_reg(R_02823C_CB_SHADER_MASK, SI_TRACKED_CB_SHADER_MASK,
                             shader->ps.cb_shader_mask);
   gfx11_end_packed_context_regs();
   radeon_end(); /* don't track context rolls on GFX11 */
}

static void gfx12_emit_shader_ps(struct si_context *sctx, unsigned index)
{
   struct si_shader *shader = sctx->queued.named.ps;

   radeon_begin(&sctx->gfx_cs);
   gfx12_begin_context_regs();
   gfx12_opt_set_context_reg(R_028640_SPI_PS_IN_CONTROL, SI_TRACKED_SPI_PS_IN_CONTROL,
                             shader->ps.spi_ps_in_control);
   gfx12_opt_set_context_reg(R_028650_SPI_SHADER_Z_FORMAT, SI_TRACKED_SPI_SHADER_Z_FORMAT,
                             shader->ps.spi_shader_z_format);
   gfx12_opt_set_context_reg(R_028654_SPI_SHADER_COL_FORMAT, SI_TRACKED_SPI_SHADER_COL_FORMAT,
                             shader->ps.spi_shader_col_format);
   gfx12_opt_set_context_reg(R_02865C_SPI_PS_INPUT_ENA, SI_TRACKED_SPI_PS_INPUT_ENA,
                             shader->ps.spi_ps_input_ena);
   gfx12_opt_set_context_reg(R_028660_SPI_PS_INPUT_ADDR, SI_TRACKED_SPI_PS_INPUT_ADDR,
                             shader->ps.spi_ps_input_addr);
   gfx12_opt_set_context_reg(R_028854_CB_SHADER_MASK, SI_TRACKED_CB_SHADER_MASK,
                             shader->ps.cb_shader_mask);
   gfx12_opt_set_context_reg(R_028BBC_PA_SC_HISZ_CONTROL, SI_TRACKED_PA_SC_HISZ_CONTROL,
                             shader->ps.pa_sc_hisz_control);
   gfx12_end_context_regs();
   radeon_end(); /* don't track context rolls on GFX12 */
}

static void si_shader_ps(struct si_screen *sscreen, struct si_shader *shader)
{
   struct si_shader_info *info = &shader->selector->info;
   const unsigned input_ena = shader->config.spi_ps_input_ena;
   /* At least one of these is required to be set. */
   ASSERTED unsigned num_required_vgpr_inputs =
      G_0286CC_PERSP_SAMPLE_ENA(input_ena) + G_0286CC_PERSP_CENTER_ENA(input_ena) +
      G_0286CC_PERSP_CENTROID_ENA(input_ena) + G_0286CC_PERSP_PULL_MODEL_ENA(input_ena) +
      G_0286CC_LINEAR_SAMPLE_ENA(input_ena) + G_0286CC_LINEAR_CENTER_ENA(input_ena) +
      G_0286CC_LINEAR_CENTROID_ENA(input_ena) + G_0286CC_LINE_STIPPLE_TEX_ENA(input_ena);

   /* we need to enable at least one of them, otherwise we hang the GPU */
   assert(num_required_vgpr_inputs > 0);
   /* POS_W_FLOAT_ENA requires one of the perspective weights. */
   assert(!G_0286CC_POS_W_FLOAT_ENA(input_ena) || G_0286CC_PERSP_SAMPLE_ENA(input_ena) ||
          G_0286CC_PERSP_CENTER_ENA(input_ena) || G_0286CC_PERSP_CENTROID_ENA(input_ena) ||
          G_0286CC_PERSP_PULL_MODEL_ENA(input_ena));

   /* Validate interpolation optimization flags (read as implications). */
   assert(!shader->key.ps.part.prolog.bc_optimize_for_persp ||
          (G_0286CC_PERSP_CENTER_ENA(input_ena) && G_0286CC_PERSP_CENTROID_ENA(input_ena)));
   assert(!shader->key.ps.part.prolog.bc_optimize_for_linear ||
          (G_0286CC_LINEAR_CENTER_ENA(input_ena) && G_0286CC_LINEAR_CENTROID_ENA(input_ena)));
   assert(!shader->key.ps.part.prolog.force_persp_center_interp || num_required_vgpr_inputs == 1 ||
          (!G_0286CC_PERSP_SAMPLE_ENA(input_ena) && !G_0286CC_PERSP_CENTROID_ENA(input_ena)));
   assert(!shader->key.ps.part.prolog.force_linear_center_interp || num_required_vgpr_inputs == 1 ||
          (!G_0286CC_LINEAR_SAMPLE_ENA(input_ena) && !G_0286CC_LINEAR_CENTROID_ENA(input_ena)));
   assert(!shader->key.ps.part.prolog.force_persp_sample_interp || num_required_vgpr_inputs == 1 ||
          (!G_0286CC_PERSP_CENTER_ENA(input_ena) && !G_0286CC_PERSP_CENTROID_ENA(input_ena)));
   assert(!shader->key.ps.part.prolog.force_linear_sample_interp || num_required_vgpr_inputs == 1 ||
          (!G_0286CC_LINEAR_CENTER_ENA(input_ena) && !G_0286CC_LINEAR_CENTROID_ENA(input_ena)));

   /* color_two_side always enables FRONT_FACE. Since st/mesa disables two-side colors if the back
    * face is culled, the only case when both color_two_side and force_front_face_input can be set
    * is when the front face is culled (which means force_front_face_input == -1).
    */
   assert(!shader->key.ps.opt.force_front_face_input || !G_0286CC_FRONT_FACE_ENA(input_ena) ||
          (shader->key.ps.part.prolog.color_two_side &&
           shader->key.ps.opt.force_front_face_input == -1));

   /* Validate cases when the optimizations are off (read as implications). */
   assert(shader->key.ps.part.prolog.bc_optimize_for_persp ||
          !G_0286CC_PERSP_CENTER_ENA(input_ena) || !G_0286CC_PERSP_CENTROID_ENA(input_ena));
   assert(shader->key.ps.part.prolog.bc_optimize_for_linear ||
          !G_0286CC_LINEAR_CENTER_ENA(input_ena) || !G_0286CC_LINEAR_CENTROID_ENA(input_ena));

   /* DB_SHADER_CONTROL */
   shader->ps.db_shader_control =
      S_02880C_Z_EXPORT_ENABLE(shader->info.writes_z) |
      S_02880C_STENCIL_TEST_VAL_EXPORT_ENABLE(shader->info.writes_stencil) |
      S_02880C_MASK_EXPORT_ENABLE(shader->info.writes_sample_mask) |
      S_02880C_COVERAGE_TO_MASK_ENABLE(sscreen->info.gfx_level <= GFX10_3 &&
                                       shader->key.ps.part.epilog.alpha_to_coverage_via_mrtz) |
      S_02880C_KILL_ENABLE(shader->info.uses_discard);

   if (sscreen->info.gfx_level >= GFX12)
      shader->ps.pa_sc_hisz_control = S_028BBC_ROUND(2); /* required minimum value */

   switch (info->base.fs.depth_layout) {
   case FRAG_DEPTH_LAYOUT_GREATER:
      shader->ps.db_shader_control |= S_02880C_CONSERVATIVE_Z_EXPORT(V_02880C_EXPORT_GREATER_THAN_Z);
      if (sscreen->info.gfx_level >= GFX12)
         shader->ps.pa_sc_hisz_control |= S_028BBC_CONSERVATIVE_Z_EXPORT(V_028BBC_EXPORT_GREATER_THAN_Z);
      break;
   case FRAG_DEPTH_LAYOUT_LESS:
      shader->ps.db_shader_control |= S_02880C_CONSERVATIVE_Z_EXPORT(V_02880C_EXPORT_LESS_THAN_Z);
      if (sscreen->info.gfx_level >= GFX12)
         shader->ps.pa_sc_hisz_control |= S_028BBC_CONSERVATIVE_Z_EXPORT(V_028BBC_EXPORT_LESS_THAN_Z);
      break;
   default:;
   }

   /* Z_ORDER, EXEC_ON_HIER_FAIL and EXEC_ON_NOOP should be set as following:
    *
    *   | early Z/S | writes_mem | allow_ReZ? |      Z_ORDER       | EXEC_ON_HIER_FAIL | EXEC_ON_NOOP
    * --|-----------|------------|------------|--------------------|-------------------|-------------
    * 1a|   false   |   false    |   true     | EarlyZ_Then_ReZ    |         0         |     0
    * 1b|   false   |   false    |   false    | EarlyZ_Then_LateZ  |         0         |     0
    * 2 |   false   |   true     |   n/a      |       LateZ        |         1         |     0
    * 3 |   true    |   false    |   n/a      | EarlyZ_Then_LateZ  |         0         |     0
    * 4 |   true    |   true     |   n/a      | EarlyZ_Then_LateZ  |         0         |     1
    *
    * In cases 3 and 4, HW will force Z_ORDER to EarlyZ regardless of what's set in the register.
    * In case 2, NOOP_CULL is a don't care field. In case 2, 3 and 4, ReZ doesn't make sense.
    *
    * Don't use ReZ without profiling !!!
    *
    * ReZ decreases performance by 15% in DiRT: Showdown on Ultra settings, which has pretty complex
    * shaders.
    */
   if (info->base.fs.early_fragment_tests) {
      /* Cases 3, 4. */
      shader->ps.db_shader_control |= S_02880C_DEPTH_BEFORE_SHADER(1) |
                                      S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z) |
                                      S_02880C_EXEC_ON_NOOP(info->base.writes_memory);
   } else if (info->base.writes_memory) {
      /* Case 2. */
      shader->ps.db_shader_control |= S_02880C_Z_ORDER(V_02880C_LATE_Z) |
                                      S_02880C_EXEC_ON_HIER_FAIL(1);
   } else {
      /* Case 1. */
      shader->ps.db_shader_control |= S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z);
   }

   if (info->base.fs.post_depth_coverage)
      shader->ps.db_shader_control |= S_02880C_PRE_SHADER_DEPTH_COVERAGE_ENABLE(1);

   /* Bug workaround for smoothing (overrasterization) on GFX6. */
   if (sscreen->info.gfx_level == GFX6 && shader->key.ps.mono.poly_line_smoothing) {
      shader->ps.db_shader_control &= C_02880C_Z_ORDER;
      shader->ps.db_shader_control |= S_02880C_Z_ORDER(V_02880C_LATE_Z);
   }

   if (sscreen->info.has_rbplus && !sscreen->info.rbplus_allowed)
      shader->ps.db_shader_control |= S_02880C_DUAL_QUAD_DISABLE(1);

   shader->ps.spi_shader_col_format = si_get_spi_shader_col_format(shader);
   shader->ps.cb_shader_mask = ac_get_cb_shader_mask(shader->key.ps.part.epilog.spi_shader_col_format);
   shader->ps.spi_ps_input_ena = shader->config.spi_ps_input_ena;
   shader->ps.spi_ps_input_addr = shader->config.spi_ps_input_addr;
   shader->ps.num_interp = si_get_ps_num_interp(shader);
   shader->ps.spi_shader_z_format =
      ac_get_spi_shader_z_format(shader->info.writes_z, shader->info.writes_stencil,
                                 shader->info.writes_sample_mask,
                                 shader->key.ps.part.epilog.alpha_to_coverage_via_mrtz);

   /* Ensure that some export memory is always allocated, for two reasons:
    *
    * 1) Correctness: The hardware ignores the EXEC mask if no export
    *    memory is allocated, so KILL and alpha test do not work correctly
    *    without this.
    * 2) Performance: Every shader needs at least a NULL export, even when
    *    it writes no color/depth output. The NULL export instruction
    *    stalls without this setting.
    *
    * Don't add this to CB_SHADER_MASK.
    *
    * GFX10 supports pixel shaders without exports by setting both
    * the color and Z formats to SPI_SHADER_ZERO. The hw will skip export
    * instructions if any are present.
    *
    * RB+ depth-only rendering requires SPI_SHADER_32_R.
    */
   bool has_mrtz = shader->ps.spi_shader_z_format != V_028710_SPI_SHADER_ZERO;

   if (!shader->ps.spi_shader_col_format) {
      if (shader->key.ps.part.epilog.rbplus_depth_only_opt) {
         shader->ps.spi_shader_col_format = V_028714_SPI_SHADER_32_R;
      } else if (!has_mrtz) {
         if (sscreen->info.gfx_level >= GFX10) {
            if (G_02880C_KILL_ENABLE(shader->ps.db_shader_control))
               shader->ps.spi_shader_col_format = V_028714_SPI_SHADER_32_R;
         } else {
            shader->ps.spi_shader_col_format = V_028714_SPI_SHADER_32_R;
         }
      }
   }

   if (sscreen->info.gfx_level >= GFX12) {
      shader->ps.spi_ps_in_control = S_028640_PARAM_GEN(shader->key.ps.mono.point_smoothing) |
                                     S_028640_PS_W32_EN(shader->wave_size == 32);
      shader->ps.spi_gs_out_config_ps = S_00B0C4_NUM_INTERP(shader->ps.num_interp);
   } else {
      /* Enable PARAM_GEN for point smoothing.
       * Gfx11 workaround when there are no PS inputs but LDS is used.
       */
      bool param_gen = shader->key.ps.mono.point_smoothing ||
                       (sscreen->info.gfx_level == GFX11 && !shader->ps.num_interp &&
                        shader->config.lds_size);

      shader->ps.spi_ps_in_control = S_0286D8_NUM_INTERP(shader->ps.num_interp) |
                                     S_0286D8_PARAM_GEN(param_gen) |
                                     S_0286D8_PS_W32_EN(shader->wave_size == 32);
   }

   struct si_pm4_state *pm4 = si_get_shader_pm4_state(shader, NULL);
   if (!pm4)
      return;

   if (sscreen->info.gfx_level >= GFX12)
      pm4->atom.emit = gfx12_emit_shader_ps;
   else if (sscreen->info.has_set_context_pairs_packed)
      pm4->atom.emit = gfx11_dgpu_emit_shader_ps;
   else
      pm4->atom.emit = gfx6_emit_shader_ps;

   /* If multiple state sets are allowed to be in a bin, break the batch on a new PS. */
   if (sscreen->dpbb_allowed &&
       (sscreen->pbb_context_states_per_bin > 1 ||
        sscreen->pbb_persistent_states_per_bin > 1)) {
      ac_pm4_cmd_add(&pm4->base, PKT3(PKT3_EVENT_WRITE, 0, 0));
      ac_pm4_cmd_add(&pm4->base, EVENT_TYPE(V_028A90_BREAK_BATCH) | EVENT_INDEX(0));
   }

   if (sscreen->info.gfx_level >= GFX12) {
      ac_pm4_set_reg(&pm4->base, R_00B01C_SPI_SHADER_PGM_RSRC4_PS,
                     S_00B01C_WAVE_LIMIT_GFX12(0x3FF) |
                     S_00B01C_LDS_GROUP_SIZE_GFX12(1) |
                     S_00B01C_INST_PREF_SIZE(si_get_shader_prefetch_size(shader)));
   } else if (sscreen->info.gfx_level >= GFX11) {
      unsigned cu_mask_ps = ac_gfx103_get_cu_mask_ps(&sscreen->info);

      ac_pm4_set_reg_idx3(&pm4->base, R_00B004_SPI_SHADER_PGM_RSRC4_PS,
                          ac_apply_cu_en(S_00B004_CU_EN(cu_mask_ps >> 16) |
                                         S_00B004_INST_PREF_SIZE(si_get_shader_prefetch_size(shader)),
                                         C_00B004_CU_EN, 16, &sscreen->info));
   }

   uint64_t va = shader->bo->gpu_address;
   ac_pm4_set_reg(&pm4->base, R_00B020_SPI_SHADER_PGM_LO_PS, va >> 8);
   ac_pm4_set_reg(&pm4->base, R_00B024_SPI_SHADER_PGM_HI_PS,
                  S_00B024_MEM_BASE(sscreen->info.address32_hi >> 8));

   ac_pm4_set_reg(&pm4->base, R_00B028_SPI_SHADER_PGM_RSRC1_PS,
                  S_00B028_VGPRS(si_shader_encode_vgprs(shader)) |
                  S_00B028_SGPRS(si_shader_encode_sgprs(shader)) |
                  S_00B028_DX10_CLAMP(sscreen->info.gfx_level < GFX12) |
                  S_00B028_MEM_ORDERED(si_shader_mem_ordered(shader)) |
                  S_00B028_FLOAT_MODE(shader->config.float_mode));
   ac_pm4_set_reg(&pm4->base, R_00B02C_SPI_SHADER_PGM_RSRC2_PS,
                  S_00B02C_EXTRA_LDS_SIZE(shader->config.lds_size) |
                  S_00B02C_USER_SGPR(SI_PS_NUM_USER_SGPR) |
                  S_00B02C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0) |
                  S_00B02C_SHARED_VGPR_CNT(shader->config.num_shared_vgprs / 8));
   ac_pm4_finalize(&pm4->base);
}

static void si_shader_init_pm4_state(struct si_screen *sscreen, struct si_shader *shader)
{
   assert(shader->wave_size);

   switch (shader->selector->stage) {
   case MESA_SHADER_VERTEX:
      if (shader->key.ge.as_ls)
         si_shader_ls(sscreen, shader);
      else if (shader->key.ge.as_es)
         si_shader_es(sscreen, shader);
      else if (shader->key.ge.as_ngg)
         gfx10_shader_ngg(sscreen, shader);
      else
         si_shader_vs(sscreen, shader, NULL);
      break;
   case MESA_SHADER_TESS_CTRL:
      si_shader_hs(sscreen, shader);
      break;
   case MESA_SHADER_TESS_EVAL:
      if (shader->key.ge.as_es)
         si_shader_es(sscreen, shader);
      else if (shader->key.ge.as_ngg)
         gfx10_shader_ngg(sscreen, shader);
      else
         si_shader_vs(sscreen, shader, NULL);
      break;
   case MESA_SHADER_GEOMETRY:
      if (shader->key.ge.as_ngg) {
         gfx10_shader_ngg(sscreen, shader);
      } else {
         /* VS must be initialized first because GS uses its fields. */
         si_shader_vs(sscreen, shader->gs_copy_shader, shader->selector);
         si_shader_gs(sscreen, shader);
      }
      break;
   case MESA_SHADER_FRAGMENT:
      si_shader_ps(sscreen, shader);
      break;
   default:
      assert(0);
   }

   assert(!(sscreen->debug_flags & DBG(SQTT)) || shader->pm4.base.spi_shader_pgm_lo_reg != 0);
}

static void si_clear_vs_key_inputs(union si_shader_key *key)
{
   key->ge.mono.instance_divisor_is_one = 0;
   key->ge.mono.instance_divisor_is_fetched = 0;
   key->ge.mono.vs_fetch_opencode = 0;
   memset(key->ge.mono.vs_fix_fetch, 0, sizeof(key->ge.mono.vs_fix_fetch));
}

void si_vs_key_update_inputs(struct si_context *sctx)
{
   struct si_shader_selector *vs = sctx->shader.vs.cso;
   struct si_vertex_elements *elts = sctx->vertex_elements;
   union si_shader_key *key = &sctx->shader.vs.key;

   if (!vs)
      return;

   if (vs->info.base.vs.blit_sgprs_amd) {
      si_clear_vs_key_inputs(key);
      key->ge.opt.prefer_mono = 0;
      sctx->uses_nontrivial_vs_inputs = false;
      return;
   }

   bool uses_nontrivial_vs_inputs = false;

   if (elts->instance_divisor_is_one || elts->instance_divisor_is_fetched)
      uses_nontrivial_vs_inputs = true;

   key->ge.mono.instance_divisor_is_one = elts->instance_divisor_is_one;
   key->ge.mono.instance_divisor_is_fetched = elts->instance_divisor_is_fetched;
   key->ge.opt.prefer_mono = elts->instance_divisor_is_fetched;

   unsigned count_mask = (1 << vs->info.num_inputs) - 1;
   unsigned fix = elts->fix_fetch_always & count_mask;
   unsigned opencode = elts->fix_fetch_opencode & count_mask;

   if (sctx->vertex_buffer_unaligned & elts->vb_alignment_check_mask) {
      uint32_t mask = elts->fix_fetch_unaligned & count_mask;
      while (mask) {
         unsigned i = u_bit_scan(&mask);
         unsigned log_hw_load_size = 1 + ((elts->hw_load_is_dword >> i) & 1);
         unsigned vbidx = elts->vertex_buffer_index[i];
         const struct pipe_vertex_buffer *vb = &sctx->vertex_buffer[vbidx];
         unsigned align_mask = (1 << log_hw_load_size) - 1;
         if (vb->buffer_offset & align_mask) {
            fix |= 1 << i;
            opencode |= 1 << i;
         }
      }
   }

   memset(key->ge.mono.vs_fix_fetch, 0, sizeof(key->ge.mono.vs_fix_fetch));

   while (fix) {
      unsigned i = u_bit_scan(&fix);
      uint8_t fix_fetch = elts->fix_fetch[i];

      key->ge.mono.vs_fix_fetch[i].bits = fix_fetch;
      if (fix_fetch)
         uses_nontrivial_vs_inputs = true;
   }
   key->ge.mono.vs_fetch_opencode = opencode;
   if (opencode)
      uses_nontrivial_vs_inputs = true;

   sctx->uses_nontrivial_vs_inputs = uses_nontrivial_vs_inputs;

   /* draw_vertex_state (display lists) requires that all VS input lowering is disabled
    * because its vertex elements never need any lowering.
    *
    * We just computed the key because we needed to set uses_nontrivial_vs_inputs, so that we know
    * whether the VS should be updated when we switch from draw_vertex_state to draw_vbo. Now
    * clear the VS input bits for draw_vertex_state. This should happen rarely because VS inputs
    * don't usually need any lowering.
    */
   if (uses_nontrivial_vs_inputs && sctx->force_trivial_vs_inputs)
      si_clear_vs_key_inputs(key);
}

static void si_get_vs_key_inputs(struct si_context *sctx, union si_shader_key *key)
{
   key->ge.mono.instance_divisor_is_one = sctx->shader.vs.key.ge.mono.instance_divisor_is_one;
   key->ge.mono.instance_divisor_is_fetched = sctx->shader.vs.key.ge.mono.instance_divisor_is_fetched;
   key->ge.mono.vs_fetch_opencode = sctx->shader.vs.key.ge.mono.vs_fetch_opencode;
   memcpy(key->ge.mono.vs_fix_fetch, sctx->shader.vs.key.ge.mono.vs_fix_fetch,
          sizeof(key->ge.mono.vs_fix_fetch));
}

void si_update_ps_inputs_read_or_disabled(struct si_context *sctx)
{
   struct si_shader_selector *ps = sctx->shader.ps.cso;

   /* Find out if PS is disabled. */
   bool ps_disabled = true;
   if (ps) {
      bool ps_modifies_zs = ps->info.base.fs.uses_discard ||
                            ps->info.writes_z ||
                            ps->info.writes_stencil ||
                            ps->info.writes_samplemask ||
                            sctx->queued.named.blend->alpha_to_coverage ||
                            sctx->queued.named.dsa->alpha_func != PIPE_FUNC_ALWAYS ||
                            sctx->queued.named.rasterizer->poly_stipple_enable ||
                            sctx->queued.named.rasterizer->point_smooth;

      ps_disabled = sctx->queued.named.rasterizer->rasterizer_discard ||
                    (!ps_modifies_zs && !ps->info.base.writes_memory &&
                     !si_any_colorbuffer_written(sctx));
   }

   uint64_t ps_inputs_read_or_disabled;

   if (ps_disabled) {
      ps_inputs_read_or_disabled = 0;
   } else {
      uint64_t inputs_read = ps->info.inputs_read;

      if (ps->info.colors_read && sctx->queued.named.rasterizer->two_side) {
         if (inputs_read & BITFIELD64_BIT(SI_UNIQUE_SLOT_COL0))
            inputs_read |= BITFIELD64_BIT(SI_UNIQUE_SLOT_BFC0);

         if (inputs_read & BITFIELD64_BIT(SI_UNIQUE_SLOT_COL1))
            inputs_read |= BITFIELD64_BIT(SI_UNIQUE_SLOT_BFC1);
      }

      ps_inputs_read_or_disabled = inputs_read;
   }

   if (sctx->ps_inputs_read_or_disabled != ps_inputs_read_or_disabled) {
      sctx->ps_inputs_read_or_disabled = ps_inputs_read_or_disabled;
      sctx->dirty_shaders_mask |=
         (sctx->shader.gs.cso ? BITFIELD_BIT(PIPE_SHADER_GEOMETRY) :
            (sctx->shader.tes.cso ? BITFIELD_BIT(PIPE_SHADER_TESS_EVAL) : BITFIELD_BIT(PIPE_SHADER_VERTEX)));
   }
}

void si_vs_ps_key_update_rast_prim_smooth_stipple(struct si_context *sctx)
{
   struct si_shader_ctx_state *hw_vs = si_get_vs(sctx);
   struct si_shader_selector *ps = sctx->shader.ps.cso;

   if (!hw_vs->cso || !ps)
      return;

   struct si_state_rasterizer *rs = sctx->queued.named.rasterizer;
   union si_shader_key *vs_key = &hw_vs->key; /* could also be TES or GS before PS */
   union si_shader_key *ps_key = &sctx->shader.ps.key;

   bool old_kill_pointsize = vs_key->ge.opt.kill_pointsize;
   bool old_color_two_side = ps_key->ps.part.prolog.color_two_side;
   bool old_poly_stipple = ps_key->ps.part.prolog.poly_stipple;
   bool old_poly_line_smoothing = ps_key->ps.mono.poly_line_smoothing;
   bool old_point_smoothing = ps_key->ps.mono.point_smoothing;
   int old_force_front_face_input = ps_key->ps.opt.force_front_face_input;

   if (sctx->current_rast_prim == MESA_PRIM_POINTS) {
      vs_key->ge.opt.kill_pointsize = 0;
      ps_key->ps.part.prolog.color_two_side = 0;
      ps_key->ps.part.prolog.poly_stipple = 0;
      ps_key->ps.mono.poly_line_smoothing = 0;
      ps_key->ps.mono.point_smoothing = rs->point_smooth;
      ps_key->ps.opt.force_front_face_input = ps->info.uses_frontface;
   } else if (util_prim_is_lines(sctx->current_rast_prim)) {
      vs_key->ge.opt.kill_pointsize = hw_vs->cso->info.writes_psize;
      ps_key->ps.part.prolog.color_two_side = 0;
      ps_key->ps.part.prolog.poly_stipple = 0;
      ps_key->ps.mono.poly_line_smoothing = rs->line_smooth && sctx->framebuffer.nr_samples <= 1;
      ps_key->ps.mono.point_smoothing = 0;
      ps_key->ps.opt.force_front_face_input = ps->info.uses_frontface;
   } else {
      /* Triangles. */
      vs_key->ge.opt.kill_pointsize = hw_vs->cso->info.writes_psize &&
                                      !rs->polygon_mode_is_points;
      ps_key->ps.part.prolog.color_two_side = rs->two_side && ps->info.colors_read;
      ps_key->ps.part.prolog.poly_stipple = rs->poly_stipple_enable;
      ps_key->ps.mono.poly_line_smoothing = rs->poly_smooth && sctx->framebuffer.nr_samples <= 1;
      ps_key->ps.mono.point_smoothing = 0;
      ps_key->ps.opt.force_front_face_input = ps->info.uses_frontface ? rs->force_front_face_input : 0;
   }

   if (vs_key->ge.opt.kill_pointsize != old_kill_pointsize) {
      sctx->dirty_shaders_mask |=
         BITFIELD_BIT(PIPE_SHADER_VERTEX) |
         BITFIELD_BIT(PIPE_SHADER_TESS_EVAL) |
         BITFIELD_BIT(PIPE_SHADER_GEOMETRY);
   }

   if (ps_key->ps.part.prolog.color_two_side != old_color_two_side ||
       ps_key->ps.part.prolog.poly_stipple != old_poly_stipple ||
       ps_key->ps.mono.poly_line_smoothing != old_poly_line_smoothing ||
       ps_key->ps.mono.point_smoothing != old_point_smoothing ||
       ps_key->ps.opt.force_front_face_input != old_force_front_face_input)
      sctx->dirty_shaders_mask |= BITFIELD_BIT(PIPE_SHADER_FRAGMENT);
}

static void si_get_vs_key_outputs(struct si_context *sctx, struct si_shader_selector *vs,
                                  union si_shader_key *key)
{
   key->ge.opt.kill_clip_distances = vs->info.clipdist_mask & ~sctx->queued.named.rasterizer->clip_plane_enable;

   /* Find out which VS outputs aren't used by the PS. */
   uint64_t outputs_written = vs->info.outputs_written_before_ps;
   uint64_t linked = outputs_written & sctx->ps_inputs_read_or_disabled;

   key->ge.opt.kill_layer = vs->info.writes_layer &&
                            sctx->framebuffer.state.layers <= 1;
   key->ge.opt.kill_outputs = ~linked & outputs_written;
   key->ge.opt.ngg_culling = sctx->ngg_culling;
   key->ge.mono.u.vs_export_prim_id = vs->stage != MESA_SHADER_GEOMETRY &&
                                      sctx->shader.ps.cso && sctx->shader.ps.cso->info.uses_primid;

   if (vs->info.enabled_streamout_buffer_mask) {
      if (sctx->streamout.enabled_mask) {
         key->ge.opt.remove_streamout = 0;
         key->ge.opt.ngg_vs_streamout_num_verts_per_prim =
            sctx->gfx_level >= GFX11 ? sctx->streamout.num_verts_per_prim : 0;
      } else {
         key->ge.opt.remove_streamout = 1;
         key->ge.opt.ngg_vs_streamout_num_verts_per_prim = 0;
      }
   } else {
      key->ge.opt.remove_streamout = 0;
      key->ge.opt.ngg_vs_streamout_num_verts_per_prim = 0;
   }

   if (sctx->gfx_level >= GFX12)
      key->ge.mono.remove_streamout = key->ge.opt.remove_streamout;

   /* The fixed-func hw only supports 6 clip planes, while gl_ClipVertex supports 8. */
   if (!vs->info.has_clip_outputs &&
       sctx->queued.named.rasterizer->clip_plane_enable & BITFIELD_RANGE(6, 2)) {
      key->ge.mono.write_pos_to_clipvertex = 1;
      key->ge.opt.kill_clip_distances = SI_USER_CLIP_PLANE_MASK &
                                        ~sctx->queued.named.rasterizer->clip_plane_enable;
   } else {
      key->ge.mono.write_pos_to_clipvertex = 0;
   }
}

static void si_clear_vs_key_outputs(struct si_context *sctx, struct si_shader_selector *vs,
                                    union si_shader_key *key)
{
   key->ge.opt.kill_clip_distances = 0;
   key->ge.opt.kill_outputs = 0;
   key->ge.opt.remove_streamout = 0;
   key->ge.opt.ngg_culling = 0;
   key->ge.opt.ngg_vs_streamout_num_verts_per_prim = 0;
   key->ge.mono.u.vs_export_prim_id = 0;
   key->ge.mono.remove_streamout = 0;
   key->ge.mono.write_pos_to_clipvertex = 0;
}

void si_ps_key_update_framebuffer(struct si_context *sctx)
{
   struct si_shader_selector *sel = sctx->shader.ps.cso;
   union si_shader_key *key = &sctx->shader.ps.key;

   if (!sel)
      return;

   /* ps_uses_fbfetch is true only if the color buffer is bound. */
   if (sctx->ps_uses_fbfetch) {
      struct pipe_surface *cb0 = &sctx->framebuffer.state.cbufs[0];
      struct pipe_resource *tex = cb0->texture;

      /* 1D textures are allocated and used as 2D on GFX9. */
      key->ps.mono.fbfetch_msaa = sctx->framebuffer.nr_samples > 1;
      key->ps.mono.fbfetch_is_1D =
         sctx->gfx_level != GFX9 &&
         (tex->target == PIPE_TEXTURE_1D || tex->target == PIPE_TEXTURE_1D_ARRAY);
      key->ps.mono.fbfetch_layered =
         tex->target == PIPE_TEXTURE_1D_ARRAY || tex->target == PIPE_TEXTURE_2D_ARRAY ||
         tex->target == PIPE_TEXTURE_CUBE || tex->target == PIPE_TEXTURE_CUBE_ARRAY ||
         tex->target == PIPE_TEXTURE_3D;
   } else {
      key->ps.mono.fbfetch_msaa = 0;
      key->ps.mono.fbfetch_is_1D = 0;
      key->ps.mono.fbfetch_layered = 0;
   }
}

void si_ps_key_update_framebuffer_blend_dsa_rasterizer(struct si_context *sctx)
{
   struct si_shader_selector *sel = sctx->shader.ps.cso;
   if (!sel)
      return;

   union si_shader_key *key = &sctx->shader.ps.key;
   struct si_state_blend *blend = sctx->queued.named.blend;
   struct si_state_dsa *dsa = sctx->queued.named.dsa;
   struct si_state_rasterizer *rs = sctx->queued.named.rasterizer;
   bool alpha_to_coverage = sel->info.colors_written & 0x1 && blend->alpha_to_coverage &&
                            rs->multisample_enable && sctx->framebuffer.nr_samples >= 2;
   unsigned need_src_alpha_4bit = blend->need_src_alpha_4bit;

   /* Old key data for comparison. */
   struct si_ps_epilog_bits old_epilog;
   memcpy(&old_epilog, &key->ps.part.epilog, sizeof(old_epilog));
   bool old_prefer_mono = key->ps.opt.prefer_mono;
#ifndef NDEBUG
   struct si_shader_key_ps old_key;
   memcpy(&old_key, &key->ps, sizeof(old_key));
#endif

   key->ps.part.epilog.kill_z = sel->info.writes_z &&
                                (!sctx->framebuffer.state.zsbuf.texture || !dsa->depth_enabled ||
                                 (sel->info.output_z_equals_input_z && !rs->multisample_enable));
   key->ps.part.epilog.kill_stencil = sel->info.writes_stencil &&
                                      (!sctx->framebuffer.has_stencil || !dsa->stencil_enabled);

   /* Remove the gl_SampleMask fragment shader output if MSAA is disabled.
    * This is required for correctness and it's also an optimization.
    */
   key->ps.part.epilog.kill_samplemask = sel->info.writes_samplemask &&
                                         (sctx->framebuffer.nr_samples <= 1 ||
                                          !rs->multisample_enable);

   key->ps.part.epilog.alpha_to_one = sel->info.colors_written & 0x1 && blend->alpha_to_one &&
                                      rs->multisample_enable;
   /* GFX11+ always exports alpha for alpha-to-coverage via mrtz. */
   key->ps.part.epilog.alpha_to_coverage_via_mrtz =
      alpha_to_coverage && (sctx->gfx_level >= GFX11 || key->ps.part.epilog.alpha_to_one) &&
      ((sel->info.writes_z && !key->ps.part.epilog.kill_z) ||
       (sel->info.writes_stencil && !key->ps.part.epilog.kill_stencil) ||
       (sel->info.writes_samplemask && !key->ps.part.epilog.kill_samplemask) ||
       /* If both alpha-to-coverage and alpha-to-one are enabled, alpha for alpha-to-coverage must
        * be exported from mrtz because mrt0.a must contain 1.0 for alpha-to-one. */
       key->ps.part.epilog.alpha_to_one);

   /* If alpha-to-coverage isn't exported via MRTZ, set that we need to export alpha
    * through MRT0.
    */
   if (alpha_to_coverage && !key->ps.part.epilog.alpha_to_coverage_via_mrtz)
      need_src_alpha_4bit |= 0xf;

   /* Select the shader color format based on whether
    * blending or alpha are needed.
    */
   key->ps.part.epilog.spi_shader_col_format =
      (blend->blend_enable_4bit & need_src_alpha_4bit &
       sctx->framebuffer.spi_shader_col_format_blend_alpha) |
      (blend->blend_enable_4bit & ~need_src_alpha_4bit &
       sctx->framebuffer.spi_shader_col_format_blend) |
      (~blend->blend_enable_4bit & need_src_alpha_4bit &
       sctx->framebuffer.spi_shader_col_format_alpha) |
      (~blend->blend_enable_4bit & ~need_src_alpha_4bit &
       sctx->framebuffer.spi_shader_col_format);
   key->ps.part.epilog.spi_shader_col_format &= blend->cb_target_enabled_4bit;

   key->ps.part.epilog.dual_src_blend_swizzle = sctx->gfx_level >= GFX11 &&
                                                blend->dual_src_blend &&
                                                (sel->info.colors_written_4bit & 0xff) == 0xff;

   /* The output for dual source blending should have
    * the same format as the first output.
    */
   if (blend->dual_src_blend) {
      key->ps.part.epilog.spi_shader_col_format |=
         (key->ps.part.epilog.spi_shader_col_format & 0xf) << 4;
   }

   /* If alpha-to-coverage is enabled, we have to export alpha
    * even if there is no color buffer.
    *
    * Gfx11 exports alpha-to-coverage via MRTZ if MRTZ is present.
    */
   if (!(key->ps.part.epilog.spi_shader_col_format & 0xf) && alpha_to_coverage &&
       !key->ps.part.epilog.alpha_to_coverage_via_mrtz)
      key->ps.part.epilog.spi_shader_col_format |= V_028710_SPI_SHADER_32_AR;

   /* On GFX6 and GFX7 except Hawaii, the CB doesn't clamp outputs
    * to the range supported by the type if a channel has less
    * than 16 bits and the export format is 16_ABGR.
    */
   if (sctx->gfx_level <= GFX7 && sctx->family != CHIP_HAWAII) {
      key->ps.part.epilog.color_is_int8 = sctx->framebuffer.color_is_int8;
      key->ps.part.epilog.color_is_int10 = sctx->framebuffer.color_is_int10;
   }

   /* Disable unwritten outputs (if WRITE_ALL_CBUFS isn't enabled). */
   if (!sel->info.color0_writes_all_cbufs) {
      key->ps.part.epilog.spi_shader_col_format &= sel->info.colors_written_4bit;
      key->ps.part.epilog.color_is_int8 &= sel->info.colors_written;
      key->ps.part.epilog.color_is_int10 &= sel->info.colors_written;
   }

   /* Enable RB+ for depth-only rendering. Registers must be programmed as follows:
    *    CB_COLOR_CONTROL.MODE = CB_DISABLE
    *    CB_COLOR0_INFO.FORMAT = COLOR_32
    *    CB_COLOR0_INFO.NUMBER_TYPE = NUMBER_FLOAT
    *    SPI_SHADER_COL_FORMAT.COL0_EXPORT_FORMAT = SPI_SHADER_32_R
    *    SX_PS_DOWNCONVERT.MRT0 = SX_RT_EXPORT_32_R
    *
    * Also, the following conditions must be met.
    */
   key->ps.part.epilog.rbplus_depth_only_opt =
      sctx->screen->info.rbplus_allowed &&
      blend->cb_target_enabled_4bit == 0 && /* implies CB_DISABLE */
      !alpha_to_coverage &&
      !sel->info.base.writes_memory &&
      !key->ps.part.epilog.spi_shader_col_format;

   /* Compile PS monolithically if it eliminates code or improves performance. */
   if (sel->info.colors_written_4bit &
       /* Dual source blending never has color buffer 1 enabled, so ignore it. */
       (blend->dual_src_blend ? 0xffffff0f : 0xffffffff) &
       ~(sctx->framebuffer.colorbuf_enabled_4bit & blend->cb_target_enabled_4bit)) {
      /* Eliminate shader code computing the color outputs that have missing color buffer
       * attachments or are disabled by colormask.
       */
      key->ps.opt.prefer_mono = 1;
   } else if (sctx->gfx_level >= GFX11 && sel->info.base.writes_memory) {
      /* On gfx11, pixel shaders that write memory should be compiled with an inlined epilog,
       * so that the compiler can see s_endpgm and deallocates VGPRs before memory stores return.
       */
      key->ps.opt.prefer_mono = 1;
   } else if (key->ps.part.epilog.kill_z || key->ps.part.epilog.kill_stencil ||
              key->ps.part.epilog.kill_samplemask) {
      /* Eliminate shader code computing the Z/S/samplemask outputs. */
      key->ps.opt.prefer_mono = 1;
   } else {
      key->ps.opt.prefer_mono = 0;
   }

   /* Update shaders only if the key changed. */
   if (memcmp(&key->ps.part.epilog, &old_epilog, sizeof(old_epilog)) ||
       key->ps.opt.prefer_mono != old_prefer_mono) {
      sctx->dirty_shaders_mask |= BITFIELD_BIT(PIPE_SHADER_FRAGMENT);
   } else {
      assert(memcmp(&key->ps, &old_key, sizeof(old_key)) == 0);
   }
}

void si_ps_key_update_rasterizer(struct si_context *sctx)
{
   struct si_shader_selector *sel = sctx->shader.ps.cso;
   union si_shader_key *key = &sctx->shader.ps.key;
   struct si_state_rasterizer *rs = sctx->queued.named.rasterizer;

   if (!sel)
      return;

   bool old_flatshade_colors = key->ps.part.prolog.flatshade_colors;
   bool old_clamp_color = key->ps.part.epilog.clamp_color;

   key->ps.part.prolog.flatshade_colors = rs->flatshade && sel->info.uses_interp_color;
   key->ps.part.epilog.clamp_color = rs->clamp_fragment_color;

   if (key->ps.part.prolog.flatshade_colors != old_flatshade_colors ||
       key->ps.part.epilog.clamp_color != old_clamp_color)
      sctx->dirty_shaders_mask |= BITFIELD_BIT(PIPE_SHADER_FRAGMENT);
}

void si_ps_key_update_dsa(struct si_context *sctx)
{
   union si_shader_key *key = &sctx->shader.ps.key;

   key->ps.part.epilog.alpha_func = sctx->queued.named.dsa->alpha_func;
}

void si_ps_key_update_sample_shading(struct si_context *sctx)
{
   struct si_shader_selector *sel = sctx->shader.ps.cso;
   if (!sel)
      return;

   union si_shader_key *key = &sctx->shader.ps.key;
   unsigned ps_iter_samples = si_get_ps_iter_samples(sctx);
   assert(ps_iter_samples <= MAX2(1, sctx->framebuffer.nr_color_samples));

   if (ps_iter_samples > 1 && sel->info.reads_samplemask) {
      /* Set samplemask_log_ps_iter=3 if full sample shading is enabled even for 2x and 4x MSAA
       * to get the fast path that fully replaces sample_mask_in with sample_id.
       */
      if (ps_iter_samples == sctx->framebuffer.nr_color_samples)
         key->ps.part.prolog.samplemask_log_ps_iter = 3;
      else
         key->ps.part.prolog.samplemask_log_ps_iter = util_logbase2(ps_iter_samples);
   } else {
      key->ps.part.prolog.samplemask_log_ps_iter = 0;
   }
}

void si_ps_key_update_framebuffer_rasterizer_sample_shading(struct si_context *sctx)
{
   struct si_shader_selector *sel = sctx->shader.ps.cso;
   union si_shader_key *key = &sctx->shader.ps.key;
   struct si_state_rasterizer *rs = sctx->queued.named.rasterizer;

   if (!sel)
      return;

   /* Old key data for comparison. */
   struct si_ps_prolog_bits old_prolog;
   memcpy(&old_prolog, &key->ps.part.prolog, sizeof(old_prolog));
   bool old_interpolate_at_sample_force_center = key->ps.mono.interpolate_at_sample_force_center;

   bool uses_persp_center = sel->info.uses_persp_center ||
                            (!rs->flatshade && sel->info.uses_persp_center_color);
   bool uses_persp_centroid = sel->info.uses_persp_centroid ||
                              (!rs->flatshade && sel->info.uses_persp_centroid_color);
   bool uses_persp_sample = sel->info.uses_persp_sample ||
                            (!rs->flatshade && sel->info.uses_persp_sample_color);

   if (!sel->info.base.fs.uses_sample_shading && rs->multisample_enable &&
       sctx->framebuffer.nr_samples > 1 && sctx->ps_iter_samples > 1) {
      key->ps.part.prolog.force_persp_sample_interp =
         uses_persp_center || uses_persp_centroid;

      key->ps.part.prolog.force_linear_sample_interp =
         sel->info.uses_linear_center || sel->info.uses_linear_centroid;

      key->ps.part.prolog.force_persp_center_interp = 0;
      key->ps.part.prolog.force_linear_center_interp = 0;
      key->ps.part.prolog.bc_optimize_for_persp = 0;
      key->ps.part.prolog.bc_optimize_for_linear = 0;
      key->ps.part.prolog.force_samplemask_to_helper_invocation = 0;
      /* Note that interpolateAt* requires center barycentrics while the PS prolog forces
       * per-sample barycentrics in center VGPRs, so it breaks it. The workaround is to
       * force monolithic compilation, which does the right thing.
       */
      key->ps.mono.force_mono = sel->info.uses_interp_at_offset || sel->info.uses_interp_at_sample;
      key->ps.mono.interpolate_at_sample_force_center = 0;
   } else if (rs->multisample_enable && sctx->framebuffer.nr_samples > 1) {
      /* Note that sample shading is possible here. If it's enabled, all barycentrics are
       * already set to "sample" except at_offset/at_sample.
       */
      key->ps.part.prolog.force_persp_sample_interp = 0;
      key->ps.part.prolog.force_linear_sample_interp = 0;
      key->ps.part.prolog.force_persp_center_interp = 0;
      key->ps.part.prolog.force_linear_center_interp = 0;
      key->ps.part.prolog.bc_optimize_for_persp =
         uses_persp_center && uses_persp_centroid;
      key->ps.part.prolog.bc_optimize_for_linear =
         sel->info.uses_linear_center && sel->info.uses_linear_centroid;
      key->ps.part.prolog.get_frag_coord_from_pixel_coord =
         !sel->info.base.fs.uses_sample_shading && sel->info.reads_frag_coord_mask & 0x3;
      key->ps.part.prolog.force_samplemask_to_helper_invocation = 0;
      key->ps.mono.force_mono = 0;
      key->ps.mono.interpolate_at_sample_force_center = 0;
   } else {
      key->ps.part.prolog.force_persp_sample_interp = 0;
      key->ps.part.prolog.force_linear_sample_interp = 0;

      /* Make sure SPI doesn't compute more than 1 pair
       * of (i,j), which is the optimization here. */
      key->ps.part.prolog.force_persp_center_interp = uses_persp_center +
                                                      uses_persp_centroid +
                                                      uses_persp_sample > 1;

      key->ps.part.prolog.force_linear_center_interp = sel->info.uses_linear_center +
                                                       sel->info.uses_linear_centroid +
                                                       sel->info.uses_linear_sample > 1;
      key->ps.part.prolog.bc_optimize_for_persp = 0;
      key->ps.part.prolog.bc_optimize_for_linear = 0;
      key->ps.part.prolog.get_frag_coord_from_pixel_coord =
         !!(sel->info.reads_frag_coord_mask & 0x3);
      key->ps.part.prolog.force_samplemask_to_helper_invocation = sel->info.reads_samplemask;
      key->ps.mono.force_mono = 0;
      key->ps.mono.interpolate_at_sample_force_center = sel->info.uses_interp_at_sample;
   }

   /* Update shaders only if the key changed. */
   if (memcmp(&key->ps.part.prolog, &old_prolog, sizeof(old_prolog)) ||
       key->ps.mono.interpolate_at_sample_force_center != old_interpolate_at_sample_force_center)
      sctx->dirty_shaders_mask |= BITFIELD_BIT(PIPE_SHADER_FRAGMENT);
}

/* Compute the key for the hw shader variant */
static inline void si_shader_selector_key(struct pipe_context *ctx, struct si_shader_selector *sel,
                                          union si_shader_key *key)
{
   struct si_context *sctx = (struct si_context *)ctx;

   switch (sel->stage) {
   case MESA_SHADER_VERTEX:
      if (!sctx->shader.tes.cso && !sctx->shader.gs.cso)
         si_get_vs_key_outputs(sctx, sel, key);
      else
         si_clear_vs_key_outputs(sctx, sel, key);
      break;
   case MESA_SHADER_TESS_CTRL:
      if (sctx->gfx_level >= GFX9) {
         si_get_vs_key_inputs(sctx, key);
         key->ge.part.tcs.ls = sctx->shader.vs.cso;
      }
      break;
   case MESA_SHADER_TESS_EVAL:
      if (!sctx->shader.gs.cso)
         si_get_vs_key_outputs(sctx, sel, key);
      else
         si_clear_vs_key_outputs(sctx, sel, key);
      break;
   case MESA_SHADER_GEOMETRY:
      if (sctx->gfx_level >= GFX9) {
         if (sctx->shader.tes.cso) {
            si_clear_vs_key_inputs(key);
            key->ge.part.gs.es = sctx->shader.tes.cso;
         } else {
            si_get_vs_key_inputs(sctx, key);
            key->ge.part.gs.es = sctx->shader.vs.cso;
         }

         /* Only NGG can eliminate GS outputs, because the code is shared with VS. */
         if (sctx->ngg)
            si_get_vs_key_outputs(sctx, sel, key);
         else
            si_clear_vs_key_outputs(sctx, sel, key);
      }
      break;
   case MESA_SHADER_FRAGMENT:
      break;
   default:
      assert(0);
   }
}

static void si_build_shader_variant(struct si_shader *shader, int thread_index, bool low_priority)
{
   struct si_shader_selector *sel = shader->selector;
   struct si_screen *sscreen = sel->screen;
   struct ac_llvm_compiler **compiler;
   struct util_debug_callback *debug = &shader->compiler_ctx_state.debug;

   if (thread_index >= 0) {
      if (low_priority) {
         assert(thread_index < (int)ARRAY_SIZE(sscreen->compiler_lowp));
         compiler = &sscreen->compiler_lowp[thread_index];
      } else {
         assert(thread_index < (int)ARRAY_SIZE(sscreen->compiler));
         compiler = &sscreen->compiler[thread_index];
      }
      if (!debug->async)
         debug = NULL;
   } else {
      assert(!low_priority);
      compiler = &shader->compiler_ctx_state.compiler;
   }

   if (!si_shader_uses_aco(shader) && !*compiler)
      *compiler = si_create_llvm_compiler(sscreen);

   if (unlikely(!si_create_shader_variant(sscreen, *compiler, shader, debug))) {
      PRINT_ERR("Failed to build shader variant (type=%u)\n", sel->stage);
      shader->compilation_failed = true;
      return;
   }

   if (shader->compiler_ctx_state.is_debug_context) {
      FILE *f = open_memstream(&shader->shader_log, &shader->shader_log_size);
      if (f) {
         si_shader_dump(sscreen, shader, NULL, f, false);
         fclose(f);
      }
   }

   si_shader_init_pm4_state(sscreen, shader);
}

static void si_build_shader_variant_low_priority(void *job, void *gdata, int thread_index)
{
   struct si_shader *shader = (struct si_shader *)job;

   assert(thread_index >= 0);

   si_build_shader_variant(shader, thread_index, true);
}

/* This should be const, but C++ doesn't allow implicit zero-initialization with const. */
static union si_shader_key zeroed;

static bool si_check_missing_main_part(struct si_screen *sscreen, struct si_shader_selector *sel,
                                       struct si_compiler_ctx_state *compiler_state,
                                       const union si_shader_key *key, unsigned wave_size)
{
   struct si_shader **mainp = si_get_main_shader_part(sel, key, wave_size);

   if (!*mainp) {
      struct si_shader *main_part = CALLOC_STRUCT(si_shader);

      if (!main_part)
         return false;

      /* We can leave the fence as permanently signaled because the
       * main part becomes visible globally only after it has been
       * compiled. */
      util_queue_fence_init(&main_part->ready);

      main_part->selector = sel;
      if (sel->stage <= MESA_SHADER_GEOMETRY) {
         main_part->key.ge.as_es = key->ge.as_es;
         main_part->key.ge.as_ls = key->ge.as_ls;
         main_part->key.ge.as_ngg = key->ge.as_ngg;
         main_part->key.ge.use_aco = key->ge.use_aco;
      }
      main_part->is_monolithic = false;
      main_part->wave_size = wave_size;

      if (!si_compile_shader(sscreen, compiler_state->compiler, main_part,
                             &compiler_state->debug)) {
         FREE(main_part);
         return false;
      }
      *mainp = main_part;
   }
   return true;
}

/* A helper to copy *key to *local_key and return local_key. */
template<typename SHADER_KEY_TYPE>
static ALWAYS_INLINE const SHADER_KEY_TYPE *
use_local_key_copy(const SHADER_KEY_TYPE *key, SHADER_KEY_TYPE *local_key, unsigned key_size)
{
   if (key != local_key)
      memcpy(local_key, key, key_size);

   return local_key;
}

#define NO_INLINE_UNIFORMS false

/**
 * Select a shader variant according to the shader key.
 *
 * This uses a C++ template to compute the optimal memcmp size at compile time, which is important
 * for getting inlined memcmp. The memcmp size depends on the shader key type and whether inlined
 * uniforms are enabled.
 */
template<bool INLINE_UNIFORMS = true, typename SHADER_KEY_TYPE>
static int si_shader_select_with_key(struct si_context *sctx, struct si_shader_ctx_state *state,
                                     const SHADER_KEY_TYPE *key)
{
   struct si_screen *sscreen = sctx->screen;
   struct si_shader_selector *sel = state->cso;
   struct si_shader_selector *previous_stage_sel = NULL;
   struct si_shader *current = state->current;
   struct si_shader *shader = NULL;
   const SHADER_KEY_TYPE *zeroed_key = (SHADER_KEY_TYPE*)&zeroed;

   /* "opt" must be the last field and "inlined_uniform_values" must be the last field inside opt.
    * If there is padding, insert the padding manually before opt or inside opt.
    */
   STATIC_ASSERT(offsetof(SHADER_KEY_TYPE, opt) + sizeof(key->opt) == sizeof(*key));
   STATIC_ASSERT(offsetof(SHADER_KEY_TYPE, opt.inlined_uniform_values) +
                 sizeof(key->opt.inlined_uniform_values) == sizeof(*key));

   const unsigned key_size_no_uniforms = sizeof(*key) - sizeof(key->opt.inlined_uniform_values);
   /* Don't compare inlined_uniform_values if uniform inlining is disabled. */
   const unsigned key_size = INLINE_UNIFORMS ? sizeof(*key) : key_size_no_uniforms;
   const unsigned key_opt_size =
      INLINE_UNIFORMS ? sizeof(key->opt) :
                        sizeof(key->opt) - sizeof(key->opt.inlined_uniform_values);

   /* si_shader_select_with_key must not modify 'key' because it would affect future shaders.
    * If we need to modify it for this specific shader (eg: to disable optimizations), we
    * use a copy.
    */
   SHADER_KEY_TYPE local_key;

   if (unlikely(sscreen->shader_debug_flags & DBG(NO_OPT_VARIANT))) {
      /* Disable shader variant optimizations. */
      key = use_local_key_copy<SHADER_KEY_TYPE>(key, &local_key, key_size);
      memset(&local_key.opt, 0, key_opt_size);
   }

again:
   /* Check if we don't need to change anything.
    * This path is also used for most shaders that don't need multiple
    * variants, it will cost just a computation of the key and this
    * test. */
   if (likely(current && memcmp(&current->key, key, key_size) == 0)) {
      if (unlikely(!util_queue_fence_is_signalled(&current->ready))) {
         if (current->is_optimized) {
            key = use_local_key_copy(key, &local_key, key_size);
            memset(&local_key.opt, 0, key_opt_size);
            goto current_not_ready;
         }

         util_queue_fence_wait(&current->ready);
      }

      return current->compilation_failed ? -1 : 0;
   }
current_not_ready:

   /* This must be done before the mutex is locked, because async GS
    * compilation calls this function too, and therefore must enter
    * the mutex first.
    */
   util_queue_fence_wait(&sel->ready);

   simple_mtx_lock(&sel->mutex);

   int variant_count = 0;
   const int max_inline_uniforms_variants = 5;

   /* Find the shader variant. */
   const unsigned cnt = sel->variants_count;
   for (unsigned i = 0; i < cnt; i++) {
      const SHADER_KEY_TYPE *iter_key = (const SHADER_KEY_TYPE *)&sel->keys[i];

      if (memcmp(iter_key, key, key_size_no_uniforms) == 0) {
         struct si_shader *iter = sel->variants[i];

         /* Check the inlined uniform values separately, and count
          * the number of variants based on them.
          */
         if (key->opt.inline_uniforms &&
             memcmp(iter_key->opt.inlined_uniform_values,
                    key->opt.inlined_uniform_values,
                    MAX_INLINABLE_UNIFORMS * 4) != 0) {
            if (variant_count++ > max_inline_uniforms_variants) {
               key = use_local_key_copy(key, &local_key, key_size);
               /* Too many variants. Disable inlining for this shader. */
               local_key.opt.inline_uniforms = 0;
               memset(local_key.opt.inlined_uniform_values, 0, MAX_INLINABLE_UNIFORMS * 4);
               simple_mtx_unlock(&sel->mutex);
               goto again;
            }
            continue;
         }

         simple_mtx_unlock(&sel->mutex);

         if (unlikely(!util_queue_fence_is_signalled(&iter->ready))) {
            /* If it's an optimized shader and its compilation has
             * been started but isn't done, use the unoptimized
             * shader so as not to cause a stall due to compilation.
             */
            if (iter->is_optimized) {
               key = use_local_key_copy(key, &local_key, key_size);
               memset(&local_key.opt, 0, key_opt_size);
               goto again;
            }

            util_queue_fence_wait(&iter->ready);
         }

         if (iter->compilation_failed) {
            return -1; /* skip the draw call */
         }

         state->current = sel->variants[i];
         return 0;
      }
   }

   /* Build a new shader. */
   shader = CALLOC_STRUCT(si_shader);
   if (!shader) {
      simple_mtx_unlock(&sel->mutex);
      return -ENOMEM;
   }

   util_queue_fence_init(&shader->ready);
   shader->selector = sel;

   if (!si_shader_uses_aco(shader) && !sctx->compiler)
      sctx->compiler = si_create_llvm_compiler(sctx->screen);

   *((SHADER_KEY_TYPE*)&shader->key) = *key;
   shader->wave_size = si_determine_wave_size(sscreen, shader);
   shader->compiler_ctx_state.compiler = sctx->compiler;
   shader->compiler_ctx_state.debug = sctx->debug;
   shader->compiler_ctx_state.is_debug_context = sctx->is_debug;

   /* If this is a merged shader, get the first shader's selector. */
   if (sscreen->info.gfx_level >= GFX9) {
      if (sel->stage == MESA_SHADER_TESS_CTRL)
         previous_stage_sel = ((struct si_shader_key_ge*)key)->part.tcs.ls;
      else if (sel->stage == MESA_SHADER_GEOMETRY)
         previous_stage_sel = ((struct si_shader_key_ge*)key)->part.gs.es;

      /* We need to wait for the previous shader. */
      if (previous_stage_sel)
         util_queue_fence_wait(&previous_stage_sel->ready);
   }

   bool is_pure_monolithic =
      sscreen->use_monolithic_shaders || memcmp(&key->mono, &zeroed_key->mono, sizeof(key->mono)) != 0;

   /* Compile the main shader part if it doesn't exist. This can happen
    * if the initial guess was wrong.
    */
   if (!is_pure_monolithic) {
      bool ok = true;

      /* Make sure the main shader part is present. This is needed
       * for shaders that can be compiled as VS, LS, or ES, and only
       * one of them is compiled at creation.
       *
       * It is also needed for GS, which can be compiled as non-NGG
       * and NGG.
       *
       * For merged shaders, check that the starting shader's main
       * part is present.
       */
      if (previous_stage_sel) {
         union si_shader_key shader1_key = zeroed;

         if (sel->stage == MESA_SHADER_TESS_CTRL) {
            shader1_key.ge.as_ls = 1;
         } else if (sel->stage == MESA_SHADER_GEOMETRY) {
            shader1_key.ge.as_es = 1;
            shader1_key.ge.as_ngg = ((struct si_shader_key_ge*)key)->as_ngg; /* for Wave32 vs Wave64 */
         } else {
            assert(0);
         }

         shader1_key.ge.use_aco = ((struct si_shader_key_ge*)key)->use_aco;

         simple_mtx_lock(&previous_stage_sel->mutex);
         ok = si_check_missing_main_part(sscreen, previous_stage_sel, &shader->compiler_ctx_state,
                                         &shader1_key, shader->wave_size);
         simple_mtx_unlock(&previous_stage_sel->mutex);
      }

      if (ok) {
         ok = si_check_missing_main_part(sscreen, sel, &shader->compiler_ctx_state,
                                         (union si_shader_key*)key, shader->wave_size);
      }

      if (!ok) {
         FREE(shader);
         simple_mtx_unlock(&sel->mutex);
         return -ENOMEM; /* skip the draw call */
      }
   }

   if (sel->variants_count == sel->variants_max_count) {
      sel->variants_max_count += 2;
      sel->variants = (struct si_shader**)
         realloc(sel->variants, sel->variants_max_count * sizeof(struct si_shader*));
      sel->keys = (union si_shader_key*)
         realloc(sel->keys, sel->variants_max_count * sizeof(union si_shader_key));
   }

   /* Keep the reference to the 1st shader of merged shaders, so that
    * Gallium can't destroy it before we destroy the 2nd shader.
    *
    * Set sctx = NULL, because it's unused if we're not releasing
    * the shader, and we don't have any sctx here.
    */
   si_shader_selector_reference(NULL, &shader->previous_stage_sel, previous_stage_sel);

   /* Monolithic-only shaders don't make a distinction between optimized
    * and unoptimized. */
   shader->is_monolithic =
      is_pure_monolithic || memcmp(&key->opt, &zeroed_key->opt, key_opt_size) != 0;

   shader->is_optimized = !is_pure_monolithic &&
                          memcmp(&key->opt, &zeroed_key->opt, key_opt_size) != 0;

   /* If it's an optimized shader, compile it asynchronously. */
   if (shader->is_optimized) {
      /* Compile it asynchronously. */
      util_queue_add_job(&sscreen->shader_compiler_queue_opt_variants, shader, &shader->ready,
                         si_build_shader_variant_low_priority, NULL, 0);

      /* Add only after the ready fence was reset, to guard against a
       * race with si_bind_XX_shader. */
      sel->variants[sel->variants_count] = shader;
      sel->keys[sel->variants_count] = shader->key;
      sel->variants_count++;

      /* Use the default (unoptimized) shader for now. */
      key = use_local_key_copy(key, &local_key, key_size);
      memset(&local_key.opt, 0, key_opt_size);
      simple_mtx_unlock(&sel->mutex);

      if (sscreen->options.sync_compile)
         util_queue_fence_wait(&shader->ready);

      goto again;
   }

   /* Reset the fence before adding to the variant list. */
   util_queue_fence_reset(&shader->ready);

   sel->variants[sel->variants_count] = shader;
   sel->keys[sel->variants_count] = shader->key;
   sel->variants_count++;

   simple_mtx_unlock(&sel->mutex);

   assert(!shader->is_optimized);
   si_build_shader_variant(shader, -1, false);

   util_queue_fence_signal(&shader->ready);

   if (!shader->compilation_failed)
      state->current = shader;

   return shader->compilation_failed ? -1 : 0;
}

int si_shader_select(struct pipe_context *ctx, struct si_shader_ctx_state *state)
{
   struct si_context *sctx = (struct si_context *)ctx;

   si_shader_selector_key(ctx, state->cso, &state->key);

   if (state->cso->stage == MESA_SHADER_FRAGMENT) {
      if (state->key.ps.opt.inline_uniforms)
         return si_shader_select_with_key(sctx, state, &state->key.ps);
      else
         return si_shader_select_with_key<NO_INLINE_UNIFORMS>(sctx, state, &state->key.ps);
   } else {
      if (state->key.ge.opt.inline_uniforms) {
         return si_shader_select_with_key(sctx, state, &state->key.ge);
      } else {
         return si_shader_select_with_key<NO_INLINE_UNIFORMS>(sctx, state, &state->key.ge);
      }
   }
}

static void si_parse_next_shader_property(nir_shader *nir, union si_shader_key *key)
{
   gl_shader_stage next_shader = nir->info.next_stage;
   bool writes_position = nir->info.outputs_written & VARYING_BIT_POS;
   assert(!nir->xfb_info || nir->xfb_info->buffers_written);

   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      switch (next_shader) {
      case MESA_SHADER_GEOMETRY:
         key->ge.as_es = 1;
         break;
      case MESA_SHADER_TESS_CTRL:
      case MESA_SHADER_TESS_EVAL:
         key->ge.as_ls = 1;
         break;
      default:
         /* If POSITION isn't written, it can only be a HW VS
          * if streamout is used. If streamout isn't used,
          * assume that it's a HW LS. (the next shader is TCS)
          * This heuristic is needed for separate shader objects.
          */
         if (next_shader == MESA_SHADER_NONE && !writes_position && !nir->xfb_info)
            key->ge.as_ls = 1;
      }
      break;

   case MESA_SHADER_TESS_EVAL:
      if (next_shader == MESA_SHADER_GEOMETRY ||
          (next_shader == MESA_SHADER_NONE && !writes_position))
         key->ge.as_es = 1;
      break;

   default:;
   }
}

/**
 * Compile the main shader part or the monolithic shader as part of
 * si_shader_selector initialization. Since it can be done asynchronously,
 * there is no way to report compile failures to applications.
 */
static void si_init_shader_selector_async(void *job, void *gdata, int thread_index)
{
   struct si_shader_selector *sel = (struct si_shader_selector *)job;
   struct si_screen *sscreen = sel->screen;
   struct ac_llvm_compiler **compiler;
   struct util_debug_callback *debug = &sel->compiler_ctx_state.debug;

   assert(!debug->debug_message || debug->async);
   assert(thread_index >= 0);
   assert(thread_index < (int)ARRAY_SIZE(sscreen->compiler));
   compiler = &sscreen->compiler[thread_index];

   if (!sel->info.base.use_aco_amd && !*compiler)
      *compiler = si_create_llvm_compiler(sscreen);

   /* Serialize NIR to save memory. Monolithic shader variants
    * have to deserialize NIR before compilation.
    */
   struct blob blob;
   size_t size;
   assert(sel->nir);

   blob_init(&blob);
   /* true = remove optional debugging data to increase
    * the likehood of getting more shader cache hits.
    * It also drops variable names, so we'll save more memory.
    * If NIR debug prints are used we don't strip to get more
    * useful logs.
    */
   nir_serialize(&blob, sel->nir, NIR_DEBUG(PRINT) == 0);
   blob_finish_get_buffer(&blob, &sel->nir_binary, &size);
   sel->nir_size = size;

   /* Compile the main shader part for use with a prolog and/or epilog.
    * If this fails, the driver will try to compile a monolithic shader
    * on demand.
    */
   if (!sscreen->use_monolithic_shaders) {
      struct si_shader *shader = CALLOC_STRUCT(si_shader);
      unsigned char ir_sha1_cache_key[20];

      if (!shader) {
         mesa_loge("can't allocate a main shader part");
         return;
      }

      /* We can leave the fence signaled because use of the default
       * main part is guarded by the selector's ready fence. */
      util_queue_fence_init(&shader->ready);

      shader->selector = sel;
      shader->is_monolithic = false;
      si_parse_next_shader_property(sel->nir, &shader->key);

      if (sel->stage <= MESA_SHADER_GEOMETRY) {
         if (sscreen->use_ngg && (!sel->info.enabled_streamout_buffer_mask ||
                                  sscreen->info.gfx_level >= GFX11) &&
             ((sel->stage == MESA_SHADER_VERTEX && !shader->key.ge.as_ls) ||
              sel->stage == MESA_SHADER_TESS_EVAL || sel->stage == MESA_SHADER_GEOMETRY))
            shader->key.ge.as_ngg = 1;

         shader->key.ge.use_aco = sel->nir->info.use_aco_amd;
      }

      shader->wave_size = si_determine_wave_size(sscreen, shader);

      if (sel->stage <= MESA_SHADER_GEOMETRY) {
         si_get_ir_cache_key(sel, shader->key.ge.as_ngg, shader->key.ge.as_es,
                             shader->wave_size, ir_sha1_cache_key);
      } else {
         si_get_ir_cache_key(sel, false, false, shader->wave_size, ir_sha1_cache_key);
      }

      /* Try to load the shader from the shader cache. */
      simple_mtx_lock(&sscreen->shader_cache_mutex);

      if (si_shader_cache_load_shader(sscreen, ir_sha1_cache_key, shader)) {
         simple_mtx_unlock(&sscreen->shader_cache_mutex);
         si_shader_dump_stats_for_shader_db(sscreen, shader, debug);
      } else {
         simple_mtx_unlock(&sscreen->shader_cache_mutex);

         /* Compile the shader if it hasn't been loaded from the cache. */
         if (!si_compile_shader(sscreen, *compiler, shader, debug)) {
            mesa_loge("can't compile a main shader part (type: %s).\n"
                      "This is probably a driver bug, please report "
                      "it to https://gitlab.freedesktop.org/mesa/mesa/-/issues.",
                      gl_shader_stage_name(shader->selector->stage));
            FREE(shader);
            return;
         }

         simple_mtx_lock(&sscreen->shader_cache_mutex);
         si_shader_cache_insert_shader(sscreen, ir_sha1_cache_key, shader, true);
         simple_mtx_unlock(&sscreen->shader_cache_mutex);
      }

      *si_get_main_shader_part(sel, &shader->key, shader->wave_size) = shader;
   }

   /* Free NIR. We only keep serialized NIR after this point. */
   ralloc_free(sel->nir);
   sel->nir = NULL;
}

void si_schedule_initial_compile(struct si_context *sctx, gl_shader_stage stage,
                                 struct util_queue_fence *ready_fence,
                                 struct si_compiler_ctx_state *compiler_ctx_state, void *job,
                                 util_queue_execute_func execute)
{
   util_queue_fence_init(ready_fence);

   struct util_async_debug_callback async_debug;
   bool debug = (sctx->debug.debug_message && !sctx->debug.async) || sctx->is_debug ||
                si_can_dump_shader(sctx->screen, stage, SI_DUMP_ALWAYS);

   if (debug) {
      u_async_debug_init(&async_debug);
      compiler_ctx_state->debug = async_debug.base;
   }

   util_queue_add_job(&sctx->screen->shader_compiler_queue, job, ready_fence, execute, NULL, 0);

   if (debug) {
      util_queue_fence_wait(ready_fence);
      u_async_debug_drain(&async_debug, &sctx->debug);
      u_async_debug_cleanup(&async_debug);
   }

   if (sctx->screen->options.sync_compile)
      util_queue_fence_wait(ready_fence);
}

/* Return descriptor slot usage masks from the given shader info. */
void si_get_active_slot_masks(struct si_screen *sscreen, const struct si_shader_info *info,
                              uint64_t *const_and_shader_buffers, uint64_t *samplers_and_images)
{
   unsigned start, num_shaderbufs, num_constbufs, num_images, num_msaa_images, num_samplers;

   num_shaderbufs = info->base.num_ssbos;
   num_constbufs = info->base.num_ubos;
   /* two 8-byte images share one 16-byte slot */
   num_images = align(info->base.num_images, 2);
   num_msaa_images = align(util_last_bit(info->base.msaa_images), 2);
   num_samplers = util_last_bit(info->base.textures_used);

   /* The layout is: sb[last] ... sb[0], cb[0] ... cb[last] */
   start = si_get_shaderbuf_slot(num_shaderbufs - 1);
   *const_and_shader_buffers = BITFIELD64_RANGE(start, num_shaderbufs + num_constbufs);

   /* The layout is:
    *   - fmask[last] ... fmask[0]     go to [15-last .. 15]
    *   - image[last] ... image[0]     go to [31-last .. 31]
    *   - sampler[0] ... sampler[last] go to [32 .. 32+last*2]
    *
    * FMASKs for images are placed separately, because MSAA images are rare,
    * and so we can benefit from a better cache hit rate if we keep image
    * descriptors together.
    */
   if (sscreen->info.gfx_level < GFX11 && num_msaa_images)
      num_images = SI_NUM_IMAGES + num_msaa_images; /* add FMASK descriptors */

   start = si_get_image_slot(num_images - 1) / 2;
   *samplers_and_images = BITFIELD64_RANGE(start, num_images / 2 + num_samplers);
}

static void *si_create_shader_selector(struct pipe_context *ctx,
                                       const struct pipe_shader_state *state)
{
   struct si_screen *sscreen = (struct si_screen *)ctx->screen;
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_shader_selector *sel = CALLOC_STRUCT(si_shader_selector);

   if (!sel)
      return NULL;

   sel->screen = sscreen;
   sel->compiler_ctx_state.debug = sctx->debug;
   sel->compiler_ctx_state.is_debug_context = sctx->is_debug;
   sel->variants_max_count = 2;
   sel->keys = (union si_shader_key *)
      realloc(NULL, sel->variants_max_count * sizeof(union si_shader_key));
   sel->variants = (struct si_shader **)
      realloc(NULL, sel->variants_max_count * sizeof(struct si_shader *));

   if (state->type == PIPE_SHADER_IR_TGSI) {
      sel->nir = tgsi_to_nir(state->tokens, ctx->screen, true);
   } else {
      assert(state->type == PIPE_SHADER_IR_NIR);
      sel->nir = state->ir.nir;
   }

   si_nir_scan_shader(sscreen, sel->nir, &sel->info, false);

   sel->stage = sel->nir->info.stage;
   sel->const_and_shader_buf_descriptors_index =
      si_const_and_shader_buffer_descriptors_idx(sel->stage);
   sel->sampler_and_images_descriptors_index =
      si_sampler_and_image_descriptors_idx(sel->stage);

   if (si_can_dump_shader(sscreen, sel->stage, SI_DUMP_INIT_NIR))
      nir_print_shader(sel->nir, stderr);

   p_atomic_inc(&sscreen->num_shaders_created);
   si_get_active_slot_masks(sscreen, &sel->info, &sel->active_const_and_shader_buffers,
                            &sel->active_samplers_and_images);

   switch (sel->stage) {
   case MESA_SHADER_GEOMETRY:
      /* Only possibilities: POINTS, LINE_STRIP, TRIANGLES */
      sel->rast_prim = (enum mesa_prim)sel->nir->info.gs.output_primitive;
      if (util_rast_prim_is_triangles(sel->rast_prim))
         sel->rast_prim = MESA_PRIM_TRIANGLES;

      /* EN_MAX_VERT_OUT_PER_GS_INSTANCE does not work with tessellation so
       * we can't split workgroups. Disable ngg if any of the following conditions is true:
       * - num_invocations * gs.vertices_out > 256
       * - LDS usage is too high
       */
      sel->tess_turns_off_ngg = sscreen->info.gfx_level >= GFX10 &&
                                sscreen->info.gfx_level <= GFX10_3 &&
                                (sel->nir->info.gs.invocations * sel->nir->info.gs.vertices_out > 256 ||
                                 sel->nir->info.gs.invocations * sel->nir->info.gs.vertices_out *
                                 (sel->info.num_outputs * 4 + 1) > 6500 /* max dw per GS primitive */);
      break;

   case MESA_SHADER_VERTEX:
   case MESA_SHADER_TESS_EVAL:
      if (sel->stage == MESA_SHADER_TESS_EVAL) {
         if (sel->nir->info.tess.point_mode)
            sel->rast_prim = MESA_PRIM_POINTS;
         else if (sel->nir->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES)
            sel->rast_prim = MESA_PRIM_LINE_STRIP;
         else
            sel->rast_prim = MESA_PRIM_TRIANGLES;
      } else {
         sel->rast_prim = MESA_PRIM_TRIANGLES;
      }
      break;
   default:;
   }

   bool ngg_culling_allowed =
      sscreen->info.gfx_level >= GFX10 &&
      sscreen->use_ngg_culling &&
      sel->nir->info.outputs_written & VARYING_BIT_POS &&
      !sel->nir->info.writes_memory &&
      /* NGG GS supports culling with streamout because it culls after streamout. */
      (sel->stage == MESA_SHADER_GEOMETRY || !sel->info.enabled_streamout_buffer_mask) &&
      (sel->stage != MESA_SHADER_GEOMETRY || sel->info.gs_writes_stream0) &&
      (sel->stage != MESA_SHADER_VERTEX ||
       (!sel->nir->info.vs.blit_sgprs_amd &&
        !sel->nir->info.vs.window_space_position));

   sel->ngg_cull_vert_threshold = UINT_MAX; /* disabled (changed below) */

   if (ngg_culling_allowed) {
      if (sel->stage == MESA_SHADER_VERTEX) {
         if (sscreen->debug_flags & DBG(ALWAYS_NGG_CULLING_ALL))
            sel->ngg_cull_vert_threshold = 0; /* always enabled */
         else
            sel->ngg_cull_vert_threshold = 128;
      } else if (sel->stage == MESA_SHADER_TESS_EVAL ||
                 sel->stage == MESA_SHADER_GEOMETRY) {
         if (sel->rast_prim != MESA_PRIM_POINTS)
            sel->ngg_cull_vert_threshold = 0; /* always enabled */
      }
   }

   (void)simple_mtx_init(&sel->mutex, mtx_plain);

   si_schedule_initial_compile(sctx, sel->stage, &sel->ready, &sel->compiler_ctx_state,
                               sel, si_init_shader_selector_async);
   return sel;
}

static void *si_create_shader(struct pipe_context *ctx, const struct pipe_shader_state *state)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_screen *sscreen = (struct si_screen *)ctx->screen;
   bool cache_hit;
   struct si_shader_selector *sel = (struct si_shader_selector *)util_live_shader_cache_get(
      ctx, &sscreen->live_shader_cache, state, &cache_hit);

   if (sel && cache_hit && sctx->debug.debug_message) {
      for (unsigned i = 0; i < ARRAY_SIZE(sel->main_parts.variants); i++) {
         if (sel->main_parts.variants[i])
            si_shader_dump_stats_for_shader_db(sscreen, sel->main_parts.variants[i], &sctx->debug);
      }
   }
   return sel;
}

static void si_update_streamout_state(struct si_context *sctx)
{
   struct si_shader_selector *shader_with_so = si_get_vs(sctx)->cso;

   if (!shader_with_so)
      return;

   sctx->streamout.enabled_stream_buffers_mask = shader_with_so->info.enabled_streamout_buffer_mask;
   sctx->streamout.stride_in_dw = shader_with_so->info.base.xfb_stride;

   /* GDS must be allocated when any GDS instructions are used, otherwise it hangs. */
   if (sctx->gfx_level >= GFX11 && sctx->gfx_level < GFX12 &&
       shader_with_so->info.enabled_streamout_buffer_mask && !sctx->screen->gds_oa) {
      /* Gfx11 only uses GDS OA, not GDS memory. */
      simple_mtx_lock(&sctx->screen->gds_mutex);
      if (!sctx->screen->gds_oa) {
         sctx->screen->gds_oa = sctx->ws->buffer_create(sctx->ws, 1, 1, RADEON_DOMAIN_OA,
                                                        RADEON_FLAG_DRIVER_INTERNAL);
         assert(sctx->screen->gds_oa);
      }
      simple_mtx_unlock(&sctx->screen->gds_mutex);

      if (sctx->screen->gds_oa)
         sctx->ws->cs_add_buffer(&sctx->gfx_cs, sctx->screen->gds_oa, RADEON_USAGE_READWRITE,
                                 (enum radeon_bo_domain)0);
   }
}

static void si_update_clip_regs(struct si_context *sctx, struct si_shader_selector *old_hw_vs,
                                struct si_shader *old_hw_vs_variant,
                                struct si_shader_selector *next_hw_vs,
                                struct si_shader *next_hw_vs_variant)
{
   if (next_hw_vs &&
       (!old_hw_vs ||
        (old_hw_vs->stage == MESA_SHADER_VERTEX && old_hw_vs->info.base.vs.window_space_position) !=
        (next_hw_vs->stage == MESA_SHADER_VERTEX && next_hw_vs->info.base.vs.window_space_position) ||
        old_hw_vs->info.has_clip_outputs != next_hw_vs->info.has_clip_outputs ||
        !old_hw_vs_variant || !next_hw_vs_variant ||
        old_hw_vs_variant->info.clipdist_mask != next_hw_vs_variant->info.clipdist_mask ||
        old_hw_vs_variant->info.culldist_mask != next_hw_vs_variant->info.culldist_mask ||
        old_hw_vs_variant->pa_cl_vs_out_cntl != next_hw_vs_variant->pa_cl_vs_out_cntl))
      si_mark_atom_dirty(sctx, &sctx->atoms.s.clip_regs);
}

static void si_update_rasterized_prim(struct si_context *sctx)
{
   struct si_shader *hw_vs = si_get_vs(sctx)->current;

   if (sctx->shader.gs.cso) {
      /* Only possibilities: POINTS, LINE_STRIP, TRIANGLES */
      si_set_rasterized_prim(sctx, sctx->shader.gs.cso->rast_prim, hw_vs, sctx->ngg);
   } else if (sctx->shader.tes.cso) {
      /* Only possibilities: POINTS, LINE_STRIP, TRIANGLES */
      si_set_rasterized_prim(sctx, sctx->shader.tes.cso->rast_prim, hw_vs, sctx->ngg);
   } else {
      /* The rasterized prim is determined by draw calls. */
   }

   /* This must be done unconditionally because it also depends on si_shader fields. */
   si_update_ngg_sgpr_state_out_prim(sctx, hw_vs, sctx->ngg);
}

void si_update_common_shader_state(struct si_context *sctx, struct si_shader_selector *sel,
                                   enum pipe_shader_type type)
{
   si_set_active_descriptors_for_shader(sctx, sel);

   if (si_shader_uses_bindless_samplers(sel))
      sctx->uses_bindless_samplers |= BITFIELD_BIT(type);
   else
      sctx->uses_bindless_samplers &= ~BITFIELD_BIT(type);

   if (si_shader_uses_bindless_images(sel))
      sctx->uses_bindless_images |= BITFIELD_BIT(type);
   else
      sctx->uses_bindless_images &= ~BITFIELD_BIT(type);

   if (type == PIPE_SHADER_VERTEX || type == PIPE_SHADER_TESS_EVAL || type == PIPE_SHADER_GEOMETRY)
      sctx->ngg_culling = 0; /* this will be enabled on the first draw if needed */

   si_invalidate_inlinable_uniforms(sctx, type);
   sctx->dirty_shaders_mask |= BITFIELD_BIT(type);
}

static void si_update_last_vgt_stage_state(struct si_context *sctx,
                                           /* hw_vs refers to the last VGT stage */
                                           struct si_shader_selector *old_hw_vs,
                                           struct si_shader *old_hw_vs_variant)
{
   struct si_shader_ctx_state *hw_vs = si_get_vs(sctx);

   si_update_vs_viewport_state(sctx);
   si_update_streamout_state(sctx);
   si_update_clip_regs(sctx, old_hw_vs, old_hw_vs_variant, hw_vs->cso, hw_vs->current);
   si_update_rasterized_prim(sctx);

   /* Clear kill_pointsize because we only want it to be set in the last shader before PS. */
   sctx->shader.vs.key.ge.opt.kill_pointsize = 0;
   sctx->shader.tes.key.ge.opt.kill_pointsize = 0;
   sctx->shader.gs.key.ge.opt.kill_pointsize = 0;
   si_vs_ps_key_update_rast_prim_smooth_stipple(sctx);
}

static void si_bind_vs_shader(struct pipe_context *ctx, void *state)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_shader_selector *sel = (struct si_shader_selector*)state;

   if (sctx->shader.vs.cso == sel)
      return;

   struct si_shader_selector *old_hw_vs = si_get_vs(sctx)->cso;
   struct si_shader *old_hw_vs_variant = si_get_vs(sctx)->current;
   bool old_uses_vbos = si_vs_uses_vbos(sctx->shader.vs.cso);
   bool new_uses_vbos = si_vs_uses_vbos(sel);

   sctx->shader.vs.cso = sel;
   sctx->shader.vs.current = (sel && sel->variants_count) ? sel->variants[0] : NULL;
#if AMD_LLVM_AVAILABLE
   sctx->shader.vs.key.ge.use_aco = sel ? sel->info.base.use_aco_amd : 0;
#endif
   sctx->num_vs_blit_sgprs = sel ? sel->info.base.vs.blit_sgprs_amd : 0;

   if (old_uses_vbos != new_uses_vbos) {
      sctx->num_vertex_elements = new_uses_vbos ? sctx->vertex_elements->count : 0;
      sctx->vertex_buffers_dirty = new_uses_vbos;
   }

   if (si_update_ngg(sctx))
      si_shader_change_notify(sctx);

   si_update_common_shader_state(sctx, sel, PIPE_SHADER_VERTEX);
   si_select_draw_vbo(sctx);
   si_update_last_vgt_stage_state(sctx, old_hw_vs, old_hw_vs_variant);
   si_vs_key_update_inputs(sctx);

   if (sctx->screen->dpbb_allowed) {
      bool force_off = sel && sel->info.options & SI_PROFILE_VS_NO_BINNING;

      if (force_off != sctx->dpbb_force_off_profile_vs) {
         sctx->dpbb_force_off_profile_vs = force_off;
         si_mark_atom_dirty(sctx, &sctx->atoms.s.dpbb_state);
      }
   }
}

static void si_update_tess_uses_prim_id(struct si_context *sctx)
{
   sctx->ia_multi_vgt_param_key.u.tess_uses_prim_id =
      sctx->shader.tes.cso &&
      ((sctx->shader.tcs.cso && sctx->shader.tcs.cso->info.uses_primid) ||
       sctx->shader.tes.cso->info.uses_primid ||
       (sctx->shader.gs.cso && sctx->shader.gs.cso->info.uses_primid) ||
       (!sctx->shader.gs.cso && sctx->shader.ps.cso && sctx->shader.ps.cso->info.uses_primid));
}

bool si_update_ngg(struct si_context *sctx)
{
   if (!sctx->screen->use_ngg) {
      assert(!sctx->ngg);
      return false;
   }

   bool new_ngg = true;

   if (sctx->shader.gs.cso && sctx->shader.tes.cso && sctx->shader.gs.cso->tess_turns_off_ngg) {
      new_ngg = false;
   } else if (sctx->gfx_level < GFX11) {
      struct si_shader_selector *last = si_get_vs(sctx)->cso;

      if ((last && last->info.enabled_streamout_buffer_mask) ||
          sctx->streamout.prims_gen_query_enabled)
         new_ngg = false;
   }

   if (new_ngg != sctx->ngg) {
      /* Transitioning from NGG to legacy GS requires VGT_FLUSH on Navi10-14.
       * VGT_FLUSH is also emitted at the beginning of IBs when legacy GS ring
       * pointers are set.
       */
      if (sctx->screen->info.has_vgt_flush_ngg_legacy_bug && !new_ngg) {
         sctx->barrier_flags |= SI_BARRIER_EVENT_VGT_FLUSH;
         si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);

         if (sctx->gfx_level == GFX10) {
            /* Workaround for https://gitlab.freedesktop.org/mesa/mesa/-/issues/2941 */
            si_flush_gfx_cs(sctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW, NULL);
         }
      }

      sctx->ngg = new_ngg;
      si_select_draw_vbo(sctx);
      return true;
   }
   return false;
}

static void si_bind_gs_shader(struct pipe_context *ctx, void *state)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_shader_selector *old_hw_vs = si_get_vs(sctx)->cso;
   struct si_shader *old_hw_vs_variant = si_get_vs(sctx)->current;
   struct si_shader_selector *sel = (struct si_shader_selector*)state;
   bool enable_changed = !!sctx->shader.gs.cso != !!sel;
   bool ngg_changed;

   if (sctx->shader.gs.cso == sel)
      return;

   sctx->shader.gs.cso = sel;
   sctx->shader.gs.current = (sel && sel->variants_count) ? sel->variants[0] : NULL;
#if AMD_LLVM_AVAILABLE
   sctx->shader.gs.key.ge.use_aco = sel ? sel->info.base.use_aco_amd : 0;
#endif
   sctx->ia_multi_vgt_param_key.u.uses_gs = sel != NULL;

   si_update_common_shader_state(sctx, sel, PIPE_SHADER_GEOMETRY);
   si_select_draw_vbo(sctx);

   ngg_changed = si_update_ngg(sctx);
   if (ngg_changed || enable_changed)
      si_shader_change_notify(sctx);
   if (enable_changed) {
      if (sctx->ia_multi_vgt_param_key.u.uses_tess)
         si_update_tess_uses_prim_id(sctx);
   }
   si_update_last_vgt_stage_state(sctx, old_hw_vs, old_hw_vs_variant);
}

static void si_bind_tcs_shader(struct pipe_context *ctx, void *state)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_shader_selector *sel = (struct si_shader_selector*)state;
   bool enable_changed = !!sctx->shader.tcs.cso != !!sel;

   /* Note it could happen that user shader sel is same as fixed function shader,
    * so we should update this field even sctx->shader.tcs.cso == sel.
    */
   sctx->is_user_tcs = !!sel;

   if (sctx->shader.tcs.cso == sel)
      return;

   sctx->shader.tcs.cso = sel;
   sctx->shader.tcs.current = (sel && sel->variants_count) ? sel->variants[0] : NULL;
#if AMD_LLVM_AVAILABLE
   sctx->shader.tcs.key.ge.use_aco = sel ? sel->info.base.use_aco_amd : 0;
#endif
   si_update_tess_uses_prim_id(sctx);
   si_update_tess_in_out_patch_vertices(sctx);

   si_update_common_shader_state(sctx, sel, PIPE_SHADER_TESS_CTRL);

   if (enable_changed)
      sctx->last_tcs = NULL; /* invalidate derived tess state */
}

static void si_bind_tes_shader(struct pipe_context *ctx, void *state)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_shader_selector *old_hw_vs = si_get_vs(sctx)->cso;
   struct si_shader *old_hw_vs_variant = si_get_vs(sctx)->current;
   struct si_shader_selector *sel = (struct si_shader_selector*)state;
   bool enable_changed = !!sctx->shader.tes.cso != !!sel;

   if (sctx->shader.tes.cso == sel)
      return;

   sctx->shader.tes.cso = sel;
   sctx->shader.tes.current = (sel && sel->variants_count) ? sel->variants[0] : NULL;
#if AMD_LLVM_AVAILABLE
   sctx->shader.tes.key.ge.use_aco = sel ? sel->info.base.use_aco_amd : 0;
#endif
   sctx->ia_multi_vgt_param_key.u.uses_tess = sel != NULL;
   si_update_tess_uses_prim_id(sctx);

   sctx->shader.tcs.key.ge.opt.tes_prim_mode =
      sel ? sel->info.base.tess._primitive_mode : 0;

   sctx->shader.tcs.key.ge.opt.tes_reads_tess_factors =
      sel ? sel->info.reads_tess_factors : 0;

   if (sel) {
      sctx->tcs_offchip_layout &= 0x1fffffff;
      sctx->tcs_offchip_layout |=
         (sel->info.base.tess._primitive_mode << 29) |
         (sel->info.reads_tess_factors << 31);

      si_mark_atom_dirty(sctx, &sctx->atoms.s.tess_io_layout);
   }

   si_update_common_shader_state(sctx, sel, PIPE_SHADER_TESS_EVAL);
   si_select_draw_vbo(sctx);

   bool ngg_changed = si_update_ngg(sctx);
   if (ngg_changed || enable_changed)
      si_shader_change_notify(sctx);
   if (enable_changed)
      sctx->last_tes_sh_base = -1; /* invalidate derived tess state */
   si_update_last_vgt_stage_state(sctx, old_hw_vs, old_hw_vs_variant);
}

void si_update_vrs_flat_shading(struct si_context *sctx)
{
   if (sctx->gfx_level >= GFX10_3 && sctx->shader.ps.cso) {
      struct si_state_rasterizer *rs = sctx->queued.named.rasterizer;
      struct si_shader_info *info = &sctx->shader.ps.cso->info;
      bool allow_flat_shading =
         info->allow_flat_shading && !sctx->framebuffer.disable_vrs_flat_shading &&
         !rs->line_smooth && !rs->poly_smooth && !rs->poly_stipple_enable &&
         !rs->point_smooth && (rs->flatshade || !info->uses_interp_color);

      if (sctx->allow_flat_shading != allow_flat_shading) {
         sctx->allow_flat_shading = allow_flat_shading;
         si_mark_atom_dirty(sctx, &sctx->atoms.s.db_render_state);
      }
   }
}

static void si_bind_ps_shader(struct pipe_context *ctx, void *state)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_shader_selector *old_sel = sctx->shader.ps.cso;
   struct si_shader_selector *sel = (struct si_shader_selector*)state;

   /* skip if supplied shader is one already in use */
   if (old_sel == sel)
      return;

   sctx->shader.ps.cso = sel;
   sctx->shader.ps.current = (sel && sel->variants_count) ? sel->variants[0] : NULL;

   si_update_common_shader_state(sctx, sel, PIPE_SHADER_FRAGMENT);
   if (sel) {
      if (sctx->ia_multi_vgt_param_key.u.uses_tess)
         si_update_tess_uses_prim_id(sctx);

      if (!old_sel || old_sel->info.colors_written != sel->info.colors_written)
         si_mark_atom_dirty(sctx, &sctx->atoms.s.cb_render_state);

      if (sctx->screen->info.has_out_of_order_rast &&
          (!old_sel || old_sel->info.base.writes_memory != sel->info.base.writes_memory ||
           old_sel->info.base.fs.early_fragment_tests !=
              sel->info.base.fs.early_fragment_tests))
         si_mark_atom_dirty(sctx, &sctx->atoms.s.msaa_config);
   }
   si_update_ps_colorbuf0_slot(sctx);

   si_ps_key_update_framebuffer(sctx);
   si_ps_key_update_framebuffer_blend_dsa_rasterizer(sctx);
   si_ps_key_update_rasterizer(sctx);
   si_ps_key_update_dsa(sctx);
   si_ps_key_update_sample_shading(sctx);
   si_ps_key_update_framebuffer_rasterizer_sample_shading(sctx);
   si_update_ps_inputs_read_or_disabled(sctx);
   si_update_vrs_flat_shading(sctx);

   if (sctx->screen->dpbb_allowed) {
      bool force_off = sel && sel->info.options & SI_PROFILE_GFX9_GFX10_PS_NO_BINNING &&
                       (sctx->gfx_level >= GFX9 && sctx->gfx_level <= GFX10_3);

      if (force_off != sctx->dpbb_force_off_profile_ps) {
         sctx->dpbb_force_off_profile_ps = force_off;
         si_mark_atom_dirty(sctx, &sctx->atoms.s.dpbb_state);
      }
   }
}

static void si_delete_shader(struct si_context *sctx, struct si_shader *shader)
{
   if (shader->is_optimized) {
      util_queue_drop_job(&sctx->screen->shader_compiler_queue_opt_variants, &shader->ready);
   }

   util_queue_fence_destroy(&shader->ready);

   /* If destroyed shaders were not unbound, the next compiled
    * shader variant could get the same pointer address and so
    * binding it to the same shader stage would be considered
    * a no-op, causing random behavior.
    */
   int state_index = -1;

   switch (shader->selector->stage) {
   case MESA_SHADER_VERTEX:
      if (shader->key.ge.as_ls) {
         if (sctx->gfx_level <= GFX8)
            state_index = SI_STATE_IDX(ls);
      } else if (shader->key.ge.as_es) {
         if (sctx->gfx_level <= GFX8)
            state_index = SI_STATE_IDX(es);
      } else if (shader->key.ge.as_ngg) {
         state_index = SI_STATE_IDX(gs);
      } else {
         state_index = SI_STATE_IDX(vs);
      }
      break;
   case MESA_SHADER_TESS_CTRL:
      state_index = SI_STATE_IDX(hs);
      break;
   case MESA_SHADER_TESS_EVAL:
      if (shader->key.ge.as_es) {
         if (sctx->gfx_level <= GFX8)
            state_index = SI_STATE_IDX(es);
      } else if (shader->key.ge.as_ngg) {
         state_index = SI_STATE_IDX(gs);
      } else {
         state_index = SI_STATE_IDX(vs);
      }
      break;
   case MESA_SHADER_GEOMETRY:
      if (shader->is_gs_copy_shader)
         state_index = SI_STATE_IDX(vs);
      else
         state_index = SI_STATE_IDX(gs);
      break;
   case MESA_SHADER_FRAGMENT:
      state_index = SI_STATE_IDX(ps);
      break;
   default:;
   }

   if (shader->gs_copy_shader)
      si_delete_shader(sctx, shader->gs_copy_shader);

   si_shader_selector_reference(sctx, &shader->previous_stage_sel, NULL);
   si_shader_destroy(shader);
   si_pm4_free_state(sctx, &shader->pm4, state_index);
}

static void si_destroy_shader_selector(struct pipe_context *ctx, void *cso)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_shader_selector *sel = (struct si_shader_selector *)cso;

   util_queue_drop_job(&sctx->screen->shader_compiler_queue, &sel->ready);

   if (sctx->shaders[sel->stage].cso == sel) {
      sctx->shaders[sel->stage].cso = NULL;
      sctx->shaders[sel->stage].current = NULL;
   }

   for (unsigned i = 0; i < sel->variants_count; i++) {
      si_delete_shader(sctx, sel->variants[i]);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(sel->main_parts.variants); i++) {
      if (sel->main_parts.variants[i])
         si_delete_shader(sctx, sel->main_parts.variants[i]);
   }

   free(sel->keys);
   free(sel->variants);

   util_queue_fence_destroy(&sel->ready);
   simple_mtx_destroy(&sel->mutex);
   ralloc_free(sel->nir);
   free(sel->nir_binary);
   free(sel);
}

static void si_delete_shader_selector(struct pipe_context *ctx, void *state)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_shader_selector *sel = (struct si_shader_selector *)state;

   si_shader_selector_reference(sctx, &sel, NULL);
}

/**
 * Writing CONFIG or UCONFIG VGT registers requires VGT_FLUSH before that.
 */
static void si_cs_preamble_add_vgt_flush(struct si_context *sctx, bool tmz)
{
   struct si_pm4_state *pm4 = tmz ? sctx->cs_preamble_state_tmz : sctx->cs_preamble_state;
   bool *has_vgt_flush = tmz ? &sctx->cs_preamble_has_vgt_flush_tmz :
                               &sctx->cs_preamble_has_vgt_flush;

   /* We shouldn't get here if registers are shadowed. */
   assert(!sctx->uses_kernelq_reg_shadowing);

   if (*has_vgt_flush)
      return;

   /* Done by Vulkan before VGT_FLUSH. */
   ac_pm4_cmd_add(&pm4->base, PKT3(PKT3_EVENT_WRITE, 0, 0));
   ac_pm4_cmd_add(&pm4->base, EVENT_TYPE(V_028A90_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));

   /* VGT_FLUSH is required even if VGT is idle. It resets VGT pointers. */
   ac_pm4_cmd_add(&pm4->base, PKT3(PKT3_EVENT_WRITE, 0, 0));
   ac_pm4_cmd_add(&pm4->base, EVENT_TYPE(V_028A90_VGT_FLUSH) | EVENT_INDEX(0));
   ac_pm4_finalize(&pm4->base);

   *has_vgt_flush = true;
}

/**
 * Writing CONFIG or UCONFIG VGT registers requires VGT_FLUSH before that.
 */
static void si_emit_vgt_flush(struct radeon_cmdbuf *cs)
{
   radeon_begin(cs);

   /* This is required before VGT_FLUSH. */
   radeon_event_write(V_028A90_VS_PARTIAL_FLUSH);

   /* VGT_FLUSH is required even if VGT is idle. It resets VGT pointers. */
   radeon_event_write(V_028A90_VGT_FLUSH);
   radeon_end();
}

/* Initialize state related to ESGS / GSVS ring buffers */
bool si_update_gs_ring_buffers(struct si_context *sctx)
{
   assert(sctx->gfx_level < GFX11);

   struct si_shader_selector *es =
      sctx->shader.tes.cso ? sctx->shader.tes.cso : sctx->shader.vs.cso;
   struct si_shader_selector *gs = sctx->shader.gs.cso;

   /* Chip constants. */
   unsigned num_se = sctx->screen->info.max_se;
   unsigned wave_size = 64;
   unsigned max_gs_waves = 32 * num_se; /* max 32 per SE on GCN */
   /* On GFX6-GFX7, the value comes from VGT_GS_VERTEX_REUSE = 16.
    * On GFX8+, the value comes from VGT_VERTEX_REUSE_BLOCK_CNTL = 30 (+2).
    */
   unsigned gs_vertex_reuse = (sctx->gfx_level >= GFX8 ? 32 : 16) * num_se;
   unsigned alignment = 256 * num_se;
   /* The maximum size is 63.999 MB per SE. */
   unsigned max_size = ((unsigned)(63.999 * 1024 * 1024) & ~255) * num_se;

   /* Calculate the minimum size. */
   unsigned min_esgs_ring_size = align(es->info.esgs_vertex_stride * gs_vertex_reuse * wave_size, alignment);

   unsigned gsvs_emit_size = 0;
   for (unsigned stream = 0; stream < 4; stream++) {
      gsvs_emit_size += (uint32_t)sctx->shader.gs.current->info.legacy_gs.num_components_per_stream[stream] *
                        4 * gs->info.base.gs.vertices_out;
   }

   /* These are recommended sizes, not minimum sizes. */
   unsigned gs_input_verts_per_prim = mesa_vertices_per_prim(gs->info.base.gs.input_primitive);
   unsigned esgs_ring_size =
      max_gs_waves * 2 * wave_size * es->info.esgs_vertex_stride * gs_input_verts_per_prim;
   unsigned gsvs_ring_size = max_gs_waves * 2 * wave_size * gsvs_emit_size;

   min_esgs_ring_size = align(min_esgs_ring_size, alignment);
   esgs_ring_size = align(esgs_ring_size, alignment);
   gsvs_ring_size = align(gsvs_ring_size, alignment);

   esgs_ring_size = CLAMP(esgs_ring_size, min_esgs_ring_size, max_size);
   gsvs_ring_size = MIN2(gsvs_ring_size, max_size);

   /* Some rings don't have to be allocated if shaders don't use them.
    * (e.g. no varyings between ES and GS or GS and VS)
    *
    * GFX9 doesn't have the ESGS ring.
    */
   bool update_esgs = sctx->gfx_level <= GFX8 && esgs_ring_size &&
                      (!sctx->esgs_ring || sctx->esgs_ring->width0 < esgs_ring_size);
   bool update_gsvs =
      gsvs_ring_size && (!sctx->gsvs_ring || sctx->gsvs_ring->width0 < gsvs_ring_size);

   if (!update_esgs && !update_gsvs)
      return true;

   if (update_esgs) {
      pipe_resource_reference(&sctx->esgs_ring, NULL);
      sctx->esgs_ring =
         pipe_aligned_buffer_create(sctx->b.screen,
                                    PIPE_RESOURCE_FLAG_UNMAPPABLE | SI_RESOURCE_FLAG_DRIVER_INTERNAL |
                                    SI_RESOURCE_FLAG_DISCARDABLE,
                                    PIPE_USAGE_DEFAULT,
                                    esgs_ring_size, sctx->screen->info.pte_fragment_size);
      if (!sctx->esgs_ring)
         return false;
   }

   if (update_gsvs) {
      pipe_resource_reference(&sctx->gsvs_ring, NULL);
      sctx->gsvs_ring =
         pipe_aligned_buffer_create(sctx->b.screen,
                                    PIPE_RESOURCE_FLAG_UNMAPPABLE | SI_RESOURCE_FLAG_DRIVER_INTERNAL |
                                    SI_RESOURCE_FLAG_DISCARDABLE,
                                    PIPE_USAGE_DEFAULT,
                                    gsvs_ring_size, sctx->screen->info.pte_fragment_size);
      if (!sctx->gsvs_ring)
         return false;
   }

   /* Set ring bindings. */
   if (sctx->esgs_ring) {
      assert(sctx->gfx_level <= GFX8);
      si_set_ring_buffer(sctx, SI_RING_ESGS, sctx->esgs_ring, 0, sctx->esgs_ring->width0, false,
                         false, 0, 0, 0);
   }
   if (sctx->gsvs_ring) {
      si_set_ring_buffer(sctx, SI_RING_GSVS, sctx->gsvs_ring, 0, sctx->gsvs_ring->width0, false,
                         false, 0, 0, 0);
   }

   if (sctx->uses_kernelq_reg_shadowing) {
      /* These registers will be shadowed, so set them only once. */
      struct radeon_cmdbuf *cs = &sctx->gfx_cs;

      assert(sctx->gfx_level >= GFX7);

      si_emit_vgt_flush(cs);

      radeon_begin(cs);

      /* Set the GS registers. */
      if (sctx->esgs_ring) {
         assert(sctx->gfx_level <= GFX8);
         radeon_set_uconfig_reg(R_030900_VGT_ESGS_RING_SIZE,
                                sctx->esgs_ring->width0 / 256);
      }
      if (sctx->gsvs_ring) {
         radeon_set_uconfig_reg(R_030904_VGT_GSVS_RING_SIZE,
                                sctx->gsvs_ring->width0 / 256);
      }
      radeon_end();
      return true;
   }

   /* The codepath without register shadowing. */
   for (unsigned tmz = 0; tmz <= 1; tmz++) {
      struct si_pm4_state *pm4 = tmz ? sctx->cs_preamble_state_tmz : sctx->cs_preamble_state;
      uint16_t *gs_ring_state_dw_offset = tmz ? &sctx->gs_ring_state_dw_offset_tmz :
                                                &sctx->gs_ring_state_dw_offset;
      unsigned old_ndw = 0;

      si_cs_preamble_add_vgt_flush(sctx, tmz);

      if (!*gs_ring_state_dw_offset) {
         /* We are here for the first time. The packets will be added. */
         *gs_ring_state_dw_offset = pm4->base.ndw;
      } else {
         /* We have been here before. Overwrite the previous packets. */
         old_ndw = pm4->base.ndw;
         pm4->base.ndw = *gs_ring_state_dw_offset;
      }

      /* Unallocated rings are written to reserve the space in the pm4
       * (to be able to overwrite them later). */
      if (sctx->gfx_level >= GFX7) {
         if (sctx->gfx_level <= GFX8)
            ac_pm4_set_reg(&pm4->base, R_030900_VGT_ESGS_RING_SIZE,
                           sctx->esgs_ring ? sctx->esgs_ring->width0 / 256 : 0);
         ac_pm4_set_reg(&pm4->base, R_030904_VGT_GSVS_RING_SIZE,
                        sctx->gsvs_ring ? sctx->gsvs_ring->width0 / 256 : 0);
      } else {
         ac_pm4_set_reg(&pm4->base, R_0088C8_VGT_ESGS_RING_SIZE,
                        sctx->esgs_ring ? sctx->esgs_ring->width0 / 256 : 0);
         ac_pm4_set_reg(&pm4->base, R_0088CC_VGT_GSVS_RING_SIZE,
                        sctx->gsvs_ring ? sctx->gsvs_ring->width0 / 256 : 0);
      }
      ac_pm4_finalize(&pm4->base);

      if (old_ndw) {
         pm4->base.ndw = old_ndw;
         pm4->base.last_opcode = 255; /* invalid opcode (we don't save the last opcode) */
      }
   }

   /* Flush the context to re-emit both cs_preamble states. */
   sctx->initial_gfx_cs_size = 0; /* force flush */
   si_flush_gfx_cs(sctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW, NULL);

   return true;
}

static void si_shader_lock(struct si_shader *shader)
{
   simple_mtx_lock(&shader->selector->mutex);
   if (shader->previous_stage_sel) {
      assert(shader->previous_stage_sel != shader->selector);
      simple_mtx_lock(&shader->previous_stage_sel->mutex);
   }
}

static void si_shader_unlock(struct si_shader *shader)
{
   if (shader->previous_stage_sel)
      simple_mtx_unlock(&shader->previous_stage_sel->mutex);
   simple_mtx_unlock(&shader->selector->mutex);
}

/**
 * @returns 1 if \p sel has been updated to use a new scratch buffer
 *          0 if not
 *          < 0 if there was a failure
 */
static int si_update_scratch_buffer(struct si_context *sctx, struct si_shader *shader)
{
   uint64_t scratch_va = sctx->scratch_buffer->gpu_address;

   if (!shader)
      return 0;

   /* This shader doesn't need a scratch buffer */
   if (shader->config.scratch_bytes_per_wave == 0)
      return 0;

   /* Prevent race conditions when updating:
    * - si_shader::scratch_va
    * - si_shader::binary::code
    * - si_shader::previous_stage::binary::code.
    */
   si_shader_lock(shader);

   /* This shader is already configured to use the current
    * scratch buffer. */
   if (shader->scratch_va == scratch_va) {
      si_shader_unlock(shader);
      return 0;
   }

   assert(sctx->scratch_buffer);

   /* Replace the shader bo with a new bo that has the relocs applied. */
   if (!si_shader_binary_upload(sctx->screen, shader, scratch_va)) {
      si_shader_unlock(shader);
      return -1;
   }

   /* Update the shader state to use the new shader bo. */
   si_shader_init_pm4_state(sctx->screen, shader);
   shader->scratch_va = scratch_va;

   si_shader_unlock(shader);
   return 1;
}

static bool si_update_scratch_relocs(struct si_context *sctx)
{
   int r;

   /* Update the shaders, so that they are using the latest scratch.
    * The scratch buffer may have been changed since these shaders were
    * last used, so we still need to try to update them, even if they
    * require scratch buffers smaller than the current size.
    */
   r = si_update_scratch_buffer(sctx, sctx->shader.ps.current);
   if (r < 0)
      return false;
   if (r == 1)
      si_pm4_bind_state(sctx, ps, sctx->shader.ps.current);

   r = si_update_scratch_buffer(sctx, sctx->shader.gs.current);
   if (r < 0)
      return false;
   if (r == 1)
      si_pm4_bind_state(sctx, gs, sctx->shader.gs.current);

   r = si_update_scratch_buffer(sctx, sctx->shader.tcs.current);
   if (r < 0)
      return false;
   if (r == 1)
      si_pm4_bind_state(sctx, hs, sctx->shader.tcs.current);

   /* VS can be bound as LS, ES, or VS. */
   r = si_update_scratch_buffer(sctx, sctx->shader.vs.current);
   if (r < 0)
      return false;
   if (r == 1) {
      if (sctx->shader.vs.current->key.ge.as_ls)
         si_pm4_bind_state(sctx, ls, sctx->shader.vs.current);
      else if (sctx->shader.vs.current->key.ge.as_es)
         si_pm4_bind_state(sctx, es, sctx->shader.vs.current);
      else if (sctx->shader.vs.current->key.ge.as_ngg)
         si_pm4_bind_state(sctx, gs, sctx->shader.vs.current);
      else
         si_pm4_bind_state(sctx, vs, sctx->shader.vs.current);
   }

   /* TES can be bound as ES or VS. */
   r = si_update_scratch_buffer(sctx, sctx->shader.tes.current);
   if (r < 0)
      return false;
   if (r == 1) {
      if (sctx->shader.tes.current->key.ge.as_es)
         si_pm4_bind_state(sctx, es, sctx->shader.tes.current);
      else if (sctx->shader.tes.current->key.ge.as_ngg)
         si_pm4_bind_state(sctx, gs, sctx->shader.tes.current);
      else
         si_pm4_bind_state(sctx, vs, sctx->shader.tes.current);
   }

   return true;
}

bool si_update_spi_tmpring_size(struct si_context *sctx, unsigned bytes)
{
   unsigned spi_tmpring_size;
   si_get_scratch_tmpring_size(sctx, bytes, false, &spi_tmpring_size);

   unsigned scratch_needed_size = sctx->max_seen_scratch_bytes_per_wave *
                                  sctx->screen->info.max_scratch_waves;

   if (scratch_needed_size > 0) {
      if (!sctx->scratch_buffer || scratch_needed_size > sctx->scratch_buffer->b.b.width0) {
         /* Create a bigger scratch buffer */
         si_resource_reference(&sctx->scratch_buffer, NULL);

         sctx->scratch_buffer = si_aligned_buffer_create(
            &sctx->screen->b,
            PIPE_RESOURCE_FLAG_UNMAPPABLE | SI_RESOURCE_FLAG_DRIVER_INTERNAL |
            SI_RESOURCE_FLAG_DISCARDABLE,
            PIPE_USAGE_DEFAULT, scratch_needed_size,
            sctx->screen->info.pte_fragment_size);
         if (!sctx->scratch_buffer)
            return false;
      }

      if (!sctx->screen->info.has_scratch_base_registers && !si_update_scratch_relocs(sctx))
         return false;
   }

   if (spi_tmpring_size != sctx->spi_tmpring_size) {
      sctx->spi_tmpring_size = spi_tmpring_size;
      si_mark_atom_dirty(sctx, &sctx->atoms.s.scratch_state);
   }
   return true;
}

void si_init_tess_factor_ring(struct si_context *sctx)
{
   struct si_screen *sscreen = sctx->screen;
   assert(!sctx->has_tessellation);

   if (sctx->has_tessellation)
      return;

   simple_mtx_lock(&sscreen->tess_ring_lock);

   if (!sscreen->tess_rings) {
      /* The address must be aligned to 2^19, because the shader only
       * receives the high 13 bits. Align it to 2MB to match the GPU page size.
       */
      sscreen->tess_rings = pipe_aligned_buffer_create(sctx->b.screen,
                                                       PIPE_RESOURCE_FLAG_UNMAPPABLE |
                                                       SI_RESOURCE_FLAG_32BIT |
                                                       SI_RESOURCE_FLAG_DRIVER_INTERNAL |
                                                       SI_RESOURCE_FLAG_DISCARDABLE,
                                                       PIPE_USAGE_DEFAULT,
                                                       sscreen->info.total_tess_ring_size,
                                                       2 * 1024 * 1024);
      if (!sscreen->tess_rings) {
         simple_mtx_unlock(&sscreen->tess_ring_lock);
         return;
      }

      if (sscreen->info.has_tmz_support) {
         sscreen->tess_rings_tmz = pipe_aligned_buffer_create(sctx->b.screen,
                                                              PIPE_RESOURCE_FLAG_UNMAPPABLE |
                                                              PIPE_RESOURCE_FLAG_ENCRYPTED |
                                                              SI_RESOURCE_FLAG_32BIT |
                                                              SI_RESOURCE_FLAG_DRIVER_INTERNAL |
                                                              SI_RESOURCE_FLAG_DISCARDABLE,
                                                              PIPE_USAGE_DEFAULT,
                                                              sscreen->info.total_tess_ring_size,
                                                              2 * 1024 * 1024);
      }
   }

   simple_mtx_unlock(&sscreen->tess_ring_lock);
   sctx->has_tessellation = true;

   si_mark_atom_dirty(sctx, &sctx->atoms.s.spi_ge_ring_state);
}

static void si_emit_vgt_pipeline_state(struct si_context *sctx, unsigned index)
{
   struct radeon_cmdbuf *cs = &sctx->gfx_cs;

   radeon_begin(cs);
   radeon_opt_set_context_reg(sctx->gfx_level >= GFX12 ?
                                 R_028A98_VGT_SHADER_STAGES_EN :
                                 R_028B54_VGT_SHADER_STAGES_EN,
                              SI_TRACKED_VGT_SHADER_STAGES_EN, sctx->vgt_shader_stages_en);
   if (sctx->gfx_level == GFX10_3) {
      /* Legacy Tess+GS should disable reuse to prevent hangs on GFX10.3. */
      bool has_legacy_tess_gs = G_028B54_HS_EN(sctx->vgt_shader_stages_en) &&
                                G_028B54_GS_EN(sctx->vgt_shader_stages_en) &&
                                !G_028B54_PRIMGEN_EN(sctx->vgt_shader_stages_en); /* !NGG */

      radeon_opt_set_context_reg(R_028AB4_VGT_REUSE_OFF, SI_TRACKED_VGT_REUSE_OFF,
                                 S_028AB4_REUSE_OFF(has_legacy_tess_gs));
   }
   radeon_end_update_context_roll();

   if (sctx->gfx_level >= GFX10) {
      uint32_t ge_cntl = sctx->ge_cntl;

      if (sctx->gfx_level < GFX11 && sctx->shader.tes.cso) {
         /* This must be a multiple of VGT_LS_HS_CONFIG.NUM_PATCHES. */
         ge_cntl |= S_03096C_PRIM_GRP_SIZE_GFX10(sctx->num_patches_per_workgroup);
      }

      radeon_begin_again(cs);
      radeon_opt_set_uconfig_reg(R_03096C_GE_CNTL, SI_TRACKED_GE_CNTL, ge_cntl);
      radeon_end();
   }
}

static void si_emit_scratch_state(struct si_context *sctx, unsigned index)
{
   struct radeon_cmdbuf *cs = &sctx->gfx_cs;

   radeon_begin(cs);
   if (sctx->gfx_level >= GFX11) {
      radeon_set_context_reg_seq(R_0286E8_SPI_TMPRING_SIZE, 3);
      radeon_emit(sctx->spi_tmpring_size);                  /* SPI_TMPRING_SIZE */
      radeon_emit(sctx->scratch_buffer->gpu_address >> 8);  /* SPI_GFX_SCRATCH_BASE_LO */
      radeon_emit(sctx->scratch_buffer->gpu_address >> 40); /* SPI_GFX_SCRATCH_BASE_HI */
   } else {
      radeon_set_context_reg(R_0286E8_SPI_TMPRING_SIZE, sctx->spi_tmpring_size);
   }
   radeon_end();

   if (sctx->scratch_buffer) {
      radeon_add_to_buffer_list(sctx, &sctx->gfx_cs, sctx->scratch_buffer,
                                RADEON_USAGE_READWRITE | RADEON_PRIO_SCRATCH_BUFFER);
   }
}

struct si_fixed_func_tcs_shader_key {
   uint64_t outputs_written;
   uint8_t vertices_out;
};

DERIVE_HASH_TABLE(si_fixed_func_tcs_shader_key);

bool si_set_tcs_to_fixed_func_shader(struct si_context *sctx)
{
   if (!sctx->fixed_func_tcs_shader_cache) {
      sctx->fixed_func_tcs_shader_cache = si_fixed_func_tcs_shader_key_table_create(NULL);
   }

   struct si_fixed_func_tcs_shader_key key;
   key.outputs_written = sctx->shader.vs.cso->info.ls_es_outputs_written;
   key.vertices_out = sctx->patch_vertices;

   struct hash_entry *entry = _mesa_hash_table_search(
      sctx->fixed_func_tcs_shader_cache, &key);

   struct si_shader_selector *tcs;
   if (entry)
      tcs = (struct si_shader_selector *)entry->data;
   else {
      tcs = (struct si_shader_selector *)si_create_passthrough_tcs(sctx);
      if (!tcs)
         return false;
      _mesa_hash_table_insert(sctx->fixed_func_tcs_shader_cache, &key, (void *)tcs);
   }

   sctx->shader.tcs.cso = tcs;
   return true;
}

static void si_update_tess_in_out_patch_vertices(struct si_context *sctx)
{
   if (sctx->is_user_tcs) {
      struct si_shader_selector *tcs = sctx->shader.tcs.cso;

      bool same_patch_vertices =
         sctx->gfx_level >= GFX9 &&
         sctx->patch_vertices == tcs->info.base.tess.tcs_vertices_out;

      if (sctx->shader.tcs.key.ge.opt.same_patch_vertices != same_patch_vertices) {
         sctx->shader.tcs.key.ge.opt.same_patch_vertices = same_patch_vertices;
         sctx->dirty_shaders_mask |= BITFIELD_BIT(PIPE_SHADER_TESS_CTRL);
      }
   } else {
      /* These fields are static for fixed function TCS. So no need to set
       * dirty_shaders_mask between fixed-TCS draws. As fixed-TCS to user-TCS
       * or opposite, dirty_shaders_mask should already be set by bind state.
       */
      sctx->shader.tcs.key.ge.opt.same_patch_vertices = sctx->gfx_level >= GFX9;

      /* User may only change patch vertices, needs to update fixed func TCS. */
      if (sctx->shader.tcs.cso &&
          sctx->shader.tcs.cso->info.base.tess.tcs_vertices_out != sctx->patch_vertices)
         sctx->dirty_shaders_mask |= BITFIELD_BIT(PIPE_SHADER_TESS_CTRL);
   }
}

static void si_set_patch_vertices(struct pipe_context *ctx, uint8_t patch_vertices)
{
   struct si_context *sctx = (struct si_context *)ctx;

   if (sctx->patch_vertices != patch_vertices) {
      sctx->patch_vertices = patch_vertices;
      si_update_tess_in_out_patch_vertices(sctx);
      if (sctx->shader.tcs.current) {
         /* Update the io layout now if possible,
          * otherwise make sure it's done by si_update_shaders.
          */
         if (sctx->has_tessellation)
            si_update_tess_io_layout_state(sctx);
         else
            sctx->dirty_shaders_mask |= BITFIELD_BIT(PIPE_SHADER_TESS_CTRL);
      }

      /* Gfx12 programs patch_vertices in VGT_PRIMITIVE_TYPE.NUM_INPUT_CP. Make sure
       * the register is updated.
       */
      if (sctx->gfx_level >= GFX12 && sctx->last_prim == MESA_PRIM_PATCHES)
         sctx->last_prim = -1;
   }
}

unsigned si_shader_lshs_vertex_stride(struct si_shader *ls)
{
   unsigned num_slots;

   if (ls->selector->stage == MESA_SHADER_VERTEX && !ls->next_shader) {
      assert(ls->key.ge.as_ls);
      assert(ls->selector->screen->info.gfx_level <= GFX8 || !ls->is_monolithic);
      num_slots = util_last_bit64(ls->selector->info.ls_es_outputs_written);
   } else {
      struct si_shader *tcs = ls->next_shader ? ls->next_shader : ls;

      assert(tcs->selector->stage == MESA_SHADER_TESS_CTRL);
      assert(tcs->selector->screen->info.gfx_level >= GFX9);

      if (tcs->is_monolithic) {
         uint64_t lds_inputs_read = tcs->selector->info.tcs_inputs_via_lds;

         /* If the TCS in/out number of vertices is different, all inputs are passed via LDS. */
         if (!tcs->key.ge.opt.same_patch_vertices)
            lds_inputs_read |= tcs->selector->info.tcs_inputs_via_temp;

         /* NIR lowering passes pack LS outputs/HS inputs if the usage masks of both are known. */
         num_slots = util_bitcount64(lds_inputs_read);
      } else {
         num_slots = util_last_bit64(tcs->previous_stage_sel->info.ls_es_outputs_written);
      }
   }

   /* Add 1 dword to reduce LDS bank conflicts, so that each vertex starts on a different LDS
    * bank.
    */
   return num_slots ? num_slots * 16 + 4 : 0;
}

/**
 * This calculates the LDS size for tessellation shaders (VS, TCS, TES).
 * LS.LDS_SIZE is shared by all 3 shader stages.
 *
 * The information about LDS and other non-compile-time parameters is then
 * written to userdata SGPRs.
 *
 * This depends on:
 * - patch_vertices
 * - VS and the currently selected shader variant (called by si_update_shaders)
 * - TCS and the currently selected shader variant (called by si_update_shaders)
 * - tess_uses_prim_id (called by si_update_shaders)
 * - sh_base[TESS_EVAL] depending on GS on/off (called by si_update_shaders)
 */
void si_update_tess_io_layout_state(struct si_context *sctx)
{
   struct si_shader *ls_current;
   struct si_shader_selector *tcs = sctx->shader.tcs.cso;
   bool tess_uses_primid = sctx->ia_multi_vgt_param_key.u.tess_uses_prim_id;
   bool has_primid_instancing_bug = sctx->gfx_level == GFX6 && sctx->screen->info.max_se == 1;
   unsigned tes_sh_base = sctx->shader_pointers.sh_base[PIPE_SHADER_TESS_EVAL];
   uint8_t num_tcs_input_cp = sctx->patch_vertices;

   assert(sctx->shader.tcs.current);

   /* Since GFX9 has merged LS-HS in the TCS state, set LS = TCS. */
   if (sctx->gfx_level >= GFX9) {
      ls_current = sctx->shader.tcs.current;
   } else {
      ls_current = sctx->shader.vs.current;

      if (!ls_current) {
         sctx->dirty_shaders_mask |= BITFIELD_BIT(PIPE_SHADER_VERTEX);
         return;
      }
   }

   if (sctx->last_ls == ls_current && sctx->last_tcs == tcs &&
       sctx->last_tes_sh_base == tes_sh_base && sctx->last_num_tcs_input_cp == num_tcs_input_cp &&
       (!has_primid_instancing_bug || (sctx->last_tess_uses_primid == tess_uses_primid)))
      return;

   sctx->last_ls = ls_current;
   sctx->last_tcs = tcs;
   sctx->last_tes_sh_base = tes_sh_base;
   sctx->last_num_tcs_input_cp = num_tcs_input_cp;
   sctx->last_tess_uses_primid = tess_uses_primid;

   /* This calculates how shader inputs and outputs among VS, TCS, and TES
    * are laid out in LDS and memory.
    */
   unsigned num_tcs_output_cp = tcs->info.base.tess.tcs_vertices_out;
   unsigned lds_input_vertex_size = si_shader_lshs_vertex_stride(ls_current);
   unsigned num_remapped_tess_level_outputs =
      !ls_current->is_monolithic || ls_current->key.ge.opt.tes_reads_tess_factors ?
            tcs->info.num_tess_level_vram_outputs : 0;
   unsigned num_patches, lds_size;

   /* Compute NUM_PATCHES and LDS_SIZE. */
   ac_nir_compute_tess_wg_info(&sctx->screen->info, &tcs->info.tess_io_info,
                               tcs->info.base.tess.tcs_vertices_out, ls_current->wave_size,
                               tess_uses_primid, num_tcs_input_cp, lds_input_vertex_size,
                               num_remapped_tess_level_outputs, &num_patches, &lds_size);

   if (sctx->num_patches_per_workgroup != num_patches) {
      sctx->num_patches_per_workgroup = num_patches;
      si_mark_atom_dirty(sctx, &sctx->atoms.s.vgt_pipeline_state);
   }

   /* Compute userdata SGPRs. */
   unsigned num_lds_vs_outputs = lds_input_vertex_size / 16;
   unsigned tcs_mem_attrib_stride = align(num_patches * num_tcs_output_cp * 16, 256) / 256;

   assert(ls_current->config.lds_size == 0);
   assert(num_tcs_input_cp <= 32);
   assert(num_tcs_output_cp <= 32);
   assert(num_patches <= 127);
   assert(tcs_mem_attrib_stride <= 31);
   assert(num_lds_vs_outputs <= 63);
   assert(tcs->info.tess_io_info.highest_remapped_vram_output <= 63);

   uint64_t ring_va =
      sctx->ws->cs_is_secure(&sctx->gfx_cs) ?
          si_resource(sctx->screen->tess_rings_tmz)->gpu_address :
          si_resource(sctx->screen->tess_rings)->gpu_address;
   assert((ring_va & BITFIELD_MASK(19)) == 0);

   unsigned shared_fields = num_patches | (tcs_mem_attrib_stride << 12) |
                            (num_lds_vs_outputs << 17) |
                            (tcs->info.tess_io_info.highest_remapped_vram_output << 23);

   sctx->tes_offchip_ring_va_sgpr = ring_va;
   sctx->tcs_offchip_layout = (sctx->tcs_offchip_layout & 0xe0000000) |
                              shared_fields | ((num_tcs_input_cp - 1) << 7);
   sctx->tes_offchip_layout = shared_fields | ((num_tcs_output_cp - 1) << 7);

   unsigned ls_hs_rsrc2;

   if (sctx->gfx_level >= GFX9) {
      ls_hs_rsrc2 = sctx->shader.tcs.current->config.rsrc2;

      if (sctx->gfx_level >= GFX10)
         ls_hs_rsrc2 |= S_00B42C_LDS_SIZE_GFX10(lds_size);
      else
         ls_hs_rsrc2 |= S_00B42C_LDS_SIZE_GFX9(lds_size);
   } else {
      ls_hs_rsrc2 = sctx->shader.vs.current->config.rsrc2;

      si_multiwave_lds_size_workaround(sctx->screen, &lds_size);
      ls_hs_rsrc2 |= S_00B52C_LDS_SIZE(lds_size);
   }

   sctx->ls_hs_rsrc2 = ls_hs_rsrc2;
   sctx->ls_hs_config =
         S_028B58_NUM_PATCHES(sctx->num_patches_per_workgroup) |
         S_028B58_HS_NUM_OUTPUT_CP(num_tcs_output_cp);

   if (sctx->gfx_level < GFX12)
      sctx->ls_hs_config |= S_028B58_HS_NUM_INPUT_CP(num_tcs_input_cp);

   si_mark_atom_dirty(sctx, &sctx->atoms.s.tess_io_layout);
}

static void gfx6_emit_tess_io_layout_state(struct si_context *sctx, unsigned index)
{
   struct radeon_cmdbuf *cs = &sctx->gfx_cs;

   assert(sctx->gfx_level < GFX12);

   if (!sctx->shader.tes.cso || !sctx->shader.tcs.current)
      return;

   radeon_begin(cs);
   if (sctx->screen->info.has_set_sh_pairs_packed) {
      gfx11_opt_push_gfx_sh_reg(R_00B42C_SPI_SHADER_PGM_RSRC2_HS,
                                SI_TRACKED_SPI_SHADER_PGM_RSRC2_HS, sctx->ls_hs_rsrc2);

      /* Set userdata SGPRs for merged LS-HS. */
      gfx11_opt_push_gfx_sh_reg(R_00B430_SPI_SHADER_USER_DATA_HS_0 +
                                GFX9_SGPR_TCS_OFFCHIP_LAYOUT * 4,
                                SI_TRACKED_SPI_SHADER_USER_DATA_HS__TCS_OFFCHIP_LAYOUT,
                                sctx->tcs_offchip_layout);
      gfx11_opt_push_gfx_sh_reg(R_00B430_SPI_SHADER_USER_DATA_HS_0 +
                                GFX9_SGPR_TCS_OFFCHIP_ADDR * 4,
                                SI_TRACKED_SPI_SHADER_USER_DATA_HS__TCS_OFFCHIP_ADDR,
                                sctx->tes_offchip_ring_va_sgpr);
   } else if (sctx->gfx_level >= GFX9) {
      radeon_opt_set_sh_reg(R_00B42C_SPI_SHADER_PGM_RSRC2_HS,
                            SI_TRACKED_SPI_SHADER_PGM_RSRC2_HS, sctx->ls_hs_rsrc2);

      /* Set userdata SGPRs for merged LS-HS. */
      radeon_opt_set_sh_reg2(R_00B430_SPI_SHADER_USER_DATA_HS_0 +
                             GFX9_SGPR_TCS_OFFCHIP_LAYOUT * 4,
                             SI_TRACKED_SPI_SHADER_USER_DATA_HS__TCS_OFFCHIP_LAYOUT,
                             sctx->tcs_offchip_layout, sctx->tes_offchip_ring_va_sgpr);
   } else {
      /* Due to a hw bug, RSRC2_LS must be written twice with another
       * LS register written in between. */
      if (sctx->gfx_level == GFX7 && sctx->family != CHIP_HAWAII)
         radeon_set_sh_reg(R_00B52C_SPI_SHADER_PGM_RSRC2_LS, sctx->ls_hs_rsrc2);
      radeon_set_sh_reg_seq(R_00B528_SPI_SHADER_PGM_RSRC1_LS, 2);
      radeon_emit(sctx->shader.vs.current->config.rsrc1);
      radeon_emit(sctx->ls_hs_rsrc2);

      /* Set userdata SGPRs for TCS. */
      radeon_opt_set_sh_reg3(R_00B430_SPI_SHADER_USER_DATA_HS_0 +
                             GFX6_SGPR_TCS_OFFCHIP_LAYOUT * 4,
                             SI_TRACKED_SPI_SHADER_USER_DATA_HS__TCS_OFFCHIP_LAYOUT,
                             sctx->tcs_offchip_layout, sctx->tes_offchip_ring_va_sgpr,
                             sctx->current_vs_state);
   }

   /* Set userdata SGPRs for TES. */
   unsigned tes_sh_base = sctx->shader_pointers.sh_base[PIPE_SHADER_TESS_EVAL];
   assert(tes_sh_base);

   /* TES (as ES or VS) reuses the BaseVertex and DrawID user SGPRs that are used when
    * tessellation is disabled. We can do that because those user SGPRs are only set in LS
    * for tessellation and are unused in TES.
    */
   if (sctx->screen->info.has_set_sh_pairs_packed) {
      gfx11_opt_push_gfx_sh_reg(tes_sh_base + SI_SGPR_TES_OFFCHIP_LAYOUT * 4,
                                SI_TRACKED_SPI_SHADER_USER_DATA_ES__BASE_VERTEX,
                                sctx->tes_offchip_layout);
      gfx11_opt_push_gfx_sh_reg(tes_sh_base + SI_SGPR_TES_OFFCHIP_ADDR * 4,
                                SI_TRACKED_SPI_SHADER_USER_DATA_ES__DRAWID,
                                sctx->tes_offchip_ring_va_sgpr);
   } else if (sctx->ngg || sctx->shader.gs.cso) {
      radeon_opt_set_sh_reg2(tes_sh_base + SI_SGPR_TES_OFFCHIP_LAYOUT * 4,
                             SI_TRACKED_SPI_SHADER_USER_DATA_ES__BASE_VERTEX,
                             sctx->tes_offchip_layout, sctx->tes_offchip_ring_va_sgpr);
   } else {
      radeon_opt_set_sh_reg2(tes_sh_base + SI_SGPR_TES_OFFCHIP_LAYOUT * 4,
                             SI_TRACKED_SPI_SHADER_USER_DATA_VS__BASE_VERTEX,
                             sctx->tes_offchip_layout, sctx->tes_offchip_ring_va_sgpr);
   }
   radeon_end();

   radeon_begin_again(cs);
   if (sctx->gfx_level >= GFX7) {
      radeon_opt_set_context_reg_idx(R_028B58_VGT_LS_HS_CONFIG,
                                     SI_TRACKED_VGT_LS_HS_CONFIG, 2, sctx->ls_hs_config);
   } else {
      radeon_opt_set_context_reg(R_028B58_VGT_LS_HS_CONFIG,
                                 SI_TRACKED_VGT_LS_HS_CONFIG, sctx->ls_hs_config);
   }
   radeon_end_update_context_roll();
}

static void gfx12_emit_tess_io_layout_state(struct si_context *sctx, unsigned index)
{
   struct radeon_cmdbuf *cs = &sctx->gfx_cs;

   if (!sctx->shader.tes.cso || !sctx->shader.tcs.current)
      return;

   gfx12_opt_push_gfx_sh_reg(R_00B42C_SPI_SHADER_PGM_RSRC2_HS,
                             SI_TRACKED_SPI_SHADER_PGM_RSRC2_HS, sctx->ls_hs_rsrc2);
   /* Set userdata SGPRs for merged LS-HS. */
   gfx12_opt_push_gfx_sh_reg(R_00B430_SPI_SHADER_USER_DATA_HS_0 +
                             GFX9_SGPR_TCS_OFFCHIP_LAYOUT * 4,
                             SI_TRACKED_SPI_SHADER_USER_DATA_HS__TCS_OFFCHIP_LAYOUT,
                             sctx->tcs_offchip_layout);
   gfx12_opt_push_gfx_sh_reg(R_00B430_SPI_SHADER_USER_DATA_HS_0 +
                             GFX9_SGPR_TCS_OFFCHIP_ADDR * 4,
                             SI_TRACKED_SPI_SHADER_USER_DATA_HS__TCS_OFFCHIP_ADDR,
                             sctx->tes_offchip_ring_va_sgpr);

   /* Set userdata SGPRs for TES. */
   unsigned tes_sh_base = sctx->shader_pointers.sh_base[PIPE_SHADER_TESS_EVAL];
   assert(tes_sh_base);

   /* TES (as ES or VS) reuses the BaseVertex and DrawID user SGPRs that are used when
    * tessellation is disabled. We can do that because those user SGPRs are only set in LS
    * for tessellation and are unused in TES.
    */
   gfx12_opt_push_gfx_sh_reg(tes_sh_base + SI_SGPR_TES_OFFCHIP_LAYOUT * 4,
                             SI_TRACKED_SPI_SHADER_USER_DATA_ES__BASE_VERTEX,
                             sctx->tes_offchip_layout);
   gfx12_opt_push_gfx_sh_reg(tes_sh_base + SI_SGPR_TES_OFFCHIP_ADDR * 4,
                             SI_TRACKED_SPI_SHADER_USER_DATA_ES__DRAWID,
                             sctx->tes_offchip_ring_va_sgpr);

   radeon_begin(cs);
   radeon_opt_set_context_reg_idx(R_028B58_VGT_LS_HS_CONFIG,
                                  SI_TRACKED_VGT_LS_HS_CONFIG, 2, sctx->ls_hs_config);
   radeon_end(); /* don't track context rolls on GFX12 */
}

void si_init_screen_live_shader_cache(struct si_screen *sscreen)
{
   util_live_shader_cache_init(&sscreen->live_shader_cache, si_create_shader_selector,
                               si_destroy_shader_selector);
}

template<int NUM_INTERP>
static void si_emit_spi_map(struct si_context *sctx, unsigned index)
{
   struct si_shader *ps = sctx->shader.ps.current;
   struct si_shader *vs = si_get_vs(sctx)->current;
   unsigned spi_ps_input_cntl[NUM_INTERP];

   STATIC_ASSERT(NUM_INTERP >= 0 && NUM_INTERP <= 32);

   if (sctx->gfx_level >= GFX12) {
      gfx12_opt_push_gfx_sh_reg(R_00B0C4_SPI_SHADER_GS_OUT_CONFIG_PS,
                                SI_TRACKED_SPI_SHADER_GS_OUT_CONFIG_PS,
                                vs->ngg.spi_vs_out_config | ps->ps.spi_gs_out_config_ps);
   }

   if (!NUM_INTERP)
      return;

   struct si_state_rasterizer *rs = sctx->queued.named.rasterizer;

   for (unsigned i = 0; i < NUM_INTERP; i++) {
      union si_ps_input_info input = ps->info.ps_inputs[i];
      unsigned ps_input_cntl = vs->info.vs_output_ps_input_cntl[input.semantic];
      bool non_default_val = G_028644_OFFSET(ps_input_cntl) != 0x20;

      if (non_default_val) {
         if (input.interpolate == INTERP_MODE_FLAT ||
             (input.interpolate == INTERP_MODE_COLOR && rs->flatshade))
            ps_input_cntl |= S_028644_FLAT_SHADE(1);

         if (input.fp16_lo_hi_valid) {
            ps_input_cntl |= S_028644_FP16_INTERP_MODE(1) |
                             S_028644_ATTR0_VALID(1) | /* this must be set if FP16_INTERP_MODE is set */
                             S_028644_ATTR1_VALID(!!(input.fp16_lo_hi_valid & 0x2));
         }
      }

      if (input.semantic == VARYING_SLOT_PNTC ||
          (input.semantic >= VARYING_SLOT_TEX0 && input.semantic <= VARYING_SLOT_TEX7 &&
           rs->sprite_coord_enable & (1 << (input.semantic - VARYING_SLOT_TEX0)))) {
         /* Overwrite the whole value (except OFFSET) for sprite coordinates. */
         ps_input_cntl &= ~C_028644_OFFSET;
         ps_input_cntl |= S_028644_PT_SPRITE_TEX(1);
         if (input.fp16_lo_hi_valid & 0x1) {
            ps_input_cntl |= S_028644_FP16_INTERP_MODE(1) |
                             S_028644_ATTR0_VALID(1);
         }
      }

      spi_ps_input_cntl[i] = ps_input_cntl;
   }

   /* Performance notes:
    *    Dota 2: Only ~16% of SPI map updates set different values.
    *    Talos: Only ~9% of SPI map updates set different values.
    */
   if (sctx->gfx_level >= GFX12) {
      radeon_begin(&sctx->gfx_cs);
      radeon_opt_set_context_regn(R_028664_SPI_PS_INPUT_CNTL_0, spi_ps_input_cntl,
                                  sctx->tracked_regs.spi_ps_input_cntl, NUM_INTERP);
      radeon_end(); /* don't track context rolls on GFX12 */
   } else {
      radeon_begin(&sctx->gfx_cs);
      radeon_opt_set_context_regn(R_028644_SPI_PS_INPUT_CNTL_0, spi_ps_input_cntl,
                                  sctx->tracked_regs.spi_ps_input_cntl, NUM_INTERP);
      radeon_end_update_context_roll();
   }
}

static void si_emit_spi_ge_ring_state(struct si_context *sctx, unsigned index)
{
   struct si_screen *sscreen = sctx->screen;

   if (sctx->has_tessellation) {
      struct pipe_resource *tf_ring =
         sctx->ws->cs_is_secure(&sctx->gfx_cs) ? sscreen->tess_rings_tmz : sscreen->tess_rings;
      uint64_t factor_va = si_resource(tf_ring)->gpu_address +
                           sscreen->info.tess_offchip_ring_size;

      unsigned tf_ring_size_field = sscreen->info.tess_factor_ring_size / 4;
      if (sctx->gfx_level >= GFX11)
         tf_ring_size_field /= sscreen->info.max_se;

      assert((tf_ring_size_field & C_030938_SIZE) == 0);

      radeon_add_to_buffer_list(sctx, &sctx->gfx_cs, si_resource(tf_ring),
                                RADEON_USAGE_READWRITE | RADEON_PRIO_SHADER_RINGS);

      radeon_begin(&sctx->gfx_cs);
      /* Required before writing tessellation config registers. */
      radeon_event_write(V_028A90_VS_PARTIAL_FLUSH);
      radeon_event_write(V_028A90_VGT_FLUSH);

      if (sctx->gfx_level >= GFX7) {
         radeon_set_uconfig_reg_seq(R_030938_VGT_TF_RING_SIZE, 3);
         radeon_emit(S_030938_SIZE(tf_ring_size_field)); /* R_030938_VGT_TF_RING_SIZE */
         radeon_emit(sscreen->info.hs_offchip_param);      /* R_03093C_VGT_HS_OFFCHIP_PARAM */
         radeon_emit(factor_va >> 8);                    /* R_030940_VGT_TF_MEMORY_BASE */

         if (sctx->gfx_level >= GFX12)
            radeon_set_uconfig_reg(R_03099C_VGT_TF_MEMORY_BASE_HI, S_03099C_BASE_HI(factor_va >> 40));
         else if (sctx->gfx_level >= GFX10)
            radeon_set_uconfig_reg(R_030984_VGT_TF_MEMORY_BASE_HI, S_030984_BASE_HI(factor_va >> 40));
         else if (sctx->gfx_level == GFX9)
            radeon_set_uconfig_reg(R_030944_VGT_TF_MEMORY_BASE_HI, S_030944_BASE_HI(factor_va >> 40));
      } else {
         radeon_set_config_reg(R_008988_VGT_TF_RING_SIZE, S_008988_SIZE(tf_ring_size_field));
         radeon_set_config_reg(R_0089B8_VGT_TF_MEMORY_BASE, factor_va >> 8);
         radeon_set_config_reg(R_0089B0_VGT_HS_OFFCHIP_PARAM, sscreen->info.hs_offchip_param);
      }
      radeon_end();
   }

   if (sctx->gfx_level >= GFX11) {
      /* We must wait for idle using an EOP event before changing the attribute ring registers.
       * Use the bottom-of-pipe EOP event, but use the PWS TS counter instead of the counter
       * in memory.
       */
      si_cp_release_acquire_mem_pws(sctx, &sctx->gfx_cs, V_028A90_BOTTOM_OF_PIPE_TS, 0,
                                    V_580_CP_ME, 0);

      uint64_t attr_address = sctx->ws->cs_is_secure(&sctx->gfx_cs) ?
         sscreen->attribute_pos_prim_ring_tmz->gpu_address :
         sscreen->attribute_pos_prim_ring->gpu_address;
      assert((attr_address >> 32) == sscreen->info.address32_hi);

      radeon_begin(&sctx->gfx_cs);
      radeon_set_uconfig_reg_seq(R_031110_SPI_GS_THROTTLE_CNTL1, 4);
      radeon_emit(0x12355123);      /* SPI_GS_THROTTLE_CNTL1 */
      radeon_emit(0x1544D);         /* SPI_GS_THROTTLE_CNTL2 */
      radeon_emit(attr_address >> 16); /* SPI_ATTRIBUTE_RING_BASE */
      radeon_emit(S_03111C_MEM_SIZE((sscreen->info.attribute_ring_size_per_se >> 16) - 1) |
                  S_03111C_BIG_PAGE(sscreen->info.discardable_allows_big_page) |
                  S_03111C_L1_POLICY(1)); /* SPI_ATTRIBUTE_RING_SIZE */

      if (sctx->gfx_level >= GFX12) {
         uint64_t pos_address = attr_address + sscreen->info.pos_ring_offset;
         uint64_t prim_address = attr_address + sscreen->info.prim_ring_offset;

         /* When one of these 4 registers is updated, all 4 must be updated. */
         radeon_set_uconfig_reg_seq(R_0309A0_GE_POS_RING_BASE, 4);
         radeon_emit(pos_address >> 16);              /* R_0309A0_GE_POS_RING_BASE */
         radeon_emit(S_0309A4_MEM_SIZE(sscreen->info.pos_ring_size_per_se >> 5)); /* R_0309A4_GE_POS_RING_SIZE */
         radeon_emit(prim_address >> 16);             /* R_0309A8_GE_PRIM_RING_BASE */
         radeon_emit(S_0309AC_MEM_SIZE(sscreen->info.prim_ring_size_per_se >> 5) |
                     S_0309AC_SCOPE(gfx12_scope_device) |
                     S_0309AC_PAF_TEMPORAL(gfx12_store_high_temporal_stay_dirty) |
                     S_0309AC_PAB_TEMPORAL(gfx12_load_last_use_discard) |
                     S_0309AC_SPEC_DATA_READ(gfx12_spec_read_auto) |
                     S_0309AC_FORCE_SE_SCOPE(1) |
                     S_0309AC_PAB_NOFILL(1));         /* R_0309AC_GE_PRIM_RING_SIZE */

         if (sctx->gfx_level == GFX12 && sscreen->info.pfp_fw_version >= 2680) {
            /* Mitigate the HiZ GPU hang by increasing a timeout when
             * BOTTOM_OF_PIPE_TS is used as the workaround. This must be
             * emitted when the gfx queue is idle.
             */
            const uint32_t timeout = sscreen->options.alt_hiz_logic ? 0xfff : 0;

            radeon_emit(PKT3(PKT3_UPDATE_DB_SUMMARIZER_TIMEOUT, 0, 0));
            radeon_emit(S_EF1_SUMM_CNTL_EVICT_TIMEOUT(timeout));
         }
      }
      radeon_end();
   }
}

void si_init_shader_functions(struct si_context *sctx)
{
   sctx->atoms.s.vgt_pipeline_state.emit = si_emit_vgt_pipeline_state;
   sctx->atoms.s.scratch_state.emit = si_emit_scratch_state;
   sctx->atoms.s.spi_ge_ring_state.emit = si_emit_spi_ge_ring_state;

   if (sctx->gfx_level >= GFX12)
      sctx->atoms.s.tess_io_layout.emit = gfx12_emit_tess_io_layout_state;
   else
      sctx->atoms.s.tess_io_layout.emit = gfx6_emit_tess_io_layout_state;

   sctx->b.create_vs_state = si_create_shader;
   sctx->b.create_tcs_state = si_create_shader;
   sctx->b.create_tes_state = si_create_shader;
   sctx->b.create_gs_state = si_create_shader;
   sctx->b.create_fs_state = si_create_shader;

   sctx->b.bind_vs_state = si_bind_vs_shader;
   sctx->b.bind_tcs_state = si_bind_tcs_shader;
   sctx->b.bind_tes_state = si_bind_tes_shader;
   sctx->b.bind_gs_state = si_bind_gs_shader;
   sctx->b.bind_fs_state = si_bind_ps_shader;

   sctx->b.delete_vs_state = si_delete_shader_selector;
   sctx->b.delete_tcs_state = si_delete_shader_selector;
   sctx->b.delete_tes_state = si_delete_shader_selector;
   sctx->b.delete_gs_state = si_delete_shader_selector;
   sctx->b.delete_fs_state = si_delete_shader_selector;

   sctx->b.set_patch_vertices = si_set_patch_vertices;

   /* This unrolls the loops in si_emit_spi_map and inlines memcmp and memcpys.
    * It improves performance for viewperf/snx.
    */
   sctx->emit_spi_map[0] = si_emit_spi_map<0>;
   sctx->emit_spi_map[1] = si_emit_spi_map<1>;
   sctx->emit_spi_map[2] = si_emit_spi_map<2>;
   sctx->emit_spi_map[3] = si_emit_spi_map<3>;
   sctx->emit_spi_map[4] = si_emit_spi_map<4>;
   sctx->emit_spi_map[5] = si_emit_spi_map<5>;
   sctx->emit_spi_map[6] = si_emit_spi_map<6>;
   sctx->emit_spi_map[7] = si_emit_spi_map<7>;
   sctx->emit_spi_map[8] = si_emit_spi_map<8>;
   sctx->emit_spi_map[9] = si_emit_spi_map<9>;
   sctx->emit_spi_map[10] = si_emit_spi_map<10>;
   sctx->emit_spi_map[11] = si_emit_spi_map<11>;
   sctx->emit_spi_map[12] = si_emit_spi_map<12>;
   sctx->emit_spi_map[13] = si_emit_spi_map<13>;
   sctx->emit_spi_map[14] = si_emit_spi_map<14>;
   sctx->emit_spi_map[15] = si_emit_spi_map<15>;
   sctx->emit_spi_map[16] = si_emit_spi_map<16>;
   sctx->emit_spi_map[17] = si_emit_spi_map<17>;
   sctx->emit_spi_map[18] = si_emit_spi_map<18>;
   sctx->emit_spi_map[19] = si_emit_spi_map<19>;
   sctx->emit_spi_map[20] = si_emit_spi_map<20>;
   sctx->emit_spi_map[21] = si_emit_spi_map<21>;
   sctx->emit_spi_map[22] = si_emit_spi_map<22>;
   sctx->emit_spi_map[23] = si_emit_spi_map<23>;
   sctx->emit_spi_map[24] = si_emit_spi_map<24>;
   sctx->emit_spi_map[25] = si_emit_spi_map<25>;
   sctx->emit_spi_map[26] = si_emit_spi_map<26>;
   sctx->emit_spi_map[27] = si_emit_spi_map<27>;
   sctx->emit_spi_map[28] = si_emit_spi_map<28>;
   sctx->emit_spi_map[29] = si_emit_spi_map<29>;
   sctx->emit_spi_map[30] = si_emit_spi_map<30>;
   sctx->emit_spi_map[31] = si_emit_spi_map<31>;
   sctx->emit_spi_map[32] = si_emit_spi_map<32>;
}
