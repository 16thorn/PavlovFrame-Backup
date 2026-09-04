// mei_menu.cpp — "mei mei [private]". Classic cheat-menu look (CSGO/Osiris style): left tab rail,
// bordered group-boxes, standard Dear ImGui widgets, dense, no prose. Bound to g_mei; debounce-saves.
#include "imgui.h"
#include "mei_menu.h"
#include "mei_settings.h"
#include "mei_esp.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <time.h>

float mei_xr_lift();   // right-stick Y (deadzoned) from mei_xr — reused here for joystick scroll

// ---- theme palette (runtime dark/light switch, MILTON-style) -----------------
static ImU32 C_WINBG, C_CARD, C_RAIL, C_HEAD, C_TEXT, C_MUTE, C_TRACK, C_BORDER, C_GREEN, C_KNOB;
static void set_theme(bool dark){
    if(dark){
        C_WINBG=IM_COL32(0x1C,0x16,0x1D,0xFF); C_CARD =IM_COL32(0x27,0x1E,0x29,0xFF);
        C_RAIL =IM_COL32(0x18,0x13,0x19,0xFF); C_HEAD =IM_COL32(0x22,0x1A,0x24,0xFF);
        C_TEXT =IM_COL32(0xED,0xE7,0xEE,0xFF); C_MUTE =IM_COL32(0x8C,0x82,0x90,0xFF);
        C_TRACK=IM_COL32(0x3A,0x30,0x3D,0xFF); C_BORDER=IM_COL32(0x33,0x2A,0x36,0xFF);
        C_GREEN=IM_COL32(0x55,0xCE,0x8C,0xFF); C_KNOB =IM_COL32(0xFF,0xFF,0xFF,0xFF);
    } else {
        C_WINBG=IM_COL32(0xEC,0xE8,0xEB,0xFF); C_CARD =IM_COL32(0xFF,0xFF,0xFF,0xFF);
        C_RAIL =IM_COL32(0xF4,0xF1,0xF3,0xFF); C_HEAD =IM_COL32(0xE7,0xE2,0xE6,0xFF);
        C_TEXT =IM_COL32(0x32,0x30,0x38,0xFF); C_MUTE =IM_COL32(0x9C,0x98,0xA4,0xFF);
        C_TRACK=IM_COL32(0xDD,0xD9,0xE0,0xFF); C_BORDER=IM_COL32(0xE3,0xDF,0xE5,0xFF);
        C_GREEN=IM_COL32(0x2E,0xA8,0x6A,0xFF); C_KNOB =IM_COL32(0xFF,0xFF,0xFF,0xFF);
    }
}
// lerp two packed colors by t (0..1)
static ImU32 lerpc(ImU32 a, ImU32 b, float t){ if(t<0)t=0; if(t>1)t=1;
    int ar=(a>>IM_COL32_R_SHIFT)&0xFF, ag=(a>>IM_COL32_G_SHIFT)&0xFF, ab=(a>>IM_COL32_B_SHIFT)&0xFF, aa=(a>>IM_COL32_A_SHIFT)&0xFF;
    int br=(b>>IM_COL32_R_SHIFT)&0xFF, bg=(b>>IM_COL32_G_SHIFT)&0xFF, bb=(b>>IM_COL32_B_SHIFT)&0xFF, ba=(b>>IM_COL32_A_SHIFT)&0xFF;
    return IM_COL32(ar+(br-ar)*t, ag+(bg-ag)*t, ab+(bb-ab)*t, aa+(ba-aa)*t); }

// ---- accents (light-theme; red default, matches the reference) ---------------
struct Accent { const char* name; ImU32 base; };
static const Accent ACCENTS[] = {
    { "red",    IM_COL32(0xEE,0x5A,0x5F,0xFF) },
    { "pink",   IM_COL32(0xF0,0x5D,0x9A,0xFF) },
    { "orange", IM_COL32(0xF2,0x8C,0x3A,0xFF) },
    { "violet", IM_COL32(0x8B,0x6C,0xF0,0xFF) },
    { "blue",   IM_COL32(0x3E,0x8E,0xF0,0xFF) },
    { "teal",   IM_COL32(0x22,0xC0,0xB0,0xFF) },
};
static const int N_ACCENT = 6;
static int  accent_idx() { int i = g_mei.ui_accent % N_ACCENT; return i < 0 ? i + N_ACCENT : i; }
static ImU32 ACC() { return ACCENTS[accent_idx()].base; }
static ImU32 ACC_SOFT() { ImU32 c=ACC(); return IM_COL32((c>>IM_COL32_R_SHIFT)&0xFF,(c>>IM_COL32_G_SHIFT)&0xFF,(c>>IM_COL32_B_SHIFT)&0xFF,0x22); }
static ImVec4 v4(ImU32 c){ return ImVec4(((c>>IM_COL32_R_SHIFT)&0xFF)/255.f,((c>>IM_COL32_G_SHIFT)&0xFF)/255.f,((c>>IM_COL32_B_SHIFT)&0xFF)/255.f,((c>>IM_COL32_A_SHIFT)&0xFF)/255.f); }
static ImVec4 v4a(ImU32 c, float a){ ImVec4 x=v4(c); x.w=a; return x; }

void mei_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 14.f; s.WindowBorderSize = 0.f; s.WindowPadding = ImVec2(0,0);
    s.ChildRounding = 12.f; s.FrameRounding = 8.f; s.GrabRounding = 10.f; s.PopupRounding = 10.f;
    s.ScrollbarRounding = 8.f; s.TabRounding = 8.f; s.ScrollbarSize = 12.f;
    s.FramePadding = ImVec2(11,9); s.ItemSpacing = ImVec2(10,14); s.ItemInnerSpacing = ImVec2(8,6);
    s.ChildBorderSize = 0.f; s.FrameBorderSize = 0.f; s.GrabMinSize = 18.f;
    ImVec4* c = s.Colors;
    ImVec4 acc = v4(ACC());
    c[ImGuiCol_WindowBg]        = v4(C_WINBG);
    c[ImGuiCol_ChildBg]         = v4(C_CARD);
    c[ImGuiCol_PopupBg]         = v4(C_CARD);
    c[ImGuiCol_Border]          = v4(C_BORDER);
    c[ImGuiCol_Text]            = v4(C_TEXT);
    c[ImGuiCol_TextDisabled]    = v4(C_MUTE);
    c[ImGuiCol_FrameBg]         = v4(C_TRACK);
    c[ImGuiCol_FrameBgHovered]  = v4(IM_COL32(0xD3,0xCF,0xD7,0xFF));
    c[ImGuiCol_FrameBgActive]   = v4(IM_COL32(0xCE,0xCA,0xD3,0xFF));
    c[ImGuiCol_Button]          = v4(C_TRACK);                        // theme-aware (was hardcoded light)
    c[ImGuiCol_ButtonHovered]   = v4(lerpc(C_TRACK, ACC(), 0.30f));
    c[ImGuiCol_ButtonActive]    = acc;
    c[ImGuiCol_CheckMark]       = acc;
    c[ImGuiCol_SliderGrab]      = acc;
    c[ImGuiCol_SliderGrabActive]= acc;
    c[ImGuiCol_Header]          = v4(ACC_SOFT());
    c[ImGuiCol_HeaderHovered]   = v4a(ACC(),0.28f);
    c[ImGuiCol_HeaderActive]    = v4a(ACC(),0.40f);
    c[ImGuiCol_Separator]       = v4(C_BORDER);
    c[ImGuiCol_ScrollbarBg]     = v4(IM_COL32(0,0,0,0));
    c[ImGuiCol_ScrollbarGrab]   = v4(IM_COL32(0xD0,0xCC,0xD4,0xFF));
    c[ImGuiCol_ScrollbarGrabHovered] = acc;
    ImGui::GetIO().FontGlobalScale = 1.2f;   // VR readability
}

