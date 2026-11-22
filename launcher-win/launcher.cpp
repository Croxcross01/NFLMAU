// Madden NFL Arcade PC - launcher (native Win32, no dependencies).
// Pick your own game files (the extracted folder or the original XBLA package), then press Play.
#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <commctrl.h>
#include <cmath>
#include <cwctype>

static HWND g_wnd, g_path, g_log, g_play, g_trace, g_status;
static HANDLE g_proc = nullptr;
static std::wstring g_exeDir;
static FILE* g_logFile = nullptr;
enum { ID_PATH = 100, ID_BROWSE_DIR, ID_BROWSE_FILE, ID_PLAY, ID_OPENLOG, ID_TRACE, WM_APPEND = WM_APP + 1, WM_DONE };
static const wchar_t* kRequired[] = {L"Arcadegame.dll", L"qkl_boot.ast", L"qkl_fe2ig.ast", L"qkl_interface.ast", L"qkl_misc.ast", L"qkl_ram.ast", L"qkl_stream.ast"};

static std::wstring IniPath() { return g_exeDir + L"\\launcher.ini"; }
static bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static bool DirExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static std::string Narrow(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); if (!s.empty()) s.pop_back(); return s; }
static std::wstring Widen(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); if (!w.empty()) w.pop_back(); return w; }

// Append text to the log box (UI thread only) and to madden_log.txt
static void AppendUI(const std::wstring& t) {
    std::wstring s; for (wchar_t c : t) { if (c == L'\n') s += L"\r\n"; else if (c != L'\r') s += c; }
    int len = GetWindowTextLengthW(g_log);
    if (len > 900000) { SendMessageW(g_log, EM_SETSEL, 0, 300000); SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L""); len = GetWindowTextLengthW(g_log); }
    SendMessageW(g_log, EM_SETSEL, len, len); SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
}
static void Log(const std::wstring& t) {   // any thread
    if (g_logFile) { std::string n = Narrow(t); fwrite(n.data(), 1, n.size(), g_logFile); fflush(g_logFile); }
    PostMessageW(g_wnd, WM_APPEND, 0, (LPARAM)new std::wstring(t));
}

#include "theme.inc"
#include "settings_page.inc"
#include "mods_page.inc"

// ---------------------------------------------------------------- STFS (XBLA package) extraction
struct Stfs {
    FILE* f = nullptr; uint64_t base = 0; int sep = 1; uint32_t ftCount = 0, ftStart = 0;
    static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
    uint32_t fb(uint32_t n) const { uint32_t r = n + (n / 0xAA + 1) * sep; if (n >= 0xAA) r += (n / 0x70E4 + 1) * sep; if (n >= 0x70E4) r += sep; return r; }
    bool block(uint32_t n, uint8_t* out) { _fseeki64(f, (long long)(base + (uint64_t)fb(n) * 0x1000), SEEK_SET); return fread(out, 1, 0x1000, f) > 0; }
    uint32_t next(uint32_t n) {
        uint32_t g = n / 0xAA; uint32_t tbl = fb(g * 0xAA) - sep; uint8_t b[3] = {};
        _fseeki64(f, (long long)(base + (uint64_t)tbl * 0x1000 + (n % 0xAA) * 0x18 + 0x15), SEEK_SET); fread(b, 1, 3, f);
        return (uint32_t)b[0] << 16 | b[1] << 8 | b[2];
    }
};
struct StfsEntry { std::string name; bool dir, consecutive; uint32_t blocks, start, size; int parent; };

