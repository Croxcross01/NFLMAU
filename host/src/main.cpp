#include <chrono>
#include <atomic>
// Host for the recompiled Madden NFL Arcade (Xbox 360 XBLA) Arcadegame.dll.
// Usage: madden_host [--game <folder with your extracted game files>] [--image <Arcadegame.img>] [--trace]
#include "host.h"
#include "platform.h"
#include <cstdlib>
#include <mutex>
#include <string>
#include <csignal>
#include <cctype>
#ifndef _WIN32
#  include <execinfo.h>
#  include <ucontext.h>
#endif

void KInitMainThread(PPCContext& ctx);
void KInitKernelVariables();
void PresenterStart();
#include "settings.h"
#include <string>
void PerfName(const std::string& name);
void PerfStart();
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
#endif
std::string LoadXexImage(const std::string& path, uint8_t* dst, size_t maxSize, uint32_t& baseOut, size_t& sizeOut);
static std::mutex g_allocLock;

uint8_t* g_base = nullptr;
bool g_trace = false;
PPC_EXTERN_FUNC(_xstart);
PPC_EXTERN_FUNC(sub_888CC600);   // Arcadegame.dll export ordinal 1 (game main)

// Bump allocators. Virtual heap: 0x40000000-0x7F000000 (where the console puts user allocations; the GPU
// registers live at 0x7FC80000). Physical heap: offsets into the 512 MB physical memory, handed out through the
// 0xC0000000 window (all three windows alias, see platform.h). Memory starts zeroed.
static uint32_t g_heapNext = 0x40000000, g_physNext = 0x00100000;
uint32_t GuestAlloc(size_t size, size_t align) {
    std::lock_guard<std::mutex> l(g_allocLock);
    g_heapNext = (g_heapNext + align - 1) & ~(uint32_t)(align - 1);
    uint32_t r = g_heapNext; g_heapNext += (uint32_t)((size + 15) & ~15ull);
    if (g_heapNext >= 0x7F000000) { fprintf(stderr, "guest heap exhausted\n"); abort(); }
    return r;
}
uint32_t GuestAllocPhys(size_t size, size_t align) {
    std::lock_guard<std::mutex> l(g_allocLock);
    if (align < 4096) align = 4096;
    g_physNext = (g_physNext + align - 1) & ~(uint32_t)(align - 1);
    uint32_t r = g_physNext; g_physNext += (uint32_t)((size + 4095) & ~4095ull);
    if (g_physNext >= 0x1FF00000) { fprintf(stderr, "guest physical memory exhausted (512 MB)\n"); abort(); }
    return 0xC0000000u + r;
}
uint32_t GuestPhysicalAddress(uint32_t a) {
    if (a >= 0xA0000000u) return (a - 0xA0000000u) & 0x1FFFFFFF;   // any of the three windows
    return a & 0x1FFFFFFF;
}

static PPCContext* g_ctx = nullptr;
void DumpGuestThreads();

