/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "common.h"
#include "riscv/rv32_cycle_budget.h"
#include <assert.h>

u32 reg[REG_MAX];
u16 io_registers[512];
gpsp_rv32_arena gpsp_rv32_rom_arena;
u32 irq_entry_cycles;
dma_transfer_type dma[DMA_CHAN_CNT];
u32 cheat_master_hook = ~0u;
extern s32 video_count;
extern timer_type timer[4];
static unsigned scanlines, updates, irq, serial_budget = 0x7fff;
static unsigned sound_dma_cycles;
static unsigned drawing_cycles;
static u32 serial_irq, gbp_state;

/* Keep the real scheduler and state codec; stub peripheral work. */
void update_scanline(void) { scanlines++; }
void video_reload_counters(void) {}
void render_gbc_sound(void) {}
void process_cheats(void) {}
bool gamepak_must_swap(void) { return false; }
void init_dynarec_caches(void) { assert(0); }
void init_emitter(bool swap) { (void)swap; assert(0); }
unsigned sound_timer(fixed8_24 step, u32 channel)
{ (void)step; (void)channel; return sound_dma_cycles; }
cpu_alert_type dma_transfer(unsigned channel, int *cycles)
{ (void)channel; (void)cycles; return CPU_ALERT_NONE; }
cpu_alert_type flag_interrupt(irq_type raised)
{ irq |= raised; return CPU_ALERT_NONE; }
u32 check_and_raise_interrupts(void)
{ u32 raised = irq; irq = 0; return raised != 0; }
bool update_serial(unsigned cycles) { (void)cycles; return false; }
u32 serial_next_event(void) { return serial_budget; }
u32 serial_get_irq_cycles(void) { return serial_irq; }
void serial_set_irq_cycles(u32 value) { serial_irq = value; }
u32 gbp_get_state(void) { return gbp_state; }
void gbp_set_state(u32 value) { gbp_state = value; }

static unsigned checked_update(int remaining)
{
    assert(remaining >= -64);
    updates++;
    return update_gba(remaining);
}

static unsigned update(int remaining)
{
    return gpsp_rv32_update_cycles(remaining, checked_update,
            &execute_cycles, &gpsp_rv32_frame_cycle_debt);
}

static void frame_edge(unsigned until_video)
{
    init_main();
    memset(reg, 0, sizeof(reg));
    reg[CPU_HALT_STATE] = CPU_ACTIVE;
    write_ioreg(REG_VCOUNT, 227);
    write_ioreg(REG_DISPSTAT, 2);
    execute_cycles = 1;
    video_count = until_video;
    scanlines = updates = irq = 0;
    serial_budget = 0x7fff;
}

static void test_frames(void)
{
    frame_edge(1);
    unsigned next = update(-100);
    assert(completed_frame(next) && updates == 1 && !scanlines);
    assert(execute_cycles == drawing_cycles - 64 &&
            gpsp_rv32_frame_cycle_debt == 36);
    int remaining = gpsp_rv32_take_cycles(execute_cycles,
            &gpsp_rv32_frame_cycle_debt);
    assert(remaining == (int)drawing_cycles - 100 &&
            gpsp_rv32_frame_cycle_debt == 0);

    /* Model retro_run reentry, then run to the next real frame boundary. */
    unsigned guest_cycles = 101;
    do
    {
        guest_cycles += remaining;
        next = update(0);
        remaining = cycles_to_run(next);
    } while (!completed_frame(next));
    assert(frame_counter == 2 && scanlines == 160);
    assert(cpu_ticks == guest_cycles && !gpsp_rv32_frame_cycle_debt);

    /* The second scheduler call, rather than the first, ends this frame. */
    frame_edge(90);
    irq = IRQ_TIMER0;
    next = update(-160);
    assert(completed_frame(next) && (next & 0x40000000u));
    assert(updates == 2 && gpsp_rv32_frame_cycle_debt == 7 && !scanlines);
    assert(cpu_ticks + gpsp_rv32_frame_cycle_debt == 161);

    /* Stop at a frame even when the next budget is shorter than the debt. */
    frame_edge(1);
    serial_budget = 4;
    reg[CPU_HALT_STATE] = CPU_HALT;
    next = update(-100);
    assert(completed_frame(next) && updates == 1 && !scanlines);
    assert(gpsp_rv32_frame_cycle_debt == 36);
    remaining = gpsp_rv32_take_cycles(execute_cycles,
            &gpsp_rv32_frame_cycle_debt);
    assert(remaining == -32);
    reg[CPU_HALT_STATE] = CPU_ACTIVE;
    next = update(remaining);
    assert(!completed_frame(next) && cpu_ticks == 101);
}

