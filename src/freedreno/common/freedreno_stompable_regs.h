/*
 * Copyright © 2023 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#ifndef __FREEDRENO_STOMPABLE_REGS_H__
#define __FREEDRENO_STOMPABLE_REGS_H__

#include <stdint.h>

#include "fd6_hw.h"

/* In order to debug issues with usage of stale reg data we need to have
 * a list of regs which we allowed to stomp.
 * The regs we are NOT allowed to stomp are:
 * - Write protected;
 * - Written by kernel but are not write protected;
 * - Some regs that are not written by anyone but do affect the result.
 *
 * In addition, some regs are only emmitted during cmdbuf setup
 * so we have to have additional filter to get a reduced list of regs
 * stompable before each renderpass/blit.
 */

/* Stomping some regs is known to cause issues */
static inline bool
fd_reg_stomp_allowed(chip CHIP, uint16_t reg)
{
   switch (CHIP) {
   case A6XX: {
      switch (reg) {
      /* Faults in
       * dEQP-VK.renderpass.suballocation.formats.r5g6b5_unorm_pack16.clear.clear
       * It seems that PC_CCU_FLUSH_COLOR_TS reads REG_A6XX_RB_DEPTH_PLANE_CNTL.
       */
      case REG_A6XX_RB_DEPTH_PLANE_CNTL:
      /* Faults in
       * dEQP-VK.conditional_rendering.draw.condition_host_memory_expect_noop.draw
       */
      case REG_A6XX_SP_VS_CONST_CONFIG ... REG_A6XX_SP_GS_CONST_CONFIG:
      case REG_A6XX_SP_PS_CONST_CONFIG:
      /* Faults in
       * dEQP-VK.memory_model.message_passing.ext.u32.coherent.atomic_atomic.atomicrmw.device.payload_local.image.guard_local.image.comp
       * while there is even no fragment shaders.
       */
      case REG_A6XX_SP_PS_BASE ... REG_A6XX_SP_PS_BASE + 1:
         return false;
      /* Not used on A6XX but causes failures when set */
      case REG_A6XX_TPL1_DBG_ECO_CNTL1:
         return false;
      }
      break;
   }
   case A7XX: {
      switch (reg) {
      case REG_A6XX_RB_DEPTH_PLANE_CNTL:
      case REG_A7XX_SP_VS_CONST_CONFIG:
      case REG_A7XX_SP_HS_CONST_CONFIG:
      case REG_A7XX_SP_DS_CONST_CONFIG:
      case REG_A7XX_SP_GS_CONST_CONFIG:
      case REG_A7XX_SP_PS_CONST_CONFIG:
      case REG_A6XX_SP_VS_BASE ... REG_A6XX_SP_VS_BASE + 1:
      case REG_A6XX_SP_PS_BASE ... REG_A6XX_SP_PS_BASE + 1:
      /* There is a guess that GPU may not be able to handle different values of
       * certain debug register between BR/BV. This one causes GPU to hang.
       */
      case REG_A7XX_SP_UNKNOWN_AE73:
      case REG_A7XX_RB_UNKNOWN_8E79:
      case REG_A7XX_SP_CHICKEN_BITS_2:
      case REG_A6XX_TPL1_DBG_ECO_CNTL:
         return false;
      case REG_A7XX_SP_GS_VGS_CNTL:
      case REG_A7XX_SP_PS_VGS_CNTL:
      case REG_A7XX_SP_CS_VGS_CNTL:
         return false;
      }
      break;
   }
   default: {
      UNREACHABLE("Unknown GPU");
   }
   }

   return true;
}

#endif /* __FREEDRENO_STOMPABLE_REGS_H__ */
