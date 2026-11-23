// PlayStation controllers (DualShock 4 and DualSense, USB or Bluetooth), read straight from Windows' HID layer and
// handed to the game as Xbox controllers: Cross = A, Circle = B, Square = X, Triangle = Y, L1/R1 = LB/RB, L2/R2 =
// triggers, Options = START, Share/Create or the touchpad = BACK, L3/R3 = stick clicks.
// Xbox controllers keep their slots; PlayStation controllers fill the free ones in the order they were plugged in.
// Everything in the game that reads a controller goes through MauXInputGetState (also the lineup and play-call
// windows). settings.ini playstation_pads=0 turns this off (e.g. with DS4Windows, which already turns them into
// Xbox controllers - without HidHide one controller would then count twice).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

bool SettingOn(const char* key, bool def);   // perf_tune.cpp

namespace {
struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
typedef DWORD(WINAPI* XGet)(DWORD, XState*);

struct Sony {
    std::wstring path; bool dualSense = false;
    std::mutex m; XState st{}; std::atomic<bool> alive{true};
    // Steam Input, DS4Windows and similar already turn this controller into an Xbox one; then it must not count a
    // second time (it would also press every button for player 2). It counts only once it has pressed buttons that
    // no Xbox controller pressed at the same moment (or when there is no Xbox controller at all).
    std::atomic<bool> own{false}, mirrored{false}; int distinct = 0, same = 0;
};
XGet RealXInput();
std::mutex g_m;
std::vector<Sony*> g_pads;   // never freed (a dead one is marked and skipped; a re-plugged pad gets a new entry)

// setupapi / hid, loaded at run time (nothing extra to link)
struct HidApi {
    void(WINAPI* GetHidGuid)(GUID*) = nullptr;
    BOOLEAN(WINAPI* GetAttributes)(HANDLE, void*) = nullptr;
    HANDLE(WINAPI* GetClassDevs)(const GUID*, PCWSTR, HWND, DWORD) = nullptr;
    BOOL(WINAPI* EnumIfaces)(HANDLE, void*, const GUID*, DWORD, void*) = nullptr;
    BOOL(WINAPI* IfaceDetail)(HANDLE, void*, void*, DWORD, DWORD*, void*) = nullptr;
    BOOL(WINAPI* Destroy)(HANDLE) = nullptr;
    bool ok = false;
} H;
struct HidAttr { ULONG Size; USHORT VendorID, ProductID, Version; };
struct IfaceData { DWORD cbSize; GUID cls; DWORD flags; ULONG_PTR reserved; };

bool LoadHid() {
    static bool done = [] {
        HMODULE hid = LoadLibraryW(L"hid.dll"), sa = LoadLibraryW(L"setupapi.dll");
        if (!hid || !sa) return false;
        H.GetHidGuid = (decltype(H.GetHidGuid))(void*)GetProcAddress(hid, "HidD_GetHidGuid");
        H.GetAttributes = (decltype(H.GetAttributes))(void*)GetProcAddress(hid, "HidD_GetAttributes");
        H.GetClassDevs = (decltype(H.GetClassDevs))(void*)GetProcAddress(sa, "SetupDiGetClassDevsW");
        H.EnumIfaces = (decltype(H.EnumIfaces))(void*)GetProcAddress(sa, "SetupDiEnumDeviceInterfaces");
        H.IfaceDetail = (decltype(H.IfaceDetail))(void*)GetProcAddress(sa, "SetupDiGetDeviceInterfaceDetailW");
        H.Destroy = (decltype(H.Destroy))(void*)GetProcAddress(sa, "SetupDiDestroyDeviceInfoList");
        H.ok = H.GetHidGuid && H.GetAttributes && H.GetClassDevs && H.EnumIfaces && H.IfaceDetail && H.Destroy;
        return H.ok;
    }();
    return done;
}

bool IsSony(USHORT vid, USHORT pid, bool& dualSense) {
    if (vid != 0x054C) return false;
    dualSense = pid == 0x0CE6 || pid == 0x0DF2;
    return dualSense || pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0;
}

SHORT Axis(BYTE v, bool flip) {   // 0..255, 128 = centre -> -32768..32767 (up is positive on Xbox)
    int x = ((int)v - 128) * 256; if (flip) x = -x - 1;
    return (SHORT)std::max(-32768, std::min(32767, x));
}
// one input report -> Xbox state. b = the report from its first data byte (left stick X); layout: sticks 0-3, then
// either buttons at 4,5,6 and triggers at 7,8 (DualShock 4, and DualSense on Bluetooth's basic report) or triggers at
// 4,5 and buttons at 7,8,9 (DualSense full report).
void Convert(const BYTE* b, bool dsFull, XState& x) {
    BYTE b0, b1, b2, l2, r2;
    if (dsFull) { l2 = b[4]; r2 = b[5]; b0 = b[7]; b1 = b[8]; b2 = b[9]; }
    else { b0 = b[4]; b1 = b[5]; b2 = b[6]; l2 = b[7]; r2 = b[8]; }
    WORD w = 0;
    static const WORD hat[8] = {0x1, 0x1 | 0x8, 0x8, 0x2 | 0x8, 0x2, 0x2 | 0x4, 0x4, 0x1 | 0x4};   // N NE E SE S SW W NW
    if ((b0 & 0x0F) < 8) w |= hat[b0 & 0x0F];
    if (b0 & 0x10) w |= 0x4000;   // square -> X
    if (b0 & 0x20) w |= 0x1000;   // cross -> A
    if (b0 & 0x40) w |= 0x2000;   // circle -> B
    if (b0 & 0x80) w |= 0x8000;   // triangle -> Y
    if (b1 & 0x01) w |= 0x0100;   // L1 -> LB
    if (b1 & 0x02) w |= 0x0200;   // R1 -> RB
    if (b1 & 0x10) w |= 0x0020;   // share / create -> BACK
    if (b1 & 0x20) w |= 0x0010;   // options -> START
    if (b1 & 0x40) w |= 0x0040;   // L3
    if (b1 & 0x80) w |= 0x0080;   // R3
    if (b2 & 0x02) w |= 0x0020;   // touchpad click -> BACK too
    x.buttons = w; x.lt = l2; x.rt = r2;
    x.lx = Axis(b[0], false); x.ly = Axis(b[1], true); x.rx = Axis(b[2], false); x.ry = Axis(b[3], true);
}

void Reader(Sony* s) {
    HANDLE h = CreateFileW(s->path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) h = CreateFileW(s->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { s->alive = false; return; }
    BYTE r[600]; DWORD n = 0;
    while (ReadFile(h, r, sizeof r, &n, nullptr) && n > 0) {
        XState x{};
        bool ok = true;
        if (r[0] == 0x01 && n >= 10) Convert(r + 1, s->dualSense && n >= 33, x);   // USB (DualSense: full layout), or Bluetooth's basic report
        else if (r[0] == 0x11 && n >= 12 && !s->dualSense) Convert(r + 3, false, x);   // DualShock 4, Bluetooth full
        else if (r[0] == 0x31 && n >= 12 && s->dualSense) Convert(r + 2, true, x);     // DualSense, Bluetooth full
        else ok = false;
        if (ok) {
            WORD pressedNow = 0;
            { std::lock_guard<std::mutex> l(s->m); pressedNow = x.buttons & ~s->st.buttons; x.packet = std::max<DWORD>(1, s->st.packet + (memcmp(&x.buttons, &s->st.buttons, sizeof(XState) - 4) ? 1 : 0)); s->st = x; }
            if (pressedNow && !s->own && !s->mirrored) {   // a new press: does an Xbox controller show it too?
                XGet xg = RealXInput(); bool any = false, match = false;
                for (DWORD i = 0; i < 4 && xg; i++) { XState t{}; if (xg(i, &t) == 0) { any = true; if ((t.buttons & pressedNow) == pressedNow) match = true; } }
                if (!any) s->own = true;
                else if (match) { if (++s->same >= 3) { s->mirrored = true; fprintf(stderr, "[input] PlayStation controller is already an Xbox controller (Steam / DS4Windows) - used once, as that\n"); } }
                else if (++s->distinct >= 2) s->own = true;
                if (s->own) fprintf(stderr, "[input] PlayStation controller in use\n");
            }
        }
    }
    CloseHandle(h);
    s->alive = false;
    fprintf(stderr, "[input] PlayStation controller disconnected\n");
}

// Looks for PlayStation controllers now and every 2 seconds (plugging one in or pairing it works mid-game).
void Scan() {
    GUID g; H.GetHidGuid(&g);
    HANDLE set = H.GetClassDevs(&g, nullptr, nullptr, 0x12 /*DIGCF_PRESENT | DIGCF_DEVICEINTERFACE*/);
    if (set == INVALID_HANDLE_VALUE) return;
    for (DWORD i = 0;; i++) {
        IfaceData d{}; d.cbSize = sizeof d;
        if (!H.EnumIfaces(set, nullptr, &g, i, &d)) break;
        DWORD need = 0; H.IfaceDetail(set, &d, nullptr, 0, &need, nullptr);
        if (need < 8 || need > 4096) continue;
        std::vector<BYTE> buf(need); *(DWORD*)buf.data() = 8;   // SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize on 64-bit Windows
        if (!H.IfaceDetail(set, &d, buf.data(), need, nullptr, nullptr)) continue;
        std::wstring path = (const wchar_t*)(buf.data() + 4);
        {
            std::lock_guard<std::mutex> l(g_m);
            bool known = false; for (auto* p : g_pads) if (p->alive && p->path == path) known = true;
            if (known) continue;
        }
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HidAttr a{}; a.Size = sizeof a; bool ds = false;
        bool sony = H.GetAttributes(h, &a) && IsSony(a.VendorID, a.ProductID, ds);
        CloseHandle(h);
        if (!sony) continue;
        // one controller shows several HID interfaces on some systems: only the game-pad one sends input reports, the
        // others' readers end at once
        Sony* s = new Sony; s->path = path; s->dualSense = ds;
        { std::lock_guard<std::mutex> l(g_m); g_pads.push_back(s); }
        fprintf(stderr, "[input] PlayStation controller found: %s (%04X)\n", ds ? "DualSense" : "DualShock 4", a.ProductID);
        std::thread(Reader, s).detach();
    }
    H.Destroy(set);
}

XGet RealXInput() {   // (declared above)
    static XGet f = [] {
        for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryA(dll)) if (auto p = (XGet)(void*)GetProcAddress(m, "XInputGetState")) return p;
        return (XGet) nullptr;
    }();
    return f;
}
bool Enabled() {
    static bool on = [] {
        bool v = SettingOn("playstation_pads", true) && LoadHid();
        if (v) std::thread([] { for (;;) { Scan(); Sleep(2000); } }).detach();
        else fprintf(stderr, "[input] PlayStation controllers: off\n");
        return v;
    }();
    return on;
}
}   // namespace

