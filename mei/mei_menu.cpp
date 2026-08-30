// mei_menu.cpp — "mei mei [private]". Classic cheat-menu look (CSGO/Osiris style): left tab rail,
// bordered group-boxes, standard Dear ImGui widgets, dense, no prose. Bound to g_mei; debounce-saves.
#include "imgui.h"
#include "mei_menu.h"
#include "mei_settings.h"
#include "mei_esp.h"
#include <cstdio>
#include <cstring>
#include <time.h>

// ---- accents -----------------------------------------------------------------
struct Accent { const char* name; ImU32 base; };
static const Accent ACCENTS[] = {
    { "pink",   IM_COL32(0xFF,0x4D,0x9A,0xFF) },
    { "cyan",   IM_COL32(0x2C,0xE0,0xD8,0xFF) },
    { "violet", IM_COL32(0x9B,0x6C,0xF5,0xFF) },
    { "lime",   IM_COL32(0x8B,0xE0,0x4A,0xFF) },
    { "amber",  IM_COL32(0xFF,0xB2,0x3A,0xFF) },
    { "red",    IM_COL32(0xFF,0x4B,0x4B,0xFF) },
};
static const int N_ACCENT = 6;
static int  accent_idx() { int i = g_mei.ui_accent % N_ACCENT; return i < 0 ? i + N_ACCENT : i; }
static ImU32 ACC() { return ACCENTS[accent_idx()].base; }
static ImVec4 v4(ImU32 c){ return ImVec4(((c>>IM_COL32_R_SHIFT)&0xFF)/255.f,((c>>IM_COL32_G_SHIFT)&0xFF)/255.f,((c>>IM_COL32_B_SHIFT)&0xFF)/255.f,((c>>IM_COL32_A_SHIFT)&0xFF)/255.f); }
static ImVec4 v4a(ImU32 c, float a){ ImVec4 x=v4(c); x.w=a; return x; }

void mei_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.f; s.WindowBorderSize = 0.f; s.WindowPadding = ImVec2(0,0);
    s.ChildRounding = 4.f; s.FrameRounding = 3.f; s.GrabRounding = 3.f; s.PopupRounding = 3.f;
    s.ScrollbarRounding = 3.f; s.TabRounding = 3.f; s.ScrollbarSize = 14.f;
    s.FramePadding = ImVec2(9,6); s.ItemSpacing = ImVec2(9,7); s.ItemInnerSpacing = ImVec2(7,5);
    s.WindowBorderSize = 0.f; s.ChildBorderSize = 1.f; s.FrameBorderSize = 0.f; s.GrabMinSize = 16.f;
    ImVec4* c = s.Colors;
    ImVec4 acc = v4(ACC());
    c[ImGuiCol_WindowBg]        = v4(IM_COL32(0x12,0x12,0x16,0xFF));
    c[ImGuiCol_ChildBg]         = v4(IM_COL32(0x18,0x18,0x1E,0xFF));
    c[ImGuiCol_PopupBg]         = v4(IM_COL32(0x1A,0x1A,0x20,0xFF));
    c[ImGuiCol_Border]          = v4(IM_COL32(0x2A,0x2A,0x33,0xFF));
    c[ImGuiCol_Text]            = v4(IM_COL32(0xE6,0xE6,0xEC,0xFF));
    c[ImGuiCol_TextDisabled]    = v4(IM_COL32(0x6E,0x6E,0x7C,0xFF));
    c[ImGuiCol_FrameBg]         = v4(IM_COL32(0x22,0x22,0x2B,0xFF));
    c[ImGuiCol_FrameBgHovered]  = v4(IM_COL32(0x2C,0x2C,0x38,0xFF));
    c[ImGuiCol_FrameBgActive]   = v4(IM_COL32(0x34,0x34,0x42,0xFF));
    c[ImGuiCol_Button]          = v4(IM_COL32(0x26,0x26,0x30,0xFF));
    c[ImGuiCol_ButtonHovered]   = v4(IM_COL32(0x30,0x30,0x3E,0xFF));
    c[ImGuiCol_ButtonActive]    = acc;
    c[ImGuiCol_CheckMark]       = acc;
    c[ImGuiCol_SliderGrab]      = acc;
    c[ImGuiCol_SliderGrabActive]= v4(IM_COL32(255,255,255,255));
    c[ImGuiCol_Header]          = v4a(ACC(),0.32f);
    c[ImGuiCol_HeaderHovered]   = v4a(ACC(),0.45f);
    c[ImGuiCol_HeaderActive]    = v4a(ACC(),0.60f);
    c[ImGuiCol_Separator]       = v4(IM_COL32(0x2A,0x2A,0x33,0xFF));
    c[ImGuiCol_ScrollbarBg]     = v4(IM_COL32(0x12,0x12,0x16,0xFF));
    c[ImGuiCol_ScrollbarGrab]   = v4(IM_COL32(0x2C,0x2C,0x38,0xFF));
    c[ImGuiCol_ScrollbarGrabHovered] = acc;
    ImGui::GetIO().FontGlobalScale = 1.25f;   // VR readability
}

