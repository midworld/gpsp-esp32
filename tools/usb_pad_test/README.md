<!-- Copyright (C) 2026 Midworld Kim -->

# USB gamepad probe

- Standalone ESP32-P4 firmware for the P4 Nano USB-A port.
- Prints device descriptors and reads the first alternate-setting-zero
  interface with an interrupt IN endpoint.
- Decodes XInput, Xbox One and Switch Pro button reports. Other reports
  are printed as hex. Report counts are logged every five seconds.
- No ROM or network connection is needed.
- Uses GPL-3.0-or-later; see [COPYING](COPYING).

## Build and use

- Use ESP-IDF v6.1. From the repository root:

```sh
cd tools/usb_pad_test
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

- Replace `PORT` with the board's serial port.
- Attach a pad and press buttons to check descriptors and reports.
- This replaces the board's application with the probe firmware.
- Local build files, managed components and `sdkconfig` are ignored by Git.
- Component versions are tracked in `dependencies.lock`.

## Validation

- The latest build was reported passing.
- Transfer error recovery has not been verified on hardware.
