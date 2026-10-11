/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_codegen.h"

#include <string.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  #error "RV32 code generation requires little-endian instruction storage"
#endif

static bool valid_regs(unsigned rd, unsigned rs1, unsigned rs2)
{
  return rd < 32 && rs1 < 32 && rs2 < 32;
}

static bool valid_i_imm(int imm)
{
  return imm >= -2048 && imm <= 2047;
}

static bool valid_b_imm(int offset)
{
  return offset >= -4096 && offset <= 4094 && !(offset & 1);
}

static bool valid_j_imm(int offset)
{
  return offset >= -1048576 && offset <= 1048574 && !(offset & 1);
}

#if defined(GPSP_RV32_TEST_HOOKS)
void (*gpsp_rv32_test_emit_hook)(const gpsp_rv32_code *code);
void (*gpsp_rv32_test_fixed_hook)(const gpsp_rv32_code *code, bool begin);
bool gpsp_rv32_test_compress = GPSP_RV32_USE_RVC;
unsigned gpsp_rv32_test_short_reach;
#define DEFAULT_COMPRESS gpsp_rv32_test_compress
#else
#define DEFAULT_COMPRESS GPSP_RV32_USE_RVC
#endif

/* x8-x15 as a 3-bit register field, or 8 when the register has none. */
static unsigned compact_reg(unsigned reg)
{
  return reg >= 8 && reg <= 15 ? reg - 8 : 8;
}

