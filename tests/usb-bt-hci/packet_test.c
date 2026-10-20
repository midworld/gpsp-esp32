/* USB HCI framing checks.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_bt_packet.h"

static unsigned calls;
static uint8_t received[GPSP_HCI_ACL_MAX + 1];
static unsigned received_size;

static void receive(uint8_t *packet, unsigned size, void *arg)
{
    (void)arg;
    assert(size <= sizeof(received));
    memcpy(received, packet, size);
    received_size = size;
    calls++;
}

static void framing(unsigned type)
{
    uint8_t input[GPSP_HCI_ACL_MAX];
    unsigned sizes[] = {0, 1, 15, 16, 63, 64, 255, 1024};
    unsigned header = type == 4 ? 2 : 4;

    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++)
    {
        unsigned size = sizes[n];
        if (type == 4 && size > 255)
            continue;
        memset(input, 0xa5, sizeof(input));
        input[header - 2] = size & 255;
        input[header - 1] = size >> 8;
        if (type == 4)
        {
            input[0] = 0x0e;
            input[1] = size;
        }
        for (unsigned chunk = 1; chunk <= size + header; chunk++)
        {
            struct gpsp_hci_packet packet = {0};
            unsigned before = calls;
            for (unsigned offset = 0; offset < size + header; offset += chunk)
            {
                unsigned count = size + header - offset;
                if (count > chunk)
                    count = chunk;
                assert(gpsp_hci_receive(&packet, type, input + offset, count,
                        false, receive, NULL));
            }
            assert(calls == before + 1 && received_size == size + header + 1);
            assert(received[0] == type && !memcmp(received + 1, input, size + header));
            assert(!packet.used && !packet.expected);
        }
    }
}

static void barrot(void)
{
    struct gpsp_hci_packet packet = {0};
    const uint8_t data[] = {0x0e, 4, 1, 3, 12, 0, 0x0e, 4, 1, 3, 12, 0, 0xff};
    unsigned before = calls;

    assert(gpsp_hci_receive(&packet, 4, data, sizeof(data), true, receive, NULL));
    assert(calls == before + 2 && !packet.used && !packet.expected);
    assert(gpsp_hci_receive(&packet, 4, data, 6, true, receive, NULL));
    assert(calls == before + 3);
    uint8_t boundary[16] = {0x0e, 14};
    const uint8_t extra = 0xff;
    assert(gpsp_hci_receive(&packet, 4, boundary, sizeof(boundary), true, receive, NULL));
    assert(gpsp_hci_receive(&packet, 4, &extra, 1, true, receive, NULL));
    assert(calls == before + 4 && !packet.used && !packet.expected);
    assert(gpsp_hci_receive(&packet, 4, data, 6, true, receive, NULL));
    assert(calls == before + 5);
    const uint8_t oversized[] = {1, 0, 0xff, 0xff};
    assert(!gpsp_hci_receive(&packet, 2, oversized, 4, false, receive, NULL));
    assert(!packet.used && !packet.expected);
    assert(!gpsp_hci_receive(&packet, 3, data, 1, false, receive, NULL));
}

int main(void)
{
    framing(2);
    framing(4);
    barrot();
    puts("USB_BT_HCI_PACKET PASS: fragmentation, bounds, Barrot quirk");
    return 0;
}
