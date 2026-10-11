<!-- Copyright (C) 2026 Midworld Kim -->

# P4 Nano speaker check

- Board: ESP32-P4-NANO-WIFI6-DB, revision v3.1; ESP-IDF v6.1.
- Output: onboard ES8311 DAC and NS4150B amplifier, one mono speaker.
- Connect an 8-ohm, 2 W speaker to the board speaker header.
- No ROM, SD card, network, or extra component is needed.
- Speaker output is signed 16-bit PCM, selectable as 48,000 Hz (default)
  or native 32,768 Hz.
- GBA supports stereo. The test accepts separate left and right samples,
  mixes them as `(L + R) / 2`, then mirrors mono into both I2S slots.
- gpSP's sound-rate option defaults to 32,768 Hz. If a frontend does not
  supply that option, the core falls back to 65,536 Hz. The test covers
  both source rates separately from the selected I2S output rate.

## Run

```sh
cd tests/p4-nano-audio-check
idf.py build
idf.py -p PORT flash monitor
```

- Use the USB-C programming port for `PORT`. Exit with `Ctrl+]`.
- The test uses the project partition layout and does not flash ROM or
  save data. It replaces the running app. Restore the main app with
  `idf.py -p PORT app-flash` from the project root.
- The default sequence has 29 stages and takes about three minutes.
- It runs once, mutes the DAC, and disables the amplifier. Reset to repeat.
- `menuconfig` > `P4 Nano audio check` > `I2S output sample rate` selects
  the physical I2S rate. The full sequence runs at the selected rate.
- The same menu controls amplitude, DAC volume, continuous test duration,
  stall injection, and repeat mode.
- `Run only the stall and recovery stages` gives an 18-second recovery check.
- Default peak amplitude: 3000; DAC volume: -12 dB.

## Sequence

| Stage | Seconds | Expected sound |
| --- | ---: | --- |
| Silence | 3 | Silence; listen for hiss or hum |
| 440 Hz | 3 | Steady tone |
| 880 Hz | 3 | One octave higher |
| 125 Hz | 2 | Low tone; a small speaker may be quiet |
| 1000 Hz | 2 | Midrange tone |
| 4000 Hz | 2 | High tone |
| Sweep | 6 | Smooth rise from 120 to 8000 Hz |
| Amplitude ramp | 6 | Quiet, louder, then quiet |
| Left input only | 3 | 440 Hz from the left channel |
| Right input only | 3 | 880 Hz from the right channel |
| Both inputs | 4 | 440 and 880 Hz together |
| Opposite phase | 3 | Silence from exact left/right cancellation |
| Pan | 6 | 440 Hz fades out as 880 Hz fades in |
| Low / medium / high volume | 2 each | Three increasing DAC levels |
| DAC mute | 2 | Silence despite nonzero PCM |
| Amplifier off | 2 | Silence with GPIO53 low |
| 44,100 Hz input | 4 | Same two-tone pitch at the selected output rate |
| 32,768 Hz input | 5 | Same pitch; native samples at 32,768 Hz output |
| 65,536 Hz input | 5 | Same pitch after conversion |
| Stereo melody | 8 | Two repeating voices mixed to mono |
| Continuous playback | 30 | Steady 440 Hz without gaps |
| GBA frame supply, 32,768 Hz | 10 | Steady two-tone sound |
| GBA frame supply, 65,536 Hz | 10 | Same sound with larger batches |
| GBA frame jitter | 10 | Steady sound despite small delays |
| Intentional producer stall | 8 | One short gap, then sound returns |
| Recovery | 10 | Clean sound without gaps |
| Final silence | 2 | Silence, then amplifier off |

- Stereo tests check channel preservation in the mono mix. One speaker
  cannot produce spatial stereo.
- Rate conversion uses streaming linear interpolation with integer phase.
  It preserves timing across varying callback sizes. It is a test path,
  not a band-limited production resampler; the 65,536 Hz test uses low tones.
- Tones have 5 ms fades at their edges. Silence is sent as zero PCM.
- GBA supply uses 280,896 cycles per frame at 16,777,216 Hz:
  about 59.7275 frames/s, not 60.
- At 32,768 Hz, batches alternate between 548 and 549 stereo frames.
  At 65,536 Hz, they alternate between 1097 and 1098 frames.
- Jitter moves producer deadlines by -3 to +4 ms.
- Stall injection delays the producer by 120 ms once. A gap is expected
  only in this stage; the following stage must run without shortages.

## Logs

- `AUDIO_PCM_CHECK`: downmix limits, exact conversion counts, GBA batches.
- `AUDIO_CODEC`: I2C probe and key ES8311 register readback.
- `AUDIO_CLOCK`: requested rate and clocks reported by the I2S driver.
  These values are not a measurement of the physical pins.
- `AUDIO_CLOCK_CHECK`: PCNT counts rising edges on the WS, BCLK, and
  MCLK output pads over about 250 ms. Each must be within 0.2% of its
  target. GPIO input reads the output pad; no loopback wire is needed.
  This measures frequency, not signal shape or analog audio quality.
  Values include software gate timing and integer count error.
- `AUDIO_STAGE` and `LISTEN`: stage name, settings, and expected sound.
- `AUDIO_RESULT`: accepted sample count, peak, RMS, queue range,
  producer shortage count, recovery count, and generation/write times.
- `AUDIO_GAP`: sample position and time of each shortage and recovery.
- `AUDIO_SUITE_RESULT`: transport pass/fail totals.
- `PASS` checks software samples and buffer supply. Listen to the speaker
  to confirm output, pitch, noise, distortion, and mute behavior.
