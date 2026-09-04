// mei_settings.cpp — the shared MeiSettings instance + flat key=value persistence to mei.cfg.
#include "mei_settings.h"
#include <cstdio>
#include <cstdlib>
#include <android/log.h>

#define SLOG(...) __android_log_print(ANDROID_LOG_INFO, "MEI-CFG", __VA_ARGS__)

MeiSettings g_mei;
MeiPlayer   g_players[MEI_PLAYERS_MAX];
int         g_players_n = 0;

// tiny flat format: one "key value" per line. Robust to partial files / added keys.
void mei_save() {
    FILE* f = fopen(MEI_CFG_PATH, "w");
    if (!f) { SLOG("save: cannot open %s", MEI_CFG_PATH); return; }
    const MeiSettings& s = g_mei;
    fprintf(f, "master %d\n",          s.master_enabled);
    fprintf(f, "aim_enabled %d\n",     s.aim_enabled);
    fprintf(f, "aim_mode %d\n",        s.aim_mode);
    fprintf(f, "aim_fov %.3f\n",       s.aim_fov);
    fprintf(f, "aim_team_check %d\n",  s.aim_team_check);
    fprintf(f, "aim_bullet_tp %d\n",   s.aim_bullet_tp);
    fprintf(f, "aim_smooth %.3f\n",    s.aim_smooth);
    fprintf(f, "aim_target_head %d\n", s.aim_target_head);
    fprintf(f, "aim_head_z %.3f\n",    s.aim_head_z);
    fprintf(f, "chams_enabled %d\n",   s.chams_enabled);
    fprintf(f, "chams_team_color %d\n",s.chams_team_color);
    fprintf(f, "chams_highlight %d\n", s.chams_highlight);
    fprintf(f, "chams_style %d\n",     s.chams_style);
    fprintf(f, "chams_col %.3f %.3f %.3f\n", s.chams_col[0], s.chams_col[1], s.chams_col[2]);
    fprintf(f, "chams_col2 %.3f %.3f %.3f\n", s.chams_col2[0], s.chams_col2[1], s.chams_col2[2]);
    fprintf(f, "esp_enabled %d\n",     s.esp_enabled);
    fprintf(f, "esp_box %d\n",         s.esp_box);
    fprintf(f, "esp_name %d\n",        s.esp_name);
    fprintf(f, "esp_dist %d\n",        s.esp_dist);
    fprintf(f, "esp_health %d\n",      s.esp_health);
    fprintf(f, "esp_role %d\n",        s.esp_role);
    fprintf(f, "esp_crosshair %d\n",   s.esp_crosshair);
    fprintf(f, "esp_fov_circle %d\n",  s.esp_fov_circle);
    fprintf(f, "esp_fov %.2f\n",       s.esp_fov);
    fprintf(f, "esp_max_dist %.1f\n",  s.esp_max_dist);
    fprintf(f, "no_recoil %d\n",       s.no_recoil);
    fprintf(f, "perfect_accuracy %d\n",s.perfect_accuracy);
    fprintf(f, "rapid_fire %d\n",      s.rapid_fire);
    fprintf(f, "force_auto %d\n",      s.force_auto);
    fprintf(f, "no_reload %d\n",       s.no_reload);
    fprintf(f, "infinite_ammo %d\n",   s.infinite_ammo);
    fprintf(f, "trigger_kill %d\n",    s.trigger_kill);
    fprintf(f, "kill_aura %d\n",       s.kill_aura);
    fprintf(f, "kill_all_loop %d\n",   s.kill_all_loop);
    fprintf(f, "wallbang %d\n",        s.wallbang);
    fprintf(f, "aura_rate %.1f\n",     s.aura_rate);
    fprintf(f, "move_enabled %d\n",    s.move_enabled);
    fprintf(f, "noclip %d\n",          s.noclip);
    fprintf(f, "anti_flash %d\n",      s.anti_flash);
    fprintf(f, "move_sprint %.3f\n",   s.move_sprint);
    fprintf(f, "move_ads %.3f\n",      s.move_ads);
    fprintf(f, "move_walk %.3f\n",     s.move_walk);
    fprintf(f, "move_crouch %.3f\n",   s.move_crouch);
    fprintf(f, "fly_speed %.3f\n",     s.fly_speed);
    fprintf(f, "godmode %d\n",         s.godmode);
    fprintf(f, "dev_tag %d\n",         s.dev_tag);
    fprintf(f, "force_vote %d\n",      s.force_vote);
    fprintf(f, "homing_knife %d\n",    s.homing_knife);
    fprintf(f, "name_enabled %d\n",    s.name_enabled);
    fprintf(f, "voice_enabled %d\n",   s.voice_enabled);
    fprintf(f, "voice_unmute %d\n",    s.voice_unmute);
    fprintf(f, "sb_loop %d\n",         s.sb_loop);
    fprintf(f, "sb_mix_mic %d\n",      s.sb_mix_mic);
    fprintf(f, "sb_monitor %d\n",      s.sb_monitor);
    fprintf(f, "sb_transmit %d\n",     s.sb_transmit);
    fprintf(f, "sb_gain %.3f\n",       s.sb_gain);
    fprintf(f, "name_text %s\n",       s.name_text[0] ? s.name_text : "-");
    fprintf(f, "skin_name %s\n",       s.skin_name[0] ? s.skin_name : "-");
    fprintf(f, "panel_dist %.3f\n",    s.panel_dist);
    fprintf(f, "panel_scale %.3f\n",   s.panel_scale);
    fprintf(f, "ui_accent %d\n",       s.ui_accent);
    fprintf(f, "ui_dark %d\n",         s.ui_dark);
    fprintf(f, "username %s\n",        s.username[0] ? s.username : "-");
    fprintf(f, "panel_custom %d\n",    s.panel_custom);
    fprintf(f, "panel_off %.4f %.4f %.4f\n", s.panel_off[0], s.panel_off[1], s.panel_off[2]);
    fclose(f);
    SLOG("saved");
}

