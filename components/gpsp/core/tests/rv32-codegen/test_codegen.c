/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_codegen.h"
#include "../rv32-rvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition); \
        exit(1); \
    } \
} while (0)

static void emit_reference_sequence(gpsp_rv32_code *code)
{
#define EMIT(name, ...) CHECK(gpsp_rv32_##name(code, __VA_ARGS__))
    EMIT(add, 1, 2, 3);
    EMIT(sub, 4, 5, 6);
    EMIT(sll, 7, 8, 9);
    EMIT(slt, 10, 11, 12);
    EMIT(sltu, 13, 14, 15);
    EMIT(xor, 16, 17, 18);
    EMIT(srl, 19, 20, 21);
    EMIT(sra, 22, 23, 24);
    EMIT(or, 25, 26, 27);
    EMIT(and, 28, 29, 30);
    EMIT(mul, 1, 2, 3);
    EMIT(mulh, 4, 5, 6);
    EMIT(mulhsu, 7, 8, 9);
    EMIT(mulhu, 10, 11, 12);
    EMIT(addi, 5, 6, -2048);
    EMIT(slti, 7, 8, 2047);
    EMIT(sltiu, 9, 10, -1);
    EMIT(xori, 11, 12, -1);
    EMIT(ori, 13, 14, 511);
    EMIT(andi, 15, 16, -17);
    EMIT(slli, 17, 18, 31);
    EMIT(srli, 19, 20, 5);
    EMIT(srai, 21, 22, 31);
    EMIT(lui, 23, 0xabcde);
    EMIT(auipc, 24, 0xfffff);
    EMIT(andn, 25, 26, 27);
    EMIT(ror, 28, 29, 30);
    EMIT(rori, 31, 1, 31);
    EMIT(xnor, 2, 3, 4);
    EMIT(sh1add, 5, 6, 7);
    EMIT(sh2add, 8, 9, 10);
    EMIT(sh3add, 11, 12, 13);
    EMIT(bext, 14, 15, 16);
    EMIT(bexti, 17, 18, 0);
    EMIT(bexti, 19, 20, 31);
    EMIT(lb, 1, 2, -2048);
    EMIT(lh, 3, 4, 2047);
    EMIT(lw, 5, 6, -9);
    EMIT(lbu, 7, 8, 3);
    EMIT(lhu, 9, 10, 0);
    EMIT(sb, 11, 12, -2048);
    EMIT(sh, 13, 14, 2047);
    EMIT(sw, 15, 16, -9);
    EMIT(beq, 1, 2, 8);
    EMIT(addi, 0, 0, 0);
    EMIT(bne, 3, 4, 8);
    EMIT(addi, 0, 0, 0);
    EMIT(blt, 5, 6, 8);
    EMIT(addi, 0, 0, 0);
    EMIT(bge, 7, 8, 8);
    EMIT(addi, 0, 0, 0);
    EMIT(bltu, 9, 10, 8);
    EMIT(addi, 0, 0, 0);
    EMIT(bgeu, 11, 12, 8);
    EMIT(addi, 0, 0, 0);
    EMIT(jal, 1, 8);
    EMIT(addi, 0, 0, 0);
    EMIT(jalr, 2, 3, -2048);
    EMIT(addi, 0, 0, 0);
    EMIT(bne, 5, 6, -4);

    uint8_t *b_site = code->ptr;
    EMIT(beq, 1, 2, 0);
    EMIT(addi, 0, 0, 0);
    uint8_t *b_target = code->ptr;
    uint8_t *j_site = code->ptr;
    EMIT(jal, 3, 0);
    EMIT(addi, 0, 0, 0);
    uint8_t *j_target = code->ptr;
    EMIT(addi, 31, 0, 1);
    CHECK(gpsp_rv32_patch_branch(code, b_site, b_target));
    CHECK(gpsp_rv32_patch_branch(code, j_site, j_target));
#undef EMIT
}

static uint32_t read_word(const uint8_t *ptr)
{
    uint32_t word;
    memcpy(&word, ptr, sizeof(word));
    return word;
}

static int32_t decode_b(uint32_t word)
{
    uint32_t imm = ((word >> 31) << 12) | (((word >> 7) & 1) << 11) |
            (((word >> 25) & 0x3f) << 5) | (((word >> 8) & 0xf) << 1);
    if (imm & 0x1000)
        imm |= ~0x1fffU;
    return (int32_t)imm;
}

static int32_t decode_j(uint32_t word)
{
    uint32_t imm = ((word >> 31) << 20) | (((word >> 12) & 0xff) << 12) |
            (((word >> 20) & 1) << 11) | (((word >> 21) & 0x3ff) << 1);
    if (imm & 0x100000)
        imm |= ~0x1fffffU;
    return (int32_t)imm;
}

static void emit_compressed_sequence(gpsp_rv32_code *code)
{
    enum { A0 = 10, A1, A2, A3, A5 = 15, A6, A7, S0 = 8, S1, S2 = 18, T0 = 5,
            T1, T5 = 30, T6, RA = 1 };
#define EMIT(name, ...) CHECK(gpsp_rv32_##name(code, __VA_ARGS__))
    EMIT(addi, 0, 0, 0);
    EMIT(addi, A0, 0, -32);
    EMIT(addi, A0, 0, 31);
    EMIT(addi, A0, 0, 32);
    EMIT(addi, A0, A0, 1);
    EMIT(addi, A0, A0, -32);
    EMIT(addi, A0, A0, 32);
    EMIT(addi, S2, S2, 5);
    EMIT(add, A0, A0, A1);
    EMIT(add, A0, 0, A1);
    EMIT(add, A0, A1, A2);
    EMIT(slli, A0, A0, 3);
    EMIT(slli, T6, T6, 31);
    EMIT(slli, A0, A1, 3);
    EMIT(lui, A0, 1);
    EMIT(lui, A0, 0xfffff);
    EMIT(lui, A0, 32);
    EMIT(jalr, 0, RA, 0);
    EMIT(jalr, RA, T1, 0);
    EMIT(jalr, T0, T1, 0);
    EMIT(srli, A0, A0, 1);
    EMIT(srai, S0, S0, 31);
    EMIT(srli, T6, T6, 1);
    EMIT(srai, A7, A7, 2);
    EMIT(andi, A5, A5, -32);
    EMIT(andi, A5, A5, 31);
    EMIT(andi, A5, A5, 32);
    EMIT(andi, A6, A6, 1);
    EMIT(andi, S1, S0, 1);
    EMIT(sub, A0, A0, A1);
    EMIT(sub, A0, A1, A0);
    EMIT(sub, T1, T1, A0);
    EMIT(xor, A0, A0, A5);
    EMIT(or, S1, S1, S0);
    EMIT(and, A2, A2, A3);
    EMIT(and, A6, A6, A3);
    EMIT(and, A2, A2, A7);
    EMIT(lw, A0, S0, 0);
    EMIT(lw, A5, A1, 124);
    EMIT(lw, A5, A1, 128);
    EMIT(lw, A5, A1, 2);
    EMIT(lw, A5, A1, -4);
    EMIT(lw, T5, S0, 0);
    EMIT(lw, A0, T0, 0);
    EMIT(sw, A0, S0, 0);
    EMIT(sw, A5, A1, 124);
    EMIT(sw, A5, A1, 128);
    EMIT(sw, T6, S0, 4);
    EMIT(sw, A0, T0, 4);
#undef EMIT
}

