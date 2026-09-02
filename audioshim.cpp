// audioshim.cpp — OpenSLES mic-hijack soundboard for Pavlov (Steam Frame build) on Meta Quest.
//
// Compiled INTO libpavchams.so (not shipped as libOpenSLES.so). libOpenSLES.so is a PUBLIC Android
// system library, so an APK-bundled copy of that name is ignored by the linker — the name-swap idiom
// that works for eosshim/steamshim CANNOT work here. Instead pavchams (already injected) GOT-patches
// libUnreal's slCreateEngine slot to our sb_slCreateEngine (see sb_hook_fn / sb_install below); the
// genuine SL functions come from the real system lib via dlopen("libOpenSLES.so").
//
// From that one hook everything cascades: our engine wrap -> CreateAudioRecorder -> recorder object
// -> Android simple-buffer-queue recorder (format read live). The buffer-queue completion callback
// fires each time SL hands the game a fresh mic buffer; we intercept it and, while a soundboard clip
// is playing, overwrite that buffer with resampled clip PCM BEFORE the game's callback reads it — so
// the encoder ships our audio to the lobby. If the Quest mic yields silence (the known dead path),
// this simply *becomes* the mic.
//
// Control: exports sb_get_control() (a static block) + sb_scan() + sb_install()/sb_hook_fn(). pavchams
// drives playback + the clip list from the mei "Soundboard" tab and installs the GOT hook.

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <dlfcn.h>
#include <dirent.h>
#include <pthread.h>
#include <android/log.h>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cmath>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "AUDIOSHIM", __VA_ARGS__)

// Built with -fvisibility=hidden, so every symbol libUnreal binds (the three SL entrypoints) or
// pavchams dlsym's (sb_*) MUST be forced back to default visibility, or it stays hidden = unresolved.
#define SB_EXPORT extern "C" __attribute__((visibility("default")))

#define SB_DIR      "/sdcard/Android/data/com.vankrupt.pavlov/files/soundboard"
#define SB_MAX_CLIPS   64
#define SB_NAME_MAX    40
#define SB_QLEN        16          // per-bq pending-buffer FIFO depth

// ============================================================================
//  shared control block (pavchams writes; the shim reads on the audio thread)
// ============================================================================
extern "C" {
struct SbControl {
    int32_t  version;                            // ABI tag = 1
    int32_t  n_clips;                             // populated by sb_scan()
    char     names[SB_MAX_CLIPS][SB_NAME_MAX];    // clip display names (filename, no ext)

    volatile int32_t play_req;                   // menu sets clip index to start; shim resets to -1
    volatile int32_t stop_req;                   // menu sets 1 to stop; shim resets to 0
    volatile int32_t loop;                        // 1 = loop the active clip
    volatile int32_t mix_mic;                     // 1 = add real mic under the clip; 0 = replace
    volatile float   gain;                        // clip gain (linear); default 1.0

    // live state (shim writes; menu reads for UI feedback)
    volatile int32_t cur_clip;                    // index currently playing, or -1
    volatile int32_t recorder_live;               // 1 once the game opened an SL recorder
    volatile int32_t rec_rate, rec_chans, rec_bits;
};
}

static SbControl g_ctl = { 1, 0, {{0}}, -1, 0, 0, 0, 1.0f, -1, 0, 0, 0, 0 };

// ============================================================================
//  clip registry (decoded to float mono at native rate)
// ============================================================================
struct Clip {
    char     name[SB_NAME_MAX];
    float*   pcm;        // mono float, [-1,1]
    uint32_t frames;
    uint32_t rate;
};
static Clip           g_clips[SB_MAX_CLIPS];
static int            g_nclips = 0;
static pthread_mutex_t g_clip_mtx = PTHREAD_MUTEX_INITIALIZER;

// ---- minimal RIFF/WAVE PCM parser (PCM u8 / s16 / f32, mono or stereo) -----
static uint32_t rd_u32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd_u16(const uint8_t* p){ return (uint16_t)(p[0]|(p[1]<<8)); }

