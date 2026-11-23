#!/usr/bin/env bash
# Build Omni Sampler: build/omni_sampler.so (armhf VST2), build/skin/, build/pluginlist-entry.xml.
#   MPC_VST=/path/to/mpc-vst-plugins ./build.sh
# MPC_VST is a checkout of https://github.com/sd88me/mpc-vst-plugins (the VST2 wrapper and skin tools).
# With a running Docker this is $MPC_VST/tools/build_port.sh. Without one it does the same with local tools: the skin through
# tools/html_art.py (HTML_ART_PYTHON: a python with Playwright, e.g. /root/.venv-htmlart/bin/python3) and the ARM
# cross compiler (ARM_CXX, default arm-linux-gnueabihf-g++-12). The design (art, params, layout) is design.py.
set -euo pipefail
PORT="$(cd "$(dirname "$0")" && pwd)"
MV="${MPC_VST:-}"
[ -n "$MV" ] && [ -f "$MV/wrapper/vst2_wrap.c" ] || { echo "build.sh: set MPC_VST to a checkout of sd88me/mpc-vst-plugins" >&2; exit 1; }
MV="$(cd "$MV" && pwd)"
if docker info >/dev/null 2>&1; then
  exec "$MV/tools/build_port.sh" "$PORT/vst.json"
fi
PY="${HTML_ART_PYTHON:-/root/.venv-htmlart/bin/python3}"
"$PY" -c 'import playwright' 2>/dev/null || { echo "build.sh: $PY has no Playwright (set HTML_ART_PYTHON)" >&2; exit 1; }
ARM_CC="${ARM_CC:-arm-linux-gnueabihf-gcc-12}"
ARM_CXX="${ARM_CXX:-arm-linux-gnueabihf-g++-12}"
ARM_STRIP="${ARM_STRIP:-arm-linux-gnueabihf-strip}"
ARCH=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -O2 -fPIC -fvisibility=hidden -DZSTDLIB_VISIBLE= -DZSTDERRORLIB_VISIBLE= -DZDICTLIB_VISIBLE=)   # 2.4.32: zstd stays private (MPC loads its own libzstd)
# 2.4.35: extra defines from the caller (build_public.sh: OMNI_DEFS=-DOMNI_PUBLIC=1); empty for the private build
[ -n "${OMNI_DEFS:-}" ] && ARCH+=(${OMNI_DEFS})
WARN=(-Wall -Wextra -Wno-unused-parameter)
cd "$PORT"
OBJ="$PORT/build/obj"
mkdir -p "$OBJ"
PATH="$(dirname "$(command -v "$PY")"):$PATH" SHADOW_ART="$MV/tools/html_art.py" "$PY" "$MV/tools/gen_vst.py" "$PORT/vst.json"
OBJS=()
PIDS=()
# At most JOBS compilers at once: launching all ~75 together OOM-killed compiles and took the whole container down.
JOBS="${JOBS:-4}"
bg() {
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || true; done
  "$@" &
  PIDS+=($!)
}
rm -f "$OBJ"/*.o
for s in $(ls src/plugin.cpp src/{core,formats,fs,engine}/*.cpp); do
  o="$OBJ/$(echo "$s" | tr / _).o"
  bg "$ARM_CXX" "${ARCH[@]}" -std=gnu++17 "${WARN[@]}" -Isrc -I"$MV/wrapper" -I"$PORT/build" -c "$s" -o "$o"
  OBJS+=("$o")
done
for n in tinflate tinfzlib tinfgzip adler32 crc32; do
  bg "$ARM_CC" "${ARCH[@]}" -std=gnu11 -c "src/third_party/tinf/$n.c" -o "$OBJ/tinf_$n.o"
  OBJS+=("$OBJ/tinf_$n.o")
done
bg "$ARM_CC" "${ARCH[@]}" -std=gnu11 -c src/third_party/zstd/zstddeclib.c -o "$OBJ/zstddeclib.o"   # 2.4.32: Serum 2 payloads
OBJS+=("$OBJ/zstddeclib.o")
bg "$ARM_CC" "${ARCH[@]}" -std=gnu11 "${WARN[@]}" -c src/glibc_compat.c -o "$OBJ/glibc_compat.o"
OBJS+=("$OBJ/glibc_compat.o")
bg "$ARM_CC" "${ARCH[@]}" -std=gnu11 "${WARN[@]}" -DMODULE_SUBDIR='"omni-sampler"' -I"$PORT/build" -I"$MV/wrapper" \
  -c "$MV/wrapper/vst2_wrap.c" -o "$OBJ/vst2_wrap.o"
OBJS+=("$OBJ/vst2_wrap.o")
FAIL=0
for p in "${PIDS[@]}"; do wait "$p" || FAIL=1; done
for o in "${OBJS[@]}"; do [ -s "$o" ] || { echo "build.sh: missing $o (compile failed or was killed)" >&2; FAIL=1; }; done
[ "$FAIL" = 0 ] || { echo "build.sh: compile failed; not linking" >&2; exit 1; }
"$ARM_CXX" "${ARCH[@]}" -shared "${OBJS[@]}" -static-libstdc++ -static-libgcc -lm -lpthread -ldl -Wl,--no-undefined \
  -o "$PORT/build/omni_sampler.so"
"$ARM_STRIP" "$PORT/build/omni_sampler.so"
echo "exported: $(readelf --dyn-syms -W "$PORT/build/omni_sampler.so" | grep -E ' GLOBAL .* [0-9]+ [A-Za-z]' | grep -v UND | awk '{print $8}' | tr '\n' ' ')"
GL=$(readelf -V "$PORT/build/omni_sampler.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
case "$GL" in GLIBC_2.3[7-9]|GLIBC_2.[4-9]*) echo "build.sh: needs $GL, over the catalog's 2.36 (see src/glibc_compat.c)" >&2; exit 1;; esac
echo "highest glibc: $(readelf -V "$PORT/build/omni_sampler.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1) (device has 2.39)"
file "$PORT/build/omni_sampler.so"
# the licence files shipped next to the plugin (release.py --extra build/licenses:vst/omni-sampler/licenses)
mkdir -p "$PORT/build/licenses"
cp "$PORT/LGPL-3.0.txt" "$PORT/GPL-3.0.txt" "$PORT/NOTICE.txt" "$PORT/src/third_party/VENDORED.md" "$PORT/build/licenses/"
cp "$PORT/src/third_party/tinf/LICENSE" "$PORT/build/licenses/tinf-LICENSE.txt"
cp "$PORT/src/third_party/zstd/LICENSE" "$PORT/build/licenses/zstd-LICENSE.txt"   # 2.4.32
[ -f "$PORT/LICENSE.txt" ] && cp "$PORT/LICENSE.txt" "$PORT/build/licenses/" || true   # 2.3 public: its own licence
