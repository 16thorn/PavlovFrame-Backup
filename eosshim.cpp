// eosshim.cpp — EOS Connect credential interposer for Pavlov (Steam Frame build) on Meta Quest.
//
// Ships AS libEOSSDK.so. The genuine SDK is renamed libEOSSDK_real.so and pulled in as a
// DT_NEEDED dependency of this wrapper. libUnreal.so hard-links libEOSSDK.so (DT_NEEDED) and
// imports every EOS_* symbol from it; this wrapper DEFINES only EOS_Connect_Login, so every
// other EOS_* symbol resolves straight through to libEOSSDK_real.so via the loader's needed
// graph. On login we rewrite the external credential from Steam (which cannot init on a headset
// with no Steam client) to Device ID — an anonymous per-headset EOS identity that needs nothing
// external. Same interposer pattern as the xrshim OpenXR loader already in this APK.

#include <dlfcn.h>
#include <android/log.h>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "EOSSHIM", __VA_ARGS__)

// ---- minimal EOS Connect ABI (stable across SDK versions) -------------------
extern "C" {

typedef struct EOS_ConnectHandle*            EOS_HConnect;
typedef struct EOS_ProductUserIdDetails*     EOS_ProductUserId;
typedef struct EOS_ContinuanceTokenDetails*  EOS_ContinuanceToken;
typedef int32_t EOS_EResult;
typedef int32_t EOS_EExternalCredentialType;

enum { EOS_ECT_DEVICEID_ACCESS_TOKEN = 10 };
enum { EOS_CONNECT_USERLOGININFO_API_001 = 1, EOS_CONNECT_CREATEDEVICEID_API_001 = 1,
       EOS_CONNECT_DELETEDEVICEID_API_001 = 1 };

typedef struct {
    int32_t                     ApiVersion;
    const char*                 Token;
    EOS_EExternalCredentialType Type;
} EOS_Connect_Credentials;

typedef struct {
    int32_t     ApiVersion;
    const char* DisplayName;
} EOS_Connect_UserLoginInfo;

typedef struct {
    int32_t                           ApiVersion;
    const EOS_Connect_Credentials*    Credentials;
    const EOS_Connect_UserLoginInfo*  UserLoginInfo;
} EOS_Connect_LoginOptions;

typedef struct {
    int32_t     ApiVersion;
    const char* DeviceModel;
} EOS_Connect_CreateDeviceIdOptions;

typedef struct {
    EOS_EResult          ResultCode;
    void*                ClientData;
    EOS_ProductUserId    LocalUserId;
    EOS_ContinuanceToken ContinuanceToken;
} EOS_Connect_LoginCallbackInfo;

typedef struct {
    EOS_EResult ResultCode;
    void*       ClientData;
} EOS_Connect_CreateDeviceIdCallbackInfo;

typedef struct {
    int32_t ApiVersion;
} EOS_Connect_DeleteDeviceIdOptions;

typedef struct {
    EOS_EResult ResultCode;
    void*       ClientData;
} EOS_Connect_DeleteDeviceIdCallbackInfo;

typedef void (*EOS_Connect_OnLoginCallback)(const EOS_Connect_LoginCallbackInfo*);
typedef void (*EOS_Connect_OnCreateDeviceIdCallback)(const EOS_Connect_CreateDeviceIdCallbackInfo*);
typedef void (*EOS_Connect_OnDeleteDeviceIdCallback)(const EOS_Connect_DeleteDeviceIdCallbackInfo*);

typedef void (*PFN_Login)(EOS_HConnect, const EOS_Connect_LoginOptions*, void*, EOS_Connect_OnLoginCallback);
typedef void (*PFN_CreateDeviceId)(EOS_HConnect, const EOS_Connect_CreateDeviceIdOptions*, void*, EOS_Connect_OnCreateDeviceIdCallback);
typedef void (*PFN_DeleteDeviceId)(EOS_HConnect, const EOS_Connect_DeleteDeviceIdOptions*, void*, EOS_Connect_OnDeleteDeviceIdCallback);

} // extern "C"

