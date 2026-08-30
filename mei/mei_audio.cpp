// mei_audio.cpp — OpenSL ES SFX for the menu. Each sound is a preloaded PCM buffer with its own
// buffer-queue player. play() clears + re-enqueues that player's buffer -> instant retrigger, no alloc.
// Everything degrades to a silent no-op if OpenSL, the engine, or a .wav is unavailable.
#include "mei_audio.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "MEI-AUDIO", __VA_ARGS__)

static const char* SND_FILE[MSND_COUNT] = {
    "/sdcard/Android/data/com.vankrupt.pavlov/files/ui_Select.wav",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/ui_Submenu_Select.wav",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/ui_Submenu_Deselect.wav",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/ui_Window_SlideOpen.wav",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/ui_Window_SlideClose.wav",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/ui_ScrollBar_Grab.wav",
    "/sdcard/Android/data/com.vankrupt.pavlov/files/ui_ScrollBar_Ungrab.wav",
};

struct Voice {
    SLObjectItf                    obj  = nullptr;
    SLPlayItf                      play = nullptr;
    SLAndroidSimpleBufferQueueItf  bq   = nullptr;
    uint8_t* pcm = nullptr; uint32_t pcmLen = 0;   // 16-bit PCM payload
};

static SLObjectItf   g_engineObj = nullptr;
static SLEngineItf   g_engine    = nullptr;
static SLObjectItf   g_mixObj    = nullptr;
static Voice         g_voice[MSND_COUNT];
static bool          g_ready = false;

// --- minimal WAV loader: 16-bit PCM only (matches Pavlov's UI assets) --------------------------
static bool load_wav(const char* path, uint8_t** outPcm, uint32_t* outLen,
                     uint32_t* rate, uint16_t* chans, uint16_t* bits) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 44 || sz > 8*1024*1024) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f); fclose(f);
    if (rd != (size_t)sz) { free(buf); return false; }
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return false; }
    uint32_t off = 12; bool haveFmt = false; uint8_t* data = nullptr; uint32_t dataLen = 0;
    while (off + 8 <= (uint32_t)sz) {
        uint32_t cksz; memcpy(&cksz, buf + off + 4, 4);
        const uint8_t* id = buf + off;
        if (!memcmp(id, "fmt ", 4) && off + 8 + 16 <= (uint32_t)sz) {
            uint16_t fmt; memcpy(&fmt, buf + off + 8, 2);
            memcpy(chans, buf + off + 10, 2);
            memcpy(rate,  buf + off + 12, 4);
            memcpy(bits,  buf + off + 22, 2);
            haveFmt = (fmt == 1);   // PCM
        } else if (!memcmp(id, "data", 4)) {
            data = buf + off + 8; dataLen = cksz;
            if (off + 8 + dataLen > (uint32_t)sz) dataLen = (uint32_t)sz - (off + 8);
        }
        off += 8 + cksz + (cksz & 1);   // chunks are word-aligned
    }
    if (!haveFmt || !data || !dataLen || *bits != 16) { free(buf); return false; }
    uint8_t* pcm = (uint8_t*)malloc(dataLen);
    if (!pcm) { free(buf); return false; }
    memcpy(pcm, data, dataLen);
    free(buf);
    *outPcm = pcm; *outLen = dataLen;
    return true;
}

static bool make_voice(Voice& v, uint32_t rate, uint16_t chans) {
    SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1 };
    SLDataFormat_PCM pcm;
    pcm.formatType    = SL_DATAFORMAT_PCM;
    pcm.numChannels   = chans;
    pcm.samplesPerSec = rate * 1000;   // OpenSL wants milliHertz
    pcm.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
    pcm.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
    pcm.channelMask   = (chans == 2) ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT) : SL_SPEAKER_FRONT_CENTER;
    pcm.endianness    = SL_BYTEORDER_LITTLEENDIAN;
    SLDataSource src = { &loc, &pcm };
    SLDataLocator_OutputMix outmix = { SL_DATALOCATOR_OUTPUTMIX, g_mixObj };
    SLDataSink sink = { &outmix, nullptr };
    const SLInterfaceID ids[1] = { SL_IID_BUFFERQUEUE };
    const SLboolean     req[1] = { SL_BOOLEAN_TRUE };
    if ((*g_engine)->CreateAudioPlayer(g_engine, &v.obj, &src, &sink, 1, ids, req) != SL_RESULT_SUCCESS) return false;
    if ((*v.obj)->Realize(v.obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return false;
    if ((*v.obj)->GetInterface(v.obj, SL_IID_PLAY, &v.play) != SL_RESULT_SUCCESS) return false;
    if ((*v.obj)->GetInterface(v.obj, SL_IID_BUFFERQUEUE, &v.bq) != SL_RESULT_SUCCESS) return false;
    (*v.play)->SetPlayState(v.play, SL_PLAYSTATE_PLAYING);
    return true;
}

void mei_audio_init() {
    if (g_ready) return;
    if (slCreateEngine(&g_engineObj, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) { ALOG("no engine"); return; }
    (*g_engineObj)->Realize(g_engineObj, SL_BOOLEAN_FALSE);
    (*g_engineObj)->GetInterface(g_engineObj, SL_IID_ENGINE, &g_engine);
    if ((*g_engine)->CreateOutputMix(g_engine, &g_mixObj, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) { ALOG("no mix"); return; }
    (*g_mixObj)->Realize(g_mixObj, SL_BOOLEAN_FALSE);
    int loaded = 0;
    for (int i = 0; i < MSND_COUNT; i++) {
        uint32_t rate = 0, len = 0; uint16_t ch = 0, bits = 0;
        if (!load_wav(SND_FILE[i], &g_voice[i].pcm, &len, &rate, &ch, &bits)) { ALOG("wav miss: %s", SND_FILE[i]); continue; }
        g_voice[i].pcmLen = len;
        if (!make_voice(g_voice[i], rate, ch)) { ALOG("voice fail %d", i); free(g_voice[i].pcm); g_voice[i].pcm = nullptr; continue; }
        loaded++;
    }
    g_ready = true;
    ALOG("audio up: %d/%d sounds", loaded, MSND_COUNT);
}

void mei_audio_play(int snd) {
    if (!g_ready || snd < 0 || snd >= MSND_COUNT) return;
    Voice& v = g_voice[snd];
    if (!v.bq || !v.pcm || !v.pcmLen) return;
    (*v.bq)->Clear(v.bq);                        // stop any in-flight copy -> instant retrigger
    (*v.bq)->Enqueue(v.bq, v.pcm, v.pcmLen);     // player is already in PLAYING state
}

void mei_audio_shutdown() {
    for (int i = 0; i < MSND_COUNT; i++) {
        if (g_voice[i].obj) { (*g_voice[i].obj)->Destroy(g_voice[i].obj); g_voice[i].obj = nullptr; }
        if (g_voice[i].pcm) { free(g_voice[i].pcm); g_voice[i].pcm = nullptr; }
    }
    if (g_mixObj)    { (*g_mixObj)->Destroy(g_mixObj); g_mixObj = nullptr; }
    if (g_engineObj) { (*g_engineObj)->Destroy(g_engineObj); g_engineObj = nullptr; }
    g_engine = nullptr; g_ready = false;
}
