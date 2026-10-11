/* RV32A/F snippets copied into executable RAM.
 * Each entry is independent and returns through an uncompressed RET.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option norvc
    .section .rodata.af_probe, "a", @progbits
    .balign 4

    .macro snippet_begin name
    .global af_\name\()_start
af_\name\()_start:
    .endm

    .macro snippet_end name
    .global af_\name\()_end
af_\name\()_end:
    .balign 4
    .endm

    /* a0 points to an aligned word; return SC status after at most 8 tries. */
    snippet_begin lrsc
    mv t2, a0
    li t1, 8
1:
    lr.w t0, (t2)
    addi t0, t0, 1
    sc.w a0, t0, (t2)
    beqz a0, 2f
    addi t1, t1, -1
    bnez t1, 1b
2:
    ret
    snippet_end lrsc

    /* AMOs return the old word in a0 and leave the new word in memory. */
    snippet_begin amoadd
    amoadd.w a0, a1, (a0)
    ret
    snippet_end amoadd

    snippet_begin amoswap
    amoswap.w a0, a1, (a0)
    ret
    snippet_end amoswap

    /* a0/a1/a2 point to input/input/output words containing IEEE-754 bits. */
    snippet_begin fcopy
    flw ft0, 0(a0)
    fsw ft0, 0(a2)
    ret
    snippet_end fcopy

    snippet_begin fadd
    flw ft0, 0(a0)
    flw ft1, 0(a1)
    fadd.s ft2, ft0, ft1
    fsw ft2, 0(a2)
    ret
    snippet_end fadd

    snippet_begin fmul
    flw ft0, 0(a0)
    flw ft1, 0(a1)
    fmul.s ft2, ft0, ft1
    fsw ft2, 0(a2)
    ret
    snippet_end fmul

    /* Explicit round-toward-zero keeps conversion independent of fcsr.frm. */
    snippet_begin fcvt
    flw ft0, 0(a0)
    fcvt.w.s a0, ft0, rtz
    ret
    snippet_end fcvt

    .option pop
