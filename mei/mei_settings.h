// mei_settings.h — shared feature state for "mei mei [private]".
//
// ONE struct that the ImGui menu writes and the reflection core (pavchams.cpp) reads. This
// replaces the single-integer chams.txt with granular per-feature toggles + tunables. The menu
// edits g_mei live; pavchams reads g_mei at each feature gate; changes persist to mei.cfg.
//
// Everything here is plain data (no engine types) so both translation units share it with zero
// coupling. Defaults below reproduce the old chams.txt=6 behaviour so nothing regresses if the
// menu never opens.
#pragma once
#include <cstdint>
#include <cstring>

#define MEI_CFG_PATH "/sdcard/Android/data/com.vankrupt.pavlov/files/mei.cfg"
#define MEI_NAME_MAX 32

// aim trigger modes (mirrors the old cfg 3/6 split)
enum MeiAimMode { AIM_OFF = 0, AIM_ONFIRE = 1, AIM_CONTINUOUS = 2 };

struct MeiSettings {
    // ---- master ----
    bool  master_enabled   = true;   // global kill switch (old cfg>=2)

    // ---- Aimbot ----
    bool  aim_enabled      = true;
    int   aim_mode         = AIM_CONTINUOUS;   // MeiAimMode
    float aim_fov          = 180.0f;  // degrees; 180 = whole view (nearest-head, old behaviour)
    bool  aim_team_check   = true;    // don't target teammates (auto FFA-detect still applies)
    bool  aim_bullet_tp    = true;    // teleport the shot to the head on fire (old do_aim TP arg)
    float aim_smooth       = 0.0f;    // 0 = instant snap; >0 = lerp fraction/frame (0..1)
    bool  aim_target_head  = true;    // aim the skull socket (vs pawn origin)
    float aim_head_z       = -13.0f;  // cm applied to skull socket (socket = crown; negative drops to head centre)

    // ---- Visuals / chams ----
    bool  chams_enabled    = true;
    bool  chams_team_color = true;    // team-0/1 xray materials (vs single color)
    bool  chams_highlight  = true;    // brighten the current aim target
    bool  chams_skip_dead  = true;    // never chams a corpse (keep true; cheap + correct)

    // ---- ESP overlay (view-locked quad drawn by mei_xr from pavchams-projected positions) ----
    bool  esp_enabled      = false;
    bool  esp_box          = true;    // 2D box around each enemy
    bool  esp_name         = true;    // player name
    bool  esp_dist         = true;    // distance in metres
    bool  esp_health       = true;    // health bar
    bool  esp_role         = true;    // TTT role (where the client has it)
    bool  esp_crosshair    = true;    // center crosshair dot
    bool  esp_fov_circle   = true;    // draw the aimbot FOV cone as a circle
    float esp_fov          = 97.0f;   // horizontal FOV (deg) used to project — calibrate to your headset
    float esp_max_dist     = 200.0f;  // metres; hide entries beyond this

    // ---- Weapon ----
    bool  no_recoil        = true;
    bool  perfect_accuracy = true;
    bool  rapid_fire       = false;   // shrink fire interval (offline; server may cap)
    bool  force_auto       = false;   // force FireMode = Automatic/FullAuto
    bool  no_reload        = false;   // zero reload cooldowns
    bool  infinite_ammo    = false;   // top the magazine each pass (Bullets = MaxBullets)

    // ---- Combat (ported/extended from the PC internal reference) ----
    bool  trigger_kill     = false;   // on fire, report a headshot on the aim target (ServerReportBulletHit)
    bool  kill_aura        = false;   // continuously report headshots on any target in FOV (no trigger)
    bool  wallbang         = false;   // on fire, teleport the gun to the target head -> shot ignores walls
    float aura_rate        = 120.f;   // kill-aura interval (ms) between reports

    // ---- Movement (PavlovMovementComponent) ----
    bool  move_enabled     = false;
    float move_sprint      = 2.0f;    // old hardcoded 2.0
    float move_ads         = 5.0f;    // old hardcoded 5.0 (fast while aiming)
    float move_walk        = 2.0f;    // multiplier on original walk (old 2.0x)
    float move_crouch      = 2.0f;    // multiplier on original crouch walk speed

    // ---- Player ----
    bool  godmode          = false;   // offline only (server-auth)
    bool  dev_tag          = false;   // self-view dev tag
    bool  homing_knife     = false;
    bool  name_enabled     = false;
    char  name_text[MEI_NAME_MAX] = {0};   // client-side PlayerNamePrivate (short!)

    // ---- Menu / panel ----
    bool  menu_open        = false;   // is the panel currently shown
    float panel_dist       = 0.85f;   // metres in front of the head (comfortable arm's-length)
    float panel_scale      = 1.3f;    // quad size multiplier
    int   ui_accent        = 0;       // theme accent index (see mei_menu.cpp)

    // ---- Panel reposition ("move panel") ----
    bool  reposition       = false;   // Move mode ON: point off-panel + hold trigger to drag it (transient, not saved)
    bool  panel_custom     = false;   // user manually placed the panel -> skip auto front-anchor on open
    float panel_off[3]     = {0.f, 0.f, -0.85f};  // panel position in HEAD-LOCAL frame (metres); used when panel_custom

    // ---- Buy (TTT ServerBuy) ----
    char  buy_name[MEI_NAME_MAX] = {0};   // equipment FName to buy
    int   my_credits             = -1;    // last-read local credits (display only, -1 = unknown)
    char  my_role[12]            = {0};   // last-read local role (display only)

    // ---- one-shot actions (menu sets true, worker consumes + clears) ----
    volatile bool act_dump_sdk       = false;
    volatile bool act_buy            = false;   // ServerBuy(buy_name) now
    volatile bool act_dump_whitelist = false;
    volatile bool act_save           = false;   // force persist now
    volatile bool act_replace        = false;   // re-anchor the panel in front of the head now (Reset position)
};

// the single shared instance (defined in mei_settings.cpp)
extern MeiSettings g_mei;

// persistence (mei_settings.cpp). Thread-safe enough for our use: one writer (menu/worker).
void mei_load();
void mei_save();

// derive the legacy cfg int from granular flags, so any un-migrated pavchams gate still works
// during the transition. (chams=2, +aim-onfire=3, +weapon/move/god=4, +continuous-aim=6)
static inline int mei_legacy_cfg(const MeiSettings& s) {
    if (!s.master_enabled) return 0;
    if (s.aim_enabled && s.aim_mode == AIM_CONTINUOUS) return 6;
    if (s.move_enabled || s.godmode || s.rapid_fire || s.force_auto) return 4;
    if (s.aim_enabled && s.aim_mode == AIM_ONFIRE) return 3;
    return 2;
}
