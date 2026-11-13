#!/bin/bash
# android/install.sh ROM NAME: install build/NAME.apk on the connected device, copy the ROM
# into the app's private files folder (run-as works because the APK is debuggable), launch it.
set -euo pipefail
cd "$(dirname "$0")/.."
ROM=${1:-rom/smb.nes}; NAME=${2:-smb}; PKG=local.recomp.$NAME
adb install -r "build/$NAME.apk"
adb push "$ROM" "/data/local/tmp/$NAME.nes" >/dev/null
adb shell run-as "$PKG" mkdir -p files
adb shell run-as "$PKG" cp "/data/local/tmp/$NAME.nes" "files/$NAME.nes"
adb shell rm "/data/local/tmp/$NAME.nes"
adb shell am start -n "$PKG/android.app.NativeActivity"
