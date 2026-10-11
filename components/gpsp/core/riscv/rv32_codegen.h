/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_CODEGEN_H
#define GPSP_RV32_CODEGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Extensions the generated code may use; independent of the compiler ISA. */
#ifndef GPSP_RV32_USE_ZBA
#define GPSP_RV32_USE_ZBA 0
#endif
#ifndef GPSP_RV32_USE_ZBB
#define GPSP_RV32_USE_ZBB 0
#endif
#ifndef GPSP_RV32_USE_ZBS
#define GPSP_RV32_USE_ZBS 0
#endif
/* Default compression policy for generated code. */
#ifndef GPSP_RV32_USE_RVC
#define GPSP_RV32_USE_RVC 0
#endif
/* Zcb forms where the compression policy allows them. */
#ifndef GPSP_RV32_USE_ZCB
#define GPSP_RV32_USE_ZCB 0
#endif

/* Encoders write to two-byte aligned memory. Sync the instruction cache
 * before execution. Failure leaves bytes, cursor and policy unchanged.
 * U immediates are 20-bit fields; B/J offsets are signed byte distances
 * from the instruction.
 *
 * Policy: compressed forms are allowed only when compress is set and no
 * fixed span is open. A fixed span keeps every instruction at its full
 * length, for code that is patched in place or measured by byte distance.
 * short_branch (default off) also lets BEQ/BNE against x0 and JAL x0 use
 * c.beqz, c.bnez and c.j. A patch beyond their reach fails and sets
 * branch_overflow; the caller then emits again without short_branch. */
typedef struct
{
  uint8_t *start;
  uint8_t *ptr;
  uint8_t *end;
  bool compress;
  bool short_branch;
  bool branch_overflow;
  uint8_t fixed_depth;
} gpsp_rv32_code;

/* Cursor and policy, for an emitter that cancels its own output. */
typedef struct
{
  uint8_t *ptr;
  bool compress;
  bool short_branch;
  uint8_t fixed_depth;
} gpsp_rv32_code_mark;

/* Default policy: compress follows GPSP_RV32_USE_RVC. */
void gpsp_rv32_code_init(gpsp_rv32_code *code, void *memory, size_t size);
/* A staging buffer with the policy of parent (default policy when NULL).
 * The parent is not changed. */
void gpsp_rv32_code_child(gpsp_rv32_code *child, const gpsp_rv32_code *parent,
    void *memory, size_t size);
bool gpsp_rv32_code_may_compress(const gpsp_rv32_code *code);
#if defined(GPSP_RV32_TEST_HOOKS)
/* Tests: called for each instruction before it is stored, and when a
 * fixed span begins (true) or ends (false). */
extern void (*gpsp_rv32_test_emit_hook)(const gpsp_rv32_code *code);
extern void (*gpsp_rv32_test_fixed_hook)(const gpsp_rv32_code *code,
    bool begin);
/* Tests: the compress policy of new buffers; starts as GPSP_RV32_USE_RVC. */
extern bool gpsp_rv32_test_compress;
/* Tests: a nonzero byte distance shortens the reach of short branches. */
extern unsigned gpsp_rv32_test_short_reach;
#endif
/* Fixed spans nest; false on overflow or with no open span. */
bool gpsp_rv32_fixed_begin(gpsp_rv32_code *code);
bool gpsp_rv32_fixed_end(gpsp_rv32_code *code);
gpsp_rv32_code_mark gpsp_rv32_code_save(const gpsp_rv32_code *code);
void gpsp_rv32_code_restore(gpsp_rv32_code *code,
    const gpsp_rv32_code_mark *mark);

bool gpsp_rv32_add(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_sub(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_addi(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm);
bool gpsp_rv32_and(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_andi(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm);
bool gpsp_rv32_or(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_ori(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm);
bool gpsp_rv32_slt(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_sltu(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_xor(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_slti(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm);
bool gpsp_rv32_sltiu(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm);
bool gpsp_rv32_xori(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm);
bool gpsp_rv32_sll(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_srl(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_sra(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_slli(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned shift);
bool gpsp_rv32_srli(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned shift);
bool gpsp_rv32_srai(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned shift);
bool gpsp_rv32_andn(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_ror(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_rori(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned shift);
bool gpsp_rv32_xnor(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_sh1add(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_sh2add(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_sh3add(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
/* rd = (rs1 << shift) + rs2 for shift 1..3. */
bool gpsp_rv32_shadd(gpsp_rv32_code *code, unsigned shift, unsigned rd,
    unsigned rs1, unsigned rs2);
bool gpsp_rv32_bext(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_bexti(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned bit);
bool gpsp_rv32_lui(gpsp_rv32_code *code, unsigned rd, uint32_t upper20);
bool gpsp_rv32_auipc(gpsp_rv32_code *code, unsigned rd, uint32_t upper20);
bool gpsp_rv32_lb(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int offset);
bool gpsp_rv32_lh(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int offset);
bool gpsp_rv32_lw(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int offset);
bool gpsp_rv32_lbu(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int offset);
bool gpsp_rv32_lhu(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int offset);
bool gpsp_rv32_sb(gpsp_rv32_code *code, unsigned rs2, unsigned rs1, int offset);
bool gpsp_rv32_sh(gpsp_rv32_code *code, unsigned rs2, unsigned rs1, int offset);
bool gpsp_rv32_sw(gpsp_rv32_code *code, unsigned rs2, unsigned rs1, int offset);
bool gpsp_rv32_beq(gpsp_rv32_code *code, unsigned rs1, unsigned rs2, int offset);
bool gpsp_rv32_bne(gpsp_rv32_code *code, unsigned rs1, unsigned rs2, int offset);
bool gpsp_rv32_blt(gpsp_rv32_code *code, unsigned rs1, unsigned rs2, int offset);
bool gpsp_rv32_bge(gpsp_rv32_code *code, unsigned rs1, unsigned rs2, int offset);
bool gpsp_rv32_bltu(gpsp_rv32_code *code, unsigned rs1, unsigned rs2, int offset);
bool gpsp_rv32_bgeu(gpsp_rv32_code *code, unsigned rs1, unsigned rs2, int offset);
bool gpsp_rv32_jal(gpsp_rv32_code *code, unsigned rd, int offset);
bool gpsp_rv32_jalr(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int offset);
bool gpsp_rv32_mul(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_mulh(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_mulhsu(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_mulhu(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_div(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);
bool gpsp_rv32_rem(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2);

/* Patch B/JAL, or c.beqz/c.bnez/c.j, at a two-byte aligned site and target
   in [start, ptr]. Failure changes no bytes; a target beyond a short
   branch's reach also sets branch_overflow. */
bool gpsp_rv32_patch_branch(gpsp_rv32_code *code, uint8_t *site,
    const uint8_t *target);

/* Align with c.nop when compression is allowed. An aligned cursor needs
   no padding; failure leaves bytes, cursor and policy unchanged. */
bool gpsp_rv32_align4(gpsp_rv32_code *code);

#endif