static void*              g_real              = nullptr;
static PFN_Login          real_Login          = nullptr;
static PFN_CreateDeviceId real_CreateDeviceId = nullptr;
static PFN_DeleteDeviceId real_DeleteDeviceId = nullptr;

static void* real_handle() {
    if (g_real) return g_real;
    g_real = dlopen("libEOSDK.so", RTLD_NOW | RTLD_NOLOAD);   // already loaded via DT_NEEDED
    if (!g_real) g_real = dlopen("libEOSDK.so", RTLD_NOW);
    if (!g_real) LOG("FATAL dlopen libEOSDK.so: %s", dlerror());
    return g_real;
}

static void resolve_real() {
    if (real_Login) return;
    void* h = real_handle();
    if (!h) return;
    real_Login          = (PFN_Login)dlsym(h, "EOS_Connect_Login");
    real_CreateDeviceId = (PFN_CreateDeviceId)dlsym(h, "EOS_Connect_CreateDeviceId");
    real_DeleteDeviceId = (PFN_DeleteDeviceId)dlsym(h, "EOS_Connect_DeleteDeviceId");
    LOG("resolved real Login=%p CreateDeviceId=%p DeleteDeviceId=%p",
        (void*)real_Login, (void*)real_CreateDeviceId, (void*)real_DeleteDeviceId);
}

// ---- diagnostics: prove routing + find the real login path ------------------
__attribute__((constructor)) static void on_load() {
    LOG("eosshim loaded (constructor)");
    // piggyback the chams lib onto our injection point (no patchelf on libUnreal needed)
    void* ch = dlopen("libpavchams.so", RTLD_NOW | RTLD_GLOBAL);
    LOG("libpavchams.so load: %s", ch ? "OK" : dlerror());
}

typedef int32_t (*PFN_Initialize)(const void*);
typedef void    (*PFN_AuthLogin)(void*, const void*, void*, void*);
typedef void    (*PFN_ConnLoginCT)(void*, const void*, void*, void*);

extern "C" __attribute__((visibility("default")))
int32_t EOS_Initialize(const void* Options) {
    void* h = real_handle();
    PFN_Initialize f = h ? (PFN_Initialize)dlsym(h, "EOS_Initialize") : nullptr;
    LOG("EOS_Initialize called -> routing through shim OK (real=%p)", (void*)f);
    return f ? f(Options) : 1;
}

// ---- autonomous Device-ID login ---------------------------------------------
// This is a Steam Frame build: it auto-logs-in via Steam, which can't init on a
// headset, so the game never calls a login function — it just polls GetLoginStatus.
// We log in via Device ID ourselves; EOS then fires the game's registered
// EOS_Connect_AddNotifyLoginStatusChanged callback and the game adopts the identity.
static EOS_HConnect g_connect = nullptr;
static bool g_autologin_started = false;
static const char* g_username = "cum";   // injected display name (login + lobby attrs)

typedef EOS_HConnect (*PFN_GetConnect)(void*);

static EOS_ProductUserId g_localpuid = nullptr;

static void autologin_login_cb(const EOS_Connect_LoginCallbackInfo* d) {
    g_localpuid = *(EOS_ProductUserId*)((const char*)d + 16);   // LocalUserId
    LOG("AUTOLOGIN login result=%d, localPUID=%p", d->ResultCode, (void*)g_localpuid);
}
static void autologin_createid_cb(const EOS_Connect_CreateDeviceIdCallbackInfo* d) {
    LOG("AUTOLOGIN CreateDeviceId result=%d -> DeviceID login", d->ResultCode);
    if (!real_Login || !g_connect) return;
    static EOS_Connect_Credentials creds;
    creds.ApiVersion = 1; creds.Token = nullptr; creds.Type = EOS_ECT_DEVICEID_ACCESS_TOKEN;
    static EOS_Connect_UserLoginInfo ui;
    ui.ApiVersion = EOS_CONNECT_USERLOGININFO_API_001; ui.DisplayName = g_username;
    static EOS_Connect_LoginOptions o;
    o.ApiVersion = 2; o.Credentials = &creds; o.UserLoginInfo = &ui;
    real_Login(g_connect, &o, nullptr, autologin_login_cb);
}
#define NEWID_PATH "/sdcard/Android/data/com.vankrupt.pavlov/files/newid.txt"

