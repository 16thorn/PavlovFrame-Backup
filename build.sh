#!/usr/bin/env bash
# Build libpavchams.so, repack the APK, align, sign, and install to a connected Quest.
# Edit the paths below for your machine, then: ./build.sh
set -e

# ---- EDIT THESE ----
NDK="/c/Users/$USER/AppData/Local/Android/Sdk/ndk/android-ndk-r27-windows/android-ndk-r27"
BUILDTOOLS="/c/Users/$USER/AppData/Local/Android/Sdk/build-tools/34.0.0"
KEYSTORE="ratkey.jks"           # your signing keystore (generate with keytool; not committed)
KS_PASS="ratman4080"            # store + key password
KS_ALIAS="rat"                  # key alias
# --------------------

CLANG="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang++.cmd"

echo "[1/4] compiling libpavchams.so"
"$CLANG" -std=c++17 -O2 -fPIC -shared -o libpavchams.so pavchams.cpp -llog

echo "[2/4] repacking APK (repack.py)"
python repack.py

echo "[3/4] zipalign + sign"
rm -f Pavlov-EOS-aligned.apk Pavlov-EOS-signed.apk
"$BUILDTOOLS/zipalign.exe" -f -p 4 Pavlov-EOS-unsigned.apk Pavlov-EOS-aligned.apk
"$BUILDTOOLS/apksigner.bat" sign --ks "$KEYSTORE" --ks-pass "pass:$KS_PASS" \
  --key-pass "pass:$KS_PASS" --ks-key-alias "$KS_ALIAS" \
  --out Pavlov-EOS-signed.apk Pavlov-EOS-aligned.apk

echo "[4/4] installing"
adb install -r Pavlov-EOS-signed.apk

echo "done. set the mode:  adb shell 'echo 6 > /sdcard/Android/data/com.vankrupt.pavlov/files/chams.txt'"
