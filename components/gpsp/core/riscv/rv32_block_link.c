/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_block_link.h"

#include "rv32_register_map.h"

#include <string.h>

static bool valid_code(const gpsp_rv32_code *code)
{
  if (!code || !code->start || !code->ptr || !code->end)
    return false;

  uintptr_t start = (uintptr_t)code->start;
  uintptr_t cursor = (uintptr_t)code->ptr;
  uintptr_t end = (uintptr_t)code->end;
  /* Slots themselves are aligned; a buffer may start after 16-bit code. */
  return !(start & 1) && start <= cursor && cursor <= end;
}

bool gpsp_rv32_emit_link_slot(gpsp_rv32_code *code, uint8_t **site)
{
  if (!site || !valid_code(code) || ((uintptr_t)code->ptr & 1))
    return false;
  /* Patches rewrite the slot as words; code before it may fall through. */
  size_t pad = (uintptr_t)code->ptr & 2;
  if ((uintptr_t)code->end - (uintptr_t)code->ptr <
          pad + GPSP_RV32_LINK_SLOT_SIZE ||
      !gpsp_rv32_align4(code))
    return false;

  uint32_t words[GPSP_RV32_LINK_SLOT_SIZE / sizeof(uint32_t)];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  /* Patching rewrites these words in place. */
  if (!gpsp_rv32_fixed_begin(&staged) ||
      !gpsp_rv32_jalr(&staged, X0, RA, 0) ||
      !gpsp_rv32_addi(&staged, X0, X0, 0))
    return false;

  uint8_t *slot = code->ptr;
  memcpy(slot, words, sizeof(words));
  code->ptr += sizeof(words);
  *site = slot;
  return true;
}

static bool slot_unlinked(const uint32_t words[2])
{
  return words[0] == 0x00008067u && words[1] == 0x00000013u;
}

static bool slot_linked(const uint32_t words[2])
{
  return ((words[0] & 0xfffu) == ((RV_TMP1 << 7) | 0x37u) &&
      (words[1] & 0xfffffu) == ((RV_TMP1 << 15) | 0x67u)) ||
      ((words[0] & 0xfffu) == 0x06fu && words[1] == 0x00000013u);
}

bool gpsp_rv32_patch_link_slot(gpsp_rv32_code *code, uint8_t *site,
    uintptr_t target)
{
  if (!valid_code(code) || !site || !target || (target & 3))
    return false;
#if UINTPTR_MAX > UINT32_MAX
  if (target > UINT32_MAX)
    return false;
#endif

  uintptr_t start = (uintptr_t)code->start;
  uintptr_t cursor = (uintptr_t)code->ptr;
  uintptr_t at = (uintptr_t)site;
  if (cursor - start < GPSP_RV32_LINK_SLOT_SIZE || at < start ||
      (at & 3) || at > cursor - GPSP_RV32_LINK_SLOT_SIZE)
    return false;

  uint32_t old[GPSP_RV32_LINK_SLOT_SIZE / sizeof(uint32_t)];
  memcpy(old, site, sizeof(old));
  if (!slot_unlinked(old) && !slot_linked(old))
    return false;

  uint32_t address = (uint32_t)target;
  int lower = (int)(address & 0xfff);
  if (lower >= 0x800)
    lower -= 0x1000;
  uint32_t upper = ((address + 0x800u) >> 12) & 0xfffffu;

  uint32_t words[GPSP_RV32_LINK_SLOT_SIZE / sizeof(uint32_t)];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  if (!gpsp_rv32_fixed_begin(&staged))
    return false;
  /* A near target takes one jal; the second word is never reached. */
  int64_t delta = (int64_t)target - (int64_t)at;
  if (delta >= -(1 << 20) && delta < (1 << 20))
  {
    if (!gpsp_rv32_jal(&staged, X0, (int32_t)delta) ||
        !gpsp_rv32_addi(&staged, X0, X0, 0))
      return false;
  }
  else if (!gpsp_rv32_lui(&staged, RV_TMP1, upper) ||
      !gpsp_rv32_jalr(&staged, X0, RV_TMP1, lower))
    return false;

  memcpy(site, words, sizeof(words));
  return true;
}

bool gpsp_rv32_unlink_link_slot(uint8_t *site)
{
  if (!site || ((uintptr_t)site & 3))
    return false;

  uint32_t words[GPSP_RV32_LINK_SLOT_SIZE / sizeof(uint32_t)];
  memcpy(words, site, sizeof(words));
  if (!slot_unlinked(words) && !slot_linked(words))
    return false;

  /* Same words as gpsp_rv32_emit_link_slot. */
  words[0] = 0x00008067u;
  words[1] = 0x00000013u;
  memcpy(site, words, sizeof(words));
  return true;
}
