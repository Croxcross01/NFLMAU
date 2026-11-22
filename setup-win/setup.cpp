// Madden Arcade Unleashed - Setup (native Win32). The one file players download.
//   1. Install folder   2. Your game (XBLA package or extracted folder; version checked)   3. Settings + mods
//   4. Install: downloads the free llvm-mingw toolchain (GitHub, hash checked), unpacks the runtime pack built into this
//      file (our runtime, launcher, recompiler - no game code), unpacks your game, recompiles it to C++ on THIS PC,
//      compiles it and links madden_host.exe, writes your settings, makes a desktop shortcut, opens the launcher.
// Nothing from the game is in this program or downloaded by it: the PC version is made from the player's own copy.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <urlmon.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <cstdint>

// ---------------------------------------------------------------- constants
static const wchar_t* kToolUrl = L"https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-x86_64.zip";
static const char* kToolSha256 = "4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412";
static const wchar_t* kToolDir = L"llvm-mingw-20261117-ucrt-x86_64";
static const char* kDllSha1 = "b4e72f27effceee64734d365ed9bb214b0772633";   // Arcadegame.dll of the supported release
enum { WM_STEP = WM_APP + 1, WM_FINISHED };

static HWND g_wnd, g_status, g_bar, g_log;
static std::wstring g_installDir, g_game, g_gameDir;
static std::atomic<bool> g_busy{false};
static int g_page = 0;
static FILE* g_logFile;
static std::mutex g_logMx;

static std::wstring Widen(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n ? n - 1 : 0, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); return w; }
static std::string Narrow(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(n ? n - 1 : 0, 0); if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); return s; }
static bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static bool DirExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void MakeDirs(const std::wstring& p) { SHCreateDirectoryExW(nullptr, p.c_str(), nullptr); }

static void Log(const std::wstring& s) {
    std::lock_guard<std::mutex> l(g_logMx);
    if (g_logFile) { fputs(Narrow(s).c_str(), g_logFile); fflush(g_logFile); }
    auto* copy = new std::wstring(s);
    PostMessageW(g_wnd, WM_STEP, 0, (LPARAM)copy);
}
static void Status(const std::wstring& s, int pct) { Log(L"\n== " + s + L"\n"); PostMessageW(g_wnd, WM_STEP, 1 + std::clamp(pct, 0, 100), (LPARAM)new std::wstring(s)); }

// ---------------------------------------------------------------- hashing (BCrypt)
static std::string HashFile(const std::wstring& path, LPCWSTR alg) {
    BCRYPT_ALG_HANDLE a = nullptr; BCRYPT_HASH_HANDLE h = nullptr; std::string hex;
    if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0) != 0) return hex;
    DWORD len = 0, got = 0; BCryptGetProperty(a, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof len, &got, 0);
    std::vector<UCHAR> out(len);
    if (BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (f) {
            std::vector<UCHAR> buf(1 << 20); size_t n;
            while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), (ULONG)n, 0);
            fclose(f);
            if (BCryptFinishHash(h, out.data(), len, 0) == 0) { char t[3]; for (UCHAR c : out) { snprintf(t, 3, "%02x", c); hex += t; } }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return hex;
}

// ---------------------------------------------------------------- STFS (XBLA package) extraction (same as the launcher)
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


// ---------------------------------------------------------------- processes, downloads, unzip
static DWORD RunWait(const std::wstring& cmd, const std::wstring& cwd, bool logOutput, std::wstring* out = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE}; HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{}; std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); Log(L"  could not start: " + cmd + L"\n"); return (DWORD)-1;
    }
    CloseHandle(wr);
    char buf[4096]; DWORD n; std::string all;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) { all.append(buf, n); if (logOutput && all.size() > 2000) { Log(Widen(all)); all.clear(); } }
    if (logOutput && !all.empty()) Log(Widen(all));
    if (out) *out = Widen(all);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return code;
}

