// mei_menu.cpp — "mei mei [private]". Classic cheat-menu look (CSGO/Osiris style): left tab rail,
// bordered group-boxes, standard Dear ImGui widgets, dense, no prose. Bound to g_mei; debounce-saves.
#include "imgui.h"
#include "mei_menu.h"
#include "mei_settings.h"
#include "mei_esp.h"
#include <cstdio>
#include <cstring>
#include <cctype>
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
static bool g_kb=false, g_kb2=false, g_shift=false;
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
    const char* styles[] = { "Team colors","Single A","Single B","Flash","Target only","Custom color" };
    ImGui::PushItemWidth(-1.f);
    if (ImGui::Combo("##chstyle",&g_mei.chams_style,styles,6)) touched();
    ImGui::PopItemWidth();
    if (g_mei.chams_style == 5) {
        if (ImGui::ColorEdit3("Team 0", g_mei.chams_col,  ImGuiColorEditFlags_NoInputs)) touched();
        if (ImGui::ColorEdit3("Team 1", g_mei.chams_col2, ImGuiColorEditFlags_NoInputs)) touched();
        ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "recolor each team (see-through tint)");
    }
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
                char label[72];
                snprintf(label, sizeof label, "%s   [team %d]%s%s", p.name, p.team,
                         p.loaded ? "" : "  (far)", p.alive ? "" : "  (dead)");
                ImGui::PushStyleColor(ImGuiCol_Text,
                    v4(p.loaded ? IM_COL32(0xE2,0xE2,0xEA,0xFF) : IM_COL32(0x6E,0x6E,0x78,0xFF)));
                if (ImGui::Selectable(label, g_mei.kill_sel == i)) { g_mei.kill_sel = i; touched(); }
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }
        bool sel_ok = (g_mei.kill_sel >= 0 && g_mei.kill_sel < n);
        bool canKill = sel_ok && g_players[g_mei.kill_sel].loaded && g_players[g_mei.kill_sel].alive;
        ImGui::BeginDisabled(!canKill);
        if (ImGui::Button("KILL SELECTED", ImVec2(200,46))) g_mei.act_kill_sel = true;
        ImGui::EndDisabled();
        if (sel_ok && !g_players[g_mei.kill_sel].loaded) {
            ImGui::SameLine(); ImGui::TextColored(v4(IM_COL32(0xC8,0x66,0x66,0xFF)), "too far / not loaded"); }
    }
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
    Chk("Noclip (fly + no collision)",&g_mei.noclip);
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
    if (g_kb2) { ImGui::Spacing(); keyboard(filter, sizeof filter); }
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
    if(ImGui::Button("Refresh mods",ImVec2(180,44))) g_mei.act_refresh=true;   // re-resolve guns/chams/movement
    ImGui::SameLine(); ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "re-hooks guns, chams & movement if they stop");
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
    ImGui::TextColored(v4(ACC()), "2016"); ImGui::SameLine(0,8);
    ImGui::TextColored(v4(IM_COL32(0x7A,0x7A,0x88,0xFF)), "client");
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
