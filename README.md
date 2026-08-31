# libpavchams — Pavlov VR (Steam Frame build) internal mod for Quest

An internal UE5.1 mod library for the **Pavlov "Steam Frame"** build (`com.vankrupt.pavlov`,
arm64 Android / Quest). It self-resolves the engine at runtime on a fully-stripped
`libUnreal.so` (no symbols) and drives the game through UObject reflection — chams, ESP,
aim assist, client-authoritative hit-reg kills (trigger-kill / kill-all / **pick-a-player**),
gun/movement tweaks, a TTT buy menu, a runtime SDK dumper, and a client-side name changer.

> **Scope / disclaimer.** This is a reverse-engineering + game-hacking learning project for
> **your own headset and offline/private testing**. Multiplayer cheating ruins games for real
> people and gets you banned — don't. Several features are intentionally limited by the server
> (see [Server-authoritative ceiling](#server-authoritative-ceiling)); that's by design and we
> don't try to defeat anti-cheat, ban evasion, or other players' moderation.

---

## What it does

Controlled by a single file `chams.txt` (see [Config](#config)). Everything is written on
the game thread from a hooked `ProcessEvent`.

| Feature | Notes | Online? |
|---|---|---|
| **Chams / wallhack** | Force-loads the game's own `M_PlayerXRay` material onto enemy body meshes. **Styles:** Team, Single-A/B, Flash (pulse), Target-only, **Custom** (per-team RGB via a dynamic material instance). Aim-target highlighted, skips corpses. | ✅ (client-side render) |
| **ESP overlay** | 2D box / name / distance / health bar / TTT role over each enemy, crosshair dot, aimbot FOV circle. Projected with the game's own camera. | ✅ |
| **Silent aim** | Snaps the gun toward the enemy head. Runs late-frame (`ReceiveDrawHUD`) so the rotation replicates — that's what lands it online. FFA vs team auto-detected; **Team-check off = target everyone**. | ✅ |
| **Trigger-kill** | On each real shot, reports a headshot on your aim target via `ServerReportBulletHit` (client-authoritative hit-reg). Lands **weaponless** too. | ✅ |
| **Kill All** | On-fire: headshots every *loaded* enemy on the bullet. Detectable — use sparingly. | ✅ (loaded enemies) |
| **Target kill** *(pick-a-player)* | Live player list by name in the Weapon tab — pick one, **KILL SELECTED**. Re-resolves the target fresh by name each press (crash-safe). White = loaded/killable, grey = too far. | ✅ (loaded enemies) |
| **No-recoil + perfect accuracy** | Zeroes recoil/spread floats on the held gun. | ✅ |
| **Rapid / auto fire, no-reload, infinite ammo** | Fire-rate config + `FireMode` = Automatic; reload/ammo tweaks. | offline; server may cap |
| **Movement** | Boosts `MaxWalkSpeed` / sprint / ADS / **crouch** on `PavlovMovementComponent`; optional **noclip** (fly + collision off, offline). | client-predicted |
| **Anti-flash** | Zeroes the flashbang blind curve on `GlobalPlayerEffects`. | ✅ (client render) |
| **TTT buy menu** | `ServerBuy(FName)` with a categorized + searchable weapon list (exact Pavlov equipment IDs). | ✅ where buying is on |
| **Godmode / homing knife / name changer / dev tag** | Server-authoritative or client-view-only — see [Server ceiling](#server-authoritative-ceiling). **⚠️ Name changer / dev tag / noclip crash the game ONLINE** (replicated writes) — offline only. | offline only |
| **SDK dumper** | Walks all `UClass`/`ScriptStruct` → `name : super + props(+offset,type) + funcs` in `sdk_dump.txt`. | tool |

---

## mei mei [private] — in-headset menu

Primary control is now an in-VR **Dear ImGui** panel, **mei mei [private]**, rendered as an OpenXR
quad layer and driven by your controller. No more adb for day-to-day toggling.

- **Open/close:** press in the **left thumbstick (L3)**.
- **Cursor:** aim the controller at the panel. **Click:** the **right trigger**.
  *(If bindings need tuning on first boot, it auto-falls-back to up-point-to-open + dwell-to-click.)*
- **Tabs:** Aimbot · Visuals · Weapon · Movement · Player · TTT · Config — every feature has its own
  toggle and sliders (aim FOV/mode, cham style + custom colors, movement multipliers, ESP, buy list…).
- Settings persist to `.../files/mei.cfg`. Master enable is on by default so the mod arms itself.

### How to kill people (for your friends)

Hit-reg in Pavlov is **client-authoritative** (`ServerReportBulletHit`), so kills land **even with no
weapon in hand** (the report supplies its own gun class — shows as a Hunting Rifle in the feed).

- **One specific player:** Weapon tab → **TARGET KILL** → tap a **white** name → **KILL SELECTED**.
  White = loaded/killable; grey = too far / not streamed in (can't be reported on — trying crashes the
  netcode, so it's locked out).
- **Whoever you point at:** turn on **Trigger kill**, aim, pull the trigger.
- **Everyone loaded:** **Kill All** (Weapon tab) on your next shot — detectable, use sparingly.

> ### ⚠️ Leave these OFF online — they crash the game
> **Name changer, Dev tag, and Noclip** write **replicated** properties (`PlayerNamePrivate`, `bDev`,
> flying `MovementMode`). The server re-serializes them on a net tick and the game **hard-crashes**
> (`SIGSEGV` in the netcode, uncatchable). They're **offline-only**. Name changer especially: if your
> name is short enough to fit the buffer it *will* write and *will* crash you online.

Architecture + first-pixel bring-up: **[docs/MEI-MENU.md](docs/MEI-MENU.md)** and
**[docs/ON-DEVICE.md](docs/ON-DEVICE.md)**. The old `chams.txt` integer still works as a fallback
(and `8`/`9` still force the one-shot dumps from adb).

## Config (legacy fallback)

Edit `/sdcard/Android/data/com.vankrupt.pavlov/files/chams.txt` (single integer):

| Value | Mode |
|---|---|
| `0` | off |
| `2` | chams + no-recoil + accuracy |
| `3` | + silent aim (on-fire) |
| `4` | + rapid/auto fire, movement, godmode |
| `6` | + **continuous online silent aim** (recommended for online) |
| `8` | one-shot **whitelist dump** (`WHITELIST` log lines) |
| `9` | one-shot **SDK dump** → `sdk_dump.txt` |

**Name changer:** put a name in `.../files/name.txt` (e.g. `6yawn`). Applied client-side in a
match. Keep it short (~≤6 chars) — see [Name changer](#name-changer-detail).

```sh
adb shell 'echo 6 > /sdcard/Android/data/com.vankrupt.pavlov/files/chams.txt'
printf '6yawn' | adb shell 'cat > /sdcard/Android/data/com.vankrupt.pavlov/files/name.txt'
```

---

## Install on Quest 3

The ready-to-run build is **`Backups/2026-08-29/Pavlov-Frame-chams.apk`** (same file as
`Pavlov-Frame-chams.apk` in the repo root). Grab it from the backup folder — pull it out with any
APK extractor / file manager, or just `adb install` it directly.

This is the Pavlov **Steam Frame** build (`com.vankrupt.pavlov`), which is an Android/arm64 VR build
originally targeting the Steam Frame headset. It runs on **Quest 3** because the APK carries an
**OpenXR interaction-profile shim** (an `xrGetInstanceProcAddr` interposer) that remaps the Steam
Frame controller profile to the Quest **Touch Plus** profile — so aim/grip/trigger/stick all bind
correctly on Quest. No native Meta/Oculus libs are involved (which is also why it never trips Meta
device-attestation — see `docs/EOS-PORT.md`).

```sh
adb install -r Pavlov-Frame-chams.apk
# then push the game content (OBBs) — REQUIRED, ~13 GB, shared separately (too big for GitHub):
adb push main.<ver>.com.vankrupt.pavlov.obb \
  /sdcard/Android/obb/com.vankrupt.pavlov/
# set the cheat mode:
adb shell 'echo 6 > /sdcard/Android/data/com.vankrupt.pavlov/files/chams.txt'
```

Without the OBBs the app installs but won't launch (Pavlov loads its content from `main.obb`).

## Libraries & load order

What's in the APK's `lib/arm64-v8a/` and how it all chains together at launch:

```
Android starts com.epicgames.unreal.GameActivity
   └─ loads libUnreal.so            ← the whole game (stripped UE5.1 monolith, no symbols)
        │  DT_NEEDED: "libEOSSDK.so" (the game thinks this is the EOS SDK)
        └─ loads libEOSSDK.so       ← OUR interposer wrapper (NOT the real SDK)
             │  __attribute__((constructor)) on_load():
             │    ├─ dlopen("libpavchams.so", RTLD_GLOBAL)   ← loads THE MOD (this repo)
             │    └─ dlopen("libEOSDK.so")                    ← the genuine EOS SDK
             │  Interposes EOS_*: defines a few (EOS_Connect_Login, EOS_Platform_Create …),
             │  forwards everything else to libEOSDK.so via dlsym. Does anonymous Device-ID
             │  auto-login so you get an EOS ProductUserId online with no Steam/Epic/Meta account.
             └─ (separately) the OpenXR loader shim remaps Frame→Touch Plus controller bindings
```

- **`libUnreal.so`** — the game engine + all Pavlov code, one stripped monolith. We never modify
  it; we reflect into it at runtime. (Game IP — not in this repo.)
- **`libEOSSDK.so`** — *our* wrapper (`eosshim.cpp`). The game links it by name as the EOS SDK, so
  its `constructor` runs automatically at load — that's our injection point. It (a) `dlopen`s the
  mod, (b) `dlopen`s the real SDK renamed `libEOSDK.so`, (c) interposes EOS calls for the
  Device-ID login. **This is how the mod gets loaded without patching `libUnreal`.**
- **`libEOSDK.so`** — the genuine Epic Online Services SDK, renamed from `libEOSSDK.so` and
  soname-patched to `libEOSDK.so` (so our wrapper can take the original name). (Epic IP — not in repo.)
- **`libpavchams.so`** — *the mod* (`pavchams.cpp`). Once `dlopen`'d, it spins a background thread,
  self-resolves the engine, inline-hooks `ProcessEvent`, and runs the feature passes. See below.
- **OpenXR shim lib** — the interaction-profile interposer that makes the Frame build's controllers
  work on Quest (Frame profile → Touch Plus). Part of the Frame port already baked into the APK.
- **`classes_patched.dex` / `classes2.dex`** — Java side: a one-method dex patch + an
  `androidx.browser.customtabs` stub the Frame build's Epic-login browser path was missing.

Full port/auth write-up (EOS Device-ID login, OBB version notes, how the wrapper is built and
soname-patched): **[docs/EOS-PORT.md](docs/EOS-PORT.md)**.

## How the mod works (architecture)

`pavchams.cpp` is a single translation unit, loaded into the game process (see
[Loading](#loading)). On a background thread it:

1. **Self-maps** its own readable/writable regions from `/proc/self/maps`.
2. **Fault guard** — a thread-local `sigsetjmp`/`siglongjmp` handler for SIGSEGV/BUS/etc. Every
   risky memory walk sets `g_fguard` and bails on fault instead of crashing.
3. **Finds `GObjects`** (`FChunkedFixedUObjectArray`) by structural validation (object & class
   vtables must point into `libUnreal .text`).
4. **Finds `GNames`** (`FNamePool`) and auto-tunes the FName header shift (=6 on this build).
5. **Finds `ProcessEvent`** by vtable index 77 / alloca signature, then **inline-hooks** it
   (16-byte `LDR x17; BR x17` patch + relocating trampoline).
6. From the hook, runs the feature passes on the game thread with re-validation.

Reflection helpers (`find_class`, `find_func`, `prop_offset`, `is_a`, FName encode/decode) let
you resolve anything **by name** at runtime, so the code survives game updates without hardcoded
offsets. UE5.1 layout constants (UObject/UStruct/FField offsets, FName shift, ProcessEvent vtable
idx) are `#define`d at the top of `pavchams.cpp`.

The chams material trick, aim, etc. are ports of a PC-VR PostRender reference — see inline
comments. The **key online-aim insight**: rotation must be written *late in the frame*
(`ReceiveDrawHUD`) so the motion controller doesn't overwrite it before replication.

---

## Server-authoritative ceiling

Pavlov is server-authoritative for gun/health state. **Client-side visual/simulation** (chams,
movement prediction, aim rotation that replicates) works online; **state the server owns**
(health, ammo counts, real player name, admin status) is validated server-side and can't be forced
from the client. Documented limits:

- **Hit registration is the exception** — `ServerReportBulletHit` is *client-authoritative*, so
  trigger-kill / target-kill / kill-all land online (even weaponless). BUT the server only accepts a
  report for a **rendered/loaded** pawn — reporting an un-streamed far player crashes the netcode on a
  deferred tick, so those are skipped (grey in the list).
- **Replicated-write features CRASH online** — writing a property the server replicates and
  re-serializes (`PlayerNamePrivate` = **name changer**, `bDev` = **dev tag**, flying `MovementMode` =
  **noclip**) hard-crashes the game (`SIGSEGV` deep in the net serializer, on a later tick, so our
  fault-guard can't catch it). These are **offline-only** — keep them off in a live match.
- **Godmode / infinite ammo** — server tracks real values → offline only.
- **Real name change** (`ServerChangeName`) — server forces the account name back (anonymous
  Device-ID auth = name `"null"`). The client-side in-place write is view-only, short-names-only, and
  only for **custom maps that gate perks by a client-side name check**.
- **Anti-votekick / admin / VIP tied to your account** — server-side, not client-reachable.

We don't attempt anti-cheat bypass, ban evasion, or forced disconnect blocking.

### Name changer detail
The server-side name is locked to the account (`"null"` on anonymous auth). The mod instead
writes `PlayerState::PlayerNamePrivate` **in place** in the existing FString buffer — so it only
fits **short** names (the anonymous name buffer is ~5–7 chars) and is **client-view only**
(others see your real name unless you host). Replacing the FString pointer with our own buffer
**crashes** (the engine frees it with its own allocator) — don't; the in-place write is the safe
path. Custom maps that gate VIP/roles by a client-side name check will honor it.

### Real server-visible name — the fake Steam persona (`steamshim.cpp`) ✅
The in-place write above is client-view only, so on the Frame build your **real** name stays `"null"`
(the anonymous Device-ID has no Steam persona — Steam is dormant on Quest) and admins blanket-ban
`null` as the sideloader tell. The fix: **give the game's own name pipeline a name** so it replicates
to the server legitimately. `steamshim.cpp` ships **as `libsteam_api.so`** (real renamed
`libsteam_ap2.so`, soname-patched; `libUnreal` `DT_NEEDED`s it — wrapped exactly like the EOS shim)
and fakes just enough of the Steam client for `ISteamFriends::GetPersonaName` to return your name:

1. `SteamInternal_SteamAPI_Init` → OK (this is the init the game actually calls — **not** `SteamAPI_Init`).
2. `SteamInternal_ContextInit` → we run the populate callback ourselves (the real one bails when Steam
   isn't truly up, so it never fills the interface table).
3. `SteamInternal_FindOrCreateUserInterface` → a fake **logged-in `ISteamUser`** (`BLoggedOn=true` +
   valid SteamID, which breaks the game's login-wait spin) and a fake **`ISteamFriends`** whose
   `GetPersonaName` (vtable[0]) returns your name; every other interface = a null-safe stub object.

Set the name in `/sdcard/Android/data/com.vankrupt.pavlov/files/**persona.txt**`, restart (read at
Steam-init on launch). EOS Device-ID auth is untouched (the EOS shim still rewrites Steam→Device-ID),
no crash. Rotate `persona.txt` + hit **New identity** (fresh anonymous PUID, via the EOS shim's
`newid.txt` flag) and you're a completely different player each time — dodges `null`/name/id admin bans.

> Build: compile `steamshim.cpp` → `libsteam_api.so`, soname-patch the real lib → `libsteam_ap2.so`,
> then repack the APK swapping `lib/arm64-v8a/libsteam_api.so` (wrapper) + adding `libsteam_ap2.so`
> (both `ZIP_STORED`), zipalign `-p 4`, re-sign. Same wrap/soname-patch pattern as the EOS shim.

---

## Build

Requires **Android NDK r27** (`aarch64-linux-android29-clang++`) and Android **build-tools**
(`zipalign`, `apksigner`). See [`build.sh`](build.sh) — edit the paths at the top, then:

```sh
./build.sh          # compile libpavchams.so, repack APK, zipalign, sign, adb install
```

Or manually:

```sh
# 1. the mod
aarch64-linux-android29-clang++ -std=c++17 -O2 -fPIC -shared -o libpavchams.so pavchams.cpp -llog

# 2. the EOS shim (only needed if rebuilding the loader — see docs/EOS-PORT.md)
aarch64-linux-android29-clang++ -std=c++17 -O2 -fPIC -fvisibility=hidden -shared \
  -Wl,-soname,libEOSSDK.so -o libEOSSDK.so eosshim.cpp \
  -Wl,--no-as-needed ./stub_libEOSDK.so -Wl,--as-needed -llog -ldl

# 3. repack + align + sign  (repack.py writes lib/arm64-v8a/libpavchams.so into the APK)
python repack.py
zipalign -f -p 4 Pavlov-EOS-unsigned.apk Pavlov-EOS-aligned.apk
apksigner sign --ks YOURKEY.jks --ks-pass pass:... --key-pass pass:... \
  --ks-key-alias ... --out Pavlov-EOS-signed.apk Pavlov-EOS-aligned.apk

# 4. install
adb install -r Pavlov-EOS-signed.apk
```

### Loading
`libpavchams.so` is loaded by the EOS shim's constructor (`libEOSSDK.so` `dlopen`s it) — no
patchelf on `libUnreal`. The EOS shim also provides anonymous Device-ID online auth. Full loader
/ EOS-port write-up: **[docs/EOS-PORT.md](docs/EOS-PORT.md)**.

---

## Repo layout

**In the repo (our source):**
- `pavchams.cpp` — the mod (all features + engine self-resolution).
- `mei/` — the **mei mei [private]** VR ImGui menu: `mei_settings` (state + `mei.cfg`), `mei_menu`
  (tabs/UI), `mei_input` (controller-ray cursor), `mei_xr` (OpenXR quad-layer + Vulkan backend).
- `eosshim.cpp`, `stub.c`, `minisrc/` — the EOS interposer / loader source.
- `repack.py` — APK repacker.
- `build.sh` — one-shot build/sign/install.
- `docs/EOS-PORT.md` — how the Quest port + EOS Device-ID auth works.
- `Backups/` — dated source snapshots.

**You provide (NOT committed — copyrighted game files / secrets, see `.gitignore`):**
- The Pavlov Frame APK + OBBs (bring your own dump).
- `libEOSDK.so` (genuine EOS SDK), `classes_patched.dex`, etc.
- Your own signing keystore (`keytool -genkey ...`).
- `*.pak` custom maps, `sdk_dump*.txt`.

---

## Contributing

The reflection API makes new features easy — no offset hunting:

```cpp
void* cls  = find_class("VRGun");                  // a UClass by name
int32_t off = prop_offset(cls, "FireRate");        // a property offset by name (walks supers)
void* fn   = find_func(cls, "K2_SetActorRotation");// a UFunction by name
g_ProcessEvent(actor, fn, &params);                // call it
```

Workflow to add a feature:
1. `chams.txt = 9`, load a match, pull `sdk_dump.txt` — find the class/property/function you need.
2. Resolve it by name (above), read/write the field or call the function on the game thread.
3. **Always** guard risky reads with the fault guard (`g_fguard`), validate pointers
   (`addr_readable`, `in_lib`), and cache per-class offsets (don't decode FNames per frame).
4. Gate it behind a `chams.txt` value in `chams_pass` / the handler.

Gotchas learned the hard way (there are inline comments too):
- Don't write in a full-object scan **per ProcessEvent** — it's ~20k objects/ call = 1 fps.
- Don't replace an FString/GC-owned pointer with your own buffer — the engine frees it → crash.
- Mobile forward renderer ignores `SetOverlayMaterial` and won't draw `TextRenderComponent` (no
  in-world text UI *through reflection*). The **mei menu** sidesteps this entirely: it draws with its
  own Vulkan ImGui backend into an **OpenXR quad layer** (`mei/mei_xr.cpp`), never touching the
  engine's renderer — the same reason chams (material swap) works but text didn't.
- Re-validate cached world/pawn pointers across map changes (they go stale → silent no-ops).

PRs welcome. Keep it single-file-friendly and comment the offsets/mechanisms you use.

---

## Credits
- EOS Device-ID auth + Quest port: see `docs/EOS-PORT.md`.
- Chams / silent-aim technique adapted from a community PC-VR PostRender reference.
