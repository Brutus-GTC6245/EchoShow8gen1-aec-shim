#!/usr/bin/env bash
#
# Fully revert scripts/install.sh: restore every ".orig" backup, remove the shim,
# clear the props, and reboot to a clean stock audio HAL.
set -euo pipefail

RC=/vendor/etc/init/android.hardware.audio.service.rc
XML=/system/etc/audio_device.xml

adb wait-for-device
echo ">> adb root + remount"
adb root >/dev/null; adb wait-for-device
adb remount >/dev/null 2>&1 || true
adb shell 'mount -o rw,remount /vendor' 2>/dev/null || true
adb shell 'mount -o rw,remount /system' 2>/dev/null || true

echo ">> restoring $RC"
adb shell "sh -c '[ -f \"$RC.orig\" ] && { cat \"$RC.orig\" > \"$RC\"; rm -f \"$RC.orig\"; echo restored; } || echo \"no $RC.orig (nothing to restore)\"'"

echo ">> restoring $XML (if it was changed)"
adb shell "sh -c '[ -f \"$XML.orig\" ] && { cat \"$XML.orig\" > \"$XML\"; rm -f \"$XML.orig\"; echo restored; } || echo \"no $XML.orig (was not changed)\"'"

echo ">> removing shim + props"
adb shell 'rm -f /vendor/lib/libamznaec_shim.so; setprop persist.vendor.amznaec.enable 0; setprop persist.vendor.amznaec.log 0'

echo ">> rebooting to clean state"
adb reboot
echo "Reverted. After boot, capture runs through the stock HAL again."