static bool ExtractPackage(const std::wstring& pkg, const std::wstring& outDir) {
    Stfs s; s.f = _wfopen(pkg.c_str(), L"rb");
    if (!s.f) { Log(L"Cannot open the package file.\n"); return false; }
    uint8_t h[0x400]; fread(h, 1, sizeof h, s.f);
    if (memcmp(h, "LIVE", 4) && memcmp(h, "PIRS", 4) && memcmp(h, "CON ", 4)) { Log(L"That file is not an Xbox 360 package (LIVE/PIRS/CON).\n"); fclose(s.f); return false; }
    s.base = (Stfs::be32(h + 0x340) + 0xFFF) & ~0xFFFull;
    s.sep = (h[0x37B] & 1) ? 1 : 2;
    s.ftCount = h[0x37C] | h[0x37D] << 8;
    s.ftStart = h[0x37E] | h[0x37F] << 8 | h[0x380] << 16;
    std::vector<StfsEntry> ents; std::vector<uint8_t> blk(0x1000);
    for (uint32_t i = 0; i < s.ftCount; i++) {
        s.block(s.ftStart + i, blk.data());
        bool end = false;
        for (int j = 0; j < 64; j++) {
            const uint8_t* e = &blk[j * 0x40];
            if (!e[0]) { end = true; break; }
            StfsEntry x; uint8_t fl = e[0x28]; x.name.assign((const char*)e, fl & 0x3F);
            x.consecutive = fl & 0x40; x.dir = fl & 0x80;
            x.blocks = e[0x29] | e[0x2A] << 8 | e[0x2B] << 16; x.start = e[0x2F] | e[0x30] << 8 | e[0x31] << 16;
            x.parent = (int16_t)(e[0x32] << 8 | e[0x33]); x.size = Stfs::be32(e + 0x34);
            ents.push_back(x);
        }
        if (end) break;
    }
    CreateDirectoryW(outDir.c_str(), nullptr);
    for (size_t i = 0; i < ents.size(); i++) {
        const auto& e = ents[i]; if (e.dir) continue;
        std::wstring rel = Widen(e.name);
        for (int p = e.parent, guard = 0; p >= 0 && p < (int)ents.size() && guard < 32; p = ents[p].parent, guard++) rel = Widen(ents[p].name) + L"\\" + rel;
        std::wstring out = outDir + L"\\" + rel;
        for (size_t k = outDir.size() + 1; k < out.size(); k++) if (out[k] == L'\\') CreateDirectoryW(out.substr(0, k).c_str(), nullptr);
        if (FileExists(out)) continue;
        Log(L"  extracting " + rel + L" (" + std::to_wstring(e.size / 1024) + L" KB)\n");
        FILE* o = _wfopen((out + L".part").c_str(), L"wb"); if (!o) { Log(L"  cannot write " + out + L"\n"); fclose(s.f); return false; }
        uint32_t rem = e.size, n = e.start;
        for (uint32_t k = 0; rem && k <= e.blocks; k++) {
            s.block(n, blk.data()); uint32_t c = rem < 0x1000 ? rem : 0x1000; fwrite(blk.data(), 1, c, o); rem -= c;
            n = e.consecutive ? n + 1 : s.next(n);
        }
        fclose(o); MoveFileW((out + L".part").c_str(), out.c_str());
    }
    fclose(s.f);
    return true;
}

// ---------------------------------------------------------------- running the game
static void RunGame(std::wstring gameDir, bool trace) {
    std::wstring host = g_exeDir + L"\\madden_host.exe";
    if (!FileExists(host)) { Log(L"madden_host.exe is missing - keep it in the same folder as this launcher.\n"); PostMessageW(g_wnd, WM_DONE, 1, 0); return; }
    // a package file gets extracted next to the launcher first
    if (FileExists(gameDir)) {
        std::wstring out = g_exeDir + L"\\game";
        Log(L"Extracting your package to " + out + L" (one time)...\n");
        if (!ExtractPackage(gameDir, out)) { PostMessageW(g_wnd, WM_DONE, 1, 0); return; }
        gameDir = out;
    }
    std::wstring missing;
    for (auto* r : kRequired) if (!FileExists(gameDir + L"\\" + r)) missing += std::wstring(L"  ") + r + L"\n";
    if (!missing.empty()) { Log(L"These game files are missing from " + gameDir + L":\n" + missing); PostMessageW(g_wnd, WM_DONE, 1, 0); return; }

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr; CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + host + L"\" --game \"" + gameDir + L"\" --save \"" + g_exeDir + L"\\save\" --settings \"" + SettingsPath() + L"\"" + (trace ? L" --trace" : L"");
    Log(L"> " + cmd + L"\n");
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g_exeDir.c_str(), &si, &pi)) {
        Log(L"Could not start madden_host.exe (error " + std::to_wstring(GetLastError()) + L")\n"); CloseHandle(rd); CloseHandle(wr); PostMessageW(g_wnd, WM_DONE, 1, 0); return;
    }
    CloseHandle(wr); CloseHandle(pi.hThread); g_proc = pi.hProcess;
    char buf[4096]; DWORD n; std::string pending;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
        pending.append(buf, n);
        size_t nl = pending.rfind('\n');
        if (nl != std::string::npos) { Log(Widen(pending.substr(0, nl + 1))); pending.erase(0, nl + 1); }
    }
    if (!pending.empty()) Log(Widen(pending) + L"\n");
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); g_proc = nullptr;
    wchar_t msg[160]; swprintf(msg, 160, L"\nGame exited with code %ld (0x%08lX).\n", (long)code, (unsigned long)code); Log(msg);
    PostMessageW(g_wnd, WM_DONE, code, 0);
}

