#!/usr/bin/env bash
#
# Standalone NDK build of libamznaec_shim.so (Speex-only AEC engine) for the
# Amazon Echo Show (LineageOS 18.1, MT8163, 32-bit armv7).
#
# The FPGA capture stream's total channel count differs by device, but the last
# two channels are always the DAC loopback (the far-end reference). Pick the
# device with --device (or the DEVICE env var); it maps to the build flag that
# sets the channel count in src/:
#
#   Show81stGen   Echo Show 8 (1st gen, crown)   6 channels (4 mic + 2 loopback)
#   Show52ndGen   Echo Show 5 (2nd gen, cronos)  4 channels (2 mic + 2 loopback)
#
#   ./scripts/build.sh [--device Show81stGen|Show52ndGen]
#   DEVICE=Show52ndGen ./scripts/build.sh
#
# Default: Show81stGen (backward compatible with earlier builds of this repo).
#
# This does NOT need the LineageOS ROM tree — it vendors the (BSD) speexdsp
# sources under third_party/ and links only the NDK sysroot's libc/liblog. The
# stronger WebRTC engine variant (38-50 dB) DOES need the ROM tree; see README.
#
# Requirements: Android NDK r28 (28.2.13676358 tested). Point ANDROID_NDK at it.
# Output: out/libamznaec_shim.so
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/28.2.13676358}"
API="${API_LEVEL:-30}"
ABI=armv7a-linux-androideabi

DEVICE="${DEVICE:-Show81stGen}"
while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done
case "$DEVICE" in
  Show81stGen|Show52ndGen) ;;
  *) echo "invalid --device: $DEVICE (want Show81stGen or Show52ndGen)" >&2; exit 1 ;;
esac

[ -d "$NDK" ] || { echo "NDK not found: $NDK  (set ANDROID_NDK)" >&2; exit 1; }

case "$(uname -s)" in
  Darwin) HOST=darwin-x86_64 ;;
  Linux)  HOST=linux-x86_64  ;;
  *) echo "unsupported build host: $(uname -s)" >&2; exit 1 ;;
esac
TC="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
CC="$TC/${ABI}${API}-clang"
CXX="$TC/${ABI}${API}-clang++"
AR="$TC/llvm-ar"
[ -x "$CC" ] || { echo "clang not found: $CC" >&2; exit 1; }

SPX="$HERE/third_party/speexdsp"
OUT="$HERE/out"; mkdir -p "$OUT"

echo ">> building speexdsp (armv7)"
SPXFLAGS=(-O2 -fPIC -DFLOATING_POINT -DUSE_KISS_FFT -DEXPORT= -DVAR_ARRAYS
          -I"$SPX/include" -I"$SPX/libspeexdsp"
          -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable)
objs=()
for f in mdf preprocess filterbank fftwrap kiss_fft kiss_fftr; do
  "$CC" -c "${SPXFLAGS[@]}" "$SPX/libspeexdsp/$f.c" -o "$OUT/$f.o"
  objs+=("$OUT/$f.o")
done
"$AR" rcs "$OUT/libspeexdsp.a" "${objs[@]}"

echo ">> building libamznaec_shim.so (armv7) for $DEVICE"
"$CXX" -shared -fPIC -O2 -std=c++17 -Wall \
  -D"$DEVICE" \
  -I"$SPX/include" \
  "$HERE/src/amznaec_speex_shim.cpp" "$OUT/libspeexdsp.a" \
  -static-libstdc++ -llog -lm -ldl \
  -o "$OUT/libamznaec_shim.so"

rm -f "${objs[@]}"
echo ">> done: $OUT/libamznaec_shim.so"
file "$OUT/libamznaec_shim.so" 2>/dev/null || true
