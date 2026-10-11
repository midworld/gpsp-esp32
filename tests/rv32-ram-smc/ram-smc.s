/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

.syntax unified
.cpu arm7tdmi
.section .text
.arm
.global _start
_start:
    b main
    .space 0xbc
main:
    ldr sp, =0x03007f00
    /* Copy relocatable Thumb code into IWRAM. */
    ldr r4, =0x03000000
    ldr r5, =thumb_image_start
    bic r5, r5, #1
    ldr r6, =thumb_image_end
copy:
    cmp r5, r6
    ldrblo r0, [r5], #1
    strblo r0, [r4], #1
    blo copy

    /* Keep the tail and exit-limit fixtures separate. */
    ldr r4, =0x03006000
    ldr r5, =arm_tail_func
    ldr r6, =arm_tail_end
copy_tail:
    cmp r5, r6
    ldrlo r0, [r5], #4
    strlo r0, [r4], #4
    blo copy_tail
    ldr r4, =0x03006200
    ldr r5, =arm_slots_func
    ldr r6, =arm_slots_end
copy_slots:
    cmp r5, r6
    ldrlo r0, [r5], #4
    strlo r0, [r4], #4
    blo copy_slots
    ldr r4, =0x03006400
    ldr r5, =arm_cut_func
    ldr r6, =arm_cut_end
copy_cut:
    cmp r5, r6
    ldrlo r0, [r5], #4
    strlo r0, [r4], #4
    blo copy_cut
    ldr r4, =0x03006500
    ldr r5, =thumb_cut_func
    bic r5, r5, #1
    ldr r6, =thumb_cut_end
copy_thumb_cut:
    cmp r5, r6
    ldrlo r0, [r5], #4
    strlo r0, [r4], #4
    blo copy_thumb_cut
    ldr r4, =0x03006600
    ldr r5, =arm_dyn_func
    ldr r6, =arm_dyn_end
copy_dyn:
    cmp r5, r6
    ldrlo r0, [r5], #4
    strlo r0, [r4], #4
    blo copy_dyn

    /* Each first call marks a phase in the generic translation log. */
    ldr r4, =0x03006100
    ldr r0, =0xe12fff1e  /* BX lr */
    .rept 15
    str r0, [r4], #4
    .endr

    /* Prime the source-to-target link before patching the target. */
    ldr r4, =0x03000001
    ldr lr, =after_first
    bx r4
after_first:
    ldr r4, =0x03000021
    ldr lr, =after_second
    bx r4
after_second:
    cmp r0, #2
    ldr r1, =0x04000000
    ldr r2, =0x00000403
    strh r2, [r1]
    ldr r1, =0x06000000
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Flush, then translate only the BX at IWRAM offset zero. */
    ldr r1, =0x03000010
    ldr r2, =0x00002002
    strh r2, [r1]
    ldr r1, =0x03000000
    ldr r2, =0x00004770
    strh r2, [r1]
    mov r0, #0
    ldr r4, =0x03000001
    ldr lr, =after_zero_first
    bx r4
after_zero_first:
    ldr r1, =0x03000000
    ldr r2, =0x0000e006
    strh r2, [r1]
    mov r0, #0
    ldr r4, =0x03000001
    ldr lr, =after_zero_second
    bx r4
after_zero_second:
    cmp r0, #2
    ldr r1, =0x06000002
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Enter a translated Thumb block at an independent interior instruction. */
    ldr r4, =0x03000001 + (thumb_alias_head - thumb_image_base)
    ldr lr, =after_alias_head
    bx r4
after_alias_head:
    cmp r0, #7
    moveq r7, #1
    movne r7, #0
    mov r0, #10
    ldr r4, =0x03000001 + (thumb_alias_entry - thumb_image_base)
    ldr lr, =after_alias_entry
    bx r4
after_alias_entry:
    cmp r0, #12
    movne r7, #0
    cmp r7, #1
    ldr r1, =0x06000004
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* The same physical code at a mirrored PC has a different PC value. */
    ldr r4, =0x03000001 + (thumb_mirror - thumb_image_base)
    ldr lr, =after_mirror_base
    bx r4
