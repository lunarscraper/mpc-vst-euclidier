#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan) of the in-process build: run after build.sh (needs build/params.h,
# build/popup.h, build/eng/). Prints PASSED/FAILED; exit code follows.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD/..":/b -w /b/vst gcc:12 bash -euc 'set -o pipefail
  mkdir -p build/x86
  for f in eqseq bjlund; do
    g++ -O0 -g -fsanitize=address,undefined -std=c++17 -w -fPIC -Ibuild/eng -c build/eng/$f.cpp -o build/x86/$f.o
  done
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -w -DNO_ALSA -fPIC -Ibuild -Ibuild/eng \
      -c euclidier_vst.cpp -o build/x86/vst.o
  g++ -shared -fsanitize=address,undefined -o build/x86/euclidier-x86.so build/x86/*.o -lpthread -ldl
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -o build/x86/host_test host_test.cpp -ldl
  ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/euclidier-x86.so | grep -v "^MIDI\|^[0-9]*$\|^rnd is"
'
