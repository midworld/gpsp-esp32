/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Generated-code dumps for rv32-golden.py. With GPSP_TEST_GOLDEN, each call
 * prints one GOLDEN line: a running id, the code buffer base and size, and
 * the emitted words. The script replaces addresses of the buffer and of ELF
 * symbols, so dumps from different builds can be compared. */

#ifndef RV32_GOLDEN_H
#define RV32_GOLDEN_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(GPSP_TEST_GOLDEN)
static inline void rv32_golden_dump(const char *tag, const void *buffer,
        size_t buffer_size, const void *begin, const void *end)
{
    static unsigned id;
    const uint8_t *p = begin;
    printf("GOLDEN %s %u base=%08lx size=%lx at=%lx", tag, id++,
            (unsigned long)(uintptr_t)buffer, (unsigned long)buffer_size,
            (unsigned long)((const uint8_t *)begin - (const uint8_t *)buffer));
    for (; p + 4 <= (const uint8_t *)end; p += 4)
    {
        uint32_t word;
        memcpy(&word, p, sizeof(word));
        printf(" %08lx", (unsigned long)word);
    }
    printf("\n");
}
#else
#define rv32_golden_dump(tag, buffer, buffer_size, begin, end) ((void)0)
#endif

#endif