/* Select compressed forms; keep B/JAL full-length. */
static bool compressed_form(uint32_t word, uint16_t *half)
{
  unsigned opcode = word & 0x7f;
  unsigned rd = (word >> 7) & 31;
  unsigned funct3 = (word >> 12) & 7;
  unsigned rs1 = (word >> 15) & 31;
  unsigned rs2 = (word >> 20) & 31;
  int imm = (int32_t)word >> 20;
  unsigned imm6 = (((unsigned)imm >> 5) & 1) << 12 | ((unsigned)imm & 31) << 2;
  unsigned rd_c = compact_reg(rd), rs1_c = compact_reg(rs1);
  unsigned rs2_c = compact_reg(rs2);

  if (opcode == 0x13 && funct3 == 0)
  {
    if (!rd && !rs1 && !imm)
      *half = 0x0001;                                         /* c.nop */
    else if (rd && !rs1 && imm >= -32 && imm <= 31)
      *half = (uint16_t)(0x4001 | imm6 | rd << 7);            /* c.li */
    else if (rd && rd == rs1 && imm && imm >= -32 && imm <= 31)
      *half = (uint16_t)(0x0001 | imm6 | rd << 7);            /* c.addi */
    else if (rd && rs1 && !imm)
      *half = (uint16_t)(0x8002 | rd << 7 | rs1 << 2);        /* c.mv */
    else
      return false;
    return true;
  }
  if (opcode == 0x13 && funct3 == 1 && !(word >> 25) && rd && rd == rs1 &&
      rs2)
  {
    *half = (uint16_t)(0x0002 | rd << 7 | rs2 << 2);          /* c.slli */
    return true;
  }
  if (opcode == 0x33 && funct3 == 0 && !(word >> 25) && rd)
  {
    if (!rs1 && rs2)
      *half = (uint16_t)(0x8002 | rd << 7 | rs2 << 2);        /* c.mv */
    else if (rs1 == rd && rs2)
      *half = (uint16_t)(0x9002 | rd << 7 | rs2 << 2);        /* c.add */
    else if (rs2 == rd && rs1)
      *half = (uint16_t)(0x9002 | rd << 7 | rs1 << 2);        /* c.add */
    else
      return false;
    return true;
  }
  if (opcode == 0x37 && rd && rd != 2)
  {
    uint32_t upper = word >> 12;
    if (!upper || (upper > 31 && upper < 0xfffe0))
      return false;
    *half = (uint16_t)(0x6001 | ((upper >> 5) & 1) << 12 | rd << 7 |
        (upper & 31) << 2);                                   /* c.lui */
    return true;
  }
  if (opcode == 0x67 && funct3 == 0 && !imm && rs1 && rd <= 1)
  {
    *half = (uint16_t)((rd ? 0x9002 : 0x8002) | rs1 << 7);    /* c.jalr, c.jr */
    return true;
  }

  /* Forms limited to x8-x15. */
  if (opcode == 0x13 && funct3 == 5 && rd == rs1 && rd_c < 8 && rs2 &&
      (!(word >> 25) || (word >> 25) == 0x20))
  {
    *half = (uint16_t)(((word >> 25) ? 0x8401 : 0x8001) | rd_c << 7 |
        rs2 << 2);                                            /* c.srli, c.srai */
    return true;
  }
  if (opcode == 0x13 && funct3 == 7 && rd == rs1 && rd_c < 8 &&
      imm >= -32 && imm <= 31)
  {
    *half = (uint16_t)(0x8801 | imm6 | rd_c << 7);            /* c.andi */
    return true;
  }
  if (opcode == 0x33 && rd_c < 8 &&
      (((word >> 25) == 0x20 && funct3 == 0) ||
       (!(word >> 25) && (funct3 == 4 || funct3 == 6 || funct3 == 7))))
  {
    static const uint16_t forms[8] =
        {0x8c01, 0, 0, 0, 0x8c21, 0, 0x8c41, 0x8c61};
    unsigned other = rs1 == rd ? rs2_c : 8;
    if (other == 8 && funct3 && rs2 == rd)
      other = rs1_c;                          /* commutative: rd = rs2 op rs1 */
    if (other == 8)
      return false;
    *half = (uint16_t)(forms[funct3] | rd_c << 7 | other << 2);
    return true;                              /* c.sub, c.xor, c.or, c.and */
  }
  if ((opcode == 0x03 || opcode == 0x23) && funct3 == 2 && rs1_c < 8)
  {
    unsigned data = opcode == 0x03 ? rd_c : rs2_c;
    unsigned offset = opcode == 0x03 ? (unsigned)imm :
        ((word >> 25) << 5) | ((word >> 7) & 31);
    if (data == 8 || offset > 124 || (offset & 3))
      return false;
    *half = (uint16_t)((opcode == 0x03 ? 0x4000 : 0xc000) |
        ((offset >> 3) & 7) << 10 | rs1_c << 7 | ((offset >> 2) & 1) << 6 |
        ((offset >> 6) & 1) << 5 | data << 2);                /* c.lw, c.sw */
    return true;
  }
#if GPSP_RV32_USE_ZCB
  /* Zcb, also limited to x8-x15. */
  if (rs1_c < 8 && ((opcode == 0x03 && (funct3 == 1 || funct3 == 4 ||
      funct3 == 5)) || (opcode == 0x23 && funct3 <= 1)))
  {
    unsigned data = opcode == 0x03 ? rd_c : rs2_c;
    unsigned offset = opcode == 0x03 ? (unsigned)imm :
        ((word >> 25) << 5) | ((word >> 7) & 31);
    bool byte = !(funct3 & 3);
    if (data == 8 || offset > 3 || (!byte && (offset & 1)))
      return false;
    static const uint16_t forms[6] = {0x8800, 0x8c00, 0, 0, 0x8000, 0x8400};
    uint16_t form = opcode == 0x23 ? forms[funct3] :
        funct3 == 1 ? 0x8440 : forms[funct3];
    *half = (uint16_t)(form | rs1_c << 7 | (byte ? (offset & 1) << 6 : 0) |
        ((offset >> 1) & 1) << 5 | data << 2);
    return true;                  /* c.sb, c.sh, c.lh, c.lbu, c.lhu */
  }
  if (opcode == 0x13 && rd == rs1 && rd_c < 8 &&
      ((funct3 == 7 && imm == 255) || (funct3 == 4 && imm == -1)))
  {
    *half = (uint16_t)((funct3 == 7 ? 0x9c61 : 0x9c75) | rd_c << 7);
    return true;                                    /* c.zext.b, c.not */
  }
  if (opcode == 0x33 && funct3 == 0 && (word >> 25) == 1 && rd_c < 8)
  {
    unsigned other = rs1 == rd ? rs2_c : rs2 == rd ? rs1_c : 8;
    if (other == 8)
      return false;
    *half = (uint16_t)(0x9c41 | rd_c << 7 | other << 2);       /* c.mul */
    return true;
  }
#endif
  return false;
}

