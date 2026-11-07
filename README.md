# Euclidier (lunarscraper fork)

Euclidier as a native plugin for Akai MPC OS standalone devices (Force, MPC Live/One/X/Key): an 8-lane Euclidean
MIDI sequencer with its own MPC screen skin and Q-Links. This is a fork of
[sd88me/mpc-vst-euclidier](https://github.com/sd88me/mpc-vst-euclidier): it has upstream's current screen design, and
keeps this fork's own engine hosting (engine inside the plugin), the insert-effect variant **Euclidier FX** and
control by MIDI CC. It is built with [open-mpc/mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins) (the
wrapper tools moved there from `sd88me/mpc-vst-plugins`).

The engine is based on [intelliriffer/EUCLIDIER-CONSOLE](https://github.com/intelliriffer/EUCLIDIER-CONSOLE) by Amit
Talwar, reworked for the MPC/Force (see [credits](#credits-and-licence)).

## What it does

Eight independent lanes each spread a number of hits (**fill**) as evenly as possible over a number of **steps**, with a
**shift** (rotation), an optional **loop** point, a note (or a drum note), MIDI channel, gate, velocity and humanise, and
a time division. The plugin makes no sound: it plays MIDI into other tracks, and it follows the project transport and
tempo, so edits stay in phase with the beat.

## The screen

Upstream's design (two tabs with live step displays) plus a SETUP tab from this fork. LANES and ALL have
**DRUM MODE ALL** and **RANDOMISE ALL** in the top bar.

| Tab | What is on it |
|---|---|
| **LANES** | The lane you are editing. A lane stepper (◀ LANE 1 ▶) chooses it. Its pattern is drawn as a circle with the play-head, with a RANDOMISE button for that lane in the corner of the panel. Knobs for STEPS, FILL, SHIFT, LOOP, GATE, VELOCITY, HUMANIZE, NOTE and MID CH., a division stepper, a NOTE / DRUM switch and the lane's enable. Q-Links 1-8: steps, fill, shift, loop, gate, velocity, note, MID CH. (HUMANIZE is touch-only). |
| **ALL** | All 8 lanes as rows: an enable lamp, the pattern as a row of cells with the play-head, the lane's FILL as a number, and a RANDOMISE button for that lane. Q-Links 1-8: the eight lanes' **FILL** (fork; upstream puts the lane enables there). |
| **SETUP** | (fork) The engine's preset bank: PRESET (slot 0-127), LOAD, SAVE. **CONTROL CH** for the CC control below. Q-Links: PRESET, CONTROL CH. |

All Q-Links are knobs; no switch sits on a Q-Link.

A new instance starts with every lane in drum mode, lanes 1-4 on and 5-8 off, each of the four with its own Euclidean
pattern: 16 steps with 4 hits (four on the floor), 8 with 3 (tresillo), 12 with 5, and 16 with 5. A saved project
restores its own settings instead. Switching a lane to DRUM sets its note to that lane's own General MIDI drum slot
(lane 1 is 36, lane 2 is 37, and so on).

Pattern displays change size with the lane's step count: the row cells and the circle come in size classes (rows: 8,
16, 32 cells; circle: 8, 12, 16, 24, 32 dots). Counts between classes use the next class up. The 32-step row doesn't
show the play-head.

## Limits

- **32 steps** on the screen and via CC (the engine allows 64): the step displays are built from many small image
  components, and 64 steps made pages too slow to open (upstream).
- **Slow first draw.** A page can take a moment to fill in when the plugin is first inserted (MPC decodes the skin's images).
- **Display only.** Tapping a step does not edit it.
- **One running instance per variant.** The engine lives inside the plugin and keeps its state in globals, so the first
  Euclidier drives it and a second Euclidier stays silent (logged). Euclidier and Euclidier FX are separate plugins, so
  one of each can run at the same time.

## Two variants / Zwei Varianten

| | Ordner | Datei | Typ | Workflow | ALSA-MIDI-Port |
|---|---|---|---|---|---|
| Euclidier | `vst/` | `euclidier.so` | Instrument (belegt einen der 8 Plugin-Slots) | VST release (draft) | `Euclidier` |
| Euclidier FX | `vst-fx/` | `euclidier_fx.so` | Insert-Effekt (belegt keinen Instrumenten-Slot) | VST release FX (draft) | `Euclidier FX` |

Beide entstehen aus derselben `vst/euclidier_vst.cpp` und demselben Skin; `vst-fx/vst.json` setzt nur `"effect": true`.
Die FX-Variante reicht das Audio des Tracks unverändert durch und sendet ihre Noten über den eigenen MIDI-Port. Beide
lassen sich nebeneinander installieren (eigene UID, eigener Name, eigener Skin-Ordner) und teilen sich die Preset-Datei
`euclidier_BANK.bin`.

Euclidier FX einsetzen: als Insert auf den Track des gesteuerten Instruments (oder auf den Master) legen und beim
Ziel-Track als MIDI-Eingang `Euclidier FX` wählen. Euclidier (Instrument): beim Ziel-Track `Euclidier` als MIDI-Eingang.

Diagnose: `/tmp/euclidier_vst.log` schreibt alle 5 s eine Zeile. `playing 1` und steigende `pulses` heißen, dass das
Plugin Transport und Tempo bekommt; `no timeinfo` > 0 heißt, dass der Host keine Zeitinfo liefert; bleiben die Zeilen
ganz aus, hält der Host das Plugin bei Stille an.

## Steuerung per MIDI-CC (beide Varianten)

Das Plugin hat einen zweiten MIDI-Port „Control In" und verbindet ihn selbst mit allen angeschlossenen
Hardware-MIDI-Eingängen (auch später eingesteckten). Es reagiert nur auf CCs des Kanals, der im Tab **SETUP** unter
CONTROL CH eingestellt ist (OFF, 1–16; Voreinstellung 16; wird mit dem Projekt gespeichert). CCs, die an den Track
des Plugins gehen, wirken genauso.

CC-Nummer = 10 × Lane + Funktion, also z. B. Lane 3 Fill = CC 32:

| Endziffer | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| Funktion | Enable | Steps | Fill | Shift | Loop | Gate | Velocity | Humanize | Note | Div |

Lane 1 = CC 10–19, Lane 2 = CC 20–29 … Lane 8 = CC 80–89. Der Wertebereich 0–127 wird auf den Bereich des Parameters
verteilt (Steps, Fill, Shift, Loop: bis 32); Enable schaltet ab Wert 64 ein.

Global: CC 100 Preset, CC 101 Load, CC 102 Save, CC 103 Rand Lane (welche Lanes CC 104 trifft: ALL, LANE 1–8,
ALL BUT 1, ALL BUT 5), CC 104 Randomize (Tasten lösen ab Wert 64 aus).

Im Log steht pro 5 s `control cc N seen, M used`: „seen" sind alle CCs, die am Port ankommen, „used" die auf dem
eingestellten Kanal mit gültiger Nummer.

Das ist die CC-Steuerung dieses Forks über den Port „Control In". Die eigene CC-Belegung der Engine (`docs/MIDI-CC.md`)
gilt für deren MIDI-Eingang im Standalone-Betrieb.

## Setting it up

1. Run the workflow **VST release (draft)** (or **VST release FX (draft)**) in this repo's Actions tab; with
   `dry_run` the zip is a run artifact, otherwise a draft release. The zip holds the plugin and its skin in one
   folder, plus an installer: follow its `INSTALL.md` (copy it to the device, run `install.sh` as root; it stops MPC,
   so save your project first). A rooted device is required.
2. Insert Euclidier on a track (or Euclidier FX as an insert). There is no separate engine to start.
3. The plugin's MIDI port ("Euclidier" / "Euclidier FX") appears on its own: enable it for the tracks that should
   receive it in Preferences, MIDI, or pick it as the track's MIDI input. MPC ignores a plugin's own VST MIDI output,
   which is why the plugin has its own port.
4. The plugin turns the host's transport and tempo into the engine's clock, so it plays and stops with the project.

## How it works

- `vst/euclidier_vst.cpp` is a hand-written VST2 wrapper with the engine (`src/`) compiled in: `vst/build.sh` copies
  the engine next to `vst/rtmidi_stub/RtMidi.h`, so the engine's MIDI output goes to the plugin's own ALSA port instead
  of RtMidi, and the wrapper calls the engine's `handleMidi()`, `processQ()` and `ctrlGet()`/`ctrlSet()` directly.
- Clock: every audio block, the host's `ppqPos` is turned into 24-PPQN pulses by absolute index, and the engine's tick
  is set to that index before each pulse, so the lanes sit on the MPC grid also after a loop, a locate or a tempo
  change (this fork's clock fix). The host tempo drives gate lengths.
- Step displays (upstream's design): display-only parameters, one per cell per size class (`g<lane>_<cap>_<slot>` for
  the lane rows, `c<cap>_<slot>` for the circle). A worker thread reads each lane's `l<N>_pattern` about 33 times a
  second and turns it into the cells' values; changes are reported to the host from `processReplacing` with
  `audioMasterAutomate`, because MPC does not redraw engine-driven values on its own. Randomise buttons set the
  engine's lane flags, fire `rand_go` and clear the flags.
- `vst/make_skin.py` generates `params.json`, `layout.conf` and `images/` (the engine's parameter list from
  `vst/module.json`, the theme from `vst/layout.theme`; the fork's additions are listed in its header). `tools/gen_vst.py`
  from mpc-vst-plugins then builds `params.h` and the skin with the browser renderer (`"art": "html"` in `vst.json`).
  `Control Ch` is the last parameter, so every upstream parameter keeps its index.

## Building

```
vst/build.sh && vst/test.sh          # Euclidier
vst-fx/build.sh && vst-fx/test.sh    # Euclidier FX
```

Needs Docker and a checkout of [open-mpc/mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins) at
`../../mpc-vst` (or set `MPC_VST`). `build.sh` runs `make_skin.py` and `gen_vst.py` (in the `mpc-vst-html-art` image,
or locally when Pillow and Playwright's Chromium are installed), then cross-builds the `.so` for armhf (glibc 2.31, so
it loads on MPC OS 2.x and 3.x). `EUCLIDIER_SKIP_ARM=1` stops after the skin and `params.h`. `test.sh` builds an x86
ASan/UBSan copy and runs `vst/host_test.cpp` against it: both variants' audio path, notes and no hanging notes after
Stop, the second instance staying silent, fresh-instance defaults, the step cells and play-head, the lane stepper,
randomise (one lane, the selected lane, all), DRUM MODE ALL, the preset bank, CC control on the Control Ch, and the
project chunk (no triggers saved or replayed).

The workflows in `.github/workflows/` call `open-mpc/mpc-vst-plugins`' shared `vst-release.yml`, pinned to one commit
in `uses:` and `tools_ref`; bump both together.

## Files

| Path | What |
|---|---|
| `vst/euclidier_vst.cpp` | the plugin (both variants) |
| `vst/rtmidi_stub/RtMidi.h` | stand-in for RtMidi: the engine's MIDI output goes to the plugin |
| `vst/make_skin.py`, `vst/layout.theme` | skin generator and its theme |
| `vst/params.json`, `vst/layout.conf`, `vst/images/` | generated by `make_skin.py` (committed) |
| `vst/params.lock`, `vst-fx/params.lock` | parameter index lock written by `gen_vst.py` (committed: saved projects depend on it) |
| `vst/module.json` | the engine's control key set |
| `vst/host_test.cpp`, `vst/test.sh`, `vst-fx/test.sh` | offline test |
| `vst-fx/vst.json`, `vst-fx/build.sh` | the insert-effect variant |
| `src/` | the sequencer engine's sources (`src/ORIGIN.md` says where they came from) |
| `docs/ENGINE.md` | what the Force rework changed, and the engine's control keys |
| `docs/MIDI-CC.md` | the engine's own MIDI CC map |
| `docs/progressions/` | chord-pad progression files for the engine's control track, with their own readme |

## Credits and licence

- The sequencer is [EUCLIDIER-CONSOLE](https://github.com/intelliriffer/EUCLIDIER-CONSOLE) by Amit Talwar
  ([intelliriffer](https://github.com/intelliriffer)); the engine and the chord progressions come from it.
- The Force rework, the plugin and its skin design are by [sd88me](https://github.com/sd88me).
- This fork (in-process engine, clock fix, Euclidier FX, CC control, SETUP tab) by lunarscraper, developed with the
  help of Claude (Anthropic).

MIT, see [LICENSE](LICENSE). That covers sd88me's own work. Upstream publishes no licence file, so code inherited from
`intelliriffer/EUCLIDIER-CONSOLE` stays subject to whatever terms its author intends, once clarified.
