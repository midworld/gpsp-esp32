/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "esp_log.h"
#include "hcd.h"

/* The diagnostic build uses the installed USB component's private types. */
extern hcd_port_event_t __real_hcd_port_handle_event(hcd_port_handle_t port);

hcd_port_event_t __wrap_hcd_port_handle_event(hcd_port_handle_t port)
{
    hcd_port_event_t event = __real_hcd_port_handle_event(port);
    const char *name;

    switch (event)
    {
        case HCD_PORT_EVENT_NONE: return event;
        case HCD_PORT_EVENT_CONNECTION: name = "connected"; break;
        case HCD_PORT_EVENT_DISCONNECTION: name = "disconnected"; break;
        case HCD_PORT_EVENT_ERROR: name = "port-error"; break;
        case HCD_PORT_EVENT_OVERCURRENT: name = "overcurrent"; break;
        case HCD_PORT_EVENT_REMOTE_WAKEUP: name = "remote-wakeup"; break;
        default: name = "unknown"; break;
    }
    ESP_LOGI("gpsp_usb_port", "Port event=%s (%d)", name, event);
    return event;
}
