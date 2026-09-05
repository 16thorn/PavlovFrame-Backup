// steamshim.cpp — fake Steam persona for Pavlov Frame on Quest. Ships AS libsteam_api.so; real renamed
// libsteam_ap2.so (soname-patched) pulled via DT_NEEDED so every real steam symbol libUnreal imports
// resolves through. We override SteamAPI_Init (force success) and SteamInternal_FindOrCreateUserInterface
// (hand back a fake ISteamFriends whose vtable[0] GetPersonaName returns our chosen name). The game then
// sets PlayerNamePrivate from the persona through its own pipeline -> replicates to the server = real name.
#include <dlfcn.h>
#include <android/log.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"STEAMSHIM",__VA_ARGS__)
#define FILES_DIR "/sdcard/Android/data/com.vankrupt.pavlov/files/"
#define PERSONA_PATH FILES_DIR "persona.txt"
#define STEAMID_PATH FILES_DIR "steamid.txt"
#define STATUS_PATH  FILES_DIR "steamshim_status.txt"
// file logging — logcat drops the earliest lines (constructor runs before logd connects), so mirror
// diagnostics to a file we can read back. `reset` truncates; otherwise append.
static void slog(bool reset, const char* fmt, ...){
    FILE* f=fopen(STATUS_PATH, reset?"w":"a"); if(!f) return;
    va_list ap; va_start(ap,fmt); vfprintf(f,fmt,ap); va_end(ap); fputc('\n',f); fclose(f);
}
// First-run seeding: drop a default flag file so a fresh sideload is standalone-ready with no adb.
// Never clobbers an existing file (user overrides win).
static void ensure_file(const char* path, const char* dflt){
    FILE* f=fopen(path,"r"); if(f){ fclose(f); return; }
    f=fopen(path,"w"); if(f){ if(dflt) fputs(dflt,f); fclose(f); }
}

// Discovered live from SteamFriends018 / SteamUtils010 (STEAMSHIM discovery boot):
//   ISteamFriends::GetPersonaName        = vtable[0]   (already working)
//   ISteamFriends::GetMediumFriendAvatar = vtable[34]  (called with our CSteamID)
//   ISteamUtils::GetImageSize            = vtable[5]
//   ISteamUtils::GetImageRGBA            = vtable[6]   (dest buffer = 64*64*4 = 16384 -> Medium = 64x64)
#define AVATAR_DIM   64
#define AVATAR_BYTES (AVATAR_DIM*AVATAR_DIM*4)
#define VT_MED_AVATAR 34
#define VT_IMG_SIZE   5
#define VT_IMG_RGBA   6

static char     g_persona[64] = "player";   // default only; override via files/persona.txt
static uint64_t g_steamid = 0x0110000100000001ULL;   // spoofed SteamID64 (override via steamid.txt)
static void* g_friends_vtbl[256];
static void* g_friends_obj[2];   // [0] = vtable ptr (C++ object layout)
static void* g_stub_vtbl[256];   // generic fake interface (all methods stubbed)
static void* g_stub_obj[2];

// ---- avatar pixels (64x64 RGBA, top-to-bottom, R first — Steam's format) ----
// g_frames holds g_nframes contiguous 64x64 RGBA frames (1 = static, >1 = animated GIF).
static uint8_t* g_frames  = nullptr;
static int*     g_delays  = nullptr;   // ms per frame (GIF)
static int      g_nframes = 0;
static int      g_cur     = 0;         // frame currently served
static bool     g_have_pfp = false;

