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
- `ratkey.jks` : signing keystore.  **storepass / keypass = `ratman4080`,  alias = `rat`**
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
apksigner sign --ks ratkey.jks --ks-pass pass:ratman4080 --key-pass pass:ratman4080 \
  --ks-key-alias rat --out Pavlov-EOS.apk Pavlov-EOS-aligned.apk
# 3. install (keeps OBB) + push OBBs if needed
adb install -r Pavlov-EOS.apk
```

## Original untouched sources
Frame APKs + keystore + xrshim: `C:\Users\lodge\Documents\pavlov-quest\`
OBB backups (13.5GB): `C:\Program Files (x86)\Steam\steamapps\content\app_3504270\depot_3504271\obb\`

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
zipalign -f 4 ... ; apksigner sign --ks ratkey.jks ... (pass ratman4080, alias rat)
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

===============================================================================
# SESSION 3 — player-hosted lobby kick ("Device Cannot Be authenticated")
===============================================================================

## Symptom
Dedicated servers: fine. **Every player-hosted (listen-server) lobby** kicks ~6s after
join with a host->client `ClientWasKicked(KickReason="Device Cannot Be authenticated")`.

## Diagnosis
The host makes the decision, in the host's process, during the EOS control-channel
handshake — it inspects the joining player's Connect PUID. Our PUID is an **anonymous
Device-ID** account: zero linked external accounts. Player-hosted hosts reject that
("Device"); dedicated servers skip the check. **Client-side readback spoofing cannot beat
this** — the identity token we send is signed by Epic and resolves to an anonymous account
no matter what any local hook rewrites. Dropping the `ClientWasKicked` RPC does nothing:
the host already severed the NetConnection; the RPC is only the notice (this is why the old
anti-votekick RPC-drop was removed).

Earlier "Epic login already tried" = the on-headset **Auth login flow never completed**
(AccountPortal browser path is half-stubbed), so it fell back to Device-ID and got kicked.
A real Epic identity was never actually put in front of a host. So the premise is untested,
not disproven.

## The fix path (staged): prove, then ship
The shim (`eosshim.cpp`) now selects an identity strategy from a text file, no recompile:
```
adb shell "echo device                  > /sdcard/Android/data/com.vankrupt.pavlov/files/eosauth.txt"  # anon (current default)
adb shell "echo portal                  > .../eosauth.txt"   # real Epic via in-headset browser (shippable)
adb shell "echo persistent              > .../eosauth.txt"   # silent Epic, after one portal login
adb shell "echo dev:127.0.0.1:6547:rat  > .../eosauth.txt"   # DevAuthTool over adb reverse (THE TEST)
```
On boot the shim reads it and runs `EOS_Auth_Login` -> `EOS_Auth_CopyIdToken` ->
`EOS_Connect_Login(type=EPIC_ID_TOKEN)`. Every step's result code is logged
(`adb logcat -s EOSSHIM`); any failure falls back to Device-ID so you stay online.

### Decisive test (do this FIRST — ~30 min, no browser work)
Proves whether a genuine Epic PUID passes the host check before investing in the portal.
1. PC: download the **EOS DevAuthTool** (in the EOS SDK `Tools/` zip from dev.epicgames.com).
2. Run it, pick a free port (e.g. 6547), log in with any **free Epic account**, and save
   the credential under a name (e.g. `rat`).
3. `adb reverse tcp:6547 tcp:6547`  (so the headset's 127.0.0.1:6547 reaches the PC tool).
4. `adb shell "echo dev:127.0.0.1:6547:rat > /sdcard/Android/data/com.vankrupt.pavlov/files/eosauth.txt"`
5. Launch Pavlov, watch `adb logcat -s EOSSHIM`. Want:
   `EOS_Auth_Login result=0` -> `got Epic ID token` -> `EPIC Connect login result=0 ... non-anonymous`.
6. Join a **player-hosted** lobby.
   - **Stays connected** => identity theory CONFIRMED. Ship the portal (below).
   - **Still kicked "Device..."** => not an account-type check; pivot (see "if it still kicks").

### If the test passes: make it shippable (no PC tethered)
- `echo portal` — finish the AccountPortal browser flow. The `CustomTabColorSchemeParams`
  stub is already added; if `EOS_Auth_Login result` is `IncompatibleVersion`, tune the
  `ApiVersion` consts in `start_epic_login()` (log line `EOS_Initialize ... SDK=<ver>`
  tells the SDK version). After ONE successful portal login the SDK stores a refresh token
  in the app data dir -> switch to `echo persistent` for silent logins thereafter.

### If it still kicks with a real Epic PUID
The check is NOT account-type. Most likely then: EOS anti-cheat (EAC) handshake, an
ownership/entitlement check, or Meta device attestation forwarded by the host. Re-read
logcat around the kick for an EAC/`AntiCheat`/`Sanction` line and pivot there — do NOT
sink time into the browser flow.

## Shim build (unchanged mechanics, now with the Auth path)
```
aarch64-linux-android29-clang -shared -fPIC -Wl,-soname,libEOSDK.so -o stub_libEOSDK.so stub.c
aarch64-linux-android29-clang++ -std=c++17 -O2 -fPIC -fvisibility=hidden -shared \
  -Wl,-soname,libEOSSDK.so -o libEOSSDK.so eosshim.cpp \
  -Wl,--no-as-needed ./stub_libEOSDK.so -Wl,--as-needed -llog -ldl
# then repack.py / zipalign / apksigner as in the Rebuild block above
```