// ---- save debounce -----------------------------------------------------------
static bool g_dirty=false; static long g_last_ms=0;
static long now_ms(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
static void touched(){ g_dirty=true; g_last_ms=now_ms(); }

// ---- widgets -----------------------------------------------------------------
static bool Chk(const char* l, bool* v){ if(ImGui::Checkbox(l,v)){touched();return true;} return false; }
static bool Sl(const char* l, float* v, float lo, float hi, const char* fmt){
    ImGui::PushItemWidth(-1.f);
    ImGui::TextUnformatted(l);
    char id[64]; snprintf(id,sizeof id,"##%s",l);
    bool ch = ImGui::SliderFloat(id,v,lo,hi,fmt); if(ch)touched();
    ImGui::PopItemWidth(); return ch;
}
static void gb_begin(const char* title){
    ImGui::BeginChild(title, ImVec2(0,0), ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_Border);
    ImGui::TextColored(v4(ACC()), "%s", title);
    ImGui::Separator();
}
static void gb_end(){ ImGui::EndChild(); ImGui::Spacing(); }

// ---- on-screen keyboard ------------------------------------------------------
static bool g_kb=false, g_shift=false;
static void kb_append(char* b,int cap,char ch){ int n=(int)strlen(b); if(n<cap-1){b[n]=ch;b[n+1]=0;touched();} }
static void keyboard(char* buf,int cap){
    const char* rows[4]={"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
    for(int r=0;r<4;r++){
        if(r==2) ImGui::Indent(26.f); if(r==3) ImGui::Indent(52.f);
        for(const char* p=rows[r]; *p; p++){
            char ch=*p; if(!g_shift && ch>='A'&&ch<='Z') ch+=32;
            char lab[2]={ch,0};
            if(ImGui::Button(lab, ImVec2(52,46))) kb_append(buf,cap,ch);
            ImGui::SameLine();
        }
        if(r==2) ImGui::Unindent(26.f); if(r==3) ImGui::Unindent(52.f);
        ImGui::NewLine();
    }
    if(ImGui::Button(g_shift?"SHIFT":"shift",ImVec2(82,46))) g_shift=!g_shift; ImGui::SameLine();
    if(ImGui::Button("space",ImVec2(220,46))) kb_append(buf,cap,' '); ImGui::SameLine();
    if(ImGui::Button("back",ImVec2(82,46))){ int n=(int)strlen(buf); if(n>0){buf[n-1]=0;touched();} } ImGui::SameLine();
    if(ImGui::Button("clear",ImVec2(82,46))){ buf[0]=0; touched(); } ImGui::SameLine();
    if(ImGui::Button("done",ImVec2(82,46))) g_kb=false;
}

// ---- tabs --------------------------------------------------------------------
static void tab_aimbot(){
    gb_begin("AIMBOT");
    Chk("Enabled", &g_mei.aim_enabled);
    const char* modes[]={"Off","On fire","Continuous"};
    ImGui::PushItemWidth(-1.f);
    if(ImGui::Combo("##mode",&g_mei.aim_mode,modes,3)) touched();
    ImGui::PopItemWidth();
    ImGui::BeginDisabled(!g_mei.aim_enabled);
    Sl("FOV",&g_mei.aim_fov,1.f,180.f,"%.0f");
    Sl("Smoothing",&g_mei.aim_smooth,0.f,0.95f,"%.2f");
    Chk("Aim at head",&g_mei.aim_target_head);
    Sl("Head offset",&g_mei.aim_head_z,-25.f,10.f,"%.0f cm");
    Chk("Team check",&g_mei.aim_team_check);
    Chk("Bullet teleport",&g_mei.aim_bullet_tp);
    ImGui::EndDisabled();
    gb_end();
}
static void tab_visuals(){
    gb_begin("CHAMS");
    Chk("Enabled",&g_mei.chams_enabled);
    ImGui::BeginDisabled(!g_mei.chams_enabled);
    Chk("Team colors",&g_mei.chams_team_color);
    Chk("Highlight target",&g_mei.chams_highlight);
    Chk("Skip dead",&g_mei.chams_skip_dead);
    ImGui::EndDisabled();
    gb_end();
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
    gb_begin("COMBAT");
    Chk("Trigger kill",&g_mei.trigger_kill);
    Chk("Kill aura",&g_mei.kill_aura);
    ImGui::BeginDisabled(!g_mei.kill_aura);
    Sl("Aura rate",&g_mei.aura_rate,40.f,500.f,"%.0f ms");
    ImGui::EndDisabled();
    Chk("Wallbang (shoot thru walls)",&g_mei.wallbang);
    gb_end();
}
static void tab_movement(){
    gb_begin("MOVEMENT");
    Chk("Enabled",&g_mei.move_enabled);
    ImGui::BeginDisabled(!g_mei.move_enabled);
    Sl("Sprint",&g_mei.move_sprint,1.f,5.f,"%.1fx");
    Sl("ADS",&g_mei.move_ads,1.f,8.f,"%.1fx");
    Sl("Walk",&g_mei.move_walk,1.f,5.f,"%.1fx");
    Sl("Crouch",&g_mei.move_crouch,1.f,5.f,"%.1fx");
    ImGui::EndDisabled();
    gb_end();
}
static void tab_player(){
    gb_begin("PLAYER");
    Chk("Godmode",&g_mei.godmode);
    Chk("Dev tag",&g_mei.dev_tag);
    Chk("Homing knife",&g_mei.homing_knife);
    gb_end();
    gb_begin("NAME CHANGER");
    Chk("Enabled",&g_mei.name_enabled);
    ImGui::BeginDisabled(!g_mei.name_enabled);
    ImGui::PushItemWidth(-1.f);
    if(ImGui::InputText("##name",g_mei.name_text,MEI_NAME_MAX)) touched();
    if(ImGui::IsItemActivated()) g_kb=true;           // auto-open keyboard on tap
    ImGui::PopItemWidth();
    if(g_kb){ ImGui::Spacing(); keyboard(g_mei.name_text,MEI_NAME_MAX); }
    ImGui::EndDisabled();
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
    gb_begin("BUY  (ServerBuy)");
    ImGui::PushItemWidth(-1.f);
    if (ImGui::InputText("##buy", g_mei.buy_name, MEI_NAME_MAX)) touched();
    if (ImGui::IsItemActivated()) g_kb = true;   // auto-open keyboard
    ImGui::PopItemWidth();
    const char* presets[] = { "Radar","Disguiser","C4","Silenced","Defuser","BodyArmor","Teleporter","HealthStation" };
    for (int i = 0; i < 8; i++) {
        if (ImGui::Button(presets[i], ImVec2(150,38))) {
            strncpy(g_mei.buy_name, presets[i], MEI_NAME_MAX-1); g_mei.buy_name[MEI_NAME_MAX-1]=0; g_mei.act_buy = true; }
        if (i % 3 != 2) ImGui::SameLine();
    }
    ImGui::NewLine();
    if (ImGui::Button("BUY", ImVec2(160,46))) g_mei.act_buy = true;
    if (g_kb) { ImGui::Spacing(); keyboard(g_mei.buy_name, MEI_NAME_MAX); }
    gb_end();
}
static void tab_config(){
    gb_begin("GENERAL");
    Chk("Master enable",&g_mei.master_enabled);
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
    gb_begin("TOOLS");
    if(ImGui::Button("Dump SDK",ImVec2(150,40))) g_mei.act_dump_sdk=true; ImGui::SameLine();
    if(ImGui::Button("Dump whitelist",ImVec2(190,40))) g_mei.act_dump_whitelist=true; ImGui::SameLine();
    if(ImGui::Button("Save",ImVec2(120,40))){ g_mei.act_save=true; g_dirty=false; }
    gb_end();
}

// ---- frame -------------------------------------------------------------------
static int g_tab=0;
static const char* TABS[]={"Aimbot","Visuals","Weapon","Movement","Player","TTT","Config"};
static const int N_TABS=7;

void mei_menu_frame(int panel_w, int panel_h){
    float W=(float)panel_w, H=(float)panel_h;
    ImGui::SetNextWindowPos(ImVec2(0,0));
    ImGui::SetNextWindowSize(ImVec2(W,H));
    ImGuiWindowFlags fl=ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|
                        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::Begin("mei",nullptr,fl);
    ImGuiIO& io = ImGui::GetIO();

    // header
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 wp = ImGui::GetWindowPos();
    const float HEAD=44.f;
    dl->AddRectFilled(wp, ImVec2(wp.x+W, wp.y+HEAD), IM_COL32(0x16,0x16,0x1C,0xFF));
    dl->AddRectFilled(ImVec2(wp.x, wp.y+HEAD-2), ImVec2(wp.x+W, wp.y+HEAD), ACC());
    ImGui::SetCursorPos(ImVec2(16, 11));
    ImGui::TextColored(v4(ACC()), "mei mei"); ImGui::SameLine(0,8);
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "[private]");
    ImGui::SameLine(0, 20); ImGui::SetCursorPosY(8);
    {
        bool rep = g_mei.reposition;
        if (rep) { ImGui::PushStyleColor(ImGuiCol_Button, v4(ACC()));
                   ImGui::PushStyleColor(ImGuiCol_Text, v4(IM_COL32(0x12,0x12,0x16,0xFF))); }
        if (ImGui::Button(rep ? "moving: point off-panel + hold trigger" : "move panel", ImVec2(0,28))) {
            g_mei.reposition = !g_mei.reposition; touched(); }
        if (rep) ImGui::PopStyleColor(2);
    }
    { char r[48]; snprintf(r,sizeof r,"%s  %.0ffps", g_mei.master_enabled?"active":"off", io.Framerate);
      ImVec2 z=ImGui::CalcTextSize(r); ImGui::SetCursorPos(ImVec2(W-z.x-16,13));
      ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)),"%s",r); }

    // body: rail + content
    ImGui::SetCursorPos(ImVec2(0, HEAD));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(IM_COL32(0x14,0x14,0x1A,0xFF)));
    ImGui::BeginChild("rail", ImVec2(168, H-HEAD), false);
    ImGui::Dummy(ImVec2(0,6));
    for(int i=0;i<N_TABS;i++){
        ImGui::SetCursorPosX(8);
        if(ImGui::Selectable(TABS[i], g_tab==i, 0, ImVec2(152,34))) g_tab=i;
    }
    ImGui::SetCursorPos(ImVec2(12, H-HEAD-30));
    ImGui::TextColored(v4(IM_COL32(0x55,0x55,0x63,0xFF)), "v4080");
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine(0,0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(IM_COL32(0x12,0x12,0x16,0xFF)));
    ImGui::BeginChild("content", ImVec2(W-168, H-HEAD), false);
    ImGui::Indent(14.f); ImGui::PushItemWidth(-14.f); ImGui::Dummy(ImVec2(0,4));
    switch(g_tab){
        case 0: tab_aimbot(); break;   case 1: tab_visuals(); break;  case 2: tab_weapon(); break;
        case 3: tab_movement(); break; case 4: tab_player(); break;   case 5: tab_ttt(); break;
        case 6: tab_config(); break;
    }
    ImGui::Dummy(ImVec2(0,12));
    ImGui::PopItemWidth(); ImGui::Unindent(14.f);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar();
}

void mei_menu_flush(){
    if(g_mei.act_save){ g_mei.act_save=false; mei_save(); g_dirty=false; return; }
    if(g_dirty && now_ms()-g_last_ms>900){ mei_save(); g_dirty=false; }
}