static bool g_persona_spoof = false;   // true if persona.txt set a name -> spoof it (overrides relay's real name)
static void load_persona(){
    FILE* f=fopen(PERSONA_PATH,"r");
    if(f){ if(fgets(g_persona,sizeof g_persona,f)){ int L=(int)strlen(g_persona);
        while(L>0&&(g_persona[L-1]=='\n'||g_persona[L-1]=='\r'||g_persona[L-1]==' ')) g_persona[--L]=0;
        if(g_persona[0]) g_persona_spoof=true; } fclose(f); }
    // steamid.txt: a SteamID64 as decimal (76561…) or hex (0x…). Read at Steam-init on launch.
    FILE* s=fopen(STEAMID_PATH,"r");
    if(s){ char b[32]={0}; if(fgets(b,sizeof b,s)){ char* p=b; while(*p==' ')p++;
        uint64_t v = (p[0]=='0'&&(p[1]=='x'||p[1]=='X')) ? strtoull(p+2,nullptr,16) : strtoull(p,nullptr,10);
        if(v) g_steamid=v; } fclose(s); }
}
// nearest-neighbour resample one w*h RGBA source frame into the 64x64 dst slot.
static void nn_resize(const uint8_t* src, int w, int h, uint8_t* dst){
    for(int y=0;y<AVATAR_DIM;y++) for(int x=0;x<AVATAR_DIM;x++){
        int sx=x*w/AVATAR_DIM, sy=y*h/AVATAR_DIM;
        const uint8_t* p=src+((size_t)sy*w+sx)*4; uint8_t* d=dst+((size_t)y*AVATAR_DIM+x)*4;
        d[0]=p[0]; d[1]=p[1]; d[2]=p[2]; d[3]=p[3];
    }
}
// load pfp: try pfp.gif as an ANIMATED multi-frame first, else a static png/jpg/bmp. All -> 64x64 RGBA.
static void load_pfp(){
    // 1) animated GIF (all frames)
    FILE* gf=fopen(FILES_DIR "pfp.gif","rb");
    if(gf){ fseek(gf,0,SEEK_END); long sz=ftell(gf); fseek(gf,0,SEEK_SET);
        uint8_t* buf=(uint8_t*)malloc(sz>0?sz:1); size_t rd=buf?fread(buf,1,sz,gf):0; fclose(gf);
        if(buf && rd==(size_t)sz){
            int w=0,h=0,frames=0,comp=0; int* delays=nullptr;
            uint8_t* all=stbi_load_gif_from_memory(buf,(int)sz,&delays,&w,&h,&frames,&comp,4);
            if(all && frames>=1 && w>0 && h>0){
                g_frames=(uint8_t*)malloc((size_t)frames*AVATAR_BYTES);
                g_delays=(int*)malloc((size_t)frames*sizeof(int));
                for(int f=0;f<frames;f++){
                    nn_resize(all+(size_t)f*w*h*4, w, h, g_frames+(size_t)f*AVATAR_BYTES);
                    int d=(delays&&delays[f]>0)?delays[f]:100; g_delays[f]=d<20?20:d;   // floor 20ms
                }
                g_nframes=frames; g_have_pfp=true;
                stbi_image_free(all); if(delays) free(delays); free(buf);
                LOG("pfp GIF loaded: %d frames %dx%d -> 64x64 (animated)", frames, w, h);
                slog(false,"load: GIF %d frames %dx%d", frames, w, h);
                return;
            }
            if(all) stbi_image_free(all);
        }
        free(buf);
    }
    // 2) static image
    const char* names[] = { FILES_DIR "pfp.png", FILES_DIR "pfp.jpg", FILES_DIR "pfp.jpeg", FILES_DIR "pfp.bmp" };
    for(unsigned n=0;n<sizeof names/sizeof names[0];n++){
        int w=0,h=0,comp=0; uint8_t* src=stbi_load(names[n],&w,&h,&comp,4);
        if(!src) continue;
        g_frames=(uint8_t*)malloc(AVATAR_BYTES); nn_resize(src,w,h,g_frames);
        stbi_image_free(src); g_nframes=1; g_have_pfp=true;
        LOG("pfp loaded from %s (%dx%d -> 64x64, static)", names[n], w, h);
        return;
    }
    LOG("no pfp.(gif/png/jpg/bmp) in files/ — no avatar served");
}

static const char* fake_GetPersonaName(void*){ return g_persona; }   // ISteamFriends vtable index 0
static void*       fake_stub(void*){ return nullptr; }               // generic safe stub
// ISteamUser vtable: 0=GetHSteamUser, 1=BLoggedOn, 2=GetSteamID (report a logged-in user so the game
// stops waiting on Steam login and proceeds to the name).
static int      fake_GetHSteamUser(void*){ return 1; }
static bool     fake_BLoggedOn(void*){ return true; }
static uint64_t fake_GetSteamID(void*){ return g_steamid; } // spoofed SteamID64 (steamid.txt)

