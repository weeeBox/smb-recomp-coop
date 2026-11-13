#!/bin/bash
# android/build.sh ROM NAME -> build/NAME.apk (arm64). Needs the Android SDK (build-tools,
# a platform), the NDK and a JDK (javac for the cheats screen, apksigner, keytool). No Gradle.
# The APK holds the recompiled code but no ROM data; android/install.sh copies the ROM over.
set -euo pipefail
cd "$(dirname "$0")/.."
ROM=${1:-rom/smb.nes}; NAME=${2:-smb}; LABEL=${LABEL:-Super Mario Bros.}
SDK=${RECOMP_SDK:-$HOME/Library/Android/sdk}  # has the NDK; $ANDROID_HOME may be another SDK
NDK=${ANDROID_NDK:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
JAR=$(ls "$SDK"/platforms/*/android.jar | sort -V | tail -1)
export JAVA_HOME=${JAVA_HOME:-$(brew --prefix openjdk)}
export PATH="$JAVA_HOME/bin:$PATH"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang"
OUT=build/android; rm -rf "$OUT"; mkdir -p "$OUT/lib/arm64-v8a"

MODS=(); [ -f "mods-$NAME.txt" ] && MODS=(--mods "mods-$NAME.txt")
python3 -I recomp.py "$ROM" "build/$NAME.gen.c" "misses-$NAME.txt" "${MODS[@]}"
GLUE="$NDK/sources/android/native_app_glue"
"$CC" -O1 -fPIC -I. -c "build/$NAME.gen.c" -o "$OUT/gen.o"
"$CC" -O2 -fPIC -Wall -I. -I"$GLUE" -DNAME="\"$NAME\"" -DBUILD="\"$(git describe --always --dirty)\"" ${CFLAGS_EXTRA:-} -shared -o "$OUT/lib/arm64-v8a/lib$NAME.so" \
  android/main.c "$GLUE/android_native_app_glue.c" "$OUT/gen.o" \
  -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384 -landroid -llog -laaudio -lEGL -lGLESv2

sed "s/NAME/$NAME/g; s/LABEL/$LABEL/" android/AndroidManifest.xml > "$OUT/AndroidManifest.xml"
"$BT/aapt2" link -o "$OUT/base.apk" --manifest "$OUT/AndroidManifest.xml" -I "$JAR" \
  --min-sdk-version 26 --target-sdk-version 34
# The cheats screen (android/Cheats.java), the only Java: compiled against the platform, dexed.
javac -source 11 -target 11 -nowarn -cp "$JAR" -d "$OUT/classes" android/Cheats.java
"$BT/d8" --min-api 26 --lib "$JAR" --output "$OUT" "$OUT"/classes/local/recomp/*.class
(cd "$OUT" && zip -q -0 base.apk "lib/arm64-v8a/lib$NAME.so" && zip -q base.apk classes.dex)
"$BT/zipalign" -f -P 16 4 "$OUT/base.apk" "$OUT/aligned.apk"
KS="$HOME/.android/debug.keystore"
if [ ! -f "$KS" ]; then
  mkdir -p "$HOME/.android"
  keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
    -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "build/$NAME.apk" "$OUT/aligned.apk"
echo "built build/$NAME.apk"
