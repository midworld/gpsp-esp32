/* Game switch test: a game loaded after another must start as from cold.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "common.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ROM_DIR
#define ROM_DIR "build/roms"
#endif

/* Guest setup run after load or before unload, as a game would do it. */
typedef void (*hook_fn)(void);

struct side
{
    const char *rom;
    hook_fn after_load;
    hook_fn before_unload;
};

struct result
{
    u32 rom_c4;
    u32 serial_cycles;
    u16 pixels[2];
    u16 palette[512];
};

static unsigned frame_index;
static u16 frame_pixels[2];

/* Palette 0 to red, then a blank display. */
static const u32 red_code[] = {
    0xe3a00405, /* mov r0, #0x05000000 */
    0xe3a0101f, /* mov r1, #0x1f */
    0xe1c010b0, /* strh r1, [r0] */
    0xe3a00301, /* mov r0, #0x04000000 */
    0xe3a01000, /* mov r1, #0 */
    0xe1c010b0, /* strh r1, [r0] */
    0xeafffffe, /* b . */
};

/* A blank display; palette 0 stays black. */
static const u32 black_code[] = {
    0xe3a00301, /* mov r0, #0x04000000 */
    0xe3a01000, /* mov r1, #0 */
    0xe1c010b0, /* strh r1, [r0] */
    0xeafffffe, /* b . */
};

/* Leaves all setup to the hooks. */
static const u32 loop_code[] = {
    0xeafffffe, /* b . */
};

/* Multiplayer, 9600 bps, start: a slow transfer. */
static void serial_slow(void)
{
    write_rcnt(0);
    write_siocnt(0x6080);
}

/* Multiplayer, 115200 bps, start: a fast transfer. */
static void serial_fast(void)
{
    write_rcnt(0);
    write_siocnt(0x6083);
}

/* Red 4bpp tile 0 for objects, objects on, no OAM writes. */
static void sprite_show(void)
{
    write_memory16(0x05000202, 0x001f);
    for (unsigned i = 0; i < 32; i += 2)
        write_memory16(0x06010000 + i, 0x1111);
    write_memory16(0x04000000, 0x1000);
}

/* Hides all objects but one, then shows them for two frames. */
static void sprite_play(void)
{
    for (unsigned i = 0; i < 128; i++)
        write_memory16(0x07000000 + i * 8, 0x0200);
    write_memory16(0x07000000, 0);
    sprite_show();
    retro_run();
    retro_run();
}

static void bg2_move(void)
{
    write_memory16(0x04000000 + REG_BG2X_L * 2, 256);
}

/* Mode 3 with red and blue as the first two pixels. */
static void bitmap_show(void)
{
    write_memory16(0x06000000, 0x001f);
    write_memory16(0x06000002, 0x7c00);
    write_memory16(0x04000000, 0x0403);
}

static void write_rom(const char *name, const char *code_name,
        const u32 *code, size_t words, size_t size)
{
    static u8 rom[1024 * 1024];
    char path[64];
    FILE *file;

    memset(rom, 0, size);
    /* b 0xc0 */
    rom[0] = 0x2e;
    rom[3] = 0xea;
    memcpy(&rom[0xa0], "SWITCH TEST ", 12);
    memcpy(&rom[0xac], code_name, 4);
    rom[0xb2] = 0x96;
    memcpy(&rom[0xc0], code, words * sizeof(*code));
    snprintf(path, sizeof(path), ROM_DIR "/%s", name);
    file = fopen(path, "wb");
    if (!file || fwrite(rom, 1, size, file) != size)
    {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fclose(file);
}

static bool environment(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE)
    {
        struct retro_variable *var = data;

        if (!strcmp(var->key, "gpsp_bios"))
            var->value = "builtin";
        else if (!strcmp(var->key, "gpsp_boot_mode"))
            var->value = "game";
        else if (!strcmp(var->key, "gpsp_frameskip"))
            var->value = "disabled";
        else
            var->value = NULL;
        return var->value != NULL;
    }
    return cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT;
}

static void video(const void *data, unsigned width, unsigned height,
        size_t pitch)
{
    (void)width;
    (void)height;
    (void)pitch;
    if (data && frame_index < 2)
        frame_pixels[frame_index++] = *(const u16 *)data;
}

static size_t audio(const int16_t *data, size_t frames)
{
    (void)data;
    return frames;
}

static void input_poll(void)
{
}

static int16_t input_state(unsigned port, unsigned device, unsigned index,
        unsigned id)
{
    (void)port;
    (void)device;
    (void)index;
    (void)id;
    return 0;
}

