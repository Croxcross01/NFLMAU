// Settings tab: GPU detection + recommendations + settings.ini (shared with madden_host.exe).
// Included by launcher.cpp.
#define VK_NO_PROTOTYPES
#include "volk.h"
#include <commctrl.h>

struct GpuInfo { std::wstring name; uint64_t vram = 0; uint32_t vkMajor = 0, vkMinor = 0; bool discrete = false; uint32_t vendor = 0; };
static std::vector<GpuInfo> g_gpus;
static bool g_vulkanOk = false, g_d3d12Present = false;
static int g_monW = 1920, g_monH = 1080;
static std::wstring g_recRenderer = L"vulkan", g_recQuality = L"medium";

static HWND g_sGpu, g_sRenderer, g_sQuality, g_sScale, g_sAniso, g_sMode, g_sWinSize, g_sVsync, g_sFps, g_sDof, g_sRec, g_sSaved, g_sNote, g_sDevice;
static std::vector<HWND> g_settingsCtl;
static HWND g_sGtagLabel, g_sGtag, g_sOnline, g_sGtInfo;
enum { ID_S_RENDERER = 300, ID_S_QUALITY, ID_S_SCALE, ID_S_ANISO, ID_S_MODE, ID_S_WINSIZE, ID_S_VSYNC, ID_S_FPS, ID_S_DOF, ID_S_RECOMMEND, ID_S_SAVE, ID_S_DEVICE, ID_S_GTAG, ID_S_ONLINE, ID_S_GTSAVE, WM_GPU_DETECTED = WM_APP + 20 };
static const wchar_t* kQualities[] = {L"Low", L"Medium", L"High", L"Custom"};
static const wchar_t* kQualityKeys[] = {L"low", L"medium", L"high", L"custom"};
static const wchar_t* kScales[] = {L"1280 x 720 - native (1x)", L"2560 x 1440 - 2K (2x)", L"3840 x 2160 - 4K (3x)"};
static const int kAniso[] = {1, 2, 4, 8, 16};
static const int kWinSizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};

static std::wstring SettingsPath() { return g_exeDir + L"\\settings.ini"; }

// ---- detection (runs on a worker thread)
static void DetectGpus() {
    g_monW = GetSystemMetrics(SM_CXSCREEN); g_monH = GetSystemMetrics(SM_CYSCREEN);
    if (HMODULE d = LoadLibraryW(L"d3d12.dll")) { g_d3d12Present = true; FreeLibrary(d); }
    if (volkInitialize() == VK_SUCCESS) {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &ai;
        VkInstance inst;
        if (vkCreateInstance(&ci, nullptr, &inst) == VK_SUCCESS) {
            volkLoadInstanceOnly(inst);
            uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
            std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
            for (auto pd : pds) {
                VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
                if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !_wgetenv(L"MADDEN_ALLOW_CPU_GPU")) continue;
                VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
                GpuInfo g; g.name = Widen(p.deviceName); g.vendor = p.vendorID;
                g.vkMajor = VK_API_VERSION_MAJOR(p.apiVersion); g.vkMinor = VK_API_VERSION_MINOR(p.apiVersion);
                g.discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
                for (uint32_t h = 0; h < mp.memoryHeapCount; h++) if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) g.vram = std::max<uint64_t>(g.vram, mp.memoryHeaps[h].size);
                g_gpus.push_back(g);
            }
            vkDestroyInstance(inst, nullptr);
        }
    }
    std::stable_sort(g_gpus.begin(), g_gpus.end(), [](const GpuInfo& a, const GpuInfo& b) { return a.discrete > b.discrete || (a.discrete == b.discrete && a.vram > b.vram); });
    g_vulkanOk = !g_gpus.empty() && (g_gpus[0].vkMajor > 1 || g_gpus[0].vkMinor >= 1);
    // recommendations
    if (g_vulkanOk) {
        g_recRenderer = L"vulkan";
        const GpuInfo& g = g_gpus[0];
        uint64_t gb = g.vram >> 30;
        if (g.discrete && gb >= 6 && g_monH >= 1440) g_recQuality = L"high";
        else if ((g.discrete && gb >= 3) || (!g.discrete && gb >= 2 && g_monH >= 1440)) g_recQuality = L"medium";
        else if (g.discrete && gb >= 6) g_recQuality = L"medium";
        else g_recQuality = L"low";
    } else { g_recRenderer = L"none"; g_recQuality = L"low"; }
}

