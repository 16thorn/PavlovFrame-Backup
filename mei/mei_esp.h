// mei_esp.h — shared ESP entry buffer. pavchams (game thread) projects each enemy with the game's
// own camera and fills this; mei_xr (render thread) plots the entries on the view-locked overlay quad.
// Projection is done in pavchams so we never fight the UE-world <-> OpenXR-space mismatch.
#pragma once
#include <cstdint>

struct EspEntry {
    float u, v;        // screen [0,1] of the pawn origin (feet)
    float uh, vh;      // screen [0,1] of the head
    float dist;        // metres to the pawn
    float health;      // 0..1, or -1 if unknown
    int   team;        // team id (for color)
    int   credits;     // TTT credits, or -1 if unknown
    char  name[24];    // player name ("" if unknown)
    char  role[12];    // TTT role text ("" if unknown / not replicated)
    bool  valid;       // in front of camera + on/near screen
};

#define MEI_ESP_MAX 64
#define MEI_ESP_ASPECT 1.6f    // ESP overlay quad aspect (w/h) — pavchams projects with this, mei_xr draws with it
extern EspEntry     g_esp[MEI_ESP_MAX];
extern volatile int g_esp_n;          // number of valid entries this frame
extern float        g_esp_fov_used;   // horizontal FOV (deg) pavchams projected with (for the quad/circle)
