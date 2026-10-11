/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "rv32_arm_memory.h"
#include "rv32_arm_flow_full.h"
#include "rv32_arm_system.h"
#include "rv32_register_map.h"
#include "rv32_stub.h"
#include "rv32_thumb_memory.h"
#include "../../rv32-golden.h"

#define CODE_SIZE 16384
#define PC 0x08000000u
#define ARM_IMM(pre, up, byte, wb, load, rn, rd, imm) \
    (0xe4000000u | ((uint32_t)(pre) << 24) | \
            ((uint32_t)(up) << 23) | ((uint32_t)(byte) << 22) | \
            ((uint32_t)(wb) << 21) | ((uint32_t)(load) << 20) | \
            ((uint32_t)(rn) << 16) | ((uint32_t)(rd) << 12) | (imm))
enum { OAM_UPDATED = 25 };

/* Page-aligned RAM: native loads use offset 0, which Zcb forms can encode. */
EXT_RAM_BSS_ATTR uint8_t ewram[0x80000] __attribute__((aligned(4096)));
EXT_RAM_BSS_ATTR uint8_t iwram[0x10000] __attribute__((aligned(4096)));
EXT_RAM_BSS_ATTR uint8_t rom_page[0x8000];
EXT_RAM_BSS_ATTR uint8_t vram[1024 * 96] __attribute__((aligned(4)));
EXT_RAM_BSS_ATTR uint8_t *memory_map_read[8 * 1024];
uint32_t gamepak_size;
uint8_t ws_cyc_seq[16][2];
uint8_t ws_cyc_nseq[16][2];
const uint32_t def_seq_cycles[16][2] = {{0}};
uint16_t oam_ram[512];
uint16_t palette_ram[512] __attribute__((aligned(4)));
uint16_t palette_ram_converted[512] __attribute__((aligned(4)));
uint32_t palette_ram_expanded[512];
uint32_t idle_loop_target_pc = UINT32_MAX;
/* Shared flow exits probe this cache; the tests keep it empty. */
uint32_t gpsp_rv32_branch_cache[1024 * 2];
uint16_t io_registers[512] __attribute__((aligned(4)));
/* Native I/O masks; zero keeps a register on the C path. */
uint32_t gpsp_rv32_io_read_mask16[512];
uint32_t gpsp_rv32_io_read_mask32[256];
uint32_t gpsp_rv32_io_write_mask16[512];
uint32_t reg[64];
uint32_t reg_mode[7][7];
uint32_t spsr[6];
const uint32_t cpu_modes[16] = {
    0x00, 0x12, 0x11, 0x13, 0x16, 0x16, 0x16, 0x14,
    0x16, 0x16, 0x16, 0x15, 0x16, 0x16, 0x16, 0x10,
};

void set_cpu_mode(uint32_t new_mode)
{
    uint32_t old_mode = reg[17];
    if (new_mode == old_mode)
        return;
    if (old_mode == 0x12)
        for (unsigned i = 8; i < 13; i++)
            reg_mode[2][i - 8] = reg[i];
    else if (new_mode == 0x12)
        for (unsigned i = 8; i < 13; i++)
            reg_mode[0][i - 8] = reg[i];
    reg_mode[old_mode & 15][5] = reg[13];
    reg_mode[old_mode & 15][6] = reg[14];
    if (new_mode == 0x12)
        for (unsigned i = 8; i < 13; i++)
            reg[i] = reg_mode[2][i - 8];
    else if (old_mode == 0x12)
        for (unsigned i = 8; i < 13; i++)
            reg[i] = reg_mode[0][i - 8];
    reg[13] = reg_mode[new_mode & 15][5];
    reg[14] = reg_mode[new_mode & 15][6];
    reg[17] = new_mode;
}

static unsigned slow_reads;
static unsigned slow_writes;