// ---- ISteamUser vtable discovery: log which index the game calls for the auth ticket ----
// GetAuthSessionTicket(void* buf, int cbMax, uint32* pcbTicket [, SteamNetworkingIdentity*]) -> HAuthTicket
// GetAuthTicketForWebApi(const char* pIdentity) -> HAuthTicket
// We don't know the index on this SDK, so instrument slots 3..63 with logging thunks that report the
// index + first two args (a ticket call passes a writable buffer ptr + a size). Return 0 (no ticket) —
// safe for every getter. The log tells us the real GetAuthSessionTicket slot + call shape.
static uint64_t user_thunk_log(int idx, void* a, void* b){
    static int n[64] = {0};
    if (idx>=0 && idx<64 && n[idx] < 4) {
        slog(false, "ISteamUser vt[%d] CALLED arg0=%p arg1=%p", idx, a, b);
        LOG("ISteamUser vt[%d] CALLED arg0=%p arg1=%p", idx, a, b);
        n[idx]++;
    }
    return 0;
}
#define UTHUNK(i) static uint64_t uthunk_##i(void* self, void* a, void* b, void*, void*){ (void)self; return user_thunk_log(i, a, b); }
UTHUNK(3)  UTHUNK(4)  UTHUNK(5)  UTHUNK(6)  UTHUNK(7)  UTHUNK(8)  UTHUNK(9)  UTHUNK(10)
UTHUNK(11) UTHUNK(12) UTHUNK(13) UTHUNK(14) UTHUNK(15) UTHUNK(16) UTHUNK(17) UTHUNK(18)
UTHUNK(19) UTHUNK(20) UTHUNK(21) UTHUNK(22) UTHUNK(23) UTHUNK(24) UTHUNK(25) UTHUNK(26)
UTHUNK(27) UTHUNK(28) UTHUNK(29) UTHUNK(30) UTHUNK(31) UTHUNK(32) UTHUNK(33) UTHUNK(34)
UTHUNK(35) UTHUNK(36) UTHUNK(37) UTHUNK(38) UTHUNK(39) UTHUNK(40) UTHUNK(41) UTHUNK(42)
UTHUNK(43) UTHUNK(44) UTHUNK(45) UTHUNK(46) UTHUNK(47) UTHUNK(48) UTHUNK(49) UTHUNK(50)
UTHUNK(51) UTHUNK(52) UTHUNK(53) UTHUNK(54) UTHUNK(55) UTHUNK(56) UTHUNK(57) UTHUNK(58)
UTHUNK(59) UTHUNK(60) UTHUNK(61) UTHUNK(62) UTHUNK(63)
static void* g_uthunks[64] = {
    0,0,0,
    (void*)uthunk_3,(void*)uthunk_4,(void*)uthunk_5,(void*)uthunk_6,(void*)uthunk_7,(void*)uthunk_8,
    (void*)uthunk_9,(void*)uthunk_10,(void*)uthunk_11,(void*)uthunk_12,(void*)uthunk_13,(void*)uthunk_14,
    (void*)uthunk_15,(void*)uthunk_16,(void*)uthunk_17,(void*)uthunk_18,(void*)uthunk_19,(void*)uthunk_20,
    (void*)uthunk_21,(void*)uthunk_22,(void*)uthunk_23,(void*)uthunk_24,(void*)uthunk_25,(void*)uthunk_26,
    (void*)uthunk_27,(void*)uthunk_28,(void*)uthunk_29,(void*)uthunk_30,(void*)uthunk_31,(void*)uthunk_32,
    (void*)uthunk_33,(void*)uthunk_34,(void*)uthunk_35,(void*)uthunk_36,(void*)uthunk_37,(void*)uthunk_38,
    (void*)uthunk_39,(void*)uthunk_40,(void*)uthunk_41,(void*)uthunk_42,(void*)uthunk_43,(void*)uthunk_44,
    (void*)uthunk_45,(void*)uthunk_46,(void*)uthunk_47,(void*)uthunk_48,(void*)uthunk_49,(void*)uthunk_50,
    (void*)uthunk_51,(void*)uthunk_52,(void*)uthunk_53,(void*)uthunk_54,(void*)uthunk_55,(void*)uthunk_56,
    (void*)uthunk_57,(void*)uthunk_58,(void*)uthunk_59,(void*)uthunk_60,(void*)uthunk_61,(void*)uthunk_62,
    (void*)uthunk_63,
};
#define STEAMDISC_PATH FILES_DIR "steamdisc.txt"   // touch to arm ISteamUser vtable discovery

