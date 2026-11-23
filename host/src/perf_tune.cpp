// Smooth play on any PC: things that depend on the player's display, CPU and Windows settings rather than the game.
//  - Display refresh: the game runs at 60 frames a second (30 in some scenes). On a 60/120/180/240 Hz display every
//    frame is shown for the same number of refreshes. On 75, 144 or 165 Hz they can't be (60 doesn't divide them),
//    so frames are shown for 2 then 3 refreshes in turn - that is the "choppy" look. If the display also has a
//    60-multiple mode at the same resolution (nearly every 144/165 Hz screen offers 120 Hz), it is switched to that
//    while the game runs; Windows puts the old rate back when the game closes. settings.ini match_refresh=0 turns
//    this off (also off with variable refresh, G-Sync/FreeSync, where it isn't needed - players can switch it off).
//  - Power throttling (Windows 11, and laptops / Intel CPUs with efficiency cores): Windows may run a program's
//    threads slowly on efficiency cores. The game asks not to be throttled and to keep 1 ms timers.
//  - Priority: slightly above normal, so background programs don't take turns from the game's threads.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static bool SettingOn(const char* key, bool def) {   // settings.ini "key=0/1"
    FILE* f = fopen("settings.ini", "r"); if (!f) return def;
    char line[256]; bool v = def; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == '=') v = atoi(line + kl + 1) != 0;
    fclose(f);
    return v;
}

static void MatchRefresh() {
    DEVMODEW cur{}; cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) return;
    int hz = (int)cur.dmDisplayFrequency;
    auto fits = [](int h) { if (h >= 58 && h <= 62) return true; int k = (h + 30) / 60; return k >= 2 && std::abs(h - 60 * k) <= 2 * k; };
    if (hz <= 1 || fits(hz)) { fprintf(stderr, "[perf] display %d Hz - fits 60 fps\n", hz); return; }
    if (!SettingOn("match_refresh", true)) { fprintf(stderr, "[perf] display %d Hz - doesn't fit 60 fps, refresh matching is off in settings\n", hz); return; }
    DEVMODEW best{}; int bestHz = 0;
    for (DWORD i = 0;; i++) {
        DEVMODEW m{}; m.dmSize = sizeof m;
        if (!EnumDisplaySettingsW(nullptr, i, &m)) break;
        int h = (int)m.dmDisplayFrequency;
        if (m.dmPelsWidth != cur.dmPelsWidth || m.dmPelsHeight != cur.dmPelsHeight || m.dmBitsPerPel != cur.dmBitsPerPel || !fits(h)) continue;
        if (h <= hz + 2 && h > bestHz) { best = m; bestHz = h; }   // the highest 60-multiple not above the current rate
    }
    if (!bestHz) { fprintf(stderr, "[perf] display %d Hz - doesn't fit 60 fps and has no 60/120 Hz mode at this resolution\n", hz); return; }
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsExW(nullptr, &best, nullptr, CDS_FULLSCREEN, nullptr);   // temporary: undone when the game exits
    fprintf(stderr, "[perf] display %d Hz doesn't fit 60 fps - %s %d Hz while the game runs\n", hz, r == DISP_CHANGE_SUCCESSFUL ? "switched to" : "couldn't switch to", bestHz);
}

static void NoThrottling() {
    struct PowerThrottling { ULONG Version, ControlMask, StateMask; } t{1, 0x1 | 0x4, 0};   // execution speed + timer resolution: not throttled
    typedef BOOL(WINAPI * SetInfo)(HANDLE, int, LPVOID, DWORD);
    if (auto f = (SetInfo)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"))
        f(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &t, sizeof t);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
}

void TuneForSmoothness() {
    NoThrottling();
    MatchRefresh();
    SYSTEM_INFO si; GetSystemInfo(&si);
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    fprintf(stderr, "[perf] %u CPU threads, %llu GB memory\n", (unsigned)si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 30));
}
// The threads that keep time (vblank, presenting): Windows' "Games" scheduling class, so they wake on time even
// when the CPU is busy.
void GameTimingThread() {
    typedef HANDLE(WINAPI * AvSet)(LPCWSTR, LPDWORD);
    static AvSet f = [] { HMODULE m = LoadLibraryW(L"avrt.dll"); return m ? (AvSet)(void*)GetProcAddress(m, "AvSetMmThreadCharacteristicsW") : nullptr; }();
    DWORD idx = 0;
    if (f) f(L"Games", &idx);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}
#else
void TuneForSmoothness() {}
void GameTimingThread() {}
#endif
