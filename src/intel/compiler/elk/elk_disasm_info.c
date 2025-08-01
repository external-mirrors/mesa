/*
 * Copyright © 2014 Intel Corporation
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

#include "elk_cfg.h"
#include "elk_eu.h"
#include "elk_disasm.h"
#include "elk_disasm_info.h"
#include "dev/intel_debug.h"
#include "compiler/nir/nir.h"

void
elk_dump_assembly(void *assembly, int start_offset, int end_offset,
              struct elk_disasm_info *disasm, const unsigned *block_latency)
{
   const struct elk_isa_info *isa = disasm->isa;
   const char *last_annotation_string = NULL;
   const void *last_annotation_ir = NULL;

   void *mem_ctx = ralloc_context(NULL);
   const struct elk_label *root_label =
      elk_label_assembly(isa, assembly, start_offset, end_offset, mem_ctx);

   brw_foreach_list_typed(struct inst_group, group, link, &disasm->group_list) {
      struct brw_exec_node *next_node = brw_exec_node_get_next(&group->link);
      if (brw_exec_node_is_tail_sentinel(next_node))
         break;

      struct inst_group *next =
         brw_exec_node_data(struct inst_group, next_node, link);

      int start_offset = group->offset;
      int end_offset = next->offset;

      if (group->block_start) {
         fprintf(stderr, "   START B%d", group->block_start->num);
         brw_foreach_list_typed(struct elk_bblock_link, predecessor_link, link,
                            &group->block_start->parents) {
            struct elk_bblock_t *predecessor_block = predecessor_link->block;
            fprintf(stderr, " <-B%d", predecessor_block->num);
         }
         if (block_latency)
            fprintf(stderr, " (%u cycles)",
                    block_latency[group->block_start->num]);
         fprintf(stderr, "\n");
      }

      if (last_annotation_ir != group->ir) {
         last_annotation_ir = group->ir;
         if (last_annotation_ir) {
            fprintf(stderr, "   ");
            nir_print_instr(group->ir, stderr);
            fprintf(stderr, "\n");
         }
      }

      if (last_annotation_string != group->annotation) {
         last_annotation_string = group->annotation;
         if (last_annotation_string)
            fprintf(stderr, "   %s\n", last_annotation_string);
      }

      elk_disassemble(isa, assembly, start_offset, end_offset,
                      root_label, stderr);

      if (group->error) {
         fputs(group->error, stderr);
      }

      if (group->block_end) {
         fprintf(stderr, "   END B%d", group->block_end->num);
         brw_foreach_list_typed(struct elk_bblock_link, successor_link, link,
                            &group->block_end->children) {
            struct elk_bblock_t *successor_block = successor_link->block;
            fprintf(stderr, " ->B%d", successor_block->num);
         }
         fprintf(stderr, "\n");
      }
   }
   fprintf(stderr, "\n");

   ralloc_free(mem_ctx);
}

struct elk_disasm_info *
elk_disasm_initialize(const struct elk_isa_info *isa,
                  const struct elk_cfg_t *cfg)
{
   struct elk_disasm_info *disasm = ralloc(NULL, struct elk_disasm_info);
   brw_exec_list_make_empty(&disasm->group_list);
   disasm->isa = isa;
   disasm->cfg = cfg;
   disasm->cur_block = 0;
   disasm->use_tail = false;
   return disasm;
}

struct inst_group *
elk_disasm_new_inst_group(struct elk_disasm_info *disasm, unsigned next_inst_offset)
{
   struct inst_group *tail = rzalloc(disasm, struct inst_group);
   tail->offset = next_inst_offset;
   brw_exec_list_push_tail(&disasm->group_list, &tail->link);
   return tail;
}

void
elk_disasm_annotate(struct elk_disasm_info *disasm,
                struct elk_backend_instruction *inst, unsigned offset)
{
   const struct intel_device_info *devinfo = disasm->isa->devinfo;
   const struct elk_cfg_t *cfg = disasm->cfg;

   struct inst_group *group;
   if (!disasm->use_tail) {
      group = elk_disasm_new_inst_group(disasm, offset);
   } else {
      disasm->use_tail = false;
      group = brw_exec_node_data(struct inst_group,
                             brw_exec_list_get_tail_raw(&disasm->group_list), link);
   }

   if (INTEL_DEBUG(DEBUG_ANNOTATION)) {
      group->ir = inst->ir;
      group->annotation = inst->annotation;
   }

   if (bblock_start(cfg->blocks[disasm->cur_block]) == inst) {
      group->block_start = cfg->blocks[disasm->cur_block];
   }

   /* There is no hardware DO instruction on Gfx6+, so since DO always
    * starts a basic block, we need to set the .block_start of the next
    * instruction's annotation with a pointer to the bblock started by
    * the DO.
    *
    * There's also only complication from emitting an annotation without
    * a corresponding hardware instruction to disassemble.
    */
   if (devinfo->ver >= 6 && inst->opcode == ELK_OPCODE_DO) {
      disasm->use_tail = true;
   }

   if (bblock_end(cfg->blocks[disasm->cur_block]) == inst) {
      group->block_end = cfg->blocks[disasm->cur_block];
      disasm->cur_block++;
   }
}

void
elk_disasm_insert_error(struct elk_disasm_info *disasm, unsigned offset,
                    unsigned inst_size, const char *error)
{
   brw_foreach_list_typed(struct inst_group, cur, link, &disasm->group_list) {
      struct brw_exec_node *next_node = brw_exec_node_get_next(&cur->link);
      if (brw_exec_node_is_tail_sentinel(next_node))
         break;

      struct inst_group *next =
         brw_exec_node_data(struct inst_group, next_node, link);

      if (next->offset <= offset)
         continue;

      if (offset + inst_size != next->offset) {
         struct inst_group *new = ralloc(disasm, struct inst_group);
         memcpy(new, cur, sizeof(struct inst_group));

         cur->error = NULL;
         cur->error_length = 0;
         cur->block_end = NULL;

         new->offset = offset + inst_size;
         new->block_start = NULL;

         brw_exec_node_insert_after(&cur->link, &new->link);
      }

      if (cur->error)
         ralloc_strcat(&cur->error, error);
      else
         cur->error = ralloc_strdup(disasm, error);
      return;
   }
}
