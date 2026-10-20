# USB Bluetooth checks

- Requires C and C++ compilers for host checks; ESP-IDF 6.1 for firmware checks.
- Host checks: USB/BT PlayStation buttons, report lengths, and CRC.
- HID descriptor host checks: button layouts, hats, signed and relative
  axes, report IDs with separate state, button arrays, keyboards,
  bounded tables, and malformed descriptors.
- The firmware model runs the production HID profile, USB HCI transport,
  and the ESP-IDF Bluedroid host.
- Transport-only checks: [USB Bluetooth HCI](../usb-bt-hci/README.md).
- Pads are not modeled over the air. HID host calls and bonds are recorded,
  and GAP/HID callbacks are injected as the host sends them.
- Checks no runtime allocations without a dongle, attach/connect/detach
  cycles, shutdown, input routing, and heap recovery.
- Checks that input waits for the descriptor after each open, the Sony
  layout on report 0x01 and full reports replacing it, a generic pad with
  two reports and reconnect, a refused keyboard, and failed, oversized, or
  malformed descriptors.
- Checks discovery by Device ID, class, or HID service (not by name),
  connection cancellation,
  a failed open, saved registrations, selective/all key deletion, the
  reconnect setting across host restarts, an adapter change, and that four
  registrations persist while a fifth pad is refused.
- Saved-pad reconnect waits for incoming connections; boot and Auto Reconnect
  do not page the pad.
- Only the selected pad stays connected; other pads are closed and bonds
  that Just Works pairing adds for them are removed.
- Pairing requests: the host completes Just Works itself. Numeric
  comparison and passkey entry are refused. A legacy PIN request from the
  selected pad gets 0000 unless 16 digits are required; other pads are
  refused.
- Checks full-report CRC, a full event queue restarting the host, and a
  profile setup failure stopping it until the next attach.
- Runs the production menu/settings for mute persistence and volume preservation,
  controller selection, deletion confirmation/cancel, and value widths.
- Runs the audio worker with modeled codec/DMA writes: mute, unmute, retry,
  preserved volume, reserved register bits, and continued queue consumption.
- ESP-IDF cannot cancel EP0 transfers directly. If USB control stalls,
  cleanup waits for completion or physical removal; it does not free active buffers.
- The model does not verify radio pairing, encryption, real USB timing,
  or gamepad compatibility. Descriptors in the checks are written for
  them, not captured from pads.

## Host

```sh
mkdir -p output/usb-bt
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I components/gpsp_devices \
  -I components/gpsp/core/libretro/libretro-common/include \
  components/gpsp_devices/playstation_pad.c \
  tests/usb-bt/packet_test.c -o output/usb-bt/check
output/usb-bt/check
c++ -std=c++17 -O1 -w -fsanitize=address,undefined -c \
  components/gpsp_devices/third_party/hid_report_parser/hid_report_parser.cpp \
  -o output/usb-bt/hid_report_parser.o
c++ -std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I components/gpsp_devices \
  -isystem components/gpsp_devices/third_party/hid_report_parser \
  -I components/gpsp/core/libretro/libretro-common/include \
  -c components/gpsp_devices/hid_gamepad.cc -o output/usb-bt/hid_gamepad.o
cc -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I components/gpsp_devices \
  -I components/gpsp/core/libretro/libretro-common/include \
  -c tests/usb-bt/hid_gamepad_test.c -o output/usb-bt/hid_gamepad_test.o
c++ -fsanitize=address,undefined output/usb-bt/hid_report_parser.o \
  output/usb-bt/hid_gamepad.o output/usb-bt/hid_gamepad_test.o \
  -o output/usb-bt/hid-check
output/usb-bt/hid-check
```

## Firmware model

```sh
cd tests/usb-bt
IDF_COMPONENT_MANAGER=0 idf.py -B build-model \
  -D SDKCONFIG=build-model/sdkconfig build merge-bin
esp-emu --chip esp32p4 --firmware build-model/merged-binary.bin \
  --psram-size 32M --timeout 300s --exit-on 'USB_BT_CHECK PASS'
```

- Require `USB_BT_CHECK PASS`; a timeout is a failure.
- A failed assert reboots the model; check that the log has one run.
- Firmware model builds require the repository's managed USB component.