static void run(const struct side *side, struct result *result)
{
    char path[64];
    struct retro_game_info game = { .path = path };

    snprintf(path, sizeof(path), ROM_DIR "/%s", side->rom);
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();
    if (!retro_load_game(&game))
    {
        fprintf(stderr, "cannot load %s\n", path);
        exit(1);
    }
    if (side->after_load)
        side->after_load();
    memcpy(&result->rom_c4, memory_map_read[0x8000000 >> 15] + 0xc4, 4);
    result->serial_cycles = serial_get_irq_cycles();
    frame_index = 0;
    frame_pixels[0] = frame_pixels[1] = 0xffff;
    /* Each run must deliver its frame, or the pixels are not compared. */
    for (unsigned i = 1; i <= 2; i++)
    {
        retro_run();
        if (frame_index != i)
        {
            fprintf(stderr, "%s: no frame %u\n", path, i);
            exit(1);
        }
    }
    memcpy(result->pixels, frame_pixels, sizeof(result->pixels));
    memcpy(result->palette, palette_ram_converted, sizeof(result->palette));
    if (side->before_unload)
        side->before_unload();
    retro_unload_game();
    retro_deinit();
}

/* Runs the sides in order in a new process and returns the last result,
   so earlier cases cannot leave state behind. */
static void run_fresh(const struct side *first, const struct side *second,
        struct result *result)
{
    int fds[2];
    pid_t pid;
    int status;

    if (pipe(fds) || (pid = fork()) < 0)
    {
        perror("fork");
        exit(1);
    }
    if (!pid)
    {
        close(fds[0]);
        if (first)
            run(first, result);
        run(second, result);
        if (write(fds[1], result, sizeof(*result)) != (ssize_t)sizeof(*result))
            _exit(1);
        _exit(0);
    }
    close(fds[1]);
    if (read(fds[0], result, sizeof(*result)) != (ssize_t)sizeof(*result) ||
            waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status))
    {
        fprintf(stderr, "child failed\n");
        exit(1);
    }
    close(fds[0]);
}

static int check(const char *title, const struct side *first,
        const struct side *second)
{
    struct result cold;
    struct result after;

    run_fresh(NULL, second, &cold);
    run_fresh(first, second, &after);
    if (cold.rom_c4 != after.rom_c4 || cold.serial_cycles != after.serial_cycles ||
            memcmp(cold.pixels, after.pixels, sizeof(cold.pixels)) ||
            memcmp(cold.palette, after.palette, sizeof(cold.palette)))
    {
        printf("FAIL %s: rom_c4 %08x/%08x serial %u/%u "
                "pixels %04x,%04x/%04x,%04x\n", title, cold.rom_c4,
                after.rom_c4, cold.serial_cycles, after.serial_cycles,
                cold.pixels[0], cold.pixels[1], after.pixels[0], after.pixels[1]);
        return 1;
    }
    printf("ok %s: rom_c4 %08x serial %u pixels %04x,%04x\n", title,
            cold.rom_c4, cold.serial_cycles, cold.pixels[0], cold.pixels[1]);
    return 0;
}

#define ROM_WORDS(code) code, sizeof(code) / sizeof(code[0])

int main(void)
{
    static const struct side red = { "red.gba", NULL, NULL };
    static const struct side black = { "black.gba", NULL, NULL };
    static const struct side rtc = { "rtc.gba", NULL, NULL };
    static const struct side mini = { "mini.gba", NULL, NULL };
    static const struct side serial_old = { "red.gba", NULL, serial_slow };
    static const struct side serial_new = { "black.gba", serial_fast, NULL };
    static const struct side sprite_old = { "loop.gba", sprite_play, NULL };
    static const struct side sprite_new = { "loop.gba", sprite_show, NULL };
    static const struct side bg2_old = { "loop.gba", NULL, bg2_move };
    static const struct side bg2_new = { "loop.gba", bitmap_show, NULL };
    int failed = 0;

    write_rom("red.gba", "SWTA", ROM_WORDS(red_code), 32 * 1024);
    write_rom("black.gba", "SWTB", ROM_WORDS(black_code), 32 * 1024);
    write_rom("loop.gba", "SWTC", ROM_WORDS(loop_code), 32 * 1024);
    /* A game code with an RTC in the game database. */
    write_rom("rtc.gba", "AXVE", ROM_WORDS(red_code), 32 * 1024);
    /* Exactly 1MB: loaded as a mirrored mini ROM. */
    write_rom("mini.gba", "SWTM", ROM_WORDS(black_code), 1024 * 1024);

    failed += check("palette", &red, &black);
    failed += check("rtc then mini ROM", &rtc, &mini);
    failed += check("serial transfer", &serial_old, &serial_new);
    failed += check("sprite list", &sprite_old, &sprite_new);
    failed += check("bg2 reference point", &bg2_old, &bg2_new);
    return failed ? 1 : 0;
}
