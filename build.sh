#!/usr/bin/env bash
# Build libpavchams.so (mod + "mei mei [private]" ImGui VR menu), repack the APK, align, sign, and
# install to a connected Quest. Edit the paths below for your machine, then: ./build.sh
set -e

# ---- EDIT THESE ----
# Auto-pick the newest installed NDK under the SDK (r26/r27/r28 all work); override NDK to pin one.
SDK_ROOT="${SDK_ROOT:-/c/Users/${USERNAME:-$USER}/AppData/Local/Android/Sdk}"
NDK="${NDK:-$(ls -d "$SDK_ROOT"/ndk/* 2>/dev/null | sort -V | tail -1)}"
BUILDTOOLS="${BUILDTOOLS:-$(ls -d "$SDK_ROOT"/build-tools/* 2>/dev/null | sort -V | tail -1)}"
# signing keystore (generate your own with keytool; the .jks is gitignored, never committed).
# override via env so no password is committed:  export KS_PASS=... KEYSTORE=... KS_ALIAS=...
KEYSTORE="${KEYSTORE:-yourkey.jks}"     # your signing keystore
KS_PASS="${KS_PASS:-changeme}"          # store + key password (set via env)
KS_ALIAS="${KS_ALIAS:-yourkey}"         # key alias
IMGUI_TAG="v1.90.9"             # pinned Dear ImGui (backend API in mei_xr.cpp targets this)
OPENXR_TAG="release-1.1.36"     # Khronos OpenXR-SDK (headers only)
# --------------------

CLANG="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang++.cmd"
CC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang.cmd"
AR="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-ar.exe"
OPUS_TAG="v1.5.2"
echo "using NDK: $NDK"
echo "using build-tools: $BUILDTOOLS"

# ---- [0/5] fetch third-party headers/sources (once) ----
mkdir -p third_party
if [ ! -d third_party/imgui ]; then
  echo "[0/5] cloning Dear ImGui $IMGUI_TAG"
  git clone --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui third_party/imgui
fi
if [ ! -d third_party/OpenXR-SDK ]; then
  echo "[0/5] cloning OpenXR-SDK $OPENXR_TAG (headers)"
  git clone --depth 1 --branch "$OPENXR_TAG" https://github.com/KhronosGroup/OpenXR-SDK third_party/OpenXR-SDK
fi
if [ ! -f third_party/stb/stb_image.h ]; then
  echo "[0/5] fetching stb_image.h (steam-shim pfp decode)"
  mkdir -p third_party/stb
  curl -sL -o third_party/stb/stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
fi
# libopus — soundboard voice encoder (compiled to a static lib, linked into libpavchams)
if [ ! -d third_party/opus ]; then
  echo "[0/5] cloning libopus $OPUS_TAG"
  git clone --depth 1 --branch "$OPUS_TAG" https://github.com/xiph/opus third_party/opus