// ---- save debounce -----------------------------------------------------------
static bool g_dirty=false; static long g_last_ms=0;
static long now_ms(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
static void touched(){ g_dirty=true; g_last_ms=now_ms(); }

// ---- widgets (custom-drawn: animated pill toggle, chip slider, rounded card) --
// animated pill: knob slides + track color fades between off/on using per-widget storage.
static bool pill_draw(const char* l, bool* v, float badgeLeft){
    const float w=46.f, h=25.f, r=h*0.5f;
    float rightX = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(); ImGui::SetCursorPosX(rightX - w);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(l, ImVec2(w,h));
    bool clicked = ImGui::IsItemClicked();
    if(clicked){ *v=!*v; touched(); }
    ImGuiID id = ImGui::GetID(l); ImGuiStorage* st = ImGui::GetStateStorage();
    float cur = st->GetFloat(id, *v?1.f:0.f), tgt=*v?1.f:0.f, dt=ImGui::GetIO().DeltaTime;
    cur += (tgt-cur)*(1.f-expf(-dt*18.f)); if(fabsf(tgt-cur)<0.002f) cur=tgt; st->SetFloat(id,cur);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(pos, ImVec2(pos.x+w,pos.y+h), lerpc(C_TRACK,ACC(),cur), r);
    d->AddCircleFilled(ImVec2(pos.x+r+(w-h)*cur, pos.y+r), r-3.5f, C_KNOB);
    if(badgeLeft>0.f){ ImVec2 bs=ImGui::CalcTextSize("RISK"); float bw=bs.x+14.f;
        ImVec2 bp=ImVec2(pos.x-bw-8.f, pos.y+(h-bs.y-6.f)*0.5f);
        d->AddRectFilled(bp, ImVec2(bp.x+bw,bp.y+bs.y+6.f), ACC_SOFT(), 5.f);
        d->AddText(ImVec2(bp.x+7,bp.y+3), ACC(), "RISK"); }
    return clicked;
}
static bool Chk(const char* l, bool* v){
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(l);
    return pill_draw(l, v, 0.f);
}
// ban-risk toggle: RISK badge + a confirmation popup on enable.
static const char* g_risk_name = nullptr; static bool* g_risk_ptr = nullptr; static bool g_risk_open = false;
static bool ChkRisk(const char* l, bool* v, const char* riskName){
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(l);
    bool was = *v;
    bool clicked = pill_draw(l, v, 1.f);       // pill_draw flips *v
    if(clicked && !was && *v){                 // user just turned it ON -> keep OFF until they confirm
        *v = false;
        g_risk_name = riskName; g_risk_ptr = v; g_risk_open = true;
    }
    return clicked;
}
static bool Sl(const char* l, float* v, float lo, float hi, const char* fmt){
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(l);
    char val[24]; snprintf(val,sizeof val,fmt,*v);
    ImVec2 ts=ImGui::CalcTextSize(val); float chipW=ts.x+18.f, chipH=ts.y+8.f;
    float rightX=ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(); ImGui::SetCursorPosX(rightX-chipW);
    ImVec2 cp=ImGui::GetCursorScreenPos();
    ImDrawList* d=ImGui::GetWindowDrawList();
    d->AddRectFilled(cp, ImVec2(cp.x+chipW,cp.y+chipH), ACC_SOFT(), 6.f);
    d->AddText(ImVec2(cp.x+9, cp.y+4), ACC(), val);
    ImGui::NewLine();
    char id[64]; snprintf(id,sizeof id,"##%s",l);
    ImVec2 tp=ImGui::GetCursorScreenPos();
    float tw=ImGui::GetContentRegionAvail().x, th=8.f, kr=10.f, rowH=kr*2.f;
    ImGui::InvisibleButton(id, ImVec2(tw,rowH));
    bool ch=false;
    if(ImGui::IsItemActive()){ float t=(ImGui::GetIO().MousePos.x-tp.x)/tw; if(t<0)t=0; if(t>1)t=1;
        float nv=lo+t*(hi-lo); if(nv!=*v){*v=nv; touched(); ch=true;} }
    float t=(*v-lo)/(hi-lo); if(t<0)t=0; if(t>1)t=1;
    float cy=tp.y+rowH*0.5f;
    d->AddRectFilled(ImVec2(tp.x,cy-th*0.5f), ImVec2(tp.x+tw,cy+th*0.5f), C_TRACK, th*0.5f);
    if(t>0.001f) d->AddRectFilled(ImVec2(tp.x,cy-th*0.5f), ImVec2(tp.x+tw*t,cy+th*0.5f), ACC(), th*0.5f);
    d->AddCircleFilled(ImVec2(tp.x+tw*t,cy), kr, C_KNOB);
    d->AddCircle(ImVec2(tp.x+tw*t,cy), kr, ACC(), 0, 2.5f);
    return ch;
}
static void gb_begin(const char* title){
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20,16));        // generous inner padding
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(C_CARD));                    // card on the content ground
    ImGui::BeginChild(title, ImVec2(-16,0), ImGuiChildFlags_AutoResizeY);   // -16 = gap so cards aren't on the edge
    ImGui::Dummy(ImVec2(0,1));
    ImGui::TextColored(v4(C_MUTE), "%s", title);
    ImGui::Dummy(ImVec2(0,3));
}
static void gb_end(){ ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopStyleVar(); ImGui::Spacing(); }
static void NEXTCOL(){ ImGui::NextColumn(); }   // jump to the right card column
// MILTON-style dropdown: label left, value box + chevron right.
static bool Dropdown(const char* label, int* v, const char** items, int n){
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
    float boxW = 130.f, rightX = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(); ImGui::SetCursorPosX(rightX - boxW);
    ImGui::PushItemWidth(boxW);
    char id[80]; snprintf(id,sizeof id,"##dd%s",label);
    bool ch=false;
    if(ImGui::BeginCombo(id, (*v>=0&&*v<n)?items[*v]:"", ImGuiComboFlags_None)){
        for(int i=0;i<n;i++){ bool sel=(*v==i);
            if(ImGui::Selectable(items[i],sel)){ *v=i; touched(); ch=true; }
            if(sel) ImGui::SetItemDefaultFocus(); }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    return ch;
}

// ---- on-screen keyboard ------------------------------------------------------
static bool g_kb=false, g_kb2=false, g_shift=false;
static void kb_append(char* b,int cap,char ch){ int n=(int)strlen(b); if(n<cap-1){b[n]=ch;b[n+1]=0;touched();} }
static void keyboard(char* buf,int cap){
    const char* rows[4]={"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
    const char* digsym ="!@#$%^&*()";     // shift on the number row -> standard US symbols
    const char* symrow ="@_-.!#$%&+";      // dedicated symbol row (literal, shift-independent)
    for(int r=0;r<4;r++){
        if(r==2) ImGui::Indent(26.f); if(r==3) ImGui::Indent(52.f);
        for(int i=0; rows[r][i]; i++){
            char ch=rows[r][i];
            if(r==0 && g_shift) ch=digsym[i];              // 1->! 2->@ ... on shift
            else if(!g_shift && ch>='A'&&ch<='Z') ch+=32;  // letters lowercase unless shift
            char lab[8]; snprintf(lab,sizeof lab,"%c##k%d_%d",ch,r,i);   // unique id (avoid dup-char clash)
            if(ImGui::Button(lab, ImVec2(52,46))) kb_append(buf,cap,ch);
            ImGui::SameLine();
        }
        if(r==2) ImGui::Unindent(26.f); if(r==3) ImGui::Unindent(52.f);
        ImGui::NewLine();
    }
    for(int i=0; symrow[i]; i++){                          // symbols row (always available: @ _ - . etc)
        char lab[8]; snprintf(lab,sizeof lab,"%c##sym%d",symrow[i],i);
        if(ImGui::Button(lab, ImVec2(52,46))) kb_append(buf,cap,symrow[i]);
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if(ImGui::Button(g_shift?"SHIFT":"shift",ImVec2(82,46))) g_shift=!g_shift; ImGui::SameLine();
    if(ImGui::Button("space",ImVec2(220,46))) kb_append(buf,cap,' '); ImGui::SameLine();
    if(ImGui::Button("back",ImVec2(82,46))){ int n=(int)strlen(buf); if(n>0){buf[n-1]=0;touched();} } ImGui::SameLine();
    if(ImGui::Button("clear",ImVec2(82,46))){ buf[0]=0; touched(); } ImGui::SameLine();
    if(ImGui::Button("done",ImVec2(82,46))){ g_kb=false; g_kb2=false; }
}

// ---- tabs --------------------------------------------------------------------
static void tab_aimbot(){
    gb_begin("SETTINGS");
    Chk("Enabled", &g_mei.aim_enabled);
    const char* modes[]={"Off","On fire","Continuous"};
    Dropdown("Mode", &g_mei.aim_mode, modes, 3);
    ImGui::BeginDisabled(!g_mei.aim_enabled);
    Chk("Aim at head",&g_mei.aim_target_head);
    Chk("Team check",&g_mei.aim_team_check);
    Chk("Bullet teleport",&g_mei.aim_bullet_tp);
    ImGui::EndDisabled();
    gb_end();
    NEXTCOL();
    gb_begin("FOV & AIM");
    ImGui::BeginDisabled(!g_mei.aim_enabled);
    Sl("FOV Radius",&g_mei.aim_fov,1.f,180.f,"%.0f");
    Sl("Smoothing",&g_mei.aim_smooth,0.f,0.95f,"%.2f");
    Sl("Head offset",&g_mei.aim_head_z,-25.f,10.f,"%.0f cm");
    ImGui::EndDisabled();
    gb_end();
}
static void tab_visuals(){
    gb_begin("CHAMS");
    Chk("Enabled",&g_mei.chams_enabled);
    ImGui::BeginDisabled(!g_mei.chams_enabled);
    const char* styles[] = { "Team colors","Single A","Single B","Flash","Target only","Custom color" };
    Dropdown("Style", &g_mei.chams_style, styles, 6);
    if (g_mei.chams_style == 5) {
        if (ImGui::ColorEdit3("Team 0", g_mei.chams_col,  ImGuiColorEditFlags_NoInputs)) touched();
        if (ImGui::ColorEdit3("Team 1", g_mei.chams_col2, ImGuiColorEditFlags_NoInputs)) touched();
        ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "recolor each team (see-through tint)");
    }
    Chk("Highlight target",&g_mei.chams_highlight);
    Chk("Skip dead",&g_mei.chams_skip_dead);
    ImGui::EndDisabled();
    gb_end();
    NEXTCOL();
    gb_begin("ESP OVERLAY");
    Chk("Enabled",&g_mei.esp_enabled);
    ImGui::BeginDisabled(!g_mei.esp_enabled);
    Chk("Box",&g_mei.esp_box);
    Chk("Name",&g_mei.esp_name);
    Chk("Distance",&g_mei.esp_dist);
    Chk("Health bar",&g_mei.esp_health);
    Chk("Role",&g_mei.esp_role);
    Chk("Crosshair",&g_mei.esp_crosshair);
    Chk("FOV circle",&g_mei.esp_fov_circle);
    Sl("Calibrate FOV",&g_mei.esp_fov,60.f,130.f,"%.0f");
    Sl("Max distance",&g_mei.esp_max_dist,20.f,400.f,"%.0f m");
    ImGui::EndDisabled();
    gb_end();
}
static void tab_weapon(){
    gb_begin("RECOIL");
    Chk("No recoil",&g_mei.no_recoil);
    Chk("Perfect accuracy",&g_mei.perfect_accuracy);
    gb_end();
    gb_begin("FIRE");
    Chk("Rapid fire",&g_mei.rapid_fire);
    Chk("Force full-auto",&g_mei.force_auto);
    Chk("No reload cooldown",&g_mei.no_reload);
    Chk("Infinite ammo",&g_mei.infinite_ammo);
    gb_end();
    NEXTCOL();
    gb_begin("COMBAT");
    ChkRisk("Trigger kill",&g_mei.trigger_kill,"Trigger kill");
    ChkRisk("Kill aura",&g_mei.kill_aura,"Kill aura");
    ImGui::BeginDisabled(!g_mei.kill_aura);
    Sl("Aura rate",&g_mei.aura_rate,40.f,500.f,"%.0f ms");
    ImGui::EndDisabled();
    ChkRisk("Wallbang (shoot thru walls)",&g_mei.wallbang,"Wallbang");
    gb_end();
    gb_begin("TARGET KILL");
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "pick a player, press Kill (weaponless works). grey = too far.");
    {
        int n = g_players_n; if (n > MEI_PLAYERS_MAX) n = MEI_PLAYERS_MAX;
        if (g_mei.kill_sel >= n) g_mei.kill_sel = -1;
        if (n <= 0) ImGui::TextDisabled("(no players — join a match)");
        else {
            ImGui::BeginChild("plist", ImVec2(0,170), true);
            for (int i = 0; i < n; i++) {
                const MeiPlayer& p = g_players[i];
                char label[128]; char png[16];
                if (p.ping >= 0) snprintf(png, sizeof png, " %dms", p.ping); else png[0] = 0;
                snprintf(label, sizeof label, "%s   [T%d]%s%s%s%s", p.name, p.team, png,
                         p.dev ? "  [DEV]" : "", p.loaded ? "" : "  (far)", p.alive ? "" : "  (dead)");
                ImGui::PushStyleColor(ImGuiCol_Text,
                    v4(p.loaded ? IM_COL32(0xE2,0xE2,0xEA,0xFF) : IM_COL32(0x6E,0x6E,0x78,0xFF)));
                if (ImGui::Selectable(label, g_mei.kill_sel == i)) { g_mei.kill_sel = i; touched(); }
                ImGui::PopStyleColor();
                if (p.platform[0]) { ImGui::SameLine();
                    ImGui::TextColored(v4(IM_COL32(0x5A,0x5A,0x66,0xFF)), " %s", p.platform); }
            }
            ImGui::EndChild();
        }
        bool sel_ok = (g_mei.kill_sel >= 0 && g_mei.kill_sel < n);
        ImGui::BeginDisabled(!sel_ok);   // only require a selection; the kill fn re-checks loaded/alive safely
        if (ImGui::Button("KILL SELECTED", ImVec2(200,46))) g_mei.act_kill_sel = true;
        ImGui::EndDisabled();
        if (sel_ok && !g_players[g_mei.kill_sel].loaded) {
            ImGui::SameLine(); ImGui::TextColored(v4(IM_COL32(0xC8,0x66,0x66,0xFF)), "far — may not land"); }
        ImGui::Separator();
        ChkRisk("KILL ALL (repeat — kills everyone loaded)", &g_mei.kill_all_loop, "Kill All");
        ImGui::TextColored(v4(IM_COL32(0xC8,0x66,0x66,0xFF)), "loud / very detectable — ban risk");
    }
    gb_end();
}
static void tab_movement(){
    gb_begin("MOVEMENT");
    Chk("Enabled",&g_mei.move_enabled);
    ImGui::BeginDisabled(!g_mei.move_enabled);
    Sl("Sprint",&g_mei.move_sprint,1.f,20.f,"%.1fx");
    Sl("ADS",&g_mei.move_ads,1.f,20.f,"%.1fx");
    Sl("Walk",&g_mei.move_walk,1.f,20.f,"%.1fx");
    Sl("Crouch",&g_mei.move_crouch,1.f,20.f,"%.1fx");
    ImGui::EndDisabled();
    Chk("Noclip (fly + no collision)",&g_mei.noclip);
    ImGui::BeginDisabled(!g_mei.noclip);
    Sl("Fly speed",&g_mei.fly_speed,1.f,15.f,"%.1fx");
    ImGui::EndDisabled();
    gb_end();
}
static void tab_player(){
    gb_begin("PLAYER");
    Chk("Godmode",&g_mei.godmode);
    Chk("Dev tag",&g_mei.dev_tag);
    Chk("Vote unlock",&g_mei.force_vote);
    Chk("Anti flash",&g_mei.anti_flash);
    Chk("Homing knife",&g_mei.homing_knife);
    gb_end();
    NEXTCOL();
    gb_begin("NAME CHANGER");
    Chk("Enabled",&g_mei.name_enabled);
    ImGui::BeginDisabled(!g_mei.name_enabled);
    ImGui::PushItemWidth(-1.f);
    if(ImGui::InputText("##name",g_mei.name_text,MEI_NAME_MAX)) touched();
    if(ImGui::IsItemActivated()) g_kb=true;           // auto-open keyboard on tap
    ImGui::PopItemWidth();
    if(g_kb){ ImGui::Spacing(); keyboard(g_mei.name_text,MEI_NAME_MAX); }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if(ImGui::Button("Set SERVER name (RPC)",ImVec2(240,40))) g_mei.act_change_name=true;
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "sends ServerChangeName -> community servers may honor it (dodges name bans)");
    gb_end();
    gb_begin("VOICE CHAT (QUEST FIX)");
    Chk("Enable voice",&g_mei.voice_enabled);
    ImGui::BeginDisabled(!g_mei.voice_enabled);
    Chk("Unmute mic",&g_mei.voice_unmute);
    ImGui::EndDisabled();
    ImGui::Spacing();
    if(ImGui::Button("Dump voice state (log)",ImVec2(240,40))) g_mei.act_voice_diag=true;
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "forces Android mic capture + net voice on. dump feeds the log for tuning.");
    gb_end();
}
static void tab_ttt(){
    gb_begin("YOU");
    ImGui::Text("Role: %s", g_mei.my_role[0] ? g_mei.my_role : "?");
    if (g_mei.my_credits >= 0) ImGui::Text("Credits: %d", g_mei.my_credits);
    else ImGui::TextDisabled("Credits: ?");
    gb_end();
    gb_begin("PLAYERS");
    int n = g_esp_n; if (n > MEI_ESP_MAX) n = MEI_ESP_MAX;
    if (n == 0) ImGui::TextDisabled("(enable ESP and be in a match)");
    for (int i = 0; i < n; i++) { EspEntry& e = g_esp[i];
        ImGui::Text("%-14s %-9s %4.0fm", e.name[0] ? e.name : "player", e.role[0] ? e.role : "-", e.dist); }
    gb_end();
    NEXTCOL();
    gb_begin("BUY  (ServerBuy)");
    // EXACT in-game buy IDs, grouped. Click an ID -> ServerBuy fires. Filter box narrows the list.
    struct BuyCat { const char* name; const char** ids; int n; };
    static const char* c_pistol[] = {"1911","57","cet9","de","goldengun","luger","m9","revolver","silentcet9","sock","tokarev","webley"};
    static const char* c_smg[]    = {"ak","ar9","detectivesmg","kross","mp40","mp5","p90","ppsh","skorpion","smg","sten","thompson","uzi"};
    static const char* c_shot[]   = {"autoshotgun","drumshotgun","sawedoff","shotgun","trenchgun"};
    static const char* c_rifle[]  = {"ak12","ak47","akshorty","ar","aug","autosniper","g43","galul","m16","m1garand","sks","stg44","svt40","vanas"};
    static const char* c_lmg[]    = {"bar","bren","dp27","lmga","mg42","pkm","tankmg"};
    static const char* c_sniper[] = {"antitank","awp","hunting","kar98","leeenfield","mosin","scur","springfield","vzz"};
    static const char* c_rl[]     = {"rl_m1a1","rl_panzer","rl_piat","rl_rpg","tankturret"};
    static const char* c_knife[]  = {"knife","tttknife","ww2knife"};
    static const char* c_special[]= {"flaregun","newtonlauncher","taser","tranqgun"};
    static const char* c_atts[]   = {"acog","bayonet_kar98","bayonet_leeenfield","bayonet_m1garand","bayonet_mosin","bayonet_springfield","bayonet_trenchgun","canted_reddot","flashlight_rifle","grip_angled","grip_vertical","holo","laser_pistol","laser_rifle","reddot","reddot_pistol","scope","scope_kar98","scope_leeenfield","scope_mosin","scope_springfield","supp_pistol","supp_rifle"};
    static const char* c_meds[]   = {"adrenaline","bandage","medkit","painkillers","syringe","ww2bandage","ww2medkit","ww2painkillers","ww2syringe"};
    static const char* c_nades[]  = {"flash","flash_aurora","flash_ru","grenade","grenade_aurora","grenade_dis","grenade_ger","grenade_ru","grenade_svt","grenade_us","smoke","smoke_ger","smoke_ru","smoke_svt","smoke_us"};
    static const char* c_mines[]  = {"antipersonnelmine","antitankmine","tripalarm"};
    static const char* c_ammo[]   = {"ammo_pistol","ammo_rifle","ammo_shotgun","ammo_smg","ammo_sniper","ammo_special","ammocrate"};
    static const char* c_other[]  = {"armour","ballisticsshield","boltcutters","cloakdisrupter","crowbar","dnascanner","handcuffs","healthstation","kevlarhelmet","keycard","lockpick","monocular","pickaxe","pipe","pliers","pushBomb","repairtool","skinhelmet_ger","skinhelmet_svt","skinhelmet_us","snowball","teleporter","tttc4"};
    #define BC(a) a, (int)(sizeof(a)/sizeof(a[0]))
    static const BuyCat CATS[] = {
        {"Pistols",BC(c_pistol)},{"SMGs",BC(c_smg)},{"Shotguns",BC(c_shot)},{"Rifles",BC(c_rifle)},
        {"LMGs",BC(c_lmg)},{"Snipers",BC(c_sniper)},{"Rocket Launchers",BC(c_rl)},{"Knives",BC(c_knife)},
        {"Special",BC(c_special)},{"Attachments",BC(c_atts)},{"Meds",BC(c_meds)},{"Grenades",BC(c_nades)},
        {"Mines",BC(c_mines)},{"Ammo",BC(c_ammo)},{"Other",BC(c_other)} };
    #undef BC
    static char filter[32] = {0};
    ImGui::TextUnformatted("Search:"); ImGui::SameLine(); ImGui::PushItemWidth(-90.f);
    if (ImGui::InputText("##buyfilter", filter, sizeof filter)) {}
    if (ImGui::IsItemActivated()) g_kb2 = true;
    ImGui::PopItemWidth(); ImGui::SameLine();
    if (ImGui::Button("clear##bf", ImVec2(78,0))) filter[0]=0;
    char flo[32]; for (int i=0; (flo[i]=(char)tolower((unsigned char)filter[i])); i++) {}
    ImGui::BeginChild("buylist", ImVec2(0, 300), true);
    for (int ci = 0; ci < (int)(sizeof(CATS)/sizeof(CATS[0])); ci++) {
        const BuyCat& c = CATS[ci];
        bool headerDrawn = false;
        for (int i = 0; i < c.n; i++) {
            if (flo[0]) { char lo[48]; int j=0; for(; c.ids[i][j] && j<47; j++) lo[j]=(char)tolower((unsigned char)c.ids[i][j]); lo[j]=0;
                          if (!strstr(lo, flo)) continue; }
            if (!headerDrawn) { headerDrawn = true; ImGui::TextColored(v4(ACC()), "%s", c.name); }
            ImGui::PushID(ci*100+i);
            if (ImGui::Selectable(c.ids[i], !strcmp(g_mei.buy_name, c.ids[i]))) {
                strncpy(g_mei.buy_name, c.ids[i], MEI_NAME_MAX-1); g_mei.buy_name[MEI_NAME_MAX-1]=0; g_mei.act_buy = true; }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::Text("selected: %s", g_mei.buy_name[0] ? g_mei.buy_name : "-"); ImGui::SameLine();
    if (ImGui::Button("BUY", ImVec2(120,40))) g_mei.act_buy = true;
    ImGui::SameLine();
    if (ImGui::Button("GIVE (free)", ImVec2(120,40))) g_mei.act_give = true;
    if (g_kb2) { ImGui::Spacing(); keyboard(filter, sizeof filter); }
    gb_end();
}
static void tab_soundboard(){
    gb_begin("VOICE TX  (ServerOnVoice)");
    if (!g_mei.sb_present)
        ImGui::TextColored(v4(IM_COL32(0xFF,0x6B,0x6B,0xFF)), "ServerOnVoice not resolved (join a match)");
    else
        ImGui::TextColored(v4(ACC()), "transmit path ready");
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)),
        g_mei.sb_rec_live ? "header sample: locked from live voice" : "header: default (hear someone talk to refine)");
    gb_end();

    NEXTCOL();
    gb_begin("CLIPS");
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "drop .wav into files/soundboard/  (any rate, mono/stereo)");
    ImGui::BeginChild("sb_list", ImVec2(0,190), true);
    if (g_mei.sb_n_clips == 0) ImGui::TextDisabled("(no clips)");
    for (int i = 0; i < g_mei.sb_n_clips && i < MEI_SB_MAX_CLIPS; i++) {
        ImGui::PushID(i);
        char label[MEI_SB_NAME_MAX + 8];
        snprintf(label, sizeof label, "%s%s", (i == g_mei.sb_cur ? "> " : "  "), g_mei.sb_names[i]);
        if (ImGui::Selectable(label, g_mei.sb_sel == i)) { g_mei.sb_sel = i; touched(); }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button(g_mei.sb_cur >= 0 ? "SENDING..." : "PLAY", ImVec2(150,44)) && g_mei.sb_sel >= 0) g_mei.sb_act_play = true;
    ImGui::SameLine();
    if (ImGui::Button("STOP", ImVec2(120,44))) g_mei.sb_act_stop = true;
    ImGui::SameLine();
    if (ImGui::Button("Rescan", ImVec2(120,44))) g_mei.sb_act_rescan = true;
    Chk("Loop", &g_mei.sb_loop);
    Chk("Monitor (hear it yourself)", &g_mei.sb_monitor);
    ChkRisk("Broadcast to lobby", &g_mei.sb_transmit, "Broadcast to lobby");
    Sl("Gain", &g_mei.sb_gain, 0.5f, 8.0f, "%.1fx");
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "gain applies on next PLAY (bakes into the encode)");
    gb_end();
}
static void tab_config(){
    gb_begin("GENERAL");
    Chk("Master enable",&g_mei.master_enabled);
    gb_end();
    gb_begin("SERVER  (RCON)");
    ImGui::TextColored(v4(C_MUTE), "admin RPC — works on dedicated servers if you're admin");
    if(ImGui::Button("Restart match", ImVec2(170,42))) g_mei.act_restart_match=true;
    ImGui::SameLine();
    if(ImGui::Button("Next map", ImVec2(150,42))) g_mei.act_next_map=true;
    gb_end();
    gb_begin("PANEL");
    Sl("Distance",&g_mei.panel_dist,0.35f,1.5f,"%.2f m");
    Sl("Scale",&g_mei.panel_scale,0.6f,2.2f,"%.2f");
    ImGui::TextUnformatted("Accent"); ImGui::SameLine();
    for(int i=0;i<N_ACCENT;i++){
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button,v4(ACCENTS[i].base));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,v4(ACCENTS[i].base));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,v4(ACCENTS[i].base));
        if(ImGui::Button("  ",ImVec2(34,26))){ g_mei.ui_accent=i; touched(); mei_style(); }
        ImGui::PopStyleColor(3); ImGui::PopID();
        if(i==accent_idx()){ ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x-2,a.y-2),ImVec2(b.x+2,b.y+2),IM_COL32(255,255,255,255),3.f,0,2.f); }
        if(i<N_ACCENT-1) ImGui::SameLine();
    }
    ImGui::Spacing();
    if(g_mei.reposition) ImGui::PushStyleColor(ImGuiCol_Button, v4(ACC()));
    if(ImGui::Button(g_mei.reposition?"Exit move mode":"Move panel",ImVec2(150,36))){ g_mei.reposition=!g_mei.reposition; touched(); }
    if(g_mei.reposition) ImGui::PopStyleColor();
    ImGui::SameLine();
    if(ImGui::Button("Reset position",ImVec2(160,36))){ g_mei.reposition=false; g_mei.act_replace=true; touched(); }
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "Move: point OFF the panel, hold trigger, drag. Release to drop.");
    gb_end();
    NEXTCOL();
    gb_begin("TOOLS");
    if(ImGui::Button("Refresh mods",ImVec2(180,44))) g_mei.act_refresh=true;   // re-resolve guns/chams/movement
    ImGui::SameLine(); ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "re-hooks guns, chams & movement if they stop");
    if(ImGui::Button("Fix pawn",ImVec2(180,44))) g_mei.act_fixpawn=true;        // re-resolve gun/movement/controller
    ImGui::SameLine(); ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "press after death / lobby switch — keeps chams");
    if(ImGui::Button("Dump SDK",ImVec2(150,40))) g_mei.act_dump_sdk=true; ImGui::SameLine();
    if(ImGui::Button("Dump whitelist",ImVec2(190,40))) g_mei.act_dump_whitelist=true; ImGui::SameLine();
    if(ImGui::Button("Save",ImVec2(120,40))){ g_mei.act_save=true; g_dirty=false; }
    gb_end();
}

