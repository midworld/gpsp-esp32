/* Exercise the production HCI transport and host without a controller profile.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_gap_bt_api.h"
#include "nvs_flash.h"
#include "usb_bt.h"
#include "usb_model.h"

/* Reset, version, commands, two feature pages, buffer size, address. */
#define PROBE_COMMANDS 7

static struct gpsp_usb_bt_info wait_state(enum gpsp_usb_bt_state state,
        int64_t timeout_us)
{
    struct gpsp_usb_bt_info info;
    int64_t end = esp_timer_get_time() + timeout_us;

    do
    {
        gpsp_usb_bt_get_info(&info);
        if (info.state == state)
            return info;
        assert(esp_timer_get_time() < end);
        vTaskDelay(1);
    } while (true);
}

static void wait_stopped(enum gpsp_usb_bt_state state, int64_t timeout_us)
{
    struct gpsp_usb_bt_info info;
    int64_t end = esp_timer_get_time() + timeout_us;

    wait_state(state, timeout_us);
    do
    {
        gpsp_usb_bt_get_info(&info);
        assert(esp_timer_get_time() < end);
        vTaskDelay(1);
    } while (info.active);
    vTaskDelay(20);
    assert(!usb_model_allocations());
}

static void start(void)
{
    gpsp_usb_bt_attach(1);
    gpsp_usb_bt_poll();
}

/* The host rewrites its config in NVS, whose RAM index follows flash
   page use. Measure without it. */
static size_t heap_free(void)
{
    assert(nvs_flash_deinit() == ESP_OK);
    size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    assert(nvs_flash_init() == ESP_OK);
    return free;
}

static void heap_check(size_t steady, const char *name)
{
    size_t free = heap_free();

    printf("USB_BT_HCI_CHECK %s free=%u\n", name, (unsigned)free);
    assert(free == steady);
}

