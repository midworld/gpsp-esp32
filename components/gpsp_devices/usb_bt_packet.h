/* USB HCI packet assembly.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_USB_BT_PACKET_H
#define GPSP_USB_BT_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPSP_HCI_ACL_MAX 1028
#define GPSP_HCI_EVENT_MAX 257

struct gpsp_hci_packet
{
    uint8_t data[GPSP_HCI_ACL_MAX + 1];
    unsigned used, expected;
    bool barrot_tail;
};

typedef void (*gpsp_hci_receive_fn)(uint8_t *packet, unsigned size, void *arg);

/* USB has no H4 indicator. Add it only when forwarding a complete packet. */
bool gpsp_hci_receive(struct gpsp_hci_packet *packet, unsigned type,
        const uint8_t *data, size_t size, bool barrot,
        gpsp_hci_receive_fn receive, void *arg);

#endif