after_mirror_base:
    ldr r5, =0x03000004 + (thumb_mirror - thumb_image_base)
    cmp r0, r5
    moveq r7, #1
    movne r7, #0
    ldr r4, =0x03008001 + (thumb_mirror_entry - thumb_image_base)
    ldr lr, =after_mirror_alias
    bx r4
after_mirror_alias:
    add r5, r5, #0x8000
    cmp r0, r5
    movne r7, #0
    cmp r7, #1
    ldr r1, =0x06000006
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Tag the low halfword as Thumb, then translate the overlapping ARM code. */
    ldr r4, =0x03000001 + (arm_alias_entry - thumb_image_base)
    ldr lr, =after_arm_thumb_tag
    bx r4
after_arm_thumb_tag:
    mov r7, #0
    ldr r4, =0x03000000 + (arm_alias_head - thumb_image_base)
    ldr lr, =after_arm_head
    bx r4
after_arm_head:
    cmp r4, #10
    moveq r6, #1
    movne r6, #0
    mov r0, #7
    ldr r4, =0x03000000 + (arm_alias_entry - thumb_image_base)
    ldr lr, =after_arm_entry
    bx r4
after_arm_entry:
    cmp r4, #14
    movne r6, #0
    ldr r1, =0x03000002 + (arm_alias_entry - thumb_image_base)
    ldr r2, =0xe040
    strh r2, [r1]
    mov r0, #7
    ldr r4, =0x03000000 + (arm_alias_entry - thumb_image_base)
    ldr lr, =after_arm_patch
    bx r4
after_arm_patch:
    cmp r4, #0
    movne r6, #0
    cmp r6, #1
    ldr r1, =0x0600000a
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* A write to an aliased instruction must discard its old translation. */
    ldr r1, =0x03000000 + (thumb_alias_entry - thumb_image_base)
    ldr r2, =0x3003
    strh r2, [r1]
    mov r0, #10
    ldr r4, =0x03000001 + (thumb_alias_entry - thumb_image_base)
    ldr lr, =after_alias_patch
    bx r4
after_alias_patch:
    cmp r0, #13
    ldr r1, =0x06000008
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* ARM immediates changed in place must run with their new values. */
    ldr r4, =0x03000000 + (arm_imm_func - thumb_image_base)
    ldr lr, =after_imm_first
    bx r4
