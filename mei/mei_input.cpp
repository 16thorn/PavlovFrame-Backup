// mei_input.cpp — ray/plane math for the head-locked panel. No engine, no OpenXR; pure vectors.
#include "imgui.h"
#include "mei_input.h"
#include "mei_settings.h"

int   g_panel_w = 1024, g_panel_h = 720;
float g_panel_wm = 0.5f, g_panel_hm = 0.35f;
bool  g_cursor_valid = false;
float g_cursor_px = -1.f, g_cursor_py = -1.f;

// last fed pose
static float H_pos[3], H_r[3], H_u[3], H_f[3], C_pos[3], C_f[3];
static bool  s_trigger = false, s_have = false;
static bool  s_hw_trigger = false;
bool  g_hw_input_active = false;
void mei_input_set_trigger(bool down) { s_hw_trigger = down; }

// view-space controller ray (primary path)
static float V_pos[3], V_fwd[3]; static bool V_valid = false;
void mei_input_feed_view(const float pos[3], const float fwd[3], bool valid) {
    if (valid) { for (int i=0;i<3;i++){ V_pos[i]=pos[i]; V_fwd[i]=fwd[i]; } }
    V_valid = valid;
}

// dwell-to-click: with no controller-button offset known yet, hovering still over a widget for
// DWELL_MS emits one click. Works with only the gun-ray reflection the mod already has. If a real
// trigger gets fed later, it ORs in (instant click) and dwell still serves as a fallback.
#define DWELL_MS   700.0
#define DWELL_RAD  22.0f   // px stillness radius
static float s_anchor_x = -1, s_anchor_y = -1;
static double s_still_since = 0;
static bool s_dwell_latched = false;

static inline float dot3(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
#include <time.h>
static double in_now_ms() { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0 + t.tv_nsec/1e6; }

void mei_input_feed(const float hp[3], const float hr[3], const float hu[3], const float hf[3],
                    const float cp[3], const float cf[3], bool trigger) {
    for (int i=0;i<3;i++){ H_pos[i]=hp[i]; H_r[i]=hr[i]; H_u[i]=hu[i]; H_f[i]=hf[i]; C_pos[i]=cp[i]; C_f[i]=cf[i]; }
    s_trigger = trigger; s_have = true;
}

// shared tail: given panel-normalised (u,v), place the cursor + emit clicks (hw trigger or dwell).
static bool finish_cursor(ImGuiIO& io, float u, float v) {
    if (u < -0.05f || u > 1.05f || v < -0.05f || v > 1.05f) {   // off panel (small edge margin)
        bool held = g_hw_input_active && s_hw_trigger;
        if (held && g_cursor_px >= 0) io.AddMousePosEvent(g_cursor_px, g_cursor_py);
        else                          io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, held);
        return false;
    }
    g_cursor_px = u * g_panel_w;
    g_cursor_py = v * g_panel_h;
    g_cursor_valid = true;
    io.AddMousePosEvent(g_cursor_px, g_cursor_py);
    if (g_hw_input_active) { io.AddMouseButtonEvent(0, s_hw_trigger); return true; }  // real trigger
    // fallback: dwell-to-click
    double tnow = in_now_ms();
    bool moved = (s_anchor_x < 0) ||
                 (g_cursor_px - s_anchor_x)*(g_cursor_px - s_anchor_x) +
                 (g_cursor_py - s_anchor_y)*(g_cursor_py - s_anchor_y) > DWELL_RAD*DWELL_RAD;
    if (moved) { s_anchor_x = g_cursor_px; s_anchor_y = g_cursor_py; s_still_since = tnow; s_dwell_latched = false; }
    bool dwell_fire = false;
    if (!s_dwell_latched && (tnow - s_still_since) >= DWELL_MS) { dwell_fire = true; s_dwell_latched = true; }
    static bool s_dwell_down = false;
    if (dwell_fire) s_dwell_down = true;
    io.AddMouseButtonEvent(0, s_dwell_down);
    if (s_dwell_down && !dwell_fire) s_dwell_down = false;
    return true;
}

