// minter.cpp — PC-side Steam encrypted-app-ticket minter + LAN relay for the Pavlov Shack (3504270)
// Steam-Frame sideload. Runs on a Windows PC where Steam is logged into the account that OWNS
// Pavlov Shack; mints a REAL encrypted app ticket (Steam encrypts it server-side with Vankrupt's key
// — we never need the key) and serves it + the account's SteamID64 over TCP to the Quest steamshim.
//
// The Quest presents this ticket as EOS STEAM_APP_TICKET(1); EOS validates it against Steam -> a
// genuine Steam-typed PUID -> the "Unable to authenticate device" gate passes.
//
// BUILD (MSVC x64, Steamworks SDK unzipped to .\sdk):
//   cl /EHsc /std:c++17 /I sdk\public minter.cpp /link sdk\redistributable_bin\win64\steam_api64.lib ws2_32.lib
//   copy sdk\redistributable_bin\win64\steam_api64.dll .
//   echo 3504270 > steam_appid.txt
//   minter.exe            (Steam must be running + logged into the owning account)
//
// The Quest fetches on demand:  one TCP connect -> we mint (cached 90s; Steam rate-limits to ~1/min)
// -> reply "STEAMID:<dec>\nTICKET:<hex>\n".  Listens on 0.0.0.0:48010.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <atomic>
#include "steam_api.h"

#pragma comment(lib, "ws2_32.lib")

static constexpr uint32_t APPID   = 3504270;
static constexpr int      PORT    = 48010;
static constexpr uint64_t CACHE_MS = 90'000;   // reuse a minted ticket for 90s (Steam ~1/min limit)

static uint64_t now_ms() { return GetTickCount64(); }

// ---- ticket minter: RequestEncryptedAppTicket -> EncryptedAppTicketResponse_t -> GetEncryptedAppTicket
class Minter {
public:
    // Blocking mint. Returns hex ticket in `outHex` + SteamID64 in `outId`, or false on failure.
    bool mint(std::string& outHex, uint64_t& outId) {
        m_done = false; m_ok = false;
        SteamAPICall_t call = SteamUser()->RequestEncryptedAppTicket(nullptr, 0);
        if (call == k_uAPICallInvalid) { printf("[minter] RequestEncryptedAppTicket returned invalid call\n"); return false; }
        m_callResult.Set(call, this, &Minter::OnResponse);

        uint64_t start = now_ms();
        while (!m_done && now_ms() - start < 15'000) { SteamAPI_RunCallbacks(); Sleep(30); }
        if (!m_done) { printf("[minter] timed out waiting for EncryptedAppTicketResponse_t\n"); return false; }
        if (!m_ok)   { printf("[minter] EncryptedAppTicketResponse_t failed (EResult=%d)\n", (int)m_result); return false; }

        uint8_t buf[2048]; uint32_t len = 0;
        if (!SteamUser()->GetEncryptedAppTicket(buf, sizeof buf, &len) || len == 0) {
            printf("[minter] GetEncryptedAppTicket failed (len=%u)\n", len); return false; }

        static const char* H = "0123456789abcdef";
        outHex.clear(); outHex.reserve(len * 2);
        for (uint32_t i = 0; i < len; i++) { outHex.push_back(H[buf[i] >> 4]); outHex.push_back(H[buf[i] & 0xF]); }
        outId = SteamUser()->GetSteamID().ConvertToUint64();
        printf("[minter] minted ticket len=%u bytes, SteamID64=%llu\n", len, (unsigned long long)outId);
        return true;
    }
private:
    void OnResponse(EncryptedAppTicketResponse_t* r, bool ioFail) {
        m_result = ioFail ? k_EResultIOFailure : r->m_eResult;
        m_ok = (!ioFail && r->m_eResult == k_EResultOK);
        m_done = true;
    }
    CCallResult<Minter, EncryptedAppTicketResponse_t> m_callResult;
    volatile bool m_done = false, m_ok = false;
    EResult m_result = k_EResultOK;
};

// ---- tiny TCP relay --------------------------------------------------------
static std::string g_cacheHex; static uint64_t g_cacheId = 0; static uint64_t g_cacheAt = 0;

static void serve_one(SOCKET c, Minter& m) {
    std::string hex; uint64_t id = 0;
    if (g_cacheAt && now_ms() - g_cacheAt < CACHE_MS && !g_cacheHex.empty()) {
        hex = g_cacheHex; id = g_cacheId; printf("[relay] serving cached ticket\n");
    } else if (m.mint(hex, id)) {
        g_cacheHex = hex; g_cacheId = id; g_cacheAt = now_ms();
    } else {
        const char* err = "ERROR: mint failed\n"; send(c, err, (int)strlen(err), 0); return;
    }
    char hdr[128]; int n = snprintf(hdr, sizeof hdr, "STEAMID:%llu\nTICKET:", (unsigned long long)id);
    send(c, hdr, n, 0);
    send(c, hex.c_str(), (int)hex.size(), 0);
    send(c, "\n", 1, 0);
}

int main() {
    FILE* f = fopen("steam_appid.txt", "w"); if (f) { fprintf(f, "%u\n", APPID); fclose(f); }

    if (SteamAPI_RestartAppIfNecessary(APPID)) { printf("relaunching via Steam...\n"); return 0; }
    if (!SteamAPI_Init()) { printf("SteamAPI_Init failed — is Steam running + logged in, and steam_api64.dll present?\n"); return 1; }
    if (!SteamUser() || !SteamUser()->BLoggedOn()) { printf("Steam user not logged on\n"); return 1; }
    printf("Steam up. Account SteamID64=%llu, appid=%u\n",
           (unsigned long long)SteamUser()->GetSteamID().ConvertToUint64(), APPID);

    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    BOOL yes = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof yes);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(PORT);
    if (bind(s, (sockaddr*)&a, sizeof a) || listen(s, 4)) { printf("bind/listen on %d failed\n", PORT); return 1; }
    printf("relay listening on 0.0.0.0:%d — point the Quest at THIS PC's LAN IP\n", PORT);

    Minter m;
    for (;;) {
        sockaddr_in ca{}; int cl = sizeof ca;
        SOCKET c = accept(s, (sockaddr*)&ca, &cl);
        if (c == INVALID_SOCKET) continue;
        printf("[relay] client %s connected\n", inet_ntoa(ca.sin_addr));
        serve_one(c, m);
        closesocket(c);
    }
}