static void do_createid() {
    if (!real_CreateDeviceId || !g_connect) return;
    EOS_Connect_CreateDeviceIdOptions cdo;
    cdo.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_001; cdo.DeviceModel = "Meta Quest 3";
    real_CreateDeviceId(g_connect, &cdo, nullptr, autologin_createid_cb);
}
// after wiping the old device id, mint a brand new one -> fresh PUID (community-server unban).
static void newid_delete_cb(const EOS_Connect_DeleteDeviceIdCallbackInfo* d) {
    LOG("NEWID DeleteDeviceId result=%d -> minting fresh device id/PUID", d ? (int)d->ResultCode : -1);
    do_createid();
}
static void start_autologin() {
    if (g_autologin_started || !g_connect) return;
    g_autologin_started = true;
    resolve_real();
    if (!real_CreateDeviceId) return;
    LOG("starting autonomous DeviceID login on connect=%p", (void*)g_connect);
    // NEW IDENTITY: if newid.txt exists, delete the stored device id first so CreateDeviceId mints a
    // brand-new one = a fresh anonymous PUID the server has never seen (dodges PUID/name bans). One-shot:
    // remove the flag so we don't rotate every launch. Pair with a fresh persona.txt for a full new player.
    FILE* nf = fopen(NEWID_PATH, "r");
    if (nf) { fclose(nf); remove(NEWID_PATH);
        if (real_DeleteDeviceId) {
            LOG("NEWID flag present -> wiping device id for a fresh PUID");
            EOS_Connect_DeleteDeviceIdOptions ddo; ddo.ApiVersion = EOS_CONNECT_DELETEDEVICEID_API_001;
            real_DeleteDeviceId(g_connect, &ddo, nullptr, newid_delete_cb);
            return;
        }
        LOG("NEWID flag present but DeleteDeviceId unresolved -> normal login");
    }
    do_createid();
}

typedef void* (*PFN_PlatformCreate)(const void*);
extern "C" __attribute__((visibility("default")))
void* EOS_Platform_Create(const void* Options) {
    void* h = real_handle();
    PFN_PlatformCreate f = h ? (PFN_PlatformCreate)dlsym(h, "EOS_Platform_Create") : nullptr;
    void* plat = f ? f(Options) : nullptr;
    if (plat && h) {
        PFN_GetConnect gc = (PFN_GetConnect)dlsym(h, "EOS_Platform_GetConnectInterface");
        if (gc) g_connect = gc(plat);
    }
    // EOS_Platform_Options: ProductId@16, SandboxId@24, ClientId@32, DeploymentId@80 (arm64)
    if (Options) {
        const char* prod = *(const char* const*)((const char*)Options + 16);
        const char* sbox = *(const char* const*)((const char*)Options + 24);
        const char* dep  = *(const char* const*)((const char*)Options + 80);
        LOG("EOS config: Product=%s Sandbox=%s Deployment=%s",
            prod?prod:"?", sbox?sbox:"?", dep?dep:"?");
    }
    LOG("EOS_Platform_Create -> %p (%s), connect=%p", plat, plat ? "OK" : "NULL/FAILED", (void*)g_connect);
    return plat;
}

