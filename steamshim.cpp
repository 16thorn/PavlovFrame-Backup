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

static void load_persona(){
    FILE* f=fopen(PERSONA_PATH,"r");
    if(f){ if(fgets(g_persona,sizeof g_persona,f)){ int L=(int)strlen(g_persona);
        while(L>0&&(g_persona[L-1]=='\n'||g_persona[L-1]=='\r'||g_persona[L-1]==' ')) g_persona[--L]=0; } fclose(f); }
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
    load_persona();
    load_pfp();
    for(int i=0;i<256;i++){ g_friends_vtbl[i]=(void*)fake_stub; g_stub_vtbl[i]=(void*)fake_stub;
                            g_user_vtbl[i]=(void*)fake_stub; g_utils_vtbl[i]=(void*)fake_stub; }
    g_friends_vtbl[0]=(void*)fake_GetPersonaName;                 // name
    g_friends_vtbl[VT_MED_AVATAR]=(void*)fake_GetMediumFriendAvatar;
    g_user_vtbl[0]=(void*)fake_GetHSteamUser;
    g_user_vtbl[1]=(void*)fake_BLoggedOn;
    g_user_vtbl[2]=(void*)fake_GetSteamID;
    g_utils_vtbl[VT_IMG_SIZE]=(void*)fake_GetImageSize;
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
// pumped by the game every frame: advance the GIF on its per-frame delay + poke the game to re-fetch.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RunCallbacks(){
    static int nrun=0; if(nrun<3){ slog(false,"RunCallbacks pumping (nframes=%d avatar_cb=%p persona_cb=%p)", g_nframes, g_cb_avatar, g_cb_persona); nrun++; }
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
    if(version && strstr(version,"SteamFriends")){ LOG("FindOrCreate %s -> FAKE friends (persona='%s')", version, g_persona); return g_friends_obj; }
    if(version && strstr(version,"SteamUtils"))  { LOG("FindOrCreate %s -> instrumented utils", version); return g_utils_obj; }
    if(version && strstr(version,"SteamUser"))   { return g_user_obj; }   // logged-in user (breaks the wait loop)
    // every other interface: hand back a non-null stub object so the game doesn't bail on the Steam path
    return g_stub_obj;
}
