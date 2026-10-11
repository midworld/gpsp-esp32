/* Core regression tests.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "common.h"
#include "gpsp_screen.h"
#include <assert.h>

/* Seed the mix ring without a frontend or an audio device. */
#include "../../sound.c"

static u8 state[GBA_STATE_MEM_SIZE];
static u8 damaged[GBA_STATE_MEM_SIZE];
static u8 before[GBA_STATE_MEM_SIZE];
static u8 after[GBA_STATE_MEM_SIZE];
static u16 buffers[2][GPSP_SCREEN_BUFFER_BYTES / sizeof(u16)];
static unsigned buffer_index, frames;

static bool environment(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE)
    {
        struct retro_variable *var = data;
        var->value = NULL;
        if (!strcmp(var->key, "gpsp_bios"))
            var->value = "builtin";
        if (!strcmp(var->key, "gpsp_boot_mode"))
            var->value = "game";
        return var->value != NULL;
    }
    return cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT;
}

static void video(const void *data, unsigned width, unsigned height, size_t pitch)
{
    assert(data == buffers[buffer_index]);
    assert(width == 240 && height == 160 && pitch == 480);
    assert(((const u16 *)data)[0] == convert_palette(0x001f));
    assert(((const u16 *)data)[1] == convert_palette(0x7c00));
    unsigned next = buffer_index ^ 1;
    assert(gpsp_screen_set_buffer(buffers[next]) == buffers[buffer_index]);
    buffer_index = next;
    frames++;
}

static size_t audio(const int16_t *data, size_t count)
{
    (void)data;
    return count;
}

static void poll(void) {}
static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)port; (void)device; (void)index; (void)id;
    return 0;
}

static void save(u8 *dst)
{
    memset(dst, 0, GBA_STATE_MEM_SIZE);
    assert(retro_serialize(dst, GBA_STATE_MEM_SIZE));
}

static u8 *field(u8 *doc, const char *key)
{
    u8 *p = (u8 *)bson_find_key(doc, key);
    assert(p);
    return p;
}

static void put32(u8 *p, u32 value)
{
    bson_write_u32(p, value);
}

static void rejected(void)
{
    save(before);
    assert(!retro_unserialize(damaged, sizeof(damaged)));
    save(after);
    assert(!memcmp(before, after, sizeof(before)));
}

static void test_state(void)
{
    save(state);
    reg[0] = 0x12345678;
    assert(retro_unserialize(state, sizeof(state)));
    assert(reg[0] != 0x12345678);
    assert(!retro_unserialize(state, sizeof(state) - 1));

    memcpy(damaged, state, sizeof(state));
    put32(damaged, 4);
    rejected();
    memcpy(damaged, state, sizeof(state));
    put32(field(damaged, "cpu"), sizeof(state));
    rejected();
    memcpy(damaged, state, sizeof(state));
    damaged[4] = 0x7f;
    rejected();
    memcpy(damaged, state, sizeof(state));
    u8 *regs = field(field(damaged, "cpu"), "regs");
    regs[4] = BSON_TYPE_DOC;
    rejected();
    memcpy(damaged, state, sizeof(state));
    regs = field(field(damaged, "cpu"), "regs");
    put32(regs + 8 + CPU_MODE * 8, 0xffffffff);
    rejected();
    memcpy(damaged, state, sizeof(state));
    put32(field(field(field(damaged, "sound"), "ds1"), "fifo-base"), 32);
    rejected();
    memcpy(damaged, state, sizeof(state));
    put32(field(field(field(damaged, "sound"), "gs3"), "rate"), 2048);
    rejected();
    memcpy(damaged, state, sizeof(state));
    put32(field(field(damaged, "sound"), "wav-samples"), 1);
    rejected();
    memcpy(damaged, state, sizeof(state));
    u8 *snd = field(damaged, "sound");
    snd[-(int)sizeof("sound") - 1] = BSON_TYPE_BIN;
    rejected();
    puts("state: PASS (round trip, malformed documents, no partial mutation)");
}

