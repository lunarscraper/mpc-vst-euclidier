#!/usr/bin/env bash
# Build Euclidier FX: the insert-effect variant of ../vst (same sources, see ../vst/build.sh).
#   vst-fx/build/euclidier_fx.so        -> /sdcard/vst/ on the device
#   vst-fx/build/pluginlist-entry.xml   the <PLUGIN ... category="Effect"> line
#   vst-fx/build/skin/                  -> /sdcard/Synths/ on the device
# Only vst.json differs ("effect": true -> PLUG_EFFECT in params.h); euclidier_vst.cpp, module.json,
# layout.conf and the RtMidi stub are taken from ../vst.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin artwork renderer (host binary; the renderer vendored in mpc-vst-plugins)
if command -v gcc >/dev/null; then
  gcc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
else
  docker run --rm -u "$U" -v "$PWD/..":/w -v "$MPC_VST":/mv:ro -w /w/vst-fx gcc:12 \
    gcc -O2 -I/mv/tools/vendor/force-shadow/tools -o build/shadow_art /mv/tools/shadow_art.c -lm
fi

# 2. params.h (with PLUG_EFFECT), skin, pluginlist-entry.xml (needs Pillow)
if python3 -c "import PIL" 2>/dev/null; then
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
else
  docker run --rm -u "$U" -v "$PWD/..":/w -v "$MPC_VST":/mv:ro -w /w/vst-fx python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 /mv/tools/gen_vst.py vst.json"
fi
grep -q "PLUG_EFFECT" build/params.h || { echo "params.h has no PLUG_EFFECT: this would build the instrument"; exit 1; }
cp "$MPC_VST/wrapper/popup.h" build/

# 3. engine copy + RtMidi stub
rm -rf build/eng && mkdir -p build/eng
cp ../src/euclidier.cpp ../src/eqseq.cpp ../src/eqseq.h ../src/bjlund.cpp ../src/bjlund.h ../src/commontypes.h build/eng/
cp ../vst/rtmidi_stub/RtMidi.h build/eng/

# 4. the plugin (armhf, glibc 2.36 so it loads on the device's 2.39)
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst-fx arm32v7/gcc:12 bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  mkdir -p build/obj
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -w -Ibuild/eng -c build/eng/eqseq.cpp -o build/obj/eqseq.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -w -Ibuild/eng -c build/eng/bjlund.cpp -o build/obj/bjlund.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-char-subscripts \
      -Wno-parentheses -Ibuild -Ibuild/eng -c ../vst/euclidier_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/euclidier_fx.so build/obj/vst.o build/obj/eqseq.o build/obj/bjlund.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -ldl -lm -Wl,--no-undefined
  strip build/euclidier_fx.so
  echo "-- exported --"; readelf --dyn-syms -W build/euclidier_fx.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/euclidier_fx.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V build/euclidier_fx.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/euclidier_fx.so