/* The assembler's choice of 16-bit forms matches the encoders. */
static void test_compressed_reference(const char *path)
{
    uint32_t storage[64];
    memset(storage, 0xcc, sizeof(storage));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    code.compress = true;
    emit_compressed_sequence(&code);
    size_t size = (size_t)(code.ptr - code.start);

    uint8_t reference[sizeof(storage)];
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    size_t read = fread(reference, 1, sizeof(reference), file);
    CHECK(fclose(file) == 0);
    CHECK(read == size);
    unsigned halves = 0;
    for (size_t at = 0, length; at < size; at += length)
    {
        uint32_t mine = 0, theirs = 0;
        length = rv32_rvc_fetch(storage, size, at, &mine);
        (void)mine;
        (void)theirs;
        unsigned other = rv32_rvc_fetch(reference, size, at, &theirs);
        if (!length || length != other ||
            memcmp((uint8_t *)storage + at, reference + at, length))
        {
            fprintf(stderr, "offset %zu: generated %zu bytes, assembler %u bytes\n",
                    at, length, other);
            exit(1);
        }
        halves += length == 2;
    }
    printf("%u 16-bit forms and their 32-bit neighbors match the assembler\n",
            halves);
}

/* Matches short_branch.S. */
static void test_short_branch_reference(const char *path)
{
    enum { A0 = 10, A4 = 14, A5 = 15, S0 = 8, S1 = 9, X0 = 0 };
    uint16_t storage[32];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    code.compress = true;
    code.short_branch = true;
    CHECK(gpsp_rv32_beq(&code, A0, X0, 254));
    CHECK(gpsp_rv32_beq(&code, X0, A0, -256));
    CHECK(gpsp_rv32_bne(&code, S1, X0, 2));
    CHECK(gpsp_rv32_bne(&code, X0, A5, -2));
    CHECK(gpsp_rv32_beq(&code, S0, X0, 0));
    CHECK(gpsp_rv32_bne(&code, A4, X0, 128));
    CHECK(gpsp_rv32_jal(&code, X0, 2046));
    CHECK(gpsp_rv32_jal(&code, X0, -2048));
    CHECK(gpsp_rv32_jal(&code, X0, 0));
    CHECK(gpsp_rv32_jal(&code, X0, 86));
    size_t size = (size_t)(code.ptr - code.start);
    CHECK(size == 20);

    uint8_t reference[sizeof(storage)];
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    size_t read = fread(reference, 1, sizeof(reference), file);
    CHECK(fclose(file) == 0);
    CHECK(read == size && !memcmp(storage, reference, size));

    /* Patching a zero-offset placeholder gives the same bytes. */
    static const int offsets[10] = {254, -256, 2, -2, 0, 128, 2046, -2048, 0, 86};
    uint8_t patched[2 * 2048 + 64];
    for (unsigned i = 0; i < 10; i++)
    {
        memset(patched, 0, sizeof(patched));
        unsigned site = offsets[i] < 0 ? (unsigned)-offsets[i] : 0;
        gpsp_rv32_code_init(&code, patched, sizeof(patched));
        code.compress = true;
        code.short_branch = true;
        code.ptr += site;
        uint16_t half;
        memcpy(&half, reference + 2 * i, 2);
        bool jump = (half >> 13) == 5;
        CHECK(jump ? gpsp_rv32_jal(&code, X0, 0) :
                ((half >> 13) == 6 ? gpsp_rv32_beq : gpsp_rv32_bne)(&code,
                        8 + ((half >> 7) & 7), X0, 0));
        CHECK(code.ptr == patched + site + 2);
        unsigned end = site + 2 + (offsets[i] > 0 ? (unsigned)offsets[i] : 0);
        code.ptr = patched + end;
        CHECK(gpsp_rv32_patch_branch(&code, patched + site,
                patched + site + offsets[i]));
        CHECK(!code.branch_overflow);
        CHECK(!memcmp(patched + site, reference + 2 * i, 2));
    }
    printf("%zu bytes of short branches match the assembler\n", size);
}

