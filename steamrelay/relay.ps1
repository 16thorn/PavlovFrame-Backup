# relay.ps1 — persistent LAN relay: mints a real Pavlov Shack (3504270) encrypted app ticket on demand
# and serves it to the Quest steamshim over TCP 48010. Steam initialized once; ticket cached 90s
# (Steam rate-limits RequestEncryptedAppTicket to ~1/min). Reply: "STEAMID:<dec>\nTICKET:<hex>\n".
# Run:  powershell -NoProfile -ExecutionPolicy Bypass -File relay.ps1   (Steam must be logged in)
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir; [Environment]::CurrentDirectory = $dir
# Steam reads steam_appid.txt from the CWD at init to know which app we are (Pavlov Shack = 3504270).
Set-Content -Path (Join-Path $dir 'steam_appid.txt') -Value '3504270' -Encoding ascii -NoNewline

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class S {
    [DllImport("kernel32", CharSet=CharSet.Unicode)] public static extern bool SetDllDirectory(string p);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern int   SteamAPI_InitFlat(byte[] e);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern void  SteamAPI_RunCallbacks();
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr SteamAPI_SteamUser_v023();
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr SteamAPI_SteamUtils_v010();
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern ulong SteamAPI_ISteamUser_GetSteamID(IntPtr s);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern bool  SteamAPI_ISteamUser_BLoggedOn(IntPtr s);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern ulong SteamAPI_ISteamUser_RequestEncryptedAppTicket(IntPtr s, IntPtr d, int cb);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern bool  SteamAPI_ISteamUser_GetEncryptedAppTicket(IntPtr s, byte[] b, int m, out uint l);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern bool  SteamAPI_ISteamUtils_IsAPICallCompleted(IntPtr s, ulong c, out bool f);
    [DllImport("steam_api64.dll", CallingConvention=CallingConvention.Cdecl)] public static extern bool  SteamAPI_ISteamUtils_GetAPICallResult(IntPtr s, ulong c, byte[] cb, int n, int exp, out bool f);

    static IntPtr U, Ut; static ulong Id;
    static string cacheHex = ""; static DateTime cacheAt = DateTime.MinValue;

    public static string Init() {
        SetDllDirectory(Environment.CurrentDirectory);
        byte[] e = new byte[1024]; int ir = SteamAPI_InitFlat(e);
        if (ir != 0) return "ERROR: InitFlat=" + ir + " '" + Encoding.ASCII.GetString(e).TrimEnd('\0') + "'";
        U = SteamAPI_SteamUser_v023(); Ut = SteamAPI_SteamUtils_v010();
        if (U == IntPtr.Zero || Ut == IntPtr.Zero) return "ERROR: null user/utils";
        if (!SteamAPI_ISteamUser_BLoggedOn(U)) return "ERROR: not logged on";
        Id = SteamAPI_ISteamUser_GetSteamID(U);
        return "OK SteamID64=" + Id;
    }
    static string MintFresh() {
        ulong call = SteamAPI_ISteamUser_RequestEncryptedAppTicket(U, IntPtr.Zero, 0);
        if (call == 0) return null;
        byte[] cb = new byte[8]; bool failed = false; int er = -1; bool done = false;
        DateTime t = DateTime.UtcNow;
        while ((DateTime.UtcNow - t).TotalMilliseconds < 15000) {
            SteamAPI_RunCallbacks(); bool f2;
            if (SteamAPI_ISteamUtils_IsAPICallCompleted(Ut, call, out f2))
                if (SteamAPI_ISteamUtils_GetAPICallResult(Ut, call, cb, 4, 154, out failed)) { er = BitConverter.ToInt32(cb,0); done = true; break; }
            System.Threading.Thread.Sleep(30);
        }
        if (!done || failed || er != 1) return null;
        byte[] tk = new byte[2048]; uint len = 0;
        if (!SteamAPI_ISteamUser_GetEncryptedAppTicket(U, tk, tk.Length, out len) || len == 0) return null;
        StringBuilder sb = new StringBuilder((int)len*2);
        for (uint i=0;i<len;i++) sb.Append(tk[i].ToString("x2"));
        return sb.ToString();
    }
    // "STEAMID:<id>\nTICKET:<hex>\n" or "ERROR:...\n"
    public static string Serve() {
        string hex;
        if ((DateTime.UtcNow - cacheAt).TotalSeconds < 90 && cacheHex.Length > 0) hex = cacheHex;
        else { hex = MintFresh(); if (hex == null) return "ERROR: mint failed\n"; cacheHex = hex; cacheAt = DateTime.UtcNow; }
        return "STEAMID:" + Id + "\nTICKET:" + hex + "\n";
    }
}
"@

$init = [S]::Init()
Write-Output "[relay] $init"
if ($init -notlike 'OK*') { exit 1 }

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, 48010)
$listener.Start()
Write-Output "[relay] listening on 0.0.0.0:48010 - Quest fetches from this PC LAN IP"
while ($true) {
    $client = $listener.AcceptTcpClient()
    $ep = $client.Client.RemoteEndPoint
    try {
        $resp = [S]::Serve()
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($resp)
        $stream = $client.GetStream()
        $stream.Write($bytes, 0, $bytes.Length); $stream.Flush()
        Write-Output ("[relay] served {0}  (resp {1} chars)" -f $ep, $resp.Length)
    } catch { Write-Output "[relay] err $ep : $_" }
    finally { $client.Close() }
}
