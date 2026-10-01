# firmware/

Sketch source for the Acid Drip S3. CI compiles it — see `../.github/workflows/build.yml`.

The folder name must match the main `.ino` inside it, so `AcidBox/` contains
`AcidBox.ino` as its primary sketch. Arduino concatenates the other `.ino`
files in that folder into one translation unit and auto-generates prototypes.

## `AcidBox/`

Vendored from [copych/AcidBox](https://github.com/copych/AcidBox), branch
`S3-regular`, commit `931753f` ("MipMap oscillator (antialiasing)"). MIT, (c)
2022 copych — see `../THIRD-PARTY-NOTICES.md`. This is the DSP layer; it is
meant to survive the fusion with the sequencer largely intact.

`data/`, `media/`, `hardware/` and `_includes/` from upstream are not copied
here:

| Upstream path | Why not |
|---|---|
| `data/` | 2.5 MB of drum samples. Samples rather than source, with provenance the MIT grant does not clearly cover. Fetch separately — see below. |
| `media/`, `hardware/` | Build assets and board documentation, ~31 MB. |
| `_includes/` | One stray `youtube.html`. |

### Fetching `data/`

The sketch falls back to the embedded `samples.h` / `DEFAULT_DRUMKIT` if the
LittleFS partition is missing, so `data/` is not needed to compile. It *is*
needed to get the real kits on the device:

```sh
cp -r ../AcidBox/data firmware/AcidBox/data
```

The upstream clone at the repo root already has it.

The partition scheme must leave room for it: `PartitionScheme=noota_3g` gives
1 MB of app space and 3 MB of SPIFFS, which the 2.5 MB fits into.

## Board settings

```
esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=noota_3g,FlashSize=16M
```

`PSRAM=opi` is required, not optional. `config.h` sets `PRELOAD_ALL`, whose
`PSRAM_SAMPLER_CACHE` is 3 MB — QSPI PSRAM would be too small.

`PartitionScheme=noota_3g` is what upstream's README calls "No OTA (1MB APP /
3MB SPIFFS)". Note that core 2.x spelled that `min_spiffs`; in core 3.x
`min_spiffs` means "Minimal SPIFFS, 1.9MB APP with OTA / 128KB SPIFFS" and is
the wrong partition. `noota_3g` is verified against `boards.txt` at tag
`3.3.12`.

`FlashSize=16M` is a guess — adjust it if your board differs.

### Deliberately not set

`USBMode` and `CDCOnBoot` are left at their Arduino defaults. `config.h`
defines `BOARD_HAS_UART_CHIP` and routes debug output to the hardware UART, so
neither option is needed for `Serial` to work. The interaction between the
default TinyUSB USB mode and `MIDIUSB_ESP32.h` is untested here, so it is left
as an item to confirm on hardware rather than pinned on a guess.

## Changes from upstream

All M0 changes are marked `M0:` in a comment.

1. **`config.h` — `DEBUG_ON` commented out.** The author's own note says
   debugging "eats ticks initially belonging to real-time tasks, so sound
   output will be spoild in most cases, turn it off for production".
2. **`config.h` — the `#undef DEBUG_ON` guard rewritten.** It read:

   ```c
   #ifdef MIDI_VIA_SERIAL || MIDI_USB_DEVICE
     #undef DEBUG_ON
   #endif
   ```

   `#ifdef` takes exactly one identifier. GCC discards the rest with "extra
   tokens at end of #ifdef directive", so this only ever tested
   `MIDI_VIA_SERIAL` — which is disabled by default. `DEBUG_ON` therefore
   survived even with `MIDI_USB_DEVICE` enabled, defeating the guard the
   author added for it. Rewritten in the same style as the working guard at
   `AcidBox.ino:36`:

   ```c
   #if defined(MIDI_VIA_SERIAL) || defined(MIDI_USB_DEVICE)
     #undef DEBUG_ON
   #endif
   ```

3. **`AcidBox.ino` — audio task priority 1 to 5.** Both synth tasks ran at
   priority 1, the same as Arduino's `loopTask`, which here is not idle: it
   calls `regular_checks()`. Later milestones add TFT redraws and a sequencer
   into `loop()`, so the headroom is taken now rather than after the first
   dropout.

`JUKEBOX` is still enabled, and that is deliberate for the baseline. `M2` turns
it off, once the sequencer takes over pattern generation. Turning it off is not
a build necessity — `AcidBanger.ino`'s `setup()` and `loop()` are inside block
comments, so the file defines no symbols of its own either way.

## Building

Push to `main`, or run the **build** workflow manually. It installs arduino-cli
and the ESP32 core, then compiles. Logs report warning count, warnings
originating in this sketch, and final sketch size. The `.bin` is uploaded as
the `AcidDripS3-firmware` artifact.

Two known unknowns that only a build on hardware will settle:

- whether `CONFIG_TINYUSB_MIDI_ENABLED` is on in the prebuilt sdkconfig
  (`MIDIUSB_ESP32.h` compiles to nothing without it, and
  `USBMIDI_CREATE_INSTANCE` then fails). The workflow greps the sdkconfig and
  prints the result, so this will not be a mystery.
- whether the app fits in the 1 MB `noota_3g` app partition, given the size of
  `samples.h` and the MipMap oscillator tables.

## Dependency

`MIDI Library@4.2.0` — [Forty Seven Effects'
arduino_midi_library](https://github.com/FortySevenEffects/arduino_midi_library).
The Library Manager name is `MIDI Library`, not the repo name. Everything else
`AcidBox` uses is either bundled with the core (`LittleFS.h`, `Wire.h`,
`ESP_I2S.h`, `driver/i2s.h`) or vendored in `AcidBox/src/`.