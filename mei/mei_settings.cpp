// mei_settings.cpp — the shared MeiSettings instance + flat key=value persistence to mei.cfg.
#include "mei_settings.h"
#include <cstdio>
#include <cstdlib>
#include <android/log.h>

#define SLOG(...) __android_log_print(ANDROID_LOG_INFO, "MEI-CFG", __VA_ARGS__)

MeiSettings g_mei;

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
    fprintf(f, "no_recoil %d\n",       s.no_recoil);
    fprintf(f, "perfect_accuracy %d\n",s.perfect_accuracy);
    fprintf(f, "rapid_fire %d\n",      s.rapid_fire);
    fprintf(f, "force_auto %d\n",      s.force_auto);
    fprintf(f, "no_reload %d\n",       s.no_reload);
    fprintf(f, "infinite_ammo %d\n",   s.infinite_ammo);
    fprintf(f, "trigger_kill %d\n",    s.trigger_kill);
    fprintf(f, "kill_aura %d\n",       s.kill_aura);
    fprintf(f, "wallbang %d\n",        s.wallbang);
    fprintf(f, "aura_rate %.1f\n",     s.aura_rate);
    fprintf(f, "move_enabled %d\n",    s.move_enabled);
    fprintf(f, "move_sprint %.3f\n",   s.move_sprint);
    fprintf(f, "move_ads %.3f\n",      s.move_ads);
    fprintf(f, "move_walk %.3f\n",     s.move_walk);
    fprintf(f, "godmode %d\n",         s.godmode);
    fprintf(f, "dev_tag %d\n",         s.dev_tag);
    fprintf(f, "homing_knife %d\n",    s.homing_knife);
    fprintf(f, "name_enabled %d\n",    s.name_enabled);
    fprintf(f, "name_text %s\n",       s.name_text[0] ? s.name_text : "-");
    fprintf(f, "panel_dist %.3f\n",    s.panel_dist);
    fprintf(f, "panel_scale %.3f\n",   s.panel_scale);
    fprintf(f, "ui_accent %d\n",       s.ui_accent);
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
        GI("chams_highlight", chams_highlight)
        GI("no_recoil", no_recoil) GI("perfect_accuracy", perfect_accuracy)
        GI("rapid_fire", rapid_fire) GI("force_auto", force_auto) GI("no_reload", no_reload)
        GI("infinite_ammo", infinite_ammo) GI("trigger_kill", trigger_kill)
        GI("kill_aura", kill_aura) GI("wallbang", wallbang) GF("aura_rate", aura_rate)
        GI("move_enabled", move_enabled) GF("move_sprint", move_sprint) GF("move_ads", move_ads)
        GF("move_walk", move_walk)
        GI("godmode", godmode) GI("dev_tag", dev_tag) GI("homing_knife", homing_knife)
        GI("name_enabled", name_enabled) GF("panel_dist", panel_dist) GF("panel_scale", panel_scale)
        GI("ui_accent", ui_accent) GI("panel_custom", panel_custom)
        if (!strcmp(key, "panel_off") &&
            sscanf(line, "%*s %f %f %f", &s.panel_off[0], &s.panel_off[1], &s.panel_off[2]) == 3) continue;
        if (!strcmp(key, "name_text") && sscanf(line, "%*s %127s", sval) == 1) {
            if (strcmp(sval, "-")) { strncpy(s.name_text, sval, MEI_NAME_MAX-1); s.name_text[MEI_NAME_MAX-1]=0; }
        }
        #undef GI
        #undef GF
    }
    fclose(f);
    SLOG("loaded (master=%d aim=%d mode=%d chams=%d)", s.master_enabled, s.aim_enabled, s.aim_mode, s.chams_enabled);
}
