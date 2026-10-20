# hid-report-parser

- Source: <https://github.com/pasztorpisti/hid-report-parser>
- Commit: `2ec9b054decd44a7560d1554e56593e18b4df29b`
- License: MIT, see [LICENSE.txt](LICENSE.txt).
  Copyright (c) 2022 Istvan Pasztor.
- Files: `src/hid_report_parser.cpp` and `src/hid_report_parser.h`,
  unmodified.

## Use

- `hid_gamepad.cc` uses only `hid::DescriptorParser` with its own bounded
  event handler. It does not use the input report parsers or their
  allocating containers; the linker drops them.
- The build compiles the source with warnings off and includes the header
  as a system header, so the files stay unmodified.