static bool wav_load(const char* path, Clip* out) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 44 || fsz > (64<<20)) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc(fsz);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, fsz, f) != (size_t)fsz) { free(buf); fclose(f); return false; }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) || memcmp(buf+8, "WAVE", 4)) { free(buf); return false; }

    uint16_t fmt = 0, chans = 0, bits = 0; uint32_t rate = 0;
    const uint8_t *data = nullptr; uint32_t dlen = 0;
    long off = 12;
    while (off + 8 <= fsz) {
        const uint8_t* ck = buf + off;
        uint32_t clen = rd_u32(ck + 4);
        if (!memcmp(ck, "fmt ", 4) && clen >= 16) {
            fmt = rd_u16(ck+8); chans = rd_u16(ck+10); rate = rd_u32(ck+12); bits = rd_u16(ck+22);
        } else if (!memcmp(ck, "data", 4)) {
            data = ck + 8;
            dlen = (off + 8 + clen <= (uint32_t)fsz) ? clen : (uint32_t)(fsz - off - 8);
        }
        off += 8 + clen + (clen & 1);   // chunks are word-aligned
    }
    if (!data || !chans || !rate || (fmt != 1 && fmt != 3)) { free(buf); return false; }

    uint32_t bytesPerSamp = bits / 8;
    if (!bytesPerSamp) { free(buf); return false; }
    uint32_t totalSamps = dlen / bytesPerSamp;
    uint32_t frames = totalSamps / chans;
    if (!frames) { free(buf); return false; }

    float* pcm = (float*)malloc((size_t)frames * sizeof(float));
    if (!pcm) { free(buf); return false; }

    for (uint32_t i = 0; i < frames; i++) {
        float acc = 0.f;
        for (uint16_t c = 0; c < chans; c++) {
            const uint8_t* s = data + ((size_t)i * chans + c) * bytesPerSamp;
            float v = 0.f;
            if (fmt == 3 && bits == 32)      { float fv; memcpy(&fv, s, 4); v = fv; }
            else if (bits == 16)             { v = (int16_t)rd_u16(s) / 32768.f; }
            else if (bits == 8)              { v = (s[0] - 128) / 128.f; }
            else if (bits == 32)             { int32_t iv = (int32_t)rd_u32(s); v = iv / 2147483648.f; }
            else if (bits == 24)             { int32_t iv = (s[0]|(s[1]<<8)|(s[2]<<16));
                                               if (iv & 0x800000) iv |= ~0xFFFFFF; v = iv / 8388608.f; }
            acc += v;
        }
        pcm[i] = acc / chans;   // downmix to mono
    }
    free(buf);
    out->pcm = pcm; out->frames = frames; out->rate = rate;
    return true;
}

static void clips_free_locked() {
    for (int i = 0; i < g_nclips; i++) { free(g_clips[i].pcm); g_clips[i].pcm = nullptr; }
    g_nclips = 0;
}

// (re)scan SB_DIR for *.wav and decode. Safe to call anytime (mutex-guarded).
SB_EXPORT int sb_scan() {
    pthread_mutex_lock(&g_clip_mtx);
    clips_free_locked();
    DIR* d = opendir(SB_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) && g_nclips < SB_MAX_CLIPS) {
            const char* n = e->d_name;
            size_t ln = strlen(n);
            if (ln < 5) continue;
            const char* ext = n + ln - 4;
            if (strcasecmp(ext, ".wav")) continue;
            char path[512]; snprintf(path, sizeof path, "%s/%s", SB_DIR, n);
            Clip c; memset(&c, 0, sizeof c);
            if (!wav_load(path, &c)) { LOG("clip skip (bad wav): %s", n); continue; }
            size_t cn = ln - 4; if (cn >= SB_NAME_MAX) cn = SB_NAME_MAX - 1;
            memcpy(c.name, n, cn); c.name[cn] = 0;
            g_clips[g_nclips++] = c;
        }
        closedir(d);
    }
    // publish names to the control block for the menu
    g_ctl.n_clips = g_nclips;
    for (int i = 0; i < g_nclips; i++) {
        strncpy(g_ctl.names[i], g_clips[i].name, SB_NAME_MAX - 1);
        g_ctl.names[i][SB_NAME_MAX - 1] = 0;
    }
    pthread_mutex_unlock(&g_clip_mtx);
    LOG("sb_scan: %d clip(s) in %s", g_nclips, SB_DIR);
    return g_nclips;
}

