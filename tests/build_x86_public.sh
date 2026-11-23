#!/usr/bin/env bash
# Omni Sampler PUBLIC 2.3: the desktop test tools of the public source (x86, ASan/UBSan, -DOMNI_PUBLIC=1).
#   MPC_VST=/path/to/mpc-vst-plugins JOBS=2 tests/build_x86_public.sh      (from the public checkout's parent or anywhere)
# The private-only tests (Kawai, Omnisphere, UJAM ...) are not built: their readers are not in the public source.
set -euo pipefail
PORT="$(cd "$(dirname "$0")/.." && pwd)"
MV="$(cd "${MPC_VST:?set MPC_VST to a checkout of mpc-vst-plugins}" && pwd)"
OUT="$PORT/build/x86"
OBJ="$OUT/obj-public"
FLAGS=(-std=gnu++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DOMNI_PUBLIC=1)
WARN=(-Wall -Wextra -Wno-unused-parameter)
mkdir -p "$OBJ"
JOBS="${JOBS:-2}"
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || true; done; }
SRC=($(cd "$PORT" && ls src/core/*.cpp src/formats/*.cpp src/fs/*.cpp src/engine/*.cpp))
OBJS=()
for s in "${SRC[@]}"; do
  o="$OBJ/$(echo "$s" | tr / _).o"
  if [ ! -f "$o" ] || [ "$PORT/$s" -nt "$o" ] || [ -n "$(find "$PORT/src" -name '*.hpp' -newer "$o" -print -quit)" ]; then
    throttle; g++ "${FLAGS[@]}" "${WARN[@]}" -I"$PORT/src" -c "$PORT/$s" -o "$o" &
  fi
  OBJS+=("$o")
done
for s in src/third_party/tinf/tinflate.c src/third_party/tinf/tinfzlib.c src/third_party/tinf/tinfgzip.c src/third_party/tinf/adler32.c src/third_party/tinf/crc32.c src/third_party/zstd/zstddeclib.c; do
  o="$OBJ/$(echo "$s" | tr / _).o"
  [ -f "$o" ] || throttle; [ -f "$o" ] || gcc "${FLAGS[@]/-std=gnu++17/-std=gnu11}" -c "$PORT/$s" -o "$o" &
  OBJS+=("$o")
done
wait
P=(-I"$PORT/src" -I"$MV/wrapper")
g++ "${FLAGS[@]}" "${WARN[@]}" -I"$PORT/src" "$PORT/tests/probe.cpp" "${OBJS[@]}" -lpthread -o "$OUT/probe-public"
g++ "${FLAGS[@]}" "${WARN[@]}" "${P[@]}" "$PORT/tests/test_public.cpp" "$PORT/src/plugin.cpp" "${OBJS[@]}" -lpthread -o "$OUT/test_public"
g++ "${FLAGS[@]}" "${WARN[@]}" "${P[@]}" "$PORT/tests/test_tap.cpp" "$PORT/src/plugin.cpp" "${OBJS[@]}" -lpthread -o "$OUT/test_tap-public"
g++ "${FLAGS[@]}" "${WARN[@]}" -I"$PORT/src" "$PORT/tests/test_serum_map.cpp" "${OBJS[@]}" -lpthread -o "$OUT/test_serum_map-public"
g++ "${FLAGS[@]}" "${WARN[@]}" "${P[@]}" "$PORT/tests/test_fx_bank.cpp" "$PORT/src/plugin.cpp" "${OBJS[@]}" -lpthread -o "$OUT/test_fx_bank-public"
echo "built $OUT/probe-public test_public test_tap-public test_serum_map-public test_fx_bank-public"