// ---- lobby search tracing ---------------------------------------------------
extern "C" __attribute__((visibility("default")))
int32_t EOS_LobbySearch_SetParameter(void* Handle, const void* Options) {
    // SetParameterOptions: ApiVersion@0, Parameter*@8, ComparisonOp@16
    // AttributeData: ApiVersion@0, Key@8, Value(union)@16, ValueType@24
    if (Options) {
        const void* attr = *(const void* const*)((const char*)Options + 8);
        int32_t cmp = *(const int32_t*)((const char*)Options + 16);
        if (attr) {
            const char* key = *(const char* const*)((const char*)attr + 8);
            int32_t vtype = *(const int32_t*)((const char*)attr + 24);
            // EOS_EAttributeType: 0=BOOLEAN,1=INT64,2=DOUBLE,3=STRING
            if (vtype == 3 /*STRING*/) {
                const char* val = *(const char* const*)((const char*)attr + 16);
                LOG("LobbySearch.SetParameter key='%s' STR='%s' cmp=%d", key?key:"?", val?val:"?", cmp);
            } else if (vtype == 1 /*INT64*/) {
                int64_t val = *(const int64_t*)((const char*)attr + 16);
                LOG("LobbySearch.SetParameter key='%s' int=%lld cmp=%d", key?key:"?", (long long)val, cmp);
            } else {
                LOG("LobbySearch.SetParameter key='%s' vtype=%d cmp=%d", key?key:"?", vtype, cmp);
            }
        }
    }
    void* h = real_handle();
    typedef int32_t (*F)(void*, const void*);
    F f = h ? (F)dlsym(h, "EOS_LobbySearch_SetParameter") : nullptr;
    return f ? f(Handle, Options) : 1;
}

// When the game reads a lobby's VERSION attribute to run its join compatibility
// check against its own build (1.0.29), rewrite the live lobby's 1.0.28 back to
// 1.0.29 so the check passes. Same length -> safe in-place.
extern "C" __attribute__((visibility("default")))
int32_t EOS_LobbyDetails_CopyAttributeByIndex(void* Handle, const void* Options, void** OutAttribute) {
    void* h = real_handle();
    typedef int32_t (*F)(void*, const void*, void**);
    F f = h ? (F)dlsym(h, "EOS_LobbyDetails_CopyAttributeByIndex") : nullptr;
    int32_t r = f ? f(Handle, Options, OutAttribute) : 1;
    return r;
}

// The game reads each member's DISPLAYNAME back here to draw the name list.
// Inject the username wherever it comes back null (our anon entry).
extern "C" __attribute__((visibility("default")))
int32_t EOS_LobbyDetails_CopyMemberAttributeByIndex(void* Handle, const void* Options, void** OutAttribute) {
    void* h = real_handle();
    typedef int32_t (*F)(void*, const void*, void**);
    F f = h ? (F)dlsym(h, "EOS_LobbyDetails_CopyMemberAttributeByIndex") : nullptr;
    int32_t r = f ? f(Handle, Options, OutAttribute) : 1;
    if (r == 0 && OutAttribute && *OutAttribute) {
        void* data = *(void* const*)((char*)*OutAttribute + 8);   // Attribute.Data
        if (data) {
            const char* key = *(const char* const*)((char*)data + 8);
            int32_t vtype = *(const int32_t*)((char*)data + 24);
            if (vtype == 3 && key && !strcmp(key, "DISPLAYNAME")) {
                const char* val = *(const char* const*)((char*)data + 16);
                if (!val || !val[0]) {
                    *(const char**)((char*)data + 16) = g_username;
                    LOG("member DISPLAYNAME readback null -> '%s'", g_username);
                }
            }
        }
    }
    return r;
}

// Guard so the SDK's Attribute release never frees our injected static string.
extern "C" __attribute__((visibility("default")))
void EOS_Lobby_Attribute_Release(void* Attribute) {
    if (Attribute) {
        void* data = *(void* const*)((char*)Attribute + 8);
        if (data) {
            const char* dn = *(const char* const*)((char*)data + 16);
            if (dn == g_username) *(const char**)((char*)data + 16) = nullptr;
        }
    }
    void* h = real_handle();
    typedef void (*F)(void*);
    F f = h ? (F)dlsym(h, "EOS_Lobby_Attribute_Release") : nullptr;
    if (f) f(Attribute);
}

