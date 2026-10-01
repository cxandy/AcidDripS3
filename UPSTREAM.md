# UPSTREAM

This repository contains **only** the fusion project's own planning documents and
source. Neither upstream project is vendored here — both are fetched separately
and referenced by pinned commit.

## Why they are not committed here

| Upstream | License | Size on disk | Handling |
|---|---|---|---|
| [`copych/AcidBox`](https://github.com/copych/AcidBox) | **MIT** (c) 2022 copych | 67 MB | Fetch locally, do not redistribute |
| [`lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth`](https://github.com/lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth) | **None — all rights reserved** | 43 MB | Fetch locally, do not redistribute |

AcidBox is permissive, but Acid_Drip ships **no LICENSE file at all**. Under default
copyright that means no grant to copy, modify, or redistribute. Committing its source
into a third repository — even a private one — is a different act from the personal
use its author invited. Both are therefore gitignored here and fetched on demand.

The AcidBox MIT notice is reproduced in `THIRD-PARTY-NOTICES.md` because the fusion
firmware derives from AcidBox source files. The filename is deliberately not
`LICENSE*`: GitHub's license detector would otherwise label this whole
repository MIT, which is not true of the fusion source itself.

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

## What this repository does contain

- `ESP32S3_FUSION_IMPLEMENTATION.md` — the design and implementation plan
- `UPSTREAM.md` — this file
- Fusion source, as it is written (M1 onward)

See `ESP32S3_FUSION_IMPLEMENTATION.md` §10 for the planned file layout.