SB_EXPORT SbControl* sb_get_control() { return &g_ctl; }

// ============================================================================
//  playback engine — fill the recorder's PCM buffer with clip audio
// ============================================================================
static int      g_active   = -1;      // active clip index, or -1
static double   g_cursor   = 0.0;     // fractional read position (in clip-native frames)

// Render `frames` frames of the active clip at (dstRate, dstChans) as interleaved bits-format PCM
// into `dst`. Applies gain; optionally leaves the existing (mic) samples underneath when mix_mic.
static void render_into(void* dst, uint32_t frames, uint32_t dstRate,
                        uint32_t dstChans, uint32_t bits, bool mix) {
    pthread_mutex_lock(&g_clip_mtx);
    if (g_active < 0 || g_active >= g_nclips) { pthread_mutex_unlock(&g_clip_mtx); return; }
    Clip* c = &g_clips[g_active];
    double step = (double)c->rate / (double)dstRate;
    float  gain = g_ctl.gain;

    int16_t* d16 = (int16_t*)dst; uint8_t* d8 = (uint8_t*)dst; float* d32 = (float*)dst;

    for (uint32_t i = 0; i < frames; i++) {
        double pos = g_cursor + (double)i * step;
        float s = 0.f;
        if (pos < c->frames) {
            uint32_t i0 = (uint32_t)pos; uint32_t i1 = i0 + 1 < c->frames ? i0 + 1 : i0;
            float fr = (float)(pos - i0);
            s = (c->pcm[i0] * (1.f - fr) + c->pcm[i1] * fr) * gain;
        }
        for (uint32_t ch = 0; ch < dstChans; ch++) {
            size_t idx = (size_t)i * dstChans + ch;
            if (bits == 16) {
                float base = mix ? (d16[idx] / 32768.f) : 0.f;
                float v = base + s; if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
                d16[idx] = (int16_t)lrintf(v * 32767.f);
            } else if (bits == 8) {
                float base = mix ? ((d8[idx] - 128) / 128.f) : 0.f;
                float v = base + s; if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
                d8[idx] = (uint8_t)(lrintf(v * 127.f) + 128);
            } else if (bits == 32) {
                float base = mix ? d32[idx] : 0.f;
                float v = base + s; if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
                d32[idx] = v;
            }
        }
    }
    g_cursor += (double)frames * step;
    if (g_cursor >= c->frames) {
        if (g_ctl.loop) { g_cursor = fmod(g_cursor, (double)c->frames); }
        else { g_active = -1; g_cursor = 0.0; g_ctl.cur_clip = -1; }
    }
    pthread_mutex_unlock(&g_clip_mtx);
}

// Poll the control block (called from the audio callback). Handle play/stop requests.
static void pump_control() {
    int stop = g_ctl.stop_req;
    if (stop) { g_ctl.stop_req = 0; pthread_mutex_lock(&g_clip_mtx);
                g_active = -1; g_cursor = 0.0; g_ctl.cur_clip = -1; pthread_mutex_unlock(&g_clip_mtx); }
    int req = g_ctl.play_req;
    if (req >= 0) { g_ctl.play_req = -1; pthread_mutex_lock(&g_clip_mtx);
                    if (req < g_nclips) { g_active = req; g_cursor = 0.0; g_ctl.cur_clip = req; }
                    pthread_mutex_unlock(&g_clip_mtx); }
}

// ============================================================================
//  vtable wrapping (recover ctx via container_of on the patched dispatch)
// ============================================================================
// A recorder's negotiated PCM format, captured at CreateAudioRecorder time.
struct RecCtx {
    bool          used;
    SLObjectItf_  disp;              // our patched copy of the recorder-object dispatch
    const SLObjectItf_* realDisp;
    uint32_t      rate, chans, bits;
};
static RecCtx g_recs[8];