// ---- vector icons (drawn — no icon font needed) ------------------------------
enum { IC_NONE=0, IC_AIM, IC_WEAPON, IC_ESP, IC_MOVE, IC_PLAYER, IC_SOUND, IC_TTT, IC_CONFIG, IC_HOME, IC_USER };
static void draw_icon(ImDrawList* d, ImVec2 c, float r, int id, ImU32 col){
    switch(id){
    case IC_AIM: case IC_TTT: case IC_WEAPON:
        d->AddCircle(c,r,col,0,2.f);
        d->AddLine(ImVec2(c.x-r-2,c.y),ImVec2(c.x-r+3,c.y),col,2.f); d->AddLine(ImVec2(c.x+r-3,c.y),ImVec2(c.x+r+2,c.y),col,2.f);
        d->AddLine(ImVec2(c.x,c.y-r-2),ImVec2(c.x,c.y-r+3),col,2.f); d->AddLine(ImVec2(c.x,c.y+r-3),ImVec2(c.x,c.y+r+2),col,2.f);
        d->AddCircleFilled(c,1.8f,col); break;
    case IC_ESP:
        d->AddBezierQuadratic(ImVec2(c.x-r,c.y),ImVec2(c.x,c.y-r*0.9f),ImVec2(c.x+r,c.y),col,2.f);
        d->AddBezierQuadratic(ImVec2(c.x-r,c.y),ImVec2(c.x,c.y+r*0.9f),ImVec2(c.x+r,c.y),col,2.f);
        d->AddCircleFilled(c,2.4f,col); break;
    case IC_MOVE:
        d->AddTriangleFilled(ImVec2(c.x,c.y-r),ImVec2(c.x-4,c.y-2),ImVec2(c.x+4,c.y-2),col);
        d->AddTriangleFilled(ImVec2(c.x,c.y+r),ImVec2(c.x-4,c.y+2),ImVec2(c.x+4,c.y+2),col); break;
    case IC_PLAYER: case IC_USER:
        d->AddCircleFilled(ImVec2(c.x,c.y-r*0.4f),r*0.42f,col);
        d->AddBezierQuadratic(ImVec2(c.x-r*0.7f,c.y+r),ImVec2(c.x,c.y+r*0.1f),ImVec2(c.x+r*0.7f,c.y+r),col,2.2f); break;
    case IC_SOUND:
        d->AddRectFilled(ImVec2(c.x-r,c.y-3),ImVec2(c.x-2,c.y+3),col,1.f);
        d->AddTriangleFilled(ImVec2(c.x-2,c.y-5),ImVec2(c.x-2,c.y+5),ImVec2(c.x+3,c.y),col);
        d->AddBezierQuadratic(ImVec2(c.x+4,c.y-4),ImVec2(c.x+r+2,c.y),ImVec2(c.x+4,c.y+4),col,1.6f); break;
    case IC_CONFIG:
        d->AddCircle(c,r*0.55f,col,0,2.f);
        for(int i=0;i<6;i++){ float a=i*1.047f; d->AddLine(ImVec2(c.x+cosf(a)*r*0.7f,c.y+sinf(a)*r*0.7f),ImVec2(c.x+cosf(a)*r,c.y+sinf(a)*r),col,2.f);} break;
    case IC_HOME:
        d->AddTriangle(ImVec2(c.x-r,c.y-1),ImVec2(c.x,c.y-r),ImVec2(c.x+r,c.y-1),col,2.f);
        d->AddRect(ImVec2(c.x-r*0.7f,c.y-1),ImVec2(c.x+r*0.7f,c.y+r),col,1.f,0,2.f); break;
    }
}

