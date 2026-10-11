/* RV32I/M/C snippets used by a generated-code emitter.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option norvc
    .section .rodata.core_probe, "a", @progbits
    .balign 4

    /* C supplies edge-case operands; each snippet returns a value in a0. */
    .macro snippet_begin name
    .global core_\name\()_start
core_\name\()_start:
    .endm

    .macro snippet_end name
    .global core_\name\()_end
core_\name\()_end:
    .balign 4
    .endm

    .macro binary name, op
    snippet_begin \name
    \op a0, a0, a1
    ret
    snippet_end \name
    .endm

    .macro immediate name, op, imm
    snippet_begin \name
    \op a0, a0, \imm
    ret
    snippet_end \name
    .endm

    .macro load name, op, offset
    snippet_begin \name
    \op a0, \offset(a1)
    ret
    snippet_end \name
    .endm

    .macro store name, op, offset
    snippet_begin \name
    \op a0, \offset(a1)
    ret
    snippet_end \name
    .endm

    .macro branch name, op
    /* C checks both paths: taken returns 1, fallthrough returns 0. */
    snippet_begin \name
    \op a0, a1, .Lbranch_taken\@
    addi a0, zero, 0
    ret
.Lbranch_taken\@:
    addi a0, zero, 1
    ret
    snippet_end \name
    .endm

    /* RV32I register and immediate arithmetic, including shift limits. */
    snippet_begin ram_baseline
    addi a0, a0, 1
    ret
    snippet_end ram_baseline

    binary add, add
    binary sub, sub
    binary sll, sll
    binary slt, slt
    binary sltu, sltu
    binary xor, xor
    binary srl, srl
    binary sra, sra
    binary or, or
    binary and, and
    immediate addi, addi, -2048
    immediate slti, slti, 0
    immediate sltiu, sltiu, 1
    immediate xori, xori, -1
    immediate ori, ori, -2048
    immediate andi, andi, 2047
    immediate slli, slli, 31
    immediate srli, srli, 31
    immediate srai, srai, 31
    /* C checks load sign extension and the word changed by each store. */
    load lb, lb, 0
    load lbu, lbu, 0
    load lh, lh, 0
    load lhu, lhu, 0
    load lw, lw, 0
    store sb, sb, 3
    store sh, sh, 2
    store sw, sw, 0
    /* Signed and unsigned comparisons use both taken and untaken inputs. */
    branch beq, beq
    branch bne, bne
    branch blt, blt
    branch bge, bge
    branch bltu, bltu
    branch bgeu, bgeu

    /* Upper immediate is fixed; C compares the full 32-bit result. */
    snippet_begin lui
    lui a0, 0x12345
    ret
    snippet_end lui

    /* Zero offset makes the returned value the copied instruction's PC. */
    snippet_begin auipc
    auipc a0, 0
    ret
    snippet_end auipc

    /* Return the link in t0; C computes its address in executable RAM. */
    snippet_begin jal
    jal t0, .Ljal_target
    .global core_jal_link
core_jal_link:
    addi a0, zero, 0
    ret
.Ljal_target:
    mv a0, t0
    ret
    snippet_end jal

    /* C passes an odd target in a1 to check JALR's low-bit clearing. */
    snippet_begin jalr
    jalr t0, 0(a1)
    .global core_jalr_link
core_jalr_link:
    addi a0, zero, 0
    ret
    .global core_jalr_target