fi
OPUS=third_party/opus
if [ ! -f third_party/libopus.a ]; then
  echo "[0/5] building libopus.a (generic C, no arch intrinsics)"
  OPUS_INC="-I$OPUS/include -I$OPUS/celt -I$OPUS/silk -I$OPUS/silk/float -I$OPUS/src"
  OPUS_DEF="-DOPUS_BUILD -DVAR_ARRAYS -DHAVE_STDINT_H -DUSE_ALLOCA"
  rm -rf third_party/opus_obj && mkdir -p third_party/opus_obj
  # gather core sources; skip arch dirs, fixed-point (we build float), demos + tests.
  OPUS_SRCS=$(ls "$OPUS"/src/*.c "$OPUS"/celt/*.c "$OPUS"/silk/*.c "$OPUS"/silk/float/*.c 2>/dev/null \
    | grep -viE "_demo|opus_compare|repacketizer_demo|trivial_example|mlp_train|/tests/")
  oi=0
  for f in $OPUS_SRCS; do
    "$CC" -O2 -fPIC -std=c11 $OPUS_DEF $OPUS_INC -c "$f" -o "third_party/opus_obj/o$oi.o" || { echo "opus compile failed: $f"; exit 1; }
    oi=$((oi+1))
  done
  "$AR" rcs third_party/libopus.a third_party/opus_obj/*.o
  echo "[0/5] libopus.a built ($oi objects)"
fi

IMGUI=third_party/imgui
OPENXR_INC=third_party/OpenXR-SDK/include

INCLUDES="-I. -Imei -I$IMGUI -I$IMGUI/backends -I$OPENXR_INC -I$OPUS/include"
# -static-libstdc++ bundles libc++ INTO libpavchams.so so the injected lib has no libc++_shared.so
# runtime dependency (nothing extra to ship in the APK).
# EXTRA_CXXFLAGS lets you tack on extra compile flags without editing this file.
CXXFLAGS="-std=c++17 -O2 -fPIC -fvisibility=hidden -static-libstdc++ -DXR_USE_GRAPHICS_API_VULKAN=1 $INCLUDES ${EXTRA_CXXFLAGS:-}"

SRCS=(
  pavchams.cpp
  audioshim.cpp
  voice_opus.cpp
  mei/mei_settings.cpp
  mei/mei_menu.cpp
  mei/mei_input.cpp
  mei/mei_xr.cpp
  "$IMGUI/imgui.cpp"
  "$IMGUI/imgui_draw.cpp"
  "$IMGUI/imgui_tables.cpp"
  "$IMGUI/imgui_widgets.cpp"
  "$IMGUI/backends/imgui_impl_vulkan.cpp"
)

# audioshim.cpp (the soundboard) is compiled INTO libpavchams.so — libOpenSLES.so is a public system
# lib so it can't be replaced by APK name; pavchams GOT-hooks slCreateEngine from inside instead.
echo "[1/5] compiling libpavchams.so (mod + soundboard + mei menu + imgui)"
"$CLANG" $CXXFLAGS -shared -o libpavchams.so "${SRCS[@]}" third_party/libopus.a -llog -lvulkan -ldl -lm -laaudio

# remove any stale standalone soundboard libs from the old name-swap approach so repack won't bundle them.
rm -f libOpenSLES.so libOpenSLE2.so

# ---- [1c/5] steam persona shim: steamshim.cpp -> libsteam_api.so (fake name/pfp/SteamID) ----
# DT_NEEDEDs the real lib (libsteam_ap2.so, already soname-patched) so unfaked symbols forward through.
if [ -f steamshim.cpp ] && [ -f libsteam_ap2.so ]; then
  echo "[1c/5] compiling libsteam_api.so (persona shim)"
  "$CLANG" -std=c++17 -O2 -fPIC -fvisibility=hidden -shared -Wl,-soname,libsteam_api.so \
    -o libsteam_api.so steamshim.cpp -Ithird_party/stb \
    -Wl,--no-as-needed ./libsteam_ap2.so -Wl,--as-needed -llog -ldl
fi

echo "[2/5] repacking APK (repack.py)"
python repack.py

echo "[3/5] zipalign + sign"
rm -f Pavlov-EOS-aligned.apk Pavlov-EOS-signed.apk
"$BUILDTOOLS/zipalign.exe" -f -p 4 Pavlov-EOS-unsigned.apk Pavlov-EOS-aligned.apk
"$BUILDTOOLS/apksigner.bat" sign --ks "$KEYSTORE" --ks-pass "pass:$KS_PASS" \
  --key-pass "pass:$KS_PASS" --ks-key-alias "$KS_ALIAS" \
  --out Pavlov-EOS-signed.apk Pavlov-EOS-aligned.apk

echo "[4/5] installing"
adb install -r Pavlov-EOS-signed.apk

echo "[5/5] done."
echo "In-headset: point a controller UP (~55 deg) for ~0.6 s to open 'mei mei [private]'."
echo "Aim the ray at a control and hold still ~0.7 s to click (dwell). Settings persist to mei.cfg."