struct Progress : IBindStatusCallback {
    std::wstring label; int lastPct = -1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { *p = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG done, ULONG total, ULONG, LPCWSTR) override {
        if (total) { int pct = (int)(100.0 * done / total); if (pct != lastPct) { lastPct = pct; PostMessageW(g_wnd, WM_STEP, 1 + pct, (LPARAM)new std::wstring(label + L" " + std::to_wstring(done >> 20) + L" / " + std::to_wstring(total >> 20) + L" MB")); } }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD* f, BINDINFO*) override { *f = BINDF_GETNEWESTVERSION | BINDF_NOWRITECACHE; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

static bool Unzip(const std::wstring& zip, const std::wstring& dir) {   // tar.exe is part of Windows 10 / 11
    MakeDirs(dir);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    return RunWait(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dir + L"\"", L"", false) == 0;
}

static bool WriteResourceTo(const std::wstring& path) {   // the runtime pack (zip) built into this program
    HRSRC r = FindResourceW(nullptr, L"PACK", RT_RCDATA); if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r); DWORD n = SizeofResource(nullptr, r); const void* p = LockResource(g);
    FILE* f = _wfopen(path.c_str(), L"wb"); if (!f) return false;
    bool ok = fwrite(p, 1, n, f) == n; fclose(f); return ok;
}

static std::string ReadText(const std::wstring& p) { std::string s; FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return s; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }
static bool WriteText(const std::wstring& p, const std::string& s) { FILE* f = _wfopen(p.c_str(), L"wb"); if (!f) return false; fwrite(s.data(), 1, s.size(), f); fclose(f); return true; }
static void Replace(std::string& s, const std::string& a, const std::string& b) { for (size_t i = 0; (i = s.find(a, i)) != std::string::npos; i += b.size()) s.replace(i, a.size(), b); }

// ---------------------------------------------------------------- choices from the wizard
struct Choices { int quality = 1; bool fullscreen = true; int lines = 0; bool cpuBook = true, extraDb = true, paFix = true, icons = true, picker = true, shortcut = true; } g_ch;

static void MakeShortcut(const std::wstring& target, const std::wstring& dir) {
    CoInitialize(nullptr);
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) {
        sl->SetPath(target.c_str()); sl->SetWorkingDirectory(dir.c_str()); sl->SetDescription(L"Madden Arcade Unleashed");
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            wchar_t desk[MAX_PATH]; SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk);
            pf->Save((std::wstring(desk) + L"\\Madden Arcade Unleashed.lnk").c_str(), TRUE); pf->Release();
        }
        sl->Release();
    }
}

// ---------------------------------------------------------------- the install
static bool Fail(const std::wstring& why) { Status(L"Stopped: " + why, 0); Log(L"\nSetup stopped: " + why + L"\nThe full log is in setup_log.txt in the install folder.\n"); return false; }

