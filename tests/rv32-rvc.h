/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Instruction fetch for host RV32 simulators: 16-bit RVC (Zca, RV32 forms)
 * and Zcb instructions expand to their 32-bit equivalents. The caller
 * advances by the returned length, so a link register gets pc + 2 after a
 * compressed jalr. */

#ifndef RV32_RVC_H
#define RV32_RVC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t rv32_rvc_r(unsigned funct7, unsigned rs2,
        unsigned rs1, unsigned funct3, unsigned rd, unsigned opcode)
{
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
            (rd << 7) | opcode;
}

static inline uint32_t rv32_rvc_i(int imm, unsigned rs1, unsigned funct3,
        unsigned rd, unsigned opcode)
{
    return ((uint32_t)(imm & 0xfff) << 20) | (rs1 << 15) | (funct3 << 12) |
            (rd << 7) | opcode;
}

static inline uint32_t rv32_rvc_s(int imm, unsigned rs2, unsigned rs1,
        unsigned funct3)
{
    uint32_t u = (uint32_t)imm & 0xfff;
    return ((u >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
            ((u & 31) << 7) | 0x23;
}

static inline uint32_t rv32_rvc_b(int imm, unsigned rs2, unsigned rs1,
        unsigned funct3)
{
    uint32_t u = (uint32_t)imm & 0x1fff;
    return (((u >> 12) & 1) << 31) | (((u >> 5) & 63) << 25) | (rs2 << 20) |
            (rs1 << 15) | (funct3 << 12) | (((u >> 1) & 15) << 8) |
            (((u >> 11) & 1) << 7) | 0x63;
}

static inline uint32_t rv32_rvc_j(int imm, unsigned rd)
{
    uint32_t u = (uint32_t)imm & 0x1fffff;
    return (((u >> 20) & 1) << 31) | (((u >> 1) & 1023) << 21) |
            (((u >> 11) & 1) << 20) | (((u >> 12) & 255) << 12) | (rd << 7) | 0x6f;
}

static inline int rv32_rvc_sext(uint32_t value, unsigned bits)
{
    uint32_t m = 1u << (bits - 1);
    value &= (1u << bits) - 1;
    return (int)((value ^ m) - m);
}

#define RVC_BIT(h, n) (((h) >> (n)) & 1u)
#define RVC_BITS(h, hi, lo) (((h) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

/* Expands one 16-bit instruction; 0 when it is reserved or unsupported. */
static inline int rv32_rvc_expand(uint16_t h, uint32_t *word)
{
    unsigned op = h & 3, f3 = h >> 13;
    unsigned rdp = 8 + RVC_BITS(h, 4, 2), rs1p = 8 + RVC_BITS(h, 9, 7);
    unsigned rd = RVC_BITS(h, 11, 7), rs2 = RVC_BITS(h, 6, 2);
    if (h == 0)
        return 0;
    if (op == 0)
    {
        unsigned lw = (RVC_BITS(h, 12, 10) << 3) | (RVC_BIT(h, 6) << 2) |
                (RVC_BIT(h, 5) << 6);
        switch (f3)
        {
            case 0:
            {
                unsigned imm = (RVC_BITS(h, 12, 11) << 4) | (RVC_BITS(h, 10, 7) << 6) |
                        (RVC_BIT(h, 6) << 2) | (RVC_BIT(h, 5) << 3);
                if (!imm)
                    return 0;
                *word = rv32_rvc_i((int)imm, 2, 0, rdp, 0x13);      /* c.addi4spn */
                return 1;
            }
            case 2: *word = rv32_rvc_i((int)lw, rs1p, 2, rdp, 0x03); return 1;
            case 6: *word = rv32_rvc_s((int)lw, rdp, rs1p, 2); return 1;
            case 4: /* Zcb loads and stores */
            {
                unsigned f6 = RVC_BITS(h, 15, 10);
                unsigned b = (RVC_BIT(h, 5) << 1) | RVC_BIT(h, 6);
                unsigned hw = RVC_BIT(h, 5) << 1;
                if (f6 == 0x20) { *word = rv32_rvc_i((int)b, rs1p, 4, rdp, 0x03); return 1; }
                if (f6 == 0x21 && !RVC_BIT(h, 6))
                { *word = rv32_rvc_i((int)hw, rs1p, 5, rdp, 0x03); return 1; }
                if (f6 == 0x21)
                { *word = rv32_rvc_i((int)hw, rs1p, 1, rdp, 0x03); return 1; }
                if (f6 == 0x22) { *word = rv32_rvc_s((int)b, rdp, rs1p, 0); return 1; }
                if (f6 == 0x23 && !RVC_BIT(h, 6))
                { *word = rv32_rvc_s((int)hw, rdp, rs1p, 1); return 1; }
                return 0;
            }
            default: return 0;
        }
    }
    if (op == 1)
    {
        int imm6 = rv32_rvc_sext((RVC_BIT(h, 12) << 5) | RVC_BITS(h, 6, 2), 6);
        int jimm = rv32_rvc_sext((RVC_BIT(h, 12) << 11) | (RVC_BIT(h, 11) << 4) |
                (RVC_BITS(h, 10, 9) << 8) | (RVC_BIT(h, 8) << 10) |
                (RVC_BIT(h, 7) << 6) | (RVC_BIT(h, 6) << 7) |
                (RVC_BITS(h, 5, 3) << 1) | (RVC_BIT(h, 2) << 5), 12);
        switch (f3)
        {
            case 0: *word = rv32_rvc_i(imm6, rd, 0, rd, 0x13); return 1; /* c.addi */
            case 1: *word = rv32_rvc_j(jimm, 1); return 1;               /* c.jal */
            case 2:
                if (!rd)
                    return 0;
                *word = rv32_rvc_i(imm6, 0, 0, rd, 0x13);                   /* c.li */
                return 1;
            case 3:
                if (rd == 2)
                {
                    int imm = rv32_rvc_sext((RVC_BIT(h, 12) << 9) |
                            (RVC_BIT(h, 6) << 4) | (RVC_BIT(h, 5) << 6) |
                            (RVC_BITS(h, 4, 3) << 7) | (RVC_BIT(h, 2) << 5), 10);
                    if (!imm)
                        return 0;
                    *word = rv32_rvc_i(imm, 2, 0, 2, 0x13);               /* c.addi16sp */
                    return 1;
                }
                if (!rd || !imm6)
                    return 0;
                *word = ((uint32_t)imm6 << 12) | (rd << 7) | 0x37;          /* c.lui */
                return 1;
            case 4:
            {
                unsigned f2 = RVC_BITS(h, 11, 10);
                unsigned shamt = RVC_BITS(h, 6, 2);
                if (f2 == 0 || f2 == 1)
                {
                    if (RVC_BIT(h, 12) || !shamt)
                        return 0;
                    *word = rv32_rvc_i((int)((f2 ? 0x400u : 0) | shamt), rs1p, 5, rs1p,
                            0x13);                                       /* c.srli, c.srai */
                    return 1;
                }
                if (f2 == 2)
                {
                    *word = rv32_rvc_i(imm6, rs1p, 7, rs1p, 0x13);          /* c.andi */
                    return 1;
                }
                unsigned f = RVC_BITS(h, 6, 5);
                if (!RVC_BIT(h, 12))
                {
                    static const unsigned f3s[4] = {0, 4, 6, 7};
                    *word = rv32_rvc_r(f ? 0 : 0x20, rdp, rs1p, f3s[f], rs1p, 0x33);
                    return 1;                                  /* c.sub/xor/or/and */
                }
                if (f == 2)
                {
                    *word = rv32_rvc_r(1, rdp, rs1p, 0, rs1p, 0x33);         /* c.mul */
                    return 1;
                }
                if (f != 3)
                    return 0;
                switch (RVC_BITS(h, 4, 2))
                {
                    case 0: *word = rv32_rvc_i(255, rs1p, 7, rs1p, 0x13); return 1;
                    case 1: *word = rv32_rvc_i(0x604, rs1p, 1, rs1p, 0x13); return 1;
                    case 2: *word = rv32_rvc_r(4, 0, rs1p, 4, rs1p, 0x33); return 1;
                    case 3: *word = rv32_rvc_i(0x605, rs1p, 1, rs1p, 0x13); return 1;
                    case 5: *word = rv32_rvc_i(-1, rs1p, 4, rs1p, 0x13); return 1;
                    default: return 0;
                }
            }
            case 5: *word = rv32_rvc_j(jimm, 0); return 1;                 /* c.j */
            default:
            {
                int bimm = rv32_rvc_sext((RVC_BIT(h, 12) << 8) |
                        (RVC_BITS(h, 11, 10) << 3) | (RVC_BITS(h, 6, 5) << 6) |
                        (RVC_BITS(h, 4, 3) << 1) | (RVC_BIT(h, 2) << 5), 9);
                *word = rv32_rvc_b(bimm, 0, rs1p, f3 == 6 ? 0 : 1); /* c.beqz/bnez */
                return 1;
            }
        }
    }
    if (op == 2)
    {
        switch (f3)
        {
            case 0:
                if (RVC_BIT(h, 12) || !rd || !rs2)
                    return 0;
                *word = rv32_rvc_i((int)rs2, rd, 1, rd, 0x13);             /* c.slli */
                return 1;
            case 2:
            {
                unsigned imm = (RVC_BIT(h, 12) << 5) | (RVC_BITS(h, 6, 4) << 2) |
                        (RVC_BITS(h, 3, 2) << 6);
                if (!rd)
                    return 0;
                *word = rv32_rvc_i((int)imm, 2, 2, rd, 0x03);              /* c.lwsp */
                return 1;
            }
            case 4:
                if (!RVC_BIT(h, 12))
                {
                    if (!rd)
                        return 0;
                    *word = rs2 ? rv32_rvc_r(0, rs2, 0, 0, rd, 0x33) :    /* c.mv */
                            rv32_rvc_i(0, rd, 0, 0, 0x67);                        /* c.jr */
                    return 1;
                }
                if (!rd && !rs2)
                    return 0;                                             /* c.ebreak */
                if (!rs2)
                {
                    *word = rv32_rvc_i(0, rd, 0, 1, 0x67);                  /* c.jalr */
                    return 1;
                }
                if (!rd)
                    return 0;
                *word = rv32_rvc_r(0, rs2, rd, 0, rd, 0x33);               /* c.add */
                return 1;
            case 6:
            {
                unsigned imm = (RVC_BITS(h, 12, 9) << 2) | (RVC_BITS(h, 8, 7) << 6);
                *word = rv32_rvc_s((int)imm, rs2, 2, 2);                   /* c.swsp */
                return 1;
            }
            default:
                return 0;
        }
    }
    return 0;
}

#undef RVC_BIT
#undef RVC_BITS

/* The instruction at byte pc: its length (2 or 4) and its 32-bit form.
   0 when pc is odd, the instruction does not fit before size, or a 16-bit
   form is reserved or unsupported. */
static inline unsigned rv32_rvc_fetch(const void *code, size_t size,
        size_t pc, uint32_t *word)
{
    const uint8_t *p = (const uint8_t *)code;
    uint16_t low;
    if ((pc & 1) || pc + 2 > size)
        return 0;
    memcpy(&low, p + pc, 2);
    if ((low & 3) != 3)
        return rv32_rvc_expand(low, word) ? 2 : 0;
    if (pc + 4 > size)
        return 0;
    memcpy(word, p + pc, 4);
    return 4;
}

#endif
