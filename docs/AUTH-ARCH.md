# Pavlov Shack — full authentication architecture (reverse-engineered)

Reconstructed 2026-08-30 from our in-process SDK dump (`SDK/` — 363 packages, native `lib+0x…`
offsets) cross-referenced against `libUnreal.so` disassembly (llvm-objdump) and our `eosshim.cpp`
interposer. Six subsystems were mapped and adversarially verified in parallel. Offsets are
module-relative to `libUnreal.so` (module base = vaddr 0). Every claim is tagged **[C]** confirmed
(disasm / strings / reflection / our shim) or **[I]** inferred.

---

## 1. Executive summary

Pavlov runs a **bespoke online stack** (`UOnlineManager` hub → `UOnlineAuthentication`,
`UOnlineConnection`, `UOnlineAnticheat`, `UOnlineLobby`, `UGameSessionServer`, …) layered over the
raw **EOS C SDK** (bound by name against `libEOSSDK.so` — the exact seam our shim interposes) plus a
**Pavlov master server** at `https://prod2-shack-pavlov-ms.vankrupt.net`. Multiplayer identity is an
**EOS Connect ProductUserId (PUID)**; matchmaking uses **EOS Lobby** (not EOS Sessions — zero
`EOS_Sessions_*` imports); player-hosted transport is **EOS P2P**; dedicated servers use IP netcode +
master-server key registration. A joining client passes **three independent gates**: (1) **identity**
— an EOS Connect ID token verified server-side by `EOS_Connect_VerifyIdToken`; (2) **device
attestation** — a Google **SafetyNet** JWS minted in Java over a server nonce, re-verified at the
master server; (3) **runtime anti-cheat** — the **EOS Anti-Cheat** client↔server message protocol with
a server-side *ClientActionRequired* kick verdict. **Identity is fully solved** by our shim
(Device-ID or a real Steam ticket both validate). The wall that kills a modded/resigned client is
gates (2) and (3), both **externally validated** (Google's signing service; the EOS AC server
interface) — not by any client-trusted code. Player-hosted (listen) hosts arm gates 2–3; the official
dedicated path does not, which is exactly why dedicated works and player-hosted does not.

---

## 2. The full handshake, in order

1. **EOS bootstrap** — `EOS_Initialize` → `EOS_Platform_Create` (Product `b6acc1fe…`, Sandbox
   `023fcb0f…`, Deployment `770970a4…`, ClientId `xyza7891DJ7HVmsrPRkNI0R8QkGpYf8x`) →
   `EOS_Platform_GetConnectInterface`. **[C]** (our shim interposes `EOS_Platform_Create`).
2. **Identity mint** — `UOnlineAuthentication` picks a `UOnlineCredential` subclass and mints a
   platform token: Device → `EOS_Connect_CreateDeviceId(DeviceModel="Quest")` (caller `lib+0x43a0838`);
   Steam → `SteamInternal_FindOrCreateUserInterface("SteamUser023")` → session ticket (`lib+0x43b3364`);
   Epic → `EOS_Auth_Login` + `EOS_Auth_CopyUserAuthToken` (`lib+0x43a0eb0`/`0x43a0d70`). **[C]**
3. **Connect login** — common handler `lib+0x43a3200` builds `EOS_Connect_LoginOptions`
   (`Credentials.Type` read live from `[cred+0x78]+0x5c`; DEVICEID_ACCESS_TOKEN=10 / STEAM / EPIC) →
   `EOS_Connect_Login` (`lib+0x43a32ec`). On `InvalidUser` the callback (`lib+0x43a3318`) calls
   `EOS_Connect_CreateUser` (`lib+0x43a339c`) to mint a fresh anonymous PUID, then retries. **[C]**
4. **PUID cached** — login callback stores the PUID; `EOS_Connect_AddNotifyLoginStatusChanged`
   (`lib+0x43a2960`) fires `OnLoginStatusChanged`. `GetLocalPlatformId` (`lib+0x427688c`) is a plain
   cached-FString getter (no live EOS call). **[C]**