// ---- frame -------------------------------------------------------------------
static int g_tab=0;
static int g_page=1;   // top-nav: 0=Home, 1=Modules(features), 2=Account
static const char* PAGES[]={"Home","Modules","Account"};
// sidebar nav: category headers (name set, tab<0) + items (tab>=0)
struct Nav { const char* label; int tab; int icon; };
static const Nav NAV[] = {
    {"COMBAT",-1,0}, {"Aimbot",0,IC_AIM}, {"Weapon",2,IC_WEAPON}, {"TTT",5,IC_TTT},
    {"VISUALS",-1,0}, {"Visuals",1,IC_ESP},
    {"GAME",-1,0}, {"Movement",3,IC_MOVE}, {"Player",4,IC_PLAYER}, {"Sound",6,IC_SOUND},
    {"SYSTEM",-1,0}, {"Config",7,IC_CONFIG},
};
static const int N_NAV = (int)(sizeof(NAV)/sizeof(NAV[0]));

// ---- app pages (top-nav): Home dashboard + Account ---------------------------
static void stat_card(const char* title, const char* val, ImU32 vc){
    gb_begin(title);
    ImGui::TextColored(v4(vc), "%s", val);
    ImGui::Dummy(ImVec2(0,2));
    gb_end();
}
static void home_page(){
    ImGui::SetCursorPosX(16); ImGui::Dummy(ImVec2(0,2));
    ImGui::TextColored(v4(C_TEXT), "Hi,"); ImGui::SameLine();
    ImGui::TextColored(v4(ACC()), "%s!", g_mei.username[0]?g_mei.username:"guest");
    ImGui::SetCursorPosX(16); ImGui::TextColored(v4(C_MUTE), "Welcome back - 2016 client");
    ImGui::Dummy(ImVec2(0,6));
    ImGui::Columns(3,"stats",false);
    stat_card("SUBSCRIPTION","PREM",ACC()); NEXTCOL();
    stat_card("DAYS LEFT","INF",C_TEXT);     NEXTCOL();
    stat_card("STATUS","UNDETECTED",C_GREEN);
    ImGui::Columns(1);
    gb_begin("WHAT'S NEW");
    ImGui::TextWrapped("Dark / light themes, animated toggles, ban-risk prompts, soundboard voice inject, 2-column layout, joystick scroll.");
    gb_end();
    ImGui::Columns(2,"guide",false);
    gb_begin("WHAT YOU CAN DO");
    struct G { int ic; const char* t; const char* d; };
    static const G gs[] = {
        {IC_AIM,   "Aimbot",  "silent aim, FOV, team-check, bullet TP"},
        {IC_ESP,   "Visuals", "chams + ESP box/name/health/role"},
        {IC_WEAPON,"Weapon",  "no-recoil, rapid, kills (RISK)"},
        {IC_MOVE,  "Movement","speed, crouch, noclip fly (offline)"},
    };
    for(auto& g : gs){ ImVec2 p=ImGui::GetCursorScreenPos();
        draw_icon(ImGui::GetWindowDrawList(), ImVec2(p.x+8,p.y+9), 8.f, g.ic, ACC());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()+26);
        ImGui::TextColored(v4(C_TEXT), "%s", g.t); ImGui::SameLine();
        ImGui::TextColored(v4(C_MUTE), "- %s", g.d); }
    gb_end();
    NEXTCOL();
    gb_begin("MENU TIPS");
    ImGui::TextColored(v4(C_MUTE), "L3 (left stick) opens/closes");
    ImGui::TextColored(v4(C_MUTE), "right stick scrolls a page");
    ImGui::TextColored(v4(C_MUTE), "sun/moon icon = dark/light");
    ImGui::TextColored(v4(C_MUTE), "RISK toggles prompt before enabling");
    gb_end();
    gb_begin("KEY");
    ImGui::TextColored(v4(ACC()), "VX-MEI-4080-XXXX-XXXX");
    ImGui::TextColored(v4(C_MUTE), "key system wiring is next");
    gb_end();
    ImGui::Columns(1);
}
#define PERSONA_FILE "/sdcard/Android/data/com.vankrupt.pavlov/files/persona.txt"
static char g_persona_buf[64]={0}; static bool g_persona_loaded=false;
static void account_page(){
    if(!g_persona_loaded){ g_persona_loaded=true;
        FILE* f=fopen(PERSONA_FILE,"r"); if(f){ if(fgets(g_persona_buf,sizeof g_persona_buf,f)){
            int L=(int)strlen(g_persona_buf); while(L>0&&(g_persona_buf[L-1]=='\n'||g_persona_buf[L-1]=='\r'))g_persona_buf[--L]=0; } fclose(f);} }
    gb_begin("ACCOUNT");
    ImGui::TextColored(v4(C_MUTE),"USER");  ImGui::SameLine(200); ImGui::TextColored(v4(C_TEXT), "%s", g_persona_buf[0]?g_persona_buf:"mei");
    ImGui::TextColored(v4(C_MUTE),"STATUS");ImGui::SameLine(200); ImGui::TextColored(v4(C_GREEN), "UNDETECTED");
    gb_end();
    gb_begin("USERNAME  (client)");
    ImGui::TextColored(v4(C_MUTE), "your display name in the client");
    ImGui::PushItemWidth(-1.f);
    if(ImGui::InputText("##uname", g_mei.username, MEI_NAME_MAX)) touched();
    if(ImGui::IsItemActivated()){ g_kb2=true; g_kb=false; }
    ImGui::PopItemWidth();
    if(g_kb2){ ImGui::Spacing(); keyboard(g_mei.username, MEI_NAME_MAX); }
    gb_end();
    gb_begin("PERSONA  (Steam name)");
    ImGui::TextColored(v4(C_MUTE), "server-visible name; restart to apply");
    ImGui::PushItemWidth(-1.f);
    if(ImGui::InputText("##persona", g_persona_buf, sizeof g_persona_buf)) touched();
    if(ImGui::IsItemActivated()){ g_kb=true; g_kb2=false; }
    ImGui::PopItemWidth();
    if(g_kb){ ImGui::Spacing(); keyboard(g_persona_buf, sizeof g_persona_buf); }
    ImGui::Spacing();
    if(ImGui::Button("Save persona", ImVec2(180,40))){
        FILE* f=fopen(PERSONA_FILE,"w"); if(f){ fputs(g_persona_buf,f); fclose(f); } }
    gb_end();
}