// XInputGetState for the game: Xbox controllers in their own slots, PlayStation controllers in the free ones.
extern "C" DWORD WINAPI MauXInputGetState(DWORD slot, void* out) {
    XGet x = RealXInput();
    XState* s = (XState*)out;
    if (x && x(slot, s) == 0) return 0;
    if (slot > 3 || !Enabled()) return 1167;   // ERROR_DEVICE_NOT_CONNECTED
    // which free slot this is (0 = the first slot with no Xbox controller); asking XInput about an empty slot is slow,
    // so whether a slot has an Xbox controller is remembered for half a second
    static std::atomic<uint32_t> xboxMask{0}, maskTime{0};
    DWORD now = GetTickCount();
    if (now - maskTime.load() > 500) {
        uint32_t m = 0; for (DWORD i = 0; i < 4; i++) { XState t; if (x && x(i, &t) == 0) m |= 1u << i; }
        xboxMask = m; maskTime = now;
    }
    int free = 0;
    for (DWORD i = 0; i < slot; i++) if (!((xboxMask.load() >> i) & 1)) free++;
    std::lock_guard<std::mutex> l(g_m);
    int k = 0;
    for (auto* p : g_pads) {
        if (!p->alive || !p->st.packet) continue;   // dead, or an interface that never sent input
        if (!p->own) {   // not confirmed yet: only while there is no Xbox controller at all
            if (p->mirrored || xboxMask.load()) continue;
        }
        if (k++ == free) { std::lock_guard<std::mutex> l2(p->m); *s = p->st; return 0; }
    }
    return 1167;
}
#endif