static bool emit_word(gpsp_rv32_code *code, uint32_t word)
{
  if (!code || !code->ptr || ((uintptr_t)code->ptr & 1) ||
      code->ptr > code->end)
    return false;
  uint16_t half;
  if (gpsp_rv32_code_may_compress(code) && compressed_form(word, &half))
  {
    if ((size_t)(code->end - code->ptr) < sizeof(half))
      return false;
#if defined(GPSP_RV32_TEST_HOOKS)
    if (gpsp_rv32_test_emit_hook)
      gpsp_rv32_test_emit_hook(code);
#endif
    memcpy(code->ptr, &half, sizeof(half));
    code->ptr += sizeof(half);
    return true;
  }
  if ((size_t)(code->end - code->ptr) < sizeof(word))
    return false;
#if defined(GPSP_RV32_TEST_HOOKS)
  if (gpsp_rv32_test_emit_hook)
    gpsp_rv32_test_emit_hook(code);
#endif

  /* RV32I instructions are 32-bit little-endian words. */
  memcpy(code->ptr, &word, sizeof(word));
  code->ptr += sizeof(word);
  return true;
}

static bool emit_half(gpsp_rv32_code *code, uint16_t half)
{
  if (!code || !code->ptr || ((uintptr_t)code->ptr & 1) ||
      code->ptr > code->end || (size_t)(code->end - code->ptr) < sizeof(half))
    return false;
#if defined(GPSP_RV32_TEST_HOOKS)
  if (gpsp_rv32_test_emit_hook)
    gpsp_rv32_test_emit_hook(code);
#endif
  memcpy(code->ptr, &half, sizeof(half));
  code->ptr += sizeof(half);
  return true;
}

/* c.beqz/c.bnez offset bits; offset is -256..254 and even. */
static uint16_t cb_imm_bits(int offset)
{
  unsigned imm = (unsigned)offset & 0x1ff;
  return (uint16_t)(((imm >> 8) & 1) << 12 | ((imm >> 3) & 3) << 10 |
      ((imm >> 6) & 3) << 5 | ((imm >> 1) & 3) << 3 | ((imm >> 5) & 1) << 2);
}

/* c.j offset bits; offset is -2048..2046 and even. */
static uint16_t cj_imm_bits(int offset)
{
  unsigned imm = (unsigned)offset & 0xfff;
  return (uint16_t)(((imm >> 11) & 1) << 12 | ((imm >> 4) & 1) << 11 |
      ((imm >> 8) & 3) << 9 | ((imm >> 10) & 1) << 8 |
      ((imm >> 6) & 1) << 7 | ((imm >> 7) & 1) << 6 |
      ((imm >> 1) & 7) << 3 | ((imm >> 5) & 1) << 2);
}

static bool short_branch_allowed(const gpsp_rv32_code *code)
{
  return code && code->short_branch && gpsp_rv32_code_may_compress(code);
}

void gpsp_rv32_code_init(gpsp_rv32_code *code, void *memory, size_t size)
{
  code->start = memory;
  code->ptr = memory;
  code->end = memory ? code->ptr + size : NULL;
  code->compress = DEFAULT_COMPRESS;
  code->short_branch = false;
  code->branch_overflow = false;
  code->fixed_depth = 0;
}

bool gpsp_rv32_align4(gpsp_rv32_code *code)
{
  const uint16_t c_nop = 0x0001;
  if (!code || !code->ptr || ((uintptr_t)code->ptr & 1) ||
      code->ptr > code->end)
    return false;
  if (!((uintptr_t)code->ptr & 2))
    return true;
  if (!gpsp_rv32_code_may_compress(code) ||
      (size_t)(code->end - code->ptr) < sizeof(c_nop))
    return false;
#if defined(GPSP_RV32_TEST_HOOKS)
  if (gpsp_rv32_test_emit_hook)
    gpsp_rv32_test_emit_hook(code);
#endif
  memcpy(code->ptr, &c_nop, sizeof(c_nop));
  code->ptr += sizeof(c_nop);
  return true;
}

void gpsp_rv32_code_child(gpsp_rv32_code *child, const gpsp_rv32_code *parent,
    void *memory, size_t size)
{
  gpsp_rv32_code_init(child, memory, size);
  if (parent)
  {
    child->compress = parent->compress;
    child->short_branch = parent->short_branch;
    child->fixed_depth = parent->fixed_depth;
  }
}

bool gpsp_rv32_code_may_compress(const gpsp_rv32_code *code)
{
  return code && code->compress && !code->fixed_depth;
}

bool gpsp_rv32_fixed_begin(gpsp_rv32_code *code)
{
  if (!code || code->fixed_depth == UINT8_MAX)
    return false;
  code->fixed_depth++;
#if defined(GPSP_RV32_TEST_HOOKS)
  if (gpsp_rv32_test_fixed_hook)
    gpsp_rv32_test_fixed_hook(code, true);
#endif
  return true;
}

