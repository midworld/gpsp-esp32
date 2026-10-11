/* Each snippet contains one Zcmp instruction and is copied to executable RAM.
 * zcmp_invoke remains linked code; it seeds and records the registers around it.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +zcmp

    .section .rodata.zcmp_probe, "a", @progbits
    .balign 4

    .macro snippet_begin name
    .global zcmp_\name\()_start
zcmp_\name\()_start:
    .endm

    .macro snippet_end name
    .global zcmp_\name\()_end
zcmp_\name\()_end:
    .balign 4
    .endm

    /* Save ra/s0/s1 with cm.push. C checks their frame words and restored sp. */
    snippet_begin push
    cm.push {ra,s0-s1}, -16
    lw t0, 4(sp)
    sw t0, 24(a0)
    lw t0, 8(sp)
    sw t0, 28(a0)
    lw t0, 12(sp)
    sw t0, 32(a0)
    addi sp, sp, 16
    ret
    snippet_end push

    /* Build the frame with ordinary stores, clear s0/s1, then check the pop
     * form restores them. The spec puts ra at the lowest slot; the _rev form
     * swaps ra and s1 to detect the reversed layout.
     */
    .macro pop_snippet name, op, ra_slot, s1_slot
    snippet_begin \name
    addi sp, sp, -16
    sw ra, \ra_slot(sp)
    sw s0, 8(sp)
    sw s1, \s1_slot(sp)
    li s0, 0
    li s1, 0
    cm.\op {ra,s0-s1}, 16
    .ifc \op,pop
    ret
    .endif
    snippet_end \name
    .endm

    pop_snippet pop, pop, 4, 12
    pop_snippet pop_rev, pop, 12, 4
    pop_snippet popret, popret, 4, 12
    pop_snippet popret_rev, popret, 12, 4
    pop_snippet popretz, popretz, 4, 12
    pop_snippet popretz_rev, popretz, 12, 4

    /* A matched push/pop checks the instructions without assuming slot order. */
    snippet_begin roundtrip_pop
    cm.push {ra,s0-s1}, -16
    li s0, 0
    li s1, 0
    cm.pop {ra,s0-s1}, 16
    ret
    snippet_end roundtrip_pop

    snippet_begin roundtrip_popret
    cm.push {ra,s0-s1}, -16
    li s0, 0
    li s1, 0
    cm.popret {ra,s0-s1}, 16
    snippet_end roundtrip_popret

    snippet_begin roundtrip_popretz
    cm.push {ra,s0-s1}, -16
    li s0, 0
    li s1, 0
    cm.popretz {ra,s0-s1}, 16
    snippet_end roundtrip_popretz

    /* Wrapper seeds s0/s1; C expects those values in a0/a1 after the move. */
    snippet_begin mva01s
    cm.mva01s s0,s1
    ret
    snippet_end mva01s

    /* Move input a0/a1 into s0/s1, expose both values, then preserve caller s registers. */
    snippet_begin mvsa01
    addi sp, sp, -16
    sw s0, 8(sp)
    sw s1, 12(sp)
    cm.mvsa01 s0,s1
    mv a0, s0
    mv a1, s1
    lw s0, 8(sp)
    lw s1, 12(sp)
    addi sp, sp, 16
    ret
    snippet_end mvsa01

    /* zcmp_result_t offsets: a0/a1/s0/s1/sp_delta at 0..16, expected_ra at 20.
     * The wrapper restores the C ABI registers after observing the snippet.
     */
    .section .text.zcmp_invoke, "ax", @progbits
    .balign 4
    .global zcmp_invoke
    .type zcmp_invoke, @function
zcmp_invoke:
    addi sp, sp, -32
    sw ra, 28(sp)
    sw s0, 24(sp)
    sw s1, 20(sp)
    sw s2, 16(sp)
    sw s3, 12(sp)
    sw s4, 8(sp)
    mv s2, a1
    mv s3, sp
    mv s4, a0
    li s0, 0x13579bdf
    li s1, 0x2468ace0
    mv a0, a2
    mv a1, a3
    la t0, .Lreturned
    sw t0, 20(s2)
    jalr ra, 0(s4)
.Lreturned:
    sw a0, 0(s2)
    sw a1, 4(s2)
    sw s0, 8(s2)
    sw s1, 12(s2)
    sub t0, sp, s3
    sw t0, 16(s2)
    lw s4, 8(sp)
    lw s3, 12(sp)
    lw s2, 16(sp)
    lw s1, 20(sp)
    lw s0, 24(sp)
    lw ra, 28(sp)
    addi sp, sp, 32
    ret
    .size zcmp_invoke, .-zcmp_invoke

    .option pop
