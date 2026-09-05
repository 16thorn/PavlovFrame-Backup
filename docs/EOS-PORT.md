# Pavlov Shack — Steam Frame build on Quest 3 (working notes / backup)

Snapshot of the Frame-build port work, 2026-08-24. Everything here reproduces a
Quest-3-runnable, EOS-authenticated Pavlov Shack **Steam Frame** build.

## Package
- App: `com.vankrupt.pavlov`  (launch activity `com.epicgames.unreal.GameActivity`)
- Frame build version: **1.0.29** (versionCode 2365)
- Source APKs came from Steam depot `download_depot 3504270 3504271` (Frame branch)

## What works
- Runs on Quest 3, controllers via OpenXR interaction-profile shim (Frame profile -> Touch Plus)
- Full content (OBBs restored)
- **EOS online auth** via anonymous Device ID (no Steam, no Epic browser, no Meta entitlement)
- Server browser populated / crossplay lobbies visible (with version override)


## The build stack (files here)
- `eosshim.cpp` -> `libEOSSDK.so` : EOS interposer (the crown jewel). Ships AS libEOSSDK.so;
  real SDK renamed `libEOSDK.so` (soname patched to `libEOSDK.so`) and pulled via DT_NEEDED.
- `libEOSDK.so` : genuine EOS SDK, soname patched from `libEOSSDK.so` -> `libEOSDK.so`.
- `stub_libEOSDK.so` / `stub.c` : tiny lib whose soname is `libEOSDK.so`, only used at link
  time so the wrapper records the right DT_NEEDED.