/* Short branch policy, reach and failure. */
static void test_short_branch_policy(void)
{
    enum { A0 = 10, A1 = 11, T0 = 5, RA = 1, X0 = 0 };
    uint8_t storage[4096 + 16];
    gpsp_rv32_code code;

    /* Off by default and outside compressed policy or fixed spans. */
    for (unsigned mode = 0; mode < 4; mode++)
    {
        gpsp_rv32_code_init(&code, storage, sizeof(storage));
        code.compress = mode != 1;
        code.short_branch = mode != 0;
        CHECK(mode != 2 || gpsp_rv32_fixed_begin(&code));
        CHECK(gpsp_rv32_beq(&code, A0, X0, 8));
        CHECK(gpsp_rv32_jal(&code, X0, 8));
        size_t expected = mode == 3 ? 4 : 8;
        CHECK((size_t)(code.ptr - code.start) == expected);
    }

    /* Forms without a short encoding stay 32-bit. */
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    code.compress = true;
    code.short_branch = true;
    CHECK(gpsp_rv32_beq(&code, T0, X0, 8));    /* not x8-x15 */
    CHECK(gpsp_rv32_bne(&code, A0, A1, 8));    /* two registers */
    CHECK(gpsp_rv32_blt(&code, A0, X0, 8));    /* other condition */
    CHECK(gpsp_rv32_beq(&code, A0, X0, 256));  /* beyond reach */
    CHECK(gpsp_rv32_jal(&code, RA, 8));        /* links */
    CHECK(gpsp_rv32_jal(&code, X0, 2048));     /* beyond reach */
    CHECK((size_t)(code.ptr - code.start) == 24);

    /* A target beyond reach fails, keeps the bytes and sets the flag. */
    for (unsigned jump = 0; jump < 2; jump++)
    {
        memset(storage, 0, sizeof(storage));
        gpsp_rv32_code_init(&code, storage, sizeof(storage));
        code.compress = true;
        code.short_branch = true;
        CHECK(jump ? gpsp_rv32_jal(&code, X0, 0) :
                gpsp_rv32_bne(&code, A0, X0, 0));
        uint16_t before;
        memcpy(&before, storage, 2);
        unsigned limit = jump ? 2048 : 256;
        code.ptr = storage + limit;
        CHECK(!gpsp_rv32_patch_branch(&code, storage, storage + limit));
        CHECK(code.branch_overflow);
        CHECK(!memcmp(storage, &before, 2));
        code.branch_overflow = false;
        CHECK(gpsp_rv32_patch_branch(&code, storage, storage + limit - 2));
        CHECK(!code.branch_overflow);
    }

    /* Other patch failures do not set the flag. */
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    code.compress = true;
    code.short_branch = true;
    CHECK(gpsp_rv32_addi(&code, A0, A0, 1));   /* c.addi, not a branch */
    CHECK(!gpsp_rv32_patch_branch(&code, storage, code.ptr));
    CHECK(!code.branch_overflow);

    /* A two-byte buffer: the patch reads and writes only its two bytes. */
    memset(storage, 0xa5, sizeof(storage));
    gpsp_rv32_code_init(&code, storage, 2);
    code.compress = true;
    code.short_branch = true;
    CHECK(gpsp_rv32_bne(&code, A0, X0, 0));
    CHECK(code.ptr == code.end);
    CHECK(gpsp_rv32_patch_branch(&code, storage, storage + 2));
    CHECK(storage[2] == 0xa5 && storage[3] == 0xa5);

    /* Children inherit the policy; save and restore keep it. */
    gpsp_rv32_code child;
    gpsp_rv32_code_child(&child, &code, storage, sizeof(storage));
    CHECK(child.short_branch && !child.branch_overflow);
    gpsp_rv32_code_mark mark = gpsp_rv32_code_save(&child);
    child.short_branch = false;
    gpsp_rv32_code_restore(&child, &mark);
    CHECK(child.short_branch);
    puts("Short branch policy and reach checks passed");
}

static void test_reference(const char *path)
{
    uint32_t storage[128];
    memset(storage, 0xcc, sizeof(storage));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    emit_reference_sequence(&code);
    size_t generated_size = (size_t)(code.ptr - code.start);

    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long file_size = ftell(file);
    CHECK(file_size >= 0 && (size_t)file_size == generated_size);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    uint8_t reference[sizeof(storage)];
    CHECK(fread(reference, 1, generated_size, file) == generated_size);
    CHECK(fclose(file) == 0);

    for (size_t i = 0; i < generated_size; i += 4)
    {
        if (memcmp((uint8_t *)storage + i, reference + i, 4))
        {
            fprintf(stderr, "instruction %zu: generated %08x, assembler %08x\n",
                    i / 4, read_word((uint8_t *)storage + i), read_word(reference + i));
            exit(1);
        }
    }
    printf("%zu RV32I/M/Zb instructions match the independent assembler\n",
            generated_size / 4);
}

#define REJECT_UNCHANGED(code, storage, expression) do { \
    uint8_t snapshot[sizeof(storage)]; \
    memcpy(snapshot, storage, sizeof(storage)); \
    gpsp_rv32_code saved = (code); \
    CHECK(!(expression)); \
    CHECK((code).ptr == saved.ptr && (code).compress == saved.compress && \
            (code).fixed_depth == saved.fixed_depth); \
    CHECK(memcmp(storage, snapshot, sizeof(storage)) == 0); \
} while (0)

static void test_operand_boundaries(void)
{
    uint32_t storage[8];
    memset(storage, 0x5a, sizeof(storage));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));

    REJECT_UNCHANGED(code, storage, gpsp_rv32_add(&code, 32, 1, 2));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_sub(&code, 1, 32, 2));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_mul(&code, 1, 2, 32));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_addi(&code, 1, 2, -2049));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_addi(&code, 1, 2, 2048));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_lw(&code, 1, 2, 2048));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_sw(&code, 1, 2, -2049));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_slli(&code, 1, 2, 32));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_srai(&code, 1, 2, 32));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_rori(&code, 1, 2, 32));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_bexti(&code, 1, 2, 32));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_shadd(&code, 0, 1, 2, 3));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_shadd(&code, 4, 1, 2, 3));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_lui(&code, 1, 0x100000));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_beq(&code, 1, 2, -4098));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_beq(&code, 1, 2, 4096));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_beq(&code, 1, 2, 3));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_jal(&code, 1, -1048578));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_jal(&code, 1, 1048576));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_jal(&code, 1, 5));

    CHECK(gpsp_rv32_beq(&code, 1, 2, -4096));
    CHECK(decode_b(read_word((uint8_t *)storage)) == -4096);
    CHECK(gpsp_rv32_beq(&code, 1, 2, 4094));
    CHECK(decode_b(read_word((uint8_t *)storage + 4)) == 4094);
    CHECK(gpsp_rv32_jal(&code, 1, -1048576));
    CHECK(decode_j(read_word((uint8_t *)storage + 8)) == -1048576);
    CHECK(gpsp_rv32_jal(&code, 1, 1048574));
    CHECK(decode_j(read_word((uint8_t *)storage + 12)) == 1048574);
    CHECK(gpsp_rv32_lui(&code, 1, 0xfffff));
    CHECK(gpsp_rv32_srai(&code, 1, 2, 31));

    gpsp_rv32_code_init(&code, storage, 4);
    CHECK(gpsp_rv32_add(&code, 1, 2, 3));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_add(&code, 1, 2, 3));
    gpsp_rv32_code_init(&code, (uint8_t *)storage + 1, sizeof(storage) - 1);
    REJECT_UNCHANGED(code, storage, gpsp_rv32_add(&code, 1, 2, 3));
}

