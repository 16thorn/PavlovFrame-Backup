# Pavlov Frame — Release Notes

## Standalone build (2026-09-05)

**Zero-setup sideload.** The build now seeds its own config on first launch — no
adb, no manual `echo` into the files dir. Fresh installs boot armed and
standalone-ready.

### New
- **Auto-created config files.** On first boot the shims create any missing flag
  file with a sane default (existing files are never overwritten — your setup
  always wins):
  - `chams.txt` → `6` (chams armed out of the box)
  - `relay.txt` → `127.0.0.1:48010` (local on-device ticket relay = standalone)
  - `steamlogin.txt` → armed (Steam-relay login path enabled by default, so
    official lobbies work without touching anything)
- **Persona editing in-menu.** ACCOUNT tab → **PERSONA (Steam name)** field →
  **Save persona**. Sets your server-visible name from the VR keyboard; applies
  on next game restart.

### Fixed
- **chams.txt=9 dump trap.** After a one-shot SDK dump the worker could get stuck
  in dump mode — chams and aim silently stopped working until the flag was reset
  by hand. It now resets itself back to `6` after the dump and resumes normally.
  Self-heals on next boot even if the flag is currently stuck at `9`.

### Notes
- Identity-spoof files (`persona.txt`, `steamid.txt`, `name.txt`) are **not**
  auto-created — they stay opt-in so a fresh install keeps a clean default
  identity.
- To point at a **PC/LAN relay** instead of the local one, edit `relay.txt` to
  `<pc-ip>:48010`. To fall back to the anonymous / community-server path, delete
  `steamlogin.txt`.
- In-place update: same signing key across builds, so `adb install -r` updates
  over any prior version with no uninstall.
