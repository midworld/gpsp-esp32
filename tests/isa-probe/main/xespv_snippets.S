/* Scalar and 128-bit PIE snippets copied to executable RAM.
 * Scalar inputs are a0/a1; vector inputs are buffers at a0/a1.
 * Vector output is at a2[0..15], with saved q0-q2 at a2[16..63].
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdkconfig.h"

    .option push
    /* Earlier P4 revisions use the v2.1 assembler feature name. */
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    .option arch, +xespv2p1
#else
    .option arch, +xespv
#endif
    .option norvc

    /* Flash-resident scalar and SIMD instructions isolate the first PIE opcode. */
    .section .text.probe_xespv_baseline, "ax", @progbits
    .balign 4
    .global probe_xespv_baseline
    .type probe_xespv_baseline, @function
probe_xespv_baseline:
    esp.addx2 a0, a0, a1
    ret
    .size probe_xespv_baseline, .-probe_xespv_baseline

    /* Isolate an arithmetic PIE opcode from vector load/store setup. */
    .balign 4
    .global probe_xespv_xorq_bare
    .type probe_xespv_xorq_bare, @function
probe_xespv_xorq_bare:
    esp.xorq q2, q0, q1
    ret
    .size probe_xespv_xorq_bare, .-probe_xespv_xorq_bare

    .balign 4
    .global probe_xespv_xorq_smoke
    .type probe_xespv_xorq_smoke, @function
probe_xespv_xorq_smoke:
    esp.vld.128.ip q0, a0, 0
    esp.vld.128.ip q1, a1, 0
    esp.xorq q2, q0, q1
    esp.vst.128.ip q2, a2, 0
    ret
    .size probe_xespv_xorq_smoke, .-probe_xespv_xorq_smoke

    .balign 4
    .global probe_xespv_vaddu8_smoke
    .type probe_xespv_vaddu8_smoke, @function
probe_xespv_vaddu8_smoke:
    esp.vld.128.ip q0, a0, 0
    esp.vld.128.ip q1, a1, 0
    esp.vadd.u8 q2, q0, q1
    esp.vst.128.ip q2, a2, 0
    ret
    .size probe_xespv_vaddu8_smoke, .-probe_xespv_vaddu8_smoke

    .balign 4
    .global probe_xespv_vld_smoke
    .type probe_xespv_vld_smoke, @function
probe_xespv_vld_smoke:
    esp.vld.128.ip q0, a0, 0
    ret
    .size probe_xespv_vld_smoke, .-probe_xespv_vld_smoke

    .balign 4
    .global probe_xespv_vst_smoke
    .type probe_xespv_vst_smoke, @function
probe_xespv_vst_smoke:
    esp.vst.128.ip q0, a0, 0
    ret
    .size probe_xespv_vst_smoke, .-probe_xespv_vst_smoke

    .balign 4
    .global probe_xespv_copy_smoke
    .type probe_xespv_copy_smoke, @function
probe_xespv_copy_smoke:
    esp.vld.128.ip q0, a0, 0
    esp.vst.128.ip q0, a1, 0
    ret
    .size probe_xespv_copy_smoke, .-probe_xespv_copy_smoke

    /* Trigger IDF's normal lazy PIE ownership path before guarded probes. */
    .balign 4
    .global probe_xespv_claim
    .type probe_xespv_claim, @function
probe_xespv_claim:
    esp.vld.128.ip q0, a0, 0
    ret
    .size probe_xespv_claim, .-probe_xespv_claim

    /* Preserve this task's live Q registers even when a probe traps. */
    .balign 4
    .global probe_xespv_save_q
    .type probe_xespv_save_q, @function
probe_xespv_save_q:
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
    esp.vst.128.ip q2, a0, 16
    ret
    .size probe_xespv_save_q, .-probe_xespv_save_q

    .balign 4
    .global probe_xespv_restore_q
    .type probe_xespv_restore_q, @function
probe_xespv_restore_q:
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, 16
    esp.vld.128.ip q2, a0, 16
    ret
    .size probe_xespv_restore_q, .-probe_xespv_restore_q

    .section .rodata.xespv_probe, "a", @progbits
    .balign 4

    .macro snippet_begin name
    .global xespv_\name\()_start
xespv_\name\()_start:
    .endm

    .macro snippet_end name
    .global xespv_\name\()_end
xespv_\name\()_end:
    .balign 4
    .endm

    .macro scalar name, op
    snippet_begin \name
    \op a0, a0, a1
    ret
    snippet_end \name
    .endm

    snippet_begin bare_xorq
    esp.xorq q2, q0, q1
    ret
    snippet_end bare_xorq

    /* C checks scaled add/sub results, including 32-bit wraparound. */
    scalar addx2, esp.addx2
    scalar addx4, esp.addx4
    scalar subx2, esp.subx2
    scalar subx4, esp.subx4

    .macro vector name, op
    /* Preserve q0-q2 so the probe does not alter the caller's PIE state. */
    snippet_begin \name
    addi a3, a2, 16
    esp.vst.128.ip q0, a3, 0
    addi a3, a2, 32
    esp.vst.128.ip q1, a3, 0
    addi a3, a2, 48
    esp.vst.128.ip q2, a3, 0
    esp.vld.128.ip q0, a0, 0
    esp.vld.128.ip q1, a1, 0
    \op q2, q0, q1
    esp.vst.128.ip q2, a2, 0
    addi a3, a2, 16
    esp.vld.128.ip q0, a3, 0
    addi a3, a2, 32
    esp.vld.128.ip q1, a3, 0
    addi a3, a2, 48
    esp.vld.128.ip q2, a3, 0
    ret
    snippet_end \name
    .endm

    /* Unsigned byte saturation versus a 128-bit bitwise XOR. */
    vector vaddu8, esp.vadd.u8
    vector xorq, esp.xorq
    vector andq, esp.andq
    vector orq, esp.orq
    vector vmaxu8, esp.vmax.u8
    vector vminu8, esp.vmin.u8

    /* Copy 16 bytes through q0 to check PIE load/store from generated code. */
    snippet_begin copy
    esp.vld.128.ip q0, a0, 0
    esp.vst.128.ip q0, a1, 0
    ret
    snippet_end copy

    .option pop