// bottom-left user card (key-system data goes here once wired; placeholder for now)
static void user_card(float railW){
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = railW-20.f, h = 92.f, x = p.x+10.f, y = p.y;
    d->AddRectFilled(ImVec2(x,y), ImVec2(x+w,y+h), C_CARD, 12.f);
    // avatar
    d->AddCircleFilled(ImVec2(x+24, y+26), 16, ACC_SOFT());
    d->AddText(ImVec2(x+18, y+18), ACC(), "M");
    // name + PREM badge
    d->AddText(ImVec2(x+48, y+13), C_TEXT, g_mei.username[0]?g_mei.username:"guest");
    { const char* pr="PREM"; ImVec2 ts=ImGui::CalcTextSize(pr);
      d->AddRectFilled(ImVec2(x+w-ts.x-24, y+11), ImVec2(x+w-8, y+11+ts.y+6), ACC_SOFT(), 6.f);
      d->AddText(ImVec2(x+w-ts.x-16, y+14), ACC(), pr); }
    d->AddText(ImVec2(x+48, y+34), C_MUTE, "#4080");
    // stat chips
    struct Chip { const char* v; const char* k; };
    static const Chip chips[3] = { {"PREM","TYPE"}, {"INF","DAYS"}, {"OK","HWID"} };
    float cw = (w-24.f)/3.f;
    for(int i=0;i<3;i++){ float cx=x+8+i*(cw+4);
        d->AddRectFilled(ImVec2(cx,y+56), ImVec2(cx+cw,y+82), IM_COL32(0xF3,0xF0,0xF4,0xFF), 7.f);
        ImVec2 vs=ImGui::CalcTextSize(chips[i].v); d->AddText(ImVec2(cx+(cw-vs.x)*0.5f, y+59), ACC(), chips[i].v);
        ImVec2 ks=ImGui::CalcTextSize(chips[i].k); d->AddText(ImVec2(cx+(cw-ks.x)*0.5f, y+70), C_MUTE, chips[i].k); }
    ImGui::Dummy(ImVec2(w, h));
}

