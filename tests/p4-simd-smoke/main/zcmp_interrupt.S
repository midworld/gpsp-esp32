/* Exercise Zcmp returns while a timer interrupt samples the instruction PC.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +zcmp
    .text

    .balign 4
    .global zcmp_irq_ret_start
    .global zcmp_irq_ret_op
    .global zcmp_irq_ret_end
    .type zcmp_irq_ret_start, @function
zcmp_irq_ret_start:
    addi sp, sp, -16
    sw ra, 12(sp)
zcmp_irq_ret_op:
    cm.popret {ra}, 16
zcmp_irq_ret_end:
    .size zcmp_irq_ret_start, .-zcmp_irq_ret_start

    .balign 4
    .global zcmp_irq_retz_start
    .global zcmp_irq_retz_op
    .global zcmp_irq_retz_end
    .type zcmp_irq_retz_start, @function
zcmp_irq_retz_start:
    addi sp, sp, -16
    sw ra, 12(sp)
zcmp_irq_retz_op:
    cm.popretz {ra}, 16
zcmp_irq_retz_end:
    .size zcmp_irq_retz_start, .-zcmp_irq_retz_start

    /* Match the observed P4 layout without executing cm.push. */
    .macro save_full_frame
    addi sp, sp, -112
    sw s11, 60(sp)
    sw s10, 64(sp)
    sw s9, 68(sp)
    sw s8, 72(sp)
    sw s7, 76(sp)
    sw s6, 80(sp)
    sw s5, 84(sp)
    sw s4, 88(sp)
    sw s3, 92(sp)
    sw s2, 96(sp)
    sw s1, 100(sp)
    sw s0, 104(sp)
    sw ra, 108(sp)
    .endm

    .balign 4
    .global zcmp_irq_ret_full_start
    .global zcmp_irq_ret_full_op
    .global zcmp_irq_ret_full_end
    .type zcmp_irq_ret_full_start, @function
zcmp_irq_ret_full_start:
    save_full_frame
zcmp_irq_ret_full_op:
    cm.popret {ra,s0-s11}, 112
zcmp_irq_ret_full_end:
    .size zcmp_irq_ret_full_start, .-zcmp_irq_ret_full_start

    .balign 4
    .global zcmp_irq_retz_full_start
    .global zcmp_irq_retz_full_op
    .global zcmp_irq_retz_full_end
    .type zcmp_irq_retz_full_start, @function
zcmp_irq_retz_full_start:
    save_full_frame
zcmp_irq_retz_full_op:
    cm.popretz {ra,s0-s11}, 112
zcmp_irq_retz_full_end:
    .size zcmp_irq_retz_full_start, .-zcmp_irq_retz_full_start

    .option pop