// Calls through an empty (zero) function pointer. The game's loading-screen UI (Apt display list update on a job
// thread) sometimes walks an object that was already freed and reused, reads a zero virtual-function entry and calls
// it: the indirect-call lookup for guest address 0 lands outside the function table and the host crashed (seen on
// the 2nd/3rd match load and when returning to the menus, also before the lineup picker existed). The table slots
// for guest addresses 0..0x7FFF are mapped to this stub, which does nothing and returns 0, so such a call is skipped.
static void NullGuestCall(PPCContext& ctx, uint8_t*) {
    static std::atomic<int> n{0};
    int k = n++;
    if (k < 20 || (k % 1000) == 0)
        fprintf(stderr, "[host] skipped a call to an empty function pointer (guest lr %08X, r3 %08X, #%d)\n",
                (uint32_t)ctx.lr, ctx.r3.u32, k + 1);
    ctx.r3.u64 = 0;
    // A worker thread loop (sub_88E049A0) asks its task object "any work?" (vtable +96) and then "how long to wait?"
    // (vtable +100) before waiting on its event. With no task object (pointer -1) both calls land here; answering 0 to
    // the second made it wait 0 ms and spin flat out (tens of thousands of loops late in a session). Answer INFINITE:
    // the thread sleeps until its event is signalled, as with a real idle task.
    if ((uint32_t)ctx.lr == 0x88E049F8u) ctx.r3.u64 = 0xFFFFFFFFu;
}
// Wider guard: a call through the function table with a garbage target (not just 0) faults when it reads the table
// entry. A vectored handler recognises the faulting host instruction - an indirect "call qword ptr [mem]"
// (FF /2) - skips it (as if the guest function returned 0) and lets the game continue.
#ifdef _WIN32
static int CallInsnLength(const uint8_t* p) {   // FF /2 with a memory operand, optional REX prefix; 0 if not one
    int n = 0;
    if ((p[0] & 0xF0) == 0x40) n++;
    if (p[n] != 0xFF) return 0;
    uint8_t modrm = p[n + 1];
    int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (reg != 2 || mod == 3) return 0;
    n += 2;
    if (rm == 4) {   // SIB
        uint8_t sib = p[n++];
        if (mod == 0 && (sib & 7) == 5) n += 4;
    } else if (mod == 0 && rm == 5) return 0;   // rip-relative: not a table lookup
    if (mod == 1) n += 1; else if (mod == 2) n += 4;
    return n;
}
static LONG WINAPI SkipBadGuestCall(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* mod = (uint8_t*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(mod + ((IMAGE_DOS_HEADER*)mod)->e_lfanew);
    uint8_t* ip = (uint8_t*)ep->ContextRecord->Rip;
    if (ip < mod || ip + 16 > mod + nt->OptionalHeader.SizeOfImage) return EXCEPTION_CONTINUE_SEARCH;
    int len = CallInsnLength(ip);
    if (!len) return EXCEPTION_CONTINUE_SEARCH;
    // recompiled code calls guest functions as f(PPCContext& ctx, uint8_t* base): rcx = ctx, rdx = base
    if ((uint8_t*)ep->ContextRecord->Rdx != g_base) return EXCEPTION_CONTINUE_SEARCH;
    PPCContext* ctx = (PPCContext*)ep->ContextRecord->Rcx;
    static std::atomic<int> n{0};
    int k = n++;
    if (k < 20 || (k % 1000) == 0)
        fprintf(stderr, "[host] skipped a call to a bad function address (guest lr %08X, ctr %08X, #%d)\n",
                (uint32_t)ctx->lr, ctx->ctr.u32, k + 1);
    ctx->r3.u64 = 0;
    ep->ContextRecord->Rip += len;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif
static void MapNullCallSlots() {
#ifdef _WIN32
    // the compiler folds the lookup into base + IMAGE_BASE + IMAGE_SIZE + 2 * (target - CODE_BASE), done in 64 bits;
    // also cover the 32-bit-wrapped form
    intptr_t folded = (intptr_t)g_base + (intptr_t)PPC_IMAGE_BASE + (intptr_t)PPC_IMAGE_SIZE - 2 * (intptr_t)PPC_CODE_BASE;
    intptr_t wrapped = (intptr_t)g_base + (intptr_t)PPC_IMAGE_BASE + (intptr_t)PPC_IMAGE_SIZE + 2 * (intptr_t)(uint32_t)(0u - (uint32_t)PPC_CODE_BASE);
    for (intptr_t at : {folded, wrapped}) {
        const size_t size = 0x10000;   // targets 0 .. 0x7FFF
        void* m = VirtualAlloc((void*)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (m != (void*)at) { fprintf(stderr, "[host] empty-call guard: could not map %p\n", (void*)at); if (m) VirtualFree(m, 0, MEM_RELEASE); continue; }
        PPCFunc** slots = (PPCFunc**)m;
        for (size_t i = 0; i < size / sizeof(PPCFunc*); i++) slots[i] = NullGuestCall;
        DWORD old; VirtualProtect(m, size, PAGE_READONLY, &old);
        fprintf(stderr, "[host] empty-call guard mapped at %p\n", m);
    }
#endif
}
PPCContext* CurrentGuestContext();
static void ReportGuestState() {
    if (const char* d = getenv("MADDEN_DUMP")) {   // MADDEN_DUMP=addr:count  -> dump guest words at crash
        unsigned a = 0, c = 16; sscanf(d, "%x:%u", &a, &c);
        for (unsigned i = 0; i < c; i++) fprintf(stderr, "%s%08X=%08X", i % 4 ? "  " : "\n  ", a + i * 4, ReadU32(a + i * 4));
        fprintf(stderr, "\n");
    }
    // the crashing thread's own guest state: all registers, then the guest call chain from the stack back chain
    // (Xbox 360 ABI: back chain at 0(r1), the caller's LR saved at -8 of the previous frame)
    if (PPCContext* c = CurrentGuestContext()) {
        fprintf(stderr, "guest regs (crashing thread): lr=%08X ctr=%08X\n", (uint32_t)c->lr, c->ctr.u32);
        const PPCRegister* r[32] = {&c->r0, &c->r1, &c->r2, &c->r3, &c->r4, &c->r5, &c->r6, &c->r7, &c->r8, &c->r9, &c->r10,
            &c->r11, &c->r12, &c->r13, &c->r14, &c->r15, &c->r16, &c->r17, &c->r18, &c->r19, &c->r20, &c->r21, &c->r22,
            &c->r23, &c->r24, &c->r25, &c->r26, &c->r27, &c->r28, &c->r29, &c->r30, &c->r31};
        for (int i = 0; i < 32; i++) fprintf(stderr, "%sr%-2d=%016llX", i % 4 ? "  " : "\n  ", i, (unsigned long long)r[i]->u64);
        // memory behind registers that hold guest addresses (with any readable names), e.g. which asset was missing
        auto gp = [](uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; };
        auto text = [&](uint32_t a) -> std::string {
            if (!gp(a)) return {};
            std::string t;
            for (int k = 0; k < 96; k++) { uint8_t ch = g_base[a + k]; if (!ch) break; if (ch < 32 || ch > 126) return {}; t += char(ch); }
            return t.size() >= 3 ? t : std::string();
        };
        fprintf(stderr, "\n  memory at register addresses:");
        for (int i = 3; i < 32; i++) {
            uint32_t a = r[i]->u32;
            if (i == 13 || !gp(a) || r[i]->u64 >> 32) continue;
            fprintf(stderr, "\n   r%-2d %08X:", i, a);
            for (int k = 0; k < 8; k++) fprintf(stderr, " %08X", ReadU32(a + k * 4));
            std::string t = text(a);
            if (!t.empty()) fprintf(stderr, "  \"%s\"", t.c_str());
            for (int k = 0; k < 8; k++) {
                uint32_t p = ReadU32(a + k * 4);
                std::string t1 = text(p);
                if (!t1.empty()) fprintf(stderr, "\n        +%02X -> \"%s\"", k * 4, t1.c_str());
                else if (gp(p)) { std::string t2 = text(ReadU32(p)); if (!t2.empty()) fprintf(stderr, "\n        +%02X -> * -> \"%s\"", k * 4, t2.c_str()); }
            }
        }
        fprintf(stderr, "\n  guest call chain:");
        uint32_t sp = c->r1.u32;
        for (int i = 0; i < 16 && sp >= 0x10000 && sp < 0xE0000000u; i++) {
            uint32_t next = ReadU32(sp);
            if (next <= sp || next - sp > 0x100000) break;
            fprintf(stderr, " %08X", ReadU32(next - 8));
            sp = next;
        }
        fprintf(stderr, "\n");
    }
    if (g_ctx) fprintf(stderr, "guest regs (main thread): ctr=%08X lr=%08llX r1=%08X r3=%08X r4=%08X r5=%08X r30=%08X r31=%08X\n",
        g_ctx->ctr.u32, (unsigned long long)g_ctx->lr, g_ctx->r1.u32, g_ctx->r3.u32, g_ctx->r4.u32, g_ctx->r5.u32, g_ctx->r30.u32, g_ctx->r31.u32);
}
#ifdef _WIN32
static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
    fprintf(stderr, "\n*** exception %08lX at %p, data address %p (guest 0x%llX)\n", ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress, a, (unsigned long long)(a - g_base));
    // Module-relative addresses (look them up in madden_host.map): the faulting instruction, then likely return
    // addresses found on the stack.
    uint8_t* mod = (uint8_t*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(mod + ((IMAGE_DOS_HEADER*)mod)->e_lfanew);
    uintptr_t lo = (uintptr_t)mod, hi = lo + nt->OptionalHeader.SizeOfImage;
    uintptr_t pref = (uintptr_t)nt->OptionalHeader.ImageBase;
    fprintf(stderr, "*** at image+0x%llX (preferred address 0x%llX)\n", (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - lo),
            (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - lo + pref));
    uintptr_t* sp = (uintptr_t*)ep->ContextRecord->Rsp;
    int shown = 0;
    uintptr_t* top = (uintptr_t*)((NT_TIB*)NtCurrentTeb())->StackBase;
    for (int i = 0; i < 2048 && shown < 16 && sp + i < top; i++) {
        uintptr_t v = sp[i];
        if (v > lo && v < hi) { fprintf(stderr, "***   stack: 0x%llX\n", (unsigned long long)(v - lo + pref)); shown++; }
    }
    ReportGuestState();
    DumpGuestThreads();   // what every other game thread was doing at the time
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static void OnCrash(int sig, siginfo_t* si, void* uc) {
    uint8_t* a = (uint8_t*)si->si_addr;
    fprintf(stderr, "\n*** signal %d at host %p (guest addr %s0x%llX)\n", sig, a,
        (a >= g_base && a < g_base + PPC_MEMORY_SIZE) ? "" : "NOT-IN-GUEST ", (unsigned long long)(a - g_base));
    ReportGuestState();
    void* bt[40]; int n = backtrace(bt, 40); backtrace_symbols_fd(bt, n, 2);
    _exit(139);
}
#endif

static void SetEnvDefault(const char* k, const std::string& v) {
#ifdef _WIN32
    if (!getenv(k)) _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 0);
#endif
}

std::chrono::steady_clock::time_point g_hostStart = std::chrono::steady_clock::now();
int main(int argc, char** argv) {
    // unbuffered output: when the launcher captures our output through a pipe, Windows buffers it, and a crash
    // would otherwise lose everything that was printed
    setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    // 1 ms timer resolution: Windows otherwise rounds every short sleep/wait up to ~15.6 ms, which stretched the
    // game's own short waits (audio thread, vblank pacing, kernel timers) and made sound fall behind
    timeBeginPeriod(1);
    { PerfName("game main"); PerfStart(); }
#endif
    std::string gameDir = "game", imgPath, settingsPath = "settings.ini";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) gameDir = argv[++i];
        else if (a == "--image" && i + 1 < argc) imgPath = argv[++i];
        else if (a == "--save" && i + 1 < argc) SetEnvDefault("MADDEN_SAVE_DIR", argv[++i]);
        else if (a == "--trace") g_trace = true;
        else if (a == "--settings" && i + 1 < argc) settingsPath = argv[++i];
        else if (a == "--headless") settingsPath = "";
        else if (a == "--test-pattern") SetEnvDefault("MADDEN_TEST_PATTERN", "1");
        else if (a == "--help" || a == "-h") {
            printf("usage: madden_host [--game DIR] [--image Arcadegame.img] [--save DIR] [--trace]\n"
                   "  --game   folder with your own extracted game files (qkl_*.ast, Arcadegame.dll ...)\n"
                   "  --image  Arcadegame.dll (XEX) or a pre-decrypted .img (default: <game>/Arcadegame.dll)\n");
            return 0;
        } else if (imgPath.empty() && a.rfind("--", 0) != 0) imgPath = a;   // legacy: first positional = image
    }
    if (getenv("MADDEN_TRACE")) g_trace = true;
    if (imgPath.empty()) imgPath = gameDir + "/Arcadegame.dll";
    SetEnvDefault("MADDEN_GAME_DIR", gameDir);
    {   // a roster set (mods/roster.txt) with a roster.tsv is built from your game files first, when its inputs changed
        bool BuildRosterSet(const std::string& gameDir, const std::string& setDir);
        const char* mr = getenv("MADDEN_MODS_ROOT"); std::string mods = mr ? mr : "mods", set;
        if (FILE* f = fopen((mods + "/roster.txt").c_str(), "rb")) { char b[64] = {}; if (fscanf(f, "%63s", b) == 1) set = b; fclose(f); }
        bool okName = !set.empty() && set != "original";
        for (char c : set) if (!isalnum((unsigned char)c) && c != '_' && c != '-') okName = false;
        if (okName) BuildRosterSet(getenv("MADDEN_GAME_DIR") ? getenv("MADDEN_GAME_DIR") : gameDir, mods + "/rosters/" + set);
    }

    g_base = (uint8_t*)PlatReserveGuestMemory(PPC_MEMORY_SIZE);
    if (!g_base) { fprintf(stderr, "could not reserve 4 GB of guest address space\n"); return 1; }
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnCrash);
    MapNullCallSlots();
    AddVectoredExceptionHandler(1, SkipBadGuestCall);
#else
    struct sigaction sa{}; sa.sa_sigaction = OnCrash; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr); sigaction(SIGILL, &sa, nullptr); sigaction(SIGFPE, &sa, nullptr);
