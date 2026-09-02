// voice_opus.cpp — soundboard clip -> Opus voice frames for Pavlov's ServerOnVoice injection.
//
// Compiled into libpavchams.so. Scans the soundboard dir for .wav, decodes + resamples to 48kHz mono,
// Opus-encodes into 20ms frames, and wraps each frame in Pavlov's 6-byte voice-packet header:
//   [0]=0x02  [1]=seq (filled by the sender)  [2]=hdr2  [3]=0x00  [4..5]=opus_len(LE)  [6..]=opus
// pavchams streams the frames through AVoiceRouter::ServerOnVoice at 20ms cadence. The genuine mic /
// OpenSL recorder is never involved — this is a pure transmit-side injection.
#include <opus.h>
#include <dirent.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <pthread.h>

#define VO_DIR       "/sdcard/Android/data/com.vankrupt.pavlov/files/soundboard"
#define VO_RATE      48000
#define VO_FRAME     960          // 20 ms @ 48 kHz
#define VO_MAX_CLIPS 64
#define VO_NAME_MAX  40
#define VO_HDR       6

// ---- clip registry ----------------------------------------------------------
static char  g_names[VO_MAX_CLIPS][VO_NAME_MAX];
static int   g_nclips = 0;

// ---- encoded-frame store (one clip at a time) -------------------------------
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t* g_arena   = nullptr;      // packed [hdr+opus] frames
static size_t   g_arenaSz = 0, g_arenaCap = 0;
static uint32_t g_offs[32768];            // per-frame offset into arena
static uint16_t g_lens[32768];            // per-frame total length (hdr+opus)
static int      g_nframes = 0;
static int      g_loaded  = -1;           // which clip index is encoded, or -1
static int16_t* g_pcm     = nullptr;      // full resampled+gained PCM (48k mono s16) for local monitor
static int      g_pcmN    = 0;            // sample count in g_pcm

// ---- minimal RIFF/WAVE PCM loader (u8 / s16 / f32, mono/stereo) -> float mono at native rate ----
static uint32_t rd_u32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd_u16(const uint8_t* p){ return (uint16_t)(p[0]|(p[1]<<8)); }

static float* wav_load(const char* path, uint32_t* outFrames, uint32_t* outRate) {
    FILE* f = fopen(path, "rb"); if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 44 || fsz > (128<<20)) { fclose(f); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(fsz);
    if (!buf) { fclose(f); return nullptr; }
    if (fread(buf, 1, fsz, f) != (size_t)fsz) { free(buf); fclose(f); return nullptr; }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) || memcmp(buf+8, "WAVE", 4)) { free(buf); return nullptr; }

    uint16_t fmt=0, chans=0, bits=0; uint32_t rate=0;
    const uint8_t* data=nullptr; uint32_t dlen=0; long off=12;
    while (off + 8 <= fsz) {
        const uint8_t* ck = buf + off; uint32_t clen = rd_u32(ck+4);
        if (!memcmp(ck,"fmt ",4) && clen>=16) { fmt=rd_u16(ck+8); chans=rd_u16(ck+10); rate=rd_u32(ck+12); bits=rd_u16(ck+22); }
        else if (!memcmp(ck,"data",4)) { data=ck+8; dlen=(off+8+clen<=(uint32_t)fsz)?clen:(uint32_t)(fsz-off-8); }
        off += 8 + clen + (clen&1);
    }
    if (!data || !chans || !rate || (fmt!=1 && fmt!=3)) { free(buf); return nullptr; }
    uint32_t bps = bits/8; if (!bps) { free(buf); return nullptr; }
    uint32_t frames = (dlen/bps)/chans; if (!frames) { free(buf); return nullptr; }

    float* pcm = (float*)malloc((size_t)frames*sizeof(float));
    if (!pcm) { free(buf); return nullptr; }
    for (uint32_t i=0;i<frames;i++) {
        float acc=0.f;
        for (uint16_t c=0;c<chans;c++) {
            const uint8_t* s = data + ((size_t)i*chans+c)*bps; float v=0.f;
            if (fmt==3 && bits==32){ float fv; memcpy(&fv,s,4); v=fv; }
            else if (bits==16) v=(int16_t)rd_u16(s)/32768.f;
            else if (bits==8)  v=(s[0]-128)/128.f;
            else if (bits==24){ int32_t iv=(s[0]|(s[1]<<8)|(s[2]<<16)); if(iv&0x800000) iv|=~0xFFFFFF; v=iv/8388608.f; }
            else if (bits==32){ int32_t iv=(int32_t)rd_u32(s); v=iv/2147483648.f; }
            acc+=v;
        }
        pcm[i]=acc/chans;
    }
    free(buf);
    *outFrames=frames; *outRate=rate; return pcm;
}

static void arena_reset() { g_arenaSz=0; g_nframes=0; }
static bool arena_push(const uint8_t* d, uint16_t len) {
    if (g_arenaSz + len > g_arenaCap) {
        size_t nc = g_arenaCap ? g_arenaCap*2 : (1<<20);
        while (nc < g_arenaSz + len) nc *= 2;
        uint8_t* na = (uint8_t*)realloc(g_arena, nc); if (!na) return false;
        g_arena=na; g_arenaCap=nc;
    }
    if (g_nframes >= (int)(sizeof(g_offs)/sizeof(g_offs[0]))) return false;
    g_offs[g_nframes]=(uint32_t)g_arenaSz; g_lens[g_nframes]=len;
    memcpy(g_arena+g_arenaSz, d, len); g_arenaSz+=len; g_nframes++;
    return true;
}