static bool Install() {
    const std::wstring dir = g_installDir, build = dir + L"\\_build", tools = dir + L"\\_tools";
    MakeDirs(dir); MakeDirs(build); MakeDirs(tools);
    { std::lock_guard<std::mutex> l(g_logMx); g_logFile = _wfopen((dir + L"\\setup_log.txt").c_str(), L"wb"); }

    // 1. the runtime pack (inside this program)
    Status(L"Unpacking Madden Arcade Unleashed files", 2);
    std::wstring packZip = build + L"\\pack.zip";
    if (!WriteResourceTo(packZip) || !Unzip(packZip, build + L"\\pack")) return Fail(L"could not unpack the program files");
    DeleteFileW(packZip.c_str());
    const std::wstring pack = build + L"\\pack";

    // 2. your game
    Status(L"Reading your game", 5);
    std::wstring gameDir = g_game;
    if (FileExists(g_game)) {   // a package file: unpack it once into <install>\game
        gameDir = dir + L"\\game";
        if (!FileExists(gameDir + L"\\Arcadegame.dll") && !ExtractPackage(g_game, gameDir)) return Fail(L"could not unpack your game package");
    }
    std::wstring dll = gameDir + L"\\Arcadegame.dll";
    if (!FileExists(dll)) return Fail(L"Arcadegame.dll was not found in your game files");
    std::string sha1 = HashFile(dll, BCRYPT_SHA1_ALGORITHM);
    if (sha1 != kDllSha1) return Fail(L"this copy of the game is a different version (Arcadegame.dll " + Widen(sha1) + L")");
    g_gameDir = gameDir;

    // 3. build tools (free, from GitHub; checked against a known fingerprint)
    std::wstring tc = tools + L"\\" + kToolDir;
    if (!FileExists(tc + L"\\bin\\clang++.exe")) {
        Status(L"Downloading build tools (about 190 MB, one time)", 8);
        std::wstring tz = tools + L"\\toolchain.zip";
        Progress cb; cb.label = L"Downloading build tools";
        if (URLDownloadToFileW(nullptr, kToolUrl, tz.c_str(), 0, &cb) != S_OK) return Fail(L"could not download the build tools - check your internet connection");
        Status(L"Checking the download", 20);
        if (HashFile(tz, BCRYPT_SHA256_ALGORITHM) != kToolSha256) { DeleteFileW(tz.c_str()); return Fail(L"the build tools download was damaged - please run setup again"); }
        Status(L"Unpacking build tools", 22);
        if (!Unzip(tz, tools)) return Fail(L"could not unpack the build tools");
        DeleteFileW(tz.c_str());
    }

    // 4. recompile the game's code to C++ (on this PC, from your copy)
    Status(L"Translating your game for PC", 26);
    std::wstring gen = build + L"\\ppc";
    MakeDirs(gen); MakeDirs(build + L"\\input");
    if (!CopyFileW(dll.c_str(), (build + L"\\input\\Arcadegame.dll").c_str(), FALSE)) return Fail(L"could not copy Arcadegame.dll");
    {
        std::string cfg = ReadText(pack + L"\\recomp\\arcade.toml.in");
        Replace(cfg, "@IMAGE_SOURCE@", "input/Arcadegame.dll"); Replace(cfg, "@OUT_DIR@", "ppc"); Replace(cfg, "@SWITCH_TABLES@", "switch_tables.toml");
        if (cfg.empty() || !WriteText(build + L"\\arcade.toml", cfg)) return Fail(L"missing recompiler settings");
        CopyFileW((pack + L"\\recomp\\switch_tables.toml").c_str(), (build + L"\\switch_tables.toml").c_str(), FALSE);
    }
    if (RunWait(L"\"" + pack + L"\\tools\\XenonRecomp.exe\" arcade.toml \"" + pack + L"\\include\\XenonUtils\\ppc_context.h\"", build, false) != 0 || !FileExists(gen + L"\\ppc_func_mapping.cpp"))
        return Fail(L"the recompiler did not finish");
    DeleteFileW((build + L"\\input\\Arcadegame.dll").c_str());

    // 5. compile it (all CPU cores)
    std::vector<std::wstring> srcs;
    { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((gen + L"\\ppc_*.cpp").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) { do srcs.push_back(fd.cFileName); while (FindNextFileW(h, &fd)); FindClose(h); } }
    if (srcs.size() < 10) return Fail(L"the recompiler produced no code");
    std::wstring obj = build + L"\\obj"; MakeDirs(obj);
    std::wstring cxx = L"\"" + tc + L"\\bin\\x86_64-w64-mingw32-clang++.exe\"";
    std::wstring inc = L" -I\"" + gen + L"\" -I\"" + pack + L"\\include\\XenonUtils\" -I\"" + pack + L"\\include\\simde\"";
    std::atomic<size_t> next{0}, done{0}; std::atomic<bool> bad{false};
    SYSTEM_INFO si; GetSystemInfo(&si); int jobs = std::max(1, (int)si.dwNumberOfProcessors);
    Status(L"Building the PC version (" + std::to_wstring(srcs.size()) + L" parts on " + std::to_wstring(jobs) + L" CPU threads)", 30);
    std::vector<std::thread> th;
    for (int j = 0; j < jobs; j++) th.emplace_back([&] {
        for (size_t i; !bad && (i = next++) < srcs.size();) {
            std::wstring o = obj + L"\\" + srcs[i].substr(0, srcs[i].size() - 4) + L".o";
            std::wstring outText;
            DWORD rc = RunWait(cxx + L" -std=c++20 -O1 -c" + inc + L" -msse4.1 -mavx2 -fno-strict-aliasing -w \"" + gen + L"\\" + srcs[i] + L"\" -o \"" + o + L"\"", build, false, &outText);
            if (rc != 0) { bad = true; Log(L"  failed: " + srcs[i] + L"\n" + outText); }
            size_t d = ++done;
            PostMessageW(g_wnd, WM_STEP, 1 + 30 + (int)(65.0 * d / srcs.size()), (LPARAM)new std::wstring(L"Building the PC version: " + std::to_wstring(d) + L" / " + std::to_wstring(srcs.size())));
        }
    });
    for (auto& t : th) t.join();
    if (bad) return Fail(L"part of the build failed");

    // 6. link
    Status(L"Putting it together", 96);
    {
        std::string rsp;
        for (auto& s : srcs) rsp += "\"obj/" + Narrow(s.substr(0, s.size() - 4)) + ".o\"\n";
        std::string lib = Narrow(pack) + "\\lib";
        for (char& c : lib) if (c == '\\') c = '/';
        rsp += "\"" + lib + "/libmadden_host.a\"\n-L\"" + lib + "\"\n";
        rsp += "-lglslang\n-lMachineIndependent\n-lGenericCodeGen\n-lOSDependent\n-lSPIRV\n-lglslang-default-resource-limits\n-lavcodec\n-lavutil\n-luser32\n-lgdi32\n-lole32\n-lbcrypt\n-lwinmm\n";
        WriteText(build + L"\\link.rsp", rsp);
    }
    if (RunWait(cxx + L" -fuse-ld=lld -static -Wl,--stack,0x4000000 -o madden_host.exe @link.rsp", build, true) != 0 || !FileExists(build + L"\\madden_host.exe"))
        return Fail(L"linking failed");
    CopyFileW((build + L"\\madden_host.exe").c_str(), (dir + L"\\madden_host.exe").c_str(), FALSE);

    // 7. launcher, settings, mods, shortcut
    Status(L"Finishing up", 98);
    CopyFileW((pack + L"\\app\\MaddenArcadeUnleashed.exe").c_str(), (dir + L"\\MaddenArcadeUnleashed.exe").c_str(), FALSE);
    WritePrivateProfileStringW(L"launcher", L"game", g_gameDir.c_str(), (dir + L"\\launcher.ini").c_str());
    std::wstring ini = dir + L"\\settings.ini";
    const wchar_t* q[] = {L"low", L"medium", L"high"};
    WritePrivateProfileStringW(L"settings", L"renderer", L"vulkan", ini.c_str());
    WritePrivateProfileStringW(L"settings", L"quality", q[std::clamp(g_ch.quality, 0, 2)], ini.c_str());
    WritePrivateProfileStringW(L"settings", L"fullscreen", g_ch.fullscreen ? L"1" : L"0", ini.c_str());
    std::wstring mods = dir + L"\\mods"; MakeDirs(mods);
    auto word = [&](const wchar_t* f, const char* w) { WriteText(mods + L"\\" + f, std::string(w) + "\n"); };
    word(L"lines.txt", g_ch.lines == 1 ? "7" : g_ch.lines == 2 ? "full" : "5");
    word(L"roster.txt", "original");
    word(L"cpu_playbook.txt", g_ch.cpuBook ? "on" : "off");
    word(L"extra_db.txt", g_ch.extraDb ? "ss" : "off");
    word(L"pa_fix.txt", g_ch.paFix ? "on" : "off");
    word(L"pass_icons.txt", g_ch.icons ? "all" : "game");
    if (!g_ch.picker) word(L"lineup_picker.off", "off"); else DeleteFileW((mods + L"\\lineup_picker.off").c_str());
    if (g_ch.shortcut) MakeShortcut(dir + L"\\MaddenArcadeUnleashed.exe", dir);

    // the generated code and objects came from the game: they are not kept
    Status(L"Cleaning up", 99);
    SHFILEOPSTRUCTW op{}; std::wstring del = build + L'\0'; op.wFunc = FO_DELETE; op.pFrom = del.c_str(); op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    Status(L"Done - Madden Arcade Unleashed is installed", 100);
    return true;
}