// ============================================================================
// STEAM TICKET RELAY (Quest side). files/relay.txt = "IP:PORT" of the PC minter
// (steamrelay/relay.ps1). We fetch a REAL encrypted app ticket + the account's
// SteamID64, hand the SteamID back at ISteamUser vt[2], and deliver the ticket
// through RequestEncryptedAppTicket (vt[21]) -> EncryptedAppTicketResponse_t ->
// GetEncryptedAppTicket (vt[22]). eosshim then sends it as EOS STEAM_APP_TICKET.
// ============================================================================
#define RELAY_PATH FILES_DIR "relay.txt"
static bool     g_relay_mode = false;
static uint8_t  g_ticket[2048];
static uint32_t g_ticketlen = 0;
static bool     g_ticket_ready = false;

static int hexval(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }

// Connect to the PC relay, read "STEAMID:<dec>\nTICKET:<hex>\n", fill g_steamid + g_ticket. Once.
static bool relay_fetch(){
    if (g_ticket_ready) return true;
    FILE* rf = fopen(RELAY_PATH,"r"); if(!rf){ slog(false,"relay.txt missing"); return false; }
    char line[64]={0}; if(!fgets(line,sizeof line,rf)){ fclose(rf); return false; } fclose(rf);
    char* colon = strchr(line,':'); if(!colon){ slog(false,"relay.txt bad (need IP:PORT)"); return false; }
    *colon=0; char* ip=line; int port=atoi(colon+1);
    for(char* p=ip; *p; ++p) if(*p=='\n'||*p=='\r'||*p==' '){*p=0;break;}
    int s=socket(AF_INET,SOCK_STREAM,0); if(s<0) return false;
    struct timeval tv{5,0}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=inet_addr(ip);
    if(connect(s,(sockaddr*)&a,sizeof a)!=0){ slog(false,"relay connect %s:%d FAILED",ip,port); close(s); return false; }
    // read the whole reply (name + 64x64 RGBA avatar hex ~32KB + ticket -> needs a big buffer)
    static char buf[131072]; int off=0,n;
    while(off<(int)sizeof(buf)-1 && (n=recv(s,buf+off,sizeof(buf)-1-off,0))>0) off+=n;
    close(s); buf[off]=0;
    slog(false,"relay reply %d bytes",off);
    char* sid=strstr(buf,"STEAMID:"); char* tik=strstr(buf,"TICKET:");
    if(!sid||!tik){ slog(false,"relay reply malformed"); return false; }
    uint64_t id=strtoull(sid+8,nullptr,10); if(id) g_steamid=id;
    // real Steam persona name from the relay -> GetPersonaName (non-null; avoids the null-name crash).
    // SKIP if persona.txt set a spoof name — that overrides the real name.
    char* nm=strstr(buf,"NAME:");
    if(nm && !g_persona_spoof){ nm+=5; int i=0; for(; nm[i] && nm[i]!='\n' && nm[i]!='\r' && i<(int)sizeof(g_persona)-1; i++) g_persona[i]=nm[i];
        g_persona[i]=0; slog(false,"relay persona name='%s'",g_persona); }
    else if(nm){ slog(false,"persona.txt spoof active -> keeping '%s' (ignoring relay name)",g_persona); }
    // real Steam avatar (medium 64x64 RGBA) from the relay -> g_frames -> GetImageRGBA
    char* av=strstr(buf,"AVATAR:");
    if(av){ av+=7; char* hx=strchr(av,':');   // format "AVATAR:<w>x<h>:<hex>"
        if(hx){ hx++;
            if(!g_frames) g_frames=(uint8_t*)malloc(AVATAR_BYTES);
            uint32_t len=0;
            for(; hx[0] && hx[1] && len<AVATAR_BYTES; hx+=2){ int hi=hexval(hx[0]); if(hi<0)break; int lo=hexval(hx[1]); if(lo<0)break; g_frames[len++]=(uint8_t)((hi<<4)|lo); }
            if(len==AVATAR_BYTES){ g_nframes=1; g_cur=0; g_have_pfp=true; slog(false,"relay avatar OK: %u bytes (64x64)",len); }
            else slog(false,"relay avatar wrong size %u (need %u) - skipped",len,(unsigned)AVATAR_BYTES);
        }
    }
    char* hx=tik+7; uint32_t len=0;
    for(; hx[0] && hx[1] && len<sizeof(g_ticket); hx+=2){
        int hi=hexval(hx[0]); if(hi<0) break; int lo=hexval(hx[1]); if(lo<0) break;
        g_ticket[len++]=(uint8_t)((hi<<4)|lo);
    }
    g_ticketlen=len; g_ticket_ready=(len>0);
    slog(false,"relay OK: SteamID64=%llu ticket=%u bytes",(unsigned long long)g_steamid,len);
    LOG("relay OK: SteamID64=%llu ticket=%u bytes",(unsigned long long)g_steamid,len);
    return g_ticket_ready;
}