bool gpsp_rv32_fixed_end(gpsp_rv32_code *code)
{
  if (!code || !code->fixed_depth)
    return false;
  code->fixed_depth--;
#if defined(GPSP_RV32_TEST_HOOKS)
  if (gpsp_rv32_test_fixed_hook)
    gpsp_rv32_test_fixed_hook(code, false);
#endif
  return true;
}

gpsp_rv32_code_mark gpsp_rv32_code_save(const gpsp_rv32_code *code)
{
  gpsp_rv32_code_mark mark =
  {
    code->ptr, code->compress, code->short_branch, code->fixed_depth
  };
  return mark;
}

void gpsp_rv32_code_restore(gpsp_rv32_code *code,
    const gpsp_rv32_code_mark *mark)
{
  code->ptr = mark->ptr;
  code->compress = mark->compress;
  code->short_branch = mark->short_branch;
  code->fixed_depth = mark->fixed_depth;
}

static bool emit_r(gpsp_rv32_code *code, unsigned rd, unsigned rs1,
    unsigned rs2, unsigned funct3, unsigned funct7)
{
  if (!valid_regs(rd, rs1, rs2))
    return false;

  return emit_word(code, (funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
      (funct3 << 12) | (rd << 7) | 0x33);
}

static bool emit_i(gpsp_rv32_code *code, unsigned rd, unsigned rs1,
    int imm, unsigned funct3, unsigned opcode)
{
  if (!valid_regs(rd, rs1, 0) || !valid_i_imm(imm))
    return false;

  return emit_word(code, (((uint32_t)imm & 0xfff) << 20) | (rs1 << 15) |
      (funct3 << 12) | (rd << 7) | opcode);
}

static bool emit_s(gpsp_rv32_code *code, unsigned rs2, unsigned rs1,
    int offset, unsigned funct3)
{
  if (!valid_regs(0, rs1, rs2) || !valid_i_imm(offset))
    return false;

  uint32_t imm = (uint32_t)offset & 0xfff;
  return emit_word(code, ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) |
      (funct3 << 12) | ((imm & 0x1f) << 7) | 0x23);
}

/* Branch immediates occupy split instruction fields. */
static uint32_t b_imm_bits(int offset)
{
  uint32_t imm = (uint32_t)offset & 0x1fff;
  return ((imm & 0x1000) << 19) | ((imm & 0x7e0) << 20) |
      ((imm & 0x1e) << 7) | ((imm & 0x800) >> 4);
}

static bool emit_b(gpsp_rv32_code *code, unsigned rs1, unsigned rs2,
    int offset, unsigned funct3)
{
  if (!valid_regs(0, rs1, rs2) || !valid_b_imm(offset))
    return false;

  /* c.beqz/c.bnez compare one x8-x15 register with x0. */
  unsigned reg = !rs2 ? rs1 : !rs1 ? rs2 : 0;
  if (short_branch_allowed(code) && (funct3 == 0 || funct3 == 1) &&
      compact_reg(reg) < 8 && offset >= -256 && offset <= 254)
    return emit_half(code, (uint16_t)((funct3 ? 0xe001 : 0xc001) |
        compact_reg(reg) << 7 | cb_imm_bits(offset)));    /* c.beqz, c.bnez */
  return emit_word(code, b_imm_bits(offset) | (rs2 << 20) | (rs1 << 15) |
      (funct3 << 12) | 0x63);
}

static uint32_t j_imm_bits(int offset)
{
  uint32_t imm = (uint32_t)offset & 0x1fffff;
  return ((imm & 0x100000) << 11) | ((imm & 0x7fe) << 20) |
      ((imm & 0x800) << 9) | (imm & 0xff000);
}

static bool emit_u(gpsp_rv32_code *code, unsigned rd, uint32_t upper20,
    unsigned opcode)
{
  if (rd >= 32 || upper20 > 0xfffff)
    return false;

  return emit_word(code, (upper20 << 12) | (rd << 7) | opcode);
}

bool gpsp_rv32_add(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2)
{
  return emit_r(code, rd, rs1, rs2, 0, 0);
}

bool gpsp_rv32_addi(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm)
{
  return emit_i(code, rd, rs1, imm, 0, 0x13);
}

bool gpsp_rv32_and(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2)
{
  return emit_r(code, rd, rs1, rs2, 7, 0);
}

bool gpsp_rv32_or(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2)
{
  return emit_r(code, rd, rs1, rs2, 6, 0);
}

bool gpsp_rv32_sltu(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2)
{
  return emit_r(code, rd, rs1, rs2, 3, 0);
}

bool gpsp_rv32_xor(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned rs2)
{
  return emit_r(code, rd, rs1, rs2, 4, 0);
}

