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
enum { EOS_CONNECT_USERLOGININFO_API_001 = 1, EOS_CONNECT_CREATEDEVICEID_API_001 = 1 };

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

typedef void (*EOS_Connect_OnLoginCallback)(const EOS_Connect_LoginCallbackInfo*);
typedef void (*EOS_Connect_OnCreateDeviceIdCallback)(const EOS_Connect_CreateDeviceIdCallbackInfo*);

typedef void (*PFN_Login)(EOS_HConnect, const EOS_Connect_LoginOptions*, void*, EOS_Connect_OnLoginCallback);
typedef void (*PFN_CreateDeviceId)(EOS_HConnect, const EOS_Connect_CreateDeviceIdOptions*, void*, EOS_Connect_OnCreateDeviceIdCallback);

// ---- EOS Auth ABI (for a real Epic-account login -> non-anonymous PUID) ------
typedef struct EOS_AuthHandle*          EOS_HAuth;
typedef struct EOS_EpicAccountIdDetails* EOS_EpicAccountId;
typedef int32_t EOS_ELoginCredentialType;
typedef uint32_t EOS_EAuthScopeFlags;

// EOS_ELoginCredentialType
enum { EOS_LCT_ExchangeCode = 1, EOS_LCT_PersistentAuth = 2, EOS_LCT_AccountPortal = 6 };
// EOS_EExternalCredentialType — the Epic Connect credential minted from an Auth login
enum { EOS_ECT_EPIC_ID_TOKEN = 16 };
// EOS_EAuthScopeFlags — BasicProfile|FriendsList|Presence is the classic minimal set
enum { EOS_AS_NoFlags = 0, EOS_AS_BasicProfile = 0x1, EOS_AS_FriendsList = 0x2, EOS_AS_Presence = 0x4 };

typedef struct {
    int32_t                     ApiVersion;
    const char*                 Id;
    const char*                 Token;
    EOS_ELoginCredentialType    Type;
    const void*                 SystemAuthCredentialsOptions;
    EOS_EExternalCredentialType ExternalType;
} EOS_Auth_Credentials;

typedef struct {
    int32_t                     ApiVersion;
    const EOS_Auth_Credentials* Credentials;
    EOS_EAuthScopeFlags         ScopeFlags;
} EOS_Auth_LoginOptions;

typedef struct {
    EOS_EResult          ResultCode;
    void*                ClientData;
    EOS_EpicAccountId    LocalUserId;
    const void*          PinGrantInfo;
    EOS_ContinuanceToken ContinuanceToken;
    const void*          AccountFeatureRestrictedInfo;
    EOS_EpicAccountId    SelectedAccountId;
} EOS_Auth_LoginCallbackInfo;

typedef struct {
    int32_t           ApiVersion;
    EOS_EpicAccountId AccountId;
} EOS_Auth_CopyIdTokenOptions;

typedef struct {
    int32_t           ApiVersion;
    EOS_EpicAccountId AccountId;
    const char*       JsonWebToken;
} EOS_Auth_IdToken;

// ---- EOS logging ABI (surface the SDK's own auth-failure reason) ------------
typedef struct {
    int32_t     ApiVersion;
    const char* Category;
    const char* Message;
    int32_t     Level;      // EOS_ELogLevel
} EOS_LogMessage;
typedef void (*EOS_LogMessageFunc)(const EOS_LogMessage*);

typedef void (*EOS_Auth_OnLoginCallback)(const EOS_Auth_LoginCallbackInfo*);
typedef void      (*PFN_AuthLoginReal)(EOS_HAuth, const EOS_Auth_LoginOptions*, void*, EOS_Auth_OnLoginCallback);
typedef EOS_EResult (*PFN_AuthCopyIdToken)(EOS_HAuth, const EOS_Auth_CopyIdTokenOptions*, EOS_Auth_IdToken**);
typedef EOS_HAuth (*PFN_GetAuth)(void*);

} // extern "C"