/* These fixtures must stay on native paths or the memory helper. */
void gpsp_rv32_interpreter_step_stub(void)
{
    abort();
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_arm_system(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
}

static uint8_t *mapped(uint32_t address)
{
    unsigned region = address >> 24;
    if (region == 2)
        return &ewram[address & 0x3ffff];
    if (region == 3)
        return &iwram[0x8000 + (address & 0x7fff)];
    if (region == 6)
    {
        uint32_t offset = address & 0x1ffff;
        return &vram[offset >= 0x18000 ? offset - 0x8000 : offset];
    }
    if (region >= 8 && region <= 12 &&
        (address & 0x1ffffffu) < gamepak_size)
    {
        uint8_t *page = memory_map_read[address >> 15];
        return page ? page + (address & 0x7fff) : NULL;
    }
    return NULL;
}

uint32_t read_memory8(uint32_t address)
{
    slow_reads++;
    uint8_t *p = mapped(address);
    return p ? *p : 0xa5;
}

uint32_t read_memory8s(uint32_t address)
{
    return (uint32_t)(int32_t)(int8_t)read_memory8(address);
}

uint32_t read_memory16(uint32_t address)
{
    slow_reads++;
    uint8_t *p = mapped(address & ~1u);
    if (!p)
        return 0x1234;
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return (address & 1) ? ((value >> 8) | ((uint32_t)value << 24)) : value;
}

uint16_t read_memory16_signed(uint32_t address)
{
    return (address & 1) ? (int8_t)read_memory8(address) :
            (uint16_t)read_memory16(address);
}

uint32_t read_memory16s(uint32_t address)
{
    return (uint32_t)(int32_t)(int16_t)read_memory16_signed(address);
}

uint32_t read_memory32(uint32_t address)
{
    slow_reads++;
    uint8_t *p = mapped(address & ~3u);
    if (!p)
        return 0x11223344;
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    unsigned shift = (address & 3) * 8;
    return shift ? (value >> shift) | (value << (32 - shift)) : value;
}

uint8_t write_memory8(uint32_t address, uint8_t value)
{
    slow_writes++;
    uint8_t *p = mapped(address);
    if (p)
        *p = value;
    return p ? 0 : 1;
}

uint8_t write_memory16(uint32_t address, uint16_t value)
{
    slow_writes++;
    uint8_t *p = mapped(address & ~1u);
    if (p)
        memcpy(p, &value, sizeof(value));
    return p ? 0 : 1;
}

uint8_t write_memory32(uint32_t address, uint32_t value)
{
    slow_writes++;
    uint8_t *p = mapped(address & ~3u);
    if (p)
        memcpy(p, &value, sizeof(value));
    return p ? 0 : 1;
}

/* No dynamic immediates here: every code change raises SMC. */
bool gpsp_rv32_code_write_absorbed(uint32_t address, unsigned bytes,
        uint32_t before, uint32_t after)
{
    return false;
}

static void init_regs(uint32_t regs[64])
{
    memset(regs, 0, 64 * sizeof(regs[0]));
    regs[15] = PC;
    regs[16] = 0x1f;
    regs[17] = 0x1f;
}

static int sync_code(void *memory)
{
    esp_err_t err = esp_cache_msync(memory, CODE_SIZE,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return 0;
    err = esp_cache_msync(memory, CODE_SIZE,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return 0;
    __asm__ volatile("fence.i" ::: "memory");
    return 1;
}

/* Cases added after a golden base was saved stay out of the dump. */
static bool golden_quiet;

static size_t emit(void *memory, uint32_t opcode, int pc_exit)
{
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, memory, CODE_SIZE);
    gpsp_rv32_arm_memory_result result = gpsp_rv32_emit_arm_memory(
            &code, opcode, PC);
    if (result != (pc_exit ? GPSP_RV32_ARM_MEMORY_PC_EXIT :
            GPSP_RV32_ARM_MEMORY_EMITTED))
    {
        printf("emit failed: %08lx result=%d\n", (unsigned long)opcode, result);
        return 0;
    }
    if ((pc_exit && !gpsp_rv32_addi(&code, RV_PC, A0, 0)) ||
        (!pc_exit && !gpsp_rv32_addi(&code, RV_PC, RV_PC, 4)) ||
            !gpsp_rv32_addi(&code, RV_BUDGET, RV_BUDGET, -2) ||
            !gpsp_rv32_jalr(&code, X0, RA, 0))
        return 0;
    if (!golden_quiet)
        rv32_golden_dump("memory", memory, CODE_SIZE, memory, code.ptr);
    return sync_code(memory) ? (size_t)(code.ptr - code.start) : 0;
}

static int run(void *memory, uint32_t opcode, uint32_t regs[64],
        uint32_t *remaining, int pc_exit)
{
    if (!emit(memory, opcode, pc_exit))
        return 0;
    *remaining = gpsp_rv32_run_block(regs, 100, memory);
    return 1;
}

static size_t emitted_bytes(uint32_t opcode)
{
    uint32_t words[256];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    code.compress = false;  /* sizes of 32-bit instructions */
    if (gpsp_rv32_emit_arm_memory(&code, opcode, PC) !=
        GPSP_RV32_ARM_MEMORY_EMITTED)
        return 0;
    return (size_t)(code.ptr - code.start);
}

static int test_single(void *memory)
{
    uint32_t regs[64], cycles, value;
    init_regs(regs);
    regs[0] = 0x02000100;
    regs[1] = 0x12345678;
    if (!run(memory, 0xe5a01004, regs, &cycles, 0))
        return 0;
    memcpy(&value, ewram + 0x104, sizeof(value));
    if (regs[0] != 0x02000104 || value != 0x12345678 ||
        regs[15] != PC + 4 || cycles != 92 || slow_writes)
    {
        printf("single 1: r0=%08lx value=%08lx pc=%08lx cycles=%lu writes=%u\n",
                (unsigned long)regs[0], (unsigned long)value,
                (unsigned long)regs[15], (unsigned long)cycles, slow_writes);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x02000100;
    regs[2] = 1;
    if (!run(memory, 0xe7901102, regs, &cycles, 0) ||
        regs[1] != 0x12345678 || regs[15] != PC + 4 || cycles != 92 ||
            slow_reads)
    {
        printf("single 2: r1=%08lx pc=%08lx cycles=%lu reads=%u\n",
                (unsigned long)regs[1], (unsigned long)regs[15],
                (unsigned long)cycles, slow_reads);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x03000000;
    iwram[0x8000] = 0x80;
    if (!run(memory, 0xe1d010d0, regs, &cycles, 0) ||
        regs[1] != 0xffffff80u || cycles != 97 || slow_reads)
    {
        printf("single 3: r1=%08lx cycles=%lu reads=%u\n",
                (unsigned long)regs[1], (unsigned long)cycles, slow_reads);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x04000100;
    regs[1] = 0x12345678;
    if (!run(memory, 0xe5801000, regs, &cycles, 0) ||
        slow_writes != 1 || regs[15] != PC + 4 ||
            cycles != 95 || !(gpsp_rv32_take_memory_alert() & 1))
    {
        printf("single 4: writes=%u pc=%08lx cycles=%lu alert=%lu\n",
                slow_writes, (unsigned long)regs[15], (unsigned long)cycles,
                (unsigned long)gpsp_rv32_take_memory_alert());
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x02000104;
    if (!run(memory, 0xe590f000, regs, &cycles, 1) ||
        regs[15] != 0x12345678 || cycles != 92)
    {
        printf("single 5: pc=%08lx cycles=%lu\n",
                (unsigned long)regs[15], (unsigned long)cycles);
        return 0;
    }

    unsigned reads = slow_reads;
    unsigned writes = slow_writes;
    value = 0x12345678;
    memcpy(ewram + 0x404, &value, sizeof(value));
    init_regs(regs);
    regs[0] = 0x82000400;
    regs[2] = 9;
    regs[16] = 0x5000001f;
    /* ROR #1 wraps bit 0 into bit 31 before pre-index writeback. */
    if (!run(memory, 0xe7b010e2, regs, &cycles, 0) ||
        regs[0] != 0x02000404 || regs[1] != value || regs[2] != 9 ||
            regs[15] != PC + 4 || regs[16] != 0x5000001f ||
            cycles != 92 || slow_reads != reads || slow_writes != writes)
    {
        printf("single ROR load: r0=%08lx r1=%08lx flags=%08lx cycles=%lu\n",
                (unsigned long)regs[0], (unsigned long)regs[1],
                (unsigned long)regs[16], (unsigned long)cycles);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x02000400;
    regs[1] = 0xaabbccdd;
    regs[2] = 0x80000002;
    regs[16] = 0xe000001f;
    /* Post-index ROR #31 keeps the store address and raw writeback. */
    if (!run(memory, 0xe6801fe2, regs, &cycles, 0))
        return 0;
    memcpy(&value, ewram + 0x400, sizeof(value));
    if (value != 0xaabbccdd || regs[0] != 0x02000405 ||
        regs[1] != 0xaabbccdd || regs[2] != 0x80000002 ||
            regs[15] != PC + 4 || regs[16] != 0xe000001f ||
            cycles != 92 || slow_reads != reads || slow_writes != writes)
    {
        printf("single ROR store: r0=%08lx value=%08lx flags=%08lx cycles=%lu\n",
                (unsigned long)regs[0], (unsigned long)value,
                (unsigned long)regs[16], (unsigned long)cycles);
        return 0;
    }

    /* Added Rm, LSL #1-3: pre-index, writeback, post-index alias, down. */
    value = 0x0badcafe;
    memcpy(ewram + 0x504, &value, sizeof(value));
    init_regs(regs);
    regs[0] = 0x02000500;
    regs[2] = 2;
    if (!run(memory, 0xe7901082, regs, &cycles, 0) ||  /* LDR [r0,r2,LSL #1] */
        regs[1] != value || regs[0] != 0x02000500 || cycles != 92 ||
            slow_reads != reads || slow_writes != writes)
    {
        printf("single LSL #1 load: r0=%08lx r1=%08lx cycles=%lu\n",
                (unsigned long)regs[0], (unsigned long)regs[1],
                (unsigned long)cycles);
        return 0;
    }
    init_regs(regs);
    regs[0] = 0x02000500;
    regs[1] = 0x11223344;
    regs[2] = 4;
    if (!run(memory, 0xe7a01182, regs, &cycles, 0))  /* STR [r0,r2,LSL #3]! */
        return 0;
    memcpy(&value, ewram + 0x520, sizeof(value));
    if (value != 0x11223344 || regs[0] != 0x02000520 || cycles != 92 ||
        slow_reads != reads || slow_writes != writes)
    {
        printf("single LSL #3 store: r0=%08lx value=%08lx cycles=%lu\n",
                (unsigned long)regs[0], (unsigned long)value,
                (unsigned long)cycles);
        return 0;
    }
    value = 0x55667788;
    memcpy(ewram + 0x600, &value, sizeof(value));
    init_regs(regs);
    regs[0] = 0x02000600;
    if (!run(memory, 0xe6901080, regs, &cycles, 0) ||  /* LDR [r0],r0,LSL #1 */
        regs[1] != value || regs[0] != 0x06001200 || cycles != 92 ||
            slow_reads != reads || slow_writes != writes)
    {
        printf("single LSL #1 post: r0=%08lx r1=%08lx cycles=%lu\n",
                (unsigned long)regs[0], (unsigned long)regs[1],
                (unsigned long)cycles);
        return 0;
    }
    value = 0x99aabbcc;
    memcpy(ewram + 0x704, &value, sizeof(value));
    init_regs(regs);
    regs[0] = 0x02000708;
    regs[2] = 2;
    if (!run(memory, 0xe7101082, regs, &cycles, 0) ||  /* LDR [r0,-r2,LSL #1] */
        regs[1] != value || regs[0] != 0x02000708 || cycles != 92 ||
            slow_reads != reads || slow_writes != writes)
    {
        printf("single LSL #1 down: r0=%08lx r1=%08lx cycles=%lu\n",
                (unsigned long)regs[0], (unsigned long)regs[1],
                (unsigned long)cycles);
        return 0;
    }
    /* PC base reads PC+8; the sum wraps into EWRAM. Rd is also Rm. */
    value = 0x13572468;
    memcpy(ewram + 0x400, &value, sizeof(value));
    for (unsigned rd = 1; rd <= 2; rd++)
    {
        init_regs(regs);
        regs[2] = 0x3e8000fe;
        if (!run(memory, 0xe79f0102 | (rd << 12), regs, &cycles, 0) ||
            regs[rd] != value || (rd == 1 && regs[2] != 0x3e8000fe) ||
                cycles != 92 || slow_reads != reads || slow_writes != writes)
        {
            printf("single PC base LSL #2: rd=%u value=%08lx cycles=%lu\n", rd,
                    (unsigned long)regs[rd], (unsigned long)cycles);
            return 0;
        }
    }
    return 1;
}

static int test_immediate_folding(void *memory)
{
    uint32_t regs[64], cycles, value;
    unsigned reads = slow_reads;
    unsigned writes = slow_writes;

    init_regs(regs);
    regs[0] = 0x02000900;
    if (!run(memory, ARM_IMM(1, 0, 0, 1, 0, 0, 0, 4),
            regs, &cycles, 0))
        return 0;
    memcpy(&value, ewram + 0x8fc, sizeof(value));
    if (value != 0x02000900 || regs[0] != 0x020008fc ||
        cycles != 92 || slow_writes != writes)
        return 0;

    init_regs(regs);
    regs[0] = 0x02000900;
    value = 0x11223344;
    memcpy(ewram + 0x900, &value, sizeof(value));
    if (!run(memory, ARM_IMM(0, 1, 0, 0, 1, 0, 0, 4),
            regs, &cycles, 0) ||
            regs[0] != value || cycles != 92 || slow_reads != reads)
        return 0;

    init_regs(regs);
    regs[0] = 0x02000900;
    if (!run(memory, ARM_IMM(0, 1, 0, 0, 0, 0, 0, 4),
            regs, &cycles, 0))
        return 0;
    memcpy(&value, ewram + 0x900, sizeof(value));
    if (value != 0x02000900 || regs[0] != 0x02000904 ||
        cycles != 92 || slow_writes != writes)
        return 0;

    static const struct
    {
        unsigned up;
        unsigned offset;
        unsigned address;
        uint8_t value;
    } cases[] = {
        {1, 2047, 0x17ff, 0xa5},
        {0, 2048, 0x0800, 0x5b},
        {1, 2048, 0x1800, 0x5c},
        {0, 2049, 0x07ff, 0x5d},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        ewram[cases[i].address] = cases[i].value;
        init_regs(regs);
        regs[0] = 0x02001000;
        if (!run(memory, ARM_IMM(1, cases[i].up, 1, 0, 1, 0, 1,
                cases[i].offset), regs, &cycles, 0) ||
                regs[0] != 0x02001000 || regs[1] != cases[i].value ||
                cycles != 95 || slow_reads != reads)
            return 0;
    }

    size_t plus_fit = emitted_bytes(ARM_IMM(1, 1, 1, 0, 1, 0, 1, 2047));
    size_t plus_wide = emitted_bytes(ARM_IMM(1, 1, 1, 0, 1, 0, 1, 2048));
    size_t minus_fit = emitted_bytes(ARM_IMM(1, 0, 1, 0, 1, 0, 1, 2048));
    size_t minus_wide = emitted_bytes(ARM_IMM(1, 0, 1, 0, 1, 0, 1, 2049));
    /* A wide offset adds its two-instruction constant. */
    if (!plus_fit || plus_wide != plus_fit + 8 ||
        !minus_fit || minus_wide != minus_fit + 8)
        return 0;

    value = 0xaabbccdd;
    memcpy(rom_page + 8, &value, sizeof(value));
    init_regs(regs);
    if (!run(memory, ARM_IMM(1, 1, 0, 0, 1, 15, 1, 0),
            regs, &cycles, 0) ||
            regs[1] != value || regs[15] != PC + 4 || slow_reads != reads)
        return 0;
    return 1;
}

static int test_oam_word(void *memory)
{
    uint32_t regs[64], cycles, value;
    unsigned writes = slow_writes;
    memset(oam_ram, 0, sizeof(oam_ram));

    init_regs(regs);
    regs[0] = 0x07000401;
    regs[1] = 0x12345678;
    if (!run(memory, 0xe5801000, regs, &cycles, 0))
        return 0;
    memcpy(&value, oam_ram, sizeof(value));
    if (value != 0x12345678 || regs[OAM_UPDATED] != 1 ||
        regs[0] != 0x07000401 || regs[15] != PC + 4 ||
            cycles != 96 || slow_writes != writes)
    {
        printf("OAM mirror: value=%08lx dirty=%lu cycles=%lu writes=%u\n",
                (unsigned long)value, (unsigned long)regs[OAM_UPDATED],
                (unsigned long)cycles, slow_writes);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x070007f8;
    regs[1] = 0xaabbccdd;
    if (!run(memory, 0xe5a01007, regs, &cycles, 0))
        return 0;
    memcpy(&value, (uint8_t *)oam_ram + 0x3fc, sizeof(value));
    if (value != 0xaabbccdd || regs[0] != 0x070007ff ||
        regs[OAM_UPDATED] != 1 || cycles != 96 || slow_writes != writes)
    {
        printf("OAM writeback: value=%08lx base=%08lx cycles=%lu writes=%u\n",
                (unsigned long)value, (unsigned long)regs[0],
                (unsigned long)cycles, slow_writes);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x07000001;
    if (!run(memory, 0xe5a00004, regs, &cycles, 0))
        return 0;
    memcpy(&value, (uint8_t *)oam_ram + 4, sizeof(value));
    if (value != 0x07000001 || regs[0] != 0x07000005 ||
        regs[OAM_UPDATED] != 1 || cycles != 96 || slow_writes != writes)
    {
        printf("OAM base store: value=%08lx base=%08lx cycles=%lu writes=%u\n",
                (unsigned long)value, (unsigned long)regs[0],
                (unsigned long)cycles, slow_writes);
        return 0;
    }
    return 1;
}

static int test_palette_word(void *memory)
{
    uint32_t regs[64], cycles, raw, converted;
    unsigned writes = slow_writes;
    memset(palette_ram, 0, sizeof(palette_ram));
    memset(palette_ram_converted, 0, sizeof(palette_ram_converted));

    init_regs(regs);
    regs[0] = 0x05000400;
    regs[1] = 0x7c1f03e0;
    if (!run(memory, 0xe5801000, regs, &cycles, 0))
        return 0;
    memcpy(&raw, palette_ram, sizeof(raw));
    memcpy(&converted, palette_ram_converted, sizeof(converted));
    if (raw != 0x7c1f03e0 || converted != 0xf81f07c0 ||
        regs[0] != 0x05000400 || regs[15] != PC + 4 ||
            cycles != 96 || slow_writes != writes)
    {
        printf("palette mirror: raw=%08lx converted=%08lx cycles=%lu writes=%u\n",
                (unsigned long)raw, (unsigned long)converted,
                (unsigned long)cycles, slow_writes);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x050007f8;
    regs[1] = 0x001f7c00;
    if (!run(memory, 0xe5a01007, regs, &cycles, 0))
        return 0;
    memcpy(&raw, (uint8_t *)palette_ram + 0x3fc, sizeof(raw));
    memcpy(&converted, (uint8_t *)palette_ram_converted + 0x3fc,
            sizeof(converted));
    if (raw != 0x001f7c00 || converted != 0xf800001f ||
        regs[0] != 0x050007ff || regs[15] != PC + 4 ||
            cycles != 96 || slow_writes != writes)
    {
        printf("palette writeback: raw=%08lx converted=%08lx base=%08lx cycles=%lu writes=%u\n",
                (unsigned long)raw, (unsigned long)converted,
                (unsigned long)regs[0], (unsigned long)cycles, slow_writes);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x05000b03;
    regs[1] = 0xd5839e6a;
    if (!run(memory, ARM_IMM(0, 1, 0, 0, 0, 0, 1, 5),
            regs, &cycles, 0))
        return 0;
    memcpy(&raw, (uint8_t *)palette_ram + 0x300, sizeof(raw));
    memcpy(&converted, (uint8_t *)palette_ram_converted + 0x300,
            sizeof(converted));
    if (raw != 0xd5839e6a || converted != 0x1b1554c7 ||
        regs[0] != 0x05000b08 || regs[15] != PC + 4 ||
            cycles != 96 || slow_writes != writes)
    {
        printf("palette post-index: raw=%08lx converted=%08lx base=%08lx cycles=%lu writes=%u\n",
                (unsigned long)raw, (unsigned long)converted,
                (unsigned long)regs[0], (unsigned long)cycles, slow_writes);
        return 0;
    }
    return 1;
}

static int test_ram_loads(void *memory)
{
    uint32_t regs[64], cycles;
    unsigned reads = slow_reads;

    init_regs(regs);
    regs[0] = 0x02040000;
    *(uint32_t *)(void *)(ewram + 4) = 0x11223344;
    if (!run(memory, 0xe5901004, regs, &cycles, 0) ||
        regs[1] != 0x11223344 || cycles != 92 || slow_reads != reads)
        return 0;

    init_regs(regs);
    regs[0] = 0x03008000;
    iwram[0x8001] = 0xa5;
    if (!run(memory, 0xe5d01001, regs, &cycles, 0) ||
        regs[1] != 0xa5 || cycles != 97 || slow_reads != reads)
        return 0;

    init_regs(regs);
    regs[2] = 0x03000000;
    *(uint16_t *)(void *)(iwram + 0x8002) = 0x81ff;
    if (!run(memory, 0xe1d230b2, regs, &cycles, 0) ||
        regs[3] != 0x81ff || cycles != 97 || slow_reads != reads)
        return 0;

    init_regs(regs);
    regs[2] = 0x02000100;
    ewram[0x101] = 0x80;
    if (!run(memory, 0xe1d230d1, regs, &cycles, 0) ||
        regs[3] != 0xffffff80u || cycles != 95 || slow_reads != reads)
        return 0;

    init_regs(regs);
    regs[0] = 0x04000000;
    if (!run(memory, 0xe5901004, regs, &cycles, 0) ||
        regs[1] != 0x11223344 || cycles != 95 || slow_reads != reads + 1)
        return 0;

    init_regs(regs);
    regs[0] = 0x02000101;
    *(uint32_t *)(void *)(ewram + 0x100) = 0x12345678;
    return run(memory, 0xe5901000, regs, &cycles, 0) &&
            regs[1] == 0x78123456 && cycles == 92 &&
            slow_reads == reads + 2;
}

static int test_io(void *memory)
{
    uint32_t regs[64], cycles, slow_cycles;
    unsigned reads = slow_reads, writes = slow_writes;
    memset(io_registers, 0, sizeof(io_registers));
    io_registers[0] = 0x1122;
    io_registers[1] = 0x3344;

    /* The native read must charge what the C path charges. */
    init_regs(regs);
    regs[0] = 0x04000000;
    if (!run(memory, 0xe1d010b0, regs, &slow_cycles, 0) ||
        slow_reads != reads + 1)
        return 0;
    gpsp_rv32_io_read_mask16[0] = 0xffff;
    gpsp_rv32_io_read_mask16[1] = 0xffff;
    gpsp_rv32_io_read_mask32[0] = 0xffffffff;
    reads = slow_reads;

    static const struct
    {
        const char *name;
        uint32_t opcode;
        uint32_t address;
        uint32_t value;
        unsigned slow;
    } loads[] = {
        { "halfword",           0xe1d010b0, 0x04000000, 0x00001122, 0 },
        { "word",               0xe5901000, 0x04000000, 0x33441122, 0 },
        /* Unaligned reads rotate on the C path. */
        { "unaligned halfword", 0xe1d010b0, 0x04000001, 0, 1 },
        { "unaligned word",     0xe5901000, 0x04000002, 0, 1 },
    };
    for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++)
    {
        init_regs(regs);
        regs[0] = loads[i].address;
        unsigned before = slow_reads;
        if (!run(memory, loads[i].opcode, regs, &cycles, 0) ||
            slow_reads != before + loads[i].slow ||
                (!loads[i].slow && regs[1] != loads[i].value) ||
                (i == 0 && cycles != slow_cycles))
        {
            printf("io %s: r1=%08lx cycles=%lu/%lu reads=%u\n", loads[i].name,
                    (unsigned long)regs[1], (unsigned long)cycles,
                    (unsigned long)slow_cycles, slow_reads - before);
            return 0;
        }
    }

    /* IF acknowledges natively; IE takes the C path once an IRQ is due. */
    io_registers[0x101] = 0x000f;
    init_regs(regs);
    regs[0] = 0x04000202;
    regs[1] = 0x0005;
    if (!run(memory, 0xe1c010b0, regs, &cycles, 0) ||
        io_registers[0x101] != 0x000a || slow_writes != writes)
        return 0;

    io_registers[0x104] = 0;
    init_regs(regs);
    regs[0] = 0x04000200;
    regs[1] = 0x0002;
    if (!run(memory, 0xe1c010b0, regs, &cycles, 0) ||
        io_registers[0x100] != 0x0002 || slow_writes != writes)
        return 0;

    io_registers[0x104] = 1;
    init_regs(regs);
    regs[0] = 0x04000200;
    regs[1] = 0x0002;
    if (!run(memory, 0xe1c010b0, regs, &cycles, 0) ||
        slow_writes != writes + 1)
        return 0;

    memset(gpsp_rv32_io_read_mask16, 0, sizeof(gpsp_rv32_io_read_mask16));
    memset(gpsp_rv32_io_read_mask32, 0, sizeof(gpsp_rv32_io_read_mask32));
    memset(io_registers, 0, sizeof(io_registers));
    return 1;
}

static int test_rom_loads(void *memory)
{
    uint32_t regs[64], cycles;
    memset(rom_page, 0, sizeof(rom_page));
    rom_page[0x100] = 0x80;
    rom_page[0x101] = 0x81;
    rom_page[0x102] = 0x7f;
    rom_page[0x103] = 0xfe;
    gamepak_size = 0x8000;
    memory_map_read[0x08000000u >> 15] = rom_page;
    memory_map_read[0x0c000000u >> 15] = rom_page;
    /* An out-of-range mapped page must still take the slow path. */
    memory_map_read[0x08008000u >> 15] = rom_page;
    ws_cyc_nseq[8][0] = 4;
    ws_cyc_nseq[8][1] = 9;
    ws_cyc_nseq[10][1] = 13;
    ws_cyc_nseq[12][1] = 11;
    ws_cyc_nseq[13][1] = 15;

    static const struct
    {
        const char *name;
        uint32_t opcode;
        uint32_t address;
        uint32_t value;
        uint32_t cycles;
        unsigned slow;
    } cases[] = {
        { "word",          0xe5901000, 0x08000100, 0xfe7f8180, 89, 0 },
        { "byte",          0xe5d01000, 0x08000100, 0x00000080, 94, 0 },
        { "halfword",      0xe1d010b0, 0x08000100, 0x00008180, 94, 0 },
        { "signed byte",   0xe1d010d0, 0x08000100, 0xffffff80, 94, 0 },
        { "signed half",   0xe1d010f0, 0x08000100, 0xffff8180, 94, 0 },
        { "ROM alias",     0xe5901000, 0x0c000100, 0xfe7f8180, 87, 0 },
        { "missing page",  0xe5901000, 0x0a000100, 0x11223344, 85, 1 },
        { "unaligned",     0xe5901000, 0x08000101, 0x80fe7f81, 89, 1 },
        { "EEPROM region", 0xe5901000, 0x0d000100, 0x11223344, 83, 1 },
        { "past ROM end",  0xe5901000, 0x08008000, 0x11223344, 89, 1 },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        unsigned reads = slow_reads;
        init_regs(regs);
        regs[0] = cases[i].address;
        if (!run(memory, cases[i].opcode, regs, &cycles, 0) ||
            regs[1] != cases[i].value || regs[15] != PC + 4 ||
                cycles != cases[i].cycles || slow_reads != reads + cases[i].slow)
        {
            printf("ROM %s: r1=%08lx pc=%08lx cycles=%lu reads=%u (+%u)\n",
                    cases[i].name, (unsigned long)regs[1],
                    (unsigned long)regs[15], (unsigned long)cycles,
                    slow_reads, cases[i].slow);
            return 0;
        }
    }

    /* Reuse one translation to verify that WAITCNT changes are observed. */
    if (!emit(memory, 0xe5901000, 0))
        return 0;
    unsigned reads = slow_reads;
    ws_cyc_nseq[8][1] = 17;
    init_regs(regs);
    regs[0] = 0x08000100;
    cycles = gpsp_rv32_run_block(regs, 100, memory);
    if (regs[1] != 0xfe7f8180 || cycles != 81 || slow_reads != reads)
        return 0;
    ws_cyc_nseq[8][1] = 7;
    init_regs(regs);
    regs[0] = 0x08000100;
    cycles = gpsp_rv32_run_block(regs, 100, memory);
    return regs[1] == 0xfe7f8180 && cycles == 91 &&
            slow_reads == reads;
}

static int test_multi(void *memory)
{
    uint32_t regs[64], cycles, values[3];
    init_regs(regs);
    regs[0] = 0x10203040;
    regs[1] = 0x55667788;
    regs[13] = 0x02000200;
    regs[14] = 0x08001000;
    if (!run(memory, 0xe92d4003, regs, &cycles, 0))
        return 0;
    memcpy(values, ewram + 0x1f4, sizeof(values));
    if (regs[13] != 0x020001f4 || values[0] != 0x10203040 ||
        values[1] != 0x55667788 || values[2] != 0x08001000 ||
            cycles != 80 || slow_writes != 1)
    {
        printf("multi 1: sp=%08lx words=%08lx,%08lx,%08lx cycles=%lu writes=%u\n",
                (unsigned long)regs[13], (unsigned long)values[0],
                (unsigned long)values[1], (unsigned long)values[2],
                (unsigned long)cycles, slow_writes);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x020001f4;
    if (!run(memory, 0xe8a00003, regs, &cycles, 0))
        return 0;
    memcpy(values, ewram + 0x1f4, 8);
    if (values[0] != 0x020001f4 || values[1] != 0 ||
        regs[0] != 0x020001fc || cycles != 86)
    {
        printf("multi 2: r0=%08lx words=%08lx,%08lx cycles=%lu\n",
                (unsigned long)regs[0], (unsigned long)values[0],
                (unsigned long)values[1], (unsigned long)cycles);
        return 0;
    }

    init_regs(regs);
    regs[3] = 0x020001f4;
    if (!run(memory, 0xe8b30003, regs, &cycles, 0) ||
        regs[0] != 0x020001f4 || regs[1] != 0 ||
            regs[3] != 0x020001fc || cycles != 86)
    {
        printf("multi 3: r0=%08lx r1=%08lx r3=%08lx cycles=%lu\n",
                (unsigned long)regs[0], (unsigned long)regs[1],
                (unsigned long)regs[3], (unsigned long)cycles);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x020001f4;
    values[0] = 0xdeadbeef;
    values[1] = 0x11223344;
    memcpy(ewram + 0x1f4, values, 8);
    if (!run(memory, 0xe8b00003, regs, &cycles, 0) ||
        regs[0] != values[0] || regs[1] != values[1] || cycles != 86)
        return 0;

    init_regs(regs);
    regs[13] = 0x020001f4;
    *(uint32_t *)(void *)(ewram + 0x1f4) = 0x08001234;
    if (!run(memory, 0xe8bd8000, regs, &cycles, 1) ||
        regs[15] != 0x08001234 || regs[13] != 0x020001f8 ||
            cycles != 92)
    {
        printf("multi 4: pc=%08lx sp=%08lx cycles=%lu\n",
                (unsigned long)regs[15], (unsigned long)regs[13],
                (unsigned long)cycles);
        return 0;
    }
    return 1;
}

static uint32_t vram_word(uint32_t offset)
{
    uint32_t value;
    memcpy(&value, vram + offset, sizeof(value));
    return value;
}

/* VRAM stores stay native: mirror, byte copies and STM runs. */
static int test_vram(void *memory)
{
    uint32_t regs[64], cycles;
    unsigned writes = slow_writes;
    memset(vram, 0, sizeof(vram));

    init_regs(regs);
    regs[0] = 0x06000102;
    regs[1] = 0x12345678;
    if (!run(memory, 0xe1c010b0, regs, &cycles, 0) ||
        vram_word(0x100) != 0x56780000 || cycles != 97 ||
            slow_writes != writes)
    {
        printf("VRAM strh: word=%08lx cycles=%lu writes=%u\n",
                (unsigned long)vram_word(0x100), (unsigned long)cycles, slow_writes);
        return 0;
    }

    /* STRB copies the low byte to both halves of the even halfword. */
    init_regs(regs);
    regs[0] = 0x06000105;
    regs[1] = 0x1234ab5c;
    if (!run(memory, 0xe5c01000, regs, &cycles, 0) ||
        vram_word(0x104) != 0x00005c5c || regs[1] != 0x1234ab5c ||
            cycles != 97 || slow_writes != writes)
    {
        printf("VRAM strb: word=%08lx r1=%08lx cycles=%lu writes=%u\n",
                (unsigned long)vram_word(0x104), (unsigned long)regs[1],
                (unsigned long)cycles, slow_writes);
        return 0;
    }

    /* 0x18000-0x1ffff mirrors 0x10000-0x17fff; the region wraps at 128K. */
    static const struct { uint32_t address, offset; } words[] = {
        {0x06018004, 0x10004}, {0x0601c000, 0x14000}, {0x06020008, 0x8},
        {0x06017ffc, 0x17ffc}, {0x06008000, 0x8000}, {0x0601fffc, 0x17ffc},
        {0x0603801c, 0x1001c},
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    {
        init_regs(regs);
        regs[0] = words[i].address;
        regs[1] = 0xa0b0c0d0 + i;
        if (!run(memory, 0xe5801000, regs, &cycles, 0) ||
            vram_word(words[i].offset) != 0xa0b0c0d0 + i || cycles != 96 ||
                slow_writes != writes)
        {
            printf("VRAM str %u: word=%08lx cycles=%lu writes=%u\n", i,
                    (unsigned long)vram_word(words[i].offset), (unsigned long)cycles,
                    slow_writes);
            return 0;
        }
    }

    /* Halfword and byte stores in the mirror. */
    init_regs(regs);
    regs[0] = 0x0601c012;
    regs[1] = 0x12345678;
    if (!run(memory, 0xe1c010b0, regs, &cycles, 0) ||
        vram_word(0x14010) != 0x56780000 || cycles != 97 ||
            slow_writes != writes)
    {
        printf("VRAM strh mirror: word=%08lx cycles=%lu writes=%u\n",
                (unsigned long)vram_word(0x14010), (unsigned long)cycles,
                slow_writes);
        return 0;
    }
    init_regs(regs);
    regs[0] = 0x0601800b;
    regs[1] = 0x1234ab5c;
    if (!run(memory, 0xe5c01000, regs, &cycles, 0) ||
        vram_word(0x10008) != 0x5c5c0000 || cycles != 97 ||
            slow_writes != writes)
    {
        printf("VRAM strb mirror: word=%08lx cycles=%lu writes=%u\n",
                (unsigned long)vram_word(0x10008), (unsigned long)cycles,
                slow_writes);
        return 0;
    }

    /* Post-index writeback keeps the unaligned sum. */
    init_regs(regs);
    regs[0] = 0x06000201;
    regs[1] = 0xbeef;
    if (!run(memory, 0xe0c01fb0, regs, &cycles, 0) ||
        vram_word(0x200) != 0xbeef || regs[0] != 0x060002f1 ||
            cycles != 97 || slow_writes != writes)
    {
        printf("VRAM strh post: word=%08lx r0=%08lx cycles=%lu\n",
                (unsigned long)vram_word(0x200), (unsigned long)regs[0],
                (unsigned long)cycles);
        return 0;
    }

    /* STMIA with writeback inside one 32 KiB run, mirrored. */
    init_regs(regs);
    regs[0] = 0x0601fff8;
    regs[1] = 0x11111111;
    regs[2] = 0x22222222;
    if (!run(memory, 0xe8a00006, regs, &cycles, 0) ||
        vram_word(0x17ff8) != 0x11111111 || vram_word(0x17ffc) != 0x22222222 ||
            regs[0] != 0x06020000 || cycles != 94 || slow_writes != writes)
    {
        printf("VRAM stm: words=%08lx,%08lx r0=%08lx cycles=%lu writes=%u\n",
                (unsigned long)vram_word(0x17ff8), (unsigned long)vram_word(0x17ffc),
                (unsigned long)regs[0], (unsigned long)cycles, slow_writes);
        return 0;
    }

    /* STMIA with bit 15 set below the mirror keeps bit 15. */
    init_regs(regs);
    regs[0] = 0x06008000;
    regs[1] = 0x55555555;
    regs[2] = 0x66666666;
    if (!run(memory, 0xe8a00006, regs, &cycles, 0) ||
        vram_word(0x8000) != 0x55555555 || vram_word(0x8004) != 0x66666666 ||
            regs[0] != 0x06008008 || cycles != 94 || slow_writes != writes)
    {
        printf("VRAM stm bit15: words=%08lx,%08lx r0=%08lx cycles=%lu\n",
                (unsigned long)vram_word(0x8000), (unsigned long)vram_word(0x8004),
                (unsigned long)regs[0], (unsigned long)cycles);
        return 0;
    }

    /* A run across 0x18000 takes the helper, word by word. */
    init_regs(regs);
    regs[0] = 0x06017ffc;
    regs[1] = 0x33333333;
    regs[2] = 0x44444444;
    if (!run(memory, 0xe8800006, regs, &cycles, 0) ||
        vram_word(0x17ffc) != 0x33333333 || vram_word(0x10000) != 0x44444444 ||
            slow_writes != writes + 2)
    {
        printf("VRAM stm cross: words=%08lx,%08lx writes=%u\n",
                (unsigned long)vram_word(0x17ffc), (unsigned long)vram_word(0x10000),
                slow_writes);
        return 0;
    }
    return 1;
}

static int test_banked_multi(void *memory)
{
    uint32_t cycles, values[2];
    memset(reg_mode, 0, sizeof(reg_mode));
    memset(spsr, 0, sizeof(spsr));
    init_regs(reg);
    reg[0] = 0x02000400;
    reg[8] = 0x11111111;
    reg[13] = 0x22222222;
    reg[16] = 0x12;
    reg[17] = 0x12;
    reg_mode[0][0] = 0x88888888;
    reg_mode[0][5] = 0xdddddddd;
    if (!run(memory, 0xe8c02100, reg, &cycles, 0))
        return 0;
    memcpy(values, ewram + 0x400, sizeof(values));
    if (values[0] != 0x88888888 || values[1] != 0xdddddddd ||
        reg[8] != 0x11111111 || reg[13] != 0x22222222 || cycles != 86)
        return 0;

    *(uint32_t *)(void *)(ewram + 0x400) = 0xaaaaaaaa;
    *(uint32_t *)(void *)(ewram + 0x404) = 0xbbbbbbbb;
    reg[15] = PC;
    if (!run(memory, 0xe8d02100, reg, &cycles, 0) ||
        reg_mode[0][0] != 0xaaaaaaaa ||
            reg_mode[0][5] != 0xbbbbbbbb ||
            reg[8] != 0x11111111 || reg[13] != 0x22222222 ||
            cycles != 86)
        return 0;

    *(uint32_t *)(void *)(ewram + 0x400) = 0x08001234;
    init_regs(reg);
    reg[0] = 0x02000400;
    reg[16] = 0xd2;
    reg[17] = 0x11;
    spsr[1] = 0x6000003f;
    if (!run(memory, 0xe8d08000, reg, &cycles, 1) ||
        reg[15] != 0x08001234 || reg[16] != 0x6000003f ||
            reg[17] != 0x10 || cycles != 94)
        return 0;

    init_regs(reg);
    reg[0] = 0x02000400;
    reg[16] = 0xd2;
    reg[17] = 0x11;
    spsr[1] = 0x6000001f;
    return run(memory, 0xe8d08000, reg, &cycles, 1) &&
            reg[15] == 0x08001234 && reg[16] == 0x6000001f &&
            reg[17] == 0x10 && cycles == 92;
}

static int test_edges(void *memory)
{
    uint32_t regs[64], cycles;
    unsigned reads = slow_reads;
    init_regs(regs);
    regs[0] = 0x02000310;
    regs[1] = 0x76543210;
    if (!run(memory, 0x05901000, regs, &cycles, 0) ||
        regs[1] != 0x76543210 || regs[15] != PC + 4 ||
            cycles != 98 || slow_reads != reads)
    {
        printf("edge 1: r1=%08lx pc=%08lx cycles=%lu reads=%u\n",
                (unsigned long)regs[1], (unsigned long)regs[15],
                (unsigned long)cycles, slow_reads);
        return 0;
    }

    uint16_t halfword = 0x1122;
    memcpy(ewram + 0x310, &halfword, sizeof(halfword));
    init_regs(regs);
    regs[0] = 0x02000311;
    if (!run(memory, 0xe1d010b0, regs, &cycles, 0) ||
        regs[1] != 0x11 || cycles != 95 || slow_reads != reads + 1)
    {
        printf("edge 2: r1=%08lx cycles=%lu reads=%u\n",
                (unsigned long)regs[1], (unsigned long)cycles, slow_reads);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x02000301;
    regs[1] = 0x77;
    ewram[0x40000 + 0x300] = 1;
    unsigned writes = slow_writes;
    if (!run(memory, 0xe5c01000, regs, &cycles, 0) ||
        ewram[0x301] != 0x77 || slow_writes != writes + 1 ||
            !(gpsp_rv32_take_memory_alert() & 2) || cycles != 95)
    {
        printf("edge 3: byte=%02x cycles=%lu writes=%u alert=%lu\n",
                ewram[0x301], (unsigned long)cycles, slow_writes,
                (unsigned long)gpsp_rv32_take_memory_alert());
        return 0;
    }
    ewram[0x40000 + 0x300] = 0;

    uint32_t first = 0x11111111, second = 0x22222222;
    memcpy(ewram + 0x3fffc, &first, sizeof(first));
    memcpy(ewram, &second, sizeof(second));
    init_regs(regs);
    regs[0] = 0x0203fffc;
    reads = slow_reads;
    if (!run(memory, 0xe8b00006, regs, &cycles, 0) ||
        regs[0] != 0x02040004 || regs[1] != first ||
            regs[2] != second || cycles != 86 ||
            slow_reads != reads + 2)
    {
        printf("edge 4: r0=%08lx r1=%08lx r2=%08lx cycles=%lu reads=%u\n",
                (unsigned long)regs[0], (unsigned long)regs[1],
                (unsigned long)regs[2], (unsigned long)cycles, slow_reads);
        return 0;
    }

    init_regs(regs);
    regs[0] = 0x02000300;
    regs[1] = 0xaabbccdd;
    regs[2] = 0x55667788;
    ewram[0x40000 + 0x304] = 1;
    writes = slow_writes;
    if (!run(memory, 0xe8a00006, regs, &cycles, 0) ||
        regs[0] != 0x02000308 || slow_writes != writes + 2 ||
            cycles != 86 || !(gpsp_rv32_take_memory_alert() & 2))
        return 0;
    ewram[0x40000 + 0x304] = 0;
    return 1;
}

static int emit_flow(void *memory, uint32_t pc, const uint32_t *ops,
        unsigned count, gpsp_rv32_arm_flow_layout *layout)
{
    gpsp_rv32_arm_flow_ir ir;
    uint8_t *ptr = memory;
    if (!gpsp_rv32_arm_flow_full_decode(pc, ops, count, 1, &ir) ||
        !gpsp_rv32_arm_flow_full_emit(&ir, &ptr,
                (uint8_t *)memory + CODE_SIZE, layout))
        return 0;
    gpsp_rv32_code tail;
    gpsp_rv32_code_init(&tail, ptr, CODE_SIZE - (ptr - (uint8_t *)memory));
    return gpsp_rv32_jalr(&tail, X0, RA, 0) &&
            (rv32_golden_dump("flow", memory, CODE_SIZE, memory, tail.ptr),
                    sync_code(memory));
}

/* Outermost fixed spans opened on the code buffer, as byte ranges. */
static const uint8_t *span_buffer;
static const uint8_t *span_begin;
static uint16_t spans[16][2];
static unsigned span_count;
static unsigned hook_words, free_words;

static void record_span(const gpsp_rv32_code *code, bool begin)
{
    if (code->start != span_buffer)
        return;
    if (begin && code->fixed_depth == 1)
        span_begin = code->ptr;
    else if (!begin && !code->fixed_depth && span_count < 16)
    {
        spans[span_count][0] = (uint16_t)(span_begin - span_buffer);
        spans[span_count++][1] = (uint16_t)(code->ptr - span_buffer);
    }
}

static void count_free(const gpsp_rv32_code *code)
{
    hook_words++;
    free_words += !code->fixed_depth;
}

static uint16_t words_at(const uint32_t *words, size_t at)
{
    uint16_t half;
    memcpy(&half, (const uint8_t *)words + at, sizeof(half));
    return half;
}

/* Moving a memory op by two bytes preserves its instruction bytes. With
   compressed forms, stores and multiple transfers use c.lw and c.sw. */
static int test_two_byte_position(void)
{
    unsigned compact_loads = 0, compact_stores = 0;
    static const uint32_t opcodes[] = {
        0xe5901000, 0xe5d01000, 0xe1d010b0, 0xe1d010d0, 0xe5801000,
        0xe5c01000, 0xe1c010b0, 0xe8900006, 0xe8a00006, 0x05901000,
    };
    for (unsigned i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++)
    {
        uint32_t words[256], shifted[257];
        gpsp_rv32_code code, moved;
        gpsp_rv32_code_init(&code, words, sizeof(words));
        gpsp_rv32_code_child(&moved, &code, (uint8_t *)shifted + 2,
                sizeof(words));
        gpsp_rv32_arm_memory_result a =
                gpsp_rv32_emit_arm_memory(&code, opcodes[i], 0x08000100);
        gpsp_rv32_arm_memory_result b =
                gpsp_rv32_emit_arm_memory(&moved, opcodes[i], 0x08000100);
        size_t size = (size_t)(code.ptr - code.start);
        if (a != GPSP_RV32_ARM_MEMORY_EMITTED || a != b || !size ||
            (size_t)(moved.ptr - moved.start) != size ||
                memcmp(moved.start, words, size))
        {
            printf("two-byte position %08lx: %d %d\n",
                    (unsigned long)opcodes[i], a, b);
            return 0;
        }
        for (size_t at = 0; at < size;)
        {
            uint16_t half = words_at(words, at);
            compact_loads += (half & 0xe003) == 0x4000;      /* c.lw */
            compact_stores += (half & 0xe003) == 0xc000;     /* c.sw */
            at += (half & 3) == 3 ? 4 : 2;
        }
    }
    if (gpsp_rv32_test_compress)
    {
        printf("RV32 ARM MEMORY COMPACT: c.lw=%u c.sw=%u\n",
                compact_loads, compact_stores);
        if (!compact_loads || !compact_stores)
            return 0;
    }
    return 1;
}

/* Emit into a parent of the given size with a forced retry; it must fail
   and leave the parent's bytes, cursor and policy unchanged. */
static int short_branch_failure(uint32_t opcode, size_t parent_size,
        size_t staging_size)
{
    static uint8_t parent[1024];
    memset(parent, 0x5a, sizeof(parent));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, parent, parent_size);
    code.short_branch = true;
    gpsp_rv32_code before = code;
    gpsp_rv32_arm_short_branch_stats stats = gpsp_rv32_arm_short_branch;
    gpsp_rv32_test_short_reach = 8;
    gpsp_rv32_test_arm_staging_bytes = staging_size;
    gpsp_rv32_arm_memory_result result =
            gpsp_rv32_emit_arm_memory(&code, opcode, 0x08000100);
    gpsp_rv32_test_short_reach = 0;
    gpsp_rv32_test_arm_staging_bytes = 0;
    if (result != GPSP_RV32_ARM_MEMORY_BUFFER_FULL ||
        code.ptr != before.ptr || code.compress != before.compress ||
            code.short_branch != before.short_branch ||
            code.branch_overflow != before.branch_overflow ||
            code.fixed_depth != before.fixed_depth ||
            gpsp_rv32_arm_short_branch.retries != stats.retries + 1 ||
            gpsp_rv32_arm_short_branch.retry_failures != stats.retry_failures + 1)
    {
        printf("short branch failure %08lx: result %d retries %u failures %u\n",
                (unsigned long)opcode, result,
                (unsigned)(gpsp_rv32_arm_short_branch.retries - stats.retries),
                (unsigned)(gpsp_rv32_arm_short_branch.retry_failures -
                        stats.retry_failures));
        return 0;
    }
    for (size_t at = 0; at < sizeof(parent); at++)
        if (parent[at] != 0x5a)
            return 0;
    return 1;
}

/* Short branches: a retry after an out-of-reach target equals output that
   never used them, and a failed emit leaves the parent unchanged. */
static int test_short_branches(void)
{
    static const uint32_t opcodes[] = {
        0xe5901000, 0xe5d01000, 0xe1d010b0, 0xe5801000, 0xe1c010b0,
        0xe8900006, 0xe8a00006, 0x05901000, 0xe8905ffe, 0xe92d5ffe,
    };
    unsigned shorts = 0, smaller = 0;
    for (unsigned i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++)
    {
        static uint32_t words[3][256];
        size_t size[3];
        gpsp_rv32_arm_memory_result result[3];
        for (unsigned mode = 0; mode < 3; mode++)
        {
            gpsp_rv32_code code;
            memset(words[mode], 0xcc, sizeof(words[mode]));
            gpsp_rv32_code_init(&code, words[mode], sizeof(words[mode]));
            gpsp_rv32_test_arm_short_branch = mode != 0;
            gpsp_rv32_test_short_reach = mode == 1 ? 8 : 0;
            result[mode] = gpsp_rv32_emit_arm_memory(&code, opcodes[i], 0x08000100);
            size[mode] = (size_t)(code.ptr - code.start);
        }
        gpsp_rv32_test_arm_short_branch = true;
        gpsp_rv32_test_short_reach = 0;
        if (result[0] > GPSP_RV32_ARM_MEMORY_PC_EXIT || result[1] != result[0] ||
            result[2] != result[0] || size[1] != size[0] ||
                memcmp(words[1], words[0], size[0]) || size[2] > size[0])
        {
            printf("short branch %08lx: results %d %d %d sizes %u %u %u\n",
                    (unsigned long)opcodes[i], result[0], result[1], result[2],
                    (unsigned)size[0], (unsigned)size[1], (unsigned)size[2]);
            return 0;
        }
        smaller += size[2] < size[0];
        for (size_t at = 0; at < size[2];)
        {
            uint16_t half = words_at(words[2], at);
            shorts += (half & 3) == 1 && (half >> 13) >= 5;  /* c.j c.beqz c.bnez */
            at += (half & 3) == 3 ? 4 : 2;
        }

        /* One byte too few: nothing changes in the parent. */
        static uint8_t parent[1024];
        memset(parent, 0x5a, sizeof(parent));
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, parent, size[2] - 1);
        if (gpsp_rv32_emit_arm_memory(&code, opcodes[i], 0x08000100) !=
                GPSP_RV32_ARM_MEMORY_BUFFER_FULL || code.ptr != parent)
            return 0;
        for (size_t at = 0; at < sizeof(parent); at++)
            if (parent[at] != 0x5a)
                return 0;

        /* A forced retry that fails, by a full parent or a staging buffer that
           holds only the short-branch output. */
        if (size[2] < size[0] &&
            (!short_branch_failure(opcodes[i], size[0] - 1, 0) ||
                    !short_branch_failure(opcodes[i], 1024, size[2])))
            return 0;
    }
    printf("RV32 ARM MEMORY SHORT BRANCH: short=%u smaller=%u\n", shorts,
            smaller);
    return gpsp_rv32_test_compress ? shorts && smaller : !shorts && !smaller;
}

/* Check the transfer's Zcb form, extension and store width, plus retry. */
static int test_zcb_transfers(void *memory)
{
    static const struct
    {
        uint32_t opcode, address, expected;
        uint16_t form, mask;
    } cases[] = {
        { 0xe5c10000, 0x03008001, 0, 0x8800, 0xfc03 },       /* STRB, c.sb */
        { 0xe1c100b0, 0x02000102, 0, 0x8c00, 0xfc43 },       /* STRH, c.sh */
        { 0xe5d10000, 0x03008003, 0x80, 0x8000, 0xfc03 },    /* LDRB, c.lbu */
        { 0xe1d100b0, 0x02000104, 0x8000, 0x8400, 0xfc43 },  /* LDRH, c.lhu */
        { 0xe1d100f0, 0x03008006, 0xffff8000, 0x8440, 0xfc43 }, /* LDRSH, c.lh */
    };
    unsigned forms = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        static uint32_t words[2][256];
        size_t size[2];
        for (unsigned mode = 0; mode < 2; mode++)
        {
            gpsp_rv32_code code;
            gpsp_rv32_code_init(&code, words[mode], sizeof(words[mode]));
            gpsp_rv32_test_arm_short_branch = mode == 1;
            gpsp_rv32_test_short_reach = mode == 1 ? 8 : 0;
            if (gpsp_rv32_emit_arm_memory(&code, cases[i].opcode, 0x08000100) !=
                GPSP_RV32_ARM_MEMORY_EMITTED)
                return 0;
            size[mode] = (size_t)(code.ptr - code.start);
        }
        gpsp_rv32_test_arm_short_branch = true;
        gpsp_rv32_test_short_reach = 0;
        if (size[1] != size[0] || memcmp(words[1], words[0], size[0]))
        {
            printf("Zcb retry %08lx differs\n", (unsigned long)cases[i].opcode);
            return 0;
        }
        uint32_t regs[64];
        init_regs(regs);
        regs[0] = 0x12345678;
        regs[1] = cases[i].address;
        uint8_t *ram = cases[i].address >> 24 == 3 ?
                &iwram[0x8000 + (cases[i].address & 0x7fff)] :
                &ewram[cases[i].address & 0x3ffff];
        memset(ram - 2, 0xa5, 6);
        bool store = !(cases[i].opcode & (1u << 20));
        bool half_word = (cases[i].opcode & 0x0e0000f0) == 0x000000b0 ||
                (cases[i].opcode & 0x0e0000f0) == 0x000000f0;
        if (!store)
        {
            ram[0] = (uint8_t)cases[i].expected;
            ram[1] = (uint8_t)(cases[i].expected >> 8);
        }
        golden_quiet = true;
        size_t bytes = emit(memory, cases[i].opcode, 0);
        golden_quiet = false;
        if (!bytes)
            return 0;
        unsigned found = 0;
        for (size_t at = 0; at < bytes;)
        {
            uint16_t half = words_at(memory, at);
            /* Match the transfer of r0, not a tag-check load. */
            found += (half & cases[i].mask) == cases[i].form &&
                    8 + ((half >> 2) & 7) == guest_reg_map[0];
            at += (half & 3) == 3 ? 4 : 2;
        }
        if (!!found != !!(gpsp_rv32_test_compress && GPSP_RV32_USE_ZCB))
        {
            printf("Zcb transfer %08lx: %u\n", (unsigned long)cases[i].opcode,
                    found);
            return 0;
        }
        forms += found;
        gpsp_rv32_run_block(regs, 100, memory);
        if (store)
        {
            uint8_t expect[6] = { 0xa5, 0xa5, 0x78, 0xa5, 0xa5, 0xa5 };
            if (half_word)
                expect[3] = 0x56;
            if (memcmp(ram - 2, expect, sizeof(expect)))
            {
                printf("Zcb store %08lx: %02x %02x %02x %02x %02x %02x\n",
                        (unsigned long)cases[i].opcode, ram[-2], ram[-1], ram[0],
                        ram[1], ram[2], ram[3]);
                return 0;
            }
        }
        else if (regs[0] != cases[i].expected)
        {
            printf("Zcb load %08lx: r0=%08lx\n", (unsigned long)cases[i].opcode,
                    (unsigned long)regs[0]);
            return 0;
        }
    }
    printf("RV32 ARM MEMORY ZCB: forms=%u\n", forms);
    return 1;
}

static int op_fixed(unsigned begin, unsigned end)
{
    for (unsigned j = 0; j < span_count; j++)
        if (spans[j][0] <= begin && end <= spans[j][1])
            return 1;
    return 0;
}

static int op_overlaps_span(unsigned begin, unsigned end)
{
    for (unsigned j = 0; j < span_count; j++)
        if (begin < spans[j][1] && spans[j][0] < end)
            return 1;
    return 0;
}

/* Check four emitted ops without executing or dumping them. */
static int check_flow_spans(void *memory, uint32_t pc, const uint32_t *ops,
        const int *want_fixed, gpsp_rv32_arm_flow_layout *layout)
{
    gpsp_rv32_arm_flow_ir ir;
    uint8_t *ptr = memory;
    span_buffer = memory;
    span_count = 0;
    gpsp_rv32_test_fixed_hook = record_span;
    int emitted = gpsp_rv32_arm_flow_full_decode(pc, ops, 4, 1, &ir) &&
            gpsp_rv32_arm_flow_full_emit(&ir, &ptr,
                    (uint8_t *)memory + CODE_SIZE, layout);
    gpsp_rv32_test_fixed_hook = NULL;
    if (!emitted)
        return 0;
    for (unsigned i = 0; i < 4; i++)
    {
        unsigned begin = layout->op_offset[i];
        unsigned end = begin + layout->op_bytes[i];
        int fixed = op_fixed(begin, end);
        if (!layout->op_mode[i] || fixed != want_fixed[i] ||
            (!fixed && op_overlaps_span(begin, end)))
        {
            printf("ARM flow op %u at %08lx: mode=%u fixed=%d\n", i,
                    (unsigned long)(pc + i * 4), layout->op_mode[i], fixed);
            return 0;
        }
    }
    return 1;
}

/* Only ops that a RAM code write patches in place keep their length. */
static int test_flow_fixed_spans(void *memory)
{
    const uint32_t ops[] = {
        0xe2800001,  /* ADD r0, r0, #1: new imm8 in place */
        0xe0811000,  /* ADD r1, r1, r0 */
        0xe1a02000,  /* MOV r2, r0 */
        0xea000000,  /* B pc + 8 */
    };
    static const int ram_fixed[4] = {1, 0, 0, 1};
    static const int rom_fixed[4] = {0, 0, 0, 0};
    gpsp_rv32_arm_flow_layout layout;
    if (!check_flow_spans(memory, 0x08001000, ops, rom_fixed, &layout) ||
        !check_flow_spans(memory, 0x03001000, ops, ram_fixed, &layout) ||
            layout.op_mode[0] != GPSP_RV32_ARM_FLOW_OP_PLAIN ||
            layout.op_mode[3] != GPSP_RV32_ARM_FLOW_OP_BRANCH)
        return 0;

    /* Repatching emits the whole op inside a fixed span. */
    uint8_t out[256];
    hook_words = free_words = 0;
    gpsp_rv32_test_emit_hook = count_free;
    int patched = gpsp_rv32_arm_flow_repatch(
            (uint8_t *)memory + layout.op_offset[0], layout.op_bytes[0],
            layout.op_mode[0], 0x03001000, ops[0], 0xe2800002, out);
    gpsp_rv32_test_emit_hook = NULL;
    return patched && hook_words && !free_words;
}

static int test_flow(void *memory)
{
    const uint32_t pc = 0x03001000;
    uint32_t ops[32], regs[64];
    gpsp_rv32_arm_flow_layout layout;
    ws_cyc_seq[12][1] = 18;
    ws_cyc_nseq[12][1] = 19;
    gamepak_size = sizeof(rom_page);
    memory_map_read[0x0c000000 >> 15] = rom_page;
    const uint32_t value = 0x76543210;
    memcpy(rom_page, &value, sizeof(value));

    for (unsigned i = 0; i < 10; i++)
        ops[i] = i < 3 ? 0xe5901000 : 0xe1a04004;  /* LDR; MOV */
    init_regs(regs);
    regs[15] = pc;
    regs[0] = 0x0c000000;
    if (!emit_flow(memory, pc, ops, 10, &layout) ||
        (int32_t)gpsp_rv32_run_block(regs, 1, memory) != -66 ||
            regs[15] != pc + 40 || regs[1] != value)
        return 0;

    const uint32_t loads[] = {0xe5901000, 0xe5d01000, 0xe1d010b0};
    for (unsigned kind = 0; kind < 3; kind++)
    {
        for (unsigned i = 0; i < 32; i++)
            ops[i] = loads[kind];
        init_regs(regs);
        regs[15] = pc;
        regs[0] = 0x02000100;
        if (!emit_flow(memory, pc, ops, 32, &layout) ||
            layout.region_count != (kind ? 2 : 4) ||
                (int32_t)gpsp_rv32_run_block(regs, 1000, memory) !=
                        (kind ? 872 : 776) || regs[15] != pc + 128)
            return 0;
        printf("ARM flow load%u x32: regions=%u bytes=%u\n",
                kind, layout.region_count, layout.code_bytes);
        if (!kind)
        {
            init_regs(regs);
            regs[15] = pc;
            regs[0] = 0x0c000000;
            if ((int32_t)gpsp_rv32_run_block(regs, 1, memory) != -179 ||
                regs[15] != pc + 36 || regs[1] != value)
                return 0;
        }
    }

    for (unsigned i = 0; i < 32; i++)
        ops[i] = 0xe8900006;  /* LDMIA r0,{r1,r2} */
    init_regs(regs);
    regs[15] = pc;
    regs[0] = 0x02000100;
    if (!emit_flow(memory, pc, ops, 32, &layout) ||
        layout.region_count != 8 ||
            (int32_t)gpsp_rv32_run_block(regs, 1000, memory) != 584 ||
            regs[15] != pc + 128)
        return 0;
    printf("ARM flow LDM2 x32: regions=%u bytes=%u\n",
            layout.region_count, layout.code_bytes);

    ops[0] = 0xe8901ffe;  /* LDMIA r0,{r1-r12}: 72 RAM cycles plus fetch. */
    ops[1] = 0xe3a0e001;  /* A new guard must stop before MOV lr,#1. */
    init_regs(regs);
    regs[15] = pc;
    regs[0] = 0x02000100;
    if (!emit_flow(memory, pc, ops, 2, &layout) ||
        (int32_t)gpsp_rv32_run_block(regs, 1, memory) != -72 ||
            regs[15] != pc + 4 || regs[14])
        return 0;

    ops[0] = 0xe3a04001;  /* MOV r4,#1 */
    ops[1] = 0xe890000e;  /* LDMIA r0,{r1-r3} */
    ops[2] = 0xe3a04002;  /* Must not run after the helper exit. */
    for (unsigned idle = 0; idle < 2; idle++)
    {
        idle_loop_target_pc = idle ? pc + 8 : UINT32_MAX;
        for (unsigned low = 0; low < 2; low++)
        {
            init_regs(regs);
            regs[15] = pc;
            regs[0] = 0x0c000000;
            unsigned budget = low ? 1 : 100;
            int expected = low ? -55 : idle ? 0 : 44;
            if (!emit_flow(memory, pc, ops, 3, &layout) ||
                (int32_t)gpsp_rv32_run_block(regs, budget, memory) != expected ||
                    regs[15] != pc + 8 || regs[4] != 1 || regs[1] != value)
                return 0;
        }
    }
    idle_loop_target_pc = UINT32_MAX;

    ops[0] = 0xe3520001;  /* CMP r2,#1 */
    ops[1] = 0xe7901063;  /* LDR r1,[r0,r3,RRX] */
    ops[2] = 0xe3540001;  /* CMP r4,#1 */
    init_regs(regs);
    regs[15] = pc;
    regs[0] = 0x82000100;
    regs[2] = 2;
    memcpy(ewram + 0x100, &value, sizeof(value));
    if (!emit_flow(memory, pc, ops, 3, &layout) ||
        (int32_t)gpsp_rv32_run_block(regs, 100, memory) != 91 ||
            regs[1] != value || regs[15] != pc + 12)
        return 0;
    memory_map_read[0x0c000000 >> 15] = NULL;
    return 1;
}

/* Conditional PC writes, PC loads and BX: not taken goes on to the next
   op in the block; taken leaves it. */
static int test_flow_conditional_exits(void *memory)
{
    const uint32_t pc = 0x03001000;
    uint32_t ops[4], regs[64];
    gpsp_rv32_arm_flow_layout layout;
    static const struct
    {
        uint32_t cmp, exit;
    } forms[] = {
        {0xe3520001, 0x328ff008},  /* CMP r2,#1; ADDLO pc,pc,#8 */
        {0xe3520002, 0x1490f004},  /* CMP r2,#2; LDRNE pc,[r0],#4 */
        {0xe3520002, 0x112fff1e},  /* CMP r2,#2; BXNE lr */
    };
    const uint32_t target = 0x03004000;
    memcpy(ewram + 0x200, &target, sizeof(target));
    for (unsigned form = 0; form < 3; form++)
    {
        ops[0] = forms[form].cmp;
        ops[1] = forms[form].exit;
        ops[2] = 0xe3a04001;  /* MOV r4,#1 */
        ops[3] = 0xe3a05001;  /* MOV r5,#1 */
        for (unsigned taken = 0; taken < 2; taken++)
        {
            init_regs(regs);
            regs[15] = pc;
            regs[0] = 0x02000200;
            regs[2] = taken ? 0 : 2;
            regs[14] = 0x03002001;
            if (!emit_flow(memory, pc, ops, 4, &layout))
                return 0;
            gpsp_rv32_run_block(regs, 100, memory);
            uint32_t expect_pc = !taken ? pc + 16 :
                    form == 0 ? pc + 4 + 8 + 8 : form == 1 ? target : 0x03002000;
            if (regs[15] != expect_pc || regs[4] != !taken || regs[5] != !taken ||
                regs[0] != (form == 1 && taken ? 0x02000204u : 0x02000200u) ||
                    ((regs[16] & 0x20) != 0) != (form == 2 && taken))
            {
                printf("conditional exit %u taken=%u: pc=%08lx r0=%08lx r4=%lu r5=%lu\n",
                        form, taken, (unsigned long)regs[15], (unsigned long)regs[0],
                        (unsigned long)regs[4], (unsigned long)regs[5]);
                return 0;
            }
        }

        /* A PC load from a helper address reads only when taken. */
        if (form == 1)
        {
            for (unsigned taken = 0; taken < 2; taken++)
            {
                init_regs(regs);
                regs[15] = pc;
                regs[0] = 0x0d000000;
                regs[2] = taken ? 0 : 2;
                unsigned reads = slow_reads;
                if (!emit_flow(memory, pc, ops, 4, &layout))
                    return 0;
                gpsp_rv32_run_block(regs, 100, memory);
                if (slow_reads != reads + taken || regs[4] != !taken ||
                    regs[0] != (taken ? 0x0d000004u : 0x0d000000u))
                {
                    printf("conditional PC load taken=%u: reads=%u r0=%08lx r4=%lu\n",
                            taken, slow_reads - reads, (unsigned long)regs[0],
                            (unsigned long)regs[4]);
                    return 0;
                }
            }
        }

        /* Out of budget after a not-taken exit: stop before the next op. */
        bool stopped = false;
        for (unsigned budget = 1; budget < 8; budget++)
        {
            init_regs(regs);
            regs[15] = pc;
            regs[0] = 0x02000200;
            regs[2] = 2;
            if (!emit_flow(memory, pc, ops, 4, &layout))
                return 0;
            gpsp_rv32_run_block(regs, budget, memory);
            if (regs[15] == pc + 8)
            {
                stopped = true;
                if (regs[4] || regs[0] != 0x02000200)
                    return 0;
            }
        }
        if (!stopped)
        {
            printf("conditional exit %u: no stop before the next op\n", form);
            return 0;
        }
    }
    return 1;
}

void app_main(void)
{
    uint8_t *memory = heap_caps_aligned_alloc(64, CODE_SIZE,
            MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!memory)
    {
        printf("RV32 ARM MEMORY RESULT: FAIL (allocation)\n");
        printf("RV32 ARM MEMORY COMPLETE\n");
        return;
    }
    memset(ewram, 0, sizeof(ewram));
    memset(iwram, 0, sizeof(iwram));
    ws_cyc_seq[2][1] = 6;
    ws_cyc_seq[3][1] = 1;
    ws_cyc_seq[8][1] = 2;
    ws_cyc_seq[4][1] = 2;
    ws_cyc_nseq[2][0] = 3;
    ws_cyc_nseq[2][1] = 6;
    ws_cyc_nseq[3][0] = 1;
    ws_cyc_nseq[3][1] = 1;
    ws_cyc_nseq[4][1] = 3;
    ws_cyc_nseq[5][1] = 2;
    ws_cyc_nseq[7][1] = 2;
    ws_cyc_seq[6][1] = 2;
    ws_cyc_nseq[6][0] = 1;
    ws_cyc_nseq[6][1] = 2;
    int pass = 1;
    if (!test_single(memory))
    {
        printf("FAIL: single\n");
        pass = 0;
    }
    if (!test_oam_word(memory))
    {
        printf("FAIL: OAM word\n");
        pass = 0;
    }
    if (!test_palette_word(memory))
    {
        printf("FAIL: palette word\n");
        pass = 0;
    }
    if (!test_ram_loads(memory))
    {
        printf("FAIL: RAM loads\n");
        pass = 0;
    }
    if (!test_rom_loads(memory))
    {
        printf("FAIL: ROM loads\n");
        pass = 0;
    }
    if (!test_immediate_folding(memory))
    {
        printf("FAIL: immediate folding\n");
        pass = 0;
    }
    if (!test_multi(memory))
    {
        printf("FAIL: multi\n");
        pass = 0;
    }
    if (!test_vram(memory))
    {
        printf("FAIL: VRAM\n");
        pass = 0;
    }
    if (!test_banked_multi(memory))
    {
        printf("FAIL: banked multi\n");
        pass = 0;
    }
    if (!test_edges(memory))
    {
        printf("FAIL: edges\n");
        pass = 0;
    }
    if (!test_flow(memory))
    {
        printf("FAIL: ARM flow budget\n");
        pass = 0;
    }
    if (!test_two_byte_position())
    {
        printf("FAIL: two-byte position\n");
        pass = 0;
    }
    if (!test_short_branches())
    {
        printf("FAIL: short branches\n");
        pass = 0;
    }
    if (!test_zcb_transfers(memory))
    {
        printf("FAIL: Zcb transfers\n");
        pass = 0;
    }
    if (!test_flow_fixed_spans(memory))
    {
        printf("FAIL: ARM flow fixed spans\n");
        pass = 0;
    }
    if (!test_flow_conditional_exits(memory))
    {
        printf("FAIL: ARM flow conditional exits\n");
        pass = 0;
    }
    /* Last: it adds slow accesses that earlier tests count absolutely. */
    if (!test_io(memory))
    {
        printf("FAIL: I/O\n");
        pass = 0;
    }
    printf("RV32 ARM MEMORY RESULT: %s\n", pass ? "PASS" : "FAIL");
    printf("RV32 ARM MEMORY COMPLETE\n");
    heap_caps_free(memory);
}