bool gpsp_rv32_sltiu(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm)
{
  return emit_i(code, rd, rs1, imm, 3, 0x13);
}

bool gpsp_rv32_xori(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int imm)
{
  return emit_i(code, rd, rs1, imm, 4, 0x13);
}

bool gpsp_rv32_slli(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned shift)
{
  return shift < 32 && emit_i(code, rd, rs1, shift, 1, 0x13);
}

bool gpsp_rv32_srli(gpsp_rv32_code *code, unsigned rd, unsigned rs1, unsigned shift)
{
  return shift < 32 && emit_i(code, rd, rs1, shift, 5, 0x13);
}

bool gpsp_rv32_sw(gpsp_rv32_code *code, unsigned rs2, unsigned rs1, int offset)
{
  return emit_s(code, rs2, rs1, offset, 2);
}

bool gpsp_rv32_jalr(gpsp_rv32_code *code, unsigned rd, unsigned rs1, int offset)
{
  return emit_i(code, rd, rs1, offset, 0, 0x67);
}

#define DEFINE_R(name, funct3, funct7) \
  bool gpsp_rv32_##name(gpsp_rv32_code *code, unsigned rd, unsigned rs1, \
      unsigned rs2) \
  { \
    return emit_r(code, rd, rs1, rs2, funct3, funct7); \
  }

#define DEFINE_I(name, funct3, opcode) \
  bool gpsp_rv32_##name(gpsp_rv32_code *code, unsigned rd, unsigned rs1, \
      int imm) \
  { \
    return emit_i(code, rd, rs1, imm, funct3, opcode); \
  }

#define DEFINE_STORE(name, funct3) \
  bool gpsp_rv32_##name(gpsp_rv32_code *code, unsigned rs2, unsigned rs1, \
      int offset) \
  { \
    return emit_s(code, rs2, rs1, offset, funct3); \
  }

#define DEFINE_BRANCH(name, funct3) \
  bool gpsp_rv32_##name(gpsp_rv32_code *code, unsigned rs1, unsigned rs2, \
      int offset) \
  { \
    return emit_b(code, rs1, rs2, offset, funct3); \
  }

DEFINE_R(sub, 0, 0x20)
DEFINE_R(sll, 1, 0)
DEFINE_R(slt, 2, 0)
DEFINE_R(srl, 5, 0)
DEFINE_R(sra, 5, 0x20)
DEFINE_R(mul, 0, 1)
DEFINE_R(mulh, 1, 1)
DEFINE_R(mulhsu, 2, 1)
DEFINE_R(mulhu, 3, 1)
DEFINE_R(div, 4, 1)
DEFINE_R(rem, 6, 1)

DEFINE_R(andn, 7, 0x20)
DEFINE_R(ror, 5, 0x30)
DEFINE_R(xnor, 4, 0x20)
DEFINE_R(sh1add, 2, 0x10)
DEFINE_R(sh2add, 4, 0x10)
DEFINE_R(sh3add, 6, 0x10)
DEFINE_R(bext, 5, 0x24)

DEFINE_I(slti, 2, 0x13)
DEFINE_I(ori, 6, 0x13)
DEFINE_I(andi, 7, 0x13)
DEFINE_I(lb, 0, 0x03)
DEFINE_I(lh, 1, 0x03)
DEFINE_I(lw, 2, 0x03)
DEFINE_I(lbu, 4, 0x03)
DEFINE_I(lhu, 5, 0x03)

DEFINE_STORE(sb, 0)
DEFINE_STORE(sh, 1)

DEFINE_BRANCH(beq, 0)
DEFINE_BRANCH(bne, 1)
DEFINE_BRANCH(blt, 4)
DEFINE_BRANCH(bge, 5)
DEFINE_BRANCH(bltu, 6)
DEFINE_BRANCH(bgeu, 7)

#undef DEFINE_R
#undef DEFINE_I
#undef DEFINE_STORE
#undef DEFINE_BRANCH

bool gpsp_rv32_srai(gpsp_rv32_code *code, unsigned rd, unsigned rs1,
    unsigned shift)
{
  return shift < 32 && emit_i(code, rd, rs1, (0x20 << 5) | shift, 5, 0x13);
}

bool gpsp_rv32_rori(gpsp_rv32_code *code, unsigned rd, unsigned rs1,
    unsigned shift)
{
  return shift < 32 && emit_i(code, rd, rs1, (0x30 << 5) | shift, 5, 0x13);
}

