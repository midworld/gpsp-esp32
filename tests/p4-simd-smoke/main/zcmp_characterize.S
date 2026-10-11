/* Record Zcmp stack slots and restored registers without assuming their order.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +zcmp
    .text

    .macro snippet_begin name
    .balign 4
    .global zcmp_char_\name\()_start
    .global zcmp_char_\name\()_end
    .type zcmp_char_\name\()_start, @function
zcmp_char_\name\()_start:
    .endm

    .macro snippet_end name
zcmp_char_\name\()_end:
    .size zcmp_char_\name\()_start, .-zcmp_char_\name\()_start
    .endm

    .macro push_tail name, frame_size
    sw sp, 236(a0)
    li t0, (\frame_size / 4)
    mv t1, sp
    addi t2, a0, 112
1:
    lw t3, 0(t1)
    sw t3, 0(t2)
    addi t1, t1, 4
    addi t2, t2, 4
    addi t0, t0, -1
    bnez t0, 1b
    addi sp, sp, \frame_size
    ret
    snippet_end \name
    .endm

    .macro pop_setup frame_size
    addi sp, sp, -\frame_size
    li t0, (\frame_size / 4)
    mv t1, a0
    mv t2, sp
1:
    lw t3, 0(t1)
    sw t3, 0(t2)
    addi t1, t1, 4
    addi t2, t2, 4
    addi t0, t0, -1
    bnez t0, 1b
    .endm

    .macro pop_tail name
    sw ra, 240(a0)
    lw ra, 224(a0)
    ret
    snippet_end \name
    .endm

    snippet_begin push_ra16
    cm.push {ra}, -16
    push_tail push_ra16, 16
    snippet_begin pop_ra16
    pop_setup 16
    cm.pop {ra}, 16
    pop_tail pop_ra16

    snippet_begin push_ra32
    cm.push {ra}, -32
    push_tail push_ra32, 32
    snippet_begin pop_ra32
    pop_setup 32
    cm.pop {ra}, 32
    pop_tail pop_ra32

    snippet_begin push_ra64
    cm.push {ra}, -64
    push_tail push_ra64, 64
    snippet_begin pop_ra64
    pop_setup 64
    cm.pop {ra}, 64
    pop_tail pop_ra64

    snippet_begin push_s0_16
    cm.push {ra,s0}, -16
    push_tail push_s0_16, 16
    snippet_begin pop_s0_16
    pop_setup 16
    cm.pop {ra,s0}, 16
    pop_tail pop_s0_16

    snippet_begin push_s0_32
    cm.push {ra,s0}, -32
    push_tail push_s0_32, 32
    snippet_begin pop_s0_32
    pop_setup 32
    cm.pop {ra,s0}, 32
    pop_tail pop_s0_32

    snippet_begin push_s0_64
    cm.push {ra,s0}, -64
    push_tail push_s0_64, 64
    snippet_begin pop_s0_64
    pop_setup 64
    cm.pop {ra,s0}, 64
    pop_tail pop_s0_64

    snippet_begin push_s1_16
    cm.push {ra,s0-s1}, -16
    push_tail push_s1_16, 16
    snippet_begin pop_s1_16
    pop_setup 16
    cm.pop {ra,s0-s1}, 16
    pop_tail pop_s1_16

    snippet_begin push_s1_64
    cm.push {ra,s0-s1}, -64
    push_tail push_s1_64, 64
    snippet_begin pop_s1_64
    pop_setup 64
    cm.pop {ra,s0-s1}, 64
    pop_tail pop_s1_64

    snippet_begin push_s2_16
    cm.push {ra,s0-s2}, -16
    push_tail push_s2_16, 16
    snippet_begin pop_s2_16
    pop_setup 16
    cm.pop {ra,s0-s2}, 16
    pop_tail pop_s2_16

    snippet_begin push_s2_64
    cm.push {ra,s0-s2}, -64
    push_tail push_s2_64, 64
    snippet_begin pop_s2_64
    pop_setup 64
    cm.pop {ra,s0-s2}, 64
    pop_tail pop_s2_64

    snippet_begin push_s3_32
    cm.push {ra,s0-s3}, -32
    push_tail push_s3_32, 32
    snippet_begin pop_s3_32
    pop_setup 32
    cm.pop {ra,s0-s3}, 32
    pop_tail pop_s3_32

    snippet_begin push_s3_48
    cm.push {ra,s0-s3}, -48
    push_tail push_s3_48, 48
    snippet_begin pop_s3_48
    pop_setup 48
    cm.pop {ra,s0-s3}, 48
    pop_tail pop_s3_48

    .macro range_pair name, last, frame_size
    snippet_begin push_\name
    cm.push {ra,s0-\last}, -\frame_size
    push_tail push_\name, \frame_size
    snippet_begin pop_\name
    pop_setup \frame_size
    cm.pop {ra,s0-\last}, \frame_size
    pop_tail pop_\name
    .endm

    range_pair s3_80, s3, 80
    range_pair s4_32, s4, 32
    range_pair s4_80, s4, 80
    range_pair s5_32, s5, 32
    range_pair s5_80, s5, 80
    range_pair s6_32, s6, 32
    range_pair s6_80, s6, 80
    range_pair s7_48, s7, 48
    range_pair s7_96, s7, 96
    range_pair s8_48, s8, 48
    range_pair s8_96, s8, 96
    range_pair s9_48, s9, 48
    range_pair s9_96, s9, 96

    snippet_begin push_s11_64
    cm.push {ra,s0-s11}, -64
    push_tail push_s11_64, 64
    snippet_begin pop_s11_64
    pop_setup 64
    cm.pop {ra,s0-s11}, 64
    pop_tail pop_s11_64

    snippet_begin push_s11_112
    cm.push {ra,s0-s11}, -112
    push_tail push_s11_112, 112
    snippet_begin pop_s11_112
    pop_setup 112
    cm.pop {ra,s0-s11}, 112
    pop_tail pop_s11_112

    .macro clobber_saved count
    .if \count >= 1
    mv s0, zero
    .endif
    .if \count >= 2
    mv s1, zero
    .endif
    .if \count >= 3
    mv s2, zero
    .endif
    .if \count >= 4
    mv s3, zero
    .endif
    .if \count >= 5
    mv s4, zero
    .endif
    .if \count >= 6
    mv s5, zero
    .endif
    .if \count >= 7
    mv s6, zero
    .endif
    .if \count >= 8
    mv s7, zero
    .endif
    .if \count >= 9
    mv s8, zero
    .endif
    .if \count >= 10
    mv s9, zero
    .endif
    .if \count >= 11
    mv s10, zero
    .endif
    .if \count >= 12
    mv s11, zero
    .endif
    .endm

    .macro ret_pair_ra name, frame_size
    snippet_begin ret_\name
    cm.push {ra}, -\frame_size
    cm.popret {ra}, \frame_size
    snippet_end ret_\name
    snippet_begin retz_\name
    cm.push {ra}, -\frame_size
    cm.popretz {ra}, \frame_size
    snippet_end retz_\name
    snippet_begin loadret_\name
    mv t4, sp
    pop_setup \frame_size
    lw t0, 224(a0)
    sw t0, -4(t4)
    cm.popret {ra}, \frame_size
    snippet_end loadret_\name
    snippet_begin loadretz_\name
    mv t4, sp
    pop_setup \frame_size
    lw t0, 224(a0)
    sw t0, -4(t4)
    cm.popretz {ra}, \frame_size
    snippet_end loadretz_\name
    .endm

    .macro ret_pair_range name, last, count, frame_size
    snippet_begin ret_\name
    cm.push {ra,s0-\last}, -\frame_size
    clobber_saved \count
    cm.popret {ra,s0-\last}, \frame_size
    snippet_end ret_\name
    snippet_begin retz_\name
    cm.push {ra,s0-\last}, -\frame_size
    clobber_saved \count
    cm.popretz {ra,s0-\last}, \frame_size
    snippet_end retz_\name
    snippet_begin loadret_\name
    mv t4, sp
    pop_setup \frame_size
    lw t0, 224(a0)
    sw t0, -4(t4)
    cm.popret {ra,s0-\last}, \frame_size
    snippet_end loadret_\name
    snippet_begin loadretz_\name
    mv t4, sp
    pop_setup \frame_size
    lw t0, 224(a0)
    sw t0, -4(t4)
    cm.popretz {ra,s0-\last}, \frame_size
    snippet_end loadretz_\name
    .endm

    .macro ret_pair_s0 name, count, frame_size
    snippet_begin ret_\name
    cm.push {ra,s0}, -\frame_size
    clobber_saved \count
    cm.popret {ra,s0}, \frame_size
    snippet_end ret_\name
    snippet_begin retz_\name
    cm.push {ra,s0}, -\frame_size
    clobber_saved \count
    cm.popretz {ra,s0}, \frame_size
    snippet_end retz_\name
    snippet_begin loadret_\name
    mv t4, sp
    pop_setup \frame_size
    lw t0, 224(a0)
    sw t0, -4(t4)
    cm.popret {ra,s0}, \frame_size
    snippet_end loadret_\name
    snippet_begin loadretz_\name
    mv t4, sp
    pop_setup \frame_size
    lw t0, 224(a0)
    sw t0, -4(t4)
    cm.popretz {ra,s0}, \frame_size
    snippet_end loadretz_\name
    .endm

    ret_pair_ra ra_16, 16
    ret_pair_ra ra_32, 32
    ret_pair_ra ra_48, 48
    ret_pair_ra ra_64, 64
    ret_pair_s0 s0_16, 1, 16
    ret_pair_s0 s0_32, 1, 32
    ret_pair_s0 s0_48, 1, 48
    ret_pair_s0 s0_64, 1, 64
    ret_pair_range s1_16, s1, 2, 16
    ret_pair_range s1_32, s1, 2, 32
    ret_pair_range s1_48, s1, 2, 48
    ret_pair_range s1_64, s1, 2, 64
    ret_pair_range s2_16, s2, 3, 16
    ret_pair_range s2_32, s2, 3, 32
    ret_pair_range s2_48, s2, 3, 48
    ret_pair_range s2_64, s2, 3, 64
    ret_pair_range s3_32, s3, 4, 32
    ret_pair_range s3_48, s3, 4, 48
    ret_pair_range s3_64, s3, 4, 64
    ret_pair_range s3_80, s3, 4, 80
    ret_pair_range s4_32, s4, 5, 32
    ret_pair_range s4_48, s4, 5, 48
    ret_pair_range s4_64, s4, 5, 64
    ret_pair_range s4_80, s4, 5, 80
    ret_pair_range s5_32, s5, 6, 32
    ret_pair_range s5_48, s5, 6, 48
    ret_pair_range s5_64, s5, 6, 64
    ret_pair_range s5_80, s5, 6, 80
    ret_pair_range s6_32, s6, 7, 32
    ret_pair_range s6_48, s6, 7, 48
    ret_pair_range s6_64, s6, 7, 64
    ret_pair_range s6_80, s6, 7, 80
    ret_pair_range s7_48, s7, 8, 48
    ret_pair_range s7_64, s7, 8, 64
    ret_pair_range s7_80, s7, 8, 80
    ret_pair_range s7_96, s7, 8, 96
    ret_pair_range s8_48, s8, 9, 48
    ret_pair_range s8_64, s8, 9, 64
    ret_pair_range s8_80, s8, 9, 80
    ret_pair_range s8_96, s8, 9, 96
    ret_pair_range s9_48, s9, 10, 48
    ret_pair_range s9_64, s9, 10, 64
    ret_pair_range s9_80, s9, 10, 80
    ret_pair_range s9_96, s9, 10, 96
    ret_pair_range s11_64, s11, 12, 64
    ret_pair_range s11_80, s11, 12, 80
    ret_pair_range s11_96, s11, 12, 96
    ret_pair_range s11_112, s11, 12, 112

    snippet_begin fault_ret_ra
    li sp, 0
    .global zcmp_char_fault_ret_ra_op
zcmp_char_fault_ret_ra_op:
    cm.popret {ra}, 16
    snippet_end fault_ret_ra

    snippet_begin fault_retz_ra
    li sp, 0
    .global zcmp_char_fault_retz_ra_op
zcmp_char_fault_retz_ra_op:
    cm.popretz {ra}, 16
    snippet_end fault_retz_ra

    /* zcmp_char_result_t offsets are fixed by the C static assertions. */
    .balign 4
    .global zcmp_char_invoke
    .type zcmp_char_invoke, @function