// ---------------------------------------------------------------- the window (wizard)
enum { ID_NEXT = 100, ID_BACK, ID_BROWSE_DIR, ID_BROWSE_PKG, ID_BROWSE_GAME };
static HWND g_next, g_back, g_dirEdit, g_gameEdit, g_quality, g_mode, g_lines, g_chk[7];
static std::vector<HWND> g_pages[4];
static HFONT g_fTitle, g_fBody, g_fBold;
static HBRUSH g_bgBrush, g_editBrush;
static const COLORREF kBg = RGB(12, 18, 33), kText = RGB(232, 240, 255), kDim = RGB(150, 166, 196);
static const wchar_t* kTitles[] = {L"Welcome", L"Your game", L"Settings", L"Install"};

static void ShowPage(int p) {
    g_page = p;
    for (int i = 0; i < 4; i++) for (HWND c : g_pages[i]) ShowWindow(c, i == p ? SW_SHOW : SW_HIDE);
    SetWindowTextW(g_next, p == 2 ? L"Install" : p == 3 ? L"Play" : L"Next");
    ShowWindow(g_back, p > 0 && p < 3 ? SW_SHOW : SW_HIDE);
    EnableWindow(g_next, p != 3);
    InvalidateRect(g_wnd, nullptr, TRUE);
}

