# mei menu — on-device bring-up checklist

This is the honest part. The reflection features are proven on your headset already; the **VR ImGui
layer is new and cannot be exercised off-headset**. Work this list top-to-bottom with
`adb logcat -s MEI-XR MEI-CFG PAVCHAMS` open. Each step has a log line to confirm it.

## 0. It builds
- `./build.sh` clones `third_party/imgui` + `third_party/OpenXR-SDK`, compiles, installs.
- If imgui's Vulkan backend API drifted, pin matters: we target **v1.90.9**
  (`ImGui_ImplVulkan_Init(&info)` with `RenderPass` inside `InitInfo`, and
  `ImGui_ImplVulkan_CreateFontsTexture()` taking no args). Newer/older tags change these two calls.

## 1. The OpenXR hook arms
- Expect: `MEI-XR: xrGetInstanceProcAddr hooked`.
- If instead `xrGetInstanceProcAddr not found`: the loader lib name differs. `adb shell run-as ...
  cat /proc/<pid>/maps | grep -i openxr` to find it, add it to `find_real_gipa()`'s `libs[]`.
- **Timing risk:** if the app resolved `xrCreateSession`/`xrEndFrame` *before* our hook armed, we
  miss them. The boot loop retries `mei_xr_install()` every 100 ms from load, so it should be in
  place before session creation — but if `hk_xrCreateSession` never logs, this is why. Fallback:
  move the `mei_xr_install()` call into the EOS shim constructor (`eosshim.cpp`), which runs earlier.

## 2. Vulkan handles captured
- Expect: `MEI-XR: captured Vk: inst=.. dev=.. queue=..` then `session created, gfx_captured=1`.
- If `no Vulkan graphics binding found`: the app may create the session with the **Vulkan2** binding
  under a different struct, or (unlikely for UE5) GLES. We already check both
  `XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR` and `..._VULKAN2_KHR`. If GLES: the whole backend needs an
  ImGui **GLES3** path instead — tell me and I'll swap `mei_xr.cpp`'s renderer.

## 2b. Controller buttons (L3 open / trigger click)
- Expect at session create: `bindings suggested: /interaction_profiles/oculus/touch_controller`
  (or facebook/meta), then `actions built=1`, then at attach `attach: injected our action set`.
- Press **L3** (left stick): expect `menu OPEN (L3 stick-press)`. Pull **right trigger** over a
  control: it should click.
- If `menu OPEN (up-gesture fallback)` shows instead, our actions never went `isActive` — the real
  Touch-Plus **interaction profile** on your runtime differs from the four we suggest. Find it:
  `adb logcat | grep -i interaction_profile`, or add a one-line log of
  `xrGetCurrentInteractionProfile` in `poll_actions`, then add that profile string to `build_actions()`'s
  `P[]` table. (Everything else stays; it's a one-line add.)
- If L3/trigger feel swapped or wrong, edit the `click`/`toggle` source paths in that same table.

## 3. Backend comes up (open the menu once)
- Press **L3** (or the up-gesture fallback). Expect: `menu OPEN ...` then `mei backend up: panel ...`.
- **Can't get the gesture yet?** Force the panel on from adb:
  `adb shell 'echo 7 > /sdcard/Android/data/com.vankrupt.pavlov/files/chams.txt'` (7 = force menu
  open — lets you test render/cursor before the gesture is dialed in). Set it back to `2` to release.
- If it opens but you see `xrCreateSwapchain failed` / `ImGui_ImplVulkan_Init failed`: usually a
  format or queue mismatch. Check the picked format in `swapchain ... fmt=`; if the runtime rejects
  it, adjust `pick_color_format()` prefs. Confirm `g_vkQF` is a graphics-capable queue family.

## 4. First pixel
- You should see the panel floating in front of you. If **black/opaque**: the quad isn't alpha
  blending — confirm `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` and that we clear to
  `(0,0,0,0)`. If **not visible at all**: the layer may be composited behind the app's projection
  layer — some runtimes honor layer order; our quad is appended **last** (top-most). If still hidden,
  the app may pass `layerCount` with a fixed-size array it owns; we build our own list, so that's
  fine, but double-check `hk_xrEndFrame` is actually being called (add a once-log).

## 5. Cursor tracks the ray
- Aim at the panel; a cursor should move. If it's **mirrored L/R or U/D**: flip the corresponding
  sign in `mei_input.cpp` (`ang_x`/`ang_y` mapping) or in `rot_basis()` (the `right`/`up` vectors) —
  UE vs OpenXR handedness is the usual culprit. One-line fix, noted where.
- If the cursor is **offset** from where you point: the head/gun poses may be in different spaces
  than assumed. `GetPlayerViewPoint` gives camera world pose; the gun transform gives the ray. If the
  gun's reported forward is the barrel vs the grip, adjust — or feed the motion-controller aim pose
  instead.

## 6. Click works
- Hold still on a toggle ~0.7 s → it flips (dwell-click). Tune `DWELL_MS` / `DWELL_RAD` in
  `mei_input.cpp` to taste. When you find the real trigger offset, feed it as the last arg of
  `mei_input_feed(...)` for instant clicks.

## 7. Persistence
- Flip something, wait ~1 s, expect `MEI-CFG: saved`. Relaunch → `MEI-CFG: loaded (...)` with your
  values. File: `/sdcard/Android/data/com.vankrupt.pavlov/files/mei.cfg`.

## 8. Performance (the no-lag rewrite)
The renderer was rebuilt so our overlay GPU work overlaps UE's frame instead of blocking it
(removed the per-frame vkWaitForFences on the app queue; per-image cmd/fence rings + a monotonic slot
in lockstep with ImGui's buffers; cached PFNs; no per-frame heap alloc; eager backend build; opaque
+ unpremultiplied-alpha quad; UNORM format). Confirm on-device:
- With the menu OPEN, the game holds refresh (72/90/120) with no global hitch — compare frame timing
  open vs closed via OVR Metrics Tool; delta should be a few percent, not a stutter.
- `adb logcat -s MEI-XR` prints `hk_xrEndFrame thread tid=<N>` once. If UE submits scene work on a
  DIFFERENT thread than this tid, the shared-queue submit is a race — tell me and we add a private
  queue / lock (S1/N4 in the plan). On Quest the render thread submits directly, so they usually match.
- Dev build with the Vulkan validation layer: zero queue external-sync / cmd-buffer-in-use / fence
  errors while clicking the menu.
- Panel is opaque where it should be, colors correct (no washed-out look), clicks land first-press,
  opening never hitches, and a 10-min toggle session shows no flicker/corruption/device-lost.

## Fallbacks if the layer never lands
The features still work headless: `mei.cfg` is plain text, so you can `adb push` a config and play
with the menu closed. And `chams.txt` still forces `8`/`9` dumps and `>=2` arming. The menu is the
front-end; the mod underneath is unchanged.
