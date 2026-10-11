/* Zcmt jump snippets copied into executable RAM.
 * C writes each target address into the corresponding jvt table entry.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +zcmt

    /* The same table jump in flash separates instruction decoding from JIT RAM. */
    .section .text.zcmt_flash_probe, "ax", @progbits
    .balign 4
    .global zcmt_flash_jt0_start
zcmt_flash_jt0_start:
    cm.jt 0
    li a0, 0
    ret
    .global zcmt_flash_jt0_target
zcmt_flash_jt0_target:
    li a0, 0x10203040
    ret
    .global zcmt_flash_jt0_end
zcmt_flash_jt0_end:

    .section .rodata.zcmt_probe, "a", @progbits
    .balign 4

    .macro snippet_begin name
    .global zcmt_\name\()_start
zcmt_\name\()_start:
    .endm

    .macro snippet_end name
    .global zcmt_\name\()_end
zcmt_\name\()_end:
    .balign 4
    .endm

    .macro target name, value
    .global zcmt_\name\()_target
zcmt_\name\()_target:
    li a0, \value
    ret
    .endm

    /* cm.jt 0 must skip the zero-valued fallthrough and return from its target. */
    snippet_begin jt0
    cm.jt 0
    li a0, 0
    ret
    target jt0, 0x10203040
    snippet_end jt0

    /* cm.jt 31 checks the highest index that does not link through ra. */
    snippet_begin jt31
    cm.jt 31
    li a0, 0
    ret
    target jt31, 0x11223344
    snippet_end jt31

    /* a2 stores the caller's ra without changing sp before the guarded jump. */
    snippet_begin jalt32
    sw ra, 0(a2)
    cm.jalt 32
    .global zcmt_jalt32_resume
zcmt_jalt32_resume:
    lw ra, 0(a2)
    ret
    .global zcmt_jalt32_target
zcmt_jalt32_target:
    sw ra, 0(a1)
    li a0, 0x55667788
    ret
    snippet_end jalt32

    /* cm.jalt 255 checks the highest table index and its link address. */
    snippet_begin jalt255
    sw ra, 0(a2)
    cm.jalt 255
    .global zcmt_jalt255_resume
zcmt_jalt255_resume:
    lw ra, 0(a2)
    ret
    .global zcmt_jalt255_target
zcmt_jalt255_target:
    sw ra, 0(a1)
    li a0, 0x99aabbcc
    ret
    snippet_end jalt255

    .option pop
