#!/usr/bin/env bash
# Build libpavchams.so (mod + "mei mei [private]" ImGui VR menu), repack the APK, align, sign, and
# install to a connected Quest. Edit the paths below for your machine, then: ./build.sh
set -e

# ---- EDIT THESE ----
# Auto-pick the newest installed NDK under the SDK (r26/r27/r28 all work); override NDK to pin one.
SDK_ROOT="${SDK_ROOT:-/c/Users/${USERNAME:-$USER}/AppData/Local/Android/Sdk}"
NDK="${NDK:-$(ls -d "$SDK_ROOT"/ndk/* 2>/dev/null | sort -V | tail -1)}"
BUILDTOOLS="${BUILDTOOLS:-$(ls -d "$SDK_ROOT"/build-tools/* 2>/dev/null | sort -V | tail -1)}"
KEYSTORE="ratkey.jks"           # your signing keystore (generate with keytool; not committed)
KS_PASS="ratman4080"            # store + key password
KS_ALIAS="rat"                  # key alias
IMGUI_TAG="v1.90.9"             # pinned Dear ImGui (backend API in mei_xr.cpp targets this)
OPENXR_TAG="release-1.1.36"     # Khronos OpenXR-SDK (headers only)
# --------------------

CLANG="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang++.cmd"
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

IMGUI=third_party/imgui
OPENXR_INC=third_party/OpenXR-SDK/include

INCLUDES="-I. -Imei -I$IMGUI -I$IMGUI/backends -I$OPENXR_INC"
# -static-libstdc++ bundles libc++ INTO libpavchams.so so the injected lib has no libc++_shared.so
# runtime dependency (nothing extra to ship in the APK).
CXXFLAGS="-std=c++17 -O2 -fPIC -fvisibility=hidden -static-libstdc++ -DXR_USE_GRAPHICS_API_VULKAN=1 $INCLUDES"

SRCS=(
  pavchams.cpp
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

echo "[1/5] compiling libpavchams.so (mod + mei menu + imgui)"
"$CLANG" $CXXFLAGS -shared -o libpavchams.so "${SRCS[@]}" -llog -lvulkan -ldl

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
