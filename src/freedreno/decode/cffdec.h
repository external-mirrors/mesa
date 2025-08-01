/*
 * Copyright © 2012 Rob Clark <robdclark@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef __CFFDEC_H__
#define __CFFDEC_H__

#include <stdbool.h>

#include "freedreno_pm4.h"
#include "freedreno_dev_info.h"

enum query_mode {
   /* default mode, dump all queried regs on each draw: */
   QUERY_ALL = 0,

   /* only dump if any of the queried regs were written
    * since last draw:
    */
   QUERY_WRITTEN,

   /* only dump if any of the queried regs changed since
    * last draw:
    */
   QUERY_DELTA,
};

struct cffdec_options {
   struct fd_dev_id dev_id;
   const struct fd_dev_info *info;
   int draw_filter;
   int color;
   int dump_shaders;
   int summary;
   int allregs;
   int dump_textures;
   int dump_bindless;
   int decode_markers;
   char *script;

   int query_compare; /* binning vs SYSMEM/GMEM compare mode */
   int query_mode;    /* enum query_mode */
   char **querystrs;
   int nquery;

   /* In "once" mode, only decode a cmdstream buffer once (per draw
    * mode, in the case of a6xx+ where a single cmdstream buffer can
    * be used for both binning and draw pass), rather than each time
    * encountered (ie. once per tile/bin in GMEM draw passes)
    */
   int once;

   /* In unit_test mode, suppress pathnames in output so that we can have references
    * independent of the build dir.
    */
   int unit_test;

   /* for crashdec, where we know CP_IBx_REM_SIZE, we can use this
    * to highlight the cmdstream not parsed yet, to make it easier
    * to see how far along the CP is.
    */
   struct {
      uint64_t base;
      uint32_t rem;
      uint32_t size;
      bool crash_found : 1;
   } ibs[4];

   /* Ringbuffer addresses are non-contiguous so we use the host address.
    */
   uint32_t *rb_host_base;
};

/**
 * A helper to deal with 64b registers by accumulating the lo/hi 32b
 * dwords.  Example usage:
 *
 *    struct regacc r = regacc(rnn);
 *
 *    for (dword in dwords) {
 *       if (regacc_push(&r, regbase, dword)) {
 *          printf("\t%08x"PRIx64", r.value);
 *          dump_register_val(r.regbase, r.value, 0);
 *       }
 *       regbase++;
 *    }
 *
 * It is expected that 64b regs will come in pairs of <lo, hi>.
 */
struct regacc {
   uint32_t regbase;
   uint64_t value;

   /* private: */
   struct rnn *rnn;
   bool has_dword_lo;
};
struct regacc regacc(struct rnn *rnn);
bool regacc_push(struct regacc *regacc, uint32_t regbase, uint32_t dword);

void printl(int lvl, const char *fmt, ...);
const char *pktname(unsigned opc);
uint32_t regbase(const char *name);
int enumval(const char *enumname, const char *enumval);
const char *regname(uint32_t regbase, int color);
bool reg_written(uint32_t regbase);
uint32_t reg_lastval(uint32_t regbase);
uint32_t reg_val(uint32_t regbase);
void reg_set(uint32_t regbase, uint32_t val);
uint32_t * parse_cp_indirect(uint32_t *dwords, uint32_t sizedwords,
                             uint64_t *ibaddr, uint32_t *ibsize);
void reset_regs(void);
void cffdec_init(const struct cffdec_options *options);
void dump_register_val(struct regacc *r, int level);
void dump_commands(uint32_t *dwords, uint32_t sizedwords, int level);

/*
 * Packets (mostly) fall into two categories, "write one or more registers"
 * (type0 or type4 depending on generation) or "packet with opcode and
 * opcode specific payload" (type3 or type7).  These helpers deal with
 * the type0+type3 vs type4+type7 differences (a2xx-a4xx vs a5xx+).
 */

static inline bool
pkt_is_regwrite(uint32_t dword, uint32_t *offset, uint32_t *size)
{
   if (pkt_is_type0(dword)) {
      *size = type0_pkt_size(dword) + 1;
      *offset = type0_pkt_offset(dword);
      return true;
   } if (pkt_is_type4(dword)) {
      *size = type4_pkt_size(dword) + 1;
      *offset = type4_pkt_offset(dword);
      return true;
   }
   return false;
}

static inline bool
pkt_is_opcode(uint32_t dword, uint32_t *opcode, uint32_t *size)
{
   if (pkt_is_type3(dword)) {
      *size = type3_pkt_size(dword) + 1;
      *opcode = cp_type3_opcode(dword);
      return true;
   } else if (pkt_is_type7(dword)) {
      *size = type7_pkt_size(dword) + 1;
      *opcode = cp_type7_opcode(dword);
     return true;
   }
   return false;
}

/**
 * For a5xx+ we can detect valid packet headers vs random other noise, and
 * can use this to "re-sync" to the start of the next valid packet.  So that
 * the same cmdstream corruption that confused the GPU doesn't confuse us!
 */
static inline uint32_t
find_next_packet(uint32_t *dwords, uint32_t sizedwords)
{
   for (uint32_t c = 0; c < sizedwords; c++) {
      if (pkt_is_type7(dwords[c]) || pkt_is_type4(dwords[c]))
         return c;
   }
   return sizedwords;
}


#endif /* __CFFDEC_H__ */