// ---- pending EncryptedAppTicket call-result delivery ----
static uint64_t g_pending_call = 0;    // SteamAPICall_t we handed the game
static uint64_t g_call_ctr     = 0x5000000000000001ULL;
static void*    g_cr_cb        = nullptr;   // CCallbackBase* registered via RegisterCallResult
static uint64_t g_cr_call      = 0;
struct EncryptedAppTicketResponse_t { int32_t m_eResult; };   // k_iCallback = 154

// ISteamUser vt[21]: RequestEncryptedAppTicket(void* pDataToInclude, int cb) -> SteamAPICall_t
static uint64_t fake_RequestEncryptedAppTicket(void*, void*, int){
    relay_fetch();
    g_pending_call = g_call_ctr++;
    slog(false,"RequestEncryptedAppTicket -> call=%llu (ticket_ready=%d)",(unsigned long long)g_pending_call,g_ticket_ready);
    LOG("RequestEncryptedAppTicket -> call=%llu ready=%d",(unsigned long long)g_pending_call,g_ticket_ready);
    return g_pending_call;
}
// ISteamUser vt[22]: GetEncryptedAppTicket(void* buf, int cbMax, uint32* pcbTicket) -> bool
// Callers size-query first: GetEncryptedAppTicket(null/small, 0, &cb) -> we report the needed size
// via *pcb (return false); the caller allocates cb and calls again with a real buffer -> we copy.
static bool fake_GetEncryptedAppTicket(void*, uint8_t* buf, int cbMax, uint32_t* pcb){
    if(!g_ticket_ready){ if(pcb)*pcb=0; slog(false,"GetEncryptedAppTicket: not ready"); return false; }
    if(!buf || cbMax < (int)g_ticketlen){                 // size query / buffer too small -> report size
        if(pcb)*pcb=g_ticketlen;
        slog(false,"GetEncryptedAppTicket size-query: max=%d -> report need=%u",cbMax,g_ticketlen);
        return false;
    }
    memcpy(buf,g_ticket,g_ticketlen); if(pcb)*pcb=g_ticketlen;
    slog(false,"GetEncryptedAppTicket -> %u bytes served",g_ticketlen);
    LOG("GetEncryptedAppTicket -> %u bytes served",g_ticketlen);
    return true;
}
// fire the registered CCallResult: vtable[1] = Run(void* param, bool ioFailure, SteamAPICall_t)
static void deliver_encapp_result(){
    if(!g_pending_call || !g_cr_cb || g_cr_call!=g_pending_call) return;
    EncryptedAppTicketResponse_t resp; resp.m_eResult = g_ticket_ready ? 1 : 2;  // 1=k_EResultOK
    void** vt=*(void***)g_cr_cb; if(!vt) return;
    typedef void(*RunCR)(void*,void*,bool,uint64_t);
    slog(false,"delivering EncryptedAppTicketResponse_t eResult=%d call=%llu",resp.m_eResult,(unsigned long long)g_pending_call);
    ((RunCR)vt[1])(g_cr_cb,&resp,false,g_pending_call);
    g_pending_call=0; g_cr_cb=nullptr; g_cr_call=0;
}
static void* g_user_vtbl[256];
static void* g_user_obj[2];
static void* g_utils_vtbl[256];
static void* g_utils_obj[2];

