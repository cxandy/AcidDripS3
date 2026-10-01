# Acid Drip × AcidBox — ESP32-S3

Fusion of two open synth projects into one ESP32-S3 firmware: AcidBox's 303-class
DSP engine driving Acid Drip's 16-step sequencer, pad gestures, and TFT UI.

> **Status: M0 — baseline builds.** The plan is complete and committed, the
> AcidBox DSP layer is vendored at `firmware/AcidBox/`, and GitHub Actions
> compiles it green (806 KB binary). On-hardware verification — flashing, audio,
> core-0 headroom — is still outstanding, because no ESP32-S3 is currently
> attached. M1 onward is unstarted.

## The idea in one paragraph

AcidBox (ESP32-S3) and Acid Drip (RP2040) already speak the same language: MIDI
events. AcidBox exposes only event entry points — `on_midi_noteON`, `ParseCC`,
`Sampler::NoteOn`. Acid Drip's sequencer is, in effect, a MIDI generator. So the
integration is not "port Mozzi to ESP32-S3" but **call AcidBox's functions
in-process instead of routing through a MIDI cable**.

That keeps everything worth keeping:

| | AcidBox (S3) | Acid Drip (RP2040) |
|---|---|---|
| Sample rate | 44.1 kHz | 16.384 kHz |
| Depth | 16-bit I2S | 8-bit PWM |
| Oscillator | MipMap anti-aliased | naive |
| Drum samples | 84, PSRAM-resident | 8, embedded |
| Effects | reverb, delay, drum-sidechain comp | none |
| 303 voices | two, filter-tuning compensated | one |
| UI | — | 16 pads + 320×240 TFT + gestures |

## Plan

See **[ESP32S3_FUSION_IMPLEMENTATION.md](ESP32S3_FUSION_IMPLEMENTATION.md)** —
12 sections, covering architecture decisions with per-decision trade-off analysis,
verified source citations, milestones M0–M5 with acceptance criteria, a risk
register, and the V5→AcidBox parameter mapping table.

Roughly 8–12 days of software work.

## Building

Push to `main`, or run the **build** workflow. It installs arduino-cli and the
ESP32 core, compiles the sketch, and uploads the `.bin` as an artifact. The
toolchain is ~1.5 GB and cached between runs.

Target is `esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=noota_3g,FlashSize=16M`.
See **[firmware/README.md](firmware/README.md)** for why each of those is the
value it is, and for the three changes made to the vendored source.

## Upstream projects

Neither is vendored here. See [UPSTREAM.md](UPSTREAM.md) for pinned commits and
fetch instructions.

- [`copych/AcidBox`](https://github.com/copych/AcidBox) — **MIT**, branch `S3-regular` @ `931753f`
- [`lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth`](https://github.com/lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth) — **no license**, branch `main` @ `f88e64d`

## Getting the source

```bash
# Audio engine (DSP, effects, sampler, MIDI routing)
git clone --depth 1 --branch S3-regular https://github.com/copych/AcidBox.git

# Sequencer / UI reference
git clone https://github.com/lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth.git
```

Both directories are gitignored. Work against them in place.

## License

Fusion source: not yet licensed.

AcidBox-derived files remain under MIT — see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