extern "C" __attribute__((visibility("default")))
uint32_t EOS_LobbySearch_GetSearchResultCount(void* Handle, const void* Options) {
    void* h = real_handle();
    typedef uint32_t (*F)(void*, const void*);
    F f = h ? (F)dlsym(h, "EOS_LobbySearch_GetSearchResultCount") : nullptr;
    uint32_t c = f ? f(Handle, Options) : 0;
    LOG("LobbySearch RESULT COUNT = %u", c);
    return c;
}

// Custom username injected in place of the null anon name.
// If a string attribute's value is null/empty, repoint it to our username.
// The SDK copies the attribute immediately, so a static string is safe.
static void fill_name_if_null(const void* attr) {
    if (!attr) return;
    int32_t vtype = *(const int32_t*)((const char*)attr + 24);
    if (vtype != 3) return;
    const char* val = *(const char* const*)((const char*)attr + 16);
    if (!val || !val[0]) {
        *(const char**)((char*)attr + 16) = g_username;
    }
}

extern "C" __attribute__((visibility("default")))
int32_t EOS_LobbyModification_AddMemberAttribute(void* Handle, const void* Options) {
    if (Options) {
        const void* attr = *(const void* const*)((const char*)Options + 8);
        if (attr) {
            const char* key = *(const char* const*)((const char*)attr + 8);
            if (key && !strcmp(key, "DISPLAYNAME")) {
                fill_name_if_null(attr);
                LOG("DISPLAYNAME -> '%s'", g_username);
            }
        }
    }
    void* h = real_handle();
    typedef int32_t (*F)(void*, const void*);
    F f = h ? (F)dlsym(h, "EOS_LobbyModification_AddMemberAttribute") : nullptr;
    return f ? f(Handle, Options) : 1;
}

extern "C" __attribute__((visibility("default")))
int32_t EOS_LobbyModification_AddAttribute(void* Handle, const void* Options) {
    if (Options) {
        const void* attr = *(const void* const*)((const char*)Options + 8);
        if (attr) {
            const char* key = *(const char* const*)((const char*)attr + 8);
            if (key && !strcmp(key, "OWNERNAME")) {
                fill_name_if_null(attr);
                LOG("OWNERNAME -> '%s'", g_username);
            }
        }
    }
    void* h = real_handle();
    typedef int32_t (*F)(void*, const void*);
    F f = h ? (F)dlsym(h, "EOS_LobbyModification_AddAttribute") : nullptr;
    return f ? f(Handle, Options) : 1;
}

// The game resolves a PUID -> display name here to show player names.
// For our anon PUID the display name comes back null; inject the username.
extern "C" __attribute__((visibility("default")))
int32_t EOS_Connect_CopyProductUserInfo(void* Handle, const void* Options, void** OutInfo) {
    void* h = real_handle();
    typedef int32_t (*F)(void*, const void*, void**);
    F f = h ? (F)dlsym(h, "EOS_Connect_CopyProductUserInfo") : nullptr;
    int32_t r = f ? f(Handle, Options, OutInfo) : 1;
    if (r == 0 && OutInfo && *OutInfo) {
        void* info = *OutInfo;
        const char* dn = *(const char* const*)((char*)info + 16);   // DisplayName
        EOS_ProductUserId tgt = Options ? *(EOS_ProductUserId*)((const char*)Options + 8) : nullptr;
        bool mine = (g_localpuid && tgt == g_localpuid);
        if (mine || !dn || !dn[0]) {
            *(const char**)((char*)info + 16) = g_username;
            LOG("CopyProductUserInfo mine=%d dn='%s' -> '%s'", mine, dn?dn:"(null)", g_username);
        }
    }
    return r;
}

