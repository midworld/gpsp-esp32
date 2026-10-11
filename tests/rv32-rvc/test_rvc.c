/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Checks rv32-rvc.h: assembler-made RVC/Zcb forms expand to the 32-bit
 * forms the assembler makes for the same instructions; mixed-length fetch
 * and fetches past the end. */

#include <stdio.h>
#include <stdlib.h>

#include "../rv32-rvc.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, \
        __LINE__, #c); failures++; } } while (0)

static size_t load(const char *path, uint8_t *buffer, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        printf("FAIL cannot open %s\n", path);
        exit(1);
    }
    size_t n = fread(buffer, 1, size, file);
    fclose(file);
    return n;
}

static void test_fixtures(const char *compressed, const char *expanded)
{
    uint8_t c[512], e[1024];
    size_t cn = load(compressed, c, sizeof(c));
    size_t en = load(expanded, e, sizeof(e));
    CHECK(cn * 2 == en);
    size_t pc = 0;
    unsigned count = 0;
    while (pc < cn)
    {
        uint32_t word, want;
        unsigned length = rv32_rvc_fetch(c, cn, pc, &word);
        CHECK(length == 2);
        if (length != 2)
            return;
        memcpy(&want, e + count * 4, 4);
        if (word != want)
        {
            printf("FAIL fixture %u: %08x expands to %08x, want %08x\n", count,
                    (unsigned)(c[pc] | c[pc + 1] << 8), (unsigned)word,
                    (unsigned)want);
            failures++;
        }
        pc += length;
        count++;
    }
    printf("%u RVC/Zcb forms match the assembler\n", count);
}

/* c.li a0, 0; addi a0, a0, 5 (at pc 2); c.jalr a5; then a 32-bit word cut
   by the end of the buffer. */
static void test_mixed(void)
{
    uint8_t code[10];
    uint16_t li = 0x4501;          /* c.li a0, 0 */
    uint32_t addi = 0x00550513;    /* addi a0, a0, 5 */
    uint16_t jalr = 0x9782;        /* c.jalr a5 */
    memcpy(code, &li, 2);
    memcpy(code + 2, &addi, 4);
    memcpy(code + 6, &jalr, 2);
    memcpy(code + 8, &addi, 2);    /* first half of a 32-bit word */
    uint32_t word;
    CHECK(rv32_rvc_fetch(code, sizeof(code), 0, &word) == 2 &&
            word == 0x00000513);
    CHECK(rv32_rvc_fetch(code, sizeof(code), 2, &word) == 4 && word == addi);
    CHECK(rv32_rvc_fetch(code, sizeof(code), 6, &word) == 2 &&
            word == 0x000780e7);       /* jalr ra, 0(a5) */
    /* Its length is 2; rv32-thumb-batch checks the stored link. */
    size_t pc = 6;
    unsigned length = rv32_rvc_fetch(code, sizeof(code), pc, &word);
    CHECK(pc + length == 8);
    CHECK(rv32_rvc_fetch(code, sizeof(code), 8, &word) == 0);
    CHECK(rv32_rvc_fetch(code, sizeof(code), 9, &word) == 0);
    CHECK(rv32_rvc_fetch(code, sizeof(code), 10, &word) == 0);
    uint16_t reserved = 0;
    CHECK(rv32_rvc_fetch(&reserved, 2, 0, &word) == 0);
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: %s compressed.bin expanded.bin\n", argv[0]);
        return 2;
    }
    test_fixtures(argv[1], argv[2]);
    test_mixed();
    printf("RV32 RVC fetch: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