static void*              g_real              = nullptr;
static PFN_Login          real_Login          = nullptr;
static PFN_CreateDeviceId real_CreateDeviceId = nullptr;
static PFN_AuthLoginReal   real_AuthLogin     = nullptr;
static PFN_AuthCopyIdToken real_AuthCopyIdTok = nullptr;
static EOS_HAuth           g_auth             = nullptr;   // Auth interface (from EOS_Platform_Create)

// Auth mode toggle, read from a text file so no recompile is needed to switch identity strategy.
// Device-ID (anonymous) is the known-working default; player-hosted lobbies reject it with
// "Device Cannot Be authenticated". The other modes log in a REAL Epic account so the Connect PUID
// is non-anonymous — what those hosts want.
//   adb shell "echo device                > /sdcard/Android/data/com.vankrupt.pavlov/files/eosauth.txt"
//   adb shell "echo portal                > .../eosauth.txt"   # in-headset browser (shippable)
//   adb shell "echo persistent            > .../eosauth.txt"   # silent, after one portal login
//   adb shell "echo dev:127.0.0.1:6547:rat > .../eosauth.txt"  # DevAuthTool over `adb reverse` (the TEST)
// STEAM external credential types (what Pavlov's EOS integration natively uses on PC).
enum { EOS_ECT_STEAM_APP_TICKET = 1, EOS_ECT_STEAM_SESSION_TICKET = 18 };

enum AuthMode { AM_DEVICE, AM_PORTAL, AM_PERSISTENT, AM_DEVELOPER, AM_STEAM };
static AuthMode g_auth_mode = AM_DEVICE;
static char     g_dev_host[64] = "127.0.0.1";
static char     g_dev_port[16] = "6547";
static char     g_dev_cred[64] = "rat";

