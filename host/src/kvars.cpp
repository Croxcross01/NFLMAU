// Kernel variables the game imports by address (values follow Xenia's xboxkrnl_module.cc), the 1 ms
// KeTimeStampBundle clock, and the timebase frequency.
#include "kernel.h"
#include "platform.h"
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#ifdef _WIN32
#  include <intrin.h>
#else
#  include <x86intrin.h>
#endif

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

struct XexImportSlot { uint32_t slot; std::string lib; uint32_t ordinal; };
extern std::vector<XexImportSlot> g_xexImportSlots;

static uint32_t g_timeStampBundle = 0;
static uint64_t g_tscFrequency = 0;

// mftb is recompiled to rdtsc, so the frequency the game divides by must be the TSC frequency.
static uint64_t MeasureTsc() {
    auto t0 = std::chrono::steady_clock::now(); uint64_t c0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto t1 = std::chrono::steady_clock::now(); uint64_t c1 = __rdtsc();
    double s = std::chrono::duration<double>(t1 - t0).count();
    return (uint64_t)((c1 - c0) / s);
}
IMP(KeQueryPerformanceFrequency) { TRACE(KeQueryPerformanceFrequency); ctx.r3.u64 = g_tscFrequency; }

static uint32_t Zeroed(uint32_t n) { return GuestAlloc(n, 16); }

void KInitKernelVariables() {
    g_tscFrequency = MeasureTsc();
    // KeTimeStampBundle: InterruptTime(u64, 100 ns since boot), SystemTime(u64 FILETIME), TickCount(u32 ms), pad
    g_timeStampBundle = Zeroed(24);
    std::thread([] {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            auto up = std::chrono::steady_clock::now() - start;
            uint64_t ns100 = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(up).count() / 100;
            WriteU64(g_timeStampBundle + 0, ns100);
            WriteU64(g_timeStampBundle + 8, PlatFileTimeNow());
            WriteU32(g_timeStampBundle + 16, (uint32_t)(ns100 / 10000));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();

    struct Var { const char* lib; uint32_t ord; const char* name; uint32_t addr; };
    uint32_t hw = Zeroed(16); WriteU32(hw, 0x20); g_base[hw + 4] = 6;                    // flags, 6 hardware threads
    uint32_t ver = Zeroed(8); g_base[ver + 1] = 2; g_base[ver + 2] = g_base[ver + 3] = g_base[ver + 4] = g_base[ver + 5] = 0xFF; g_base[ver + 6] = 0x80;
    uint32_t clk = Zeroed(4); WriteU32(clk, 500);                                          // GPU clock 500 MHz
    uint32_t hsio = Zeroed(0x20); g_base[hsio] = 1; WriteU32(hsio + 0x10, 0xFFFFFFFF);     // initialised critical section
    uint32_t mod = Zeroed(0x100), pmod = Zeroed(4); WriteU32(pmod, mod);                  // fake loader entry
    WriteU32(mod + 0x18, PPC_IMAGE_BASE);
    Var vars[] = {
        {"xboxkrnl", 0x0AD, "KeTimeStampBundle", g_timeStampBundle},
        {"xboxkrnl", 0x156, "XboxHardwareInfo", hw},
        {"xboxkrnl", 0x158, "XboxKrnlVersion", ver},
        {"xboxkrnl", 0x1C0, "VdGpuClockInMHz", clk},
        {"xboxkrnl", 0x1C1, "VdHSIOCalibrationLock", hsio},
        {"xboxkrnl", 0x193, "XexExecutableModuleHandle", pmod},
        {"xboxkrnl", 0x059, "KeDebugMonitorData", Zeroed(4)},
        {"xboxkrnl", 0x266, "KeCertMonitorData", Zeroed(4)},
        {"xboxkrnl", 0x01B, "ExThreadObjectType", Zeroed(0x40)},
        {"xboxkrnl", 0x00E, "ExEventObjectType", Zeroed(0x40)},
        {"xboxkrnl", 0x1BE, "VdGlobalDevice", Zeroed(4)},
        {"xboxkrnl", 0x1BF, "VdGlobalXamDevice", Zeroed(4)},
    };
    int n = 0;
    for (auto& s : g_xexImportSlots)
        for (auto& v : vars)
            if (s.lib == v.lib && s.ordinal == v.ord) { WriteU32(s.slot, v.addr); n++; if (g_trace) fprintf(stderr, "[host] variable %s -> %08X (slot %08X)\n", v.name, v.addr, s.slot); }
    fprintf(stderr, "[host] %d kernel variables linked, timebase %.0f MHz\n", n, g_tscFrequency / 1e6);
}
