#!/usr/bin/env bash
#
# Reversible install of the AEC shim onto an Echo Show 8 (1st gen) over adb.
# Injects it via LD_PRELOAD in the audio HAL init rc (the proven method on this
# ROM — camerahalserver preloads a shim the same way). Every file it touches is
# backed up to a ".orig" sibling on the device; scripts/uninstall.sh restores them.
#
#   ./scripts/install.sh [--so path/to/libamznaec_shim.so] [--pga 40] [--log 1]
#
# Defaults: prebuilt/libamznaec_shim.so, PGA left as-is, log on.
#
# !! SUPERVISED USE ONLY. The Echo Show's SPI/FPGA audio path is fragile; a bad
#    install can wedge it. Keep a way to power-cycle (pull DC ~5 s) and a flashable
#    ROM handy. If capture ever breaks: run scripts/uninstall.sh (or restore .orig
#    + reboot). Do not run against a device you can't physically reach.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SO="$HERE/prebuilt/libamznaec_shim.so"
RC=/vendor/etc/init/android.hardware.audio.service.rc
PGA=""          # e.g. 40 to lower ADC MICPGA (doc-recommended); empty = leave
LOG=1           # persist.vendor.amznaec.log

while [ $# -gt 0 ]; do
  case "$1" in
    --so)  SO="$2"; shift 2 ;;
    --pga) PGA="$2"; shift 2 ;;
    --log) LOG="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done
[ -f "$SO" ] || { echo "shim not found: $SO (run scripts/build.sh first)" >&2; exit 1; }

adb wait-for-device
echo ">> adb root + remount /vendor rw"
adb root >/dev/null; adb wait-for-device
adb remount >/dev/null 2>&1 || true
adb shell 'mount -o rw,remount /vendor' 2>/dev/null || true

echo ">> pushing shim to /vendor/lib"
adb push "$SO" /vendor/lib/libamznaec_shim.so >/dev/null
adb shell 'chmod 644 /vendor/lib/libamznaec_shim.so; chown root:root /vendor/lib/libamznaec_shim.so; \
           chcon u:object_r:vendor_file:s0 /vendor/lib/libamznaec_shim.so'

echo ">> injecting LD_PRELOAD into $RC (backup .orig)"
adb shell "sh -c '
  set -e
  [ -f \"$RC\" ] || { echo \"rc not found: $RC\"; exit 1; }
  [ -f \"$RC.orig\" ] || cp \"$RC\" \"$RC.orig\"
  if grep -q libamznaec_shim.so \"$RC\"; then
    echo \"   LD_PRELOAD already present\"
  else
    awk '\''/^service /{print; print \"    setenv LD_PRELOAD libamznaec_shim.so\"; next}1'\'' \"$RC.orig\" > \"$RC.tmp\"
    cat \"$RC.tmp\" > \"$RC\"; rm -f \"$RC.tmp\"
    echo \"   inserted setenv LD_PRELOAD\"
  fi
'"

if [ -n "$PGA" ]; then
  XML=/system/etc/audio_device.xml
  echo ">> lowering ADC MICPGA to $PGA in $XML (backup .orig)"
  adb shell 'mount -o rw,remount /system' 2>/dev/null || true
  adb shell "sh -c '
    [ -f \"$XML.orig\" ] || cp \"$XML\" \"$XML.orig\"
    sed \"s/\\(ADC_[AB] MICPGA Volume Ctrl\\\" value=\\\"\\)[0-9]*/\\1$PGA/g\" \"$XML.orig\" > \"$XML.tmp\"
    cat \"$XML.tmp\" > \"$XML\"; rm -f \"$XML.tmp\"
  '"
fi

echo ">> setting properties"
adb shell "setprop persist.vendor.amznaec.enable 1; setprop persist.vendor.amznaec.log $LOG"

echo ">> rebooting"
adb reboot
cat <<EOF

Installed. After boot, verify with:
  adb logcat -d | grep amznaec
Expect: 'libamznaec_shim loaded', then 'mic PCM 0:22 opened: 6 ch 16000 Hz ... SPEEX',
and (with log=1) '5s: ref .. dBFS, mic in .. dBFS, out .. dBFS' during playback.
Keep the display awake while testing capture (the app's mic is blocked while the
device dozes). To revert: ./scripts/uninstall.sh
EOF