// ---- avatar pipeline (arm64: self=x0, args in x1..x3) ----
// ISteamFriends::GetMediumFriendAvatar(CSteamID) -> int image handle (non-zero = valid).
// Returns a handle that CHANGES every frame — the game caches avatars by handle, so a new handle each
// frame is what makes it re-fetch GetImageRGBA (= the animation). Serve g_cur pixels for any handle.
static int g_handle = 1;
static int  fake_GetMediumFriendAvatar(void*, uint64_t /*steamID*/){
    static int n=0; if(n<60){ slog(false,"GetMediumFriendAvatar #%d -> handle %d", n, g_handle); n++; }
    return g_have_pfp ? g_handle : 0;
}
// ISteamUtils::GetImageSize(int handle, uint32* w, uint32* h) -> bool
static bool fake_GetImageSize(void*, int h, uint32_t* w, uint32_t* ht){
    static int n=0; if(n<60){ slog(false,"GetImageSize #%d handle=%d", n, h); n++; }
    if(w) *w=AVATAR_DIM; if(ht) *ht=AVATAR_DIM; return g_have_pfp;
}
// ISteamUtils::GetImageRGBA(int handle, uint8* dest, int destBufferSize) -> bool
static bool fake_GetImageRGBA(void*, int /*h*/, uint8_t* dest, int destSize){
    if(!g_have_pfp || !g_frames || !dest || destSize < AVATAR_BYTES) return false;
    int f = (g_cur>=0 && g_cur<g_nframes) ? g_cur : 0;
    memcpy(dest, g_frames + (size_t)f*AVATAR_BYTES, AVATAR_BYTES);   // serve the current (animated) frame
    static int nrgba=0; if(nrgba<40){ slog(false,"GetImageRGBA #%d frame=%d", nrgba, f); nrgba++; }
    return true;
}

// ---- GIF animation: drive the game to re-fetch each frame via Steam's callback system ----------------
// The game registers callbacks with SteamAPI_RegisterCallback and pumps them via SteamAPI_RunCallbacks —
// both of which WE export (wrapper loads first, so ours win). We capture the AvatarImageLoaded_t /
// PersonaStateChange_t callback objects, and on RunCallbacks we advance the frame + invoke the callback's
// virtual Run() so the game re-calls GetImageRGBA -> we hand it the next frame. That's the moving pfp.
enum { CB_AVATAR_LOADED = 300 + 34, CB_PERSONA_STATE = 300 + 4 };   // k_iSteamFriendsCallbacks(300) + n
static void* g_cb_avatar  = nullptr;   // AvatarImageLoaded_t listener
static void* g_cb_persona = nullptr;   // PersonaStateChange_t listener
static uint64_t now_ms(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000; }
struct AvatarImageLoaded_t { uint64_t m_steamID; int m_iImage; int m_iWide; int m_iTall; };
struct PersonaStateChange_t { uint64_t m_ulSteamID; int m_nChangeFlags; };
static void cb_run(void* cb, void* param){   // CCallbackBase::Run(void*) = vtable[0]
    if(!cb) return; void** vt=*(void***)cb; if(!vt) return;
    typedef void(*RunFn)(void*,void*); ((RunFn)vt[0])(cb,param);
}
static void fire_avatar_changed(){
    // fire BOTH: persona-change (avatar bit) makes the game re-query the handle, then avatar-loaded with
    // the NEW handle makes it re-fetch the pixels. m_iImage must be the new handle for the cache to miss.
    if(g_cb_persona){ PersonaStateChange_t p{g_steamid,0x0400}; cb_run(g_cb_persona,&p); } // k_EPersonaChangeAvatar
    if(g_cb_avatar){ AvatarImageLoaded_t p{g_steamid,g_handle,AVATAR_DIM,AVATAR_DIM}; cb_run(g_cb_avatar,&p); }
}