zcmp_char_invoke:
    addi sp, sp, -64
    sw ra, 60(sp)
    sw s0, 56(sp)
    sw s1, 52(sp)
    sw s2, 48(sp)
    sw s3, 44(sp)
    sw s4, 40(sp)
    sw s5, 36(sp)
    sw s6, 32(sp)
    sw s7, 28(sp)
    sw s8, 24(sp)
    sw s9, 20(sp)
    sw s10, 16(sp)
    sw s11, 12(sp)
    sw a1, 0(sp)
    sw a0, 4(sp)
    li s0, 0x51500000
    li s1, 0x51500001
    li s2, 0x51500002
    li s3, 0x51500003
    li s4, 0x51500004
    li s5, 0x51500005
    li s6, 0x51500006
    li s7, 0x51500007
    li s8, 0x51500008
    li s9, 0x51500009
    li s10, 0x5150000a
    li s11, 0x5150000b
    la t0, .Lreturned
    sw t0, 224(a1)
    sw sp, 228(a1)
    mv a0, a1
    lw t0, 4(sp)
    jalr ra, 0(t0)
.Lreturned:
    lw t0, 0(sp)
    sw a0, 292(t0)
    sw sp, 232(t0)
    sw s0, 244(t0)
    sw s1, 248(t0)
    sw s2, 252(t0)
    sw s3, 256(t0)
    sw s4, 260(t0)
    sw s5, 264(t0)
    sw s6, 268(t0)
    sw s7, 272(t0)
    sw s8, 276(t0)
    sw s9, 280(t0)
    sw s10, 284(t0)
    sw s11, 288(t0)
    lw s11, 12(sp)
    lw s10, 16(sp)
    lw s9, 20(sp)
    lw s8, 24(sp)
    lw s7, 28(sp)
    lw s6, 32(sp)
    lw s5, 36(sp)
    lw s4, 40(sp)
    lw s3, 44(sp)
    lw s2, 48(sp)
    lw s1, 52(sp)
    lw s0, 56(sp)
    lw ra, 60(sp)
    addi sp, sp, 64
    ret
    .size zcmp_char_invoke, .-zcmp_char_invoke

    .option pop