- `classes_patched.dex` : app dex with one added method (see browser fix).
- `miniout/classes.dex` : minimal `androidx.browser.customtabs.CustomTabColorSchemeParams(+Builder)`
  stub, added as classes2.dex (Frame build's Epic-login browser path was missing it).
- `minisrc/…/CustomTabColorSchemeParams.java` : source for that stub.
- `AndroidManifest.xml` : versionName patched 1.0.29 -> 1.0.28 (turned out NOT to be the
  version source — see OBB note).
- `UECommandLine.txt` : `-ini:` ProjectVersion override attempt (did NOT take; version is in OBB).
- `repack.py` : rebuilds the APK (swaps libEOSSDK.so, adds libEOSDK.so, patched classes.dex,
  classes2.dex, patched manifest + UECommandLine).
- `ratkey.jks` : signing keystore (generate your own with `keytool -genkey`; store/key pass + alias are yours, never commit them).
- `Pavlov-EOS.apk` : the final signed, working Frame build.
- `tools/` : dexlib2 + deps (for the dex patcher), baksmali/smali jars.
- `browser-1.8.0.aar` : androidx.browser source for the customtabs classes.

## How the EOS auth actually works
Pavlov's multiplayer runs on the **EOS Connect ProductUserId (PUID)**. The Frame build feeds
Connect from **Steam**, which can't init on a Quest, so the game never logs in — it just polls
`EOS_Connect_GetLoginStatus` forever. The shim:
1. Hooks `EOS_Platform_Create` -> grabs the Connect interface handle.
2. Hooks `EOS_Connect_GetLoginStatus`; on first `NotLoggedIn`, fires an autonomous login:
   `EOS_Connect_CreateDeviceId` -> `EOS_Connect_Login(type = DEVICEID_ACCESS_TOKEN)`.
3. EOS issues a real PUID; the game's own `AddNotifyLoginStatusChanged` callback fires and the
   game adopts the identity. Anonymous (per-device) — no named account/stats, but you're online.

This shim is **build-agnostic** — it drops onto a Quest build the same way.

## EOS config seen (Frame build)
```
Product    = b6acc1fef71e4c6ab494151c85875d16
Sandbox    = 023fcb0f2b5043c08fe1cda975f7aaf1
Deployment = 770970a45fc24cbc8da3dc41f269bcdf
```

## Version override (to SEE/JOIN live lobbies — cosmetic, doesn't fix netcode)
The runtime version string comes from the cooked ini inside **main.obb**, NOT the manifest and
NOT `-ini`. In `main.2365.com.vankrupt.pavlov.obb` (plaintext, same length so in-place safe):
- offset **183756680** : `VersionDisplayName=1.0.29`
- offset **183764729** : `ProjectVersion=1.0.29`
Patched both to `1.0.28` on-device with `dd ... conv=notrunc`. This made the lobby search ask
for 1.0.28 (live) -> lobbies appeared, joins handshook. Netcode still mismatched -> drops.

## Rebuild
```
# 1. build the EOS shim
aarch64-linux-android29-clang++ -std=c++17 -O2 -fPIC -fvisibility=hidden -shared \
  -Wl,-soname,libEOSSDK.so -o libEOSSDK.so eosshim.cpp \
  -Wl,--no-as-needed ./stub_libEOSDK.so -Wl,--as-needed -llog -ldl
# 2. repack + align + sign
python repack.py
zipalign -f 4 Pavlov-EOS-unsigned.apk Pavlov-EOS-aligned.apk
apksigner sign --ks ratkey.jks --ks-pass pass:YOURPASS --key-pass pass:YOURPASS \
  --ks-key-alias YOURALIAS --out Pavlov-EOS.apk Pavlov-EOS-aligned.apk
# 3. install (keeps OBB) + push OBBs if needed
adb install -r Pavlov-EOS.apk
```

## Original untouched sources
Frame APKs + keystore + xrshim: keep your own local copy (bring your own dump).
OBBs: pulled from your Steam Pavlov Shack install under `steamapps\content\app_3504270\...\obb\`.

## Next: Quest-3 build plan
Take a genuine **Quest** Shack build (netcode matches live), re-sign with ratkey.jks, and bolt on
`libEOSSDK.so` (the EOS Device-ID auto-login) ONLY IF its native auth breaks when sideloaded.
No controller shim needed (native Touch Plus), no version patch (already live version).

## IGNORE Next: Quest-3 build plan
We have tested it further and you will auth, connect, but it will detect your modifying your game files and kick you from the server, this is unpatchable and its from the server.

===============================================================================
# SESSION 2 — full findings (supersedes some notes above)
===============================================================================

## Live caught up -> Frame build now PLAYS on live
Live Quest Shack updated to **1.0.29** (same as the Frame build). Netcode now
matches, so the Frame build (`Pavlov-EOS.apk`) connects to live public servers and
**stays connected** — no more "connection lost". No version patch needed anymore;
keep the OBBs at clean 1.0.29. The "What does NOT work" netcode note above is now moot.

## Two builds, each has half — they can't be merged
- **Frame build (1.0.29, `Pavlov-EOS.apk`)**: plays live, auth via anon Device ID.
  Has NO Oculus platform libs -> does NOT run Meta device-attestation -> never
  integrity-kicked. BUT identity (name/pfp) is Steam-client-served, and a Quest has
  no Steam client -> name/pfp/mic all blank/dead.
- **Quest build (1.0.28, `QuestBuild/Pavlov-Quest-EOS.apk`)**: native Touch Plus,
  native Meta name/pfp/mic. Same EOS Device-ID shim dropped in cleanly and auth works.
  BUT it runs the Meta **HorizonDsat device-integrity** check server-side -> a
  re-signed sideload fails it -> "device integrity check failed" kick. Unpatchable
  from the client (server-verified against Meta). versionCode 2356 = now behind live.
- Can't transplant between them: identity/mic/netcode are compiled into each
  `libUnreal.so` (stripped monolith, no module seam). Frame's binary has **0 `ovr_`
  calls**, so adding Meta libs does nothing.

## Name / pfp / mic — confirmed unreachable on the Frame build (all Steam-client)
- **Name**: pulled at runtime from `ISteamFriends::GetPersonaName` (Steam vtable).
  No Steam client on Quest -> empty. `UPavlovGameUserSettings.DesiredPlayerName`
  exists and I wrote `DesiredPlayerName=cum` into the real internal config (persists),
  but THIS build never applies it to the visible nametag (only the removed settings-UI
  `SetDesiredPlayerName` path did; no console receiver either). Shim also injects the
  EOS lobby `DISPLAYNAME`/`OWNERNAME` attrs = 'cum' (server-browser label only).
- **Mic**: the game itself wipes `VoiceDevice=` back to empty every launch — it
  enumerates zero audio-input devices (Frame desktop-capture path finds nothing on
  Quest). Not a permission issue (RECORD_AUDIO granted + appops allow, confirmed).
- **pfp**: `ISteamFriends::GetSmallFriendAvatar` + `GetImageRGBA` — Steam avatar, none.
- The only way to supply these is to emulate the Steam client (Goldberg-class fake
  `libsteam_api` with correct ISteamFriends/ISteamUser vtables) on arm64 — fragile,
  crash-prone, and risks the working auth. Not worth it for cosmetics.
- The only build with play+name+pfp+mic all working = a **genuinely owned Quest
  store install** (no re-sign -> passes integrity; Meta supplies name/pfp/mic).

## Making the app debuggable (to reach internal config) — `Debuggable/Pavlov-dbg.apk`
Used apktool to add `android:debuggable="true"` so `run-as` works:
```
java -jar apktool.jar d -s -f -o apkdec2 Pavlov-EOS.apk     # -s keeps dex raw
# edit apkdec2/AndroidManifest.xml: add android:debuggable="true" to <application>
java -jar apktool.jar b apkdec2 -o Pavlov-dbg-unsigned.apk
zipalign -f 4 ... ; apksigner sign --ks ratkey.jks ... (your own pass + alias)
```
Then read/write the real internal config (game must be force-stopped first):
```
DST=/data/data/com.vankrupt.pavlov/files/UnrealGame/Pavlov/Pavlov/Saved/Config/Android/GameUserSettings.ini
adb shell run-as com.vankrupt.pavlov cat "$DST"                 # read
adb shell "run-as com.vankrupt.pavlov sh -c 'cat > $DST'" < local.ini   # write (pipe via stdin;
#   run-as is sandboxed from /sdcard, so stdin-pipe is the way in)
```
`Debuggable/GameUserSettings-with-name.ini` = the internal config with
`DesiredPlayerName=cum` + section `[/Script/Pavlov.PavlovGameUserSettings]`.

## Shim additions this session (`eosshim.cpp`, `libEOSSDK.so`)
Beyond the Device-ID auto-login: hooks for `EOS_LobbyModification_AddMemberAttribute`
/ `AddAttribute` (inject username into DISPLAYNAME/OWNERNAME), `EOS_Connect_CopyProductUserInfo`,
`EOS_LobbyDetails_CopyMemberAttributeByIndex` (+ release guards), and lobby-search
`VERSION`/`GAMETYPE` logging. All confirmed firing; none reach the local nametag.

## Folders
- `QuestBuild/` — the 1.0.28 Quest build repacked with the EOS shim (integrity-kicks live, do NOT edit.).
- `Debuggable/` — the debuggable Frame APK + the internal GameUserSettings.ini w/ name.
- Root — the clean playable Frame build (`Pavlov-EOS.apk`) + full shim source/tools.

## Bottom line
The Frame build is a **fully playable** Pavlov Shack on Quest 3 — launches, controllers,
content, auth, live public servers, stays connected. Name / pfp / mic are the
Steam-Frame/account skin a Quest can't wear; they need a real Steam or owned-Meta
identity, not reachable by any shim/config. That's the floor, confirmed from every angle.