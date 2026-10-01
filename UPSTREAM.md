# UPSTREAM

Two upstream projects feed this one.

| Upstream | License | Handling |
|---|---|---|
| [`copych/AcidBox`](https://github.com/copych/AcidBox) | **MIT**, (c) 2022 copych | **Vendored** at `firmware/AcidBox/`, then modified. Clone at the repo root is a pristine reference copy. |
| [`lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth`](https://github.com/lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth) | **None — all rights reserved** | Gitignored reference clone. Never committed. |

AcidBox is vendored because MIT permits it, and because the build has to work
without network access to a 67 MB clone. Its source is ~559 KB; the rest of the
clone — `data/` drum samples, `media/`, `hardware/` — is not committed, for
reasons in `firmware/README.md`.

Acid_Drip ships **no LICENSE file at all**. Under default copyright that means
no grant to copy, modify, or redistribute. Committing its source into a third
repository — even a private one — is a different act from the personal use its
author invited. So its code is read from the local clone and not committed.
That constraint shapes the work: the sequencer and UI have to be written here
rather than lifted wholesale, even though the plan is to port its behaviour.

The AcidBox MIT notice is reproduced in `THIRD-PARTY-NOTICES.md`. That filename
is deliberate — it is not `LICENSE*` because GitHub's license detector would
otherwise label this whole repository MIT, which is not true of the fusion code
itself.

## Pinned versions

### AcidBox — branch `S3-regular`, commit `931753f`

```
git clone --depth 1 --branch S3-regular https://github.com/copych/AcidBox.git
```

- `VERSION "v.1.5.0 S3"` (`config.h`)
- Last commit 2025-08-07 — *"MipMap oscillator (antialiasing)"*

`S3-regular` is chosen over `S3-only` deliberately:

| | `S3-regular` | `S3-only` |
|---|---|---|
| Version | v.1.5.0 | v.1.4.0 |
| Last commit | 2025-08-07 | 2025-03-11 |
| `MIDI_USB_DEVICE` | enabled | — |
| `PRELOAD_ALL` (OPI PSRAM) | enabled, noted as recommended | — |

The project's own README recommends `S3-regular` for ESP32-S3.

### Acid_Drip — branch `main`, commit `f88e64d`

```
git clone https://github.com/lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth.git
```

Reference implementation is `src/Acid_Drip_Drum_Acid_Drift_V5` (~9,578 lines
across four files). `src/Acid_Drip_Drum_Acid_V4` is the earlier, separate sketch.

## What this repository contains

- `firmware/AcidBox/` — the DSP layer, vendored from AcidBox and modified
- `ESP32S3_FUSION_IMPLEMENTATION.md` — the design and implementation plan
- `UPSTREAM.md` — this file
- `THIRD-PARTY-NOTICES.md` — MIT notice for the vendored AcidBox source

See `ESP32S3_FUSION_IMPLEMENTATION.md` §10 for the planned file layout, and
`firmware/README.md` for how to build.