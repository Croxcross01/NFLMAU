// Audio: XAudio render-driver clients (paced like real playback; output to speakers comes later) and XMA
// decoder contexts (allocated, not yet decoded). Semantics follow Xenia's AudioSystem (BSD-3-Clause).
#include "kernel.h"
void GameTimingThread();   // perf_tune.cpp
#include "audio_out.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <string>
void PerfName(const std::string& name);
void PerfStart();

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

namespace {
constexpr int kMaxClients = 8, kMaxQueuedFrames = 64;
constexpr auto kFrameTime = std::chrono::nanoseconds(5333333);   // 256 samples at 48 kHz
struct Client { std::atomic<bool> used{false}; uint32_t callback = 0, wrappedArg = 0; std::atomic<int> credits{0}; std::atomic<uint64_t> frames{0}; };
Client g_clients[kMaxClients];
std::mutex g_mu; std::condition_variable g_cv;
std::atomic<bool> g_started{false};

struct GuestCaller {
    PPCContext ctx{}; bool ready = false;
    void Call(uint32_t fn, uint32_t a0) {
        if (!ready) { KInitHostGuestThread(ctx, 0x40000); ready = true; }
        uint32_t sp = ctx.r1.u32; ctx.r3.u64 = a0;
        (PPC_LOOKUP_FUNC(g_base, fn))(ctx, g_base);
        ctx.r1.u64 = sp;
    }
};

// Worker: like the console's audio driver thread, call a client's render callback whenever it has room to queue
// another frame.
void AudioWorker() {
    { PerfName("audio"); }
    GameTimingThread();
    GuestCaller caller;
    for (;;) {
        int idx = -1;
        {
            std::unique_lock<std::mutex> l(g_mu);
            g_cv.wait_for(l, std::chrono::milliseconds(20), [&] {
                for (int i = 0; i < kMaxClients; i++) if (g_clients[i].used && g_clients[i].credits > 0) { idx = i; return true; }
                return false;
            });
            if (idx < 0) continue;
            g_clients[idx].credits--;
        }
        caller.Call(g_clients[idx].callback, g_clients[idx].wrappedArg);
    }
}
// "Playback": one queued frame finishes every 5.33 ms per client.
// With a real output device, the device's consumption is the clock (OnDeviceConsumed); the timer only runs
// when there is no device. Queue depth with a device is kept short so sound stays in sync with the picture.
constexpr int kDeviceQueuedFrames = 3;
void OnDeviceConsumed() {
    bool any = false;
    for (auto& c : g_clients) if (c.used && c.credits < kDeviceQueuedFrames) { c.credits++; any = true; }
    if (any) g_cv.notify_all();
}
void PlaybackClock() {
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += kFrameTime;
        std::this_thread::sleep_until(next);
        if (AudioOutActive()) { next = std::chrono::steady_clock::now(); continue; }
        bool any = false;
        for (auto& c : g_clients) if (c.used && c.credits < kMaxQueuedFrames) { c.credits++; any = true; }
        if (any) g_cv.notify_all();
    }
}
}   // namespace

// XAudioRegisterRenderDriverClient(callback_ptr -> {callback, arg}, driver_ptr*)
IMP(XAudioRegisterRenderDriverClient) {
    TRACE(XAudioRegisterRenderDriverClient);
    uint32_t cb = ReadU32(ctx.r3.u32), arg = ReadU32(ctx.r3.u32 + 4);
    int idx = -1;
    for (int i = 0; i < kMaxClients; i++) { bool f = false; if (g_clients[i].used.compare_exchange_strong(f, true)) { idx = i; break; } }
    if (idx < 0) { ctx.r3.u64 = 0x8007000E; return; }
    auto& c = g_clients[idx];
    if (!g_started.load()) AudioOutStart(OnDeviceConsumed);
    c.callback = cb; c.wrappedArg = GuestAlloc(4); WriteU32(c.wrappedArg, arg); c.credits = AudioOutActive() ? kDeviceQueuedFrames : kMaxQueuedFrames;
    WriteU32(ctx.r4.u32, 0x41550000 | idx);
    fprintf(stderr, "[audio] render client %d registered (callback %08X)\n", idx, cb);
    if (!g_started.exchange(true)) { std::thread(AudioWorker).detach(); std::thread(PlaybackClock).detach(); }
    g_cv.notify_all();
    ctx.r3.u64 = 0;
}
IMP(XAudioUnregisterRenderDriverClient) {
    TRACE(XAudioUnregisterRenderDriverClient);
    uint32_t i = ctx.r3.u32 & 0xFFFF; if (i < kMaxClients) g_clients[i].used = false;
    ctx.r3.u64 = 0;
}
// XAudioSubmitRenderDriverFrame(driver, samples*): 256 samples x 6 channels of big-endian float
IMP(XAudioSubmitRenderDriverFrame) {
    TRACE(XAudioSubmitRenderDriverFrame);
    uint32_t i = ctx.r3.u32 & 0xFFFF;
    if (i < kMaxClients) {
        uint64_t n = ++g_clients[i].frames;
        if (n == 1) fprintf(stderr, "[audio] first audio frame submitted by client %u\n", i);
        if (i == 0 && ctx.r4.u32) AudioOutSubmit(g_base + ctx.r4.u32);   // client 0 is the game's mixer
    }
    ctx.r3.u64 = 0;
}
IMP(XAudioGetVoiceCategoryVolume) { TRACE(XAudioGetVoiceCategoryVolume); float one = 1.0f; uint32_t b; memcpy(&b, &one, 4); if (ctx.r4.u32) WriteU32(ctx.r4.u32, b); ctx.r3.u64 = 0; }

// ---- XMA contexts: 64-byte hardware context records in physical memory
IMP(XMACreateContext) {
    TRACE(XMACreateContext);
    uint32_t c = GuestAllocPhys(64, 256);
    memset(g_base + c, 0, 64);
    if (ctx.r3.u32) WriteU32(ctx.r3.u32, c);
    ctx.r3.u64 = 0;
}
IMP(XMAReleaseContext) { TRACE(XMAReleaseContext); ctx.r3.u64 = 0; }