__attribute__((constructor)) static void on_load(){
    slog(true,"steamshim boot");
    // standalone default: local on-device minter at 127.0.0.1:48010. Seeded once; edit files/relay.txt
    // to point at a LAN PC relay instead. Absent file = no relay mode (community-server device-id path).
    ensure_file(RELAY_PATH, "127.0.0.1:48010\n");
    load_persona();   // still read steamid.txt as a fallback for GetSteamID (name unused while spoof off)
    // Avatar is served from the REAL Steam avatar the relay sends (parsed into g_frames in relay_fetch).
    // load_pfp() (a local pfp.png spoof) stays off; uncomment it to override with a local image instead.
    // load_pfp();
    for(int i=0;i<256;i++){ g_friends_vtbl[i]=(void*)fake_stub; g_stub_vtbl[i]=(void*)fake_stub;
                            g_user_vtbl[i]=(void*)fake_stub; g_utils_vtbl[i]=(void*)fake_stub; }
    // GetPersonaName MUST be non-null or the game derefs it and crashes. It returns g_persona, which
    // relay_fetch fills with the REAL Steam name (not a spoof). Falls back to persona.txt/"player" if
    // the relay is unreachable. To spoof a name instead, set persona.txt and drop relay.txt's NAME.
    g_friends_vtbl[0]=(void*)fake_GetPersonaName;
    g_friends_vtbl[VT_MED_AVATAR]=(void*)fake_GetMediumFriendAvatar;  // serves the REAL relayed avatar
    g_user_vtbl[0]=(void*)fake_GetHSteamUser;
    g_user_vtbl[1]=(void*)fake_BLoggedOn;
    g_user_vtbl[2]=(void*)fake_GetSteamID;
    // ISteamUser discovery: if steamdisc.txt present, instrument slots 3..63 to log the ticket call.
    { FILE* df=fopen(STEAMDISC_PATH,"r");
      if(df){ fclose(df);
        for(int i=3;i<64;i++) g_user_vtbl[i]=g_uthunks[i];
        LOG("ISteamUser vtable discovery ARMED (slots 3..63 instrumented)");
        slog(false,"ISteamUser discovery armed"); } }
    // RELAY MODE: if relay.txt present, install the real ticket handlers at vt[21]/vt[22]
    // (override any discovery thunk) and pre-fetch the ticket so GetSteamID already returns the real id.
    { FILE* rf=fopen(RELAY_PATH,"r");
      if(rf){ fclose(rf); g_relay_mode=true;
        g_user_vtbl[21]=(void*)fake_RequestEncryptedAppTicket;
        g_user_vtbl[22]=(void*)fake_GetEncryptedAppTicket;
        LOG("STEAM RELAY MODE armed (vt[21]=RequestEncAppTicket vt[22]=GetEncAppTicket)");
        slog(false,"relay mode armed"); relay_fetch(); } }
    g_utils_vtbl[VT_IMG_SIZE]=(void*)fake_GetImageSize;   // serves the REAL relayed avatar (64x64)
    g_utils_vtbl[VT_IMG_RGBA]=(void*)fake_GetImageRGBA;
    g_friends_obj[0]=(void*)g_friends_vtbl;
    g_stub_obj[0]=(void*)g_stub_vtbl;
    g_user_obj[0]=(void*)g_user_vtbl;
    g_utils_obj[0]=(void*)g_utils_vtbl;
    LOG("steam shim loaded, persona='%s' pfp=%d", g_persona, g_have_pfp);
}

// THE REAL INIT the game calls (flat header routes SteamAPI_Init through this). Return OK (0).
extern "C" __attribute__((visibility("default")))
int SteamInternal_SteamAPI_Init(const char* versions, void* /*SteamErrMsg*/ err){
    LOG("SteamInternal_SteamAPI_Init(%s) -> faked OK", versions?versions:"?");
    return 0;   // k_ESteamAPIInitResult_OK
}
extern "C" __attribute__((visibility("default"))) bool SteamAPI_Init(){ LOG("SteamAPI_Init -> true"); return true; }
extern "C" __attribute__((visibility("default"))) bool SteamAPI_RestartAppIfNecessary(unsigned int){ return false; }
extern "C" __attribute__((visibility("default"))) int  SteamAPI_GetHSteamUser(){ return 1; }