bool gpsp_rv32_bexti(gpsp_rv32_code *code, unsigned rd, unsigned rs1,
    unsigned bit)
{
  return bit < 32 && emit_i(code, rd, rs1, (0x24 << 5) | bit, 5, 0x13);
}

bool gpsp_rv32_shadd(gpsp_rv32_code *code, unsigned shift, unsigned rd,
    unsigned rs1, unsigned rs2)
{
  switch (shift)
  {
    case 1: return gpsp_rv32_sh1add(code, rd, rs1, rs2);
    case 2: return gpsp_rv32_sh2add(code, rd, rs1, rs2);
    case 3: return gpsp_rv32_sh3add(code, rd, rs1, rs2);
    default: return false;
  }
}

bool gpsp_rv32_lui(gpsp_rv32_code *code, unsigned rd, uint32_t upper20)
{
  return emit_u(code, rd, upper20, 0x37);
}

bool gpsp_rv32_auipc(gpsp_rv32_code *code, unsigned rd, uint32_t upper20)
{
  return emit_u(code, rd, upper20, 0x17);
}

bool gpsp_rv32_jal(gpsp_rv32_code *code, unsigned rd, int offset)
{
  if (rd >= 32 || !valid_j_imm(offset))
    return false;

  if (!rd && short_branch_allowed(code) && offset >= -2048 && offset <= 2046)
    return emit_half(code, (uint16_t)(0xa001 | cj_imm_bits(offset))); /* c.j */
  return emit_word(code, j_imm_bits(offset) | (rd << 7) | 0x6f);
}

bool gpsp_rv32_patch_branch(gpsp_rv32_code *code, uint8_t *site,
    const uint8_t *target)
{
  if (!code || !code->start || !code->ptr || !site || !target)
    return false;

  uintptr_t start = (uintptr_t)code->start;
  uintptr_t cursor = (uintptr_t)code->ptr;
  uintptr_t end = (uintptr_t)code->end;
  uintptr_t at = (uintptr_t)site;
  uintptr_t to = (uintptr_t)target;

  if ((start & 1) || cursor < start || cursor > end || cursor - start < 2 ||
      at < start || at > cursor - 2 || (at & 1) ||
      to < start || to > cursor || (to & 1))
    return false;

  uint16_t half;
  memcpy(&half, site, sizeof(half));
  if ((half & 3) != 3)
  {
    /* c.beqz/c.bnez (funct3 6/7) and c.j (5) in quadrant 1. */
    unsigned funct3 = half >> 13;
    if ((half & 3) != 1 || funct3 < 5)
      return false;
    int offset = (to >= at) ? (int)(to - at) : -(int)(at - to);
    int low = funct3 == 5 ? -2048 : -256;
#if defined(GPSP_RV32_TEST_HOOKS)
    if (gpsp_rv32_test_short_reach &&
        (int)gpsp_rv32_test_short_reach < -low)
      low = -(int)gpsp_rv32_test_short_reach;
#endif
    if (offset < low || offset > -low - 2)
    {
      code->branch_overflow = true;
      return false;
    }
    half = funct3 == 5 ? (uint16_t)((half & 0xe003) | cj_imm_bits(offset)) :
        (uint16_t)((half & 0xe383) | cb_imm_bits(offset));
    memcpy(site, &half, sizeof(half));
    return true;
  }
  if (at > cursor - 4)
    return false;

  uint32_t word;
  memcpy(&word, site, sizeof(word));
  unsigned opcode = word & 0x7f;
  int offset;
  uint32_t bits;
  uint32_t mask;

  if (opcode == 0x63)
  {
    unsigned funct3 = (word >> 12) & 7;
    if (funct3 == 2 || funct3 == 3)
      return false;
    if ((to >= at && to - at > 4094) ||
        (to < at && at - to > 4096))
      return false;
    offset = (to >= at) ? (int)(to - at) : -(int)(at - to);
    if (!valid_b_imm(offset))
      return false;
    bits = b_imm_bits(offset);
    mask = 0xfe000f80;
  }
  else if (opcode == 0x6f)
  {
    if ((to >= at && to - at > 1048574) ||
        (to < at && at - to > 1048576))
      return false;
    offset = (to >= at) ? (int)(to - at) : -(int)(at - to);
    if (!valid_j_imm(offset))
      return false;
    bits = j_imm_bits(offset);
    mask = 0xfffff000;
  }
  else
    return false;

  word = (word & ~mask) | bits;
  memcpy(site, &word, sizeof(word));
  return true;
}