// Steam auth ticket (hex string) minted on a PC by a real Steam client that owns Pavlov, pushed to
// the headset. Fed to Connect as a Steam credential -> a genuine Steam-backed PUID (non-anonymous),
// which is exactly the identity a player-hosted lobby wants. See tools/mint_steam_ticket.cpp.
static char g_steam_ticket[16384] = {0};
static bool read_steam_ticket() {
    const char* paths[2] = {
        "/sdcard/Android/data/com.vankrupt.pavlov/files/steamticket.txt",
        "/data/data/com.vankrupt.pavlov/files/steamticket.txt",
    };
    for (int i = 0; i < 2; i++) {
        FILE* f = fopen(paths[i], "r");
        if (!f) continue;
        size_t n = fread(g_steam_ticket, 1, sizeof g_steam_ticket - 1, f); fclose(f);
        g_steam_ticket[n] = 0;
        // strip any whitespace/newlines the file editor added (ticket must be a clean hex run)
        size_t w = 0;
        for (size_t r = 0; g_steam_ticket[r]; r++) {
            char c = g_steam_ticket[r];
            if ((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')) g_steam_ticket[w++] = c;
        }
        g_steam_ticket[w] = 0;
        if (w >= 32) { LOG("steam ticket loaded (%zu hex chars) from %s", w, paths[i]); return true; }
    }
    LOG("steam ticket missing/too short -> cannot do Steam login");
    return false;
}

static AuthMode read_auth_mode() {
    const char* paths[2] = {
        "/sdcard/Android/data/com.vankrupt.pavlov/files/eosauth.txt",
        "/data/data/com.vankrupt.pavlov/files/eosauth.txt",
    };
    for (int i = 0; i < 2; i++) {
        FILE* f = fopen(paths[i], "r");
        if (!f) continue;
        char b[160] = {0}; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
        while (n && (b[n-1] == '\n' || b[n-1] == '\r' || b[n-1] == ' ')) b[--n] = 0;
        for (size_t k = 0; k < n && b[k] != ':'; k++) if (b[k] >= 'A' && b[k] <= 'Z') b[k] += 32;
        if (!strncmp(b, "dev:", 4)) {
            // dev:HOST:PORT:CRED  (DevAuthTool: Type=Developer, Id="HOST:PORT", Token=credential name)
            char h[64] = {0}, p[16] = {0}, c[64] = {0};
            if (sscanf(b + 4, "%63[^:]:%15[^:]:%63s", h, p, c) >= 2) {
                strncpy(g_dev_host, h, sizeof g_dev_host - 1);
                strncpy(g_dev_port, p, sizeof g_dev_port - 1);
                if (c[0]) strncpy(g_dev_cred, c, sizeof g_dev_cred - 1);
            }
            LOG("auth mode = DEVELOPER host=%s port=%s cred=%s (from %s)", g_dev_host, g_dev_port, g_dev_cred, paths[i]);
            return AM_DEVELOPER;
        }
        if (strstr(b, "steam"))      { LOG("auth mode = STEAM (from %s)", paths[i]);      return AM_STEAM; }
        if (strstr(b, "portal"))     { LOG("auth mode = PORTAL (from %s)", paths[i]);     return AM_PORTAL; }
        if (strstr(b, "persistent")) { LOG("auth mode = PERSISTENT (from %s)", paths[i]); return AM_PERSISTENT; }
        if (strstr(b, "device"))     { LOG("auth mode = DEVICE (from %s)", paths[i]);     return AM_DEVICE; }
    }
    return AM_DEVICE;   // default: keep the known-good Device-ID path
}

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
    LOG("resolved real Login=%p CreateDeviceId=%p", (void*)real_Login, (void*)real_CreateDeviceId);
}

// ---- diagnostics: prove routing + find the real login path ------------------
__attribute__((constructor)) static void on_load() {
    LOG("eosshim loaded (constructor)");
    // EOS Anti-Cheat test: if nochams.txt exists, DON'T inject the mod — isolates whether the
    // player-hosted "Authentication Failed" is EOS-AC catching our injected lib vs the resigned binary.
    FILE* nc = fopen("/sdcard/Android/data/com.vankrupt.pavlov/files/nochams.txt", "r");
    if (nc) { fclose(nc); LOG("nochams.txt present -> NOT loading libpavchams (clean-client AC test)"); return; }
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
    typedef const char* (*PFN_GetVersion)(void);
    PFN_GetVersion gv = h ? (PFN_GetVersion)dlsym(h, "EOS_GetVersion") : nullptr;
    LOG("EOS_Initialize called -> routing through shim OK (real=%p) SDK=%s", (void*)f, gv ? gv() : "?");
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
static void start_device_login() {
    if (!real_CreateDeviceId) { LOG("device login: real_CreateDeviceId null"); return; }
    LOG("starting autonomous DeviceID login on connect=%p", (void*)g_connect);
    EOS_Connect_CreateDeviceIdOptions cdo;
    cdo.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_001; cdo.DeviceModel = "Meta Quest 3";
    real_CreateDeviceId(g_connect, &cdo, nullptr, autologin_createid_cb);
}

// ---- Epic-account path: Auth login -> Connect login with EPIC_ID_TOKEN ------
// A real Epic account gives a non-anonymous Connect PUID (backed by an Epic external account), which
// is what player-hosted lobbies check for. The Auth login result code is logged loudly — that single
// line tells us whether the identity is real before we ever reach a host.
static void connect_epic_login_cb(const EOS_Connect_LoginCallbackInfo* d) {
    g_localpuid = *(EOS_ProductUserId*)((const char*)d + 16);
    LOG("EPIC Connect login result=%d, localPUID=%p (non-anonymous if result==0)", d->ResultCode, (void*)g_localpuid);
    if (d->ResultCode != 0) { LOG("EPIC Connect FAILED -> falling back to Device-ID"); start_device_login(); }
}
static void auth_login_cb(const EOS_Auth_LoginCallbackInfo* d) {
    LOG("EOS_Auth_Login result=%d LocalUserId=%p (0=Success; else portal/dev flow did not complete)",
        d->ResultCode, (void*)d->LocalUserId);
    if (d->ResultCode != 0 || !d->LocalUserId) {
        LOG("Auth login did not complete -> Device-ID fallback so you still get online");
        start_device_login();
        return;
    }
    if (!real_AuthCopyIdTok) real_AuthCopyIdTok = (PFN_AuthCopyIdToken)dlsym(real_handle(), "EOS_Auth_CopyIdToken");
    if (!real_AuthCopyIdTok) { LOG("no EOS_Auth_CopyIdToken -> cannot mint Epic Connect token"); start_device_login(); return; }
    EOS_Auth_CopyIdTokenOptions o{}; o.ApiVersion = 1; o.AccountId = d->LocalUserId;
    EOS_Auth_IdToken* tok = nullptr;
    EOS_EResult r = real_AuthCopyIdTok(g_auth, &o, &tok);
    if (r != 0 || !tok || !tok->JsonWebToken) { LOG("CopyIdToken failed r=%d -> Device-ID fallback", r); start_device_login(); return; }
    LOG("got Epic ID token (%zu chars) -> Connect login as EPIC_ID_TOKEN", strlen(tok->JsonWebToken));
    static EOS_Connect_Credentials creds; creds.ApiVersion = 1;
    static char tokbuf[4096]; strncpy(tokbuf, tok->JsonWebToken, sizeof tokbuf - 1);
    creds.Token = tokbuf; creds.Type = EOS_ECT_EPIC_ID_TOKEN;
    static EOS_Connect_LoginOptions lo; lo.ApiVersion = 2; lo.Credentials = &creds; lo.UserLoginInfo = nullptr;
    real_Login(g_connect, &lo, nullptr, connect_epic_login_cb);
    // (SDK copied the token into the login op synchronously; releasing here is fine but harmless to skip)
}
static void start_epic_login() {
    if (!real_AuthLogin) real_AuthLogin = (PFN_AuthLoginReal)dlsym(real_handle(), "EOS_Auth_Login");
    if (!real_AuthLogin || !g_auth) { LOG("Epic login: AuthLogin=%p auth=%p null -> Device-ID", (void*)real_AuthLogin, (void*)g_auth); start_device_login(); return; }

    static EOS_Auth_Credentials creds{}; creds.ApiVersion = 3;
    static char idbuf[80];
    if (g_auth_mode == AM_DEVELOPER) {
        snprintf(idbuf, sizeof idbuf, "%s:%s", g_dev_host, g_dev_port);
        creds.Id = idbuf; creds.Token = g_dev_cred; creds.Type = 4 /*EOS_LCT_Developer*/;
        LOG("Epic login via DevAuthTool id='%s' cred='%s'", idbuf, g_dev_cred);
    } else if (g_auth_mode == AM_PERSISTENT) {
        creds.Id = nullptr; creds.Token = nullptr; creds.Type = EOS_LCT_PersistentAuth;
        LOG("Epic login via PersistentAuth (needs a prior successful portal login)");
    } else { // AM_PORTAL
        creds.Id = nullptr; creds.Token = nullptr; creds.Type = EOS_LCT_AccountPortal;
        LOG("Epic login via AccountPortal (in-headset browser)");
    }
    static EOS_Auth_LoginOptions o{}; o.ApiVersion = 3; o.Credentials = &creds;
    o.ScopeFlags = EOS_AS_BasicProfile | EOS_AS_FriendsList | EOS_AS_Presence;
    real_AuthLogin(g_auth, &o, nullptr, auth_login_cb);
}

// ---- Steam path: real Steam ticket -> Steam-backed Connect PUID -------------
// EOS validates a Steam ticket server-side and the exact ticket flavour it accepts (WebApi identity
// string vs legacy session ticket, SESSION_TICKET(18) vs APP_TICKET(1)) isn't documented for this
// product. So walk a matrix of candidate ticket files x credential types in ONE boot, logging each
// result code, and stop at the first that returns Success. Push variants as steamticket.txt .. 4.
static const char* g_steam_files[] = {
    "/sdcard/Android/data/com.vankrupt.pavlov/files/steamticket.txt",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/steamticket2.txt",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/steamticket3.txt",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/steamticket4.txt",
};
static const int  g_steam_types[] = { EOS_ECT_STEAM_SESSION_TICKET, EOS_ECT_STEAM_APP_TICKET };
static int  g_steam_fi = 0, g_steam_ti = 0;          // current file / type index into the matrix
static void start_steam_attempt();

static bool load_ticket_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(g_steam_ticket, 1, sizeof g_steam_ticket - 1, f); fclose(f);
    g_steam_ticket[n] = 0;
    size_t w = 0;
    for (size_t r = 0; g_steam_ticket[r]; r++) { char c = g_steam_ticket[r];
        if ((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')) g_steam_ticket[w++] = c; }
    g_steam_ticket[w] = 0;
    return w >= 32;
}
// Self-verify our own ID token exactly like the host does — reveals what EOS resolves our identity to
// (external account type: 1=Steam) and whether the token validates. If this passes but the host still
// kicks "Authentication Failed", the rejection is host policy/anti-cheat, not our token.
static void self_verify_cb(const void* d) {
    int32_t rc   = *(const int32_t*)((const char*)d + 0);
    int32_t info = *(const int32_t*)((const char*)d + 24);   // bIsAccountInfoPresent
    int32_t atyp = *(const int32_t*)((const char*)d + 28);   // AccountIdType (1=Steam)
    const char* aid = *(const char* const*)((const char*)d + 32);
    LOG("SELF-VERIFY result=%d accountInfoPresent=%d extAccountType=%d (1=Steam) accountId=%s",
        rc, info, atyp, aid ? aid : "(null)");
}
static void self_verify_token() {
    void* h = real_handle();
    typedef EOS_EResult (*FCopy)(void*, const void*, void**);
    typedef void (*FVerify)(void*, const void*, void*, void(*)(const void*));
    FCopy   cp = (FCopy)dlsym(h, "EOS_Connect_CopyIdToken");
    FVerify vf = (FVerify)dlsym(h, "EOS_Connect_VerifyIdToken");
    if (!cp || !vf) { LOG("self-verify: CopyIdToken=%p VerifyIdToken=%p unavailable", (void*)cp, (void*)vf); return; }
    struct { int32_t v; void* puid; } copt{ 1, g_localpuid };
    void* tok = nullptr;
    EOS_EResult r = cp(g_connect, &copt, &tok);
    if (r != 0 || !tok) { LOG("self-verify: CopyIdToken failed r=%d", r); return; }
    struct { int32_t v; void* idtok; } vopt{ 1, tok };
    LOG("self-verify: verifying our own token like the host would...");
    vf(g_connect, &vopt, nullptr, self_verify_cb);
}
static void steam_login_cb(const EOS_Connect_LoginCallbackInfo* d) {
    LOG("STEAM try [file#%d type=%d] result=%d (0=OK,7000=TokenValidationFailed,7003=InvalidToken,7004=UnsupportedType)",
        g_steam_fi, g_steam_types[g_steam_ti], d->ResultCode);
    if (d->ResultCode == 0) {
        g_localpuid = *(EOS_ProductUserId*)((const char*)d + 16);
        LOG("STEAM login OK -> real Steam-backed identity, PUID=%p", (void*)g_localpuid);
        self_verify_token();
        return;
    }
    // advance the matrix: next type, then next file
    g_steam_ti++;
    if (g_steam_ti >= (int)(sizeof g_steam_types / sizeof *g_steam_types)) { g_steam_ti = 0; g_steam_fi++; }
    start_steam_attempt();
}
static void start_steam_attempt() {
    int nfiles = (int)(sizeof g_steam_files / sizeof *g_steam_files);
    while (g_steam_fi < nfiles) {
        if (load_ticket_file(g_steam_files[g_steam_fi])) break;   // found a present, valid-length ticket
        g_steam_fi++; g_steam_ti = 0;                            // skip missing files
    }
    if (g_steam_fi >= nfiles) { LOG("STEAM: all ticket variants exhausted -> Device-ID fallback"); start_device_login(); return; }
    static EOS_Connect_Credentials creds;
    creds.ApiVersion = 1; creds.Token = g_steam_ticket; creds.Type = g_steam_types[g_steam_ti];
    static EOS_Connect_LoginOptions o;
    o.ApiVersion = 2; o.Credentials = &creds; o.UserLoginInfo = nullptr;   // Steam supplies the display name
    LOG("Steam Connect login: file#%d type=%d ticketlen=%zu", g_steam_fi, creds.Type, strlen(g_steam_ticket));
    real_Login(g_connect, &o, nullptr, steam_login_cb);
}
static void start_steam_login() { g_steam_fi = 0; g_steam_ti = 0; start_steam_attempt(); }

static void start_autologin() {
    if (g_autologin_started || !g_connect) return;
    g_autologin_started = true;
    resolve_real();
    g_auth_mode = read_auth_mode();
    if      (g_auth_mode == AM_DEVICE) start_device_login();
    else if (g_auth_mode == AM_STEAM)  start_steam_login();
    else                               start_epic_login();
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
        PFN_GetAuth ga = (PFN_GetAuth)dlsym(h, "EOS_Platform_GetAuthInterface");
        if (ga) g_auth = ga(plat);
        LOG("interfaces: connect=%p auth=%p", (void*)g_connect, (void*)g_auth);
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

// ---- join-time auth tracing: who mints/verifies the Connect ID token --------
// The player-hosted join handshake exchanges an EOS Connect ID token. CopyIdToken (client mints its
// token to send) and VerifyIdToken (whoever validates it) are the whole auth conversation. Log them
// with the PUID + token length so we can see if the game even asks for our (now Steam-backed) token.
extern "C" __attribute__((visibility("default")))
EOS_EResult EOS_Connect_CopyIdToken(void* Handle, const void* Options, void** OutIdToken) {
    void* h = real_handle();
    typedef EOS_EResult (*F)(void*, const void*, void**);
    F f = h ? (F)dlsym(h, "EOS_Connect_CopyIdToken") : nullptr;
    EOS_EResult r = f ? f(Handle, Options, OutIdToken) : 1;
    void* puid = Options ? *(void**)((const char*)Options + 8) : nullptr;   // LocalUserId@8
    const char* jwt = (r == 0 && OutIdToken && *OutIdToken) ? *(const char* const*)((char*)*OutIdToken + 16) : nullptr;
    LOG("EOS_Connect_CopyIdToken PUID=%p result=%d tokenlen=%zu", puid, r, jwt ? strlen(jwt) : 0);
    return r;
}

typedef void (*PFN_VerifyCb)(const void*);
extern "C" __attribute__((visibility("default")))
void EOS_Connect_VerifyIdToken(void* Handle, const void* Options, void* ClientData, PFN_VerifyCb Cb) {
    const void* idtok = Options ? *(const void* const*)((const char*)Options + 8) : nullptr;   // IdToken*@8
    void* puid = idtok ? *(void**)((const char*)idtok + 8) : nullptr;
    const char* jwt = idtok ? *(const char* const*)((const char*)idtok + 16) : nullptr;
    LOG("EOS_Connect_VerifyIdToken called PUID=%p tokenlen=%zu", puid, jwt ? strlen(jwt) : 0);
    void* h = real_handle();
    typedef void (*F)(void*, const void*, void*, PFN_VerifyCb);
    F f = h ? (F)dlsym(h, "EOS_Connect_VerifyIdToken") : nullptr;
    if (f) f(Handle, Options, ClientData, Cb);
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
