/* Production-style RV32 batch size for 32 straight-line Thumb MOVS.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv32_block_emit.c"

/* The host encodes these addresses but never reads the wait tables. */
#if defined(__APPLE__)
__asm__(".globl _ws_cyc_seq\n_ws_cyc_seq = 0x20001230\n"
        ".globl _ws_cyc_nseq\n_ws_cyc_nseq = 0x20001330\n");
#else
__asm__(".globl ws_cyc_seq\n.set ws_cyc_seq, 0x20001230\n"
        ".globl ws_cyc_nseq\n.set ws_cyc_nseq, 0x20001330\n");
#endif

uint32_t idle_loop_target_pc = UINT32_MAX;

gpsp_rv32_arm_alu_result gpsp_rv32_emit_arm_alu_block(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_ALU_UNSUPPORTED;
}

gpsp_rv32_arm_memory_result gpsp_rv32_emit_arm_memory(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_MEMORY_UNSUPPORTED;
}

gpsp_rv32_arm_mul_result gpsp_rv32_emit_arm_mul(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_MUL_UNSUPPORTED;
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_arm_system(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_thumb_swi(
        gpsp_rv32_code *code, uint16_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
}

const gpsp_rv32_helper_fn
        gpsp_rv32_thumb_memory_helpers[GPSP_RV32_MEM_KIND_COUNT] = {0};

#define UNUSED_RV32_ENTRY(name) void name(void) { abort(); }
UNUSED_RV32_ENTRY(gpsp_rv32_call_helper)
UNUSED_RV32_ENTRY(gpsp_rv32_interpreter_step_stub)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_s8)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_s16)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_ram)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_ram_store)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_slow_bridge)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u32_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u32_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_pop_exit_shared)
#undef UNUSED_RV32_ENTRY

enum { START_PC = 0x08000100, VIRTUAL_CODE_BASE = 0x30000000,
        SIDE_PREFLIGHT_STUB = 0x50004000, GUEST_OPS = 32 };

static uint16_t thumb_opcode(unsigned index)
{
    return 0x2000u | ((index & 7u) << 8) | (index + 1u);
}

static void require(bool condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    bool all_entries = argc == 2 && !strcmp(argv[1], "--all-entries");
    require(argc == 1 || all_entries,
            "usage: current_batch [--all-entries]");
    uint32_t words[1024];
    uint8_t *start = (uint8_t *)words;
    uint8_t *cursor = start;
    uint8_t *limit = start + sizeof(words);
    uint8_t *slow_site = NULL;
    size_t body_offsets[GUEST_OPS];
    uintptr_t wait_address = (uintptr_t)&ws_cyc_seq[(START_PC >> 24) & 15][0];
    require(gpsp_rv32_emit_thumb_preflight(&cursor, limit, wait_address,
            GUEST_OPS, &slow_site), "preflight emission failed");
    size_t preflight_bytes = (size_t)(cursor - start);

    for (unsigned i = 0; i < GUEST_OPS; i++)
    {
        bool ends_block = false;
        gpsp_rv32_block_link link;
        uint16_t opcode = thumb_opcode(i);
        body_offsets[i] = (size_t)(cursor - start);
        require(gpsp_rv32_thumb_batch_alu(opcode), "opcode left batch");
        require(gpsp_rv32_emit_block_instruction(&cursor, limit,
                START_PC + i * 2, opcode, true, true,
                i + 1 == GUEST_OPS ? 0x0f : 0, true, &ends_block, &link),
                "instruction emission failed");
        require(!ends_block && !link.site, "unexpected batch exit");
    }
    size_t body_bytes = (size_t)(cursor - start) - preflight_bytes;

    require(gpsp_rv32_emit_thumb_batch_finish(&cursor, limit, GUEST_OPS),
            "batch finish failed");
    size_t finish_bytes = (size_t)(cursor - start) - preflight_bytes - body_bytes;
    gpsp_rv32_code code;
    uint8_t *fallthrough_site = NULL;
    gpsp_rv32_code_init(&code, cursor, (size_t)(limit - cursor));
    require(gpsp_rv32_emit_link_slot(&code, &fallthrough_site),
            "fallthrough link failed");
    cursor = code.ptr;
    size_t link_bytes = (size_t)(cursor - start) - preflight_bytes -
            body_bytes - finish_bytes;
    require(fallthrough_site != NULL, "missing fallthrough link");

    uint8_t *slow = cursor;
    require(gpsp_rv32_emit_thumb_batch_slow(&cursor, limit, START_PC,
            thumb_opcode(0)), "cold single-step emission failed");
    size_t slow_bytes = (size_t)(cursor - slow);
    gpsp_rv32_code_init(&code, start, sizeof(words));
    code.ptr = cursor;
    require(gpsp_rv32_patch_link_slot(&code, slow_site,
            VIRTUAL_CODE_BASE + (uintptr_t)(slow - start)),
            "cold link patch failed");

    size_t side_adapter_bytes = 0;
    size_t side_cold_bytes = 0;
    if (all_entries)
    {
        for (unsigned i = 1; i < GUEST_OPS; i++)
        {
            uint8_t *entry = cursor;
            uint8_t *slow_literal = NULL;
            uint32_t pc = START_PC + i * 2;
            uint16_t opcode = thumb_opcode(i);
            /* The adapter encodes this address; it does not dereference it. */
            uint8_t *virtual_body = (uint8_t *)(uintptr_t)
                    (VIRTUAL_CODE_BASE + body_offsets[i]);
            require(gpsp_rv32_emit_thumb_side_entry(&cursor, limit, pc, opcode,
                    wait_address, i, GUEST_OPS - i, virtual_body,
                    SIDE_PREFLIGHT_STUB, &slow_literal),
                    "side entry emission failed");
            side_adapter_bytes += (size_t)(cursor - entry);
            require(slow_literal == entry + 16, "missing slow-path pointer");

            slow = cursor;
            require(gpsp_rv32_emit_thumb_batch_slow(&cursor, limit, pc, opcode),
                    "side cold single-step emission failed");
            side_cold_bytes += (size_t)(cursor - slow);
            uint32_t virtual_slow =
                    VIRTUAL_CODE_BASE + (uint32_t)(slow - start);
            memcpy(slow_literal, &virtual_slow, sizeof(virtual_slow));
        }
    }

    size_t total = (size_t)(cursor - start);
    require(!(total & 3), "unaligned output");
    printf("translator=current_batch guest_ops=%u entry_count=%u "
            "code_bytes=%zu rv32_words=%zu cache_header_bytes=8 "
            "cache_total_bytes=%zu\n", GUEST_OPS,
            all_entries ? GUEST_OPS : 1, total, total / 4, total + 8);
    printf("preflight_bytes=%zu body_bytes=%zu finish_bytes=%zu "
            "fallthrough_link_bytes=%zu head_cold_step_bytes=%zu "
            "side_adapter_bytes=%zu side_cold_step_bytes=%zu\n",
            preflight_bytes, body_bytes, finish_bytes, link_bytes, slow_bytes,
            side_adapter_bytes, side_cold_bytes);
    /* Pending side entries occupy alias slots before adapters are created. */
    printf("alias_entries=%u alias_payload_bytes=%zu "
            "shared_preflight_text_bytes=64\n", GUEST_OPS,
            GUEST_OPS * sizeof(uint32_t[2]));
    return 0;
}