static void Pick(HWND edit, bool folder, const wchar_t* filter) {
    wchar_t buf[MAX_PATH] = L"";
    if (folder) {
        BROWSEINFOW bi{}; bi.hwndOwner = g_wnd; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) { if (SHGetPathFromIDListW(id, buf)) SetWindowTextW(edit, buf); CoTaskMemFree(id); }
    } else {
        OPENFILENAMEW of{}; of.lStructSize = sizeof of; of.hwndOwner = g_wnd; of.lpstrFile = buf; of.nMaxFile = MAX_PATH; of.lpstrFilter = filter; of.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameW(&of)) SetWindowTextW(edit, buf);
    }
}

static std::wstring GetText(HWND h) { wchar_t b[MAX_PATH * 2]; GetWindowTextW(h, b, MAX_PATH * 2); return b; }

static void OnNext() {
    if (g_page == 0) {
        g_installDir = GetText(g_dirEdit);
        while (!g_installDir.empty() && (g_installDir.back() == L'\\' || g_installDir.back() == L' ')) g_installDir.pop_back();
        if (g_installDir.size() < 3) { MessageBoxW(g_wnd, L"Choose a folder to install to.", L"Madden Arcade Unleashed", MB_ICONINFORMATION); return; }
        ShowPage(1);
    } else if (g_page == 1) {
        g_game = GetText(g_gameEdit);
        bool ok = (FileExists(g_game)) || (DirExists(g_game) && FileExists(g_game + L"\\Arcadegame.dll"));
        if (!ok) { MessageBoxW(g_wnd, L"Choose your game: the XBLA package file, or a folder that has Arcadegame.dll in it.", L"Madden Arcade Unleashed", MB_ICONINFORMATION); return; }
        ShowPage(2);
    } else if (g_page == 2) {
        g_ch.quality = (int)SendMessageW(g_quality, CB_GETCURSEL, 0, 0);
        g_ch.fullscreen = SendMessageW(g_mode, CB_GETCURSEL, 0, 0) == 0;
        g_ch.lines = (int)SendMessageW(g_lines, CB_GETCURSEL, 0, 0);
        bool* f[] = {&g_ch.cpuBook, &g_ch.extraDb, &g_ch.paFix, &g_ch.icons, &g_ch.picker, &g_ch.shortcut};
        for (int i = 0; i < 6; i++) *f[i] = SendMessageW(g_chk[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
        ShowPage(3);
        g_busy = true;
        std::thread([] { bool ok = Install(); PostMessageW(g_wnd, WM_FINISHED, ok, 0); }).detach();
    } else if (g_page == 4) { DestroyWindow(g_wnd);   // setup stopped: Close
    } else if (g_page == 3) {
        STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
        std::wstring exe = g_installDir + L"\\MaddenArcadeUnleashed.exe";
        if (CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, g_installDir.c_str(), &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        DestroyWindow(g_wnd);
    }
}

static void DrawButton(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC; RECT r = di->rcItem;
    FillRect(dc, &r, g_bgBrush);
    bool down = di->itemState & ODS_SELECTED, dis = di->itemState & ODS_DISABLED, primary = di->CtlID == ID_NEXT;
    COLORREF top = dis ? RGB(40, 46, 60) : primary ? (down ? RGB(11, 79, 192) : RGB(72, 176, 255)) : (down ? RGB(24, 35, 58) : RGB(38, 50, 77));
    COLORREF bot = dis ? RGB(26, 32, 44) : primary ? (down ? RGB(47, 143, 255) : RGB(11, 79, 192)) : (down ? RGB(38, 50, 77) : RGB(26, 35, 56));
    TRIVERTEX v[2] = {{r.left, r.top, (COLOR16)(GetRValue(top) << 8), (COLOR16)(GetGValue(top) << 8), (COLOR16)(GetBValue(top) << 8), 0},
                      {r.right, r.bottom, (COLOR16)(GetRValue(bot) << 8), (COLOR16)(GetGValue(bot) << 8), (COLOR16)(GetBValue(bot) << 8), 0}};
    GRADIENT_RECT gr{0, 1};
    HRGN rg = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, 10, 10); SelectClipRgn(dc, rg);
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V); SelectClipRgn(dc, nullptr); DeleteObject(rg);
    wchar_t t[64]; GetWindowTextW(di->hwndItem, t, 64);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, dis ? RGB(110, 120, 140) : RGB(255, 255, 255)); SelectObject(dc, g_fBold);
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static HWND Mk(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, HFONT f = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_fBody), TRUE);
    if (page >= 0) g_pages[page].push_back(c);
    if (!_wcsicmp(cls, L"BUTTON") && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX) { typedef HRESULT(WINAPI * T)(HWND, LPCWSTR, LPCWSTR); if (HMODULE u = LoadLibraryW(L"uxtheme.dll")) if (T st = (T)(void*)GetProcAddress(u, "SetWindowTheme")) st(c, L"", L""); }
    return c;
}

