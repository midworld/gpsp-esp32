/* RV32 RAM dirty range tests.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_ram_units.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

static void check_range(uint32_t address, unsigned bytes)
{
    for (unsigned i = 0; i < gpsp_rv32_ram_dirty_count(); i++)
    {
        unsigned size;
        if (gpsp_rv32_ram_dirty_get(i, &size) == address && size == bytes)
            return;
    }
    CHECK(0 && "missing dirty range");
}

static void test_contiguous(void)
{
    gpsp_rv32_ram_dirty_clear();
    for (unsigned i = 0; i < 41; i++)
        gpsp_rv32_ram_dirty_add(0x03002880 + 4 * i, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == 1);
    check_range(0x03002880, 164);
    CHECK(!gpsp_rv32_ram_dirty_overflow());
}

static void test_overlap(void)
{
    gpsp_rv32_ram_dirty_clear();
    gpsp_rv32_ram_dirty_add(0x02000102, 4);
    gpsp_rv32_ram_dirty_add(0x02000102, 4);
    gpsp_rv32_ram_dirty_add(0x02000100, 4);
    gpsp_rv32_ram_dirty_add(0x02000104, 4);
    gpsp_rv32_ram_dirty_add(0x02000102, 2);
    CHECK(gpsp_rv32_ram_dirty_count() == 1);
    check_range(0x02000100, 8);
    gpsp_rv32_ram_dirty_add(0x0200010a, 2);
    CHECK(gpsp_rv32_ram_dirty_count() == 2);
    check_range(0x02000100, 8);
    check_range(0x0200010a, 2);
    CHECK(!gpsp_rv32_ram_dirty_overflow());
}

static void test_bridges(bool reverse)
{
    gpsp_rv32_ram_dirty_clear();
    unsigned count = GPSP_RV32_RAM_DIRTY;
    for (unsigned i = 0; i < count; i++)
    {
        unsigned index = reverse ? count - 1 - i : i;
        gpsp_rv32_ram_dirty_add(0x02000000 + 8 * index, 4);
    }
    CHECK(gpsp_rv32_ram_dirty_count() == count);
    for (unsigned i = 0; i < count - 1; i++)
    {
        unsigned index = reverse ? count - 2 - i : i;
        gpsp_rv32_ram_dirty_add(0x02000004 + 8 * index, 4);
        CHECK(!gpsp_rv32_ram_dirty_overflow());
    }
    CHECK(gpsp_rv32_ram_dirty_count() == 1);
    check_range(0x02000000, 8 * count - 4);
    gpsp_rv32_ram_dirty_add(0x02000100, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == 2);
    check_range(0x02000100, 4);
    CHECK(!gpsp_rv32_ram_dirty_overflow());
}

static void test_multi_merge(void)
{
    gpsp_rv32_ram_dirty_clear();
    gpsp_rv32_ram_dirty_add(0x02000000, 1);
    gpsp_rv32_ram_dirty_add(0x02000002, 1);
    gpsp_rv32_ram_dirty_add(0x02000004, 1);
    CHECK(gpsp_rv32_ram_dirty_count() == 3);
    /* The last range moves into a removed entry and must still be checked. */
    gpsp_rv32_ram_dirty_add(0x02000000, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == 1);
    check_range(0x02000000, 5);
    CHECK(!gpsp_rv32_ram_dirty_overflow());
}

static void test_mirror(uint32_t end, uint32_t start, uint32_t alias)
{
    gpsp_rv32_ram_dirty_clear();
    gpsp_rv32_ram_dirty_add(end, 4);
    gpsp_rv32_ram_dirty_add(alias, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == 2);
    check_range(end, 4);
    check_range(start, 4);
    gpsp_rv32_ram_dirty_add(start, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == 2);
    CHECK(!gpsp_rv32_ram_dirty_overflow());
}

static void test_size_limit(void)
{
    gpsp_rv32_ram_dirty_clear();
    for (unsigned i = 0; i < 16384; i++)
        gpsp_rv32_ram_dirty_add(0x02000000 + 4 * i, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == 2);
    check_range(0x02000000, 65532);
    check_range(0x0200fffc, 4);
    CHECK(!gpsp_rv32_ram_dirty_overflow());
}

static void test_overflow(void)
{
    gpsp_rv32_ram_dirty_clear();
    for (unsigned i = 0; i < GPSP_RV32_RAM_DIRTY; i++)
        gpsp_rv32_ram_dirty_add(0x02000000 + 8 * i, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == GPSP_RV32_RAM_DIRTY);
    CHECK(!gpsp_rv32_ram_dirty_overflow());
    gpsp_rv32_ram_dirty_add(0x02000000 + 8 * GPSP_RV32_RAM_DIRTY, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == GPSP_RV32_RAM_DIRTY);
    CHECK(gpsp_rv32_ram_dirty_overflow());
    gpsp_rv32_ram_dirty_clear();
    CHECK(gpsp_rv32_ram_dirty_count() == 0);
    CHECK(!gpsp_rv32_ram_dirty_overflow());
    gpsp_rv32_ram_dirty_add(0x02000000, 4);
    CHECK(gpsp_rv32_ram_dirty_count() == 1);
    check_range(0x02000000, 4);
}

int main(void)
{
    test_contiguous();
    test_overlap();
    test_bridges(false);
    test_bridges(true);
    test_multi_merge();
    test_mirror(0x03007ffc, 0x03000000, 0x03008000);
    test_mirror(0x0203fffc, 0x02000000, 0x02040000);
    test_size_limit();
    test_overflow();
    puts("dirty ranges PASS: copies, overlaps, bridges, mirrors, size limit, overflow");
    return 0;
}
