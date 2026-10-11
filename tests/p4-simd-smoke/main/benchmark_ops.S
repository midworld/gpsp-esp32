/* Timed register-save pairs and 16-byte SIMD kernels.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +zcmp, +xespv
    .section .iram1, "ax", @progbits

    .macro bench_start name
    .balign 4
    .global \name
    .type \name, @function
\name:
    rdcycle t0
1:
    .endm

    .macro bench_end name, counter
    addi \counter, \counter, -1
    bnez \counter, 1b
    rdcycle t1
    sub a0, t1, t0
    ret
    .size \name, .-\name
    .endm

    bench_start benchmark_empty
    bench_end benchmark_empty, a0

    bench_start benchmark_manual_pair
    addi sp, sp, -16
    sw s1, 4(sp)
    sw s0, 8(sp)
    sw ra, 12(sp)
    lw s1, 4(sp)
    lw s0, 8(sp)
    lw ra, 12(sp)
    addi sp, sp, 16
    bench_end benchmark_manual_pair, a0

    bench_start benchmark_zcmp_pair
    cm.push {ra,s0-s1}, -16
    cm.pop {ra,s0-s1}, 16
    bench_end benchmark_zcmp_pair, a0

    .macro simd_binary name, operation
    .balign 4
    .global benchmark_simd_\name
    .type benchmark_simd_\name, @function
benchmark_simd_\name:
    rdcycle t0
1:
    esp.vld.128.ip q0, a0, 0
    esp.vld.128.ip q1, a1, 0
    \operation q2, q0, q1
    esp.vst.128.ip q2, a2, 0
    addi a3, a3, -1
    bnez a3, 1b
    rdcycle t1
    sub a0, t1, t0
    ret
    .size benchmark_simd_\name, .-benchmark_simd_\name
    .endm

    .balign 4
    .global benchmark_simd_copy
    .type benchmark_simd_copy, @function
benchmark_simd_copy:
    rdcycle t0
1:
    esp.vld.128.ip q0, a0, 0
    esp.vst.128.ip q0, a2, 0
    addi a3, a3, -1
    bnez a3, 1b
    rdcycle t1
    sub a0, t1, t0
    ret
    .size benchmark_simd_copy, .-benchmark_simd_copy

    simd_binary xorq, esp.xorq
    simd_binary vaddu8, esp.vadd.u8
    simd_binary andq, esp.andq
    simd_binary orq, esp.orq
    simd_binary vmaxu8, esp.vmax.u8
    simd_binary vminu8, esp.vmin.u8

    .option pop
