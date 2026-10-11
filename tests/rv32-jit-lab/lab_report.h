/* Small, shared report format for RV32 translation experiments.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RV32_JIT_LAB_REPORT_H
#define RV32_JIT_LAB_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { LAB_THUMB_COUNT = 4 };

typedef struct
{
    const char *name;
    const char *segment_label;
    const uint8_t *code;
    size_t code_bytes;
    size_t entry_offsets[LAB_THUMB_COUNT];
    size_t instruction_bytes[LAB_THUMB_COUNT];
    size_t cache_header_bytes;
    size_t link_slots;
    size_t cold_bytes;
    size_t workspace_capacity_bytes;
    size_t emitter_stage_capacity_bytes;
    size_t metadata_bytes;
    uint64_t wall_ns;
    uint64_t cpu_ns;
    unsigned iterations;
    unsigned trials;
} lab_report_data;

void lab_print_report(const lab_report_data *report, bool dump);

#endif
