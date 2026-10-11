/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>

#include "rv32_thumb_alu.h"

static gpsp_rv32_thumb_result emit_thumb_body(gpsp_rv32_code *code,
        uint16_t opcode, uint32_t pc, uint8_t needed);

#define gpsp_rv32_emit_thumb emit_thumb_body
#include "rv32_thumb_flow_full.c"
#undef gpsp_rv32_emit_thumb

uint8_t ws_cyc_seq[16][2];
uint8_t ws_cyc_nseq[16][2];
const uint32_t def_seq_cycles[16][2] = {[8] = {1, 1}};
uint32_t idle_loop_target_pc = UINT32_MAX;
void gpsp_rv32_interpreter_step_stub(void) { abort(); }

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

static bool force_body_padding;
static uint8_t *body_pad;

/* Leave the second body at 2 mod 4 to exercise its entry padding. */
static gpsp_rv32_thumb_result emit_thumb_body(gpsp_rv32_code *code,
        uint16_t opcode, uint32_t pc, uint8_t needed)
{
    gpsp_rv32_thumb_result result =
            gpsp_rv32_emit_thumb(code, opcode, pc, needed);
    if (result == GPSP_RV32_THUMB_EMITTED && force_body_padding && !body_pad)
    {
        CHECK(gpsp_rv32_code_may_compress(code));
        if (!((uintptr_t)code->ptr & 2))
        {
            const uint16_t c_nop = 0x0001;
            CHECK(code->ptr <= code->end && code->end - code->ptr >= 2);
            memcpy(code->ptr, &c_nop, sizeof(c_nop));
            code->ptr += sizeof(c_nop);
        }
        body_pad = code->ptr;
    }
    return result;
}

/* Two ADDS r0, r0, #1 in ROM after a two-byte instruction. */
static bool emit_after_prefix(bool compress, uint32_t *words,
        gpsp_rv32_thumb_flow_full_layout *layout)
{
    const uint16_t prefix = 0x0001;
    gpsp_rv32_code code;
    gpsp_rv32_thumb_flow_full_ir ir = {
        .pc = 0x08000000, .count = 2, .rom_region = true
    };
    uint8_t needed[2] = {0, 0}, lazy[2] = {0, 0};
    ir.ops[0].opcode = ir.ops[1].opcode = 0x1c40;
    memset(layout, 0, sizeof(*layout));
    body_pad = NULL;
    gpsp_rv32_code_init(&code, words, 1024);
    code.compress = compress;
    memcpy(code.ptr, &prefix, sizeof(prefix));
    code.ptr += sizeof(prefix);
    return emit_alu_region(&code, code.start, &ir, 0, 2, needed, lazy, layout);
}

int main(void)
{
    uint32_t words[256];
    gpsp_rv32_thumb_flow_full_layout layout;
    CHECK(!emit_after_prefix(false, words, &layout));

    CHECK(emit_after_prefix(true, words, &layout));
    uint16_t pad;
    memcpy(&pad, (uint8_t *)words + 2, sizeof(pad));
    CHECK(pad == 0x0001 && layout.entry_offset[0] == 4);
    CHECK(!(layout.body_offset[0] & 3) && !(layout.body_offset[1] & 3));

    force_body_padding = true;
    CHECK(emit_after_prefix(true, words, &layout));
    CHECK(body_pad && ((uintptr_t)body_pad & 3) == 2);
    CHECK(layout.body_offset[1] == body_pad - (uint8_t *)words + 2);
    memcpy(&pad, body_pad, sizeof(pad));
    CHECK(pad == 0x0001 && !(layout.body_offset[1] & 3));
    CHECK(layout.side_prefix[1] == 1 && layout.side_remaining[1] == 1);
    puts("RV32 Thumb flow alignment: ok");
    return 0;
}
