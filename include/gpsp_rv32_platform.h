/* RV32 JIT services supplied by the frontend.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_RV32_PLATFORM_H
#define GPSP_RV32_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    GPSP_RV32_LOG_INFO,
    GPSP_RV32_LOG_WARN
} gpsp_rv32_log_level;

struct gpsp_rv32_platform_ops
{
    void *(*exec_alloc)(size_t alignment, size_t size);
    void (*exec_free)(void *code);
    bool (*exec_publish)(void *code, size_t size);
    void (*vlog)(gpsp_rv32_log_level level, const char *tag,
            const char *format, va_list args);
};

/* Register before loading a game. Keep ops alive and unchanged until all
 * JIT caches are released; allocations must be freed by the same provider.
 * exec_publish makes writes visible to instruction fetch before execution.
 * NULL or missing required callbacks disable executable allocation. */
void gpsp_rv32_set_platform(const struct gpsp_rv32_platform_ops *ops);
void *gpsp_rv32_exec_alloc(size_t alignment, size_t size);
void gpsp_rv32_exec_free(void *code);
bool gpsp_rv32_exec_publish(void *code, size_t size);
void gpsp_rv32_log(gpsp_rv32_log_level level, const char *tag,
        const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_RV32_PLATFORM_H */
