/* Fixed-alpha black background for RGB565 statistics bars.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +xespv
    .text
    .balign 4
    .global gpsp_dsi_hud_quarter_p4
    .type gpsp_dsi_hud_quarter_p4, @function
gpsp_dsi_hud_quarter_p4:
    beqz a1, 2f
    la a2, .Lhud_constants
    esp.vld.128.ip q1, a2, 16
    esp.vld.128.ip q2, a2, 16
1:
    esp.vld.128.ip q0, a0, 0
    esp.vsrd.16 q0, q0, q2
    esp.andq q0, q0, q1
    esp.vst.128.ip q0, a0, 16
    addi a1, a1, -1
    bnez a1, 1b
2:
    ret
    .size gpsp_dsi_hud_quarter_p4, .-gpsp_dsi_hud_quarter_p4

    .section .rodata
    .balign 16
.Lhud_constants:
    .short 0x39e7, 0x39e7, 0x39e7, 0x39e7, 0x39e7, 0x39e7, 0x39e7, 0x39e7
    .short 2, 2, 2, 2, 2, 2, 2, 2
    .option pop
