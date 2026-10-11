/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>

#include "rv32_arm_flow_full.c"

uint8_t ws_cyc_seq[16][2];
uint8_t ws_cyc_nseq[16][2];
const uint32_t def_seq_cycles[16][2] = {[3] = {1, 1}};
uint32_t idle_loop_target_pc = UINT32_MAX;
void gpsp_rv32_arm_bx_exit_shared(void) { abort(); }

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

/* BL sets LR before the advance that SMC may patch. */
static bool emit_after_prefix(bool compress, unsigned prefix_bytes,
        uint32_t pc, uint32_t opcode, uint32_t *words, gpsp_rv32_code *code,
        gpsp_rv32_arm_flow_layout *layout)
{
    const uint16_t prefix = 0x0001;
    bool terminal = false;
    CHECK(prefix_bytes == 0 || prefix_bytes == sizeof(prefix));
    memset(layout, 0, sizeof(*layout));
    gpsp_rv32_code_init(code, words, 1024);
    code->compress = compress;
    memcpy(code->ptr, &prefix, prefix_bytes);
    code->ptr += prefix_bytes;
    return emit_branch(code, code->start, layout, 0, pc, opcode, &terminal,
            NULL);
}

int main(void)
{
    uint32_t words[256];
    gpsp_rv32_code code;
    gpsp_rv32_arm_flow_layout layout;
    CHECK(!emit_after_prefix(false, 2, 0x03000000, 0xeb000000, words, &code,
            &layout));

    /* Compressed LR setup changes which starting position needs padding. */
    for (unsigned prefix_bytes = 0; prefix_bytes <= 2; prefix_bytes += 2)
    {
        CHECK(emit_after_prefix(true, prefix_bytes, 0x03000000, 0xeb000000,
                words, &code, &layout));
        CHECK(layout.op_mode[0] == GPSP_RV32_ARM_FLOW_OP_BRANCH);
        CHECK(!(layout.op_offset[0] & 3));
        CHECK(layout.link_offset[0] == layout.op_offset[0] + layout.op_bytes[0]);
        CHECK(gpsp_rv32_patch_link_slot(&code,
                code.start + layout.op_offset[0] + layout.op_bytes[0], 0x40001000));
    }

    /* ROM: not patched, so the advance right after the 8-byte guard is not
       padded. */
    CHECK(emit_after_prefix(true, 2, 0x08000000, 0xea000000, words, &code,
            &layout));
    CHECK(layout.op_offset[0] == 10);
    CHECK(!(layout.link_offset[0] & 3));
    puts("RV32 ARM flow alignment: ok");
    return 0;
}
