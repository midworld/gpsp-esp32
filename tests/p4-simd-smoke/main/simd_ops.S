/* Each snippet is position independent so it can also run from RAM. */
/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +xespv
    .option norvc
    .text

    .macro operation name, op
    .balign 4
    .global simd_\name\()_start
    .global simd_\name\()_end
    .type simd_\name\()_start, @function
simd_\name\()_start:
    esp.vld.128.ip q0, a0, 0
    esp.vld.128.ip q1, a1, 0
    \op q2, q0, q1
    esp.vst.128.ip q2, a2, 0
    ret
simd_\name\()_end:
    .size simd_\name\()_start, .-simd_\name\()_start
    .endm

    .balign 4
    .global simd_copy_start
    .global simd_copy_end
    .type simd_copy_start, @function
simd_copy_start:
    esp.vld.128.ip q0, a0, 0
    esp.vst.128.ip q0, a2, 0
    ret
simd_copy_end:
    .size simd_copy_start, .-simd_copy_start

    operation xorq, esp.xorq
    operation vaddu8, esp.vadd.u8
    operation andq, esp.andq
    operation orq, esp.orq
    operation vmaxu8, esp.vmax.u8
    operation vminu8, esp.vmin.u8

    .option pop
