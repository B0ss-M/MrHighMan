#!/usr/bin/env bash
# Build the desktop test tools (x86, ASan/UBSan): build/x86/probe and build/x86/test_engine.
#   MPC_VST=/path/to/mpc-vst-plugins tests/build_x86.sh [--fast]   (--fast: -O2, no sanitizers)
set -euo pipefail
PORT="$(cd "$(dirname "$0")/.." && pwd)"
MV="$(cd "${MPC_VST:?set MPC_VST to a checkout of sd88me/mpc-vst-plugins}" && pwd)"
OUT="$PORT/build/x86"
FLAGS=(-std=gnu++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
OBJ="$OUT/obj-asan"
[ "${1:-}" = "--fast" ] && FLAGS=(-std=gnu++17 -g -O2) && OBJ="$OUT/obj-fast"
mkdir -p "$OBJ"
WARN=(-Wall -Wextra -Wno-unused-parameter)
SRC=($(cd "$PORT" && ls src/core/*.cpp src/formats/*.cpp src/fs/*.cpp src/engine/*.cpp))
OBJS=()
for s in "${SRC[@]}"; do
  o="$OBJ/$(echo "$s" | tr / _).o"
  if [ ! -f "$o" ] || [ "$PORT/$s" -nt "$o" ] || [ -n "$(find "$PORT/src" -name '*.hpp' -newer "$o" -print -quit)" ]; then
    g++ "${FLAGS[@]}" "${WARN[@]}" -I"$PORT/src" -c "$PORT/$s" -o "$o" &
  fi
  OBJS+=("$o")
done
for s in src/third_party/tinf/tinflate.c src/third_party/tinf/tinfzlib.c src/third_party/tinf/tinfgzip.c src/third_party/tinf/adler32.c src/third_party/tinf/crc32.c; do
  o="$OBJ/$(echo "$s" | tr / _).o"
  [ -f "$o" ] || gcc "${FLAGS[@]/-std=gnu++17/-std=gnu11}" -c "$PORT/$s" -o "$o" &
  OBJS+=("$o")
done
wait
g++ "${FLAGS[@]}" "${WARN[@]}" -I"$PORT/src" "$PORT/tests/probe.cpp" "${OBJS[@]}" -lpthread -o "$OUT/probe"
g++ "${FLAGS[@]}" "${WARN[@]}" -I"$PORT/src" -I"$MV/wrapper" "$PORT/tests/test_engine.cpp" "$PORT/src/plugin.cpp" "${OBJS[@]}" \
  -lpthread -o "$OUT/test_engine"
echo "built $OUT/probe $OUT/test_engine"