static void test_patch_boundaries(void)
{
    uint32_t storage[1030];
    memset(storage, 0, sizeof(storage));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    uint8_t *first = code.ptr;
    CHECK(gpsp_rv32_beq(&code, 1, 2, 0));
    for (unsigned i = 0; i < 1023; i++)
        CHECK(gpsp_rv32_addi(&code, 0, 0, 0));
    uint8_t *last = code.ptr;
    CHECK(gpsp_rv32_bne(&code, 3, 4, 0));

    REJECT_UNCHANGED(code, storage,
            gpsp_rv32_patch_branch(&code, first, last));
    CHECK(gpsp_rv32_patch_branch(&code, first, last - 4));
    CHECK(decode_b(read_word(first)) == 4092);
    CHECK(gpsp_rv32_patch_branch(&code, last, first));
    CHECK(decode_b(read_word(last)) == -4096);

    REJECT_UNCHANGED(code, storage,
            gpsp_rv32_patch_branch(&code, first + 1, first));
    REJECT_UNCHANGED(code, storage,
            gpsp_rv32_patch_branch(&code, first + 4, first));
    REJECT_UNCHANGED(code, storage,
            gpsp_rv32_patch_branch(&code, first, first + 1));
    REJECT_UNCHANGED(code, storage,
            gpsp_rv32_patch_branch(&code, first, code.ptr + 4));
    REJECT_UNCHANGED(code, storage,
            gpsp_rv32_patch_branch(&code, code.ptr, first));
    REJECT_UNCHANGED(code, storage,
            gpsp_rv32_patch_branch(&code, first,
                    (uint8_t *)((uintptr_t)storage - 4)));

    uint32_t first_word = read_word(first);
    uint32_t reserved_branch = (first_word & ~(7u << 12)) | (2u << 12);
    memcpy(first, &reserved_branch, sizeof(reserved_branch));
    REJECT_UNCHANGED(code, storage,
            gpsp_rv32_patch_branch(&code, first, last - 4));
    memcpy(first, &first_word, sizeof(first_word));

    uint8_t *j_site = code.ptr;
    CHECK(gpsp_rv32_jal(&code, 1, 0));
    CHECK(gpsp_rv32_patch_branch(&code, j_site, first));
    CHECK(decode_j(read_word(j_site)) == -(int32_t)(j_site - first));
    CHECK(gpsp_rv32_patch_branch(&code, j_site, code.ptr));
    CHECK(decode_j(read_word(j_site)) == 4);
}

static unsigned hook_count;

static void count_hook(const gpsp_rv32_code *code)
{
    (void)code;
    hook_count++;
}

/* Compressed code puts instructions on two-byte boundaries. */
static void test_two_byte_positions(void)
{
    uint32_t aligned[4], storage[8];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, aligned, sizeof(aligned));
    CHECK(gpsp_rv32_addi(&code, 10, 10, 1));

    memset(storage, 0, sizeof(storage));
    uint8_t *base = (uint8_t *)storage + 2;
    gpsp_rv32_code_init(&code, base, sizeof(storage) - 2);
    CHECK(gpsp_rv32_addi(&code, 10, 10, 1));
    CHECK(read_word(base) == aligned[0] && code.ptr == base + 4);
    uint8_t *site = code.ptr;
    CHECK(gpsp_rv32_beq(&code, 1, 2, 0));
    CHECK(gpsp_rv32_addi(&code, 0, 0, 0));
    CHECK(gpsp_rv32_patch_branch(&code, site, code.ptr));
    CHECK(decode_b(read_word(site)) == 8);
    CHECK(gpsp_rv32_patch_branch(&code, site, base + 2));
    CHECK(decode_b(read_word(site)) == -2);

    /* Padding requires compression outside a fixed span. */
    code.compress = false;
    REJECT_UNCHANGED(code, storage, gpsp_rv32_align4(&code));
    code.compress = true;
    CHECK(gpsp_rv32_fixed_begin(&code));
    REJECT_UNCHANGED(code, storage, gpsp_rv32_align4(&code));
    CHECK(gpsp_rv32_fixed_end(&code));

    /* Padding reaches the next four-byte boundary with one c.nop. */
    hook_count = 0;
    gpsp_rv32_test_emit_hook = count_hook;
    CHECK(gpsp_rv32_align4(&code));
    CHECK(code.ptr == base + 14 && ((uintptr_t)code.ptr & 3) == 0);
    CHECK(hook_count == 1);
    uint16_t half;
    memcpy(&half, base + 12, sizeof(half));
    CHECK(half == 0x0001);
    /* An aligned cursor needs no padding under any policy. */
    code.compress = false;
    CHECK(gpsp_rv32_align4(&code) && code.ptr == base + 14);
    CHECK(hook_count == 1);
    gpsp_rv32_test_emit_hook = NULL;
    code.compress = true;
    code.ptr = base + 11;
    REJECT_UNCHANGED(code, storage, gpsp_rv32_align4(&code));
    gpsp_rv32_code_init(&code, base, 1);
    code.compress = true;
    REJECT_UNCHANGED(code, storage, gpsp_rv32_align4(&code));
}

/* Memory for loads and stores: an eight-byte window at window_base, and a
   byte derived from the address elsewhere. */
static const uint32_t window_base = 0x20000000;
static uint8_t window[8];

static uint8_t read_byte(uint32_t address)
{
    return address - window_base < sizeof(window) ?
            window[address - window_base] : (uint8_t)((address * 0x9e3779b1u) >> 24);
}

/* Evaluate the decoded word using the original instruction length. A
   store reports rd = 32 + width, the address in target and the stored bits
   in value, and writes the window. */
