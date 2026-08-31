#!/usr/bin/env python3
# Generate a CLASSIC Steam session ticket (ISteamUser::GetAuthSessionTicket) for Pavlov Shack Frame
# (appid 3504270) and write it as hex to steam_ticket.txt. This is the ticket flavor Pavlov's EOS
# integration actually validates (the game imports GetAuthSessionTicket + OnAuthSessionTicketResponse,
# NOT GetAuthTicketForWebApi). Requires Steam running + logged into the account that OWNS 3504270,
# and steam_api64.dll in this folder. No compiler needed.
import ctypes, os, time, sys

APPID = (sys.argv[1] if len(sys.argv) > 1 else "3504270")   # override: python steam_session_ticket.py 555160
os.environ["SteamAppId"] = APPID; os.environ["SteamGameId"] = APPID
open("steam_appid.txt", "w").write(APPID)

dll = ctypes.WinDLL(os.path.abspath("steam_api64.dll"))
errbuf = ctypes.create_string_buffer(1024)
dll.SteamAPI_InitFlat.restype = ctypes.c_int
dll.SteamAPI_InitFlat.argtypes = [ctypes.c_char_p]
rc = dll.SteamAPI_InitFlat(errbuf)
if rc != 0:
    print("SteamAPI_InitFlat FAILED rc=%d msg=%s (Steam running+logged in? own appid %s?)"
          % (rc, errbuf.value.decode(errors="ignore"), APPID)); sys.exit(1)
print("SteamAPI_InitFlat OK (appid %s)" % APPID)

dll.SteamAPI_SteamUser_v023.restype = ctypes.c_void_p
user = dll.SteamAPI_SteamUser_v023()
dll.SteamAPI_GetHSteamPipe.restype = ctypes.c_int
hpipe = dll.SteamAPI_GetHSteamPipe()
print("ISteamUser=0x%x hPipe=%d" % (user or 0, hpipe))
if not user: print("no ISteamUser"); sys.exit(1)

# HAuthTicket GetAuthSessionTicket(self, void* pTicket, int cbMaxTicket, uint32* pcbTicket,
#                                  const SteamNetworkingIdentity* pSnid)
# The flat API is __cdecl; passing pSnid=NULL is fine for EOS (no target identity). Older DLLs that
# take only 3 data args ignore the extra NULL harmlessly.
buf = ctypes.create_string_buffer(2048)
pcb = ctypes.c_uint32(0)
g = dll.SteamAPI_ISteamUser_GetAuthSessionTicket
g.restype = ctypes.c_uint32
g.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
handle = g(user, buf, 2048, ctypes.byref(pcb), None)
print("GetAuthSessionTicket handle=%u cbTicket=%d (waiting for validation callback...)" % (handle, pcb.value))
if handle == 0:
    print("GetAuthSessionTicket returned invalid handle"); sys.exit(1)

# The ticket bytes are already in buf, but the ticket is not usable until Steam confirms it via
# GetAuthSessionTicketResponse_t (k_iSteamUserCallbacks=100 + 63 = 163) with m_eResult == k_EResultOK(1).
class CallbackMsg(ctypes.Structure):
    _fields_ = [("m_hSteamUser", ctypes.c_int), ("m_iCallback", ctypes.c_int),
                ("m_pubParam", ctypes.POINTER(ctypes.c_ubyte)), ("m_cubParam", ctypes.c_int)]
dll.SteamAPI_ManualDispatch_Init()
dll.SteamAPI_ManualDispatch_RunFrame.argtypes = [ctypes.c_int]
dll.SteamAPI_ManualDispatch_GetNextCallback.restype = ctypes.c_bool
dll.SteamAPI_ManualDispatch_GetNextCallback.argtypes = [ctypes.c_int, ctypes.POINTER(CallbackMsg)]
dll.SteamAPI_ManualDispatch_FreeLastCallback.argtypes = [ctypes.c_int]

TICKET_CB = 163   # GetAuthSessionTicketResponse_t
validated = False
for _ in range(200):                       # ~20s
    dll.SteamAPI_ManualDispatch_RunFrame(hpipe)
    msg = CallbackMsg()
    while dll.SteamAPI_ManualDispatch_GetNextCallback(hpipe, ctypes.byref(msg)):
        if msg.m_iCallback == TICKET_CB:
            p = msg.m_pubParam
            h   = int.from_bytes(bytes(p[0:4]), "little")   # m_hAuthTicket
            res = int.from_bytes(bytes(p[4:8]), "little")   # m_eResult
            print("AuthSessionTicketResponse: handle=%u result=%d" % (h, res))
            if h == handle and res == 1: validated = True
        dll.SteamAPI_ManualDispatch_FreeLastCallback(hpipe)
    if validated: break
    time.sleep(0.1)

if not validated:
    print("ticket not validated by Steam (timeout/rejected)")
    try: dll.SteamAPI_Shutdown()
    except Exception: pass
    sys.exit(1)

ticket = bytes(buf.raw[:pcb.value])
ticket_hex = ticket.hex()
open("steam_ticket.txt", "w").write(ticket_hex)
print("\nSESSION TICKET (%d bytes) -> steam_ticket.txt:" % pcb.value)
print(ticket_hex)
try: dll.SteamAPI_Shutdown()
except Exception: pass
