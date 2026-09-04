# mint.ps1 — drive steam_api64.dll's flat API to mint a real encrypted app ticket for Pavlov Shack
# (3504270) using the logged-in Steam account. No compiler/SDK — P/Invoke via Add-Type. Prints the
# SteamID64 + ticket hex. Steam must be running + logged into the account that OWNS 3504270.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir
[Environment]::CurrentDirectory = $dir

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class S {
    [DllImport("kernel32", CharSet=CharSet.Unicode)] public static extern bool SetDllDirectory(string p);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern int SteamAPI_InitFlat(byte[] errMsg);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern void SteamAPI_Shutdown();
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern void SteamAPI_RunCallbacks();
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr SteamAPI_SteamUser_v023();
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr SteamAPI_SteamUtils_v010();
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern ulong SteamAPI_ISteamUser_GetSteamID(IntPtr s);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern bool  SteamAPI_ISteamUser_BLoggedOn(IntPtr s);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern ulong SteamAPI_ISteamUser_RequestEncryptedAppTicket(IntPtr s, IntPtr data, int cb);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern bool  SteamAPI_ISteamUser_GetEncryptedAppTicket(IntPtr s, byte[] buf, int max, out uint len);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern bool  SteamAPI_ISteamUtils_IsAPICallCompleted(IntPtr s, ulong call, out bool failed);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern bool  SteamAPI_ISteamUtils_GetAPICallResult(IntPtr s, ulong call, byte[] cb, int cubCb, int iExpected, out bool failed);

    // Returns "STEAMID:<dec>|TICKET:<hex>" or "ERROR:<msg>"
    public static string Mint() {
        SetDllDirectory(Environment.CurrentDirectory);
        byte[] err = new byte[1024];
        int ir = SteamAPI_InitFlat(err);
        if (ir != 0) return "ERROR: SteamAPI_InitFlat=" + ir + " '" + Encoding.ASCII.GetString(err).TrimEnd('\0') + "' (Steam not running / not logged in / no license / steam_appid.txt missing)";
        try {
            IntPtr user = SteamAPI_SteamUser_v023();
            IntPtr utils = SteamAPI_SteamUtils_v010();
            if (user == IntPtr.Zero || utils == IntPtr.Zero) return "ERROR: null interface (user/utils accessor)";
            if (!SteamAPI_ISteamUser_BLoggedOn(user)) return "ERROR: Steam user not logged on";
            ulong id = SteamAPI_ISteamUser_GetSteamID(user);

            ulong call = SteamAPI_ISteamUser_RequestEncryptedAppTicket(user, IntPtr.Zero, 0);
            if (call == 0) return "ERROR: RequestEncryptedAppTicket returned invalid call handle";

            const int CB_ENC_APP_TICKET = 154; // k_iSteamUserCallbacks(100)+54
            byte[] cbBuf = new byte[8]; bool failed = false; bool done = false; int eresult = -1;
            DateTime start = DateTime.UtcNow;
            while ((DateTime.UtcNow - start).TotalMilliseconds < 15000) {
                SteamAPI_RunCallbacks();
                bool f2;
                if (SteamAPI_ISteamUtils_IsAPICallCompleted(utils, call, out f2)) {
                    if (SteamAPI_ISteamUtils_GetAPICallResult(utils, call, cbBuf, 4, CB_ENC_APP_TICKET, out failed)) {
                        eresult = BitConverter.ToInt32(cbBuf, 0); done = true; break;
                    }
                }
                System.Threading.Thread.Sleep(30);
            }
            if (!done) return "ERROR: timed out waiting for EncryptedAppTicketResponse_t";
            if (failed) return "ERROR: API call IO failure";
            if (eresult != 1) return "ERROR: EncryptedAppTicketResponse EResult=" + eresult + " (1=OK; 2=Fail, 25=LimitExceeded/rate, 9=NoLicense?)";

            byte[] tk = new byte[2048]; uint len = 0;
            if (!SteamAPI_ISteamUser_GetEncryptedAppTicket(user, tk, tk.Length, out len) || len == 0)
                return "ERROR: GetEncryptedAppTicket failed (len=" + len + ")";
            StringBuilder sb = new StringBuilder((int)len * 2);
            for (uint i = 0; i < len; i++) sb.Append(tk[i].ToString("x2"));
            return "STEAMID:" + id + "|LEN:" + len + "|TICKET:" + sb.ToString();
        } finally { SteamAPI_Shutdown(); }
    }
}
"@

$r = [S]::Mint()
Write-Output $r
