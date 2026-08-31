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
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"STEAMSHIM",__VA_ARGS__)
#define PERSONA_PATH "/sdcard/Android/data/com.vankrupt.pavlov/files/persona.txt"

static char  g_persona[64] = "ratman4080";
static void* g_friends_vtbl[256];
static void* g_friends_obj[2];   // [0] = vtable ptr (C++ object layout)
static void* g_stub_vtbl[256];   // generic fake interface (all methods stubbed)
static void* g_stub_obj[2];

static void load_persona(){
    FILE* f=fopen(PERSONA_PATH,"r");
    if(f){ if(fgets(g_persona,sizeof g_persona,f)){ int L=(int)strlen(g_persona);
        while(L>0&&(g_persona[L-1]=='\n'||g_persona[L-1]=='\r'||g_persona[L-1]==' ')) g_persona[--L]=0; } fclose(f); }
}
static const char* fake_GetPersonaName(void*){ return g_persona; }   // ISteamFriends vtable index 0
static void*       fake_stub(void*){ return nullptr; }               // generic safe stub
// ISteamUser vtable: 0=GetHSteamUser, 1=BLoggedOn, 2=GetSteamID (report a logged-in user so the game
// stops waiting on Steam login and proceeds to the name).
static int      fake_GetHSteamUser(void*){ return 1; }
static bool     fake_BLoggedOn(void*){ return true; }
static uint64_t fake_GetSteamID(void*){ return 0x0110000100000001ULL; } // valid individual SteamID
static void* g_user_vtbl[256];
static void* g_user_obj[2];

__attribute__((constructor)) static void on_load(){
    load_persona();
    for(int i=0;i<256;i++){ g_friends_vtbl[i]=(void*)fake_stub; g_stub_vtbl[i]=(void*)fake_stub; g_user_vtbl[i]=(void*)fake_stub; }
    g_friends_vtbl[0]=(void*)fake_GetPersonaName;   // ISteamFriends::GetPersonaName = vtable[0]
    g_user_vtbl[0]=(void*)fake_GetHSteamUser;       // ISteamUser vtable
    g_user_vtbl[1]=(void*)fake_BLoggedOn;
    g_user_vtbl[2]=(void*)fake_GetSteamID;
    g_friends_obj[0]=(void*)g_friends_vtbl;
    g_stub_obj[0]=(void*)g_stub_vtbl;
    g_user_obj[0]=(void*)g_user_vtbl;
    LOG("steam shim loaded, persona='%s'", g_persona);
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
    if(version && strstr(version,"SteamUser"))   { return g_user_obj; }   // logged-in user (breaks the wait loop)
    // every other interface: hand back a non-null stub object so the game doesn't bail on the Steam path
    return g_stub_obj;
}