static LRESULT CALLBACK WndProc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = hw;
        auto font = [](int px, int w, const wchar_t* face) { return CreateFontW(-px, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face); };
        g_fTitle = font(30, FW_BOLD, L"Segoe UI"); g_fBody = font(15, FW_NORMAL, L"Segoe UI"); g_fBold = font(15, FW_SEMIBOLD, L"Segoe UI");
        g_bgBrush = CreateSolidBrush(kBg); g_editBrush = CreateSolidBrush(RGB(8, 13, 26));
        int x = 40, w = 640;
        // page 0
        Mk(0, L"STATIC", L"This sets up Madden Arcade Unleashed - a native PC version of Madden NFL Arcade with gameplay mods - from your own copy of the game. You need the XBLA game (package file or extracted folder), Windows 10 or 11, a graphics card with Vulkan, and an internet connection for the free build tools (about 190 MB, downloaded once).", SS_LEFT, x, 110, w, 80);
        Mk(0, L"STATIC", L"Install folder", 0, x, 210, w, 20, 0, g_fBold);
        wchar_t la[MAX_PATH]; SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, la);
        g_dirEdit = Mk(0, L"EDIT", (std::wstring(la) + L"\\MaddenArcadeUnleashed").c_str(), ES_AUTOHSCROLL | WS_BORDER, x, 236, w - 130, 28);
        Mk(0, L"BUTTON", L"Browse...", 0, x + w - 118, 235, 118, 30, ID_BROWSE_DIR);
        Mk(0, L"STATIC", L"Needs about 2 GB free while installing (the build tools and temporary files are removed at the end, about 1 GB stays).", SS_LEFT, x, 276, w, 40);
        // page 1
        Mk(1, L"STATIC", L"Choose your copy of Madden NFL Arcade: the XBLA package file (the big file with no extension), or a folder with the extracted game (it has Arcadegame.dll in it). Setup checks that it is the supported version.", SS_LEFT, x, 110, w, 60);
        g_gameEdit = Mk(1, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, x, 186, w - 262, 28);
        Mk(1, L"BUTTON", L"Package file...", 0, x + w - 250, 185, 124, 30, ID_BROWSE_PKG);
        Mk(1, L"BUTTON", L"Folder...", 0, x + w - 118, 185, 118, 30, ID_BROWSE_GAME);
        Mk(1, L"STATIC", L"Your game files are only read. A package is unpacked once into the install folder.", SS_LEFT, x, 226, w, 40);
        // page 2
        Mk(2, L"STATIC", L"Picture quality", 0, x, 116, 180, 20, 0, g_fBold);
        g_quality = Mk(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, x + 190, 112, 300, 200);
        for (const wchar_t* s : {L"Low (720p)", L"Medium (2K) - recommended", L"High (4K)"}) SendMessageW(g_quality, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_quality, CB_SETCURSEL, 1, 0);
        Mk(2, L"STATIC", L"Display", 0, x, 156, 180, 20, 0, g_fBold);
        g_mode = Mk(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST, x + 190, 152, 300, 200);
        for (const wchar_t* s : {L"Fullscreen", L"Window"}) SendMessageW(g_mode, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_mode, CB_SETCURSEL, 0, 0);
        Mk(2, L"STATIC", L"Players per side", 0, x, 196, 180, 20, 0, g_fBold);
        g_lines = Mk(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST, x + 190, 192, 300, 200);
        for (const wchar_t* s : {L"5 on 5 (normal)", L"7 on 7", L"Full lines"}) SendMessageW(g_lines, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_lines, CB_SETCURSEL, 0, 0);
        Mk(2, L"STATIC", L"Mods (you can change these later on the launcher's Mods page)", 0, x, 240, w, 20, 0, g_fBold);
        const wchar_t* names[] = {L"CPU calls plays from the full playbook", L"Extra defender (SS) against Shotgun plays", L"Play action: stay on the QB after the fake",
                                  L"Pass icons on every receiver", L"Pre-game lineup window", L"Desktop shortcut"};
        for (int i = 0; i < 6; i++) { g_chk[i] = Mk(2, L"BUTTON", names[i], BS_AUTOCHECKBOX, x + (i % 2) * 330, 268 + (i / 2) * 30, 320, 24); SendMessageW(g_chk[i], BM_SETCHECK, BST_CHECKED, 0); }
        // page 3
        g_status = Mk(3, L"STATIC", L"Starting...", SS_LEFT, x, 110, w, 22, 0, g_fBold);
        g_bar = Mk(3, PROGRESS_CLASSW, L"", 0, x, 140, w, 18);
        SendMessageW(g_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        g_log = Mk(3, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL | WS_BORDER, x, 172, w, 250);
        SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
        g_back = Mk(-1, L"BUTTON", L"Back", BS_OWNERDRAW | WS_VISIBLE, x + w - 250, 450, 110, 36, ID_BACK, g_fBold);
        g_next = Mk(-1, L"BUTTON", L"Next", BS_OWNERDRAW | WS_VISIBLE, x + w - 128, 450, 128, 36, ID_NEXT, g_fBold);
        ShowPage(0);
        return 0; }
    case WM_ERASEBKGND: { RECT r; GetClientRect(hw, &r); FillRect((HDC)wp, &r, g_bgBrush); return 1; }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        SetBkMode(dc, TRANSPARENT);
        RECT t{40, 22, 700, 60}; SelectObject(dc, g_fTitle); SetTextColor(dc, kText); DrawTextW(dc, L"Madden Arcade Unleashed", -1, &t, DT_LEFT | DT_SINGLELINE);
        RECT s{42, 62, 700, 86}; SelectObject(dc, g_fBody); SetTextColor(dc, RGB(110, 190, 255));
        std::wstring step = L"Setup  \x2022  Step " + std::to_wstring(g_page + 1) + L" of 4: " + kTitles[g_page];
        DrawTextW(dc, step.c_str(), -1, &s, DT_LEFT | DT_SINGLELINE);
        RECT line{40, 92, 680, 94}; HBRUSH b = CreateSolidBrush(RGB(47, 143, 255)); FillRect(dc, &line, b); DeleteObject(b);
        EndPaint(hw, &ps); return 0; }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_log) { SetTextColor((HDC)wp, RGB(170, 205, 240)); SetBkColor((HDC)wp, RGB(8, 13, 26)); return (LRESULT)g_editBrush; }
        SetTextColor((HDC)wp, kText); SetBkColor((HDC)wp, kBg); return (LRESULT)g_bgBrush;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: SetTextColor((HDC)wp, kText); SetBkColor((HDC)wp, RGB(8, 13, 26)); return (LRESULT)g_editBrush;
    case WM_DRAWITEM: DrawButton((DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_NEXT: OnNext(); return 0;
        case ID_BACK: if (g_page > 0 && g_page < 3) ShowPage(g_page - 1); return 0;
        case ID_BROWSE_DIR: Pick(g_dirEdit, true, nullptr); return 0;
        case ID_BROWSE_PKG: Pick(g_gameEdit, false, L"All files\0*.*\0"); return 0;
        case ID_BROWSE_GAME: Pick(g_gameEdit, true, nullptr); return 0;
        }
        break;
    case WM_STEP: {
        auto* s = (std::wstring*)lp;
        if (wp == 0) {   // log text
            std::wstring t = *s; for (size_t i = 0; (i = t.find(L'\n', i)) != std::wstring::npos; i += 2) if (!i || t[i - 1] != L'\r') t.insert(i, 1, L'\r'); else i--;
            int n = GetWindowTextLengthW(g_log); SendMessageW(g_log, EM_SETSEL, n, n); SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
        } else { SetWindowTextW(g_status, s->c_str()); SendMessageW(g_bar, PBM_SETPOS, wp - 1, 0); }
        delete s; return 0; }
    case WM_FINISHED:
        g_busy = false;
        if (wp) { EnableWindow(g_next, TRUE); SetWindowTextW(g_next, L"Play"); }
        else { SetWindowTextW(g_next, L"Close"); EnableWindow(g_next, TRUE); g_page = 4; }
        return 0;
    case WM_CLOSE:
        if (g_busy && MessageBoxW(hw, L"Setup is still working. Stop it?", L"Madden Arcade Unleashed", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(hw); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);
    CoInitialize(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"MaddenArcadeUnleashedSetup";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1)); if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{0, 0, 720, 510}; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hw = CreateWindowW(wc.lpszClassName, L"Madden Arcade Unleashed - Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (HMODULE d = LoadLibraryW(L"dwmapi.dll")) { typedef HRESULT(WINAPI * F)(HWND, DWORD, LPCVOID, DWORD); if (F f = (F)(void*)GetProcAddress(d, "DwmSetWindowAttribute")) { BOOL on = TRUE; f(hw, 20, &on, sizeof on); } }
    ShowWindow(hw, show); UpdateWindow(hw);
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { if (!IsDialogMessageW(hw, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    return 0;
}
