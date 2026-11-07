#!/usr/bin/env bash
# Build Euclidier FX: the insert-effect variant of ../vst (same sources and skin, see ../vst/build.sh).
#   vst-fx/build/euclidier_fx.so        the plugin
#   vst-fx/build/pluginlist-entry.xml   the <PLUGIN ... category="Effect"> line
#   vst-fx/build/skin/                  the skin
# Only vst.json differs ("effect": true -> PLUG_EFFECT in params.h); euclidier_vst.cpp, make_skin.py's params.json /
# layout.conf / images and the RtMidi stub are taken from ../vst.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. ../vst's skin inputs, then params.h (with PLUG_EFFECT) + the skin
export SHADOW_SKIN_MPC_OS="${SHADOW_SKIN_MPC_OS:-2}"
if [ -z "${EUCLIDIER_DOCKER_ART:-}" ] && python3 -c "import PIL, playwright" 2>/dev/null; then
  (cd ../vst && python3 make_skin.py)
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
else
  docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null
  docker run --rm -u "$U" -e HOME=/tmp -v "$PWD/..":/w -w /w/vst mpc-vst-html-art python3 make_skin.py
  docker run --rm -u "$U" -e HOME=/tmp -e SHADOW_SKIN_MPC_OS -v "$PWD/..":/w -v "$MPC_VST":/mv:ro -w /w/vst-fx mpc-vst-html-art \
    python3 /mv/tools/gen_vst.py vst.json
fi
grep -q "PLUG_EFFECT" build/params.h || { echo "params.h has no PLUG_EFFECT: this would build the instrument"; exit 1; }
cp "$MPC_VST/wrapper/popup.h" build/

# 2. engine copy + RtMidi stub
rm -rf build/eng && mkdir -p build/eng
cp ../src/euclidier.cpp ../src/eqseq.cpp ../src/eqseq.h ../src/bjlund.cpp ../src/bjlund.h ../src/commontypes.h build/eng/
cp ../vst/rtmidi_stub/RtMidi.h build/eng/

[ -n "${EUCLIDIER_SKIP_ARM:-}" ] && { echo "EUCLIDIER_SKIP_ARM set: skin and params.h only"; exit 0; }

# 3. the plugin (armhf, glibc 2.31 (bullseye) so it loads on MPC OS 2.x and 3.x)
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst-fx arm32v7/gcc:11-bullseye bash -euxc '
  apt-get update -qq && apt-get install -y -qq -t bullseye libasound2-dev >/dev/null
  mkdir -p build/obj
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -w -Ibuild/eng -c build/eng/eqseq.cpp -o build/obj/eqseq.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -w -Ibuild/eng -c build/eng/bjlund.cpp -o build/obj/bjlund.o
  g++ -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-char-subscripts \
      -Wno-parentheses -Ibuild -Ibuild/eng -c ../vst/euclidier_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/euclidier_fx.so build/obj/vst.o build/obj/eqseq.o build/obj/bjlund.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -ldl -lm -Wl,--no-undefined -Wl,--gc-sections
  # --gc-sections drops the standalone main() and its control-socket server (never called in-process), so the
  # catalog check does not flag network calls
  strip build/euclidier_fx.so
  echo "-- exported --"; readelf --dyn-syms -W build/euclidier_fx.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/euclidier_fx.so | grep NEEDED
  echo "-- highest glibc (MPC OS 2.x has 2.32) --"; readelf -V build/euclidier_fx.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/euclidier_fx.so