struct BqCtx {
    bool                          used;
    SLAndroidSimpleBufferQueueItf_ disp;      // patched bq dispatch
    const SLAndroidSimpleBufferQueueItf_* realDisp;
    slAndroidSimpleBufferQueueCallback gameCb;
    void*                         gameCtx;
    RecCtx*                       rec;
    void*                         bufs[SB_QLEN];   // FIFO of enqueued buffers awaiting fill
    uint32_t                      sizes[SB_QLEN];
    int                           head, tail;
};
static BqCtx g_bqs[8];

struct EngCtx {
    bool          used;
    SLEngineItf_  disp;              // patched engine-interface dispatch
    const SLEngineItf_* realDisp;
};
static EngCtx g_engs[4];

struct EngObjCtx {
    bool          used;
    SLObjectItf_  disp;              // patched engine-OBJECT dispatch
    const SLObjectItf_* realDisp;
};
static EngObjCtx g_engObjs[4];

// Interface-id globals resolved from the real lib at load (NOT linked) — so this wrapper carries no
// DT_NEEDED on libOpenSLES.so (which is now us) and no -lOpenSLES self-reference.
static SLInterfaceID g_iid_bq     = nullptr;   // SL_IID_ANDROIDSIMPLEBUFFERQUEUE
static SLInterfaceID g_iid_engine = nullptr;   // SL_IID_ENGINE
static SLInterfaceID g_iid_outmix = nullptr;   // SL_IID_OUTPUTMIX
static SLInterfaceID g_iid_play   = nullptr;   // SL_IID_PLAY
static bool iid_eq(const SLInterfaceID a, const SLInterfaceID b) {
    return a && b && !memcmp(a, b, sizeof(struct SLInterfaceID_));
}

static pthread_mutex_t g_wrap_mtx = PTHREAD_MUTEX_INITIALIZER;
template<class T, int N> static T* slot(T (&pool)[N]) {
    for (int i = 0; i < N; i++) if (!pool[i].used) { pool[i].used = true; return &pool[i]; }
    return nullptr;
}
#define CTX_OF(self, Type, field) \
    ((Type*)((char*)(*(void**)(self)) - offsetof(Type, field)))

// ---- buffer-queue thunks ---------------------------------------------------
static void bq_completed(SLAndroidSimpleBufferQueueItf caller, void* pContext) {
    BqCtx* b = (BqCtx*)pContext;
    static int once = 0; if (!once) { once = 1; LOG("bq_completed: first mic buffer (active=%d)", g_active); }
    pump_control();
    // pop the completed (head) buffer; SL just wrote mic PCM into it.
    if (b->head != b->tail) {
        void* buf = b->bufs[b->head]; uint32_t sz = b->sizes[b->head];
        b->head = (b->head + 1) % SB_QLEN;
        if (g_active >= 0 && b->rec) {
            uint32_t bits  = b->rec->bits  ? b->rec->bits  : 16;
            uint32_t chans = b->rec->chans ? b->rec->chans : 1;
            uint32_t rate  = b->rec->rate  ? b->rec->rate  : 16000;
            uint32_t bytesPerFrame = (bits / 8) * chans;
            uint32_t frames = bytesPerFrame ? sz / bytesPerFrame : 0;
            if (frames) render_into(buf, frames, rate, chans, bits, g_ctl.mix_mic != 0);
        }
    }
    if (b->gameCb) b->gameCb(caller, b->gameCtx);   // hand the game its (now-injected) buffer
}

static SLresult bq_RegisterCallback(SLAndroidSimpleBufferQueueItf self,
                                    slAndroidSimpleBufferQueueCallback cb, void* ctx) {
    BqCtx* b = CTX_OF(self, BqCtx, disp);
    b->gameCb = cb; b->gameCtx = ctx;
    return b->realDisp->RegisterCallback(self, bq_completed, b);
}
static SLresult bq_Enqueue(SLAndroidSimpleBufferQueueItf self, const void* buffer, SLuint32 size) {
    BqCtx* b = CTX_OF(self, BqCtx, disp);
    int nt = (b->tail + 1) % SB_QLEN;
    if (nt != b->head) { b->bufs[b->tail] = (void*)buffer; b->sizes[b->tail] = size; b->tail = nt; }
    return b->realDisp->Enqueue(self, buffer, size);
}

