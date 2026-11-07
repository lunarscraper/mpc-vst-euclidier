#!/usr/bin/env bash
# Build Euclidier as a VST2 plugin for the MPC OS plugin host (armhf), engine IN-PROCESS.
#   vst/build/euclidier.so          -> the plugin folder on the device (release.py / install.sh put it there)
#   vst/build/pluginlist-entry.xml  the <PLUGIN> line for MPC.settings' pluginList-arm
#   vst/build/skin/                 the skin (upstream's design: make_skin.py, browser-drawn art)
# The engine (../src) is compiled into the .so: build/eng/ gets a copy of it next to rtmidi_stub/RtMidi.h, so the
# engine's `#include "RtMidi.h"` picks up the stub (no ports, no RtMidi.cpp): its MIDI output goes through
# euclidier_vst.cpp to the plugin's own ALSA seq port "Euclidier". No child process, no engine binary to ship.
# Offline test: test.sh. Needs Docker and mpc-vst-plugins (open-mpc/mpc-vst-plugins) at ../../mpc-vst or $MPC_VST.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin inputs (params.json, layout.conf, images/) from make_skin.py, then params.h + the skin (browser renderer:
#    vst.json "art": "html"). Locally with Pillow + Playwright's Chromium if present, else in the mpc-vst-html-art image.
#    SHADOW_SKIN_MPC_OS=2 (the default here) writes TUI.json in the MPC OS 2.x shape; it also draws on 3.x.
export SHADOW_SKIN_MPC_OS="${SHADOW_SKIN_MPC_OS:-2}"
if [ -z "${EUCLIDIER_DOCKER_ART:-}" ] && python3 -c "import PIL, playwright" 2>/dev/null; then
  python3 make_skin.py
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
else
  docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null
  docker run --rm -u "$U" -e HOME=/tmp -v "$PWD":/w -w /w mpc-vst-html-art python3 make_skin.py
  docker run --rm -u "$U" -e HOME=/tmp -e SHADOW_SKIN_MPC_OS -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w mpc-vst-html-art \
    python3 /mv/tools/gen_vst.py vst.json
fi
cp "$MPC_VST/wrapper/popup.h" build/   # popup open-flag handling shared with mpc-vst's own wrapper

# 2. engine copy + RtMidi stub (see header)
rm -rf build/eng && mkdir -p build/eng
cp ../src/euclidier.cpp ../src/eqseq.cpp ../src/eqseq.h ../src/bjlund.cpp ../src/bjlund.h ../src/commontypes.h build/eng/
cp rtmidi_stub/RtMidi.h build/eng/

[ -n "${EUCLIDIER_SKIP_ARM:-}" ] && { echo "EUCLIDIER_SKIP_ARM set: skin and params.h only"; exit 0; }

# 3. the plugin (armhf, glibc 2.31 (bullseye) so it loads on MPC OS 2.x (2.32) and 3.x (2.39))
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst arm32v7/gcc:11-bullseye bash -euxc '
  apt-get update -qq && apt-get install -y -qq -t bullseye libasound2-dev >/dev/null
  mkdir -p build/obj
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -w -Ibuild/eng -c build/eng/eqseq.cpp -o build/obj/eqseq.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -w -Ibuild/eng -c build/eng/bjlund.cpp -o build/obj/bjlund.o
  g++ -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-char-subscripts \
      -Wno-parentheses -Ibuild -Ibuild/eng -c euclidier_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/euclidier.so build/obj/vst.o build/obj/eqseq.o build/obj/bjlund.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -ldl -lm -Wl,--no-undefined -Wl,--gc-sections
  # --gc-sections drops the standalone main() and its control-socket server (never called in-process), so the
  # catalog check does not flag network calls
  strip build/euclidier.so
  echo "-- exported --"; readelf --dyn-syms -W build/euclidier.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/euclidier.so | grep NEEDED
  echo "-- highest glibc (MPC OS 2.x has 2.32) --"; readelf -V build/euclidier.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/euclidier.so
