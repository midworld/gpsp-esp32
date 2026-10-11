/* ROM cache regression tests.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "common.h"
#include <streams/file_stream.h>
#include <assert.h>

static int allocations_left = -1;
static unsigned reads;

static void *test_malloc(size_t size)
{
    if (allocations_left == 0)
        return NULL;
    if (allocations_left > 0)
        allocations_left--;
    return malloc(size);
}

static int64_t test_read(RFILE *file, void *dst, int64_t size)
{
    reads++;
    return filestream_read(file, dst, size);
}

#define malloc test_malloc
#define filestream_read test_read
#include "../../gba_memory.c"
#undef malloc
#undef filestream_read

static void test_resize(void)
{
    assert(!gpsp_set_rom_buffer_limit(0));
    assert(!gpsp_set_rom_buffer_limit(ROM_BUFFER_SIZE + 1));
    assert(gpsp_set_rom_buffer_limit(4));
    allocations_left = 0;
    init_gamepak_buffer();
    assert(gamepak_buffer_count == 0);
    allocations_left = 2;
    fit_gamepak_buffer(4 * 1024 * 1024);
    assert(gamepak_buffer_count == 2);
    assert(!gpsp_set_rom_buffer_limit(8));
    u8 *first = gamepak_buffers[0];
    fit_gamepak_buffer(1);
    assert(gamepak_buffer_count == 1 && gamepak_buffers[0] == first);
    allocations_left = 0;
    fit_gamepak_buffer(8 * 1024 * 1024);
    assert(gamepak_buffer_count == 1 && gamepak_buffers[0] == first);
    allocations_left = -1;
    fit_gamepak_buffer(1024 * 1024 + 1);
    assert(gamepak_buffer_count == 2);
    fit_gamepak_buffer(32 * 1024 * 1024);
    assert(gamepak_buffer_count == 4);
    fit_gamepak_buffer(0);
    assert(gamepak_buffer_count == 1);
    for (unsigned i = 0; i < 1024; i++)
        assert(gamepak_slot_page[i] == -1 && gamepak_page_slot[i] == -1);
}

static void check_page(unsigned page)
{
    u8 *p = load_gamepak_page(page);
    assert(p[0x200] == page && p[0x7fff] == page);
    assert(gamepak_page_slot[page] >= 0);
    assert(gamepak_slot_page[gamepak_page_slot[page]] == (int)page);
    for (unsigned base = 0x08000000; base < 0x0e000000; base += 0x02000000)
        assert(memory_map_read[(base >> 15) + page] == p);
}

int main(void)
{
    test_resize();
    const char *path = ROM_DIR "/paging.gba";
    FILE *file = fopen(path, "wb");
    u8 page[32768];
    assert(file);
    for (unsigned i = 0; i < 128; i++)
    {
        memset(page, i, sizeof(page));
        assert(fwrite(page, 1, sizeof(page), file) == sizeof(page));
    }
    assert(!fclose(file));
    gamepak_file_large = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, 0);
    assert(gamepak_file_large);
    gamepak_size = 128 * sizeof(page);
    gamepak_file_blocks = 128;
    clear_gamepak_stickybits();

    for (unsigned i = 0; i < 32; i++)
        check_page(i);
    assert(reads == 32);
    touch_gamepak_page(0);
    touch_gamepak_page(31);
    check_page(32);
    assert(reads == 33 && gamepak_page_slot[1] == -1);
    assert(gamepak_page_slot[0] >= 0 && gamepak_page_slot[31] >= 0);
    assert(!memory_map_read[(0x08000000 >> 15) + 2]);
    check_page(2);
    assert(reads == 33); /* Resident remap needs no disk read. */
    check_page(1);
    assert(reads == 34);
    assert(load_gamepak_page(129)[0x200] == 1 && reads == 34);

    for (unsigned i = 33; i < 128; i++)
        check_page(i);
    check_page(0);
    check_page(31);
    assert(reads == 129);
    clear_gamepak_stickybits();
    assert(!filestream_close(gamepak_file_large));
    gamepak_file_large = NULL;
    while (gamepak_buffer_count)
        free(gamepak_buffers[--gamepak_buffer_count]);
    assert(gpsp_set_rom_buffer_limit(8));
    init_gamepak_buffer();
    assert(gamepak_buffer_count == 8);
    fit_gamepak_buffer(32 * 1024 * 1024);
    assert(gamepak_buffer_count == 8);
    while (gamepak_buffer_count)
        free(gamepak_buffers[--gamepak_buffer_count]);
    puts("ROM cache: PASS (growth failure, resizing, clock, sticky, remap, mirrors)");
    return 0;
}