static void OnPlay() {
    if (g_proc) { if (g_proc != INVALID_HANDLE_VALUE) TerminateProcess(g_proc, 1); return; }
    wchar_t p[MAX_PATH * 2]; GetWindowTextW(g_path, p, MAX_PATH * 2);
    std::wstring path = p; while (!path.empty() && (path.back() == L'\\' || path.back() == L' ')) path.pop_back();
    if (path.empty() || (!DirExists(path) && !FileExists(path))) { MessageBoxW(g_wnd, L"Choose your game folder (or package file) first.", L"Madden NFL Arcade PC", MB_ICONINFORMATION); return; }
    WritePrivateProfileStringW(L"launcher", L"game", path.c_str(), IniPath().c_str());
    if (g_logFile) fclose(g_logFile);
    g_logFile = _wfopen((g_exeDir + L"\\madden_log.txt").c_str(), L"wb");
    SetWindowTextW(g_log, L"");
    g_statusText = L"Running..."; g_statusDot = RGB(47, 143, 255); g_proc = INVALID_HANDLE_VALUE; Repaint();
    bool trace = SendMessageW(g_trace, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::thread(RunGame, path, trace).detach();
}

static void BrowseDir() {
    BROWSEINFOW bi{}; bi.hwndOwner = g_wnd; bi.lpszTitle = L"Select the folder with your game files (Arcadegame.dll, qkl_*.ast)"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) { wchar_t p[MAX_PATH]; if (SHGetPathFromIDListW(id, p)) SetWindowTextW(g_path, p); CoTaskMemFree(id); }
}
static void BrowseFile() {
    wchar_t p[MAX_PATH * 2] = L""; OPENFILENAMEW of{sizeof of}; of.hwndOwner = g_wnd; of.lpstrFile = p; of.nMaxFile = MAX_PATH * 2;
    of.lpstrFilter = L"Xbox 360 package (no extension)\0*.*\0"; of.lpstrTitle = L"Select your Madden NFL Arcade XBLA package"; of.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&of)) SetWindowTextW(g_path, p);
}

static HWND g_pathLabel;
// mods\mod_update.txt is written by the dev tools on "Save mod": "<when>|<roster>|<edits>". A new one is shown in a
// banner on the Play tab (and the log) - the game uses the update the next time Play is pressed.
static std::wstring g_lastUpdate;
static void CheckModUpdate(bool startup) {
    std::wstring path = g_exeDir + L"\\mods\\mod_update.txt";
    FILE* f = _wfopen(path.c_str(), L"rb"); if (!f) return;
    char b[512] = {}; fgets(b, sizeof b, f); fclose(f);
    std::string line = b; while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    std::wstring w = Widen(line);
    if (w.empty() || w == g_lastUpdate) return;
    wchar_t seen[512] = L""; GetPrivateProfileStringW(L"launcher", L"seen_update", L"", seen, 512, IniPath().c_str());
    g_lastUpdate = w;
    size_t a = w.find(L'|'), c = a == std::wstring::npos ? a : w.find(L'|', a + 1);
    std::wstring when = w.substr(0, a), roster = a == std::wstring::npos ? L"" : w.substr(a + 1, c - a - 1), edits = c == std::wstring::npos ? L"?" : w.substr(c + 1);
    std::wstring msg = L"Mod updated: " + roster + L" - " + edits + L" player edits saved " + when + L". " +
                       (g_proc ? L"Close the game and press Play again to use it." : L"Press Play to use it.");
    g_updateMsg = msg; if (g_frame) Repaint();
    if (!startup || w != seen) { AppendUI(L"\n" + msg + L"\n"); MessageBeep(MB_ICONINFORMATION); }
    WritePrivateProfileStringW(L"launcher", L"seen_update", w.c_str(), IniPath().c_str());
}
static std::vector<HWND> g_logCtl, g_filesCtl;
static void Layout(int w, int h) {
    RECT c{kSideW + 32, 100, w - 32, h - 28};   // the card (see BuildFrame)
    int x = c.left + 28, r = c.right - 28, y = c.top + 24;
    // Settings: game files on top, then the graphics settings
    MoveWindow(g_pathLabel, x, y, 600, 20, TRUE);
    MoveWindow(g_path, x, y + 24, r - x - 252, 28, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_BROWSE_DIR), r - 240, y + 23, 116, 30, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_BROWSE_FILE), r - 116, y + 23, 116, 30, TRUE);
    LayoutSettingsPage(x, y + 76, r, c.bottom - 16);
    LayoutModsPage(x, y, r);
    // Log
    MoveWindow(g_trace, x, y, 160, 24, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_OPENLOG), r - 120, y - 3, 120, 30, TRUE);
    MoveWindow(g_log, x, y + 40, r - x, c.bottom - 20 - (y + 40), TRUE);
}
static void ShowTab(int i) {
    g_page = i;
    for (HWND c : g_logCtl) ShowWindow(c, i == PAGE_LOG ? SW_SHOW : SW_HIDE);
    for (HWND c : g_filesCtl) ShowWindow(c, i == PAGE_SETTINGS ? SW_SHOW : SW_HIDE);
    ShowSettingsPage(i == PAGE_SETTINGS);
    ShowModsPage(i == PAGE_MODS);
    Repaint(true);
}

