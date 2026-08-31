#!/usr/bin/env python3
# Generate a Steam WebApi auth ticket (what EOS wants) for Pavlov Shack Frame (appid 3504270)
# and write it as hex to steam_ticket.txt. Requires Steam running + logged into the account that
# owns 3504270, and steam_api64.dll (Pavlov TV's) in this folder. No compiler needed.
import ctypes, os, time, sys

APPID = "3504270"
# EOS validates the WebApi ticket by passing this identity to Steam; it must match. Override from argv.
import sys as _sys
IDENTITY = (_sys.argv[1].encode() if len(_sys.argv) > 1 else b"epiconlineservices")
print("using WebApi identity = %r" % IDENTITY)
os.environ["SteamAppId"] = APPID; os.environ["SteamGameId"] = APPID
open("steam_appid.txt","w").write(APPID)

dll = ctypes.WinDLL(os.path.abspath("steam_api64.dll"))
# modern SDK: SteamAPI_InitFlat(char err[1024]) -> ESteamAPIInitResult (0=OK)
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

# HAuthTicket GetAuthTicketForWebApi(ISteamUser*, const char* pchIdentity)
g = dll.SteamAPI_ISteamUser_GetAuthTicketForWebApi
g.restype = ctypes.c_uint32; g.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
handle = g(user, IDENTITY)
print("GetAuthTicketForWebApi handle=%u (waiting for callback...)" % handle)

# manual callback dispatch to catch GetTicketForWebApiResponse_t (callback id 168)
class CallbackMsg(ctypes.Structure):
    _fields_ = [("m_hSteamUser", ctypes.c_int), ("m_iCallback", ctypes.c_int),
                ("m_pubParam", ctypes.POINTER(ctypes.c_ubyte)), ("m_cubParam", ctypes.c_int)]
dll.SteamAPI_ManualDispatch_Init()
dll.SteamAPI_ManualDispatch_RunFrame.argtypes = [ctypes.c_int]
dll.SteamAPI_ManualDispatch_GetNextCallback.restype = ctypes.c_bool
dll.SteamAPI_ManualDispatch_GetNextCallback.argtypes = [ctypes.c_int, ctypes.POINTER(CallbackMsg)]
dll.SteamAPI_ManualDispatch_FreeLastCallback.argtypes = [ctypes.c_int]

TICKET_CB = 168   # k_iSteamUserCallbacks(100) + 68
ticket_hex = None
for _ in range(200):                       # ~20s
    dll.SteamAPI_ManualDispatch_RunFrame(hpipe)
    msg = CallbackMsg()
    while dll.SteamAPI_ManualDispatch_GetNextCallback(hpipe, ctypes.byref(msg)):
        if msg.m_iCallback == TICKET_CB:
            p = msg.m_pubParam
            # GetTicketForWebApiResponse_t: hAuthTicket@0, eResult@4, cubTicket@8, rgubTicket@12
            res = int.from_bytes(bytes(p[4:8]), "little")
            cub = int.from_bytes(bytes(p[8:12]), "little")
            data = bytes(p[12:12+cub])
            print("WebApi ticket callback: result=%d bytes=%d" % (res, cub))
            if res == 1 and cub > 0: ticket_hex = data.hex()
        dll.SteamAPI_ManualDispatch_FreeLastCallback(hpipe)
    if ticket_hex: break
    time.sleep(0.1)

if not ticket_hex:
    print("no ticket received (timeout)");
    try: dll.SteamAPI_Shutdown()
    except Exception: pass
    sys.exit(1)
open("steam_ticket.txt","w").write(ticket_hex)
print("\nWEBAPI TICKET (%d bytes) -> steam_ticket.txt:" % (len(ticket_hex)//2))
print(ticket_hex)
try: dll.SteamAPI_Shutdown()
except Exception: pass
