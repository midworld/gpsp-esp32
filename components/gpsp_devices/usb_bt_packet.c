/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "usb_bt_packet.h"
#include <string.h>

bool gpsp_hci_receive(struct gpsp_hci_packet *packet, unsigned type,
        const uint8_t *data, size_t size, bool barrot,
        gpsp_hci_receive_fn receive, void *arg)
{
    unsigned header = type == 4 ? 2 : 4;
    unsigned limit = type == 4 ? GPSP_HCI_EVENT_MAX : GPSP_HCI_ACL_MAX;

    if (type != 2 && type != 4)
        return false;
    if (packet->barrot_tail && size)
    {
        packet->barrot_tail = false;
        /* An appended byte can occupy a separate USB interrupt packet. */
        if (barrot && type == 4 && size == 1)
            return true;
    }
    packet->data[0] = type;
    while (size)
    {
        unsigned target = packet->expected ? packet->expected : header;
        unsigned count = target - packet->used;

        if (count > size)
            count = size;
        memcpy(packet->data + 1 + packet->used, data, count);
        packet->used += count;
        data += count;
        size -= count;
        if (!packet->expected && packet->used == header)
        {
            unsigned payload = type == 4 ? packet->data[2] :
                    packet->data[3] | (packet->data[4] << 8);

            packet->expected = header + payload;
            if (packet->expected > limit)
            {
                packet->used = packet->expected = 0;
                return false;
            }
        }
        if (packet->expected && packet->used == packet->expected)
        {
            receive(packet->data, packet->used + 1, arg);
            packet->used = packet->expected = 0;
            /* Some Barrot firmware appends one byte to a complete event. */
            if (barrot && type == 4 && size == 1)
                return true;
            packet->barrot_tail = barrot && type == 4 && !size;
        }
    }
    return true;
}