// Primary cursor entry (world-anchored panel): mei_xr computes u,v and calls this each frame.
void mei_input_set_cursor(float u, float v, bool valid) {
    ImGuiIO& io = ImGui::GetIO();
    g_cursor_valid = false;
    if (!valid) { finish_cursor(io, -10.f, -10.f); return; }   // park off-screen, keep click state sane
    finish_cursor(io, u, v);
}

bool mei_input_apply() {
    ImGuiIO& io = ImGui::GetIO();
    g_cursor_valid = false;
    if (!g_mei.menu_open || (!s_have && !V_valid)) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        return false;
    }
    const float d = g_mei.panel_dist;

    // ---- PRIMARY: OpenXR controller aim ray in VIEW space (metres) ----
    // View space: head at origin, looking down -Z. Panel is centered at (0,0,-d), facing +Z (toward
    // the viewer), half-extents (wm/2, hm/2). Intersect the controller ray with plane z = -d.
    if (V_valid) {
        float Ox = V_pos[0], Oy = V_pos[1], Oz = V_pos[2];
        float Dx = V_fwd[0], Dy = V_fwd[1], Dz = V_fwd[2];
        if (Dz < -1e-4f) {                                  // must point toward -Z (into the panel)
            float t = (-d - Oz) / Dz;
            if (t > 0.f) {
                float hx = Ox + t*Dx, hy = Oy + t*Dy;       // hit point on the panel plane (metres)
                float u = (hx + g_panel_wm*0.5f) / g_panel_wm;         // x: left->right
                float v = (g_panel_hm*0.5f - hy) / g_panel_hm;         // y: top->bottom (flip)
                return finish_cursor(io, u, v);
            }
        }
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, g_hw_input_active ? s_hw_trigger : false);
        return false;
    }

    // ---- FALLBACK: engine head-local ray (gun-based) ----
    if (!s_have) { io.AddMousePosEvent(-FLT_MAX,-FLT_MAX); return false; }
    // panel is at head-local depth d (metres) along fwd. Express controller in head-local basis.
    float rel[3] = { C_pos[0]-H_pos[0], C_pos[1]-H_pos[1], C_pos[2]-H_pos[2] };
    // NOTE: engine units may be cm while d is metres. We normalise by the head->panel definition:
    // the head-local frame is unitless-consistent because right/up/fwd are unit basis vectors, so
    // Ox/Oy/Oz come out in engine units. Convert d to engine units via g_unit (set by caller-scale).
    // We instead solve on DIRECTIONS + a depth ratio, which is unit-free:
    float Oz = dot3(rel, H_f);                 // controller depth ahead of head (engine units)
    float Ox = dot3(rel, H_r);
    float Oy = dot3(rel, H_u);
    float Dz = dot3(C_f, H_f);                 // ray direction in head-local
    float Dx = dot3(C_f, H_r);
    float Dy = dot3(C_f, H_u);
    if (Dz <= 1e-4f) { io.AddMousePosEvent(-FLT_MAX,-FLT_MAX); return false; }  // pointing away

    // The plane sits at engine-depth (d * unit). But we don't know `unit`; instead we place the
    // plane at the same head-local *angular* position the quad occupies. The quad half-width in
    // metres is g_panel_wm/2 at depth d, i.e. angular half-width = atan((wm/2)/d). We intersect the
    // ray with a plane at an ARBITRARY positive depth D0 and map by angle so units cancel.
    const float D0 = (Oz > 1e-3f ? Oz + 1.0f : 1.0f);       // any plane ahead of the controller
    float t = (D0 - Oz) / Dz;
    if (t <= 0.f) { io.AddMousePosEvent(-FLT_MAX,-FLT_MAX); return false; }
    float hx = Ox + t*Dx, hy = Oy + t*Dy;                   // hit point, head-local, at depth D0
    // angle of the hit relative to straight-ahead-from-head
    float ang_x = hx / D0;                                   // small-angle tan ~ x/z
    float ang_y = hy / D0;
    // panel angular half-extents (metres at metric depth d -> tan = half/d)
    float half_ax = (g_panel_wm * 0.5f) / d;
    float half_ay = (g_panel_hm * 0.5f) / d;
    // map angle -> [0,1] across the panel (x right, y flipped so up-is-top)
    float u = (ang_x + half_ax) / (2.f * half_ax);
    float v = (half_ay - ang_y) / (2.f * half_ay);
    return finish_cursor(io, u, v);
}
