#!/usr/bin/env bash
# Build Euclidier as a VST2 plugin for the MPC OS plugin host (armhf), engine IN-PROCESS.
#   vst/build/euclidier.so          -> /sdcard/vst/ on the device
#   vst/build/pluginlist-entry.xml  the <PLUGIN> line for MPC.settings' pluginList-arm
#   vst/build/skin/                 -> /sdcard/Synths/ on the device
# Like the Acid VST: the vendored engine (../src, unmodified) is compiled into the .so. build/eng/
# gets a copy of it next to rtmidi_stub/RtMidi.h, so the engine's `#include "RtMidi.h"` picks up
# the stub (no ports, no RtMidi.cpp): its MIDI output goes through euclidier_vst.cpp to the
# plugin's own ALSA seq port "Euclidier". No child process, no AddOns path.
# vst.json/module.json only feed tools/gen_vst.py for params.h + the skin. Offline test: test.sh.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin artwork renderer (host binary; the renderer vendored in mpc-vst-plugins)
if command -v gcc >/dev/null; then
  gcc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w gcc:12 \
    gcc -O2 -I/mv/tools/vendor/force-shadow/tools -o build/shadow_art /mv/tools/shadow_art.c -lm
fi

# 2. params.h, skin, pluginlist-entry.xml (needs Pillow)
if python3 -c "import PIL" 2>/dev/null; then
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 /mv/tools/gen_vst.py vst.json"
fi
cp "$MPC_VST/wrapper/popup.h" build/   # popup open-flag handling shared with mpc-vst's own wrapper

# 3. engine copy + RtMidi stub (see header)
rm -rf build/eng && mkdir -p build/eng
cp ../src/euclidier.cpp ../src/eqseq.cpp ../src/eqseq.h ../src/bjlund.cpp ../src/bjlund.h ../src/commontypes.h build/eng/
cp rtmidi_stub/RtMidi.h build/eng/

# 4. the plugin (armhf, glibc 2.36 so it loads on the device's 2.39)
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst arm32v7/gcc:12 bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  mkdir -p build/obj
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -w -Ibuild/eng -c build/eng/eqseq.cpp -o build/obj/eqseq.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -w -Ibuild/eng -c build/eng/bjlund.cpp -o build/obj/bjlund.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-char-subscripts \
      -Wno-parentheses -Ibuild -Ibuild/eng -c euclidier_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/euclidier.so build/obj/vst.o build/obj/eqseq.o build/obj/bjlund.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -ldl -lm -Wl,--no-undefined
  strip build/euclidier.so
  echo "-- exported --"; readelf --dyn-syms -W build/euclidier.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/euclidier.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V build/euclidier.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/euclidier.so
