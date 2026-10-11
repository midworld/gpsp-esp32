/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"

EXT_RAM_BSS_ATTR uint8_t ewram[0x80000];
EXT_RAM_BSS_ATTR uint8_t iwram[0x10000];

extern int test_multi_ram_call(uint32_t address, uint32_t count,
        uint32_t *host, uint32_t *cycles);

static int check(uint32_t address, uint32_t count,
        const void *expected_host, uint32_t expected_cycles)
{
    uint32_t host = UINT32_MAX;
    uint32_t cycles = UINT32_MAX;
    int preserved = test_multi_ram_call(address, count, &host, &cycles);
    int pass = preserved && host == (uint32_t)expected_host &&
            cycles == expected_cycles;
    if (!pass)
        printf("FAIL addr=%08lx count=%lu host=%08lx cycles=%lu preserved=%d\n",
                (unsigned long)address, (unsigned long)count,
                (unsigned long)host, (unsigned long)cycles, preserved);
    return pass;
}

void app_main(void)
{
    memset(ewram, 0xa5, sizeof(ewram));
    memset(iwram, 0x5a, sizeof(iwram));
    int pass = 1;

    pass &= check(0x02000000, 1, ewram, 6);
    pass &= check(0x02040004, 3, ewram + 4, 18);
    pass &= check(0x0203fff8, 2, ewram + 0x3fff8, 12);
    pass &= check(0x0203fff8, 3, NULL, 0);
    pass &= check(0x03008000, 9, iwram + 0x8000, 9);
    pass &= check(0x03017ffc, 1, iwram + 0xfffc, 1);
    pass &= check(0x03017ffc, 2, NULL, 0);
    pass &= check(0x04000000, 2, NULL, 0);
    pass &= check(0x08000000, 2, NULL, 0);
    pass &= check(0x02000001, 1, NULL, 0);
    pass &= check(0x02000000, 0, NULL, 0);
    pass &= check(0x02000000, 10, NULL, 0);
    pass &= ewram[0] == 0xa5 && ewram[0x3ffff] == 0xa5 &&
            iwram[0x8000] == 0x5a && iwram[0xffff] == 0x5a;

    printf("RV32 THUMB MULTI RAM RESULT: %s\n", pass ? "PASS" : "FAIL");
    printf("RV32 THUMB MULTI RAM COMPLETE\n");
}
