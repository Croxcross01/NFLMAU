#include <atomic>
#include <cstdlib>
// Controller input: XamInputGetState & friends. Windows: XInput controllers (any of the 4 ports) plus a keyboard
// fallback on pad 1. Any OS: MADDEN_AUTOPRESS="Start:3:200,A:6:200" presses buttons at given seconds (testing).
#include "kernel.h"
#include <chrono>
#include <string>
#include <vector>
#include <mutex>
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

namespace {
constexpr uint32_t X_ERROR_SUCCESS = 0, X_ERROR_DEVICE_NOT_CONNECTED = 0x48F, X_ERROR_EMPTY = 0x10D2;
enum : uint16_t { DPAD_UP = 0x1, DPAD_DOWN = 0x2, DPAD_LEFT = 0x4, DPAD_RIGHT = 0x8, START = 0x10, BACK = 0x20,
                  LTHUMB = 0x40, RTHUMB = 0x80, LSHOULDER = 0x100, RSHOULDER = 0x200, GUIDE = 0x400,
                  BTN_A = 0x1000, BTN_B = 0x2000, BTN_X = 0x4000, BTN_Y = 0x8000 };
struct Pad { uint16_t buttons = 0; uint8_t lt = 0, rt = 0; int16_t lx = 0, ly = 0, rx = 0, ry = 0; };

struct AutoPress { uint16_t button; double at, dur; };
std::vector<AutoPress> g_auto;
auto g_start = std::chrono::steady_clock::now();

uint16_t ButtonByName(const std::string& n) {
    struct { const char* n; uint16_t b; } t[] = { {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"Start", START},
        {"Back", BACK}, {"Up", DPAD_UP}, {"Down", DPAD_DOWN}, {"Left", DPAD_LEFT}, {"Right", DPAD_RIGHT}, {"LB", LSHOULDER}, {"RB", RSHOULDER} };
    for (auto& e : t) if (n == e.n) return e.b;
    return 0;
}
void InitInput() {
    if (const char* e = getenv("MADDEN_AUTOPRESS")) {
        std::string s = e; size_t p = 0;
        while (p < s.size()) {
            size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
            std::string item = s.substr(p, q - p); p = q + 1;
            char name[32] = {}; double at = 0, ms = 150;
            if (sscanf(item.c_str(), "%31[^:]:%lf:%lf", name, &at, &ms) >= 2 && ButtonByName(name)) g_auto.push_back({ButtonByName(name), at, ms / 1000.0});
        }
        fprintf(stderr, "[input] %zu scripted button presses\n", g_auto.size());
    }
}

#ifdef _WIN32
// XInput is loaded at runtime so the game still starts on systems without it.
struct XINPUT_GAMEPAD_ { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XINPUT_STATE_ { DWORD dwPacketNumber; XINPUT_GAMEPAD_ Gamepad; };
struct XINPUT_VIBRATION_ { WORD wLeftMotorSpeed, wRightMotorSpeed; };
extern "C" unsigned long MauXInputGetState(unsigned long slot, void* out);   // sony_pad.cpp: Xbox + PlayStation controllers
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE_*);
typedef DWORD (WINAPI *XInputSetState_t)(DWORD, XINPUT_VIBRATION_*);
XInputGetState_t pXInputGetState = nullptr; XInputSetState_t pXInputSetState = nullptr;
void LoadXInput() {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryA(dll)) {
            pXInputGetState = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
            if (pXInputGetState) pXInputGetState = (XInputGetState_t)(void*)MauXInputGetState;   // + PlayStation controllers
            pXInputSetState = (XInputSetState_t)GetProcAddress(m, "XInputSetState");
            if (pXInputGetState) { fprintf(stderr, "[input] using %s\n", dll); return; }
        }
    fprintf(stderr, "[input] XInput not available - keyboard only\n");
}
bool KeyboardFocusOk() {   // only read the keyboard when our console/launcher process owns the foreground window
    HWND fg = GetForegroundWindow(); if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() || getenv("MADDEN_KEYBOARD_ALWAYS");
}
bool KeyboardPad(Pad& p) {
    if (!KeyboardFocusOk()) return false;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    struct { int vk; uint16_t b; } map[] = { {VK_RETURN, START}, {VK_ESCAPE, BACK}, {VK_UP, DPAD_UP}, {VK_DOWN, DPAD_DOWN},
        {VK_LEFT, DPAD_LEFT}, {VK_RIGHT, DPAD_RIGHT}, {'Z', BTN_A}, {VK_SPACE, BTN_A}, {'X', BTN_B}, {'C', BTN_X}, {'V', BTN_Y},
        {'Q', LSHOULDER}, {'E', RSHOULDER} };
    bool any = false;
    for (auto& m : map) if (down(m.vk)) { p.buttons |= m.b; any = true; }
    if (down('W')) { p.ly = 32767; any = true; } if (down('S')) { p.ly = -32768; any = true; }
    if (down('A')) { p.lx = -32768; any = true; } if (down('D')) { p.lx = 32767; any = true; }
    if (down(VK_SHIFT)) { p.rt = 255; any = true; }
    return any;
}
#endif

}   // namespace
uint32_t GameStateWord();
std::atomic<int> g_padMute{0};
std::atomic<uint32_t> g_padMuteMask{0};        // per port (bit): the game gets nothing from that pad (its playbook is open)
std::atomic<uint32_t> g_padMuteRelease{0};     // per port: nothing until every button on that pad is let go
std::atomic<uint32_t> g_padPressCount[4];      // presses during a play (game state 3), per port
std::atomic<uint32_t> g_padPressMask[4];       // buttons pressed during a play (cleared by the play-action fix)
std::atomic<bool> g_padMaskBack{false};
std::atomic<uint32_t> g_padInject{0};          // buttons pressed for the game (port 0) until g_padInjectUntil
std::atomic<uint32_t> g_padInjectUntil{0};      // GetTickCount() time
std::atomic<uint32_t> g_padInjectPorts{1};      // which ports get it (bit per port)
namespace {
// Returns true if the pad on this port is connected.
}   // namespace
// Is a controller on this port (controller 1 always counts: the keyboard plays there)?
bool PadConnected(uint32_t user) {
    if (user == 0) return true;
    if (user > 3) return false;
#ifdef _WIN32
    static bool loaded = [] { if (!pXInputGetState) LoadXInput(); return true; }();
    (void)loaded;
    if (!pXInputGetState) return false;
    XINPUT_STATE_ s{};
    return pXInputGetState(user, &s) == 0;
#else
    return false;
#endif
}
namespace {
bool ReadPad(uint32_t user, Pad& p, uint32_t& packet) {
    static const bool inited = [] {
        InitInput();
#ifdef _WIN32
        LoadXInput();
#endif
        return true;
    }();
    (void)inited;
    static uint32_t packets[4] = {};
    bool connected = false;
#ifdef _WIN32
    if (pXInputGetState) {
        XINPUT_STATE_ s{};
        if (pXInputGetState(user, &s) == 0) {
            connected = true;
            p.buttons = s.Gamepad.wButtons; p.lt = s.Gamepad.bLeftTrigger; p.rt = s.Gamepad.bRightTrigger;
            p.lx = s.Gamepad.sThumbLX; p.ly = s.Gamepad.sThumbLY; p.rx = s.Gamepad.sThumbRX; p.ry = s.Gamepad.sThumbRY;
        }
    }
    if (user == 0) { Pad k; if (KeyboardPad(k)) { p.buttons |= k.buttons; if (k.lx) p.lx = k.lx; if (k.ly) p.ly = k.ly; if (k.rt) p.rt = k.rt; } connected = true; }
#endif
    if (user == 0 && !g_auto.empty()) {
        connected = true;
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        for (auto& a : g_auto) if (t >= a.at && t < a.at + a.dur) p.buttons |= a.button;
    }
    // Full playbook box open (1): the game gets no input. 2: none until every button is let go (the A that called the
    // play must not reach the game as a hike). BACK is kept from the game during the play call (it opens the box).
    {
        int mute = g_padMute.load();
        bool idle = !p.buttons && p.lt < 30 && p.rt < 30 && std::abs((int)p.lx) < 9000 && std::abs((int)p.ly) < 9000;
        if (mute == 2 && idle) { g_padMute = 0; mute = 0; }
        if (user < 4) {
            uint32_t bit = 1u << user;
            if ((g_padMuteRelease.load() & bit) && idle) g_padMuteRelease &= ~bit;
            if ((g_padMuteMask.load() | g_padMuteRelease.load()) & bit) mute = 1;
        }
        if (mute) p = Pad{};
        else if (g_padMaskBack.load()) p.buttons &= ~0x0020;
#ifdef _WIN32
        if (!mute && user < 4 && ((g_padInjectPorts.load() >> user) & 1) && g_padInject.load() && (int32_t)(g_padInjectUntil.load() - GetTickCount()) > 0) p.buttons |= (uint16_t)g_padInject.load();
#endif
    }
    // Menus (outside a match): one tap of the d-pad or stick moves one item. A press is shown to the game for its
    // first 15 ms (one menu update), then held back until 400 ms (so the game's own repeat cannot fire on a normal tap), then passed
    // through so holding still scrolls. A release shorter than 30 ms (switch bounce) is ignored.
    if (user < 4 && GameStateWord() >= 16) {   // match states are small numbers; menus read as none
        using namespace std::chrono;
        int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        struct Dir { int64_t down = -1, up = -1; };
        static Dir dirs[4][4];   // port, up/down/left/right
        static const uint16_t bits[4] = { DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT };
        bool held[4] = { (p.buttons & DPAD_UP) || p.ly > 16000, (p.buttons & DPAD_DOWN) || p.ly < -16000,
                         (p.buttons & DPAD_LEFT) || p.lx < -16000, (p.buttons & DPAD_RIGHT) || p.lx > 16000 };
        for (int d = 0; d < 4; d++) {
            Dir& s = dirs[user][d];
            if (held[d]) { if (s.down < 0) s.down = now; s.up = -1; }
            else if (s.down >= 0) { if (s.up < 0) s.up = now; if (now - s.up >= 30) s.down = -1; else held[d] = true; }
            static int tapMs = 15, holdMs = 400;   // 15 ms: the menus see a tap for one update only
            static const bool cfg = [] {   // mods/menu_input.txt: "<tap ms> <hold ms>" to tune this
                if (FILE* f = fopen("mods/menu_input.txt", "r")) { int a, b; if (fscanf(f, "%d %d", &a, &b) == 2 && a > 0 && b > a) { tapMs = a; holdMs = b; } fclose(f); }
                fprintf(stderr, "[input] menu taps: %d ms, hold repeat after %d ms\n", tapMs, holdMs); return true; }();
            (void)cfg;
            bool show = held[d] && s.down >= 0 && (now - s.down < tapMs || now - s.down >= holdMs);
            if (show) p.buttons |= bits[d]; else p.buttons &= ~bits[d];
            if (!show) { if (d < 2 && (d == 0 ? p.ly > 0 : p.ly < 0)) p.ly = 0; if (d >= 2 && (d == 2 ? p.lx < 0 : p.lx > 0)) p.lx = 0; }
        }
    }
    {   // pass study (temporary): buttons pressed during a play
        static uint16_t prevB[4];
        if (p.buttons != prevB[user & 3]) {
            uint16_t press = p.buttons & ~prevB[user & 3]; prevB[user & 3] = p.buttons;
            if (press && GameStateWord() == 3) { static const bool lg = getenv("MADDEN_STUDY") != nullptr; if (lg) fprintf(stderr, "[pad] port %u pressed %04X\n", user, press); g_padPressCount[user & 3]++; g_padPressMask[user & 3] |= press; }
        }
    }
    static Pad last[4];
    if (memcmp(&last[user & 3], &p, sizeof p) != 0) { last[user & 3] = p; packets[user & 3]++; }
    packet = packets[user & 3];
    return connected;
}
void W16(uint32_t a, uint16_t v) { g_base[a] = v >> 8; g_base[a + 1] = v & 0xFF; }
}   // namespace

