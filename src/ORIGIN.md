# Where the engine comes from

`src/` is Euclidier's standalone sequencer engine, and this repo is now its home: edit it here.

- It started as [intelliriffer/EUCLIDIER-CONSOLE](https://github.com/intelliriffer/EUCLIDIER-CONSOLE) by Amit Talwar.
- It was reworked for the Akai Force in `sd88me/force-euclidier` (now archived), then copied here from that repo's
  commit `1268b0e1f0f55e963c754891d94cc0055bee4cb1` (2026-09-25). The sources are unchanged from that commit, which
  was the last one to touch them.
- Files: `euclidier.cpp`, `eqseq.cpp`/`.h`, `bjlund.cpp`/`.h`, `commontypes.h`, `RtMidi.cpp`/`.h`, `RtError.h`,
  `testalgo.cpp`.

In this fork the plugin does not start the engine as a program: `vst/build.sh` copies `euclidier.cpp`, `eqseq.*`,
`bjlund.*` and `commontypes.h` next to `vst/rtmidi_stub/RtMidi.h` and compiles them into the plugin (`main()` renamed
and never called; `RtMidi.cpp` is not built). The offline host test runs the same in-process build.

Changes on top of upstream's copy (sd88me/mpc-vst-euclidier, 2026-10-14):
- `eqseq.cpp` `killHanging()`: drum lanes (mode 2) are stopped with a note-off. It sent a CC instead, so a drum lane's
  last note kept sounding after Stop (found by the host test, 2026-11-06, offline).

Licence: see the note in the top-level `README.md`.
- `euclidier.cpp` global `sendNote()`: new switch `feedbackOut` (default on; the plugin turns it off). That function
  only sends controller feedback (the lane values as CCs on channel 16 after a preset load, a randomise or an edit).
  In-process it went out of the plugin's MIDI port into the driven instrument, which took the CCs as sound changes:
  on the Force the level dropped with every RANDOMISE ALL in Euclidier FX (user report 2026-11-08, device).