// ---- recorder-object GetInterface: wrap the buffer queue --------------------
static SLresult rec_GetInterface(SLObjectItf self, const SLInterfaceID iid, void* pInterface) {
    RecCtx* r = CTX_OF(self, RecCtx, disp);
    SLresult res = r->realDisp->GetInterface(self, iid, pInterface);
    if (res == SL_RESULT_SUCCESS && pInterface && iid_eq(iid, g_iid_bq)) {
        pthread_mutex_lock(&g_wrap_mtx);
        BqCtx* b = slot(g_bqs);
        if (b) {
            // h -> itf handle. *h = itf (const S_ *const *); **h = the dispatch ptr; ***h = the struct.
            SLAndroidSimpleBufferQueueItf* h = (SLAndroidSimpleBufferQueueItf*)pInterface;
            b->realDisp = **h;
            b->disp     = ***h;                // copy the full dispatch
            b->disp.RegisterCallback = bq_RegisterCallback;
            b->disp.Enqueue          = bq_Enqueue;
            b->rec = r; b->head = b->tail = 0; b->gameCb = nullptr; b->gameCtx = nullptr;
            *(const SLAndroidSimpleBufferQueueItf_**)(*h) = &b->disp;
            LOG("wrapped record buffer-queue (rate=%u ch=%u bits=%u)", r->rate, r->chans, r->bits);
        }
        pthread_mutex_unlock(&g_wrap_mtx);
    }
    return res;
}

// ---- engine CreateAudioRecorder: capture format, wrap the recorder object ---
static SLresult eng_CreateAudioRecorder(SLEngineItf self, SLObjectItf* pRecorder,
                                        SLDataSource* src, SLDataSink* sink,
                                        SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    EngCtx* e = CTX_OF(self, EngCtx, disp);
    SLresult res = e->realDisp->CreateAudioRecorder(self, pRecorder, src, sink, n, ids, req);
    if (res != SL_RESULT_SUCCESS || !pRecorder || !*pRecorder) return res;

    uint32_t rate = 16000, chans = 1, bits = 16;
    if (sink && sink->pFormat && *(SLuint32*)sink->pFormat == SL_DATAFORMAT_PCM) {
        SLDataFormat_PCM* pcm = (SLDataFormat_PCM*)sink->pFormat;
        rate  = pcm->samplesPerSec / 1000;      // milliHz -> Hz
        chans = pcm->numChannels;
        bits  = pcm->bitsPerSample;
    }
    pthread_mutex_lock(&g_wrap_mtx);
    RecCtx* r = slot(g_recs);
    if (r) {
        SLObjectItf h = *pRecorder;
        r->realDisp = *h;
        r->disp     = **h;
        r->disp.GetInterface = rec_GetInterface;
        r->rate = rate; r->chans = chans; r->bits = bits;
        *(const SLObjectItf_**)h = &r->disp;
        g_ctl.recorder_live = 1;
        g_ctl.rec_rate = rate; g_ctl.rec_chans = chans; g_ctl.rec_bits = bits;
        LOG("CreateAudioRecorder -> wrapped (rate=%u ch=%u bits=%u)", rate, chans, bits);
    }
    pthread_mutex_unlock(&g_wrap_mtx);
    return res;
}

// ---- engine-object GetInterface: wrap the engine interface ------------------
static SLresult engobj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void* pInterface) {
    EngObjCtx* eo = CTX_OF(self, EngObjCtx, disp);
    SLresult res = eo->realDisp->GetInterface(self, iid, pInterface);
    if (res == SL_RESULT_SUCCESS && pInterface && iid_eq(iid, g_iid_engine)) {
        pthread_mutex_lock(&g_wrap_mtx);
        EngCtx* e = slot(g_engs);
        if (e) {
            // h -> itf handle (one level deeper than an object handle, same as the bq wrap above).
            SLEngineItf* h = (SLEngineItf*)pInterface;
            e->realDisp = **h;
            e->disp     = ***h;
            e->disp.CreateAudioRecorder = eng_CreateAudioRecorder;
            *(const SLEngineItf_**)(*h) = &e->disp;
            LOG("wrapped SLEngineItf (CreateAudioRecorder hooked)");
        }
        pthread_mutex_unlock(&g_wrap_mtx);
    }
    return res;
}