#endif

    size_t n = 0;
    std::string ext = imgPath.size() >= 4 ? imgPath.substr(imgPath.size() - 4) : "";
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".img") {   // already-decrypted image (developer workflow)
        FILE* f = fopen(imgPath.c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open image %s\n", imgPath.c_str()); return 1; }
        n = fread(g_base + PPC_IMAGE_BASE, 1, PPC_IMAGE_SIZE, f); fclose(f);
        if (n == 0) { fprintf(stderr, "empty image %s\n", imgPath.c_str()); return 1; }
    } else {               // the user's own Arcadegame.dll (XEX2), decrypted in memory
        uint32_t base = 0;
        std::string err = LoadXexImage(imgPath, g_base + PPC_IMAGE_BASE, PPC_IMAGE_SIZE, base, n);
        if (!err.empty()) { fprintf(stderr, "could not load %s: %s\n", imgPath.c_str(), err.c_str()); return 1; }
        if (base != PPC_IMAGE_BASE) { fprintf(stderr, "unexpected image base %08X (this build expects %08X)\n", base, (uint32_t)PPC_IMAGE_BASE); return 1; }
    }

    size_t mapped = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m)
        if (m->host) { PPC_LOOKUP_FUNC(g_base, m->guest) = m->host; ++mapped; }
    fprintf(stderr, "[host] image loaded (%zu bytes), %zu functions registered, game folder: %s\n", n, mapped, gameDir.c_str());

    KInitKernelVariables();
    LoadSettings(settingsPath);
    if (settingsPath.empty()) g_settings.renderer = "none";
    PresenterStart();

    // main guest thread: stack, PCR and KTHREAD (see threads.cpp)
    PPCContext ctx{};
    g_ctx = &ctx;
    KInitMainThread(ctx);
    const uint32_t mainStackTop = ctx.r1.u32;
    ctx.r3.u64 = PPC_IMAGE_BASE;   // module handle
    ctx.r4.u64 = 1;                // DLL_PROCESS_ATTACH
    ctx.r5.u64 = 0;
    fprintf(stderr, "[host] entering _xstart (0x890ACD80)\n");
    _xstart(ctx, g_base);
    fprintf(stderr, "[host] _xstart returned, r3=0x%X\n", ctx.r3.u32);

    // The XBLA launcher (default.xex) loads Arcadegame.dll, resolves export ordinal 1 and calls it as
    // main(2, { "madden.xex", "<launch param>" }). Do the same.
    uint32_t s0 = GuestAlloc(16), s1 = GuestAlloc(16), av = GuestAlloc(16);
    strcpy((char*)g_base + s0, "madden.xex"); strcpy((char*)g_base + s1, "0");
    WriteU32(av, s0); WriteU32(av + 4, s1); WriteU32(av + 8, 0);
    ctx.r1.u64 = mainStackTop;
    ctx.r3.u64 = 2; ctx.r4.u64 = av;
    fprintf(stderr, "[host] calling game main (export ordinal 1, 0x888CC600)\n");
    sub_888CC600(ctx, g_base);
    fprintf(stderr, "[host] game main returned, r3=0x%X\n", ctx.r3.u32);
    return 0;
}
