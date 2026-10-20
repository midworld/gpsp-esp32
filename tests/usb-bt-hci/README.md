# USB Bluetooth HCI checks

- Runs the production USB HCI transport and the ESP-IDF Bluedroid host
  without a controller profile.
- Checks idle allocation, start and release cycles, removal, shutdown,
  and heap recovery.
- Checks the controller probe before the host starts: no reply,
  a BLE-only adapter, and an ACL length above 1024 bytes.
- A refused reset is retried twice, 200 ms apart; a failed probe command
  is an initialization failure, not a missing feature.
- Removes, silences, or fails each command up to host ready.
  The transport then fails pending commands and reports links down,
  so the host finishes its own disable and deinit. Start-up reads the
  host asserts on reuse the probed replies; a differing reply closes
  the adapter.
- Checks a command submit failure, an open link at removal, and a silent
  adapter during shutdown.
- Uses one interrupt packet per request and fragmented HCI events, including
  the Barrot trailing byte in a separate USB packet.
- Heap checks release NVS first; its RAM index follows flash page use.
- The model does not verify radio operation or real USB timing.

## Host

```sh
mkdir -p output/usb-bt-hci
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I components/gpsp_devices components/gpsp_devices/usb_bt_packet.c \
  tests/usb-bt-hci/packet_test.c -o output/usb-bt-hci/check
output/usb-bt-hci/check
```

## Firmware model

```sh
cd tests/usb-bt-hci
IDF_COMPONENT_MANAGER=0 idf.py -B build-model \
  -D SDKCONFIG=build-model/sdkconfig build merge-bin
esp-emu --chip esp32p4 --firmware build-model/merged-binary.bin \
  --psram-size 32M --timeout 1500s --exit-on 'USB_BT_HCI_CHECK PASS'
```

- Requires ESP-IDF 6.1 and the repository's managed USB component.
- Require `USB_BT_HCI_CHECK PASS`; a timeout is a failure.
- A failed assert reboots the model; check that the log has one run.
