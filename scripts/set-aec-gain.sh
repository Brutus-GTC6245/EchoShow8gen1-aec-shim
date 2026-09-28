#!/usr/bin/env bash
# Set the Echo Show 8 (gen-1, `crown`) AEC shim's post-cancellation makeup gain,
# `persist.vendor.amznaec.gain_db`, over adb.
#
# WHY THIS IS A SCRIPT AND NOT AN IN-APP SETTING SOMEWHERE
# ----------------------------------------------
# `gain_db` is a **vendor system property** read by the `libamznaec_shim.so` AEC shim
# that is `LD_PRELOAD`ed into `android.hardware.audio.service`.
 # Setting a `persist.vendor.*` prop goes through init's
# property service and is gated by SELinux.
# no `su`/Magisk on the device for it to borrow. So shim gain is a **device-provisioning
# step**, done here with adb root. (You could implement a  "Capture gain (dB)" setting in the
# app-side of whatever you implement, a root-free analogue that boosts the signal later in the apps audio pipeline;
#  this script tunes the shim's own makeup gain, applied inside the HAL after cancellation.
#
# The shim reads `gain_db` when the audio HAL process starts, so a change only takes
# effect after the HAL restarts. This script sets the (persistent) prop and then
# restarts the audio HAL; if that can't be resolved it tells you to reboot.
#
# Usage:
#   scripts/set-aec-gain.sh [GAIN_DB] [ADB_SERIAL]
#
#   GAIN_DB     integer dB, default 34 (the value tuned for the production unit; the
#               shim's own default is 20). Raise toward ~34 if the streamed mic level is
#               too low (missed speech onset); dial to ~30 if close/loud speech clips.
#   ADB_SERIAL  optional; target a specific device when several are attached.
set -euo pipefail

GAIN="${1:-34}"
SERIAL="${2:-}"
PROP="persist.vendor.amznaec.gain_db"

if ! [[ "$GAIN" =~ ^[0-9]+$ ]]; then
  echo "✗ GAIN_DB must be a non-negative integer (got: '$GAIN')" >&2
  exit 2
fi

adb() { command adb ${SERIAL:+-s "$SERIAL"} "$@"; }

echo "→ elevating adbd to root"
adb root >/dev/null 2>&1 || true
adb wait-for-device

# Confirm the shim is actually present before touching its props.
if ! adb shell '[ -f /system/vendor/lib/libamznaec_shim.so ] && echo yes' | grep -q yes; then
  echo "✗ libamznaec_shim.so not found on this device — the AEC shim is not installed." >&2
  echo "  Install it first: https://github.com/Brutus-GTC6245/EchoShow8gen1-aec-shim" >&2
  exit 1
fi

echo "→ setting $PROP = $GAIN"
adb shell "setprop $PROP $GAIN"

GOT="$(adb shell "getprop $PROP" | tr -d '\r')"
if [ "$GOT" != "$GAIN" ]; then
  echo "✗ failed to set $PROP (reads back '$GOT'). Is adb root available on this build?" >&2
  exit 1
fi
echo "✓ $PROP = $GOT (persists across reboot)"

# Restart the audio HAL so the shim re-reads the prop. Find its init service name.
SVC="$(adb shell 'getprop | sed -n "s/^\[init\.svc\.\([^]]*\)\].*/\1/p"' \
        | tr -d '\r' | grep -iE 'audio.*hal|hal.*audio|android\.hardware\.audio' | head -n1 || true)"
if [ -n "$SVC" ]; then
  echo "→ restarting audio HAL service: $SVC"
  adb shell "setprop ctl.restart $SVC"
  echo "✓ requested restart of '$SVC' — new gain is now live"
else
  echo "! could not resolve the audio HAL init service name automatically."
  echo "  Reboot the device for the new gain to take effect:  adb reboot"
fi

echo
echo "Done. To revert to the shim default:  $0 20 ${SERIAL:-}"