after_imm_first:
    cmp r1, #1
    moveq r7, #1
    movne r7, #0
    ldr r5, =0x03000000 + (arm_imm_func - thumb_image_base)
    mov r2, #5
    strb r2, [r5]       /* MOV r0, #5 */
    mov r2, #9
    strb r2, [r5, #8]   /* CMP r0, #9 */
    ldr lr, =after_imm_second
    bx r4
after_imm_second:
    cmp r0, #9
    movne r7, #0
    cmp r1, #1
    movne r7, #0
    mov r2, #10
    strb r2, [r5, #8]   /* CMP r0, #10 */
    ldr lr, =after_imm_third
    bx r4
after_imm_third:
    cmp r0, #9
    movne r7, #0
    cmp r1, #0
    movne r7, #0
    /* A block that changes one of its own later immediates. */
    ldr r4, =0x03000000 + (arm_imm_self - thumb_image_base)
    add r1, r4, #12
    mov r2, #6
    ldr lr, =after_imm_self_first
    bx r4
after_imm_self_first:
    cmp r0, #6
    movne r7, #0
    ldr r4, =0x03000000 + (arm_imm_self - thumb_image_base)
    add r1, r4, #12
    mov r2, #8
    ldr lr, =after_imm_self_second
    bx r4
after_imm_self_second:
    cmp r0, #8
    movne r7, #0
    /* A B moved to another target outside its block. */
    ldr r4, =0x03000000 + (arm_b_func - thumb_image_base)
    ldr lr, =after_b_first
    bx r4
after_b_first:
    cmp r0, #1
    movne r7, #0
    mov r2, #2
    strb r2, [r4, #4]   /* B arm_b_two */
    ldr lr, =after_b_second
    bx r4
after_b_second:
    cmp r0, #2
    movne r7, #0
    mov r2, #0
    strb r2, [r4, #4]   /* B arm_b_one */
    ldr lr, =after_b_third
    bx r4
after_b_third:
    cmp r0, #1
    movne r7, #0
    cmp r7, #1
    ldr r1, =0x06000010
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* A dormant unit runs again once its bytes come back. */
    mov r6, #1
    ldr r5, =0x03000000 + (arm_rev - thumb_image_base)
    adr lr, rev_first
    bx r5
rev_first:
    cmp r1, #1
    movne r6, #0
    ldr r2, =0xe3e01001  /* MVN r1, #1 */
    str r2, [r5]
    adr lr, rev_second
    bx r5
rev_second:
    cmn r1, #2
    movne r6, #0
    ldr r2, =0xe3a01001  /* MOV r1, #1 */
    str r2, [r5]
    adr lr, rev_third
    bx r5
rev_third:
    cmp r1, #1
    movne r6, #0

    /* 65 RAM units cover one word: entries are called back to front, so
       each one starts a new unit. A write there must drop them all. */
    ldr r8, =0x03000000 + (arm_many - thumb_image_base)
    mov r9, #64
many_build:
    add r4, r8, r9, lsl #2
    adr lr, many_build_ret
    bx r4
many_build_ret:
    subs r9, r9, #1
    bpl many_build
    ldr r2, =0xe3e01001  /* MVN r1, #1 */
    str r2, [r8, #280]
    mov r7, #1
    mov r9, #64
many_check:
    add r4, r8, r9, lsl #2
    adr lr, many_check_ret
    bx r4
many_check_ret:
    cmn r1, #2
    movne r7, #0
    subs r9, r9, #1
    bpl many_check
    and r7, r7, r6
    cmp r7, #1
    ldr r1, =0x06000012
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* One STM changes code at the end of EWRAM and, through the mirror,
       code at its start. Both translations must see their new words. */
    ldr r5, =0x02000000
    ldr r2, =0xe3a01001  /* MOV r1, #1 */
    str r2, [r5]
    ldr r2, =0xe12fff1e  /* BX lr */
    str r2, [r5, #4]
    str r2, [r5, #12]
    ldr r2, =0xe2811004  /* ADD r1, r1, #4 */
    str r2, [r5, #8]
    ldr r6, =0x0203fff8
    ldr r2, =0xe3a01003  /* MOV r1, #3 */
    str r2, [r6]
    ldr r2, =0xe12fff1e
    str r2, [r6, #4]
    adr lr, mirror_low_first
    bx r5
mirror_low_first:
    cmp r1, #1
    moveq r7, #1
    movne r7, #0
    adr lr, mirror_high_first
    bx r6
mirror_high_first:
    cmp r1, #3
    movne r7, #0
    ldr r2, =0xea000001  /* B to the helper at 0x02040008 */
    ldr r3, =0xe3a01002  /* MOV r1, #2 */
    add r0, r6, #4
    stmia r0, {r2, r3}   /* 0x0203fffc and 0x02040000 */
    adr lr, mirror_low_second
    bx r5
mirror_low_second:
    cmp r1, #2
    movne r7, #0
    adr lr, mirror_high_second
    bx r6
mirror_high_second:
    cmp r1, #7
    movne r7, #0
    cmp r7, #1
    ldr r1, =0x06000014
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* A switch slot starts the second flow chunk. The CMP ending the first
       chunk must stay, since the slot may become a B over the next CMP. */
    ldr r4, =0x03000000 + (arm_flag_func - thumb_image_base)
    add r5, r4, #128
    ldr r8, =0xea000000  /* B over the second CMP */
    ldr r9, =0xe1a00000  /* MOV r0, r0 */
    mov r7, #1
    mov r0, #1
    adr lr, flag_nop_first
    bx r4
flag_nop_first:
    cmp r1, #1
    movne r7, #0
    str r8, [r5]         /* learn the slot */
    /* From here the head is translated only while the slot is B. */
    ldr r12, =0x0300612c
    adr lr, flag_b_call
    bx r12
flag_b_call:
    mov r0, #0
    movs r2, #1          /* Z clear on entry */
    adr lr, flag_b_first
    bx r4
flag_b_first:
    cmp r1, #1
    movne r7, #0
    str r9, [r5]
    mov r0, #1
    adr lr, flag_nop_second
    bx r4
flag_nop_second:
    cmp r1, #1
    movne r7, #0
    str r8, [r5]         /* patched in place */
    mov r0, #0
    movs r2, #1
    adr lr, flag_b_second
    bx r4
flag_b_second:
    cmp r1, #1
    movne r7, #0
    str r9, [r5]
    ldr r12, =0x03006130
    adr lr, flag_done
    bx r12
flag_done:
    cmp r7, #1
    ldr r1, =0x06000016
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Reject the reachable NOP unit translated before this slot was known. */
    ldr r4, =0x03006014
    ldr r8, =0xeaffffff  /* B to the next op */
    ldr r9, =0xe1a00000
    adr lr, switch_unknown_nop
    bx r4
switch_unknown_nop:
    str r8, [r4]
    adr lr, switch_learn_b
    bx r4
switch_learn_b:
    str r9, [r4]
    adr lr, switch_known_nop
    bx r4
switch_known_nop:
    /* The slot is in the scanned tail, beyond the unconditional BX. */
    ldr r12, =0x03006100
    adr lr, switch_tail_begin
    bx r12
switch_tail_begin:
    ldr r5, =0x03006000
    mov r0, #1
    mov r7, #1
    adr lr, switch_tail_first
    bx r5
switch_tail_first:
    cmp r1, #1
    movne r7, #0
    mov r6, #32
switch_tail_loop:
    str r8, [r4]
    mov r0, #1
    adr lr, switch_tail_b
    bx r5
switch_tail_b:
    cmp r1, #1
    movne r7, #0
    str r9, [r4]
    mov r0, #1
    adr lr, switch_tail_nop
    bx r5
switch_tail_nop:
    cmp r1, #1
    movne r7, #0
    cmp r6, #32
    bne switch_tail_continue
    ldr r12, =0x03006104
    adr lr, switch_tail_continue
    bx r12
switch_tail_continue:
    subs r6, r6, #1
    bne switch_tail_loop
    ldr r12, =0x03006108
    adr lr, switch_tail_done
    bx r12
switch_tail_done:
    cmp r7, #1
    ldr r1, =0x0600001a
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Learn many switch slots in one block. Their links count as exits,
       so the block must split instead of failing translation. */
    ldr r12, =0x0300610c
    adr lr, slots_begin
    bx r12
slots_begin:
    ldr r4, =0x03006200
    ldr r8, =0xeaffffff  /* B to the next op */
    ldr r9, =0xe1a00000
    mov r6, #0
slots_learn:
    adr lr, slots_nop
    bx r4
slots_nop:
    /* No call while it is B: a B-state unit would be patched, not
       translated again with the new slot. */
    str r8, [r4, r6, lsl #2]
    str r9, [r4, r6, lsl #2]
    add r6, r6, #1
    cmp r6, #32
    blo slots_learn
    mov r1, #0
    adr lr, slots_done
    bx r4
slots_done:
    cmp r1, #1
    ldr r1, =0x06000018
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]
    ldr r12, =0x03006110
    adr lr, slots_end
    bx r12
slots_end:

    /* Slots translated as B. The slot table is full, so both functions
       reuse addresses learned above. First, B->NOP must keep the flags
       that the NOP path reads. */
    ldr r4, =0x03006200
    ldr r5, =arm_bflag_func
    ldr r6, =arm_bflag_end
bflag_copy:
    cmp r5, r6
    ldrlo r0, [r5], #4
    strlo r0, [r4], #4
    blo bflag_copy
    ldr r4, =0x03006200
    ldr r8, =0xea000000  /* B over the next op */
    ldr r9, =0xe1a00000
    mov r7, #1
    str r8, [r4, #12]    /* slot A is B at the first translation */
    mov r0, #1
    adr lr, bflag_a
    bx r4
bflag_a:
    cmp r1, #0
    movne r7, #0
    str r9, [r4, #12]
    str r8, [r4, #16]
    mov r0, #1
    adr lr, bflag_b
    bx r4
bflag_b:
    cmp r1, #1
    movne r7, #0

    /* A backward B to an op in its own unit must patch to NOP. */
    ldr r4, =0x03006240
    ldr r5, =arm_bback_func
    ldr r6, =arm_bback_end
bback_copy:
    cmp r5, r6
    ldrlo r0, [r5], #4
    strlo r0, [r4], #4
    blo bback_copy
    ldr r4, =0x03006240
    ldr r8, =0xeafffffb  /* B back to the loop */
    str r8, [r4, #16]
    ldr r12, =0x03006134
    adr lr, bback_start
    bx r12
bback_start:
    mov r6, #2
bback_loop:
    mov r0, #2
    adr lr, bback_b
    bx r4
bback_b:
    cmp r1, #2
    movne r7, #0
    str r9, [r4, #16]
    mov r0, #2
    adr lr, bback_nop
    bx r4
bback_nop:
    cmp r1, #1
    movne r7, #0
    str r8, [r4, #16]
    subs r6, r6, #1
    bne bback_loop
    ldr r12, =0x03006138
    adr lr, bback_done
    bx r12
bback_done:
    cmp r7, #1
    ldr r1, =0x06000022
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Keep the writer live while its body changes. */
    ldr r4, =0x03006400
    /* Create both uncut units before learning to test the revival guard. */
    ldr r0, =cut_add_one
    adr lr, cut_prep_a
    bx r4
cut_prep_a:
    ldr r0, =cut_double
    ldmia r0, {r2, r3, r5, r6}
    add r1, r4, #(arm_cut_body - arm_cut_func)
    stmia r1, {r2, r3, r5, r6}
    adr lr, cut_prep_b
    bx r4
cut_prep_b:
    mov r6, #0
    mov r7, #1
cut_loop:
    cmp r6, #4
    bne cut_call
    ldr r12, =0x03006114
    adr lr, cut_call
    bx r12
cut_call:
    tst r6, #1
    ldreq r0, =cut_add_one
    ldrne r0, =cut_double
    moveq r5, #5
    movne r5, #16
    adr lr, cut_ret
    bx r4
cut_ret:
    cmp r1, r5
    movne r7, #0
    add r6, r6, #1
    cmp r6, #16
    blo cut_loop
    ldr r12, =0x03006118
    adr lr, cut_done
    bx r12
cut_done:
    cmp r7, #1
    ldr r1, =0x0600001c
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Check self-write cuts for Thumb code. */
    ldr r4, =0x03006501
    mov r6, #0
    mov r7, #1
thumb_cut_loop:
    cmp r6, #4
    bne thumb_cut_call
    ldr r12, =0x0300611c
    adr lr, thumb_cut_call
    bx r12
thumb_cut_call:
    tst r6, #1
    ldreq r0, =thumb_cut_add_one
    ldrne r0, =thumb_cut_double
    moveq r5, #5
    movne r5, #16
    adr lr, thumb_cut_ret
    bx r4
thumb_cut_ret:
    cmp r1, r5
    movne r7, #0
    add r6, r6, #1
    cmp r6, #16
    blo thumb_cut_loop
    ldr r12, =0x03006120
    adr lr, thumb_cut_done
    bx r12
thumb_cut_done:
    cmp r7, #1
    ldr r1, =0x0600001e
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Rewrite two immediates 16 times. Learned ones need no new code.
       Odd calls enter through the IWRAM mirror. */
    ldr r4, =0x03006600
    mov r6, #0
    mov r7, #1
dyn_loop:
    cmp r6, #8
    bne dyn_write
    ldr r12, =0x03006124
    adr lr, dyn_write
    bx r12
dyn_write:
    add r2, r6, #1
    strb r2, [r4]
    mov r3, r6, lsl #4
    orr r3, r3, #3
    strb r3, [r4, #4]
    mov r0, #0xff
    tst r6, #1
    ldreq r12, =0x03006600
    ldrne r12, =0x0300e600
    adr lr, dyn_ret
    bx r12
dyn_ret:
    add r5, r2, r3
    cmp r1, r5
    movne r7, #0
    add r6, r6, #1
    cmp r6, #16
    blo dyn_loop
    ldr r12, =0x03006128
    adr lr, dyn_rom
    bx r12
dyn_rom:
    /* ROM code with the same low address bits stays static. */
    ldr r12, =rom_alias_func
    adr lr, dyn_rom_ret
    bx r12
dyn_rom_ret:
    cmp r1, #7
    movne r7, #0
dyn_opcode:
    /* A new opcode needs new code: AND becomes EOR. */
    ldr r2, =0xe2200005
    str r2, [r4, #4]
    mov r0, #0xff
    adr lr, dyn_eor
    bx r4
dyn_eor:
    ldr r5, =16 + (0xff ^ 5)
    cmp r1, r5
    movne r7, #0
    cmp r7, #1
    ldr r1, =0x06000020
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    /* Translate a ROM region with RAM sites, then enter after its load. */
    ldr r0, =0x02000100
    mov r2, #10
    str r2, [r0]
    ldr r3, =rom_memory_head + 1
    ldr lr, =after_rom_head
    bx r3
after_rom_head:
    cmp r2, #19
    ldr r1, =0x0600000c
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]

    ldr r4, =0x03000001
loop:
    /* Keep running patched IWRAM code through the benchmark warm window. */
    ldr lr, =warm_after_ram
    bx r4
warm_after_ram:
    ldr r0, =0x02000100
    mov r2, #40
    ldr r3, =rom_memory_entry + 1
    ldr lr, =after_rom_entry
    bx r3
after_rom_entry:
    cmp r2, #47
    ldr r1, =0x0600000e
    moveq r2, #0x03e0
    movne r2, #0x001f
    strh r2, [r1]
    bne failed
    b loop
failed:
    b failed
.pool

.align 2
.thumb
thumb_image_base:
.thumb_func
thumb_image_start:
    b thumb_target
    .space 0x0e
thumb_target:
    movs r0, #1
    bx lr
    .space 0x0c
thumb_patcher:
    ldr r1, =0x03000010
    ldr r2, =0x00002002
    strh r2, [r1]
    b thumb_image_start
    .pool

.align 2
thumb_alias_head:
    movs r0, #5
thumb_alias_entry:
    adds r0, #2
    bx lr

.align 2
thumb_mirror:
    movs r0, #0
thumb_mirror_entry:
    .hword 0xa000  /* ADR r0, PC + 0 */
    bx lr

.align 2
.arm
arm_alias_head:
    mov r0, #5
arm_alias_entry:
    .word 0xe0804770  /* ADD r4, r0, r0, ROR r7; Thumb BX LR at +0 */
    bx lr

arm_imm_func:
    mov r0, #3
    add r0, r0, #4
    cmp r0, #7
    moveq r1, #1
    movne r1, #0
    bx lr

arm_b_func:
    mov r0, #0
    b arm_b_one
    bx lr
arm_b_one:
    mov r0, #1
    bx lr
arm_b_two:
    mov r0, #2
    bx lr

arm_rev:
    mov r1, #1
    bx lr

/* 70 ADDs, then the word at +280 that the test rewrites. */
arm_many:
    .rept 70
    add r0, r0, #1
    .endr
    mov r1, #1
    bx lr

/* r1 = the MOV below, r2 = its new immediate. */
arm_imm_self:
    strb r2, [r1]
    mov r0, r0
    mov r0, r0
    mov r0, #1
    bx lr
/* Ops 0-31 fill the first flow chunk; op 32 is the switch slot. */
arm_flag_func:
    .rept 31
    add r3, r3, #1
    .endr
    cmp r0, #0
    mov r0, r0
    cmp r0, #1
    beq arm_flag_one
    mov r1, #0
    bx lr
arm_flag_one:
    mov r1, #1
    bx lr

thumb_image_end:

/* Two forward exits keep the scanner going past BX into the slot tail. */
arm_tail_func:
    cmp r0, #0
    beq arm_tail_slot
    beq arm_tail_slot
    mov r1, #1
    bx lr
arm_tail_slot:
    mov r0, r0
    mov r1, #1
    bx lr
arm_tail_end:

/* 32 slot candidates, then three exits; copied to IWRAM at 0x03006200. */
arm_slots_func:
    .rept 32
    mov r0, r0
    .endr
    cmp r0, #0
    beq 1f
1:
    beq 2f
2:
    beq 3f
3:
    mov r1, #1
    bx lr
arm_slots_end:

/* r1 = 1 only when slot B branches after CMP r0, #1 set Z; copied to
   0x03006200. Slots A and B sit at learned 0x0300620c and 0x03006210. */
arm_bflag_func:
    cmp r0, #0
    beq 1f
    cmp r0, #1
    mov r0, r0
1:
    mov r0, r0
    cmp r0, #2
    moveq r1, #1
    movne r1, #0
    bx lr
arm_bflag_end:

/* r1 = 1 with the NOP slot, r0 with the backward B; copied to
   0x03006240. The slot sits at learned 0x03006250. */
arm_bback_func:
    mov r1, #0
1:
    add r1, r1, #1
    cmp r1, r0
    bxeq lr
    mov r0, r0
    bx lr
arm_bback_end:

/* Different instruction forms force invalidation instead of immediate patches. */
arm_cut_func:
    adr r12, arm_cut_body
    mov r3, #4
arm_cut_copy:
    ldr r2, [r0], #4
    str r2, [r12], #4
    subs r3, r3, #1
    bne arm_cut_copy
    mov r1, #1
arm_cut_body:
    .rept 4
    add r1, r1, #1
    .endr
    bx lr
arm_cut_end:

/* r1 = imm + (r0 & imm); copied to 0x03006600. */
arm_dyn_func:
    mov r1, #0
    and r0, r0, #0
    add r1, r1, r0
    bx lr
arm_dyn_end:

cut_add_one:
    .rept 4
    add r1, r1, #1
    .endr
cut_double:
    .rept 4
    add r1, r1, r1
    .endr

/* Thumb copy of the self-writing function; copied to 0x03006500. */
.thumb
.align 2
thumb_cut_func:
    ldr r3, =0x03006500 + (thumb_cut_body - thumb_cut_func)
    movs r1, r3
    adds r1, #8
thumb_cut_copy:
    ldrh r2, [r0]
    adds r0, #2
    strh r2, [r3]
    adds r3, #2
    cmp r3, r1
    bne thumb_cut_copy
    movs r1, #1
thumb_cut_body:
    .rept 4
    adds r1, #1
    .endr
    bx lr
.align 2
    .ltorg
thumb_cut_end:

thumb_cut_add_one:
    .rept 4
    adds r1, #1
    .endr
thumb_cut_double:
    .rept 4
    lsls r1, r1, #1
    .endr
.arm
.align 2

.align 2
.thumb
rom_memory_head:
    movs r1, #1
    ldr r2, [r0]
    adds r2, #2
rom_memory_entry:
    adds r2, #3
    str r2, [r0, #8]
    adds r2, #4
    bx lr

/* Same low 15 address bits as the learned op at 0x03006600. */
.arm
.org 0x6600
rom_alias_func:
    mov r1, #7
    bx lr
