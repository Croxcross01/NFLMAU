// Audio output: the game's render-driver frames (256 samples x 6 channels, planar big-endian float at 48 kHz) are
// downmixed to stereo and played through WASAPI (Windows) or SDL2 (Linux). The device's consumption drives the
// game's audio clock, the way the console's audio hardware does. Downmix weights follow Xenia (BSD-3-Clause).
#include "audio_out.h"
#include "settings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#else
#include <SDL2/SDL.h>
#endif

namespace {
constexpr uint32_t kFrameSamples = 256, kRingFrames = 32;
float g_ring[kRingFrames][kFrameSamples * 2];
std::atomic<uint32_t> g_head{0}, g_tail{0};   // frames written / frames read
std::mutex g_pushLock;
AudioConsumedFn g_onConsumed = nullptr;
std::atomic<bool> g_active{false};
constexpr uint32_t kMaxBacklogFrames = 8, kTargetBacklogFrames = 3;   // 256-sample frames (5.3 ms each)
std::atomic<uint64_t> g_statSubmitted{0}, g_statSkipped{0}, g_statDropped{0}, g_statUnderrunSamples{0};
float g_volume = 1.0f;

// Pull `n` stereo sample pairs into out (float interleaved), padding with silence on underrun.
// Every whole frame consumed is reported so the game can render another one.
float g_partial[kFrameSamples * 2]; uint32_t g_partialPos = kFrameSamples;
void Pull(float* out, uint32_t n) {
    while (n) {
        if (g_partialPos >= kFrameSamples) {
            uint32_t t = g_tail.load(), h = g_head.load(std::memory_order_acquire);
            // keep latency bounded: if a backlog built up (the game produced a burst after a stall), skip ahead
            // so sound stays in step with the picture instead of trailing it for the rest of the session
            if (h - t > kMaxBacklogFrames) {
                g_statSkipped += (h - t) - kTargetBacklogFrames;
                t = h - kTargetBacklogFrames; g_tail.store(t, std::memory_order_release);
            }
            if (t == h) {   // underrun: silence, and still tick the clock so the game is never starved
                memset(out, 0, n * 2 * sizeof(float));
                g_statUnderrunSamples += n;
                static uint32_t silent = 0; silent += n;
                while (silent >= kFrameSamples) { silent -= kFrameSamples; if (g_onConsumed) g_onConsumed(); }
                return;
            }
            memcpy(g_partial, g_ring[t % kRingFrames], sizeof g_partial);
            g_tail.store(t + 1, std::memory_order_release);
            g_partialPos = 0;
            if (g_onConsumed) g_onConsumed();
        }
        uint32_t k = std::min(n, kFrameSamples - g_partialPos);
        for (uint32_t i = 0; i < k * 2; i++) out[i] = g_partial[g_partialPos * 2 + i] * g_volume;
        out += k * 2; n -= k; g_partialPos += k;
    }
}

#ifdef _WIN32
// explicit GUIDs (no dependency on __uuidof support)
const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

void WasapiThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr;
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    auto fail = [&](const char* what, HRESULT hr) { fprintf(stderr, "[audio] WASAPI %s failed (0x%08lX) - sound off\n", what, (unsigned long)hr); g_active = false; };
    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) return fail("device enumerator", hr);
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (FAILED(hr)) return fail("default output device", hr);
    hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr)) return fail("activate", hr);
    WAVEFORMATEX wf{}; wf.wFormatTag = 3 /*WAVE_FORMAT_IEEE_FLOAT*/; wf.nChannels = 2; wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 32; wf.nBlockAlign = 8; wf.nAvgBytesPerSec = 48000 * 8;
    // 20 ms shared-mode buffer; Windows converts to the device's rate/format if it differs
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                        200000, 0, &wf, nullptr);
    if (FAILED(hr)) return fail("initialize", hr);
    ac->SetEventHandle(ev);
    UINT32 bufFrames = 0; ac->GetBufferSize(&bufFrames);
    hr = ac->GetService(kIID_IAudioRenderClient, (void**)&rc);
    if (FAILED(hr)) return fail("render client", hr);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ac->Start();
    fprintf(stderr, "[audio] WASAPI output started (48 kHz stereo, %u-frame buffer)\n", bufFrames);
    for (;;) {
        WaitForSingleObject(ev, 100);
        UINT32 pad = 0; if (FAILED(ac->GetCurrentPadding(&pad))) continue;
        UINT32 room = bufFrames - pad;
        if (!room) continue;
        BYTE* p = nullptr;
        if (FAILED(rc->GetBuffer(room, &p))) continue;
        Pull((float*)p, room);
        rc->ReleaseBuffer(room, 0);
    }
}
#else
void SdlCallback(void*, Uint8* stream, int len) { Pull((float*)stream, uint32_t(len / 8)); }
#endif
}   // namespace