static void evaluate(uint32_t word, const uint32_t regs[32], uint32_t pc,
        unsigned length, unsigned *rd, uint32_t *value, uint32_t *target)
{
    unsigned opcode = word & 0x7f, funct3 = (word >> 12) & 7;
    uint32_t a = regs[(word >> 15) & 31], b = regs[(word >> 20) & 31];
    int32_t imm = (int32_t)word >> 20;
    *rd = (word >> 7) & 31;
    *target = 0;
    unsigned funct7 = word >> 25;
    if (opcode == 0x13 && funct3 == 0)
        *value = a + (uint32_t)imm;
    else if (opcode == 0x13 && funct3 == 1 && !funct7)
        *value = a << (imm & 31);
    else if (opcode == 0x13 && funct3 == 4)
        *value = a ^ (uint32_t)imm;
    else if (opcode == 0x13 && funct3 == 5 && (!funct7 || funct7 == 0x20))
        *value = funct7 ? (uint32_t)((int32_t)a >> (imm & 31)) : a >> (imm & 31);
    else if (opcode == 0x13 && funct3 == 7)
        *value = a & (uint32_t)imm;
    else if (opcode == 0x33 && funct3 == 0 && !funct7)
        *value = a + b;
    else if (opcode == 0x33 && funct3 == 0 && funct7 == 0x20)
        *value = a - b;
    else if (opcode == 0x33 && !funct7 && funct3 >= 4)
        *value = funct3 == 4 ? a ^ b : funct3 == 6 ? a | b : a & b;
    else if (opcode == 0x33 && funct7 == 1 && funct3 == 0)
        *value = a * b;
    else if (opcode == 0x03 && funct3 != 3 && funct3 < 6)
    {
        unsigned bytes = 1u << (funct3 & 3);
        uint32_t address = a + (uint32_t)imm, data = 0;
        for (unsigned i = 0; i < bytes; i++)
            data |= (uint32_t)read_byte(address + i) << (8 * i);
        if (funct3 < 2)
        {
            unsigned shift = 32 - 8 * bytes;
            data = (uint32_t)((int32_t)(data << shift) >> shift);
        }
        *value = data;
    }
    else if (opcode == 0x23 && funct3 <= 2)
    {
        unsigned bytes = 1u << funct3;
        *rd = 32 + bytes;             /* no register written */
        *value = bytes == 4 ? b : b & ((1u << (8 * bytes)) - 1);
        *target = a + (uint32_t)(((int32_t)word >> 25 << 5) |
                ((word >> 7) & 31));
        for (unsigned i = 0; i < bytes; i++)
            if (*target + i - window_base < sizeof(window))
                window[*target + i - window_base] = (uint8_t)(b >> (8 * i));
        return;
    }
    else if (opcode == 0x37)
        *value = word & 0xfffff000;
    else if (opcode == 0x67)
    {
        *value = pc + length;
        *target = (a + (uint32_t)imm) & ~1u;
    }
    else
    {
        fprintf(stderr, "evaluate: unsupported word %08x\n", word);
        exit(1);
    }
    if (!*rd)
        *value = 0;
}

typedef bool (*emit_fn)(gpsp_rv32_code *, unsigned, unsigned, int);

static bool emit_addi(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_addi(c, rd, rs1, v);
}
static bool emit_add(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_add(c, rd, rs1, (unsigned)v);
}
static bool emit_slli(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_slli(c, rd, rs1, (unsigned)v);
}
static bool emit_lui(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    (void)rs1;
    return gpsp_rv32_lui(c, rd, (uint32_t)v);
}
static bool emit_jalr(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_jalr(c, rd, rs1, v);
}
static bool emit_sub(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_sub(c, rd, rs1, (unsigned)v);
}
static bool emit_xor(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_xor(c, rd, rs1, (unsigned)v);
}
static bool emit_or(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_or(c, rd, rs1, (unsigned)v);
}
static bool emit_and(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_and(c, rd, rs1, (unsigned)v);
}
static bool emit_srli(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_srli(c, rd, rs1, (unsigned)v);
}
static bool emit_srai(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_srai(c, rd, rs1, (unsigned)v);
}
static bool emit_andi(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_andi(c, rd, rs1, v);
}
static bool emit_sw(gpsp_rv32_code *c, unsigned rs2, unsigned rs1, int v)
{
    return gpsp_rv32_sw(c, rs2, rs1, v);
}
static bool emit_lw(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_lw(c, rd, rs1, v);
}
static bool emit_beq(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_beq(c, rd, rs1, v);
}
static bool emit_lb(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_lb(c, rd, rs1, v);
}
static bool emit_lbu(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_lbu(c, rd, rs1, v);
}
static bool emit_lh(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_lh(c, rd, rs1, v);
}
static bool emit_lhu(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_lhu(c, rd, rs1, v);
}
static bool emit_sb(gpsp_rv32_code *c, unsigned rs2, unsigned rs1, int v)
{
    return gpsp_rv32_sb(c, rs2, rs1, v);
}
static bool emit_sh(gpsp_rv32_code *c, unsigned rs2, unsigned rs1, int v)
{
    return gpsp_rv32_sh(c, rs2, rs1, v);
}
static bool emit_xori(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_xori(c, rd, rs1, v);
}
static bool emit_mul(gpsp_rv32_code *c, unsigned rd, unsigned rs1, int v)
{
    return gpsp_rv32_mul(c, rd, rs1, (unsigned)v);
}

/* Compare results, allowing JALR's link to follow the instruction length. */
static unsigned check_form(emit_fn emit, unsigned rd, unsigned rs1, int v)
{
    uint32_t full[2], packed[2];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, full, sizeof(full));
    code.compress = false;
    if (!emit(&code, rd, rs1, v))
        return 0;
    CHECK(code.ptr == code.start + 4);
    gpsp_rv32_code_init(&code, packed, sizeof(packed));
    code.compress = true;
    CHECK(emit(&code, rd, rs1, v));
    unsigned length = (unsigned)(code.ptr - code.start);
    uint32_t word = 0;
    CHECK(rv32_rvc_fetch(packed, length, 0, &word) == length);
    if (length == 4)
    {
        CHECK(word == full[0]);
        return 4;
    }
    uint32_t regs[32];
    for (unsigned trial = 0; trial < 8; trial++)
    {
        for (unsigned r = 0; r < 32; r++)
            regs[r] = r ? (uint32_t)(r * 0x9e3779b9u + trial * 0x7f4a7c15u) : 0;
        unsigned rd_a, rd_b;
        uint32_t value_a, value_b, target_a, target_b;
        const uint32_t pc = 0x30000002;
        evaluate(full[0], regs, pc, 4, &rd_a, &value_a, &target_a);
        evaluate(word, regs, pc, length, &rd_b, &value_b, &target_b);
        CHECK(rd_a == rd_b && target_a == target_b);
        if ((full[0] & 0x7f) == 0x67 && rd_a)
            CHECK(value_a == pc + 4 && value_b == pc + length);
        else
            CHECK(value_a == value_b);
    }
    /* A fixed span keeps the full word. */
    gpsp_rv32_code_init(&code, packed, sizeof(packed));
    code.compress = true;
    CHECK(gpsp_rv32_fixed_begin(&code) && emit(&code, rd, rs1, v));
    CHECK(code.ptr == code.start + 4 && packed[0] == full[0]);
    return 2;
}