- Normal stages require `underrun_blocks=0`. Each shortage substitutes
  about 10 ms of silence. The stall stage requires one recovered gap of
  at most 120 ms plus one block of scheduling tolerance.
- After a shortage, playback waits for four queued blocks before resuming.
  This prevents another short gap between GBA callbacks during recovery.
- Once production finishes, playback drains the remaining blocks and end
  marker without waiting for a full refill.
- The queue holds about 80 ms of mono PCM, with about 60 ms startup
  prefill. I2S DMA holds about 20 ms. Logs give the actual buffer times.
- Queue entries copy the PCM; the producer can reuse its source buffer.
- Fatal I2C, I2S, or queue errors stop output and reset the app.

## Board pins

| Signal | GPIO |
| --- | ---: |
| I2C SDA / SCL | 7 / 8 |
| I2S MCLK / BCLK / WS | 13 / 12 / 10 |
| I2S output to ES8311 | 9 |
| Amplifier enable, active high | 53 |

- ES8311 uses 7-bit I2C address `0x18`.
- I2S: master, Philips, 16-bit stereo slots; ES8311 DAC reads the left slot.
- MCLK is 256 times the output rate, using APLL:

| I2S rate / WS | BCLK | MCLK |
| ---: | ---: | ---: |
| 48,000 Hz | 1,536,000 Hz | 12,288,000 Hz |
| 32,768 Hz | 1,048,576 Hz | 8,388,608 Hz |

- The microphone input is unused.
- Board reference: [Waveshare documentation](https://docs.waveshare.com/ESP32-P4-NANO-WIFI6-DB).

## Codec implementation

- This is a separate ESP-IDF project; it does not link the gpSP core.
- The output driver uses the ES8311 hardware register specification:
  [Datasheet revision 10](https://files.waveshare.com/wiki/common/ES8311.DS.pdf),
  section 8; [User Guide](https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf),
  sections 8 and 9.
- Both output rates use MCLK / Fs = 256, clock divider = 1, DAC OSR = 64.
- The codec is an I2S slave, with 16-bit left-slot input and line output.
- ADC clocks and bias stay off. Reserved analog bits retain their defaults.
- Startup resets registers, checks chip ID `0x8311`, and charges VMID
  before selecting normal operation. The DAC starts muted.
- Stop disables the amplifier, powers down the codec, and releases buses.

## License

- Test source files and firmware use GPL-3.0-or-later;
  see [COPYING](COPYING).
- ESP-IDF includes Apache-2.0 code and other component licenses;
  see [ESP-IDF licenses](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/COPYRIGHT.html).
- Apache-2.0 is compatible with GPLv3.
  [Compatibility reference](https://www.apache.org/licenses/GPL-compatibility.html).
- When distributing firmware, provide access to the corresponding source
  and build/flash instructions. Keep required license texts and notices.

## Initial 48 kHz board result (2026-10-03)

- P4 revision v3.1; ESP-IDF v6.1; onboard speaker output.
- The datasheet driver passed flash hash, chip ID, and register checks.
- All 29 transport stages passed. Normal stages had no buffer shortages.
- The intentional 120 ms stall produced 100 ms of silence and one recovery.
  The following 10-second recovery stage had no shortages.
- The sequence finished with the DAC muted and the amplifier disabled.
- User reported clear speaker sound on an earlier run of the same firmware.
- Driver-reported clocks: BCLK 1,536,000 Hz; MCLK 12,287,998 Hz.
- Local logs: `build/audio-run-datasheet-20261003-011105.log` and `.json`.

## Native 32,768 Hz board result (2026-10-03)

- P4 revision v3.1; ESP-IDF v6.1; physical I2S output at 32,768 Hz.
- Chip ID, codec registers, and all three clock counters passed.

| Clock | Target Hz | PCNT estimate Hz |
| --- | ---: | ---: |
| WS | 32,768 | 32,771.0 |
| BCLK | 1,048,576 | 1,048,579.7 |
| MCLK | 8,388,608 | 8,388,649.0 |

- All 29 transport stages passed; normal stages had no shortages.
- The 32,768 Hz source stage produced 163,840 frames over five seconds
  with the same input and output rate.
- A 120 ms producer stall caused one 129 ms silence gap and one recovery.
  The following 10-second stage had no shortages.
- Recovery now waits for four queued blocks.
- User reported clear sound and normal pitch on an earlier 32,768 Hz run
  with the same codec and PCM implementation.
- The sequence finished with the DAC muted and the amplifier disabled.
- Final native log:
  `build/native32768/audio-run-native-32768-20261003-024424.log`, with matching `.json`.
- The 48 kHz full sequence passed before the final refill adjustment.
  The final 48 kHz stall/recovery check passed both stages with one
  120 ms gap: `build/recovery48000/audio-run-native-48000-20261003-024342.log`.
- These board runs preceded the end-of-stage tail fix. Host checks at both
  output rates verified tail drainage and the four-block refill rule.

## Host verification

```sh
mkdir -p build
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined main/audio_test_pcm.c test_audio_pcm.c \
  -lm -o build/test_audio_pcm
./build/test_audio_pcm

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -DAUDIO_TEST_RATE=32768u -fsanitize=address,undefined \
  main/audio_test_pcm.c test_audio_pcm.c -lm -o build/test_audio_pcm_32768
./build/test_audio_pcm_32768
```

- Checks conversion rates, signed interpolation, changing callback sizes,
  left/right tone pitch, opposite-phase cancellation, and GBA frame counts.
- Reviewed source passed the ESP32-P4 build and host checks.
  Physical board results are listed above.
