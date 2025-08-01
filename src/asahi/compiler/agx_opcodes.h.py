template = """/*
 * Copyright 2021 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#ifndef _AGX_OPCODES_
#define _AGX_OPCODES_

#include <stdbool.h>
#include <stdint.h>
#include "util/macros.h"

enum agx_schedule_class {
   AGX_SCHEDULE_CLASS_INVALID,
   AGX_SCHEDULE_CLASS_NONE,
   AGX_SCHEDULE_CLASS_LOAD,
   AGX_SCHEDULE_CLASS_STORE,
   AGX_SCHEDULE_CLASS_ATOMIC,
   AGX_SCHEDULE_CLASS_COVERAGE,
   AGX_SCHEDULE_CLASS_PRELOAD,
   AGX_SCHEDULE_CLASS_BARRIER,
};

/* Listing of opcodes */

enum agx_opcode {
% for op in opcodes:
   AGX_OPCODE_${op.upper()},
% endfor
   AGX_NUM_OPCODES
};

% for name in enums:
enum agx_${name} {
% for k, v in enums[name].items():
   AGX_${name.upper()}_${v.replace('.', '_').upper()} = ${k},
% endfor
};

static inline const char *
agx_${name}_as_str(enum agx_${name} x)
{
    switch (x) {
% for k, v in enums[name].items():
    case AGX_${name.upper()}_${v.replace('.', '_').upper()}: return "${v}";
% endfor
    default: UNREACHABLE("Nonexhaustive enum");
    }
}

% endfor

/* Runtime accessible info on each defined opcode */

<% assert(len(immediates) < 64); %>

enum agx_immediate {
% for i, imm in enumerate(immediates):
   AGX_IMMEDIATE_${imm.upper()} = (1ull << ${i}),
% endfor
};

struct agx_encoding {
   uint64_t exact;
   unsigned length_short : 4;
   bool extensible : 1;
};

struct agx_opcode_info {
   const char *name;
   unsigned nr_srcs;
   unsigned nr_dests;
   uint64_t immediates;
   struct agx_encoding encoding;
   enum agx_schedule_class schedule_class;
   bool is_float : 1;
   bool can_eliminate : 1;
   bool can_reorder : 1;
};

extern const struct agx_opcode_info agx_opcodes_info[AGX_NUM_OPCODES];

#endif
"""

from mako.template import Template
from agx_opcodes import opcodes, immediates, enums

print(Template(template).render(opcodes=opcodes, immediates=immediates,
         enums=enums))