static void test_compressed_forms(void)
{
    static const unsigned regs[] = {0, 1, 2, 5, 8, 10, 15, 18, 31};
    static const int imms[] = {-2048, -33, -32, -1, 0, 1, 31, 32, 2047};
    unsigned packed = 0, total = 0;
    for (unsigned i = 0; i < 9; i++)
        for (unsigned j = 0; j < 9; j++)
        {
            for (unsigned k = 0; k < 9; k++)
            {
                unsigned n;
                n = check_form(emit_addi, regs[i], regs[j], imms[k]);
                total += !!n;
                packed += n == 2;
                n = check_form(emit_add, regs[i], regs[j], (int)regs[k]);
                total += !!n;
                packed += n == 2;
                static const emit_fn ops[] = {emit_sub, emit_xor, emit_or, emit_and};
                for (unsigned op = 0; op < 4; op++)
                {
                    n = check_form(ops[op], regs[i], regs[j], (int)regs[k]);
                    total += !!n;
                    packed += n == 2;
                }
                n = check_form(emit_andi, regs[i], regs[j], imms[k]);
                total += !!n;
                packed += n == 2;
            }
            static const int shifts[] = {0, 1, 31};
            static const emit_fn shift_ops[] = {emit_slli, emit_srli, emit_srai};
            for (unsigned op = 0; op < 3; op++)
                for (unsigned k = 0; k < 3; k++)
                {
                    unsigned n = check_form(shift_ops[op], regs[i], regs[j],
                            shifts[k]);
                    total += !!n;
                    packed += n == 2;
                }
            static const int offsets[] = {0, 4, -4};
            for (unsigned k = 0; k < 3; k++)
            {
                unsigned n = check_form(emit_jalr, regs[i], regs[j], offsets[k]);
                total += !!n;
                packed += n == 2;
            }
            static const int memory[] = {0, 4, 124, 128, 2, -4};
            for (unsigned k = 0; k < 6; k++)
            {
                unsigned n = check_form(emit_lw, regs[i], regs[j], memory[k]);
                total += !!n;
                packed += n == 2;
                n = check_form(emit_sw, regs[i], regs[j], memory[k]);
                total += !!n;
                packed += n == 2;
            }
            /* Byte and halfword accesses, NOT, zero extension and MUL. */
            static const int small[] = {0, 1, 2, 3, 4, -1};
            static const emit_fn narrow[] = {
                emit_lb, emit_lbu, emit_lh, emit_lhu, emit_sb, emit_sh,
            };
            for (unsigned op = 0; op < 6; op++)
                for (unsigned k = 0; k < 6; k++)
                {
                    unsigned n = check_form(narrow[op], regs[i], regs[j], small[k]);
                    total += !!n;
                    packed += n == 2;
                }
            static const int logic[] = {-1, 1, 255, 254};
            for (unsigned k = 0; k < 4; k++)
            {
                unsigned n = check_form(emit_xori, regs[i], regs[j], logic[k]);
                total += !!n;
                packed += n == 2;
                n = check_form(emit_andi, regs[i], regs[j], logic[k]);
                total += !!n;
                packed += n == 2;
            }
            for (unsigned k = 0; k < 9; k++)
            {
                unsigned n = check_form(emit_mul, regs[i], regs[j], (int)regs[k]);
                total += !!n;
                packed += n == 2;
            }
            CHECK(check_form(emit_beq, regs[i], regs[j], 8) == 4);
        }
    static const int uppers[] = {0, 1, 31, 32, 0xfffdf, 0xfffe0, 0xfffff};
    for (unsigned i = 0; i < 9; i++)
        for (unsigned k = 0; k < 7; k++)
        {
            unsigned n = check_form(emit_lui, regs[i], 0, uppers[k]);
            total += !!n;
            packed += n == 2;
        }
    CHECK(total && packed);

    /* Selected compressed forms and boundary fallbacks. */
    CHECK(check_form(emit_addi, 0, 0, 0) == 2);       /* c.nop */
    CHECK(check_form(emit_addi, 10, 0, -32) == 2);    /* c.li */
    CHECK(check_form(emit_addi, 10, 0, 32) == 4);
    CHECK(check_form(emit_addi, 10, 10, 31) == 2);    /* c.addi */
    CHECK(check_form(emit_addi, 10, 10, 0) == 2);     /* c.mv a0, a0 */
    CHECK(check_form(emit_addi, 10, 11, 0) == 2);     /* c.mv */
    CHECK(check_form(emit_addi, 10, 11, 1) == 4);
    CHECK(check_form(emit_addi, 0, 10, 0) == 4);
    CHECK(check_form(emit_add, 10, 10, 11) == 2);     /* c.add */
    CHECK(check_form(emit_add, 10, 11, 10) == 2);
    CHECK(check_form(emit_add, 10, 0, 11) == 2);      /* c.mv */
    CHECK(check_form(emit_add, 10, 11, 12) == 4);
    CHECK(check_form(emit_slli, 10, 10, 3) == 2);     /* c.slli */
    CHECK(check_form(emit_slli, 10, 11, 3) == 4);
    CHECK(check_form(emit_slli, 10, 10, 0) == 4);
    CHECK(check_form(emit_lui, 10, 0, 31) == 2);      /* c.lui */
    CHECK(check_form(emit_lui, 10, 0, 0xfffe0) == 2);
    CHECK(check_form(emit_lui, 10, 0, 32) == 4);
    CHECK(check_form(emit_lui, 2, 0, 1) == 4);
    CHECK(check_form(emit_jalr, 0, 1, 0) == 2);       /* c.jr ra */
    CHECK(check_form(emit_jalr, 1, 6, 0) == 2);       /* c.jalr */
    CHECK(check_form(emit_jalr, 5, 6, 0) == 4);
    CHECK(check_form(emit_jalr, 0, 1, 4) == 4);
    /* With two bytes left, a 16-bit form fits and a 32-bit one fails
       without changes. */
    {
        uint32_t storage[1];
        memset(storage, 0x5a, sizeof(storage));
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, storage, 2);
        code.compress = true;
        REJECT_UNCHANGED(code, storage, gpsp_rv32_sub(&code, 8, 15, 8));
        REJECT_UNCHANGED(code, storage, gpsp_rv32_lw(&code, 8, 9, 128));
        CHECK(gpsp_rv32_sub(&code, 8, 8, 15) && code.ptr == code.start + 2);
        REJECT_UNCHANGED(code, storage, gpsp_rv32_lw(&code, 8, 9, 0));
    }

    /* Forms limited to x8-x15. */
    CHECK(check_form(emit_sub, 8, 8, 15) == 2);       /* c.sub */
    CHECK(check_form(emit_sub, 8, 15, 8) == 4);       /* not commutative */
    CHECK(check_form(emit_sub, 7, 7, 8) == 4);
    CHECK(check_form(emit_and, 15, 8, 15) == 2);      /* c.and, swapped */
    CHECK(check_form(emit_or, 9, 9, 16) == 4);
    CHECK(check_form(emit_srai, 9, 9, 31) == 2);      /* c.srai */
    CHECK(check_form(emit_srli, 16, 16, 1) == 4);
    CHECK(check_form(emit_andi, 10, 10, -32) == 2);   /* c.andi */
    CHECK(check_form(emit_andi, 10, 10, 32) == 4);
    CHECK(check_form(emit_andi, 10, 11, 1) == 4);
    CHECK(check_form(emit_lw, 10, 8, 124) == 2);      /* c.lw */
    CHECK(check_form(emit_lw, 10, 8, 128) == 4);
    CHECK(check_form(emit_lw, 10, 8, 2) == 4);
    CHECK(check_form(emit_lw, 10, 8, -4) == 4);
    CHECK(check_form(emit_lw, 16, 8, 0) == 4);
    CHECK(check_form(emit_sw, 15, 9, 4) == 2);        /* c.sw */
    CHECK(check_form(emit_sw, 31, 9, 4) == 4);

    /* Zcb forms, also limited to x8-x15. */
    const unsigned zcb = GPSP_RV32_USE_ZCB ? 2 : 4;
    CHECK(check_form(emit_lbu, 10, 8, 3) == zcb);     /* c.lbu */
    CHECK(check_form(emit_lbu, 10, 8, 4) == 4);
    CHECK(check_form(emit_lbu, 10, 8, -1) == 4);
    CHECK(check_form(emit_lbu, 5, 8, 0) == 4);
    CHECK(check_form(emit_lb, 10, 8, 0) == 4);        /* no c.lb */
    CHECK(check_form(emit_lhu, 15, 9, 2) == zcb);     /* c.lhu */
    CHECK(check_form(emit_lhu, 15, 9, 1) == 4);
    CHECK(check_form(emit_lh, 8, 15, 2) == zcb);      /* c.lh */
    CHECK(check_form(emit_lh, 8, 15, 4) == 4);
    CHECK(check_form(emit_sb, 15, 8, 3) == zcb);      /* c.sb */
    CHECK(check_form(emit_sb, 15, 18, 0) == 4);
    CHECK(check_form(emit_sh, 9, 10, 2) == zcb);      /* c.sh */
    CHECK(check_form(emit_sh, 9, 10, 3) == 4);
    CHECK(check_form(emit_andi, 11, 11, 255) == zcb); /* c.zext.b */
    CHECK(check_form(emit_andi, 11, 12, 255) == 4);
    CHECK(check_form(emit_andi, 5, 5, 255) == 4);
    CHECK(check_form(emit_xori, 12, 12, -1) == zcb);  /* c.not */
    CHECK(check_form(emit_xori, 12, 12, 1) == 4);
    CHECK(check_form(emit_mul, 10, 10, 11) == zcb);   /* c.mul */
    CHECK(check_form(emit_mul, 10, 11, 10) == zcb);   /* swapped */
    CHECK(check_form(emit_mul, 10, 11, 12) == 4);
    CHECK(check_form(emit_mul, 5, 5, 10) == 4);
    printf("%u of %u encoder cases use 16-bit forms\n", packed,
            total);
}

