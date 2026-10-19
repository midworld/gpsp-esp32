/* Aligned RGB565 row expansion with ESP32-P4 PIE.
 * Eight input pixels per group; Q registers are caller-saved.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +xespv
    .text

    .balign 4
    .global gpsp_display_nearest2_p4
    .type gpsp_display_nearest2_p4, @function
gpsp_display_nearest2_p4:
    add a4, a0, a3
    beqz a2, 2f
1:
    esp.vld.128.ip q0, a1, 16
    esp.orq q1, q0, q0
    esp.vzip.16 q0, q1
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
    esp.vst.128.ip q0, a4, 16
    esp.vst.128.ip q1, a4, 16
    addi a2, a2, -1
    bnez a2, 1b
2:
    ret
    .size gpsp_display_nearest2_p4, .-gpsp_display_nearest2_p4

    .balign 4
    .global gpsp_display_nearest3_p4
    .type gpsp_display_nearest3_p4, @function
gpsp_display_nearest3_p4:
    add a4, a0, a3
    add a5, a4, a3
    beqz a2, 2f
1:
    esp.vld.128.ip q0, a1, 16
    esp.orq q1, q0, q0
    esp.orq q2, q0, q0
    esp.vzipt.16 q0, q1, q2
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
    esp.vst.128.ip q2, a0, 16
    esp.vst.128.ip q0, a4, 16
    esp.vst.128.ip q1, a4, 16
    esp.vst.128.ip q2, a4, 16
    esp.vst.128.ip q0, a5, 16
    esp.vst.128.ip q1, a5, 16
    esp.vst.128.ip q2, a5, 16
    addi a2, a2, -1
    bnez a2, 1b
2:
    ret
    .size gpsp_display_nearest3_p4, .-gpsp_display_nearest3_p4

    /* a0/a1: bright rows, a2: dark row; a3/a4/a5: normal/dim/corner.
       VZIPT interleaves three vectors into consecutive output vectors. */
    .macro grid_loop label, first
\label:
    .if \first
    esp.vld.128.ip q1, a3, 16
    esp.vld.128.ip q0, a4, 16
    esp.vld.128.ip q3, a5, 16
    esp.orq q2, q1, q1
    esp.orq q4, q0, q0
    esp.orq q5, q0, q0
    .else
    esp.vld.128.ip q0, a3, 16
    esp.vld.128.ip q3, a4, 16
    esp.vld.128.ip q5, a5, 16
    esp.orq q1, q0, q0
    esp.orq q2, q3, q3
    esp.orq q4, q3, q3
    .endif
    esp.vzipt.16 q0, q1, q2
    esp.vzipt.16 q3, q4, q5
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
    esp.vst.128.ip q2, a0, 16
    esp.vst.128.ip q0, a1, 16
    esp.vst.128.ip q1, a1, 16
    esp.vst.128.ip q2, a1, 16
    esp.vst.128.ip q3, a2, 16
    esp.vst.128.ip q4, a2, 16
    esp.vst.128.ip q5, a2, 16
    addi a6, a6, -1
    bnez a6, \label
    ret
    .endm

    .balign 4
    .global gpsp_display_grid3_p4
    .type gpsp_display_grid3_p4, @function
gpsp_display_grid3_p4:
    beqz a6, 1f
    bnez a7, .Lgrid_first
    grid_loop .Lgrid_last, 0
    grid_loop .Lgrid_first, 1
1:
    ret
    .size gpsp_display_grid3_p4, .-gpsp_display_grid3_p4

    .option pop
