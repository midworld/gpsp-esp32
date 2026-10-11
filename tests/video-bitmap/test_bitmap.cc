/* Bitmap BLIT clipping and mode 4 backdrop regression.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <array>
#include <stdint.h>
#include <stdio.h>
#include "../../video.cc"

u8 vram[96 * 1024];
u16 palette_ram_converted[512];
u16 io_registers[512];

static const s32 x_refs[] = {
  -0x08000000, -61441, -61440, -61439, -2049, -1025, -1024,
  -257, -256, -255, -1, 0, 1, 127, 255, 256, 257,
  159 * 256, 159 * 256 + 255, 160 * 256, 161 * 256,
  239 * 256, 239 * 256 + 255, 240 * 256, 241 * 256,
  98304 * 256, 0x07ffff00, 0x07ffffff
};
static const s32 y_refs[] = {
  -0x08000000, -257, -256, -1, 0, 1, 255, 256,
  127 * 256, 127 * 256 + 255, 128 * 256, 128 * 256 + 255,
  159 * 256, 159 * 256 + 255, 160 * 256, 0x07ffffff
};
static const unsigned segments[][2] = {
  {0, 0}, {0, 1}, {0, 3}, {0, 4}, {0, 5}, {0, 239}, {0, 240},
  {1, 8}, {2, 17}, {20, 64}, {60, 64}, {120, 240}, {235, 239},
  {236, 240}, {239, 240}, {240, 240}
};

static void fail(unsigned mode, bool mosaic, unsigned page, s32 x, s32 y,
    unsigned start, unsigned end, unsigned alignment)
{
  fprintf(stderr, "FAIL mode=%u mosaic=%u page=%u x=%d y=%d "
      "start=%u end=%u alignment=%u\n", mode, mosaic, page, x, y,
      start, end, alignment);
  exit(1);
}

template<rendtype rdtype, typename dsttype, unsigned mode,
    typename pixfmt, unsigned width, unsigned height, bool mosaic>
static unsigned test_blit(void)
{
  unsigned cases = 0;
  for (unsigned page = 0; page < 2; page++)
  for (s32 rx : x_refs)
  for (s32 ry : y_refs)
  for (const auto &segment : segments)
  for (unsigned alignment = 0; alignment < 4; alignment++)
  {
    alignas(8) std::array<dsttype, 248> actual, expected;
    actual.fill((dsttype)0xa5a5);
    expected = actual;
    write_ioreg(REG_DISPCNT, mode | (page ? 0x10 : 0));
    write_ioreg(REG_BG2PA, 256);
    write_ioreg(REG_BG2PC, 0);
    write_ioreg(REG_MOSAIC, 4);
    write_ioreg(REG_BLDCNT, 0x0404);
    affine_reference_x[0] = rx;
    affine_reference_y[0] = ry;
    unsigned first_visible = segment[1];
    for (unsigned x = segment[0]; x < segment[1]; x++)
    {
      int64_t sx = int64_t(rx) + int64_t(x) * 256;
      if (sx < 0 || sx >= width * 256 || ry < 0 || ry >= s32(height * 256))
        continue;
      if (first_visible == segment[1])
        first_visible = x;
      if (mosaic)
        sx = int64_t(rx) + int64_t(first_visible +
            ((x - first_visible) / 5) * 5) * 256;
      unsigned pixel = unsigned(ry / 256) * width + unsigned(sx / 256);
      unsigned offset = ((mode >= 4 && page) ? 0xa000 : 0) +
          pixel * sizeof(pixfmt);
      pixfmt value;
      memcpy(&value, vram + offset, sizeof(value));
      dsttype &output = expected[alignment + x];
      if (mode != 4)
        output = convert_palette(eswap16(value));
      else if (value)
      {
        if (rdtype == FULLCOLOR)
          output = palette_ram_converted[value];
        else if (rdtype == INDXCOLOR)
          output = value | color_flags(2);
        else
          output = value | color_flags(2) | ((u32)output << 16);
      }
    }
    render_scanline_bitmap<rdtype, dsttype, mode, pixfmt, width, height,
        BLIT, mosaic>(segment[0], segment[1], actual.data() + alignment,
        palette_ram_converted);
    if (actual != expected)
      fail(mode, mosaic, page, rx, ry, segment[0], segment[1], alignment);
    cases++;
  }
  return cases;
}

static unsigned test_mode4_backdrop(void)
{
  unsigned cases = 0;
  for (unsigned page = 0; page < 2; page++)
  for (s32 rx : x_refs)
  for (s32 ry : y_refs)
  for (const auto &segment : segments)
  for (unsigned alignment = 0; alignment < 4; alignment++)
  {
    alignas(8) std::array<u16, 248> actual, expected;
    actual.fill(0xa5a5);
    expected = actual;
    write_ioreg(REG_DISPCNT, 4 | (page ? 0x10 : 0));
    affine_reference_x[0] = rx;
    affine_reference_y[0] = ry;
    for (unsigned x = segment[0]; x < segment[1]; x++)
    {
      int64_t sx = int64_t(rx) + int64_t(x) * 256;
      u16 color = palette_ram_converted[0];
      if (sx >= 0 && sx < 240 * 256 && ry >= 0 && ry < 160 * 256)
      {
        unsigned offset = (page ? 0xa000 : 0) +
            unsigned(ry / 256) * 240 + unsigned(sx / 256);
        color = palette_ram_converted[vram[offset]];
      }
      expected[alignment + x] = color;
    }
    render_mode4_blit_backdrop(segment[0], segment[1], actual.data() + alignment);
    if (actual != expected)
      fail(4, false, page, rx, ry, segment[0], segment[1], alignment);
    cases++;
  }
  return cases;
}

int main(void)
{
  for (unsigned i = 0; i < sizeof(vram); i++)
    vram[i] = (i * 37u) ^ (i >> 4);
  for (unsigned i = 0; i < 512; i++)
    palette_ram_converted[i] = i * 977u + 12345u;
  unsigned cases = 0;
  cases += test_blit<FULLCOLOR, u16, 3, u16, 240, 160, false>();
  cases += test_blit<FULLCOLOR, u16, 3, u16, 240, 160, true>();
  cases += test_blit<FULLCOLOR, u16, 4, u8, 240, 160, false>();
  cases += test_blit<FULLCOLOR, u16, 4, u8, 240, 160, true>();
  cases += test_blit<FULLCOLOR, u16, 5, u16, 160, 128, false>();
  cases += test_blit<FULLCOLOR, u16, 5, u16, 160, 128, true>();
  cases += test_blit<INDXCOLOR, u16, 4, u8, 240, 160, false>();
  cases += test_blit<INDXCOLOR, u16, 4, u8, 240, 160, true>();
  cases += test_blit<STCKCOLOR, u32, 4, u8, 240, 160, false>();
  cases += test_blit<STCKCOLOR, u32, 4, u8, 240, 160, true>();
  printf("PASS: %u bitmap BLIT clipping cases\n", cases);
  printf("PASS: %u mode 4 backdrop cases\n", test_mode4_backdrop());
  return 0;
}
