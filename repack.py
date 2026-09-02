#!/usr/bin/env python3
# Repack Pavlov-Shim-signed.apk:
#   - replace lib/arm64-v8a/libEOSSDK.so  with the EOS interposer wrapper
#   - add     lib/arm64-v8a/libEOSDK.so   (the genuine SDK, renamed, soname-patched)
#   - drop the old v1 META-INF signature so apksigner can re-sign cleanly
# Output: Pavlov-EOS-unsigned.apk  (then: zipalign -> apksigner)
import zipfile, os

B   = os.path.dirname(os.path.abspath(__file__)) + "/"
src = r"C:/Users/lodge/Documents/pavlov-quest/Pavlov-Shim-signed.apk"
out = B + "Pavlov-EOS-unsigned.apk"

wrap_b = open(B + "libEOSSDK.so", "rb").read()   # -> lib/arm64-v8a/libEOSSDK.so
real_b = open(B + "libEOSDK.so",  "rb").read()   # -> lib/arm64-v8a/libEOSDK.so
steam_wrap_b = open(B + "libsteam_api.so", "rb").read()   # persona wrapper -> lib/arm64-v8a/libsteam_api.so
steam_real_b = open(B + "libsteam_ap2.so", "rb").read()   # real Steam renamed -> lib/arm64-v8a/libsteam_ap2.so

# soundboard shim (optional): our libOpenSLES.so wrapper + the real platform lib renamed libOpenSLE2.so.
def _opt(p):
    try:  return open(B + p, "rb").read()
    except FileNotFoundError: return None
audio_wrap_b = _opt("libOpenSLES.so")   # audioshim wrapper -> lib/arm64-v8a/libOpenSLES.so
audio_real_b = _opt("libOpenSLE2.so")   # real platform lib renamed -> lib/arm64-v8a/libOpenSLE2.so
audio_swapped = False

zin  = zipfile.ZipFile(src, "r")
zout = zipfile.ZipFile(out, "w")
for zi in zin.infolist():
    name = zi.filename
    if name.upper().endswith((".SF", ".RSA", ".DSA")) and name.startswith("META-INF/"):
        continue
    if name == "META-INF/MANIFEST.MF":
        continue
    if name == "lib/arm64-v8a/libEOSSDK.so":
        ni = zipfile.ZipInfo("lib/arm64-v8a/libEOSSDK.so")
        ni.compress_type = zipfile.ZIP_DEFLATED
        ni.external_attr = zi.external_attr
        zout.writestr(ni, wrap_b)
        continue
    if name == "lib/arm64-v8a/libOpenSLES.so" and audio_wrap_b is not None:
        # swap the platform OpenSLES for OUR soundboard wrapper (mic-inject). libUnreal DT_NEEDEDs
        # libOpenSLES.so; the wrapper dlopen()s libOpenSLE2.so (real, renamed + soname-patched, added below).
        ni = zipfile.ZipInfo("lib/arm64-v8a/libOpenSLES.so")
        ni.compress_type = zipfile.ZIP_DEFLATED
        ni.external_attr = zi.external_attr
        zout.writestr(ni, audio_wrap_b)
        audio_swapped = True
        continue
    if name == "lib/arm64-v8a/libsteam_api.so":
        # swap the real Steam lib for OUR persona wrapper (fake ISteamFriends::GetPersonaName).
        # libUnreal DT_NEEDEDs libsteam_api.so; the wrapper DT_NEEDEDs libsteam_ap2.so (real, added below).
        ni = zipfile.ZipInfo("lib/arm64-v8a/libsteam_api.so")
        ni.compress_type = zipfile.ZIP_DEFLATED
        ni.external_attr = zi.external_attr
        zout.writestr(ni, steam_wrap_b)
        continue
    if name == "AndroidManifest.xml":
        ni = zipfile.ZipInfo("AndroidManifest.xml")
        ni.compress_type = zi.compress_type
        ni.external_attr = zi.external_attr
        zout.writestr(ni, open(B + "AndroidManifest.xml", "rb").read())
        continue
    if name == "assets/UECommandLine.txt":
        ni = zipfile.ZipInfo("assets/UECommandLine.txt")
        ni.compress_type = zi.compress_type
        ni.external_attr = zi.external_attr
        zout.writestr(ni, open(B + "UECommandLine.txt", "rb").read())
        continue
    if name == "classes.dex":
        ni = zipfile.ZipInfo("classes.dex")
        ni.compress_type = zipfile.ZIP_DEFLATED
        ni.external_attr = zi.external_attr
        zout.writestr(ni, open(B + "classes_patched.dex", "rb").read())
        continue
    data = zin.read(name)
    ni = zipfile.ZipInfo(name)
    ni.compress_type = zi.compress_type
    ni.external_attr = zi.external_attr
    ni.date_time     = zi.date_time
    zout.writestr(ni, data)

ri = zipfile.ZipInfo("lib/arm64-v8a/libEOSDK.so")
ri.compress_type = zipfile.ZIP_DEFLATED
zout.writestr(ri, real_b)

# add the real Steam lib under its patched soname; the persona wrapper (libsteam_api.so) DT_NEEDEDs it.
si = zipfile.ZipInfo("lib/arm64-v8a/libsteam_ap2.so")
si.compress_type = zipfile.ZIP_DEFLATED
zout.writestr(si, steam_real_b)

# soundboard: add the real platform lib under its patched soname, and add our wrapper if the base
# APK didn't already carry lib/arm64-v8a/libOpenSLES.so (system libs often aren't bundled).
if audio_real_b is not None:
    ai = zipfile.ZipInfo("lib/arm64-v8a/libOpenSLE2.so")
    ai.compress_type = zipfile.ZIP_DEFLATED
    zout.writestr(ai, audio_real_b)
if audio_wrap_b is not None and not audio_swapped:
    aw = zipfile.ZipInfo("lib/arm64-v8a/libOpenSLES.so")
    aw.compress_type = zipfile.ZIP_DEFLATED
    zout.writestr(aw, audio_wrap_b)

# add the chams lib; libEOSSDK.so's constructor dlopen()s it (no patchelf on libUnreal)
chams_b = open(B + "libpavchams.so", "rb").read()
ci = zipfile.ZipInfo("lib/arm64-v8a/libpavchams.so")
ci.compress_type = zipfile.ZIP_DEFLATED
zout.writestr(ci, chams_b)

# add androidx.browser (customtabs) as a secondary dex so EOSSDK.PrewarmURL / the
# Epic-account login browser can resolve CustomTabColorSchemeParams at runtime.
dex2 = open(B + "miniout/classes.dex", "rb").read()
di = zipfile.ZipInfo("classes2.dex")
di.compress_type = zipfile.ZIP_DEFLATED
zout.writestr(di, dex2)

zin.close(); zout.close()

z = zipfile.ZipFile(out)
print("EOS libs:", [n for n in z.namelist() if "EOS" in n and n.startswith("lib/arm64")])
print("audio libs:", [n for n in z.namelist() if "OpenSLE" in n and n.startswith("lib/arm64")])
print("residual v1 sig:", any(n.upper().endswith((".RSA", ".SF")) for n in z.namelist()))
print("output:", out, os.path.getsize(out), "bytes")