// ============================================================================
//  GOT-hook entrypoint (compiled INTO libpavchams.so; NOT a DT_NEEDED replacement)
// ============================================================================
// libOpenSLES.so is a PUBLIC Android system library — the linker always resolves it from
// /system, so an APK-bundled copy of that name is ignored. Instead we live inside libpavchams (which
// is already injected) and let pavchams GOT-patch libUnreal's slCreateEngine slot to our
// sb_slCreateEngine below. The genuine SL functions come from the real system lib via dlopen.
typedef SLresult (*fn_slCreateEngine)(SLObjectItf*, SLuint32, const SLEngineOption*,
                                      SLuint32, const SLInterfaceID*, const SLboolean*);
static void*             g_real         = nullptr;   // system libOpenSLES.so handle
static fn_slCreateEngine r_createEngine = nullptr;   // genuine slCreateEngine

// Our replacement for slCreateEngine: forward to the real one, then wrap the returned engine object
// so CreateAudioRecorder -> recorder -> buffer-queue all cascade into our mic-inject path.
static SLresult sb_slCreateEngine(SLObjectItf* pEngine, SLuint32 numOptions,
                                  const SLEngineOption* pEngineOptions, SLuint32 numInterfaces,
                                  const SLInterfaceID* pInterfaceIds, const SLboolean* pInterfaceReq) {
    LOG("sb_slCreateEngine called (real=%p)", (void*)r_createEngine);
    if (!r_createEngine) return SL_RESULT_FEATURE_UNSUPPORTED;
    SLresult res = r_createEngine(pEngine, numOptions, pEngineOptions,
                                  numInterfaces, pInterfaceIds, pInterfaceReq);
    if (res == SL_RESULT_SUCCESS && pEngine && *pEngine) {
        pthread_mutex_lock(&g_wrap_mtx);
        EngObjCtx* eo = slot(g_engObjs);
        if (eo) {
            SLObjectItf h = *pEngine;
            eo->realDisp = *h;
            eo->disp     = **h;
            eo->disp.GetInterface = engobj_GetInterface;
            *(const SLObjectItf_**)h = &eo->disp;
        }
        pthread_mutex_unlock(&g_wrap_mtx);
    }
    return res;
}

// Resolve the genuine SL entrypoints + interface ids from the real system lib, and stage clips.
// Safe to call repeatedly (idempotent). Returns 1 once the real slCreateEngine is in hand.
SB_EXPORT int sb_install() {
    if (r_createEngine) return 1;
    if (!g_real) g_real = dlopen("libOpenSLES.so", RTLD_NOW | RTLD_GLOBAL);   // the PUBLIC system lib
    if (!g_real) return 0;
    r_createEngine = (fn_slCreateEngine)dlsym(g_real, "slCreateEngine");
    void* p;
    if ((p = dlsym(g_real, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))) g_iid_bq     = *(SLInterfaceID*)p;
    if ((p = dlsym(g_real, "SL_IID_ENGINE")))                   g_iid_engine = *(SLInterfaceID*)p;
    if ((p = dlsym(g_real, "SL_IID_OUTPUTMIX")))                g_iid_outmix = *(SLInterfaceID*)p;
    if ((p = dlsym(g_real, "SL_IID_PLAY")))                     g_iid_play   = *(SLInterfaceID*)p;
    g_ctl.play_req = -1; g_ctl.cur_clip = -1;
    sb_scan();
    LOG("sb_install: real=%p createEngine=%p iid_bq=%p iid_eng=%p clips=%d",
        g_real, (void*)r_createEngine, (void*)g_iid_bq, (void*)g_iid_engine, g_nclips);
    return r_createEngine ? 1 : 0;
}

