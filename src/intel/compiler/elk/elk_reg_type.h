/*
 * Copyright © 2017 Intel Corporation
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

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_PURE
#define ATTRIBUTE_PURE __attribute__((__pure__))
#else
#define ATTRIBUTE_PURE
#endif

enum elk_reg_file;
struct intel_device_info;

/*
 * The ordering has been chosen so that no enum value is the same as a
 * compatible hardware encoding.
 */
enum PACKED elk_reg_type {
   /** Floating-point types: @{ */
   ELK_REGISTER_TYPE_NF, /* >64-bit (accumulator-only) native float (gfx11+) */
   ELK_REGISTER_TYPE_DF, /* 64-bit float (double float) */
   ELK_REGISTER_TYPE_F,  /* 32-bit float */
   ELK_REGISTER_TYPE_HF, /* 16-bit float (half float) */
   ELK_REGISTER_TYPE_VF, /* 32-bit vector of 4 8-bit floats */
   /** @} */

   /** Integer types: @{ */
   ELK_REGISTER_TYPE_Q,  /* 64-bit   signed integer (quad word) */
   ELK_REGISTER_TYPE_UQ, /* 64-bit unsigned integer (quad word) */
   ELK_REGISTER_TYPE_D,  /* 32-bit   signed integer (double word) */
   ELK_REGISTER_TYPE_UD, /* 32-bit unsigned integer (double word) */
   ELK_REGISTER_TYPE_W,  /* 16-bit   signed integer (word) */
   ELK_REGISTER_TYPE_UW, /* 16-bit unsigned integer (word) */
   ELK_REGISTER_TYPE_B,  /*  8-bit   signed integer (byte) */
   ELK_REGISTER_TYPE_UB, /*  8-bit unsigned integer (byte) */
   ELK_REGISTER_TYPE_V,  /* vector of 8   signed 4-bit integers (treated as W) */
   ELK_REGISTER_TYPE_UV, /* vector of 8 unsigned 4-bit integers (treated as UW) */
   /** @} */

   ELK_REGISTER_TYPE_LAST = ELK_REGISTER_TYPE_UV
};

static inline bool
elk_reg_type_is_floating_point(enum elk_reg_type type)
{
   switch (type) {
   case ELK_REGISTER_TYPE_NF:
   case ELK_REGISTER_TYPE_DF:
   case ELK_REGISTER_TYPE_F:
   case ELK_REGISTER_TYPE_HF:
      return true;
   default:
      return false;
   }
}

static inline bool
elk_reg_type_is_integer(enum elk_reg_type type)
{
   switch (type) {
   case ELK_REGISTER_TYPE_Q:
   case ELK_REGISTER_TYPE_UQ:
   case ELK_REGISTER_TYPE_D:
   case ELK_REGISTER_TYPE_UD:
   case ELK_REGISTER_TYPE_W:
   case ELK_REGISTER_TYPE_UW:
   case ELK_REGISTER_TYPE_B:
   case ELK_REGISTER_TYPE_UB:
      return true;
   default:
      return false;
   }
}

static inline bool
elk_reg_type_is_unsigned_integer(enum elk_reg_type tp)
{
   return tp == ELK_REGISTER_TYPE_UB ||
          tp == ELK_REGISTER_TYPE_UW ||
          tp == ELK_REGISTER_TYPE_UD ||
          tp == ELK_REGISTER_TYPE_UQ;
}

/*
 * Returns a type based on a reference_type (word, float, half-float) and a
 * given bit_size.
 */
static inline enum elk_reg_type
elk_reg_type_from_bit_size(unsigned bit_size,
                           enum elk_reg_type reference_type)
{
   switch(reference_type) {
   case ELK_REGISTER_TYPE_HF:
   case ELK_REGISTER_TYPE_F:
   case ELK_REGISTER_TYPE_DF:
      switch(bit_size) {
      case 16:
         return ELK_REGISTER_TYPE_HF;
      case 32:
         return ELK_REGISTER_TYPE_F;
      case 64:
         return ELK_REGISTER_TYPE_DF;
      default:
         UNREACHABLE("Invalid bit size");
      }
   case ELK_REGISTER_TYPE_B:
   case ELK_REGISTER_TYPE_W:
   case ELK_REGISTER_TYPE_D:
   case ELK_REGISTER_TYPE_Q:
      switch(bit_size) {
      case 8:
         return ELK_REGISTER_TYPE_B;
      case 16:
         return ELK_REGISTER_TYPE_W;
      case 32:
         return ELK_REGISTER_TYPE_D;
      case 64:
         return ELK_REGISTER_TYPE_Q;
      default:
         UNREACHABLE("Invalid bit size");
      }
   case ELK_REGISTER_TYPE_UB:
   case ELK_REGISTER_TYPE_UW:
   case ELK_REGISTER_TYPE_UD:
   case ELK_REGISTER_TYPE_UQ:
      switch(bit_size) {
      case 8:
         return ELK_REGISTER_TYPE_UB;
      case 16:
         return ELK_REGISTER_TYPE_UW;
      case 32:
         return ELK_REGISTER_TYPE_UD;
      case 64:
         return ELK_REGISTER_TYPE_UQ;
      default:
         UNREACHABLE("Invalid bit size");
      }
   default:
      UNREACHABLE("Unknown type");
   }
}


#define INVALID_REG_TYPE    ((enum elk_reg_type)-1)
#define INVALID_HW_REG_TYPE ((unsigned)-1)

unsigned
elk_reg_type_to_hw_type(const struct intel_device_info *devinfo,
                        enum elk_reg_file file, enum elk_reg_type type);

enum elk_reg_type ATTRIBUTE_PURE
elk_hw_type_to_reg_type(const struct intel_device_info *devinfo,
                        enum elk_reg_file file, unsigned hw_type);

unsigned
elk_reg_type_to_a16_hw_3src_type(const struct intel_device_info *devinfo,
                                 enum elk_reg_type type);

enum elk_reg_type
elk_a16_hw_3src_type_to_reg_type(const struct intel_device_info *devinfo,
                                 unsigned hw_type);

unsigned
elk_reg_type_to_size(enum elk_reg_type type);

const char *
elk_reg_type_to_letters(enum elk_reg_type type);

#ifdef __cplusplus
}
#endif