core_jalr_target:
    mv a0, t0
    ret
    snippet_end jalr

    /* Fence is observable here as a successful return after execution. */
    snippet_begin fence
    fence iorw, iorw
    ret
    snippet_end fence

    /* The guard expects these synchronous exceptions at the first opcode. */
    snippet_begin ecall
    ecall
    ret
    snippet_end ecall

    snippet_begin ebreak
    ebreak
    ret
    snippet_end ebreak

    /* RV32M includes signed/unsigned high products and division edge cases. */
    binary mul, mul
    binary mulh, mulh
    binary mulhsu, mulhsu
    binary mulhu, mulhu
    binary div, div
    binary divu, divu
    binary rem, rem
    binary remu, remu

    .option rvc

    /* Compressed arithmetic, memory, jumps, and both branch outcomes. */
    snippet_begin cli
    c.li a0, -1
    ret
    snippet_end cli

    snippet_begin cmv
    c.mv a0, a1
    ret
    snippet_end cmv

    snippet_begin cadd
    c.add a0, a1
    ret
    snippet_end cadd

    snippet_begin csub
    c.sub a0, a1
    ret
    snippet_end csub

    snippet_begin cslli
    c.slli a0, 31
    ret
    snippet_end cslli

    snippet_begin csrai
    c.srai a0, 31
    ret
    snippet_end csrai

    snippet_begin clw
    c.lw a0, 0(a1)
    ret
    snippet_end clw

    snippet_begin csw
    c.sw a0, 0(a1)
    ret
    snippet_end csw

    snippet_begin cj
    c.j .Lcj_target
    c.li a0, 0
    ret
.Lcj_target:
    c.li a0, 1
    ret
    snippet_end cj

    snippet_begin cbeqz
    c.beqz a0, .Lcbeqz_target
    c.li a0, 0
    ret
.Lcbeqz_target:
    c.li a0, 1
    ret
    snippet_end cbeqz

    snippet_begin cbnez
    c.bnez a0, .Lcbnez_target
    c.li a0, 0
    ret
.Lcbnez_target:
    c.li a0, 1
    ret
    snippet_end cbnez

    /* Remaining RV32C integer encodings; one legal operand set each. */
    snippet_begin caddi4spn
    c.addi4spn a0, sp, 16
    sub a0, a0, sp
    ret
    snippet_end caddi4spn

    snippet_begin cnop
    c.nop
    c.li a0, 1
    ret
    snippet_end cnop

    snippet_begin caddi
    c.addi a0, -1
    ret
    snippet_end caddi

    snippet_begin cjal
    mv a2, ra
    c.jal .Lcjal_target
    .global core_cjal_link
core_cjal_link:
    c.li a0, 0
    mv ra, a2
    ret
.Lcjal_target:
    mv a0, ra
    mv ra, a2
    ret
    snippet_end cjal

    snippet_begin caddi16sp
    mv a0, sp
    c.addi16sp sp, 16
    sub a0, sp, a0
    addi sp, sp, -16
    ret
    snippet_end caddi16sp

    snippet_begin clui
    c.lui a0, 0x1f
    ret
    snippet_end clui

    snippet_begin csrli
    c.srli a0, 31
    ret
    snippet_end csrli

    snippet_begin candi
    c.andi a0, -2
    ret
    snippet_end candi

    snippet_begin cxor
    c.xor a0, a1
    ret
    snippet_end cxor

    snippet_begin cor
    c.or a0, a1
    ret
    snippet_end cor

    snippet_begin cand
    c.and a0, a1
    ret
    snippet_end cand

    snippet_begin clwsp
    addi sp, sp, -16
    sw a1, 0(sp)
    c.lwsp a0, 0(sp)
    addi sp, sp, 16
    ret
    snippet_end clwsp

    snippet_begin cswsp
    addi sp, sp, -16
    c.swsp a0, 0(sp)
    lw a0, 0(sp)
    addi sp, sp, 16
    ret
    snippet_end cswsp

    snippet_begin cjr
    c.jr a1
    c.li a0, 0
    ret
    .global core_cjr_target
core_cjr_target:
    c.li a0, 1
    ret
    snippet_end cjr

    snippet_begin cjalr
    mv a2, ra
    c.jalr a1
    .global core_cjalr_link
core_cjalr_link:
    c.li a0, 0
    mv ra, a2
    ret
    .global core_cjalr_target
core_cjalr_target:
    mv a0, ra
    mv ra, a2
    ret
    snippet_end cjalr

    snippet_begin cebreak
    c.ebreak
    ret
    snippet_end cebreak

    .option pop
