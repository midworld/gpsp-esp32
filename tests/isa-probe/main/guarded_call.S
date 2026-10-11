/* Run a snippet and record synchronous instruction faults.
 * The caller must mask interrupts and keep the task on one hart.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .option push
    .option norvc
    .section .iram1, "ax", @progbits
    .balign 4
    .global probe_guarded_call
    .type probe_guarded_call, @function
    .global probe_guarded_call_access
    .type probe_guarded_call_access, @function
    .global probe_guarded_call_system
    .type probe_guarded_call_system, @function

/* a0=code, a1..a3=arguments, a4=result, a5={cause,pc,tval}. */
probe_guarded_call:
    j .Lguard_start
probe_guarded_call_access:
    j .Lguard_start
probe_guarded_call_system:
.Lguard_start:
    addi sp, sp, -64
    sw ra, 60(sp)
    sw s0, 44(sp)
    sw s1, 40(sp)
    sw s2, 36(sp)
    sw s3, 32(sp)
    sw s4, 28(sp)
    sw s5, 24(sp)
    mv s0, a4
    mv s1, a5
    mv s2, a0
    mv s3, a1
    mv s4, a2
    mv s5, a3
    li t0, -1                 /* mcause=0 is also a valid exception. */
    sw t0, 0(s1)
    sw zero, 4(s1)
    sw zero, 8(s1)
    sw s1, 4(sp)
    sw s0, 52(sp)
    /* mscratch locates this frame even if the snippet changes sp. */
    csrr t0, mscratch
    sw t0, 8(sp)
    csrw mscratch, sp
    /* Install a temporary direct trap vector for this one snippet call. */
    csrr t0, mtvec
    sw t0, 0(sp)
    la t0, .Lillegal_handler
    csrw mtvec, t0
    mv a0, s3
    mv a1, s4
    mv a2, s5
    jalr ra, 0(s2)
.Lreturned:
    /* Restore the caller's vector before inspecting the recorded trap. */
    lw t0, 0(sp)
    csrw mtvec, t0
    lw t0, 8(sp)
    csrw mscratch, t0
    lw t0, 4(sp)
    lw t0, 0(t0)
    li t1, -1
    bne t0, t1, .Lillegal
    lw t0, 52(sp)
    sw a0, 0(t0)
    li a0, 1
    j .Ldone
.Lillegal:
    li a0, 0
.Ldone:
    lw s5, 24(sp)
    lw s4, 28(sp)
    lw s3, 32(sp)
    lw s2, 36(sp)
    lw s1, 40(sp)
    lw s0, 44(sp)
    lw ra, 60(sp)
    addi sp, sp, 64
    ret
    .size probe_guarded_call, .-probe_guarded_call
    .size probe_guarded_call_access, .-probe_guarded_call_access
    .size probe_guarded_call_system, .-probe_guarded_call_system

    .balign 64
.Lillegal_handler:
    /* A snippet may fault for reasons other than an unsupported opcode. */
    csrrw t0, mscratch, t0    /* t0=guard frame; mscratch=trapped t0 */
    sw sp, 48(t0)
    sw t1, 16(t0)
    sw t2, 20(t0)
    csrr t1, mscratch
    sw t1, 12(t0)
    csrr t1, mcause
    bltz t1, .Linterrupt
    /* CLIC mcause also mirrors MPP/MPIE; keep the exception code. */
    slli t1, t1, 20
    srli t1, t1, 20
    lw t2, 4(t0)              /* fault pointer for this invocation */
    sw t1, 0(t2)
    csrr t1, mepc
    sw t1, 4(t2)
    csrr t1, mtval
    sw t1, 8(t2)
    mv sp, t0
    la t1, .Lreturned
    csrw mepc, t1
    mret
.Linterrupt:
    /* Interrupts are masked by the caller. Let IDF diagnose an unexpected one. */
    lw t1, 0(t0)
    csrw mtvec, t1
    lw t1, 8(t0)
    csrw mscratch, t1
    lw t2, 20(t0)
    lw sp, 48(t0)
    lw t1, 16(t0)
    lw t0, 12(t0)
    j _panic_handler
    .option pop
