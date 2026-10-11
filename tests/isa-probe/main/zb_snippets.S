/* One Zb instruction per snippet, followed by a return.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +zba, +zbb, +zbs
    .option norvc
    .section .rodata.zb_probe, "a", @progbits
    .balign 4

    .macro binary name, op
    .global zb_\name\()_start
zb_\name\()_start:
    \op a0, a0, a1
    ret
    .global zb_\name\()_end
zb_\name\()_end:
    .endm

    .macro unary name, op
    .global zb_\name\()_start
zb_\name\()_start:
    \op a0, a0
    ret
    .global zb_\name\()_end
zb_\name\()_end:
    .endm

    .macro immediate name, op, bit
    .global zb_\name\()_start
zb_\name\()_start:
    \op a0, a0, \bit
    ret
    .global zb_\name\()_end
zb_\name\()_end:
    .endm

    /* Zba scaled addition; C includes 32-bit wraparound operands. */
    binary sh1add, sh1add
    binary sh2add, sh2add
    binary sh3add, sh3add

    /* Zbb logic, counts, min/max, rotations, and byte/halfword extension. */
    /* C checks zero inputs, signedness, and wrapped register shift counts. */
    binary andn, andn
    binary orn, orn
    binary xnor, xnor
    unary clz, clz
    unary ctz, ctz
    unary cpop, cpop
    binary max, max
    binary maxu, maxu
    binary min, min
    binary minu, minu
    unary orcb, orc.b
    unary rev8, rev8
    binary rol, rol
    binary ror, ror
    immediate rori31, rori, 31
    unary sextb, sext.b
    unary sexth, sext.h
    unary zexth, zext.h

    /* Zbs bit operations; C checks bit indices 31, 32, and immediate 0. */
    binary bclr, bclr
    immediate bclri0, bclri, 0
    binary bext, bext
    immediate bexti0, bexti, 0
    binary binv, binv
    immediate binvi0, binvi, 0
    binary bset, bset
    immediate bseti0, bseti, 0

    .option pop