// We own callback registration + pumping (wrapper loads first). Capture the avatar/persona listeners.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RegisterCallback(void* pCallback, int iCallback){
    static int nreg=0; if(nreg<60){ slog(false,"RegisterCallback iCallback=%d", iCallback); nreg++; }
    if(iCallback==CB_AVATAR_LOADED){ g_cb_avatar=pCallback;  slog(false,"  -> captured AvatarImageLoaded_t"); LOG("captured AvatarImageLoaded_t cb=%p", pCallback); }
    else if(iCallback==CB_PERSONA_STATE){ g_cb_persona=pCallback; slog(false,"  -> captured PersonaStateChange_t"); LOG("captured PersonaStateChange_t cb=%p", pCallback); }
}
extern "C" __attribute__((visibility("default")))
void SteamAPI_UnregisterCallback(void* pCallback){
    if(pCallback==g_cb_avatar)  g_cb_avatar=nullptr;
    if(pCallback==g_cb_persona) g_cb_persona=nullptr;
}
// The game registers a CCallResult for the RequestEncryptedAppTicket SteamAPICall_t here.
// Capture it so RunCallbacks can fire EncryptedAppTicketResponse_t back on the matching handle.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RegisterCallResult(void* pCallback, uint64_t hAPICall){
    slog(false,"RegisterCallResult cb=%p call=%llu (pending=%llu)",pCallback,(unsigned long long)hAPICall,(unsigned long long)g_pending_call);
    if(g_relay_mode && hAPICall==g_pending_call){ g_cr_cb=pCallback; g_cr_call=hAPICall;
        LOG("captured CallResult for encapp ticket call=%llu",(unsigned long long)hAPICall); }
}
extern "C" __attribute__((visibility("default")))
void SteamAPI_UnregisterCallResult(void* pCallback, uint64_t hAPICall){
    if(pCallback==g_cr_cb) { g_cr_cb=nullptr; g_cr_call=0; }
}
// pumped by the game every frame: advance the GIF on its per-frame delay + poke the game to re-fetch.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RunCallbacks(){
    static int nrun=0; if(nrun<3){ slog(false,"RunCallbacks pumping (nframes=%d avatar_cb=%p persona_cb=%p)", g_nframes, g_cb_avatar, g_cb_persona); nrun++; }
    if(g_relay_mode) deliver_encapp_result();   // fire the encrypted-app-ticket call-result when pending
    if(g_nframes<=1 || !g_have_pfp) return;                 // static pfp: nothing to animate
    static uint64_t last=0; uint64_t t=now_ms();
    int d = (g_cur>=0 && g_cur<g_nframes && g_delays) ? g_delays[g_cur] : 100;
    if(last==0){ last=t; return; }
    if(t-last >= (uint64_t)d){ last=t; g_cur=(g_cur+1)%g_nframes; g_handle++;   // new handle = cache miss
        static int nfire=0; if(nfire<20){ slog(false,"advance -> frame %d handle %d, fire cb", g_cur, g_handle); nfire++; }
        fire_avatar_changed(); }
}

// The flat accessors (SteamFriends(), SteamUtils()...) reach interfaces through this, NOT directly.
// Real one bails when Steam isn't truly initialized. Layout: { void(*pFn)(void* ctx); uintptr_t counter;
// <ctx inline> }. We run pFn ourselves so it populates ctx via our FindOrCreateUserInterface (fakes),
// then hand back the ctx. That's what makes SteamFriends() return our fake object.
extern "C" __attribute__((visibility("default")))
void* SteamInternal_ContextInit(void* p){
    if(!p) return nullptr;
    void (*pFn)(void*) = *(void(**)(void*))p;   // offset 0
    void* ctx = (char*)p + 16;                  // inline ctx after pFn(8) + counter(8)
    if(pFn) pFn(ctx);                           // fills ctx via our FindOrCreateUserInterface
    *(uintptr_t*)((char*)p + 8) = 1;            // mark initialized
    return ctx;
}

typedef void* (*PFN_FOCUI)(int, const char*);
static void* real_h(){ static void* h=nullptr; if(!h){ h=dlopen("libsteam_ap2.so",RTLD_NOW|RTLD_NOLOAD); if(!h)h=dlopen("libsteam_ap2.so",RTLD_NOW);} return h; }

extern "C" __attribute__((visibility("default")))
void* SteamInternal_FindOrCreateUserInterface(int hUser, const char* version){
    if(version && strstr(version,"SteamFriends")){ static bool once=false; if(!once){once=true; LOG("FindOrCreate %s -> FAKE friends (persona='%s')", version, g_persona);} return g_friends_obj; }
    if(version && strstr(version,"SteamUtils"))  { static bool once=false; if(!once){once=true; LOG("FindOrCreate %s -> instrumented utils", version);} return g_utils_obj; }
    if(version && strstr(version,"SteamUser"))   { return g_user_obj; }   // logged-in user (breaks the wait loop)
    // every other interface: hand back a non-null stub object so the game doesn't bail on the Steam path
    return g_stub_obj;
}
