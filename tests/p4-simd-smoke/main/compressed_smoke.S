/* Mixed-width fetch and equivalent ADDI timing snippets.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    /* Preserve alignment of copied snippets through linking. */
    .option norelax
    .option rvc
    .text
    .balign 4
    .global compressed_mixed_start
    .global compressed_mixed_word
    .global compressed_mixed_end
    .type compressed_mixed_start, @function
compressed_mixed_start:
    c.li a0, 1
compressed_mixed_word:
    .option norvc
    addi a0, a0, 40
    .option rvc
    c.addi a0, 1
    c.jr ra
compressed_mixed_end:
    .size compressed_mixed_start, .-compressed_mixed_start

    /* a0=count, a1=result pointer; return elapsed cycles in a0. */
    .macro bench_begin name
    .balign 4
    .global \name\()_start
    .global \name\()_body
    .global \name\()_body_end
    .global \name\()_end
    .type \name\()_start, @function
\name\()_start:
    .option norvc
    li a2, 0
    rdcycle t0
\name\()_body:
1:
    .endm

    .macro bench_end name
\name\()_body_end:
    .option norvc
    addi a0, a0, -1
    bnez a0, 1b
    rdcycle t1
    sw a2, 0(a1)
    sub a0, t1, t0
    ret
\name\()_end:
    .size \name\()_start, .-\name\()_start
    .endm

    bench_begin compressed_bench_empty
    bench_end compressed_bench_empty

    bench_begin compressed_bench_caddi
    .option rvc
    .rept 16
    c.addi a2, 1
    .endr
    bench_end compressed_bench_caddi

    bench_begin compressed_bench_addi
    .option norvc
    .rept 16
    addi a2, a2, 1
    .endr
    bench_end compressed_bench_addi
    .option pop