static void test_irq(void)
{
    reset_gba();
    reg[REG_CPSR] &= ~0x80u;
    reg[REG_PC] = 0x08000100;
    write_ioreg(REG_IE, IRQ_TIMER0);
    write_ioreg(REG_IME, 1);
    flag_interrupt(IRQ_TIMER0);
    assert(check_and_raise_interrupts());
    assert(reg[REG_PC] == 0x18 && reg[CPU_MODE] == MODE_IRQ);
    assert(REG_MODE(MODE_IRQ)[6] == 0x08000104);
    assert(take_irq_entry_cycles() == 3);
    assert(take_irq_entry_cycles() == 0);

    /* An entry cost larger than the next budget carries into the next call. */
    irq_entry_cycles = 3;
    execute_cycles = 1;
    assert(frame_start_cycles() == 0);
    execute_cycles = 10;
    assert(frame_start_cycles() == 8);
    assert(frame_start_cycles() == 10);
    puts("IRQ: PASS (vector, return address, entry cost, carried stall)");
}

static void set_rate(u32 bits)
{
    sound_freq_bits = bits;
    sound_frequency = 1u << bits;
    sound_frequency_changed();
}

static void test_sound(void)
{
    reset_gba();
    gbc_sound_channel[0].rate = 1024;
    gbc_sound_channel[2].rate = 1024;
    write_ioreg(REG_SOUND4CNT_H, 1);
    set_rate(16);
    save(state);
    set_rate(15);
    assert(retro_unserialize(state, sizeof(state)));
    assert(sound_frequency == 32768);
    assert(gbc_sound_channel[0].frequency_step == 2048);
    assert(gbc_sound_channel[2].frequency_step == 4096);
    assert(gbc_sound_channel[3].frequency_step == 524288);

    for (u32 bits = 15; bits <= 16; bits++)
    {
        reset_sound();
        set_rate(bits);
        sound_on = 1;
        gbc_sound_channel[0].active_flag = 1;
        gbc_sound_channel[0].status = GBC_SOUND_LEFTRIGHT;
        gbc_sound_channel[0].length_status = 1;
        gbc_sound_channel[0].length_ticks = 10;
        /* 1/256 second advances the length unit once at either rate. */
        cpu_ticks = gbc_sound_last_cpu_ticks + 65536;
        render_gbc_sound();
        assert(gbc_sound_channel[0].length_ticks == 9);
    }

    gbc_sound_channel[3].sample_index = u32_to_fp16_16(1000) + 123;
    write_memory16(0x04000000 + REG_SOUND4CNT_H * 2, 8);
    assert(gbc_sound_channel[3].sample_index == u32_to_fp16_16(118) + 123);
    save(state);
    put32(field(field(field(state, "sound"), "gs3"), "sample-idx"), 0xffffffff);
    assert(retro_unserialize(state, sizeof(state)));
    assert(gbc_sound_channel[3].sample_index < u32_to_fp16_16(gbc_noise_wrap_half));

    for (u32 shift = 0; shift <= 1; shift++)
    {
        s16 samples[2];
        sound_buffer_base = 0;
        gbc_sound_buffer_index = 514;
        sound_buffer[0] = 40000;
        sound_buffer[1] = -40000;
        sound_headroom_shift = shift;
        assert(sound_read_samples(samples, 1) == 1);
        assert(samples[0] == (shift ? 20000 : 32767));
        assert(samples[1] == (shift ? -20000 : -32768));
        assert(sound_buffer[0] == 0 && sound_buffer[1] == 0);
        sound_flush_ring();
        assert(sound_read_samples(samples, 1) == 0);
    }
    puts("sound: PASS (rate, state, length clock, noise wrap, headroom)");
}

int main(void)
{
    static const u8 rom[32768] = {0xfe, 0xff, 0xff, 0xea}; /* b . */
    const char *path = ROM_DIR "/regression.gba";
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(rom, 1, sizeof(rom), file) == sizeof(rom));
    assert(!fclose(file));
    struct retro_game_info game = { .path = path };
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio);
    retro_set_input_poll(poll);
    retro_set_input_state(input);
    retro_init();
    assert(retro_load_game(&game));

    u16 *owned = gpsp_screen_set_buffer(NULL);
    assert(owned && gpsp_screen_set_buffer(buffers[0]) == owned);
    assert(gpsp_screen_set_buffer(NULL) == buffers[0]);
    write_memory16(0x06000000, 0x001f);
    write_memory16(0x06000002, 0x7c00);
    write_memory16(0x04000000, 0x0403);
    for (unsigned i = 0; i < 4; i++)
        retro_run();
    assert(frames == 4);
    assert(gpsp_screen_set_buffer(owned) == buffers[buffer_index]);
    puts("screen: PASS (query, alternating buffers, pixels, restore)");

    test_state();
    test_irq();
    test_sound();
    retro_unload_game();
    retro_deinit();
    assert(!gpsp_screen_set_buffer(NULL));
    return 0;
}