// ---- exports ----------------------------------------------------------------
extern "C" __attribute__((visibility("default"))) int vo_scan() {
    pthread_mutex_lock(&g_mtx);
    g_nclips=0;
    DIR* dd=opendir(VO_DIR);
    if (dd) { struct dirent* e;
        while ((e=readdir(dd)) && g_nclips<VO_MAX_CLIPS) {
            const char* n=e->d_name; size_t ln=strlen(n);
            if (ln<5 || strcasecmp(n+ln-4,".wav")) continue;
            size_t cn=ln-4; if (cn>=VO_NAME_MAX) cn=VO_NAME_MAX-1;
            memcpy(g_names[g_nclips],n,cn); g_names[g_nclips][cn]=0; g_nclips++;
        }
        closedir(dd);
    }
    pthread_mutex_unlock(&g_mtx);
    return g_nclips;
}
extern "C" __attribute__((visibility("default"))) int vo_count() { return g_nclips; }
extern "C" __attribute__((visibility("default"))) const char* vo_name(int i) {
    return (i>=0 && i<g_nclips) ? g_names[i] : "";
}

// Decode + Opus-encode clip `idx` into 20ms frames with the Pavlov header (hdr2 = header byte [2]).
// Returns frame count, or -1 on failure. Heavy — call off the game thread.
extern "C" __attribute__((visibility("default"))) int vo_load(int idx, uint8_t hdr2, float gain) {
    if (idx<0 || idx>=g_nclips) return -1;
    if (gain <= 0.f) gain = 1.f;
    char path[512]; snprintf(path,sizeof path,"%s/%s.wav",VO_DIR,g_names[idx]);
    uint32_t sf=0, sr=0; float* src = wav_load(path,&sf,&sr);
    if (!src) return -1;

    int err=0; OpusEncoder* enc = opus_encoder_create(VO_RATE,1,OPUS_APPLICATION_AUDIO,&err);
    if (!enc || err!=OPUS_OK) { free(src); return -1; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
    opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    pthread_mutex_lock(&g_mtx);
    arena_reset();
    // 1) resample src (sr Hz) -> 48kHz mono int16, gain-applied. Store the whole thing (local monitor).
    double step = (double)sr / (double)VO_RATE;
    int outN = (int)((double)sf / step) + 1;
    free(g_pcm); g_pcm = (int16_t*)malloc((size_t)((outN + VO_FRAME) & ~(VO_FRAME-1)) * 2);
    bool ok = (g_pcm != nullptr); int wrote = 0;
    if (ok) for (int o = 0; o < outN; o++) {
        double sp = o * step; uint32_t i0=(uint32_t)sp; if (i0>=sf) break;
        uint32_t i1=(i0+1<sf)?i0+1:i0; float fr=(float)(sp-i0);
        float s=(src[i0]*(1.f-fr)+src[i1]*fr)*gain;
        int v=(int)lrintf(s*32767.f); if(v>32767)v=32767; else if(v<-32768)v=-32768;
        g_pcm[wrote++]=(int16_t)v;
    }
    while (wrote % VO_FRAME) g_pcm[wrote++] = 0;              // pad to a whole frame
    g_pcmN = wrote;
    // 2) Opus-encode g_pcm in 20ms frames with the Pavlov header.
    uint8_t frame[VO_HDR+1275];
    for (int off = 0; ok && off + VO_FRAME <= wrote; off += VO_FRAME) {
        int n = opus_encode(enc, g_pcm + off, VO_FRAME, frame+VO_HDR, (int)sizeof(frame)-VO_HDR);
        if (n<0) { ok=false; break; }
        frame[0]=0x02; frame[1]=0x00; frame[2]=hdr2; frame[3]=0x00;
        frame[4]=(uint8_t)(n & 0xFF); frame[5]=(uint8_t)((n>>8)&0xFF);
        if (!arena_push(frame, (uint16_t)(VO_HDR+n))) { ok=false; break; }
    }
    g_loaded = ok ? idx : -1;
    int nf = g_nframes;
    pthread_mutex_unlock(&g_mtx);
    opus_encoder_destroy(enc); free(src);
    return ok ? nf : -1;
}

extern "C" __attribute__((visibility("default"))) int vo_frames() { return g_nframes; }

// Full resampled+gained PCM (48kHz mono s16) for the local monitor player. Valid until the next vo_load.
extern "C" __attribute__((visibility("default"))) int16_t* vo_pcm() { return g_pcm; }
extern "C" __attribute__((visibility("default"))) int      vo_pcm_samples() { return g_pcmN; }

// Copy frame `i` (hdr+opus) into out (cap bytes). Returns length, or 0. Thread-safe vs vo_load.
extern "C" __attribute__((visibility("default"))) int vo_frame(int i, uint8_t* out, int cap) {
    pthread_mutex_lock(&g_mtx);
    int r=0;
    if (i>=0 && i<g_nframes) { int len=g_lens[i]; if (len<=cap) { memcpy(out, g_arena+g_offs[i], len); r=len; } }
    pthread_mutex_unlock(&g_mtx);
    return r;
}
