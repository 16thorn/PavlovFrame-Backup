# mei mei [private] — VR ImGui menu

An in-headset **Dear ImGui** control panel for the mod, rendered as an **OpenXR quad composition
layer** and driven by a **controller ray**. It replaces the single-integer `chams.txt` control with
granular per-feature toggles + sliders that persist to `mei.cfg`.

## How it's wired

```
pavchams.cpp (reflection core)                 mei/ (menu subsystem)
────────────────────────────                   ─────────────────────
boot(): mei_load(), mei_xr_install()   ──▶     mei_xr.cpp   hooks xrGetInstanceProcAddr
handler(): mei_feed_input() every PE            │             ├─ xrCreateSession: grab VkInstance/
   • head pose  = GetPlayerViewPoint            │             │   PhysicalDevice/Device/Queue, make
   • aim ray    = held-gun transform            │             │   a VIEW space + our action set
   • open/click = injected action set (L3/trig) │             ├─ xrAttach/xrSync: splice our actions
                                                 │             └─ xrEndFrame: poll buttons; render into
feature gates read g_mei  ◀────────────────┐    │                 XrSwapchain (Vulkan) and append a
                                            │    │                 XrCompositionLayerQuad in front of
mei_input_feed(head, ctrl, ...)  ──────────▶│    │                 the face
                                            │   mei_input.cpp  ray ∩ head-local panel plane → cursor;
g_mei  (MeiSettings)  ◀── menu edits ───────┘    │             right trigger drives click (dwell fallback)
                                                 mei_menu.cpp  the "mei mei [private]" tabs + widgets
                                                 mei_settings.cpp  g_mei + mei.cfg persistence
```

**Key idea (no calibration):** the panel is defined purely in **head-local** space. `mei_xr` renders
it as a **VIEW-space** quad (OpenXR tracks the head for us). `mei_input` expresses the controller ray
in the head-local frame built from the reflected camera transform. Both anchor to the same head
through each API's own head reference, so cursor and quad line up **without** any world↔tracking
transform.

## Controls

Real controller buttons, via an **injected OpenXR action set** (`mei_xr.cpp`): we create our own
`XrActionSet` (click + toggle), suggest bindings, and splice it into the app's
`xrAttachSessionActionSets` / `xrSyncActions` so we read genuine controller state alongside the game.

- **Open / close:** **left thumbstick press (L3)** (`.../left/input/thumbstick/click`).
- **Click:** **right trigger** (`.../right/input/trigger/value`, thresholded to a boolean).
- **Move cursor:** aim the controller at the panel (the held gun is the ray origin).

**Fallback:** until our actions report `isActive`, `mei_xr_actions_live()` stays false and pavchams
keeps the **up-point-to-open** gesture + **dwell-to-click** so the menu is usable on first boot while
bindings are tuned. Once real buttons read live, both fallbacks switch off automatically.

## Tabs

| Tab | Controls |
|---|---|
| **Aimbot** | enable, mode (off / on-fire / continuous), FOV, aim-at-head, team check, bullet-TP, smoothing |
| **Visuals** | chams enable, team colors, highlight target, skip dead |
| **Weapon** | no-recoil, perfect accuracy, rapid fire, force full-auto, no-reload |
| **Movement** | enable, sprint×, ADS×, walk× |
| **Player** | godmode, dev tag, homing knife, name changer (text) |
| **Config** | master enable, panel distance/scale, accent, dump SDK / whitelist, save |

Every widget writes `g_mei`; the reflection core reads `g_mei` at each feature gate. Changes
auto-save to `mei.cfg` ~1 s after the last edit (or the **Save** button).

## Build

`build.sh` clones the pinned deps into `third_party/` on first run:
- Dear ImGui `v1.90.9` (the `ImGui_ImplVulkan_Init(&info)` + `CreateFontsTexture()` API mei_xr targets)
- OpenXR-SDK `release-1.1.36` (headers only)

Then compiles `pavchams.cpp` + `mei/*.cpp` + imgui core + `imgui_impl_vulkan.cpp` into
`libpavchams.so`, linking `-lvulkan -ldl -llog`. Vulkan is a system lib on Quest.

See **ON-DEVICE.md** for first-pixel bring-up (this part can't be tested off-headset).
