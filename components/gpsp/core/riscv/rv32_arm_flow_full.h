/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ARM_FLOW_FULL_H
#define GPSP_RV32_ARM_FLOW_FULL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPSP_RV32_ARM_FLOW_MAX_OPS 32

typedef enum
{
  GPSP_RV32_ARM_FLOW_DIRECT_ALU,
  GPSP_RV32_ARM_FLOW_OTHER,
  GPSP_RV32_ARM_FLOW_BRANCH,
} gpsp_rv32_arm_flow_kind;

typedef struct
{
  uint32_t opcode;
  uint8_t kind;
  uint8_t condition;
  uint8_t flags_read;
  uint8_t flags_write;
  uint8_t region_start;
  uint8_t entry_required;
  /* Flags live at a B target outside the chunk. */
  uint8_t target_live;
} gpsp_rv32_arm_flow_node;

typedef struct
{
  uint32_t pc;
  uint8_t count;
  /* Flags live after the last op when execution falls through. */
  uint8_t fallthrough_live;
  gpsp_rv32_arm_flow_node nodes[GPSP_RV32_ARM_FLOW_MAX_OPS];
} gpsp_rv32_arm_flow_ir;

/* How a pure op was emitted; 0 when it has no op_offset. */
enum
{
  GPSP_RV32_ARM_FLOW_OP_PLAIN = 1,
  GPSP_RV32_ARM_FLOW_OP_NO_FLAGS,    /* S dropped: flags were dead */
  GPSP_RV32_ARM_FLOW_OP_DROPPED,     /* compare with dead flags: no code */
  GPSP_RV32_ARM_FLOW_OP_BRANCH,      /* B or BL: PC update and waits,
                                        then its link slot */
  GPSP_RV32_ARM_FLOW_OP_SWITCH,      /* RAM NOP that may become B; same
                                        shape as BRANCH (switch trick) */
  GPSP_RV32_ARM_FLOW_OP_DYNAMIC,     /* imm8 loaded at run time */
};

typedef struct
{
  uint16_t entry_offset[GPSP_RV32_ARM_FLOW_MAX_OPS];
  uint16_t link_offset[GPSP_RV32_ARM_FLOW_MAX_OPS];
  uint32_t link_target[GPSP_RV32_ARM_FLOW_MAX_OPS];
  /* Native code of each pure op, for gpsp_rv32_arm_flow_repatch. */
  uint16_t op_offset[GPSP_RV32_ARM_FLOW_MAX_OPS];
  uint8_t op_bytes[GPSP_RV32_ARM_FLOW_MAX_OPS];
  uint8_t op_mode[GPSP_RV32_ARM_FLOW_MAX_OPS];
  uint16_t code_bytes;
  uint8_t link_count;
  uint8_t guest_ops;
  uint8_t helper_ops;
  uint8_t region_count;
  bool ends_block;
} gpsp_rv32_arm_flow_layout;

/* Flags live before the code; unknown control flow keeps all live. */
uint8_t gpsp_rv32_arm_flow_live_flags(uint32_t pc, const uint32_t *opcodes,
    size_t count);

/* RAM NOPs seen swapping with B; see the switch slot trick. */
bool gpsp_rv32_arm_flow_switch_known(uint32_t pc);
void gpsp_rv32_arm_flow_switch_note(uint32_t pc);
unsigned gpsp_rv32_arm_flow_switch_list(const uint32_t **slots);
void gpsp_rv32_arm_flow_switch_clear(void);

bool gpsp_rv32_arm_flow_immediate_known(uint32_t pc);
/* Count a patch attempt; byte is the host imm8 address. True once learned. */
bool gpsp_rv32_arm_flow_immediate_note(uint32_t pc, const uint8_t *byte);
void gpsp_rv32_arm_flow_immediate_clear(void);

/* B target, or UINT32_MAX when the op is not a plain B or is a switch slot. */
uint32_t gpsp_rv32_arm_flow_branch_target(const gpsp_rv32_arm_flow_ir *ir,
    unsigned index);

/* Bits in required_entry_mask force entry points. */
bool gpsp_rv32_arm_flow_full_decode(uint32_t pc, const uint32_t *opcodes,
    size_t count, uint32_t required_entry_mask, gpsp_rv32_arm_flow_ir *out);

/* Publish region heads and static branch links after the whole chunk succeeds. */
bool gpsp_rv32_arm_flow_full_emit(const gpsp_rv32_arm_flow_ir *ir,
    uint8_t **translation_ptr, uint8_t *translation_limit,
    gpsp_rv32_arm_flow_layout *layout);

/* Data processing with an 8-bit immediate (rotate 0) and no PC operand.
   Its code can take a new immediate in place. */
bool gpsp_rv32_arm_flow_imm_op(uint32_t opcode);
/* RAM code-write tracking, including dynamic and dropped ops. */
bool gpsp_rv32_arm_flow_patch_site(uint32_t pc, uint32_t opcode,
    unsigned mode);

/* Code for an op that replaces old_opcode, emitted as before in mode.
   native holds the bytes emitted for old_opcode. Writes them to out and
   returns true when the new code has the same size and does not depend on
   its address. */
bool gpsp_rv32_arm_flow_repatch(const uint8_t *native, unsigned bytes,
    unsigned mode, uint32_t pc, uint32_t old_opcode, uint32_t new_opcode,
    uint8_t *out);

#if defined(GPSP_BENCHMARK_RAM_CODE)
/* Why the last gpsp_rv32_arm_flow_repatch() returned false. */
enum
{
  GPSP_RV32_REPATCH_MODE = 1,     /* not a re-emittable mode or size */
  GPSP_RV32_REPATCH_OLD,          /* old opcode does not rebuild the code */
  GPSP_RV32_REPATCH_SIZE,         /* new opcode needs other bytes */
  GPSP_RV32_REPATCH_ADDRESS,      /* code depends on its address */
};
extern unsigned gpsp_rv32_arm_flow_repatch_failure;
#endif

#endif
