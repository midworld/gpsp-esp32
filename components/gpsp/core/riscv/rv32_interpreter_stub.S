/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_register_map.h"

  .text
  .option push
  .option norvc
  .option norelax
  .align 2
  .globl gpsp_rv32_interpreter_step_stub
  .type gpsp_rv32_interpreter_step_stub, @function
gpsp_rv32_interpreter_step_stub:
  /* ra still points to run_block's exit; the bridge returns there. */
  lla a0, gpsp_rv32_interpret_one_helper
  li a1, 0
  li a2, 0
  lla RV_TMP1, gpsp_rv32_call_helper
  jr RV_TMP1
  .size gpsp_rv32_interpreter_step_stub, .-gpsp_rv32_interpreter_step_stub
  .option pop
