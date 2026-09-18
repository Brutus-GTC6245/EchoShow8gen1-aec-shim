#!/usr/bin/env bash
#
# Standalone NDK build of libamznaec_shim.so (Speex-only AEC engine) for the
# Echo Show 8 (1st gen, LineageOS 18.1 "crown", 32-bit armv7).
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

echo ">> building libamznaec_shim.so (armv7)"
"$CXX" -shared -fPIC -O2 -std=c++17 -Wall \
  -I"$SPX/include" \
  "$HERE/src/amznaec_speex_shim.cpp" "$OUT/libspeexdsp.a" \
  -static-libstdc++ -llog -lm -ldl \
  -o "$OUT/libamznaec_shim.so"

rm -f "${objs[@]}"
echo ">> done: $OUT/libamznaec_shim.so"
file "$OUT/libamznaec_shim.so" 2>/dev/null || true
