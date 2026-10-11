/* Hardware-loop instruction snippets; end instruction is arithmetic, not a jump.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option arch, +xesploop
    .option norvc
    .section .rodata.xesploop_probe, "a", @progbits
    .balign 4

    .macro snippet_begin name
    .global xesploop_\name\()_start
xesploop_\name\()_start:
    .endm

    .macro snippet_end name
    .global xesploop_\name\()_end
xesploop_\name\()_end:
    .balign 4
    .endm

    /* Each variant runs three iterations of (+1,+2) and must return 9. */
    /* Immediate count and end address in one instruction. */
    snippet_begin setupi
    esp.lp.setupi 0, 3, .Lsetupi_end
    addi a0, a0, 1
.Lsetupi_end:
    addi a0, a0, 2
    ret
    snippet_end setupi

    /* Register count comes from a1; end address is encoded. */
    snippet_begin setup
    esp.lp.setup 0, a1, .Lsetup_end
    addi a0, a0, 1
.Lsetup_end:
    addi a0, a0, 2
    ret
    snippet_end setup

    /* C preloads end/count; only starti supplies the start address. */
    snippet_begin starti
    esp.lp.starti 0, .Lstarti_start
    .global xesploop_starti_body_start
xesploop_starti_body_start:
.Lstarti_start:
    addi a0, a0, 1
    .global xesploop_starti_body_end
xesploop_starti_body_end:
    addi a0, a0, 2
    ret
    snippet_end starti

    /* C preloads start/count; only endi supplies the end address. */
    snippet_begin endi
    esp.lp.endi 0, .Lendi_end
    .global xesploop_endi_body_start
xesploop_endi_body_start:
    addi a0, a0, 1
    .global xesploop_endi_body_end
xesploop_endi_body_end:
.Lendi_end:
    addi a0, a0, 2
    ret
    snippet_end endi

    /* C preloads start/end; counti must replace the count of one. */
    snippet_begin counti
    esp.lp.counti 0, 3
    .global xesploop_counti_body_start
xesploop_counti_body_start:
    addi a0, a0, 1
    .global xesploop_counti_body_end
xesploop_counti_body_end:
    addi a0, a0, 2
    ret
    snippet_end counti

    /* C preloads start/end; count from a1 must replace the count of one. */
    snippet_begin count
    esp.lp.count 0, a1
    .global xesploop_count_body_start
xesploop_count_body_start:
    addi a0, a0, 1
    .global xesploop_count_body_end
xesploop_count_body_end:
    addi a0, a0, 2
    ret
    snippet_end count

    /* Repeat the failing forms directly from firmware flash. */
    .section .text.xesploop_flash_probe, "ax", @progbits
    .balign 4

    snippet_begin flash_setupi
    esp.lp.setupi 0, 3, .Lflash_setupi_end
    addi a0, a0, 1
.Lflash_setupi_end:
    addi a0, a0, 2
    ret
    snippet_end flash_setupi

    snippet_begin flash_endi
    esp.lp.endi 0, .Lflash_endi_end
    .global xesploop_flash_endi_body_start
xesploop_flash_endi_body_start:
    addi a0, a0, 1
    .global xesploop_flash_endi_body_end
xesploop_flash_endi_body_end:
.Lflash_endi_end:
    addi a0, a0, 2
    ret
    snippet_end flash_endi

    snippet_begin flash_counti
    esp.lp.counti 0, 3
    .global xesploop_flash_counti_body_start
xesploop_flash_counti_body_start:
    addi a0, a0, 1
    .global xesploop_flash_counti_body_end
xesploop_flash_counti_body_end:
    addi a0, a0, 2
    ret
    snippet_end flash_counti

    .option pop
