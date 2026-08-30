// steamshim.cpp — libsteam_api.so interposer for Pavlov (Steam Frame build) on Quest.
//
// DIAGNOSTIC build: confirms whether the game's Steam layer initializes on a headset (no Steam client)
// and which interfaces it creates at join. Ships AS libsteam_api.so; the genuine lib is renamed
// libsteamreal.so (soname hex-patched, same length) and pulled via DT_NEEDED, so every SteamAPI_*
// symbol the game imports that we DON'T define resolves straight through to the real lib. Same
// interposer pattern as libEOSSDK.so. Tag STEAMSHIM.
//
// If SteamInternal_SteamAPI_Init returns 2 (NoSteamClient) / FindOrCreateUserInterface returns null,
// the game's Steam is dead on Quest -> player-hosted Steam peer-auth is unreachable from the client
// and "Authentication Failed" is a server-side wall. If init succeeds and ISteamUser is created, the
// next step is a vtable proxy on that interface to feed our PC-minted ticket.

#include <dlfcn.h>
#include <android/log.h>
#include <cstdint>
#include <cstring>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "STEAMSHIM", __VA_ARGS__)

typedef int32_t HSteamUser;
typedef int32_t ESteamAPIInitResult;   // 0=OK,1=FailedGeneric,2=NoSteamClient,3=VersionMismatch

static void* g_real = nullptr;
static void* real() {
    if (g_real) return g_real;
    g_real = dlopen("libsteamreal.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_real) g_real = dlopen("libsteamreal.so", RTLD_NOW);
    if (!g_real) LOG("FATAL dlopen libsteamreal.so: %s", dlerror());
    return g_real;
}

__attribute__((constructor)) static void on_load() { LOG("steamshim loaded (constructor)"); }

extern "C" __attribute__((visibility("default")))
ESteamAPIInitResult SteamInternal_SteamAPI_Init(const char* versions, char* errmsg) {
    void* h = real();
    typedef ESteamAPIInitResult (*F)(const char*, char*);
    F f = h ? (F)dlsym(h, "SteamInternal_SteamAPI_Init") : nullptr;
    ESteamAPIInitResult r = f ? f(versions, errmsg) : 1;
    LOG("SteamInternal_SteamAPI_Init -> %d (0=OK,2=NoSteamClient,3=VerMismatch) errmsg='%s'",
        r, (errmsg && errmsg[0]) ? errmsg : "");
    return r;
}

extern "C" __attribute__((visibility("default")))
void* SteamInternal_FindOrCreateUserInterface(HSteamUser u, const char* ver) {
    void* h = real();
    typedef void* (*F)(HSteamUser, const char*);
    F f = h ? (F)dlsym(h, "SteamInternal_FindOrCreateUserInterface") : nullptr;
    void* p = f ? f(u, ver) : nullptr;
    LOG("FindOrCreateUserInterface('%s') -> %p", ver ? ver : "?", p);
    return p;
}

extern "C" __attribute__((visibility("default")))
void* SteamInternal_FindOrCreateGameServerInterface(HSteamUser u, const char* ver) {
    void* h = real();
    typedef void* (*F)(HSteamUser, const char*);
    F f = h ? (F)dlsym(h, "SteamInternal_FindOrCreateGameServerInterface") : nullptr;
    void* p = f ? f(u, ver) : nullptr;
    LOG("FindOrCreateGameServerInterface('%s') -> %p", ver ? ver : "?", p);
    return p;
}
