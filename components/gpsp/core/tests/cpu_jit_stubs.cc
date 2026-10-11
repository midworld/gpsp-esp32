/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Interpreter fixtures have no translated RAM cache. */
extern "C" void invalidate_translation_cache_ram(void) {}

extern "C" void gpsp_rv32_add_memory_alert(unsigned int) {}
