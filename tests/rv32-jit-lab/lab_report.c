/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "lab_report.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void lab_print_report(const lab_report_data *report, bool dump)
{
    static const uint16_t opcodes[LAB_THUMB_COUNT] = {
        0x4200, 0xd000, 0x2102, 0x2203
    };

    printf("translator=%s\n", report->name);
    printf("guest_pc=0x08000100 guest_opcodes=4200,d000,2102,2203\n");
    printf("code_bytes=%zu rv32_words=%zu cache_header_bytes=%zu "
            "cache_total_bytes=%zu\n", report->code_bytes,
            report->code_bytes / 4, report->cache_header_bytes,
            report->code_bytes + report->cache_header_bytes);
    for (unsigned i = 0; i < LAB_THUMB_COUNT; i++)
    {
        const char *label = report->segment_label ? report->segment_label : "op";
        if (report->entry_offsets[i] == SIZE_MAX)
            printf("%s[%u]=0x%04x offset=none bytes=%zu\n", label, i,
                    opcodes[i], report->instruction_bytes[i]);
        else
            printf("%s[%u]=0x%04x offset=%zu bytes=%zu\n", label, i,
                    opcodes[i], report->entry_offsets[i],
                    report->instruction_bytes[i]);
    }
    printf("link_slots=%zu cold_bytes=%zu\n", report->link_slots,
            report->cold_bytes);
    printf("workspace_capacity_bytes=%zu emitter_stage_capacity_bytes=%zu "
            "metadata_bytes=%zu\n", report->workspace_capacity_bytes,
            report->emitter_stage_capacity_bytes, report->metadata_bytes);
    printf("timing_iterations_per_trial=%u timing_trials=%u "
            "cpu_ns_median_trial=%" PRIu64
            " cpu_ns_per_translation=%.1f wall_ns_median_trial=%" PRIu64
            " wall_ns_per_translation=%.1f\n", report->iterations,
            report->trials ? report->trials : 1,
            report->cpu_ns, (double)report->cpu_ns / report->iterations,
            report->wall_ns, (double)report->wall_ns / report->iterations);

    if (!dump)
        return;
    for (size_t i = 0; i < report->code_bytes; i += 4)
    {
        uint32_t word;
        memcpy(&word, report->code + i, sizeof(word));
        printf("rv32[%03zu]=0x%08x\n", i / 4, word);
    }
}
