/* Shared infrastructure for the standalone ISA probe.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ISA_PROBE_H
#define ISA_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROBE_CODE_ALIGNMENT 256
#define PROBE_CODE_SIZE 256
#define PROBE_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef enum {
    PROBE_PASS,
    PROBE_FAIL,
    PROBE_UNAVAILABLE,
    PROBE_SKIPPED,
} probe_status_t;

typedef struct {
    uint32_t cause;
    uint32_t pc;
    uint32_t tval;
} probe_fault_t;

bool probe_sync(void *memory, size_t size);
void probe_report(const char *group, probe_status_t status,
        unsigned passed, unsigned total, const char *detail);

probe_status_t probe_zcb_baseline(uint8_t *code);
probe_status_t probe_core_baseline(uint8_t *code);
void probe_zcb_run(uint8_t *code);
void probe_zca_run(uint8_t *code);
void probe_zcmp_run(uint8_t *code);
void probe_zcmt_run(uint8_t *code);
void probe_zb_run(uint8_t *code);
void probe_core_run(uint8_t *code);
void probe_af_run(uint8_t *code);
void probe_xesploop_run(uint8_t *code);
void probe_xespv_run(uint8_t *code);
/* Caller masks interrupts; the snippet may change sp before faulting. */
bool probe_guarded_call(const void *code, uint32_t arg0, uint32_t arg1,
        uint32_t arg2, uint32_t *result, probe_fault_t *fault);
bool probe_guarded_call_access(const void *code, uint32_t arg0, uint32_t arg1,
        uint32_t arg2, uint32_t *result, probe_fault_t *fault);
bool probe_guarded_call_system(const void *code, uint32_t arg0, uint32_t arg1,
        uint32_t arg2, uint32_t *result, probe_fault_t *fault);

#endif