// XamInputGetState(user, flags, X_INPUT_STATE*)
IMP(XamInputGetState) {
    TRACE(XamInputGetState);
    uint32_t user = ctx.r3.u32, ps = ctx.r5.u32;
    if (user == 0xFF) user = 0;   // "any user"
    if (user > 3) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    Pad p; uint32_t packet = 0;
    if (!ReadPad(user, p, packet)) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    if (ps) {
        WriteU32(ps, packet); W16(ps + 4, p.buttons); g_base[ps + 6] = p.lt; g_base[ps + 7] = p.rt;
        W16(ps + 8, (uint16_t)p.lx); W16(ps + 10, (uint16_t)p.ly); W16(ps + 12, (uint16_t)p.rx); W16(ps + 14, (uint16_t)p.ry);
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamInputGetCapabilities(user, flags, X_INPUT_CAPABILITIES*)
IMP(XamInputGetCapabilities) {
    TRACE(XamInputGetCapabilities);
    uint32_t user = ctx.r3.u32, pc = ctx.r5.u32;
    if (user == 0xFF) user = 0;
    Pad p; uint32_t packet;
    if (user > 3 || !ReadPad(user, p, packet)) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    if (pc) {
        memset(g_base + pc, 0, 20);
        g_base[pc] = 1; g_base[pc + 1] = 1;          // gamepad
        W16(pc + 4, 0xFFFF); g_base[pc + 6] = 0xFF; g_base[pc + 7] = 0xFF;
        for (int i = 0; i < 4; i++) W16(pc + 8 + 2 * i, 0xFFC0);
        W16(pc + 16, 0xFFFF); W16(pc + 18, 0xFFFF);  // vibration supported
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamInputSetState(user, unk, X_INPUT_VIBRATION*)
IMP(XamInputSetState) {
    TRACE(XamInputSetState);
#ifdef _WIN32
    if (pXInputSetState && ctx.r3.u32 < 4 && ctx.r5.u32) {
        XINPUT_VIBRATION_ v{ (WORD)((g_base[ctx.r5.u32] << 8) | g_base[ctx.r5.u32 + 1]), (WORD)((g_base[ctx.r5.u32 + 2] << 8) | g_base[ctx.r5.u32 + 3]) };
        pXInputSetState(ctx.r3.u32, &v);
    }
#endif
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
IMP(XamInputGetKeystrokeEx) { TRACE(XamInputGetKeystrokeEx); ctx.r3.u64 = X_ERROR_EMPTY; }
