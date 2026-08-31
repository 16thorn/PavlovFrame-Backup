#!/usr/bin/env python3
# Generate a Steam ENCRYPTED APP TICKET (ISteamUser::RequestEncryptedAppTicket) for Pavlov Shack Frame
# (appid 3504270) -> hex to steam_ticket.txt. Feed with EOS cred type 1 (STEAM_APP_TICKET).
# Requires Steam running + logged into the owning account, steam_api64.dll here, AND the publisher must
# have set an app-ticket encryption key on Steamworks for the appid (else Request fails).
import ctypes, os, time, sys

APPID = "3504270"
os.environ["SteamAppId"] = APPID; os.environ["SteamGameId"] = APPID
open("steam_appid.txt", "w").write(APPID)

dll = ctypes.WinDLL(os.path.abspath("steam_api64.dll"))
errbuf = ctypes.create_string_buffer(1024)
dll.SteamAPI_InitFlat.restype = ctypes.c_int
dll.SteamAPI_InitFlat.argtypes = [ctypes.c_char_p]
if dll.SteamAPI_InitFlat(errbuf) != 0:
    print("SteamAPI_InitFlat FAILED msg=%s" % errbuf.value.decode(errors="ignore")); sys.exit(1)
print("SteamAPI_InitFlat OK (appid %s)" % APPID)

dll.SteamAPI_SteamUser_v023.restype = ctypes.c_void_p
user = dll.SteamAPI_SteamUser_v023()
dll.SteamAPI_GetHSteamPipe.restype = ctypes.c_int
hpipe = dll.SteamAPI_GetHSteamPipe()
if not user: print("no ISteamUser"); sys.exit(1)

req = dll.SteamAPI_ISteamUser_RequestEncryptedAppTicket
req.restype = ctypes.c_uint64
req.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
call = req(user, None, 0)
print("RequestEncryptedAppTicket call=%d (waiting for response...)" % call)

class CallbackMsg(ctypes.Structure):
    _fields_ = [("m_hSteamUser", ctypes.c_int), ("m_iCallback", ctypes.c_int),
                ("m_pubParam", ctypes.POINTER(ctypes.c_ubyte)), ("m_cubParam", ctypes.c_int)]
dll.SteamAPI_ManualDispatch_Init()
dll.SteamAPI_ManualDispatch_RunFrame.argtypes = [ctypes.c_int]
dll.SteamAPI_ManualDispatch_GetNextCallback.restype = ctypes.c_bool
dll.SteamAPI_ManualDispatch_GetNextCallback.argtypes = [ctypes.c_int, ctypes.POINTER(CallbackMsg)]
dll.SteamAPI_ManualDispatch_FreeLastCallback.argtypes = [ctypes.c_int]

RESP_CB = 154   # EncryptedAppTicketResponse_t
ready = False
for _ in range(200):
    dll.SteamAPI_ManualDispatch_RunFrame(hpipe)
    msg = CallbackMsg()
    while dll.SteamAPI_ManualDispatch_GetNextCallback(hpipe, ctypes.byref(msg)):
        if msg.m_iCallback == RESP_CB:
            res = int.from_bytes(bytes(msg.m_pubParam[0:4]), "little")   # m_eResult
            print("EncryptedAppTicketResponse: result=%d (1=OK)" % res)
            if res == 1: ready = True
        dll.SteamAPI_ManualDispatch_FreeLastCallback(hpipe)
    if ready: break
    time.sleep(0.1)

if not ready:
    print("no encrypted app ticket (publisher likely has no encryption key set for this appid)")
    try: dll.SteamAPI_Shutdown()
    except Exception: pass
    sys.exit(1)

buf = ctypes.create_string_buffer(2048)
pcb = ctypes.c_uint32(0)
get = dll.SteamAPI_ISteamUser_GetEncryptedAppTicket
get.restype = ctypes.c_bool
get.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint32)]
if not get(user, buf, 2048, ctypes.byref(pcb)):
    print("GetEncryptedAppTicket failed"); sys.exit(1)

ticket_hex = bytes(buf.raw[:pcb.value]).hex()
open("steam_ticket.txt", "w").write(ticket_hex)
open("steam_type.txt", "w").write("1")   # STEAM_APP_TICKET
print("\nAPP TICKET (%d bytes) -> steam_ticket.txt, type=1:" % pcb.value)
print(ticket_hex)
try: dll.SteamAPI_Shutdown()
except Exception: pass