/* Narrow loads extend as their width says; narrow stores write only their
   bytes. Both lengths of each instruction give the same results. */
static void test_narrow_access(void)
{
    enum { BASE = 8, DATA = 9 };
    static const struct
    {
        emit_fn emit;
        int offset;
        uint8_t bytes[2];
        uint32_t expected;
    } loads[] = {
        {emit_lbu, 1, {0x80, 0}, 0x80}, {emit_lb, 1, {0x80, 0}, 0xffffff80},
        {emit_lbu, 3, {0xff, 0}, 0xff}, {emit_lb, 3, {0x7f, 0}, 0x7f},
        {emit_lhu, 2, {0x00, 0x80}, 0x8000}, {emit_lh, 2, {0x00, 0x80}, 0xffff8000},
        {emit_lhu, 0, {0xff, 0xff}, 0xffff}, {emit_lh, 0, {0xff, 0xff}, 0xffffffff},
    };
    for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++)
        for (unsigned compress = 0; compress < 2; compress++)
        {
            uint32_t words[2], regs[32] = {0}, word = 0, value, target;
            unsigned rd;
            gpsp_rv32_code code;
            gpsp_rv32_code_init(&code, words, sizeof(words));
            code.compress = compress;
            CHECK(loads[i].emit(&code, DATA, BASE, loads[i].offset));
            unsigned length = (unsigned)(code.ptr - code.start);
            CHECK(rv32_rvc_fetch(words, length, 0, &word) == length);
            memset(window, 0xa5, sizeof(window));
            memcpy(window + loads[i].offset, loads[i].bytes, 2);
            regs[BASE] = window_base;
            evaluate(word, regs, 0, length, &rd, &value, &target);
            CHECK(rd == DATA && value == loads[i].expected);
        }

    static const struct
    {
        emit_fn emit;
        int offset;
        uint8_t after[8];
    } stores[] = {
        {emit_sb, 1, {0xa5, 0x78, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5}},
        {emit_sb, 3, {0xa5, 0xa5, 0xa5, 0x78, 0xa5, 0xa5, 0xa5, 0xa5}},
        {emit_sh, 2, {0xa5, 0xa5, 0x78, 0x56, 0xa5, 0xa5, 0xa5, 0xa5}},
        {emit_sh, 0, {0x78, 0x56, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5}},
    };
    for (unsigned i = 0; i < sizeof(stores) / sizeof(stores[0]); i++)
        for (unsigned compress = 0; compress < 2; compress++)
        {
            uint32_t words[2], regs[32] = {0}, word = 0, value, target;
            unsigned rd;
            gpsp_rv32_code code;
            gpsp_rv32_code_init(&code, words, sizeof(words));
            code.compress = compress;
            CHECK(stores[i].emit(&code, DATA, BASE, stores[i].offset));
            unsigned length = (unsigned)(code.ptr - code.start);
            CHECK(rv32_rvc_fetch(words, length, 0, &word) == length);
            memset(window, 0xa5, sizeof(window));
            regs[BASE] = window_base;
            regs[DATA] = 0x12345678;
            evaluate(word, regs, 0, length, &rd, &value, &target);
            CHECK(!memcmp(window, stores[i].after, sizeof(window)));
        }
    puts("Narrow load extension and store widths checked");
}