static void test_within_frame(void)
{
    static const int remaining[] = {20, 0, -1, -64, -65, -287};
    for (unsigned i = 0; i < sizeof(remaining) / sizeof(remaining[0]); i++)
    {
        init_main();
        reg[CPU_HALT_STATE] = CPU_ACTIVE;
        write_ioreg(REG_VCOUNT, 0);
        write_ioreg(REG_DISPSTAT, 0);
        serial_budget = 0x7fff;
        unsigned next = update(remaining[i]);
        assert(!completed_frame(next) && !gpsp_rv32_frame_cycle_debt);
        assert(cpu_ticks + execute_cycles - cycles_to_run(next) ==
                drawing_cycles - remaining[i]);
    }
}

static void test_dma_budget(void)
{
    const int remaining[] = {0, -100};
    for (unsigned i = 0; i < 2; i++)
    {
        frame_edge(1);
        sound_dma_cycles = 7;
        timer[0].status = TIMER_PRESCALE;
        timer[0].count = 1;
        timer[0].reload = 10000;
        timer[0].direct_sound_channels = TIMER_DS_CHANNEL_A;
        unsigned next = update(remaining[i]);
        unsigned debt = i ? 36 : 0;
        assert(completed_frame(next));
        assert(execute_cycles == cycles_to_run(next));
        assert(gpsp_rv32_frame_cycle_debt == debt);
        unsigned budget = frame_start_cycles();
        assert(execute_cycles - budget == 7);
        assert(gpsp_rv32_take_cycles(budget,
                &gpsp_rv32_frame_cycle_debt) == (int)(budget - debt));
        sound_dma_cycles = 0;
    }
}

static void test_state(void)
{
    frame_edge(1);
    update(-100);
    u8 state[2048] = {0};
    u8 *p = state + 4;
    p += main_write_savestate(p);
    *p++ = 0;
    unsigned size = p - state;
    p = state;
    bson_write_u32(p, size);
    assert(main_check_savestate(state));

    init_main();
    assert(gpsp_rv32_frame_cycle_debt == 0);
    assert(main_read_savestate(state));
    assert(execute_cycles == drawing_cycles - 64 &&
            gpsp_rv32_frame_cycle_debt == 36);
    assert(gpsp_rv32_take_cycles(execute_cycles,
            &gpsp_rv32_frame_cycle_debt) == (int)drawing_cycles - 100);
    assert(gpsp_rv32_take_cycles(execute_cycles,
            &gpsp_rv32_frame_cycle_debt) == (int)drawing_cycles - 64);

    const u8 *emu = bson_find_key(state, "emu");
    u8 *debt = (u8 *)bson_find_key(emu, "rv32-cycle-debt");
    p = debt;
    bson_write_u32(p, 0xffffffffu);
    assert(!main_check_savestate(state));

    /* An unknown key models an older state with no debt field. */
    debt[-(int)sizeof("rv32-cycle-debt")] = 'x';
    gpsp_rv32_frame_cycle_debt = 99;
    assert(main_check_savestate(state) && main_read_savestate(state));
    assert(gpsp_rv32_frame_cycle_debt == 0);
}

static void test_hblank(void)
{
    for (unsigned enabled = 0; enabled <= 1; enabled++)
    {
        init_main();
        memset(reg, 0, sizeof(reg));
        reg[CPU_HALT_STATE] = CPU_ACTIVE;
        write_ioreg(REG_VCOUNT, 0);
        write_ioreg(REG_DISPSTAT, enabled ? 0x10 : 0);
        serial_budget = 0x7fff;
        irq = 0;
        update_gba(1);
        assert(cpu_ticks == 1005 && !(read_ioreg(REG_DISPSTAT) & 2));
        unsigned next = update_gba(0);
        assert(cpu_ticks == 1006 && (read_ioreg(REG_DISPSTAT) & 2));
        assert(!(next & 0x40000000u));

        /* Preserve the delayed IRQ between the flag and interrupt edges. */
        u8 state[2048] = {0};
        u8 *p = state + 4;
        p += main_write_savestate(p);
        *p++ = 0;
        unsigned size = p - state;
        p = state;
        bson_write_u32(p, size);
        init_main();
        assert(main_check_savestate(state) && main_read_savestate(state));

        if (!enabled)
            write_ioreg(REG_DISPSTAT, read_ioreg(REG_DISPSTAT) | 0x10);
        next = update_gba(execute_cycles - 2);
        assert(cpu_ticks == 1008 && !(next & 0x40000000u));
        next = update_gba(execute_cycles - 1);
        assert(cpu_ticks == 1009 && !!(next & 0x40000000u) == enabled);
    }
}

int main(void)
{
    init_main();
    drawing_cycles = execute_cycles;
    test_frames();
    test_within_frame();
    test_dma_budget();
    test_state();
    test_hblank();
    puts("RV32 cycle budget: PASS (frame reentry, HALT, HBlank, IRQ, DMA, state)");
    return 0;
}
