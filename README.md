# mpc-vst-euclidier

MPC/Force VST2 port of [force-euclidier](https://github.com/sd88me/force-euclidier)'s 8-lane
Euclidean MIDI sequencer, for the Akai MPC OS plugin host (MPC Live/One/X/Key, Force).

`vst/` is the plugin port (see `vst/build.sh`); `src/` vendors the standalone engine's own
sources (see `src/VENDORED.md`) so `vst/build.sh`'s offline host_test build is self-contained.

Build/deploy workflow: see `sd88me/mpc-vst-plugins`' `docs/PORTING.md` and
`.claude/skills/mpc-vst-plugin/SKILL.md`.

## Zwei Varianten

| | Ordner | Datei | Typ | Workflow | ALSA-MIDI-Port |
|---|---|---|---|---|---|
| Euclidier | `vst/` | `euclidier.so` | Instrument (belegt einen der 8 Plugin-Slots) | VST release (draft) | `Euclidier` |
| Euclidier FX | `vst-fx/` | `euclidier_fx.so` | Insert-Effekt (belegt keinen Instrumenten-Slot) | VST release FX (draft) | `Euclidier FX` |

Beide entstehen aus derselben `vst/euclidier_vst.cpp`; `vst-fx/vst.json` setzt nur `"effect": true`.
Die FX-Variante reicht das Audio des Tracks unverändert durch und sendet ihre Noten wie bisher
über den eigenen MIDI-Port. Beide lassen sich nebeneinander installieren (eigene UID, eigener
Name, eigener Skin-Ordner) und teilen sich die Preset-Datei `euclidier_BANK.bin`.

Euclidier FX einsetzen: als Insert auf den Track des gesteuerten Instruments (oder auf den
Master) legen und beim Ziel-Track als MIDI-Eingang `Euclidier FX` wählen.

Diagnose: `/tmp/euclidier_vst.log` schreibt alle 5 s eine Zeile. `playing 1` und steigende
`pulses` heißen, dass der Insert Transport und Tempo bekommt; `no timeinfo` > 0 heißt, dass der
Host einem Insert keine Zeitinfo liefert; bleiben die Zeilen ganz aus, hält der Host den Insert
bei Stille an.

## Steuerung per MIDI-CC (beide Varianten)

Das Plugin hat einen zweiten MIDI-Port „Control In" und verbindet ihn selbst mit allen
angeschlossenen Hardware-MIDI-Eingängen (auch später eingesteckten). Es reagiert nur auf CCs
des Kanals, der im Tab PRESETS unter CONTROL CH eingestellt ist (OFF, 1–16; Voreinstellung 16;
wird mit dem Projekt gespeichert).

CC-Nummer = 10 × Lane + Funktion, also z. B. Lane 3 Fill = CC 32:

| Endziffer | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| Funktion | Enable | Steps | Fill | Shift | Loop | Gate | Velocity | Humanize | Note | Div |

Lane 1 = CC 10–19, Lane 2 = CC 20–29 … Lane 8 = CC 80–89. Der Wertebereich 0–127 wird auf
den Bereich des Parameters verteilt; Enable schaltet ab Wert 64 ein.

Global: CC 100 Preset, CC 101 Load, CC 102 Save, CC 103 Rand Lane, CC 104 Randomize
(Tasten lösen ab Wert 64 aus).

Im Log (`/tmp/euclidier_vst.log`) steht pro 5 s `control cc N seen, M used`: „seen" sind alle
CCs, die am Port ankommen, „used" die auf dem eingestellten Kanal mit gültiger Nummer.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
