// mei_input.h — controller-ray -> ImGui cursor bridge.
//
// The reflection core (pavchams.cpp) owns all engine access, so it FEEDS us the head + controller
// poses (in engine/world space) and the button state each frame. We turn the right-controller ray
// into a panel-space cursor. Because the panel is defined purely in HEAD-LOCAL space and we express
// both the panel plane and the ray in that same head-local frame, the input stays consistent with
// the view-space OpenXR quad WITHOUT any world<->tracking calibration (see mei_xr.cpp).
#pragma once
#include <cstdint>

// Panel pixel size (backbuffer). Set by mei_xr at init; read by input for UV->pixel mapping.
extern int  g_panel_w, g_panel_h;
// Panel metric size (metres) — kept in sync with the OpenXR quad extent.
extern float g_panel_wm, g_panel_hm;

// Fed by pavchams each frame (engine world space, any consistent unit — metres OR cm, we only use
// ratios and directions). Vectors are 3 floats {x,y,z}.
//   head_pos/right/up/fwd : the HMD/camera transform basis (fwd = look direction).
//   ctrl_pos/ctrl_fwd     : right controller aim origin + forward.
//   trigger               : right trigger pressed (click).
//   toggle                : menu open/close edge already debounced by caller.
void mei_input_feed(const float head_pos[3], const float head_right[3], const float head_up[3],
                    const float head_fwd[3], const float ctrl_pos[3], const float ctrl_fwd[3],
                    bool trigger);

// Hardware button state, fed by mei_xr from our injected OpenXR action set. When real buttons are
// live, dwell-click is disabled and the fed trigger drives clicks instead.
void mei_input_set_trigger(bool down);   // right trigger -> ImGui click
extern bool g_hw_input_active;           // true once real controller actions are reading

// Controller aim ray expressed in the panel's VIEW space (metres): origin + forward direction.
// This is the PRIMARY input path — a real OpenXR aim pose located against the panel's VIEW space,
// so pointing works with hand alone (no gun needed). `valid`=false falls back to the engine feed.
void mei_input_feed_view(const float pos[3], const float fwd[3], bool valid);

// Direct panel-normalised cursor (u,v in [0,1], top-left origin) computed by mei_xr from the aim ray
// vs the world-anchored panel. valid=false parks the cursor off-screen. Drives clicks (trigger/dwell).
void mei_input_set_cursor(float u, float v, bool valid);

// Compute the cursor from the last fed pose and push it into ImGui IO. Call inside the XR frame,
// after ImGui::NewFrame's IO is ready but before building widgets. Returns true if the ray hit the
// panel (cursor valid); false = ray misses (cursor parked off-screen).
bool mei_input_apply();

// true while the ray is on the panel (for drawing a laser dot, optional).
extern bool  g_cursor_valid;
extern float g_cursor_px, g_cursor_py;