static LRESULT CALLBACK WndProc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = hw;
        HFONT ui = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        ThemeFonts(); LoadBackgroundPicture(); DarkTitleBar(hw);
        HFONT mono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
        HWND lbl = CreateWindowW(L"STATIC", L"Game files: the extracted folder, or the original XBLA package file", WS_CHILD, 12, 10, 600, 20, hw, nullptr, nullptr, nullptr);
        g_path = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hw, (HMENU)ID_PATH, nullptr, nullptr);
        HWND b1 = CreateWindowW(L"BUTTON", L"Folder...", WS_CHILD, 0, 0, 0, 0, hw, (HMENU)ID_BROWSE_DIR, nullptr, nullptr);
        HWND b2 = CreateWindowW(L"BUTTON", L"Package...", WS_CHILD, 0, 0, 0, 0, hw, (HMENU)ID_BROWSE_FILE, nullptr, nullptr);
        g_trace = CreateWindowW(L"BUTTON", L"Verbose trace", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hw, (HMENU)ID_TRACE, nullptr, nullptr);
        HWND ol = CreateWindowW(L"BUTTON", L"Open log", WS_CHILD, 0, 0, 0, 0, hw, (HMENU)ID_OPENLOG, nullptr, nullptr);
        g_log = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0, 0, 0, hw, nullptr, nullptr, nullptr);
        for (HWND c : {lbl, g_path, b1, b2, g_trace, ol}) SendMessageW(c, WM_SETFONT, (WPARAM)ui, TRUE);
        g_pathLabel = lbl;
        g_filesCtl = {lbl, g_path, b1, b2}; g_logCtl = {g_trace, ol, g_log};
        CreateSettingsPage(hw, ui);
        CreateModsPage(hw, ui);
        ThemeChildren(hw);
        { RECT rc; GetClientRect(hw, &rc); Layout(rc.right, rc.bottom); }
        ShowTab(PAGE_PLAY);
        SetTimer(hw, 7, 2000, nullptr);   // watch for dev tools mod updates
        std::thread([] { DetectGpus(); PostMessageW(g_wnd, WM_GPU_DETECTED, 0, 0); }).detach();
        SendMessageW(g_log, WM_SETFONT, (WPARAM)mono, TRUE);
        SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
        wchar_t saved[MAX_PATH * 2] = L""; GetPrivateProfileStringW(L"launcher", L"game", L"", saved, MAX_PATH * 2, IniPath().c_str());
        SetWindowTextW(g_path, saved);
        if (!saved[0]) { g_statusText = L"Choose your game files on the Settings page"; g_statusDot = RGB(255, 193, 58); }
        CheckModUpdate(true);
        AppendUI(L"Madden NFL Arcade PC - early test build\n\n"
                 L"Use your own copy of the game. Pick the folder with your game files (Arcadegame.dll and the qkl_*.ast archives),\n"
                 L"or the original XBLA package file, then press Play.\n\n"
                 L"What to expect right now: the game opens its own window (Vulkan) and runs at ~30 frames per second - logic, loading,\n"
                 L"timing, audio engine and controller input all run - but the window stays black: the part that draws the game's\n"
                 L"picture is being built next, and there is no sound yet. Choose renderer, resolution (720p / 2K / 4K) and quality\n"
                 L"on the Settings tab - your graphics card is detected and the recommended options are marked.\n"
L"Everything shown here is also saved to madden_log.txt next to this launcher.\n");
        return 0; }
    case WM_SIZE: Layout(LOWORD(lp), HIWORD(lp)); Repaint(); return 0;
    case WM_ERASEBKGND: return 1;   // all drawing happens in WM_PAINT
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        if (!g_frame) BuildFrame();
        if (g_frame) {
            HDC m = CreateCompatibleDC(dc); HGDIOBJ o = SelectObject(m, g_frame);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, m, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(m, o); DeleteDC(m);
        }
        EndPaint(hw, &ps);
        return 0; }
    case WM_MOUSEMOVE: {
        int t = HitAt((short)LOWORD(lp), (short)HIWORD(lp));
        if (t != g_hot) { g_hot = t; Repaint(); }
        TRACKMOUSEEVENT tm{sizeof tm, TME_LEAVE, hw, 0}; TrackMouseEvent(&tm);
        SetCursor(LoadCursor(nullptr, t >= 0 ? IDC_HAND : IDC_ARROW));
        return 0; }
    case WM_MOUSELEAVE: if (g_hot >= 0 || g_down) { g_hot = -1; g_down = false; Repaint(); } return 0;
    case WM_LBUTTONDOWN: { g_down = true; SetCapture(hw); if (g_hot == HIT_PLAY) Repaint(); return 0; }
    case WM_LBUTTONUP: {
        ReleaseCapture(); bool was = g_down; g_down = false;
        int t = HitAt((short)LOWORD(lp), (short)HIWORD(lp));
        if (was && t >= HIT_NAV && t < HIT_NAV + PAGE_COUNT) { if (t - HIT_NAV != g_page) ShowTab(t - HIT_NAV); }
        else if (was && t == HIT_PLAY) { OnPlay(); Repaint(); }
        else if (was && t >= HIT_CARD && t < HIT_CARD + 3) ShowTab(t == HIT_CARD + 2 ? PAGE_MODS : PAGE_SETTINGS);
        else Repaint();
        return 0; }
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT && (HWND)wp == hw) { SetCursor(LoadCursor(nullptr, g_hot >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; } break;
    case WM_DRAWITEM: { auto* di = (DRAWITEMSTRUCT*)lp; if (di->CtlType == ODT_BUTTON) { DrawButton(di); return TRUE; } break; }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, kText); SetBkColor((HDC)wp, kEditBg); return (LRESULT)g_editBrush;
    case WM_TIMER: if (wp == 7) CheckModUpdate(false); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = {1060, 720}; return 0;
    case WM_CTLCOLORSTATIC:   // labels and check boxes sit on the solid card
        if ((HWND)lp == g_log) { SetTextColor((HDC)wp, RGB(170, 205, 240)); SetBkColor((HDC)wp, kEditBg); return (LRESULT)g_editBrush; }
        SetTextColor((HDC)wp, (HWND)lp == g_pathLabel || (HWND)lp == g_sNote || (HWND)lp == g_sRec ? kDim : kText);
        SetBkColor((HDC)wp, kCardC); SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)g_cardBrush;
    case WM_NOTIFY:
        return 0;
    case WM_GPU_DETECTED: {
        bool first = GetFileAttributesW(SettingsPath().c_str()) == INVALID_FILE_ATTRIBUTES;
        FillSettingsControls();
        if (first) SaveSettings(true);   // first run: write the recommended settings
        std::wstring msg = g_gpus.empty() ? L"\nNo Vulkan graphics driver was found - update your GPU driver, then see the Settings tab.\n"
                                          : L"\nGraphics card: " + g_gpus[0].name + L" - see the Settings tab for the recommended renderer and resolution.\n";
        AppendUI(msg);
        return 0; }
    case WM_COMMAND:
        if (SettingsCommand(LOWORD(wp), HIWORD(wp))) return 0;
        if (ModsCommand(LOWORD(wp), HIWORD(wp))) return 0;
        switch (LOWORD(wp)) {
        case ID_PLAY: OnPlay(); break;
        case ID_BROWSE_DIR: BrowseDir(); break;
        case ID_BROWSE_FILE: BrowseFile(); break;
        case ID_OPENLOG: ShellExecuteW(hw, L"open", (g_exeDir + L"\\madden_log.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        }
        return 0;
    case WM_APPEND: { auto* s = (std::wstring*)lp; AppendUI(*s); delete s; return 0; }
    case WM_DONE:
        if (g_proc == INVALID_HANDLE_VALUE) g_proc = nullptr;
        g_statusText = wp == 0 ? L"Finished - ready to play again" : L"Stopped - see the Log page"; g_statusDot = wp == 0 ? RGB(46, 204, 113) : RGB(255, 99, 99);
        Repaint(); return 0;
    case WM_CLOSE: if (g_proc && g_proc != INVALID_HANDLE_VALUE) TerminateProcess(g_proc, 1); DestroyWindow(hw); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe; g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L"\\/"));
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"MaddenArcadeLauncher";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = nullptr;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1)); if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    HWND hw = CreateWindowW(wc.lpszClassName, L"Madden NFL Arcade PC - Launcher", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1360, 860, nullptr, nullptr, inst, nullptr);
    ShowWindow(hw, show); UpdateWindow(hw);
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { if (!IsDialogMessageW(hw, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    return 0;
}
