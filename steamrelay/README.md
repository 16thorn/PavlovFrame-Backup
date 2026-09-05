# Play normal (official) lobbies on the sideloaded Frame build

Official Pavlov Shack lobbies kick the sideload with **"Unable to authenticate device"** — the
server rejects our anonymous EOS Device-ID identity. The fix is to log in as a **real Steam
account that owns Pavlov Shack**, by relaying a genuine Steam *encrypted app ticket* from your PC to
the headset. No cheats or spoofing involved in this part — it's just presenting your real,
legitimately-owned Steam identity.

Each person runs their **own** relay with their **own** Steam account. You can't share one relay —
the ticket is that account's identity, so two people on one relay would try to be the same player.

---

## What you need

- A **PC** with **Steam installed and logged in** to an account that **owns Pavlov Shack** (appid
  **3504270**) — you don't need it downloaded, just owned.
- The PC and the **Quest on the same network** (same Wi-Fi / LAN).
- The **relay-enabled APK** (shared separately — the normal `Pavlov-Frame-chams.apk` in the repo is
  older and has no relay). Install it with `adb install -r <apk>`.
- `adb` working to your Quest (`adb devices` shows it).

---

## Part 1 — PC relay

Materials:
- `steamrelay/relay.ps1` (in this repo)
- `steam_api64.dll` — grab it from any Steam game you own, e.g.
  `C:\Program Files (x86)\Steam\steamapps\common\<any game>\...\steam_api64.dll`, and drop it in the
  `steamrelay\` folder next to `relay.ps1`.

Steps:
1. Put `steam_api64.dll` in the `steamrelay\` folder. (`relay.ps1` writes `steam_appid.txt` itself.)
2. Make sure **Steam is running and logged in** to the account that owns Shack.
3. Open the folder in PowerShell and run:
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\relay.ps1
   ```
   You should see:
   ```
   [relay] OK SteamID64=765...
   [relay] listening on 0.0.0.0:48010 - Quest fetches from this PC LAN IP
   ```
   If it says `InitFlat` failed → Steam isn't running/logged in, or `steam_api64.dll` is missing.
4. **Open the firewall** for port 48010 (once), in an **admin** PowerShell:
   ```powershell
   New-NetFirewallRule -DisplayName SteamTicketRelay48010 -Direction Inbound -LocalPort 48010 -Protocol TCP -Action Allow
   ```
5. Find your **PC's LAN IP** (you'll need it on the headset):
   ```powershell
   (Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.IPAddress -like '192.168.*' -or $_.IPAddress -like '10.*'}).IPAddress
   ```

Leave the relay window **running** while you play. It mints a fresh ticket per join (Steam limits it
to about one per minute, so it caches for 90s).

---

## Part 2 — headset

Set two files in the app's data folder (replace the IP with **your PC's** LAN IP from step 5):

```sh
adb shell "echo '192.168.1.50:48010' > /sdcard/Android/data/com.vankrupt.pavlov/files/relay.txt"
adb shell "echo x > /sdcard/Android/data/com.vankrupt.pavlov/files/steamlogin.txt"
```

- `relay.txt` — tells the headset where your PC relay is (`<PC-LAN-IP>:48010`).
- `steamlogin.txt` — switches the build to real-Steam login (its presence is the only thing that
  matters).

Then fully close and reopen Pavlov:
```sh
adb shell am force-stop com.vankrupt.pavlov
```
Launch the game and join a normal lobby.

---

## Verify / troubleshoot

Watch the logs while you launch + join:
```sh
adb logcat -c ; adb logcat | grep -E "STEAMSHIM|EOSSHIM|authenticate"
```

Good signs (in order):
- `STEAMSHIM: relay OK: SteamID64=... ticket=143 bytes` — headset reached your PC and got a ticket.
- `STEAMSHIM: GetEncryptedAppTicket -> 143 bytes served` — ticket handed to the game.
- `EOSSHIM: Connect_CopyIdToken result=0` — EOS accepted it (0 = success).
- No `ClientWasKicked` / no "Unable to authenticate device" — you're in.

Common problems:
- **`relay connect ... FAILED`** in logcat → PC IP wrong in `relay.txt`, firewall not opened, or PC
  and Quest aren't on the same network. Test from the PC: `Test-NetConnection <PC-IP> -Port 48010`.
- **`relay OK` but still kicked** → the PC's Steam account doesn't actually own Shack (3504270), or
  the relay window isn't running.
- **Ticket looks stale after a while** → just rejoin; the relay re-mints on the next connect.
- **Back to community servers only** → delete `steamlogin.txt` to return to the anonymous Device-ID
  login:
  ```sh
  adb shell rm /sdcard/Android/data/com.vankrupt.pavlov/files/steamlogin.txt
  ```

---

## Changing your display name (persona.txt)

`persona.txt` sets the name shown in-game. If it has a name, that name is used (a spoof); if it's
empty or missing, your real Steam name (from the relay) is used instead.

**Set a name (adb):**
```sh
adb shell "printf 'YourName' > /sdcard/Android/data/com.vankrupt.pavlov/files/persona.txt"
adb shell am force-stop com.vankrupt.pavlov
```
Then relaunch Pavlov — you'll show as `YourName`.

**Or on the headset:** with any file manager, edit
`Android/data/com.vankrupt.pavlov/files/persona.txt` and type the name (one line, no quotes).

**Use your real Steam name instead:** empty the file (or delete it), then restart:
```sh
adb shell "rm -f /sdcard/Android/data/com.vankrupt.pavlov/files/persona.txt"
```

Notes:
- It's read **once at launch** — always restart the game after changing it.
- This only changes the **displayed name**. The account you're authenticated as is still the relay's
  Steam account (that's what gets you into lobbies) — the name is cosmetic.
- Your avatar comes from the relay (your real Steam avatar) in this build; there's no local
  `pfp.png` override enabled.

## How it works (short version)

The game authenticates to official lobbies with a Steam **encrypted app ticket** (`RequestEncrypted
AppTicket` → EOS `STEAM_APP_TICKET`). That ticket is encrypted by **Steam's servers** with the
game's key, so the requesting client never needs any secret — it only needs to **own the app**. The
PC relay mints a real one for your account; the headset feeds it into the game's Steam interface in
place of the missing Steam client, and EOS validates it for real. The result is a genuine Steam PUID
the server accepts — the same identity a real Steam Frame player would present.