5. **Master-server login** — `UOnlineConnection::Connect` (`lib+0x43a4a3c`) rewrites
   `https://prod2-shack-pavlov-ms.vankrupt.net` → `wss://…/ws` and opens a WebSocket; in parallel the
   client HTTP-POSTs `/auth/v1/login` (builder `lib+0x439fe24`) with JSON
   `{device_id, display_name, platform_id, access_token}` and header `Authorization: Bearer <token>`
   where **access_token = the EOS Connect ID token** (`EOS_Connect_CopyIdToken`, caller `lib+0x43a34c0`).
   Success → `/auth/login/ok`. **[C]** strings/builders; **[I]** access_token=IdToken.
6. **Net connection + lobby** — `EOS_Lobby_CreateLobbySearch/JoinLobby` (thunks `lib+0x427b4b4`/
   `0x427b5ec`) resolve the target lobby; `EOS_P2P_AcceptConnection`/`AddNotifyPeerConnectionRequest`/
   `QueryNATType` bring up the P2P transport to the listen host. **[C]** thunks; **[I]** EOS bl targets.
7. **Server identity check** — host calls `ClientAuthenticate()` (`lib+0x42b6f28`); client replies
   `ServerAuthenticate(FString IdToken)` (`lib+0x42b6e3c`); host verifies the JWT with
   `EOS_Connect_VerifyIdToken` (`lib+0x43a3e0c`, adjacent to the `"Authentication failed"` literal
   `@0x1d33b38` xref'd `0x43a3830`). **A genuine (incl. Steam-ticket) PUID passes.** **[C]**
8. **Device attestation** — host issues a nonce (stored `AttestationNonce` @`+0xad8`) via
   `ClientRequestAttestation(Nonce)` (`lib+0x42b6d80`); client runs **Google SafetyNet** in Java
   (`GameActivity` JNI `nativeOnSafetyNetAttestationSucceeded`/`…Failed`, name literals `@0x5c81`/
   `@0x5cce`) → returns `ServerSubmitAttestation(Nonce, AttestationToken)` (`lib+0x42b6c38`); the token
   is re-verified at the master server `/auth/v1/attestation/verify` (fn `lib+0x445c100`) with
   `{nonce, attestation_token, product_user_id}`. Failure → `"Device integrity check failed"`
   (`@0x1d33b8c`, xref `0x445c83c`). **[C]**
9. **Runtime anti-cheat** — `UOnlineAnticheat::Initialize` (`lib+0x43a2094`) grabs
   `EOS_Platform_GetAntiCheat{Client,Server}Interface` (`lib+0x43a21c8`/`0x43a21d4`) and registers
   notifies; the EOS AC protocol runs as opaque blobs relayed by `ServerAnticheatMessage`
   (`lib+0x42b94e0` → `EOS_AntiCheatServer_ReceiveMessageFromClient`) and `ClientAnticheatMessage`
   (`lib+0x42b95cc` → `EOS_AntiCheatClient_ReceiveMessageFromServer`, caller `lib+0x43a2760`). **[C]**
10. **Verdict** — if identity + attestation + EOS-AC all pass within `AuthTimeout` (@`+0x944`), the
    host sets `APavlovPlayerState.bAuthenticated` (@`+0x448`) = true and admits the player. Otherwise:
    the `EOS_AntiCheatServer_AddNotifyClientActionRequired` callback (`lib+0x43a212c`) returns
    *RemovePlayer*, **or** the timer fires `OnAuthTimedout` (`lib+0x42b6c24` / impl `lib+0x445bdac`)
    while `authState` (controller `+0x150`) is still `3` (pending) → `AGameSession::KickPlayer`
    (GameSession vtable slot `+0x758`) → `ClientWasKicked(FText reason)` → connection `Destroy`. **[C]**
11. **Sanctions (parallel track)** — the host queries EOS Player Sanctions for the joiner's PUID;
    `OnQueryPlayerSanctionsComplete` (`lib+0x428cae4`) kicks any holder of
    `ESanctionType::RESTRICT_GAME_ACCESS`(=5) with `"You're Banned from Online Play"`. **[C]**

---

## 3. Identity layer (Steam / Epic / Oculus / Device)

All platforms converge on **one** `EOS_Connect_Login` and produce **one** PUID + one ID token:

| Path | Token source | External account | Result |
|---|---|---|---|
| **Device-ID** (our shim's forced path) | `EOS_Connect_CreateDeviceId("Quest")` → type `DEVICEID_ACCESS_TOKEN`(10) | none (anonymous per-headset) | valid PUID, verifiable ID token |
| **Steam** | `ISteamUser023` session/app ticket via `SteamInternal_FindOrCreateUserInterface` | real Steam account | valid PUID bound to Steam |
| **Epic** | `EOS_Auth_Login` → `EOS_Auth_CopyUserAuthToken` → `EPIC_ID_TOKEN` | real Epic account | valid PUID bound to Epic |

The client's join credential is always `EOS_Connect_CopyIdToken` (`lib+0x43a34c0`) for the **local**
PUID — the exact FString handed to `ServerAuthenticate(IdToken)`. **Identity is satisfiable by a
modded client**; our shim's Device-ID or a real Steam ticket both pass `EOS_Connect_VerifyIdToken`.
This is *proven* by the operator's observation: a verified real Steam account is still kicked.

---

## 4. EOS layer

- **Not** Epic's UE `OnlineSubsystemEOS`/`EOSPlus`/`SocketSubsystemEOS` plugin (those module strings
  are absent) — Pavlov calls the EOS **C API** by name against `libEOSSDK.so`. ~104 unique `EOS_*`
  import strings (file `~0x6c00–0x8300`). **[C]**
- **Connect** (identity): `Login`/`CreateDeviceId`/`CreateUser`/`CopyIdToken`/`VerifyIdToken`/
  `CopyProductUserInfo`/`GetLoginStatus`/`AddNotifyLoginStatusChanged`/`AddNotifyAuthExpiration`.
- **Lobby** (matchmaking): full `EOS_Lobby_*`/`LobbyModification_*`/`LobbySearch_*`/`LobbyDetails_*`,
  surfaced through Steam-shaped structs (`FSteamLobbyInfo`/`FSteamLobbyMember`) for crossplatform.
- **P2P** (transport): `SendPacket`/`ReceivePacket`/`AcceptConnection`/`AddNotifyPeerConnectionRequest`/
  `QueryNATType`/`CloseConnections`.
- **Anti-Cheat**: `EOS_Platform_GetAntiCheat{Client,Server}Interface` + `AddNotifyMessageTo{Server,
  Client}` + `AddNotifyClientActionRequired` + `ReceiveMessageFrom{Server,Client}`. **No
  `RegisterClient`/`SetClientDetails`/`BeginSession` imports** → Pavlov uses the *message-relay +
  ClientActionRequired verdict* model, not full session provisioning. **[C]**

---

## 5. Master-server backend

- Host: `https://prod2-shack-pavlov-ms.vankrupt.net` (`@0x1bce530`, xref `0x43ab4d4`/`0x43c6258`).
- Two transports: a **WebSocket** `wss://…/ws` (`UOnlineConnection`) for the auth/session RPC
  lifecycle, and **libcurl HTTP** (`Authorization: Bearer %s` `@0x202dbd7`) for REST.
- Endpoints (strings): `/auth/v1/login`, `/auth/v1/attestation`, `/auth/v1/attestation/verify`,
  `/sessions/v1/{create,join,server/report,server/dispatch}`, `/servers/v1/heartbeat`,
  `/matchmaking/v1/queue`, `/servers/v2/list`. **[C]** strings.
- Role: **identity + session broker + policy gate**. It authenticates the client (access_token =
  IdToken), can demand an attestation, and can refuse with `EConnectorFailureReason`
  *Banned/WhitelistOnly/WrongPlatform/ServerFull/WrongPin* → `UOnlineManager::OnServerConnectFailure`
  **before** brokering a host. Server hosts register with `ServerKey`/`SERVERKEY` (`@0x1d54978`) via
  `/servers/v1/heartbeat`. Necessary but **not** the terminal wall — the operator's kick happens
  *after* a successful dispatch, in-match.

---

## 6. Anti-cheat & attestation — the wall

Two server-verified walls, both external, sitting behind the (passing) identity check:

### Gate A — Device attestation (Google SafetyNet)
- Server nonce → `ClientRequestAttestation(Nonce)` (`lib+0x42b6d80`).
- Client mints a **SafetyNet JWS** in Java (`nativeOnSafetyNetAttestationSucceeded` `@lib+0x2da5f88`).
  The JWS is **signed by Google** and embeds the running **package name**, the **APK signing-cert
  SHA-256 digest** (`apkCertificateDigestSha256`), and the **basicIntegrity/ctsProfileMatch** verdict.
- Client → `ServerSubmitAttestation(Nonce, AttestationToken)` (`lib+0x42b6c38`); the master re-verifies
  at `/auth/v1/attestation/verify` (`lib+0x445c100`) bound to `{nonce, product_user_id}`. Mismatch →
  `"nonce mismatch"`; bad token → `"Device integrity check failed"` (`lib+0x445c83c`/`0x447cc24`/
  `0x447d944`).
- No `"PlayIntegrity"`/`"EasyAntiCheat"` literal exists — attestation is **SafetyNet** (now deprecated
  by Google, an operational note but irrelevant to us). **[C]** strings/JNI; **[I]** exact JWS fields.

### Gate B — EOS Anti-Cheat runtime integrity
- `UOnlineAnticheat::Initialize` (`lib+0x43a2094`) arms the AC interfaces. The AC client attests
  **process/module integrity**; our injected `libpavchams.so` + patched `libUnreal.so` trip it. The
  host's `EOS_AntiCheatServer_AddNotifyClientActionRequired` callback (`lib+0x43a212c`) raises
  *RemovePlayer* → `KickPlayer`. **[C]** imports/callers.

### Surfacing
- `bAuthenticated` (`PlayerState+0x448`) never flips → `AuthTimeout` (`+0x944`) fires `OnAuthTimedout`
  (impl `lib+0x445bdac`) with in-binary reason `"Authentication time out"`; user-facing
  `"Authentication Failed"` / `"Device Cannot Be authenticated"` are client-side `LOCTEXT` surfacings
  of the server's teardown, **not** independent checks. **[C]**

---

## 7. Where a modded/sideloaded client dies — verdict

**Blunt: not client-side interceptable.** Identity (Gate/step 7) passes — that's solved. The two
walls that reject us are validated by **external authorities**, not by any code running in our process:

- **SafetyNet JWS** is signed by **Google's** attestation service over a **server-issued nonce** and
  attests the **APK signing certificate + package + device integrity**. Our APK is **resigned with
  ratkey** (different `apkCertificateDigestSha256`) and **sideloaded on a modded process** → Google
  will not issue a passing token, and the token cannot be forged (no Google signing key). Hooking
  `ServerSubmitAttestation`/the JNI callback on the client only lets us submit a token we cannot
  legitimately obtain.
- **EOS Anti-Cheat** messages are validated by the **EOS AC server interface / Epic backend**. The
  server's *ClientActionRequired* verdict is computed off-client. Hooking `ServerAnticheatMessage`/
  `ClientAnticheatMessage` on our side cannot manufacture AC state a patched binary does not possess.

There is **no client-side hook point** that produces a genuine attestation or AC report for a modified
package. This confirms the SESSION-2 note ("detects modified files, kicks, unpatchable, server-side")
and names the mechanism precisely. **The mod stays dedicated-server-only.** Dedicated hosts don't arm
`EOS_AntiCheatServer` / the attestation path, so they admit our shim on identity + sanctions alone.

*(The only ways a build passes are the ones we can't do: an unmodified, officially-signed, store-
installed client on an un-rooted device — i.e. not a sideload.)*

---

## 8. Component reference table

| Component | Native offset | Role |
|---|---|---|
| `EOS_Connect_Login` (caller) | `lib+0x43a32ec` | identity login (all platforms) |
| `EOS_Connect_CreateDeviceId` | `lib+0x43a0838` | Device-ID mint (`DeviceModel="Quest"`) |
| `EOS_Connect_CreateUser` | `lib+0x43a339c` | anonymous PUID fallback on InvalidUser |
| `EOS_Connect_CopyIdToken` | `lib+0x43a34c0` | client join ID token (→ ServerAuthenticate) |
| `EOS_Connect_VerifyIdToken` | `lib+0x43a3e0c` | **server** identity check |
| `EOS_Auth_Login` / `CopyUserAuthToken` | `lib+0x43a0eb0` / `0x43a0d70` | Epic credential |
| `SteamInternal_FindOrCreateUserInterface` | `lib+0x43b3364` | Steam credential (`SteamUser023`) |
| `UOnlineAuthentication::GetLocalPlatformId` | `lib+0x427688c` | cached PUID/platform id getter |
| `UOnlineConnection::Connect` | `lib+0x43a4a3c` (thunk `0x4276d5c`) | open `wss://…/ws` to master |
| `OnWebSocketConnected` / `OnWebSocketMessage` | `lib+0x43a40a4` / `0x43a4e50` | master login + JSON dispatch |
| `/auth/v1/login` builder | `lib+0x439fe24` | master identity login (Bearer IdToken) |
| `/auth/v1/attestation` builder | `lib+0x43ba84c` | submit SafetyNet token |
| `/auth/v1/attestation/verify` | `lib+0x445c100` | server attestation verify |
| `ClientAuthenticate` (RPC) | `lib+0x42b6f28` | host → client: begin identity |
| `ServerAuthenticate(IdToken)` (RPC) | `lib+0x42b6e3c` | client → host: EOS ID token |
| `ClientRequestAttestation(Nonce)` (RPC) | `lib+0x42b6d80` | host → client: attestation nonce |
| `ServerSubmitAttestation(Nonce,Token)` (RPC) | `lib+0x42b6c38` | client → host: SafetyNet JWS |
| `Server/ClientAnticheatMessage(DataBlob)` (RPC) | `lib+0x42b94e0` / `0x42b95cc` | EOS AC message relay |
| `OnAuthTimedout` (RPC / impl) | `lib+0x42b6c24` / `0x445bdac` | timeout → kick ("Authentication time out") |
| `UOnlineAnticheat::Initialize` | `lib+0x43a2094` | arms EOS AC client+server interfaces |
| `EOS_AntiCheatServer_AddNotifyClientActionRequired` | caller `lib+0x43a212c` | **the kick verdict channel** |
| `nativeOnSafetyNetAttestationSucceeded` (JNI) | `lib+0x2da5f88` | SafetyNet result → game thread |
| `AGameSession::KickPlayer` | GameSession vtable `+0x758` | the single kick sink → `ClientWasKicked` + Destroy |
| `OnQueryPlayerSanctionsComplete` | `lib+0x428cae4` | ban gate (RESTRICT_GAME_ACCESS=5) |
| `APavlovPlayerController.AuthTimeout` | field `+0x944` | auth timer |
| `APavlovPlayerController.AttestationNonce` | field `+0xad8` | server nonce |
| `APavlovPlayerController` authState | field `+0x150` | `3` = auth pending |
| `APavlovPlayerState.bAuthenticated` | field `+0x448` | set true only on full pass |

**Config:** Product `b6acc1fef71e4c6ab494151c85875d16`, Sandbox `023fcb0f2b5043c08fe1cda975f7aaf1`,
Deployment `770970a45fc24cbc8da3dc41f269bcdf`, OAuth ClientId `xyza7891DJ7HVmsrPRkNI0R8QkGpYf8x`,
Master `https://prod2-shack-pavlov-ms.vankrupt.net`.

---

## 9. Confidence & open questions

**Disasm/strings/reflection-confirmed [C]:** the master-server host + REST/WS endpoints; EOS Connect
identity path incl. `VerifyIdToken`; the RPC handshake shape and offsets; SafetyNet JNI + the
`/attestation/verify` path and failure strings; the EOS AntiCheat import set and the
`AddNotifyClientActionRequired` verdict channel; the `KickPlayer` sink and `OnAuthTimedout`.

**Inferred [I], needs a live trace to fully settle:**
- That the master `access_token` is precisely the EOS Connect IdToken (strongly implied by adjacency,
  not byte-traced).
- The exact SafetyNet JWS field the server rejects on (package vs cert-digest vs integrity verdict).
- Whether the dedicated path *disables* AC/attestation entirely or merely doesn't arm the
  server-side verdict — both produce the observed "dedicated works" result.
- Precise `bl` stub offsets for a few EOS AC calls (PLT/GOT-resolved, not adrp-string anchored).

**To settle live:** hook (log-only) `EOS_Connect_VerifyIdToken`, `EOS_AntiCheatServer_*`, and the
`/auth/v1/attestation/verify` HTTP body on a listen-server join, and diff a dedicated vs listen join.
This confirms which gate fires first and whether the dedicated path is disabled or merely permissive —
but it does not change the verdict in §7.
