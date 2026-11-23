// Short waits that really are short. On Windows std::this_thread::sleep_for rounds up to the timer tick (1 ms at
// best, often 1-2 ms): a GPU or game thread that naps while waiting for the other side then reacts late, several times
// per frame. PreciseWait uses a high-resolution waitable timer (Windows 10 1803+), else a yield loop.
#include "precise_wait.h"
#include <chrono>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

void PreciseWait(int64_t microseconds) {
    if (microseconds <= 0) { std::this_thread::yield(); return; }
#ifdef _WIN32
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due; due.QuadPart = -microseconds * 10;   // relative, 100 ns units
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) { WaitForSingleObject(timer, INFINITE); return; }
    }
    // older Windows (no high-resolution timer): sleep whole milliseconds (1 ms timer resolution is on), then yield
    auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(microseconds);
    if (microseconds > 2000) Sleep((DWORD)((microseconds - 1500) / 1000));
    while (std::chrono::steady_clock::now() < end) std::this_thread::yield();
#else
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
#endif
}

// Sleeps until a point in time without the C++ library's timed sleep (on the setup's toolchain it rounded short
// waits to nothing - the thread spun at full speed - or woke early).
void PreciseSleepUntil(std::chrono::steady_clock::time_point t) {
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= t) return;
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(t - now).count();
        if (us <= 0) return;
        PreciseWait(us);
    }
}
