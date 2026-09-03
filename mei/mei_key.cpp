// mei_key.cpp — 2016 client license gate. Activation validates against the key server (host in
// files/keyserver.txt, e.g. "192.168.1.20:8016") which binds the key to this device's HWID and returns
// the tier + expiry. The result is cached to key.dat so the periodic in-game re-check is OFFLINE (never
// blocks the game thread on a socket). A couple of embedded lifetime master keys work with no server.
// Tiers: 0=weekly(7d) 1=monthly(30d) 2=lifetime. Key format: 2016-XXXX-XXXX-XXXX.
#include "mei_key.h"
#include <sys/system_properties.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>

#define KEY_FILE    "/sdcard/Android/data/com.vankrupt.pavlov/files/key.dat"
#define SERVER_FILE "/sdcard/Android/data/com.vankrupt.pavlov/files/keyserver.txt"

// Offline master keys. EMPTY for release — any key listed here is PUBLIC (this file is on GitHub) and
// would let anyone unlock for free. Add your own ONLY in a private/local build for offline testing.
static const struct KE { const char* key; int tier; } EMBED[] = {
    { "", 2 },   // placeholder (never matches a real key)
};
static const int DAYS[3] = { 7, 30, 0 };

static bool g_lic=false; static int g_tier=-1; static long g_daysleft=-1;
static char g_key[64]={0}; static char g_hwid[24]={0};

static void calc_hwid(){
    if(g_hwid[0]) return;
    char s[PROP_VALUE_MAX]={0};
    __system_property_get("ro.serialno", s);
    if(!s[0]) __system_property_get("ro.boot.serialno", s);
    if(!s[0]) __system_property_get("ro.boot.hardware", s);
    if(!s[0]) strcpy(s,"nodevice");
    uint32_t h=2166136261u; for(int i=0;s[i];i++){ h^=(uint8_t)s[i]; h*=16777619u; }
    snprintf(g_hwid,sizeof g_hwid,"%08X",h);
}
const char* mei_key_hwid(){ calc_hwid(); return g_hwid; }

// ---- minimal HTTP GET (plaintext) to the key server; returns body in `body` -------------------------
static bool http_get(const char* path, char* body, int blen){
    char hp[128]={0}; FILE* f=fopen(SERVER_FILE,"r");
    if(f){ if(fgets(hp,sizeof hp,f)){ int L=(int)strlen(hp); while(L>0&&(hp[L-1]=='\n'||hp[L-1]=='\r'||hp[L-1]==' '))hp[--L]=0; } fclose(f); }
    if(!hp[0]) return false;
    char host[96]={0}; int port=8016;
    { char* c=strchr(hp,':'); if(c){ *c=0; port=atoi(c+1);} strncpy(host,hp,sizeof host-1); }
    struct addrinfo hints; memset(&hints,0,sizeof hints); hints.ai_socktype=SOCK_STREAM;
    struct addrinfo* res=nullptr; char ps[8]; snprintf(ps,sizeof ps,"%d",port);
    if(getaddrinfo(host,ps,&hints,&res)!=0||!res) return false;
    int s=socket(res->ai_family,SOCK_STREAM,0);
    if(s<0){ freeaddrinfo(res); return false; }
    struct timeval tv; tv.tv_sec=3; tv.tv_usec=0;
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    if(connect(s,res->ai_addr,res->ai_addrlen)!=0){ close(s); freeaddrinfo(res); return false; }
    freeaddrinfo(res);
    char req[512]; snprintf(req,sizeof req,"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",path,host);
    send(s,req,strlen(req),0);
    char buf[2048]={0}; int tot=0,n;
    while((n=recv(s,buf+tot,(int)sizeof buf-1-tot,0))>0){ tot+=n; if(tot>=(int)sizeof buf-1) break; }
    close(s); buf[tot]=0;
    char* p=strstr(buf,"\r\n\r\n"); if(!p) return false; p+=4;
    strncpy(body,p,blen-1); body[blen-1]=0;
    return true;
}

static const KE* find_embed(const char* k){ if(!k||!k[0]) return nullptr;
    for(auto& e:EMBED) if(e.key[0] && !strcmp(e.key,k)) return &e; return nullptr; }

// OFFLINE re-check: trust the cached key.dat (hwid|key|tier|expiry) until expiry. No socket here.
bool mei_key_check(){
    calc_hwid();
    g_lic=false; g_tier=-1; g_daysleft=-1; g_key[0]=0;
    FILE* f=fopen(KEY_FILE,"r"); if(!f) return false;
    char line[200]={0}; if(!fgets(line,sizeof line,f)){ fclose(f); return false; } fclose(f);
    char *a=strtok(line,"|"), *b=strtok(nullptr,"|"), *c=strtok(nullptr,"|"), *d=strtok(nullptr,"|\r\n");
    if(!a||!b||!c||!d) return false;
    if(strcmp(a,g_hwid)) return false;                 // bound to another device
    int tier=atoi(c); long expiry=atol(d); long now=(long)time(nullptr);
    if(now>=expiry) return false;                      // expired
    g_lic=true; g_tier=tier; g_daysleft=(expiry>=4102444800L)?99999:(expiry-now)/86400;
    strncpy(g_key,b,sizeof g_key-1);
    return true;
}

// ONLINE activate: ask the server (or embedded fallback), then cache to key.dat.
bool mei_key_activate(const char* k){
    calc_hwid();
    char key[64]={0}; strncpy(key,k,sizeof key-1);
    for(char* p=key;*p;p++) if(*p>='a'&&*p<='z') *p-=32;   // upper-case
    int tier=-1; long expiry=0;
    // 1) server
    char path[256]; snprintf(path,sizeof path,"/validate?key=%s&hwid=%s", key, g_hwid);
    char body[256]={0};
    if(http_get(path, body, sizeof body)){
        if(!strncmp(body,"OK",2)){ int t; long e; if(sscanf(body,"OK %d %ld",&t,&e)==2){ tier=t; expiry=e; } }
    }
    // 2) embedded fallback (offline master keys)
    if(tier<0){ const KE* e=find_embed(key); if(e){ tier=e->tier;
        expiry = DAYS[tier]>0 ? (long)time(nullptr)+(long)DAYS[tier]*86400 : 4102444800L; } }
    if(tier<0) return false;
    FILE* f=fopen(KEY_FILE,"w"); if(!f) return false;
    fprintf(f,"%s|%s|%d|%ld", g_hwid, key, tier, expiry); fclose(f);
    return mei_key_check();
}
bool        mei_key_licensed(){ return g_lic; }
int         mei_key_tier(){ return g_tier; }
long        mei_key_days_left(){ return g_daysleft; }
const char* mei_key_str(){ return g_key; }
