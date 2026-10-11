/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_entry_pack.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_offset(void)
{
    CHECK(!gpsp_rv32_packed_offset_valid(0, 0));
    CHECK(!gpsp_rv32_packed_offset_valid(0, 33));
    CHECK(gpsp_rv32_packed_offset_valid(4, 1));
    CHECK(!gpsp_rv32_packed_offset_valid(8, 1));
    CHECK(gpsp_rv32_packed_offset_valid(0xfffffffc, 32));
    CHECK(gpsp_rv32_packed_offset_valid(0, 22));
    CHECK(gpsp_rv32_packed_offset_valid(0x00fffffc, 22));
    CHECK(!gpsp_rv32_packed_offset_valid(0x01000000, 22));
    CHECK(!gpsp_rv32_packed_offset_valid(0x00000002, 22));
    CHECK(!gpsp_rv32_packed_offset_valid(UINT32_MAX, 22));
    CHECK(gpsp_rv32_packed_offset_valid(0xfffffff8, 30));
}

static void test_slow(void)
{
    uint32_t value = 0;
    CHECK(gpsp_rv32_slow_pack(0x00fffffc, 4092, &value));
    CHECK(gpsp_rv32_slow_offset(value) == 0x00fffffc);
    CHECK(gpsp_rv32_slow_size(value) == 4092);
    CHECK(gpsp_rv32_slow_pack(0, 0, &value) && value == 0);

    /* Each rejection keeps the previous value. */
    const uint32_t kept = 0x12345678;
    static const uint32_t bad[][2] = {
        { 0x01000000, 4 },   /* offset field */
        { 0, 4096 },         /* size field */
        { 2, 4 },            /* unaligned offset */
        { 4, 6 },            /* unaligned size */
        { UINT32_MAX, 4 },   /* not in the cache */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        value = kept;
        CHECK(!gpsp_rv32_slow_pack(bad[i][0], bad[i][1], &value));
        CHECK(value == kept);
    }
}

int main(void)
{
    test_offset();
    test_slow();
    printf("RV32 entry packing: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
