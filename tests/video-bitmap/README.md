<!-- Copyright (C) 2026 Midworld Kim -->

# Bitmap renderer regression

- Run `make test` with a C++ compiler on a little-endian host.
- Run `make test-sanitize` with AddressSanitizer and UndefinedBehaviorSanitizer.
- The harness includes `video.cc` and calls its bitmap BLIT renderer and mode 4
  backdrop helper directly.
- Compare modes 3, 4 and 5 against a pixel-by-pixel coordinate reference.
- Cover both pages, mosaic, empty and partial segments, extreme and fractional
  coordinates, and four destination alignments.
- Mode 4 also checks indexed and stacked destinations and transparent pixels.
- The mode 4 backdrop helper must match scalar palette lookup, including index
  zero and pixels outside the bitmap. Aligned groups and scalar tails are checked.
- Sentinels outside the requested segment must remain unchanged.