// Guard: if we injected our static DisplayName, null it before the SDK frees the struct.
extern "C" __attribute__((visibility("default")))
void EOS_Connect_ExternalAccountInfo_Release(void* Info) {
    if (Info) {
        const char* dn = *(const char* const*)((char*)Info + 16);
        if (dn == g_username) *(const char**)((char*)Info + 16) = nullptr;
    }
    void* h = real_handle();
    typedef void (*F)(void*);
    F f = h ? (F)dlsym(h, "EOS_Connect_ExternalAccountInfo_Release") : nullptr;
    if (f) f(Info);
}

typedef void (*PFN_ConnCreateUser)(void*, const void*, void*, void*);
extern "C" __attribute__((visibility("default")))
void EOS_Connect_CreateUser(void* Handle, const void* Options, void* ClientData, void* Cb) {
    LOG("EOS_Connect_CreateUser called (creating a new anon PUID)");
    void* h = real_handle();
    PFN_ConnCreateUser f = h ? (PFN_ConnCreateUser)dlsym(h, "EOS_Connect_CreateUser") : nullptr;
    if (f) f(Handle, Options, ClientData, Cb);
}

extern "C" __attribute__((visibility("default")))
void EOS_Connect_CreateDeviceId(void* Handle, const void* Options, void* ClientData, void* Cb) {
    LOG("EOS_Connect_CreateDeviceId called by game");
    void* h = real_handle();
    PFN_CreateDeviceId f = h ? (PFN_CreateDeviceId)dlsym(h, "EOS_Connect_CreateDeviceId") : nullptr;
    if (f) f((EOS_HConnect)Handle, (const EOS_Connect_CreateDeviceIdOptions*)Options,
             ClientData, (EOS_Connect_OnCreateDeviceIdCallback)Cb);
}

typedef int32_t (*PFN_GetLoginStatus)(void*, void*);
extern "C" __attribute__((visibility("default")))
int32_t EOS_Connect_GetLoginStatus(void* Handle, void* LocalUserId) {
    void* h = real_handle();
    PFN_GetLoginStatus f = h ? (PFN_GetLoginStatus)dlsym(h, "EOS_Connect_GetLoginStatus") : nullptr;
    int32_t st = f ? f(Handle, LocalUserId) : -1;
    static int cnt = 0;
    if (cnt < 5) { LOG("EOS_Connect_GetLoginStatus -> %d (0=NotLoggedIn,2=LoggedIn)", st); cnt++; }
    // game is polling but never logs in (Steam trigger absent) -> do it ourselves
    if (st == 0 && !g_autologin_started) {
        if (!g_connect) g_connect = (EOS_HConnect)Handle;   // Handle is the connect interface
        start_autologin();
    }
    return st;
}

extern "C" __attribute__((visibility("default")))
void EOS_Auth_Login(void* Handle, const void* Options, void* ClientData, void* Cb) {
    // Options layout: {int32 ApiVersion; const EOS_Auth_Credentials* Credentials; ...}
    // EOS_Auth_Credentials: {int32 ApiVersion; const char* Id; const char* Token; int32 Type; ...}
    int32_t ctype = -1;
    const void* creds = Options ? *(const void* const*)((const char*)Options + 8) : nullptr;
    if (creds) ctype = *(const int32_t*)((const char*)creds + 24);
    LOG("EOS_Auth_Login called (Epic-account path), credential type=%d", ctype);
    void* h = real_handle();
    PFN_AuthLogin f = h ? (PFN_AuthLogin)dlsym(h, "EOS_Auth_Login") : nullptr;
    if (f) f(Handle, Options, ClientData, Cb);
}