void AudioStatsThread();
bool AudioOutStart(AudioConsumedFn onConsumed) {
    g_onConsumed = onConsumed;
    g_volume = std::clamp(g_settings.volume, 0, 100) / 100.0f;
    if (!g_settings.audio) { fprintf(stderr, "[audio] sound disabled in settings\n"); return false; }
    g_active = true;
    std::thread(AudioStatsThread).detach();
#ifdef _WIN32
    std::thread(WasapiThread).detach();
    return true;
#else
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[audio] SDL audio init failed: %s - sound off\n", SDL_GetError()); g_active = false; return false; }
    SDL_AudioSpec want{}, have{}; want.freq = 48000; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 512; want.callback = SdlCallback;
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!d) { fprintf(stderr, "[audio] no audio device: %s - sound off\n", SDL_GetError()); g_active = false; return false; }
    SDL_PauseAudioDevice(d, 0);
    fprintf(stderr, "[audio] SDL output started (48 kHz stereo)\n");
    return true;
#endif
}
bool AudioOutActive() { return g_active; }
// every 10 s: how the game's audio kept up (log lines "[audio] stats ...")
void AudioStatsThread() {
    uint64_t lastSub = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        uint64_t sub = g_statSubmitted.load();
        fprintf(stderr, "[audio] stats: %.1f%% of real-time produced, queued %u, underrun %.0f ms, skipped %llu, dropped %llu\n",
                (sub - lastSub) * 256.0 / (48000.0 * 10.0) * 100.0, g_head.load() - g_tail.load(),
                g_statUnderrunSamples.exchange(0) / 48.0, (unsigned long long)g_statSkipped.exchange(0), (unsigned long long)g_statDropped.exchange(0));
        lastSub = sub;
    }
}

// samples: guest memory, 6 planar channels x 256 big-endian floats
void AudioOutSubmit(const uint8_t* samples) {
    if (!g_active) return;
    auto be = [&](uint32_t ch, uint32_t i) {
        uint32_t v; memcpy(&v, samples + (ch * kFrameSamples + i) * 4, 4);
        v = __builtin_bswap32(v); float f; memcpy(&f, &v, 4); return f;
    };
    std::lock_guard<std::mutex> l(g_pushLock);
    uint32_t h = g_head.load(), t = g_tail.load(std::memory_order_acquire);
    g_statSubmitted++;
    if (h - t >= kRingFrames) { g_statDropped++; return; }   // device fell behind: drop this frame rather than add latency
    float* o = g_ring[h % kRingFrames];
    for (uint32_t i = 0; i < kFrameSamples; i++) {
        float c = be(2, i) * 0.5f;   // centre to both sides, LFE dropped
        float L = (be(0, i) + be(4, i) + c) * 0.4f, R = (be(1, i) + be(5, i) + c) * 0.4f;
        o[i * 2] = std::clamp(L, -1.0f, 1.0f); o[i * 2 + 1] = std::clamp(R, -1.0f, 1.0f);
    }
    g_head.store(h + 1, std::memory_order_release);
    // MADDEN_AUDIO_DUMP=path: also write the stereo mix as raw 48 kHz float (debugging)
    static FILE* dump = getenv("MADDEN_AUDIO_DUMP") ? fopen(getenv("MADDEN_AUDIO_DUMP"), "wb") : nullptr;
    if (dump) { fwrite(o, sizeof(float), kFrameSamples * 2, dump); fflush(dump); }
}
