/* RV32 block link slot tests.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_block_link.h"
#include "rv32_register_map.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

/* Instructions emitted outside a fixed span. */
static unsigned free_words;

static void count_free(const gpsp_rv32_code *code)
{
    free_words += !code->fixed_depth;
}

static uint32_t linked_target(const uint32_t words[2])
{
    CHECK((words[0] & 0xfff) == ((RV_TMP1 << 7) | 0x37u));
    CHECK((words[1] & 0xfffff) == ((RV_TMP1 << 15) | 0x67u));
    int32_t lower = (int32_t)words[1] >> 20;
    return (words[0] & 0xfffff000u) + (uint32_t)lower;
}

int main(void)
{
    uint32_t words[4] = {0};
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));

    uint8_t *site = NULL;
    CHECK(gpsp_rv32_emit_link_slot(&code, &site));
    CHECK(site == code.start && code.ptr == code.start + GPSP_RV32_LINK_SLOT_SIZE);
    CHECK(words[0] == 0x00008067u);
    CHECK(words[1] == 0x00000013u);

    const uint32_t targets[] = {0x00001000u, 0x12345800u,
            0x80000000u, 0xfffff800u};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
    {
        CHECK(gpsp_rv32_patch_link_slot(&code, site, targets[i]));
        CHECK(linked_target(words) == targets[i]);
    }

    uint32_t before[2];
    memcpy(before, words, sizeof(before));
    CHECK(!gpsp_rv32_patch_link_slot(&code, site, 0));
    CHECK(!gpsp_rv32_patch_link_slot(&code, site, 0x12345u));
#if UINTPTR_MAX > UINT32_MAX
    CHECK(!gpsp_rv32_patch_link_slot(&code, site, UINT64_C(0x100000000)));
#endif
    CHECK(!gpsp_rv32_patch_link_slot(&code, site + 1, 0x1000u));
    CHECK(!gpsp_rv32_patch_link_slot(&code, site + 4, 0x1000u));
    CHECK(memcmp(before, words, sizeof(before)) == 0);

    code.ptr = site + 4;
    CHECK(!gpsp_rv32_patch_link_slot(&code, site, 0x1000u));
    CHECK(memcmp(before, words, sizeof(before)) == 0);
    code.ptr = site + GPSP_RV32_LINK_SLOT_SIZE;

    /* Unlinking restores the emitted form, and an unlinked slot stays so. */
    CHECK(gpsp_rv32_unlink_link_slot(site));
    CHECK(words[0] == 0x00008067u && words[1] == 0x00000013u);
    CHECK(gpsp_rv32_unlink_link_slot(site));
    CHECK(words[0] == 0x00008067u && words[1] == 0x00000013u);
    CHECK(!gpsp_rv32_unlink_link_slot(site + 2));
    CHECK(gpsp_rv32_patch_link_slot(&code, site, targets[0]));

    words[1] = 0;
    memcpy(before, words, sizeof(before));
    CHECK(!gpsp_rv32_patch_link_slot(&code, site, 0x1000u));
    CHECK(memcmp(before, words, sizeof(before)) == 0);
    CHECK(!gpsp_rv32_unlink_link_slot(site));
    CHECK(memcmp(before, words, sizeof(before)) == 0);

    uint32_t short_words[1] = {0};
    gpsp_rv32_code short_code;
    gpsp_rv32_code_init(&short_code, short_words, sizeof(short_words));
    uint8_t *old_site = site;
    CHECK(!gpsp_rv32_emit_link_slot(&short_code, &site));
    CHECK(site == old_site && short_code.ptr == short_code.start);
    CHECK(short_words[0] == 0);

    /* Link slots keep full-length words whatever the parent policy. */
    gpsp_rv32_code_init(&code, words, sizeof(words));
    code.compress = true;
    gpsp_rv32_test_emit_hook = count_free;
    free_words = 0;
    CHECK(gpsp_rv32_emit_link_slot(&code, &site));
    CHECK(gpsp_rv32_patch_link_slot(&code, site, 0x12345800u));
    CHECK(free_words == 0 && code.fixed_depth == 0);
    CHECK(gpsp_rv32_addi(&code, A0, A0, 1) && free_words == 1);
    gpsp_rv32_test_emit_hook = NULL;

    /* A slot after two bytes of code needs a c.nop before it. */
    uint32_t padded[4];
    memset(padded, 0xa5, sizeof(padded));
    uint8_t *base = (uint8_t *)padded;
    gpsp_rv32_code_init(&code, base, sizeof(padded));
    code.compress = false;
    code.ptr += 2;
    CHECK(!gpsp_rv32_emit_link_slot(&code, &site));   /* compress off */
    CHECK(code.ptr == base + 2 && padded[1] == 0xa5a5a5a5u);
    code.compress = true;
    CHECK(gpsp_rv32_fixed_begin(&code));
    CHECK(!gpsp_rv32_emit_link_slot(&code, &site));   /* fixed span */
    CHECK(gpsp_rv32_fixed_end(&code) && code.ptr == base + 2);
    gpsp_rv32_code_init(&code, base, 11);
    code.compress = true;
    code.ptr += 2;
    CHECK(!gpsp_rv32_emit_link_slot(&code, &site));   /* no room */
    CHECK(code.ptr == base + 2 && padded[0] == 0xa5a5a5a5u);
    gpsp_rv32_code_init(&code, base, sizeof(padded));
    code.compress = true;
    code.ptr += 2;
    CHECK(gpsp_rv32_emit_link_slot(&code, &site));
    uint16_t half;
    memcpy(&half, base + 2, sizeof(half));
    CHECK(half == 0x0001 && site == base + 4);
    CHECK(padded[1] == 0x00008067u && padded[2] == 0x00000013u);
    CHECK(code.ptr == base + 4 + GPSP_RV32_LINK_SLOT_SIZE);

    /* A buffer may start after 16-bit code, as a block's fallthrough gate
       does; the slot is still aligned. */
    memset(padded, 0xa5, sizeof(padded));
    gpsp_rv32_code_init(&code, base + 2, sizeof(padded) - 2);
    code.compress = true;
    CHECK(gpsp_rv32_emit_link_slot(&code, &site));
    CHECK(site == base + 4 && padded[1] == 0x00008067u);
    CHECK(gpsp_rv32_patch_link_slot(&code, site, 0x12345800u));
    CHECK(linked_target(&padded[1]) == 0x12345800u);

    puts("RV32 block link slots: ok");
    return 0;
}