// ---- settings.ini
static std::wstring IniGet(const wchar_t* k, const wchar_t* def) {
    wchar_t b[256]; GetPrivateProfileStringW(L"settings", k, def, b, 256, SettingsPath().c_str()); return b;
}
static void IniSet(const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"settings", k, v.c_str(), SettingsPath().c_str()); }

static int IndexOf(const wchar_t* const* arr, int n, const std::wstring& v) { for (int i = 0; i < n; i++) if (v == arr[i]) return i; return -1; }
static void ComboFill(HWND c, const std::vector<std::wstring>& items) {
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
}
static int ComboSel(HWND c) { return (int)SendMessageW(c, CB_GETCURSEL, 0, 0); }
static void ComboSet(HWND c, int i) { SendMessageW(c, CB_SETCURSEL, i, 0); }

static void ApplyPresetToControls() {
    int q = ComboSel(g_sQuality);
    bool custom = q == 3;
    if (!custom) { ComboSet(g_sScale, q); ComboSet(g_sAniso, q == 0 ? 0 : q == 1 ? 3 : 4); }
    EnableWindow(g_sScale, custom); EnableWindow(g_sAniso, custom);
}
static void FillSettingsControls() {
    // renderer list with the recommendation marked
    std::vector<std::wstring> rends = {
        std::wstring(L"Vulkan") + (g_recRenderer == L"vulkan" ? L"  (Recommended)" : g_vulkanOk ? L"" : L"  (not supported on this PC)"),
        std::wstring(L"Direct3D 12  (coming soon - uses Vulkan for now)"),
        std::wstring(L"None - headless test") + (g_recRenderer == L"none" ? L"  (Recommended: no Vulkan driver found)" : L"") };
    ComboFill(g_sRenderer, rends);
    std::vector<std::wstring> qs;
    const wchar_t* qdesc[] = {L"Low - 720p native, fastest", L"Medium - 2K (1440p) internal", L"High - 4K (2160p) internal", L"Custom"};
    for (int i = 0; i < 4; i++) qs.push_back(std::wstring(qdesc[i]) + (g_recQuality == kQualityKeys[i] ? L"  (Recommended)" : L""));
    ComboFill(g_sQuality, qs);
    ComboFill(g_sScale, {kScales[0], kScales[1], kScales[2]});
    ComboFill(g_sAniso, {L"Off", L"2x", L"4x", L"8x", L"16x"});
    ComboFill(g_sMode, {L"Windowed", L"Fullscreen (borderless)"});
    std::vector<std::wstring> sizes;
    for (auto& s : kWinSizes) sizes.push_back(std::to_wstring(s[0]) + L" x " + std::to_wstring(s[1]) + (s[0] > g_monW || s[1] > g_monH ? L"  (larger than your screen)" : L""));
    ComboFill(g_sWinSize, sizes);
    std::vector<std::wstring> devs = {L"Automatic (best GPU)"};
    for (auto& g : g_gpus) devs.push_back(g.name);
    ComboFill(g_sDevice, devs);

    // detected hardware summary
    std::wstring det;
    if (g_gpus.empty()) det = L"No Vulkan-capable graphics card found. Update your graphics drivers (NVIDIA / AMD / Intel) to enable the renderer.";
    else {
        const GpuInfo& g = g_gpus[0];
        wchar_t b[512];
        swprintf(b, 512, L"Detected: %ls  -  %.1f GB video memory  -  Vulkan %u.%u%ls  -  screen %dx%d",
                 g.name.c_str(), g.vram / 1073741824.0, g.vkMajor, g.vkMinor, g.discrete ? L"  (dedicated GPU)" : L"  (integrated GPU)", g_monW, g_monH);
        det = b;
    }
    SetWindowTextW(g_sGpu, det.c_str());
    std::wstring rec = L"Recommended for this PC: " + std::wstring(g_recRenderer == L"vulkan" ? L"Vulkan" : L"no renderer (driver update needed)") +
                       L", " + (g_recQuality == L"high" ? L"High (4K)" : g_recQuality == L"medium" ? L"Medium (2K)" : L"Low (720p)") + L" quality, fullscreen.";
    SetWindowTextW(g_sRec, rec.c_str());

    // current values (first run: recommended)
    bool first = GetFileAttributesW(SettingsPath().c_str()) == INVALID_FILE_ATTRIBUTES;
    std::wstring r = first ? g_recRenderer : IniGet(L"renderer", g_recRenderer.c_str());
    ComboSet(g_sRenderer, r == L"d3d12" ? 1 : r == L"none" ? 2 : 0);
    std::wstring q = first ? g_recQuality : IniGet(L"quality", g_recQuality.c_str());
    int qi = IndexOf(kQualityKeys, 4, q); ComboSet(g_sQuality, qi < 0 ? 1 : qi);
    int sc = _wtoi(IniGet(L"resolution_scale", L"2").c_str()); ComboSet(g_sScale, std::clamp(sc, 1, 3) - 1);
    int an = _wtoi(IniGet(L"anisotropy", L"8").c_str()); int ai = 3; for (int i = 0; i < 5; i++) if (kAniso[i] == an) ai = i; ComboSet(g_sAniso, ai);
    ComboSet(g_sMode, (first ? true : IniGet(L"fullscreen", L"1") == L"1") ? 1 : 0);
    int ww = _wtoi(IniGet(L"window_width", L"1600").c_str()), wi = 1;
    for (int i = 0; i < 5; i++) if (kWinSizes[i][0] == ww) wi = i;
    ComboSet(g_sWinSize, wi);
    SendMessageW(g_sVsync, BM_SETCHECK, IniGet(L"vsync", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_sFps, BM_SETCHECK, IniGet(L"show_fps", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_sDof, BM_SETCHECK, IniGet(L"depth_of_field", L"0") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    std::wstring gpu = IniGet(L"gpu", L""); int gi = 0; for (size_t i = 0; i < g_gpus.size(); i++) if (g_gpus[i].name == gpu) gi = (int)i + 1;
    ComboSet(g_sDevice, gi);
    ApplyPresetToControls();
}
static void SaveSettings(bool quiet = false) {
    const wchar_t* rk[] = {L"vulkan", L"d3d12", L"none"};
    int r = std::max(0, ComboSel(g_sRenderer)), q = std::max(0, ComboSel(g_sQuality));
    IniSet(L"renderer", rk[r]);
    IniSet(L"quality", kQualityKeys[q]);
    IniSet(L"resolution_scale", std::to_wstring(std::max(0, ComboSel(g_sScale)) + 1));
    IniSet(L"anisotropy", std::to_wstring(kAniso[std::max(0, ComboSel(g_sAniso))]));
    IniSet(L"fullscreen", ComboSel(g_sMode) == 1 ? L"1" : L"0");
    int wi = std::max(0, ComboSel(g_sWinSize));
    IniSet(L"window_width", std::to_wstring(kWinSizes[wi][0])); IniSet(L"window_height", std::to_wstring(kWinSizes[wi][1]));
    IniSet(L"vsync", SendMessageW(g_sVsync, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    IniSet(L"show_fps", SendMessageW(g_sFps, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    IniSet(L"depth_of_field", SendMessageW(g_sDof, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    int gi = ComboSel(g_sDevice); IniSet(L"gpu", gi > 0 && gi <= (int)g_gpus.size() ? g_gpus[gi - 1].name : L"");
    if (!quiet) SetWindowTextW(g_sSaved, L"Saved.");
}

static HWND MakeLabel(HWND p, const wchar_t* t, HFONT f) { HWND h = CreateWindowW(L"STATIC", t, WS_CHILD, 0, 0, 0, 0, p, nullptr, nullptr, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); g_settingsCtl.push_back(h); return h; }
static HWND MakeCombo(HWND p, int id, HFONT f) { HWND h = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 0, 300, p, (HMENU)(INT_PTR)id, nullptr, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); g_settingsCtl.push_back(h); return h; }
static HWND MakeButton(HWND p, const wchar_t* t, int id, HFONT f, DWORD style = 0) { HWND h = CreateWindowW(L"BUTTON", t, WS_CHILD | style, 0, 0, 0, 0, p, (HMENU)(INT_PTR)id, nullptr, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); g_settingsCtl.push_back(h); return h; }


// ---- Online profile (Xbox Live mode). Your gamertag and online id live in mods/online.txt (gamertag, then
// xuid=...), which the game reads at start; without that file the game stays offline. A gamertag is 1-15 letters,
// digits or spaces and starts with a letter. After the first one it can be changed once every 7 days.
static constexpr int kGtagChangeDays = 7;
static std::wstring OnlineFile() { return g_exeDir + L"\\mods\\online.txt"; }
static std::wstring LauncherIni() { return g_exeDir + L"\\launcher.ini"; }
static std::wstring OnlineGet(const wchar_t* k) { wchar_t b[128] = L""; GetPrivateProfileStringW(L"online", k, L"", b, 128, LauncherIni().c_str()); return b; }
static void OnlineSet(const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"online", k, v.c_str(), LauncherIni().c_str()); }
static bool GamertagOk(const std::wstring& t, std::wstring& why) {
    if (t.empty() || t.size() > 15) { why = L"A gamertag is 1 to 15 characters."; return false; }
    if (!iswalpha(t[0]) || t[0] > 127) { why = L"A gamertag starts with a letter (A-Z)."; return false; }
    for (wchar_t c : t) if (c > 127 || !(iswalnum(c) || c == L' ')) { why = L"Use only letters, numbers and spaces."; return false; }
    if (t.back() == L' ' || t.find(L"  ") != std::wstring::npos) { why = L"No spaces at the end or two in a row."; return false; }
    return true;
}
static uint64_t NowDays() { FILETIME ft; GetSystemTimeAsFileTime(&ft); return (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 864000000000ull; }
static std::string Utf8(const std::wstring& w) { std::string s; for (wchar_t c : w) s += (char)(c < 128 ? c : '?'); return s; }
static void WriteOnlineFile() {   // what the game reads; written when online is on, removed when it is off
    std::wstring tag = OnlineGet(L"gamertag"), xuid = OnlineGet(L"xuid");
    if (OnlineGet(L"enabled") != L"1" || tag.empty()) { DeleteFileW(OnlineFile().c_str()); return; }
    CreateDirectoryW((g_exeDir + L"\\mods").c_str(), nullptr);
    if (FILE* f = _wfopen(OnlineFile().c_str(), L"wb")) { fprintf(f, "%s\nxuid=%s\n", Utf8(tag).c_str(), Utf8(xuid).c_str()); fclose(f); }
}
static void ShowOnlineInfo(const std::wstring& extra = L"") {
    std::wstring tag = OnlineGet(L"gamertag"), info;
    if (tag.empty()) info = L"Pick a gamertag to play online. You can change it later (once every 7 days).";
    else {
        uint64_t last = _wtoi64(OnlineGet(L"changed").c_str()), next = last + kGtagChangeDays;
        info = L"Signed in as " + tag + (OnlineGet(L"enabled") == L"1" ? L" - online play is on." : L" - online play is off.");
        if (last && NowDays() < next) info += L" Next name change in " + std::to_wstring(next - NowDays()) + L" day(s).";
    }
    SetWindowTextW(g_sGtInfo, extra.empty() ? info.c_str() : (extra + L" " + info).c_str());
}
static void FillOnlineControls() {
    // an install that already had mods/online.txt (online testing) keeps that gamertag
    if (OnlineGet(L"gamertag").empty()) {
        std::string t; if (FILE* f = _wfopen(OnlineFile().c_str(), L"rb")) { char b[64] = ""; if (fgets(b, sizeof b, f)) t = b; fclose(f); }
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (!t.empty()) { OnlineSet(L"gamertag", std::wstring(t.begin(), t.end())); OnlineSet(L"enabled", L"1"); }
    }
    if (OnlineGet(L"xuid").empty()) {   // the online id stays the same when the gamertag changes
        uint64_t r = 0; for (int i = 0; i < 4; i++) r = r << 16 ^ (uint64_t)(rand() & 0xFFFF) ^ GetTickCount64();
        wchar_t b[32]; swprintf(b, 32, L"0009%012llX", (unsigned long long)(r & 0xFFFFFFFFFFFFull)); OnlineSet(L"xuid", b);
    }
    SetWindowTextW(g_sGtag, OnlineGet(L"gamertag").c_str());
    SendMessageW(g_sOnline, BM_SETCHECK, OnlineGet(L"enabled") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    WriteOnlineFile();
    ShowOnlineInfo();
}
static void SaveOnline() {
    wchar_t b[64] = L""; GetWindowTextW(g_sGtag, b, 64);
    std::wstring tag = b; while (!tag.empty() && tag.front() == L' ') tag.erase(0, 1); while (!tag.empty() && tag.back() == L' ') tag.pop_back();
    bool on = SendMessageW(g_sOnline, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring old = OnlineGet(L"gamertag"), why;
    if (tag != old) {
        if (!GamertagOk(tag, why)) { SetWindowTextW(g_sGtag, old.c_str()); ShowOnlineInfo(why); return; }
        uint64_t last = _wtoi64(OnlineGet(L"changed").c_str());
        if (!old.empty() && last && NowDays() < last + kGtagChangeDays) {
            SetWindowTextW(g_sGtag, old.c_str());
            ShowOnlineInfo(L"You changed your gamertag recently - try again in " + std::to_wstring(last + kGtagChangeDays - NowDays()) + L" day(s).");
            return;
        }
        if (!old.empty()) OnlineSet(L"changed", std::to_wstring(NowDays()));
        OnlineSet(L"gamertag", tag);
    }
    if (on && tag.empty()) { ShowOnlineInfo(L"Pick a gamertag first."); SendMessageW(g_sOnline, BM_SETCHECK, BST_UNCHECKED, 0); return; }
    OnlineSet(L"enabled", on ? L"1" : L"0");
    WriteOnlineFile();
    ShowOnlineInfo(L"Saved.");
}
struct Row { HWND label, ctl; };
static std::vector<Row> g_rows;
static void CreateSettingsPage(HWND hw, HFONT f) {
    g_sGpu = MakeLabel(hw, L"Detecting your graphics card...", f);
    g_sRec = MakeLabel(hw, L"", f);
    g_rows.push_back({MakeLabel(hw, L"Renderer", f), g_sRenderer = MakeCombo(hw, ID_S_RENDERER, f)});
    g_rows.push_back({MakeLabel(hw, L"Graphics card", f), g_sDevice = MakeCombo(hw, ID_S_DEVICE, f)});
    g_rows.push_back({MakeLabel(hw, L"Quality preset", f), g_sQuality = MakeCombo(hw, ID_S_QUALITY, f)});
    g_rows.push_back({MakeLabel(hw, L"Internal resolution", f), g_sScale = MakeCombo(hw, ID_S_SCALE, f)});
    g_rows.push_back({MakeLabel(hw, L"Texture filtering", f), g_sAniso = MakeCombo(hw, ID_S_ANISO, f)});
    g_rows.push_back({MakeLabel(hw, L"Display mode", f), g_sMode = MakeCombo(hw, ID_S_MODE, f)});
    g_rows.push_back({MakeLabel(hw, L"Window size", f), g_sWinSize = MakeCombo(hw, ID_S_WINSIZE, f)});
    g_sVsync = MakeButton(hw, L"VSync (no screen tearing)", ID_S_VSYNC, f, BS_AUTOCHECKBOX);
    g_sFps = MakeButton(hw, L"Show FPS in the title bar", ID_S_FPS, f, BS_AUTOCHECKBOX);
    g_sDof = MakeButton(hw, L"Replay / cutscene blur (depth of field)", ID_S_DOF, f, BS_AUTOCHECKBOX);
    MakeButton(hw, L"Use recommended", ID_S_RECOMMEND, f);
    MakeButton(hw, L"Save", ID_S_SAVE, f, BS_DEFPUSHBUTTON);
    g_sSaved = MakeLabel(hw, L"", f);
    g_sGtagLabel = MakeLabel(hw, L"Online gamertag", f);
    g_sGtag = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hw, (HMENU)(INT_PTR)ID_S_GTAG, nullptr, nullptr);
    SendMessageW(g_sGtag, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(g_sGtag, EM_LIMITTEXT, 15, 0); g_settingsCtl.push_back(g_sGtag);
    g_sOnline = MakeButton(hw, L"Play online (Xbox Live)", ID_S_ONLINE, f, BS_AUTOCHECKBOX);
    MakeButton(hw, L"Save", ID_S_GTSAVE, f);
    g_sGtInfo = MakeLabel(hw, L"", f);
    g_sNote = MakeLabel(hw, L"Internal resolution is what the game renders at before it is scaled to your screen: 2K and 4K make the picture sharper and need "
                            L"a stronger GPU. Alt+Enter or F11 switches fullscreen while playing. (The in-game picture itself is still being built.)", f);
}
static void LayoutSettingsPage(int x, int y, int right, int bottom) {
    int cw = std::min(460, right - x - 200);
    MoveWindow(g_sGpu, x, y, right - x, 20, TRUE); y += 22;
    MoveWindow(g_sRec, x, y, right - x, 20, TRUE); y += 34;
    for (auto& r : g_rows) { MoveWindow(r.label, x, y + 5, 160, 20, TRUE); MoveWindow(r.ctl, x + 180, y, cw, 300, TRUE); y += 33; }
    MoveWindow(g_sVsync, x + 180, y, 320, 24, TRUE); y += 28;
    MoveWindow(g_sFps, x + 180, y, 320, 24, TRUE); y += 28;
    MoveWindow(g_sDof, x + 180, y, 360, 24, TRUE); y += 38;
    MoveWindow(GetDlgItem(g_wnd, ID_S_RECOMMEND), x + 180, y, 170, 34, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_S_SAVE), x + 362, y, 110, 34, TRUE);
    MoveWindow(g_sSaved, x + 486, y + 7, 260, 20, TRUE); y += 46;
    MoveWindow(g_sGtagLabel, x, y + 5, 160, 20, TRUE);
    MoveWindow(g_sGtag, x + 180, y, 200, 28, TRUE);
    MoveWindow(g_sOnline, x + 396, y + 2, 220, 24, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_S_GTSAVE), x + 626, y - 1, 90, 30, TRUE); y += 34;
    MoveWindow(g_sGtInfo, x + 180, y, right - x - 180, 20, TRUE); y += 30;
    MoveWindow(g_sNote, x, y, right - x, std::max(40, bottom - y), TRUE);
}
static void ShowSettingsPage(bool show) { for (HWND c : g_settingsCtl) ShowWindow(c, show ? SW_SHOW : SW_HIDE); }
static bool SettingsCommand(WORD id, WORD code) {
    switch (id) {
    case ID_S_QUALITY: if (code == CBN_SELCHANGE) { ApplyPresetToControls(); SetWindowTextW(g_sSaved, L""); } return true;
    case ID_S_RECOMMEND: {
        DeleteFileW(SettingsPath().c_str()); FillSettingsControls(); SaveSettings(true); SetWindowTextW(g_sSaved, L"Recommended settings applied.");
        return true; }
    case ID_S_SAVE: SaveSettings(); return true;
    case ID_S_GTSAVE: SaveOnline(); return true;
    case ID_S_GTAG: case ID_S_ONLINE: return true;
    case ID_S_RENDERER: case ID_S_SCALE: case ID_S_ANISO: case ID_S_MODE: case ID_S_WINSIZE: case ID_S_VSYNC: case ID_S_FPS: case ID_S_DOF: case ID_S_DEVICE:
        SetWindowTextW(g_sSaved, L""); return true;
    }
    return false;
}