void mei_menu_frame(int panel_w, int panel_h){
    set_theme(g_mei.ui_dark); mei_style();   // apply theme + style each frame (cheap; supports live toggle)
    float W=(float)panel_w, H=(float)panel_h;
    ImGui::SetNextWindowPos(ImVec2(0,0));
    ImGui::SetNextWindowSize(ImVec2(W,H));
    ImGuiWindowFlags fl=ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|
                        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::Begin("mei",nullptr,fl);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 wp = ImGui::GetWindowPos();
    const float HEAD=56.f, RAIL=186.f;

    // ---- header ----
    dl->AddRectFilled(wp, ImVec2(wp.x+W, wp.y+HEAD), C_HEAD, 14.f, ImDrawFlags_RoundCornersTop);
    dl->AddCircleFilled(ImVec2(wp.x+30, wp.y+HEAD*0.5f), 13, ACC());
    dl->AddText(ImVec2(wp.x+22, wp.y+HEAD*0.5f-8), C_KNOB, "M");
    dl->AddText(ImVec2(wp.x+54, wp.y+HEAD*0.5f-9), C_TEXT, "2016 client");
    // top-center page pills (Home / Modules / Account)
    { float ph=32.f, py=HEAD*0.5f-ph*0.5f, gap=8.f, padx=18.f, pw[3], tot=gap*2;
      for(int i=0;i<3;i++){ pw[i]=ImGui::CalcTextSize(PAGES[i]).x+padx*2; tot+=pw[i]; }
      float px=W*0.5f-tot*0.5f;
      for(int i=0;i<3;i++){
        ImGui::SetCursorPos(ImVec2(px,py));
        char id[16]; snprintf(id,sizeof id,"##pg%d",i);
        ImGui::InvisibleButton(id, ImVec2(pw[i],ph));
        if(ImGui::IsItemClicked()) g_page=i;
        bool act=(g_page==i), hov=ImGui::IsItemHovered();
        ImVec2 sp=ImGui::GetItemRectMin();
        if(act) dl->AddRectFilled(sp, ImVec2(sp.x+pw[i],sp.y+ph), ACC(), ph*0.5f);
        else if(hov) dl->AddRectFilled(sp, ImVec2(sp.x+pw[i],sp.y+ph), ACC_SOFT(), ph*0.5f);
        ImVec2 tz=ImGui::CalcTextSize(PAGES[i]);
        dl->AddText(ImVec2(sp.x+(pw[i]-tz.x)*0.5f, sp.y+(ph-tz.y)*0.5f), act?C_KNOB:C_MUTE, PAGES[i]);
        px += pw[i]+gap;
      } }
    { char r[40]; snprintf(r,sizeof r,"%.0f fps", io.Framerate);
      ImVec2 z=ImGui::CalcTextSize(r);
      dl->AddCircleFilled(ImVec2(wp.x+W-z.x-96, wp.y+HEAD*0.5f), 5, g_mei.master_enabled?ACC():C_MUTE);
      dl->AddText(ImVec2(wp.x+W-z.x-84, wp.y+HEAD*0.5f-8), C_MUTE, g_mei.master_enabled?"active":"off");
      dl->AddText(ImVec2(wp.x+W-z.x-24, wp.y+HEAD*0.5f-8), C_MUTE, r); }
    // move-panel button (top-right, small)
    ImGui::SetCursorPos(ImVec2(W-150, HEAD*0.5f-15));
    { bool rep=g_mei.reposition;
      if(rep){ ImGui::PushStyleColor(ImGuiCol_Button,v4(ACC())); ImGui::PushStyleColor(ImGuiCol_Text,v4(C_KNOB)); }
      if(ImGui::Button(rep?"drag it":"move",ImVec2(0,30))){ g_mei.reposition=!g_mei.reposition; touched(); }
      if(rep) ImGui::PopStyleColor(2); }
    // theme toggle icon (sun = light, moon = dark)
    { float bs=32.f, bx=W-198.f, by=HEAD*0.5f-bs*0.5f;
      ImGui::SetCursorPos(ImVec2(bx,by));
      ImGui::InvisibleButton("##theme", ImVec2(bs,bs));
      if(ImGui::IsItemClicked()){ g_mei.ui_dark=!g_mei.ui_dark; touched(); }
      ImU32 ic = ImGui::IsItemHovered()?ACC():C_MUTE;
      ImVec2 c=ImVec2(wp.x+bx+bs*0.5f, wp.y+by+bs*0.5f);
      if(g_mei.ui_dark){ dl->AddCircleFilled(c,8,ic); dl->AddCircleFilled(ImVec2(c.x+4,c.y-3),7,C_HEAD); } // moon
      else { dl->AddCircleFilled(c,5,ic);
             for(int i=0;i<8;i++){ float a=i*0.7853982f; dl->AddLine(ImVec2(c.x+cosf(a)*8,c.y+sinf(a)*8),ImVec2(c.x+cosf(a)*11,c.y+sinf(a)*11),ic,2.f);} } } // sun

    // ---- sidebar ----
    ImGui::SetCursorPos(ImVec2(0, HEAD));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(C_RAIL));
    ImGui::BeginChild("rail", ImVec2(RAIL, H-HEAD), false);
    ImGui::Dummy(ImVec2(0,6));
    for(int i=0;i<N_NAV;i++){
        if(NAV[i].tab<0){ ImGui::Dummy(ImVec2(0,8)); ImGui::SetCursorPosX(16);
            ImGui::TextColored(v4(C_MUTE), "%s", NAV[i].label); ImGui::Dummy(ImVec2(0,2)); continue; }
        float iw=RAIL-20.f, ih=40.f; ImGui::SetCursorPosX(10);
        ImVec2 sp=ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(NAV[i].label, ImVec2(iw,ih));
        if(ImGui::IsItemClicked()){ g_tab=NAV[i].tab; g_page=1; }
        bool act=(g_page==1 && g_tab==NAV[i].tab), hov=ImGui::IsItemHovered();
        ImDrawList* d=ImGui::GetWindowDrawList();
        // animated active/hover fill
        ImGuiID nid=ImGui::GetID(NAV[i].label); ImGuiStorage* st=ImGui::GetStateStorage();
        float a=st->GetFloat(nid, act?1.f:0.f), tg=act?1.f:(hov?0.4f:0.f), dt=ImGui::GetIO().DeltaTime;
        a+=(tg-a)*(1.f-expf(-dt*16.f)); st->SetFloat(nid,a);
        if(a>0.01f){ ImU32 bg=IM_COL32((ACC()>>IM_COL32_R_SHIFT)&0xFF,(ACC()>>IM_COL32_G_SHIFT)&0xFF,(ACC()>>IM_COL32_B_SHIFT)&0xFF,(int)(0x22*a));
            d->AddRectFilled(sp, ImVec2(sp.x+iw,sp.y+ih), bg, 9.f);
            d->AddRectFilled(sp, ImVec2(sp.x+3,sp.y+ih), lerpc(IM_COL32(0,0,0,0),ACC(),act?1.f:0.f), 2.f); }
        ImU32 col = lerpc(C_TEXT, ACC(), act?1.f:0.f);
        draw_icon(d, ImVec2(sp.x+24, sp.y+ih*0.5f), 8.f, NAV[i].icon, col);
        d->AddText(ImVec2(sp.x+42, sp.y+ih*0.5f-8), col, NAV[i].label);
    }
    // user card pinned near the bottom
    { float used=ImGui::GetCursorPosY(); float want=H-HEAD-104.f; if(want>used) ImGui::SetCursorPosY(want); }
    user_card(RAIL);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- content ----
    ImGui::SameLine(0,0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(C_WINBG));
    ImGui::BeginChild("content", ImVec2(W-RAIL, H-HEAD), false);
    // joystick scroll: right-stick Y scrolls the content (up = toward top).
    { float sy=mei_xr_lift(); if(sy>0.15f||sy<-0.15f) ImGui::SetScrollY(ImGui::GetScrollY() - sy*io.DeltaTime*1600.f); }
    ImGui::Indent(16.f); ImGui::Dummy(ImVec2(0,6));
    // tab-change fade
    static int prevtab=0; static float tabfade=1.f;
    if(g_tab!=prevtab){ prevtab=g_tab; tabfade=0.f; }
    tabfade += io.DeltaTime*6.f; if(tabfade>1.f) tabfade=1.f;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.15f+0.85f*tabfade);
    if(g_page==0){ ImGui::PushItemWidth(-8.f); home_page(); ImGui::PopItemWidth(); }
    else if(g_page==2){
        float gridW=(W-RAIL)-32.f; ImGui::Columns(2,"acct",false); ImGui::SetColumnWidth(0, gridW*0.5f+8.f);
        ImGui::PushItemWidth(-8.f); account_page(); ImGui::PopItemWidth(); ImGui::Columns(1);
    } else {
        // Modules: 2-column card grid (tabs call NEXTCOL() at their split point)
        float gridW=(W-RAIL)-32.f;
        ImGui::Columns(2, "grid", false);
        ImGui::SetColumnWidth(0, gridW*0.5f + 8.f);
        ImGui::PushItemWidth(-8.f);
        switch(g_tab){
            case 0: tab_aimbot(); break;   case 1: tab_visuals(); break;  case 2: tab_weapon(); break;
            case 3: tab_movement(); break; case 4: tab_player(); break;   case 5: tab_ttt(); break;
            case 6: tab_soundboard(); break; case 7: tab_config(); break;
        }
        ImGui::PopItemWidth();
        ImGui::Columns(1);
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0,14));
    ImGui::Unindent(16.f);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- ban-risk confirmation modal ----
    if(g_risk_open){ ImGui::OpenPopup("banrisk"); g_risk_open=false; }
    ImGui::SetNextWindowPos(ImVec2(wp.x+W*0.5f, wp.y+H*0.5f), ImGuiCond_Always, ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSize(ImVec2(380,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22,20));
    if(ImGui::BeginPopupModal("banrisk", nullptr, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove)){
        ImDrawList* pd=ImGui::GetWindowDrawList(); ImVec2 pp=ImGui::GetWindowPos();
        pd->AddCircleFilled(ImVec2(pp.x+30,pp.y+30), 11, ACC_SOFT());
        pd->AddText(ImVec2(pp.x+26,pp.y+22), ACC(), "!");
        ImGui::SetCursorPosX(52); ImGui::TextColored(v4(ACC()), "BAN RISK");
        ImGui::Spacing();
        ImGui::TextWrapped("%s is highly detectable — servers can flag or ban it. Enable anyway?",
                           g_risk_name?g_risk_name:"This feature");
        ImGui::Spacing(); ImGui::Spacing();
        if(ImGui::Button("Enable anyway", ImVec2(170,42))){ if(g_risk_ptr){ *g_risk_ptr=true; touched(); } ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if(ImGui::Button("Cancel", ImVec2(140,42))){ if(g_risk_ptr){ *g_risk_ptr=false; touched(); } ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar();
}

void mei_menu_flush(){
    if(g_mei.act_save){ g_mei.act_save=false; mei_save(); g_dirty=false; return; }
    if(g_dirty && now_ms()-g_last_ms>900){ mei_save(); g_dirty=false; }
}