// Address of our replacement, for pavchams to write into libUnreal's slCreateEngine GOT slot.
SB_EXPORT void* sb_hook_fn() { return (void*)&sb_slCreateEngine; }

// ============================================================================
//  local monitor player — hear the clip out your own headset while it transmits
// ============================================================================
// A dedicated OpenSL player (our own engine, separate from the game's) fed one big PCM buffer. Lets
// you set the soundboard volume solo without a second listener.
static SLObjectItf g_lpEng = nullptr; static SLEngineItf   g_lpEngItf = nullptr;
static SLObjectItf g_lpMix = nullptr; static SLObjectItf   g_lpPlayer = nullptr;
static SLPlayItf   g_lpPlay = nullptr; static SLAndroidSimpleBufferQueueItf g_lpBq = nullptr;
static int16_t*    g_lpBuf = nullptr;

SB_EXPORT void sb_local_stop() {
    if (g_lpPlay) (*g_lpPlay)->SetPlayState(g_lpPlay, SL_PLAYSTATE_STOPPED);
    if (g_lpBq)   (*g_lpBq)->Clear(g_lpBq);
    if (g_lpPlayer) { (*g_lpPlayer)->Destroy(g_lpPlayer); g_lpPlayer = nullptr; g_lpPlay = nullptr; g_lpBq = nullptr; }
    free(g_lpBuf); g_lpBuf = nullptr;
}

// Play `samples` of 48kHz mono s16 out the local speaker. Copies the buffer (caller keeps ownership).
SB_EXPORT int sb_local_play(const int16_t* pcm, int samples) {
    if (!pcm || samples <= 0) return 0;
    if (!r_createEngine) sb_install();
    if (!r_createEngine || !g_iid_engine || !g_iid_outmix || !g_iid_play || !g_iid_bq) return 0;
    sb_local_stop();

    if (!g_lpEng) {                         // one-time: our own engine + output mix
        if (r_createEngine(&g_lpEng, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) { g_lpEng=nullptr; return 0; }
        (*g_lpEng)->Realize(g_lpEng, SL_BOOLEAN_FALSE);
        if ((*g_lpEng)->GetInterface(g_lpEng, g_iid_engine, &g_lpEngItf) != SL_RESULT_SUCCESS) return 0;
        if ((*g_lpEngItf)->CreateOutputMix(g_lpEngItf, &g_lpMix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return 0;
        (*g_lpMix)->Realize(g_lpMix, SL_BOOLEAN_FALSE);
    }
    SLDataLocator_AndroidSimpleBufferQueue locbq = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1 };
    SLDataFormat_PCM fmt = { SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_48,
                             SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                             SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource src = { &locbq, &fmt };
    SLDataLocator_OutputMix locmix = { SL_DATALOCATOR_OUTPUTMIX, g_lpMix };
    SLDataSink sink = { &locmix, nullptr };
    const SLInterfaceID ids[1] = { g_iid_bq }; const SLboolean req[1] = { SL_BOOLEAN_TRUE };
    if ((*g_lpEngItf)->CreateAudioPlayer(g_lpEngItf, &g_lpPlayer, &src, &sink, 1, ids, req) != SL_RESULT_SUCCESS)
        { g_lpPlayer = nullptr; return 0; }
    (*g_lpPlayer)->Realize(g_lpPlayer, SL_BOOLEAN_FALSE);
    (*g_lpPlayer)->GetInterface(g_lpPlayer, g_iid_play, &g_lpPlay);
    (*g_lpPlayer)->GetInterface(g_lpPlayer, g_iid_bq,   &g_lpBq);

    g_lpBuf = (int16_t*)malloc((size_t)samples * 2);
    if (!g_lpBuf) return 0;
    memcpy(g_lpBuf, pcm, (size_t)samples * 2);
    (*g_lpBq)->Enqueue(g_lpBq, g_lpBuf, (SLuint32)samples * 2);
    (*g_lpPlay)->SetPlayState(g_lpPlay, SL_PLAYSTATE_PLAYING);
    return 1;
}