void mei_load() {
    FILE* f = fopen(MEI_CFG_PATH, "r");
    if (!f) { SLOG("load: no cfg yet (%s) — defaults", MEI_CFG_PATH); return; }
    MeiSettings& s = g_mei;
    char key[64]; char sval[128]; int iv; float fv;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        // read key + rest of line
        if (sscanf(line, "%63s", key) != 1) continue;
        #define GI(k, field) if (!strcmp(key,k) && sscanf(line,"%*s %d",&iv)==1) { s.field = iv; continue; }
        #define GF(k, field) if (!strcmp(key,k) && sscanf(line,"%*s %f",&fv)==1) { s.field = fv; continue; }
        GI("master", master_enabled) GI("aim_enabled", aim_enabled) GI("aim_mode", aim_mode)
        GF("aim_fov", aim_fov) GI("aim_team_check", aim_team_check) GI("aim_bullet_tp", aim_bullet_tp)
        GF("aim_smooth", aim_smooth) GI("aim_target_head", aim_target_head)
        GF("aim_head_z", aim_head_z)
        GI("chams_enabled", chams_enabled) GI("chams_team_color", chams_team_color)
        GI("chams_highlight", chams_highlight) GI("chams_style", chams_style)
        if (!strcmp(key, "chams_col") && sscanf(line, "%*s %f %f %f", &s.chams_col[0], &s.chams_col[1], &s.chams_col[2]) == 3) continue;
        if (!strcmp(key, "chams_col2") && sscanf(line, "%*s %f %f %f", &s.chams_col2[0], &s.chams_col2[1], &s.chams_col2[2]) == 3) continue;
        GI("esp_enabled", esp_enabled) GI("esp_box", esp_box) GI("esp_name", esp_name)
        GI("esp_dist", esp_dist) GI("esp_health", esp_health) GI("esp_role", esp_role)
        GI("esp_crosshair", esp_crosshair) GI("esp_fov_circle", esp_fov_circle)
        GF("esp_fov", esp_fov) GF("esp_max_dist", esp_max_dist)
        GI("no_recoil", no_recoil) GI("perfect_accuracy", perfect_accuracy)
        GI("rapid_fire", rapid_fire) GI("force_auto", force_auto) GI("no_reload", no_reload)
        GI("infinite_ammo", infinite_ammo) GI("trigger_kill", trigger_kill)
        GI("kill_aura", kill_aura) GI("kill_all_loop", kill_all_loop) GI("wallbang", wallbang) GF("aura_rate", aura_rate)
        GI("move_enabled", move_enabled) GI("noclip", noclip) GI("anti_flash", anti_flash)
        GF("move_sprint", move_sprint) GF("move_ads", move_ads)
        GF("move_walk", move_walk) GF("move_crouch", move_crouch) GF("fly_speed", fly_speed)
        GI("godmode", godmode) GI("dev_tag", dev_tag) GI("force_vote", force_vote) GI("homing_knife", homing_knife)
        GI("name_enabled", name_enabled) GI("voice_enabled", voice_enabled) GI("voice_unmute", voice_unmute)
        GI("sb_loop", sb_loop) GI("sb_mix_mic", sb_mix_mic) GI("sb_monitor", sb_monitor) GI("sb_transmit", sb_transmit) GF("sb_gain", sb_gain)
        GF("panel_dist", panel_dist) GF("panel_scale", panel_scale)
        GI("ui_accent", ui_accent) GI("ui_dark", ui_dark) GI("panel_custom", panel_custom)
        if (!strcmp(key, "panel_off") &&
            sscanf(line, "%*s %f %f %f", &s.panel_off[0], &s.panel_off[1], &s.panel_off[2]) == 3) continue;
        if (!strcmp(key, "name_text") && sscanf(line, "%*s %127s", sval) == 1) {
            if (strcmp(sval, "-")) { strncpy(s.name_text, sval, MEI_NAME_MAX-1); s.name_text[MEI_NAME_MAX-1]=0; }
        }
        if (!strcmp(key, "skin_name") && sscanf(line, "%*s %127s", sval) == 1) {
            if (strcmp(sval, "-")) { strncpy(s.skin_name, sval, MEI_NAME_MAX-1); s.skin_name[MEI_NAME_MAX-1]=0; }
        }
        if (!strcmp(key, "username") && sscanf(line, "%*s %127s", sval) == 1) {
            if (strcmp(sval, "-")) { strncpy(s.username, sval, MEI_NAME_MAX-1); s.username[MEI_NAME_MAX-1]=0; }
        }
        #undef GI
        #undef GF
    }
    fclose(f);
    SLOG("loaded (master=%d aim=%d mode=%d chams=%d)", s.master_enabled, s.aim_enabled, s.aim_mode, s.chams_enabled);
}
