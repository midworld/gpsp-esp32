/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Address generation for rotated affine backgrounds with the XespV
   extension, four pixels per step. Coordinates wrap with the mask, so
   walks that stay inside the map work too.

   a0: parameter block (16-byte aligned, see affine_pie_params)
       0:  x[4]   source x of pixels 0..3 (8.8 fixed point)
       16: y[4]   source y of pixels 0..3
       32: wrap mask, then the per-step constants in the order the loop
       reads them: 7, tile base, map base, 4*dx, 4*dy
   a1: pixel count, a multiple of 4
   a2: map pitch (log2 of tiles per row)
   a3: output, 16-byte aligned: map byte addresses, then 256 words later
       the tile base plus the offset inside the tile. */

  .text
  .option push
  .option arch, +xespv
  .option norvc
  .align 2
  .globl gpsp_affine_pie_addr
  .type gpsp_affine_pie_addr, @function
gpsp_affine_pie_addr:
  /* PIE takes only x8-x15 as address and scalar operands: s0/s1 hold the
     shift counts, a4 walks the constants and a5 is the tile output. */
  beqz a1, 2f
  addi sp, sp, -16
  sw s0, 0(sp)
  sw s1, 4(sp)
  li s0, 8
  li s1, 3
  mv a4, a0
  esp.vld.128.ip q0, a4, 16         /* x */
  esp.vld.128.ip q1, a4, 16         /* y */
  esp.vldbc.32.ip q2, a4, 4         /* wrap mask; a4 -> constants */
  mv a6, a4
  addi a5, a3, 1024
1:
  mv a4, a6
  esp.movx.w.sar s0
  esp.vsr.u32 q3, q0                /* px = (x >> 8) & mask */
  esp.andq q3, q3, q2
  esp.vsr.u32 q4, q1                /* py = (y >> 8) & mask */
  esp.andq q4, q4, q2

  esp.vldbc.32.ip q7, a4, 4         /* 7 */
  esp.andq q5, q4, q7
  esp.andq q6, q3, q7
  esp.movx.w.sar s1
  esp.vsl.32 q5, q5                 /* (py & 7) << 3 | (px & 7) */
  esp.orq q5, q5, q6
  esp.vldbc.32.ip q7, a4, 4         /* tile base */
  esp.vadd.u32 q5, q5, q7
  esp.vst.128.ip q5, a5, 16

  esp.vsr.u32 q3, q3                /* (px >> 3) + ((py >> 3) << pitch) */
  esp.vsr.u32 q4, q4
  esp.movx.w.sar a2
  esp.vsl.32 q4, q4
  esp.vadd.u32 q3, q3, q4
  esp.vldbc.32.ip q7, a4, 4         /* map base */
  esp.vadd.u32 q3, q3, q7
  esp.vst.128.ip q3, a3, 16

  /* PIE adds saturate: steps may be negative, so use the signed add.
     Coordinates are 28-bit, far from the signed limits. */
  esp.vldbc.32.ip q7, a4, 4         /* next four pixels */
  esp.vadd.s32 q0, q0, q7
  esp.vldbc.32.ip q7, a4, 4
  esp.vadd.s32 q1, q1, q7
  addi a1, a1, -4
  bnez a1, 1b

  lw s0, 0(sp)
  lw s1, 4(sp)
  addi sp, sp, 16
2:
  ret
  .size gpsp_affine_pie_addr, .-gpsp_affine_pie_addr

  .option pop