/* Matches zcb.S. */
static void emit_zcb_sequence(gpsp_rv32_code *code)
{
    enum { A0 = 10, A1, A2, A4 = 14, A5, S0 = 8, S1, T0 = 5 };
#define EMIT(name, ...) CHECK(gpsp_rv32_##name(code, __VA_ARGS__))
    EMIT(lbu, A0, A1, 0);
    EMIT(lbu, S0, A5, 3);
    EMIT(lbu, A0, A1, 4);
    EMIT(lbu, T0, A1, 0);
    EMIT(lhu, A0, A1, 0);
    EMIT(lhu, S1, A5, 2);
    EMIT(lhu, A0, A1, 1);
    EMIT(lh, A0, A1, 0);
    EMIT(lh, A4, S0, 2);
    EMIT(lh, A0, A1, 4);
    EMIT(sb, A0, A1, 0);
    EMIT(sb, S0, A5, 3);
    EMIT(sb, A0, A1, -1);
    EMIT(sh, A0, A1, 0);
    EMIT(sh, A5, S1, 2);
    EMIT(sh, A0, A1, 3);
    EMIT(andi, A0, A0, 255);
    EMIT(andi, S1, S1, 255);
    EMIT(andi, A0, A1, 255);
    EMIT(xori, A5, A5, -1);
    EMIT(xori, A5, A5, 1);
    EMIT(mul, A0, A0, A1);
    EMIT(mul, A0, A1, A0);
    EMIT(mul, A0, A1, A2);
    EMIT(mul, T0, T0, A0);
    EMIT(lbu, A1, A2, 1);
#undef EMIT
}

static void test_zcb_reference(const char *path)
{
    uint32_t storage[32];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    code.compress = true;
    emit_zcb_sequence(&code);
    size_t size = (size_t)(code.ptr - code.start);
    if (!GPSP_RV32_USE_ZCB)
    {
        CHECK(size == 26 * 4);
        puts("Zcb off: byte, halfword, NOT and MUL stay 32-bit");
        return;
    }
    uint8_t reference[sizeof(storage)];
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    size_t read = fread(reference, 1, sizeof(reference), file);
    CHECK(fclose(file) == 0);
    CHECK(read == size && !memcmp(storage, reference, size));

    /* With two bytes left, a Zcb form fits; others fail without changes. */
    uint32_t small[1];
    memset(small, 0x5a, sizeof(small));
    gpsp_rv32_code_init(&code, small, 2);
    code.compress = true;
    REJECT_UNCHANGED(code, small, gpsp_rv32_lbu(&code, 10, 11, 4));
    REJECT_UNCHANGED(code, small, gpsp_rv32_mul(&code, 10, 11, 12));
    CHECK(gpsp_rv32_lbu(&code, 10, 11, 3) && code.ptr == code.start + 2);
    REJECT_UNCHANGED(code, small, gpsp_rv32_sh(&code, 10, 11, 0));
    printf("%zu bytes of Zcb forms match the assembler\n", size);
}

static void test_policy(void)
{
    uint32_t storage[4], child_storage[4];
    gpsp_rv32_code code, child;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    CHECK(code.compress == gpsp_rv32_test_compress && code.fixed_depth == 0);
    CHECK(gpsp_rv32_code_may_compress(&code) == gpsp_rv32_test_compress);
    CHECK(!gpsp_rv32_fixed_end(&code));
    CHECK(code.fixed_depth == 0);

    /* Fixed spans nest and turn compression off until the outer one ends. */
    code.compress = true;
    CHECK(gpsp_rv32_code_may_compress(&code));
    CHECK(gpsp_rv32_fixed_begin(&code) && gpsp_rv32_fixed_begin(&code));
    CHECK(!gpsp_rv32_code_may_compress(&code));
    CHECK(gpsp_rv32_fixed_end(&code));
    CHECK(!gpsp_rv32_code_may_compress(&code));
    CHECK(gpsp_rv32_fixed_end(&code));
    CHECK(gpsp_rv32_code_may_compress(&code));
    code.fixed_depth = UINT8_MAX;
    CHECK(!gpsp_rv32_fixed_begin(&code) && code.fixed_depth == UINT8_MAX);
    code.fixed_depth = 0;

    /* A child takes the parent policy; its changes stay in the child. */
    CHECK(gpsp_rv32_fixed_begin(&code));
    gpsp_rv32_code_child(&child, &code, child_storage, sizeof(child_storage));
    CHECK(child.start == (uint8_t *)child_storage &&
            child.ptr == child.start && child.end == child.start + 16);
    CHECK(child.compress && child.fixed_depth == 1);
    CHECK(gpsp_rv32_fixed_end(&child) && gpsp_rv32_code_may_compress(&child));
    CHECK(gpsp_rv32_addi(&child, 1, 1, 1));
    CHECK(code.ptr == code.start && code.fixed_depth == 1);
    CHECK(gpsp_rv32_fixed_end(&code));
    gpsp_rv32_code_child(&child, NULL, child_storage, sizeof(child_storage));
    CHECK(child.compress == gpsp_rv32_test_compress && child.fixed_depth == 0);

    /* Restore brings back the cursor and the policy of the mark. */
    gpsp_rv32_code_mark mark = gpsp_rv32_code_save(&code);
    CHECK(gpsp_rv32_addi(&code, 1, 1, 1) && gpsp_rv32_fixed_begin(&code));
    code.compress = false;
    gpsp_rv32_code_restore(&code, &mark);
    CHECK(code.ptr == code.start && code.compress && code.fixed_depth == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 1 || argc == 5);
    /* Exact sizes below assume 32-bit instructions. */
    gpsp_rv32_test_compress = false;
    if (argc == 5)
    {
        test_reference(argv[1]);
        test_compressed_reference(argv[2]);
        test_short_branch_reference(argv[3]);
        test_zcb_reference(argv[4]);
    }
    test_short_branch_policy();
    test_narrow_access();
    test_operand_boundaries();
    test_patch_boundaries();
    test_policy();
    test_two_byte_positions();
    test_compressed_forms();
    puts("RV32 codegen boundary and patch checks passed");
    return 0;
}