// Heap trampoline: carries the caller's original callback + client-data through our
// create-device-id -> login chain, and owns copies of the rewritten option structs so
// they stay valid across the async ticks.
struct Ctx {
    EOS_HConnect                 h;
    EOS_Connect_OnLoginCallback  cb;
    void*                        cd;
    EOS_Connect_Credentials      creds;
    EOS_Connect_UserLoginInfo    uinfo;
    EOS_Connect_LoginOptions     opts;
    char                         name[256];
};

static void login_cb(const EOS_Connect_LoginCallbackInfo* d) {
    Ctx* c = (Ctx*)d->ClientData;
    // hand the game its callback with ITS ClientData restored (patch the field in place;
    // the SDK's callback-info memory is transient and not reused after we return).
    *(void**)((char*)d + offsetof(EOS_Connect_LoginCallbackInfo, ClientData)) = c->cd;
    LOG("DeviceID login result=%d, forwarding to game", d->ResultCode);
    EOS_Connect_OnLoginCallback cb = c->cb;
    cb(d);
    free(c);
}

static void createdevid_cb(const EOS_Connect_CreateDeviceIdCallbackInfo* d) {
    Ctx* c = (Ctx*)d->ClientData;
    // EOS_Success (created) or duplicate (already exists) are both fine — proceed to login.
    LOG("CreateDeviceId result=%d -> DeviceID login", d->ResultCode);
    if (!real_Login) { free(c); return; }
    real_Login(c->h, &c->opts, c, login_cb);
}

extern "C" __attribute__((visibility("default")))
void EOS_Connect_Login(EOS_HConnect Handle,
                       const EOS_Connect_LoginOptions* Options,
                       void* ClientData,
                       EOS_Connect_OnLoginCallback CompletionDelegate) {
    resolve_real();
    if (!real_Login || !real_CreateDeviceId || !Options) {
        if (real_Login) real_Login(Handle, Options, ClientData, CompletionDelegate);
        return;
    }

    int32_t inType = Options->Credentials ? Options->Credentials->Type : -1;
    // Only rewrite Steam credentials (STEAM_APP_TICKET=1, STEAM_SESSION_TICKET=18) — those can't
    // work on a headset with no Steam client. Epic (0) and everything else pass through untouched
    // so a real Epic-account Connect login keeps its proper identity.
    if (inType != 1 && inType != 18) {
        LOG("EOS_Connect_Login (cred type=%d) -> passthrough (not Steam)", inType);
        real_Login(Handle, Options, ClientData, CompletionDelegate);
        return;
    }
    LOG("EOS_Connect_Login intercepted (Steam cred type=%d) -> forcing Device ID", inType);

    Ctx* c = (Ctx*)calloc(1, sizeof(Ctx));
    c->h  = Handle;
    c->cb = CompletionDelegate;
    c->cd = ClientData;

    // Display name is required for a Device ID login. Keep the game's if it supplied one.
    const char* dn = g_username;
    if (Options->UserLoginInfo && Options->UserLoginInfo->DisplayName && Options->UserLoginInfo->DisplayName[0])
        dn = Options->UserLoginInfo->DisplayName;
    strncpy(c->name, dn, sizeof(c->name) - 1);

    c->creds.ApiVersion = Options->Credentials ? Options->Credentials->ApiVersion : 1;
    c->creds.Token      = nullptr;
    c->creds.Type       = EOS_ECT_DEVICEID_ACCESS_TOKEN;

    c->uinfo.ApiVersion  = EOS_CONNECT_USERLOGININFO_API_001;
    c->uinfo.DisplayName = c->name;

    c->opts.ApiVersion    = Options->ApiVersion;   // keep the game's login ApiVersion
    c->opts.Credentials   = &c->creds;
    c->opts.UserLoginInfo = &c->uinfo;

    EOS_Connect_CreateDeviceIdOptions cdo;
    cdo.ApiVersion  = EOS_CONNECT_CREATEDEVICEID_API_001;
    cdo.DeviceModel = "Meta Quest 3";
    real_CreateDeviceId(Handle, &cdo, c, createdevid_cb);
}
