/* Check whether timer interrupts reach Zcmp return instructions.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "probe.h"

extern const uint8_t zcmp_irq_ret_start[];
extern const uint8_t zcmp_irq_ret_op[];
extern const uint8_t zcmp_irq_ret_end[];
extern const uint8_t zcmp_irq_retz_start[];
extern const uint8_t zcmp_irq_retz_op[];
extern const uint8_t zcmp_irq_retz_end[];
extern const uint8_t zcmp_irq_ret_full_start[];
extern const uint8_t zcmp_irq_ret_full_op[];
extern const uint8_t zcmp_irq_ret_full_end[];
extern const uint8_t zcmp_irq_retz_full_start[];
extern const uint8_t zcmp_irq_retz_full_op[];
extern const uint8_t zcmp_irq_retz_full_end[];

static const char *TAG = "zcmp_irq";
static volatile uint32_t interrupt_count;
static volatile uint32_t function_hits;
static volatile uint32_t operation_hits;
static volatile uintptr_t function_start;
static volatile uintptr_t function_end;
static volatile uintptr_t operation_pc;

typedef uint32_t (*zcmp_irq_fn_t)(uint32_t value);

static void IRAM_ATTR timer_callback(void *arg)
{
    uint32_t pc;
    __asm__ volatile ("csrr %0, mepc" : "=r"(pc));
    interrupt_count++;
    if (pc >= function_start && pc < function_end)
        function_hits++;
    if (pc == operation_pc)
        operation_hits++;
}

static void run_case(esp_timer_handle_t timer, const uint8_t *start,
        const uint8_t *end, const uint8_t *operation, uint8_t *code,
        bool ram, bool zero, const char *name)
{
    const uint8_t *entry = start;
    size_t length = (size_t)(end - start);
    if (length > PROBE_CODE_SIZE)
        return;
    if (ram)
    {
        memcpy(code, start, length);
        if (!probe_sync(code, PROBE_CODE_SIZE))
            return;
        entry = code;
    }

    function_start = (uintptr_t)entry;
    function_end = (uintptr_t)(entry + length);
    operation_pc = (uintptr_t)(entry + (operation - start));
    interrupt_count = 0;
    function_hits = 0;
    operation_hits = 0;

    if (esp_timer_start_periodic(timer, 20) != ESP_OK)
    {
        ESP_LOGE(TAG, "timer start failed");
        return;
    }
    zcmp_irq_fn_t fn = (zcmp_irq_fn_t)entry;
    uint32_t errors = 0;
    for (unsigned i = 0; i < 2000000; i++)
    {
        uint32_t result = fn(0x5a5aa5a5);
        if (result != (zero ? 0 : 0x5a5aa5a5))
            errors++;
    }
    bool stopped = esp_timer_stop(timer) == ESP_OK;
    if (!stopped)
        ESP_LOGE(TAG, "timer stop failed");
    bool passed = stopped && !errors && operation_hits;
    ESP_LOGI(TAG, "%s %s %s calls=2000000 errors=%lu interrupts=%lu "
            "in_function=%lu at_operation=%lu",
            ram ? "RAM" : "flash", name, passed ? "PASS" : "FAIL",
            (unsigned long)errors, (unsigned long)interrupt_count,
            (unsigned long)function_hits, (unsigned long)operation_hits);
}

void zcmp_interrupt_run(void)
{
    if (xPortGetCoreID() != 0)
    {
        ESP_LOGE(TAG, "timer interrupt test requires core0");
        return;
    }
    uint8_t *code = heap_caps_aligned_alloc(PROBE_CODE_ALIGNMENT,
            PROBE_CODE_SIZE, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!code)
    {
        ESP_LOGE(TAG, "executable RAM allocation failed");
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = timer_callback,
        .dispatch_method = ESP_TIMER_ISR,
        .name = "zcmp_irq",
    };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) != ESP_OK)
    {
        ESP_LOGE(TAG, "timer creation failed");
        heap_caps_free(code);
        return;
    }
    for (unsigned ram = 0; ram < 2; ram++)
    {
        run_case(timer, zcmp_irq_ret_start, zcmp_irq_ret_end,
                zcmp_irq_ret_op, code, ram, false, "popret {ra}");
        run_case(timer, zcmp_irq_retz_start, zcmp_irq_retz_end,
                zcmp_irq_retz_op, code, ram, true, "popretz {ra}");
        run_case(timer, zcmp_irq_ret_full_start, zcmp_irq_ret_full_end,
                zcmp_irq_ret_full_op, code, ram, false, "popret {ra,s0-s11}");
        run_case(timer, zcmp_irq_retz_full_start, zcmp_irq_retz_full_end,
                zcmp_irq_retz_full_op, code, ram, true, "popretz {ra,s0-s11}");
    }
    esp_timer_delete(timer);
    heap_caps_free(code);
}
