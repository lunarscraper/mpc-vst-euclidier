# Vendored: Euclidier standalone engine

Source: `sd88me/force-euclidier` (fork of `intelliriffer/EUCLIDIER-CONSOLE`), repo root:
`euclidier.cpp`, `eqseq.cpp`/`.h`, `bjlund.cpp`/`.h`, `commontypes.h`, `RtMidi.cpp`/`.h`, `RtError.h`,
`testalgo.cpp`.

Vendored at commit: `1268b0e1f0f55e963c754891d94cc0055bee4cb1` (2026-09-25).
License: MIT (same author, same license as this repo — see `LICENSE`).

The VST port (`vst/`) compiles this engine IN-PROCESS: `vst/build.sh` copies `euclidier.cpp`,
`eqseq.*`, `bjlund.*` and `commontypes.h` into `vst/build/eng/` next to `vst/rtmidi_stub/RtMidi.h`, so
the engine's `#include "RtMidi.h"` resolves to that stub (no real ports, `RtMidi.cpp` isn't built), and
`vst/euclidier_vst.cpp` `#include`s `euclidier.cpp` with its `main()` renamed, calling the engine's own
`handleMidi()`, `processQ()`, `SQ[i].clock()` and control-socket `ctrlGet()`/`ctrlSet()` directly.
Not modified from the source commit above; re-vendor by diffing against force-euclidier's repo root at a
newer commit and copying the same files over. (Earlier versions of the port spawned the deployed
standalone binary from a hard-coded /media/662522/AddOns path instead.)
