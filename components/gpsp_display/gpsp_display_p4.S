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

    /* Per channel: round(3*c/4) = c - (c>>2) - ((c & (c>>1)) & 1). */
    .macro shade output, input, quarter
    esp.vsrd.16 \quarter, \input, q7
    esp.andq \quarter, \quarter, q4
    esp.vsrd.16 q3, \input, q6
    esp.andq q3, q3, \input
    esp.andq q3, q3, q5
    esp.vsub.u16 \output, \input, \quarter
    esp.vsub.u16 \output, \output, q3
    .endm

    /* Shade and replicate without spilling intermediate pixel rows.
       q5/q6/q7 keep constants; interleaving overwrites only q4's mask. */
    .macro grid_loop label, first
\label:
    esp.vld.128.ip q0, a3, 16
    esp.vld.128.ip q4, a5, 0
    shade q1, q0, q1
    shade q2, q1, q2
    esp.orq q3, q0, q0
    esp.orq q4, q1, q1
    .if \first
    esp.vzipt.16 q4, q0, q3
    .else
    esp.vzipt.16 q0, q3, q4
    .endif
    .if \first
    esp.vst.128.ip q4, a0, 16
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q3, a0, 16
    esp.vst.128.ip q4, a1, 16
    esp.vst.128.ip q0, a1, 16
    esp.vst.128.ip q3, a1, 16
    esp.orq q0, q1, q1
    esp.vzipt.16 q2, q1, q0
    esp.vst.128.ip q2, a2, 16
    esp.vst.128.ip q1, a2, 16
    esp.vst.128.ip q0, a2, 16
    .else
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q3, a0, 16
    esp.vst.128.ip q4, a0, 16
    esp.vst.128.ip q0, a1, 16
    esp.vst.128.ip q3, a1, 16
    esp.vst.128.ip q4, a1, 16
    esp.orq q0, q1, q1
    esp.vzipt.16 q1, q0, q2
    esp.vst.128.ip q1, a2, 16
    esp.vst.128.ip q0, a2, 16
    esp.vst.128.ip q2, a2, 16
    .endif
    addi a4, a4, -1
    bnez a4, \label
    ret
    .endm

    .balign 4
    .global gpsp_display_grid3_p4
    .type gpsp_display_grid3_p4, @function
gpsp_display_grid3_p4:
    beqz a4, 1f
    mv a6, a5
    la a5, .Lshade_constants
    addi a5, a5, 16
    esp.vld.128.ip q5, a5, 16
    esp.vld.128.ip q6, a5, 16
    esp.vld.128.ip q7, a5, 16
    addi a5, a5, -64
    bnez a6, .Lgrid_first
    grid_loop .Lgrid_last, 0
    grid_loop .Lgrid_first, 1
1:
    ret
    .size gpsp_display_grid3_p4, .-gpsp_display_grid3_p4

    /* Four RGB565 columns; either pitch can be negative. */
    .balign 4
    .global gpsp_display_transpose4_p4
    .type gpsp_display_transpose4_p4, @function
gpsp_display_transpose4_p4:
    esp.vld.l.64.xp q0, a1, a2
    esp.vld.l.64.xp q1, a1, a2
    esp.vld.l.64.xp q2, a1, a2
    esp.vld.l.64.xp q3, a1, a2
    esp.vzip.16 q0, q1
    esp.vzip.16 q2, q3
    esp.vzip.32 q0, q2
    esp.vst.l.64.xp q0, a0, a3
    esp.vst.h.64.xp q0, a0, a3
    esp.vst.l.64.xp q2, a0, a3
    esp.vst.h.64.xp q2, a0, a3
    ret
    .size gpsp_display_transpose4_p4, .-gpsp_display_transpose4_p4

    .section .rodata
    .balign 16
.Lshade_constants:
    .rept 8
    .half 0x39e7
    .endr
    .rept 8
    .half 0x0821
    .endr
    .rept 8
    .half 1
    .endr
    .rept 8
    .half 2
    .endr

    .option pop