static void checks(void *arg)
{
    (void)arg;
    assert(nvs_flash_init() == ESP_OK);
    vTaskDelay(20);
    size_t before = heap_free();

    for (unsigned i = 0; i < 20; i++)
        gpsp_usb_bt_poll();
    wait_stopped(GPSP_USB_BT_ABSENT, 1000000);
    assert(heap_free() == before);
    puts("USB_BT_HCI_CHECK absent: no runtime allocations");
    gpsp_usb_bt_attach(1);
    gpsp_usb_bt_shutdown();
    gpsp_usb_bt_poll();
    wait_stopped(GPSP_USB_BT_ABSENT, 1000000);
    assert(heap_free() == before);
    puts("USB_BT_HCI_CHECK pending attach cancellation: pass");

    /* A reply to Host Number Of Completed Packets with status 0x07 and no
       command credits must not stop later commands. */
    {
        const uint8_t reply[] = {0, 0x35, 0x0c, 0x07};
        uint8_t params[4];

        usb_model_present(true);
        usb_model_clear_commands();
        start();
        wait_state(GPSP_USB_BT_READY, 5000000);
        /* The model is a Barrot adapter: flow control stays off. */
        assert(usb_model_command(0x0c31, params, 1) && !params[0]);
        usb_model_event(0x0e, reply, sizeof(reply));
        vTaskDelay(50);
        usb_model_clear_commands();
        assert(esp_bt_gap_set_device_name("check") == ESP_OK);
        int64_t end = esp_timer_get_time() + 2000000;
        while (!usb_model_command(0x0c13, params, 1))
        {
            assert(esp_timer_get_time() < end);
            vTaskDelay(1);
        }
        gpsp_usb_bt_shutdown();
        wait_stopped(GPSP_USB_BT_ABSENT, 3000000);
        puts("USB_BT_HCI_CHECK completed packets reply ignored: pass");
    }

    size_t steady = 0;
    for (unsigned cycle = 0; cycle < 4; cycle++)
    {
        usb_model_present(true);
        int64_t begin = esp_timer_get_time();
        size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t external = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        start();
        struct gpsp_usb_bt_info info = wait_state(GPSP_USB_BT_READY, 5000000);
        assert(info.received > PROBE_COMMANDS && usb_model_commands() > PROBE_COMMANDS &&
                usb_model_allocations() == 4 && !usb_model_connection_attempts());
        printf("USB_BT_HCI_CHECK ready_ms=%u internal_used=%u psram_used=%u\n",
                (unsigned)((esp_timer_get_time() - begin) / 1000),
                (unsigned)(internal - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                (unsigned)(external - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        if (cycle & 1)
            gpsp_usb_bt_shutdown();
        else
            usb_model_present(false);
        wait_stopped(GPSP_USB_BT_ABSENT, 3000000);
        size_t free = heap_free();
        printf("USB_BT_HCI_CHECK cycle=%u free=%u\n", cycle, (unsigned)free);
        if (cycle)
            assert(free == steady);
        steady = free;
    }
    puts("USB_BT_HCI_CHECK host start/removal/shutdown cycles: pass");

    usb_model_stall(true);
    usb_model_present(true);
    start();
    vTaskDelay(20);
    usb_model_present(false);
    wait_stopped(GPSP_USB_BT_ABSENT, 1000000);
    heap_check(steady, "probe_removal");
    puts("USB_BT_HCI_CHECK removal before host start: pass");

    usb_model_present(true);
    start();
    struct gpsp_usb_bt_info info = wait_state(GPSP_USB_BT_ERROR, 12000000);
    assert(info.error == ESP_ERR_TIMEOUT);
    wait_stopped(GPSP_USB_BT_ERROR, 1000000);
    heap_check(steady, "probe_timeout");
    puts("USB_BT_HCI_CHECK unanswered controller: pass");
    usb_model_stall(false);

    usb_model_no_classic(true);
    start();
    info = wait_state(GPSP_USB_BT_ERROR, 5000000);
    assert(info.error == ESP_ERR_NOT_SUPPORTED);
    wait_stopped(GPSP_USB_BT_ERROR, 1000000);
    usb_model_no_classic(false);
    usb_model_acl_size(4096);
    start();
    info = wait_state(GPSP_USB_BT_ERROR, 5000000);
    assert(info.error == ESP_ERR_NOT_SUPPORTED);
    wait_stopped(GPSP_USB_BT_ERROR, 1000000);
    usb_model_acl_size(1021);
    heap_check(steady, "unsupported");
    puts("USB_BT_HCI_CHECK BLE-only and oversized ACL adapters: pass");

    unsigned base;

    /* Refused resets are retried; a failed probe is an init failure, not a
       missing feature. */
    usb_model_refuse_resets(2);
    start();
    info = wait_state(GPSP_USB_BT_READY, 5000000);
    gpsp_usb_bt_shutdown();
    wait_stopped(GPSP_USB_BT_ABSENT, 3000000);
    usb_model_refuse_resets(3);
    start();
    info = wait_state(GPSP_USB_BT_ERROR, 5000000);
    assert(info.error == ESP_FAIL);
    wait_stopped(GPSP_USB_BT_ERROR, 1000000);
    base = usb_model_commands();
    usb_model_status_after(base + 2);
    start();
    info = wait_state(GPSP_USB_BT_ERROR, 5000000);
    assert(info.error == ESP_FAIL);
    wait_stopped(GPSP_USB_BT_ERROR, 1000000);
    usb_model_status_after(0);
    /* Removal or shutdown while waiting to retry sends no further reset. */
    for (unsigned stop = 0; stop < 2; stop++)
    {
        base = usb_model_commands();
        usb_model_refuse_resets(3);
        start();
        while (usb_model_commands() == base)
            vTaskDelay(1);
        if (stop)
            gpsp_usb_bt_shutdown();
        else
            usb_model_present(false);
        int64_t end = esp_timer_get_time() + 3000000;
        do
        {
            gpsp_usb_bt_get_info(&info);
            assert(esp_timer_get_time() < end);
            vTaskDelay(1);
        } while (info.active || info.state == GPSP_USB_BT_STARTING);
        vTaskDelay(20);
        assert(!usb_model_allocations() && usb_model_commands() == base + 1);
        usb_model_refuse_resets(0);
        usb_model_present(true);
    }
    heap_check(steady, "refused_reset");
    puts("USB_BT_HCI_CHECK refused resets and failed probes: pass");

    /* Faults at each command up to ready leave Bluedroid waiting inside
       enable or the profile-less start. */
    base = usb_model_commands();
    start();
    wait_state(GPSP_USB_BT_READY, 5000000);
    unsigned commands = usb_model_commands() - base - PROBE_COMMANDS;
    gpsp_usb_bt_shutdown();
    wait_stopped(GPSP_USB_BT_ABSENT, 3000000);
    printf("USB_BT_HCI_CHECK host start commands=%u\n", commands);
    for (unsigned extra = 1; extra <= commands; extra++)
    {
        base = usb_model_commands();
        int64_t begin = esp_timer_get_time();

        usb_model_remove_after(base + PROBE_COMMANDS + extra);
        start();
        wait_stopped(GPSP_USB_BT_ABSENT, 5000000);
        usb_model_remove_after(0);
        printf("USB_BT_HCI_CHECK removal opcode=%04x stop_ms=%u\n",
                usb_model_last_opcode(),
                (unsigned)((esp_timer_get_time() - begin) / 1000));
        heap_check(steady, "enable_removal");
        usb_model_present(true);

        base = usb_model_commands();
        begin = esp_timer_get_time();
        usb_model_stall_after(base + PROBE_COMMANDS + extra);
        start();
        info = wait_state(GPSP_USB_BT_ERROR, 10000000);
        assert(info.error == ESP_ERR_TIMEOUT);
        wait_stopped(GPSP_USB_BT_ERROR, 1000000);
        usb_model_stall_after(0);
        usb_model_stall(false);
        printf("USB_BT_HCI_CHECK stall opcode=%04x stop_ms=%u\n",
                usb_model_last_opcode(),
                (unsigned)((esp_timer_get_time() - begin) / 1000));
        heap_check(steady, "enable_stall");

        /* Failed replies the host checks close the adapter; others leave
           Bluedroid to decide. Either way it must stop cleanly. */
        base = usb_model_commands();
        usb_model_status_after(base + PROBE_COMMANDS + extra);
        start();
        int64_t end = esp_timer_get_time() + 5000000;
        do
        {
            gpsp_usb_bt_get_info(&info);
            assert(esp_timer_get_time() < end);
            vTaskDelay(1);
        } while (info.state == GPSP_USB_BT_STARTING);
        printf("USB_BT_HCI_CHECK failure opcode=%04x state=%d error=%s\n",
                usb_model_last_opcode(), info.state, esp_err_to_name(info.error));
        if (info.state == GPSP_USB_BT_READY)
        {
            gpsp_usb_bt_shutdown();
            wait_stopped(GPSP_USB_BT_ABSENT, 3000000);
        }
        else
            wait_stopped(GPSP_USB_BT_ERROR, 3000000);
        usb_model_status_after(0);
        heap_check(steady, "enable_failure");
    }
    puts("USB_BT_HCI_CHECK removal and silence during host enable: pass");

    start();
    wait_state(GPSP_USB_BT_READY, 5000000);
    usb_model_stall(true);
    gpsp_usb_bt_shutdown();
    wait_stopped(GPSP_USB_BT_ABSENT, 10000000);
    usb_model_stall(false);
    heap_check(steady, "disable_stall");
    puts("USB_BT_HCI_CHECK silent controller during shutdown: pass");

    base = usb_model_commands();
    usb_model_fail_after(base + PROBE_COMMANDS + 3);
    start();
    info = wait_state(GPSP_USB_BT_ERROR, 5000000);
    assert(info.error == ESP_FAIL);
    wait_stopped(GPSP_USB_BT_ERROR, 1000000);
    usb_model_fail_after(0);
    heap_check(steady, "submit_failure");
    puts("USB_BT_HCI_CHECK command submit failure during host enable: pass");

    /* Removal reports the link down before the host disables. */
    for (unsigned cycle = 0; cycle < 2; cycle++)
    {
        start();
        wait_state(GPSP_USB_BT_READY, 5000000);
        assert(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE) == ESP_OK);
        vTaskDelay(20);
        usb_model_connect();
        int64_t end = esp_timer_get_time() + 1000000;
        while (!usb_model_linked())
        {
            assert(esp_timer_get_time() < end);
            vTaskDelay(1);
        }
        vTaskDelay(50);
        if (cycle)
        {
            /* The unanswered disconnect fails the adapter, then the host's
               disable timer finishes. */
            usb_model_stall(true);
            gpsp_usb_bt_shutdown();
            info = wait_state(GPSP_USB_BT_ERROR, 10000000);
            assert(info.error == ESP_ERR_TIMEOUT);
            wait_stopped(GPSP_USB_BT_ERROR, 1000000);
        }
        else
        {
            usb_model_present(false);
            wait_stopped(GPSP_USB_BT_ABSENT, 5000000);
        }
        usb_model_stall(false);
        usb_model_present(true);
        heap_check(steady, "open_link");
    }
    puts("USB_BT_HCI_CHECK open link removal and silent shutdown: pass");

    /* An adapter refused the packet type change with no command credits,
       which stops later commands; the host skips that change. */
    for (int credits = 1; credits >= 0; credits--)
    {
        uint8_t params[4];

        usb_model_refuse_packet_type(credits);
        start();
        wait_state(GPSP_USB_BT_READY, 5000000);
        assert(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE) == ESP_OK);
        vTaskDelay(20);
        usb_model_clear_commands();
        usb_model_connect();
        int64_t end = esp_timer_get_time() + 2000000;
        /* Link setup ends with the extended features read. */
        while (!usb_model_command(0x041c, params, 1))
        {
            assert(esp_timer_get_time() < end);
            vTaskDelay(1);
        }
        vTaskDelay(50);
        assert(!usb_model_command(0x040f, params, 1));
        usb_model_clear_commands();
        assert(esp_bt_gap_set_device_name("check") == ESP_OK);
        end = esp_timer_get_time() + 2000000;
        while (!usb_model_command(0x0c13, params, 1))
        {
            assert(esp_timer_get_time() < end);
            vTaskDelay(1);
        }
        usb_model_present(false);
        wait_stopped(GPSP_USB_BT_ABSENT, 5000000);
        usb_model_present(true);
        heap_check(steady, "packet_type");
    }
    usb_model_refuse_packet_type(-1);
    puts("USB_BT_HCI_CHECK link setup skips the packet type change: pass");

    usb_model_stall_control(true);
    start();
    vTaskDelay(20);
    usb_model_present(false);
    wait_stopped(GPSP_USB_BT_ABSENT, 1000000);
    usb_model_stall_control(false);
    heap_check(steady, "control_removal");
    puts("USB_BT_HCI_CHECK removal during control transfer: pass");
    puts("USB_BT_HCI_CHECK PASS");
    vTaskDelete(NULL);
}

void app_main(void)
{
    assert(xTaskCreatePinnedToCore(checks, "hci_check", 4096, NULL, 4, NULL, 1) == pdPASS);
}
