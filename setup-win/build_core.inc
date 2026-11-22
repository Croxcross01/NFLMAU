// Shared by the setup and the launcher's auto-update: build tools, runtime pack, recompile / compile / link.
// Install folder layout:
//   MaddenArcadeUnleashed.exe  madden_host.exe  version.txt  launcher.ini  settings.ini  mods\  game\
//   _tools\llvm-mingw-...\      the free build tools (kept, so updates only need to re-link)
//   _cache\pack\                the runtime pack of the installed version (libraries, recompiler, headers)
//   _cache\obj\                 your game, compiled (made on this PC from your copy; never uploaded anywhere)
// An update downloads a new pack; if its manifest says rebuild=1 (or there is no compiled game yet) the game is
// recompiled and compiled again, otherwise only re-linked (seconds).
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cstdio>

namespace bc {

const wchar_t* const kToolUrl = L"https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-x86_64.zip";
const char* const kToolSha256 = "4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412";
const wchar_t* const kToolDir = L"llvm-mingw-20261117-ucrt-x86_64";
const char* const kDllSha1 = "b4e72f27effceee64734d365ed9bb214b0772633";   // Arcadegame.dll of the supported release
const wchar_t* const kRepoRaw = L"https://raw.githubusercontent.com/Croxcross01/NFLMAU/main/";

struct Sink {   // where progress goes (setup window / launcher status line)
    std::function<void(const std::wstring&)> log;
    std::function<void(const std::wstring&, int)> status;   // text, percent 0..100
};

inline std::wstring W(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n ? n - 1 : 0, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); return w; }
inline std::string N(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(n ? n - 1 : 0, 0); if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); return s; }
inline bool IsFile(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
inline bool IsDir(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
inline void Mkdirs(const std::wstring& p) { SHCreateDirectoryExW(nullptr, p.c_str(), nullptr); }
inline void RemoveTree(const std::wstring& p) {
    if (!IsDir(p) && !IsFile(p)) return;
    std::wstring from = p + L'\0';
    SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}
inline std::string ReadText(const std::wstring& p) { std::string s; FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return s; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }
inline bool WriteText(const std::wstring& p, const std::string& s) { FILE* f = _wfopen(p.c_str(), L"wb"); if (!f) return false; fwrite(s.data(), 1, s.size(), f); fclose(f); return true; }
inline std::string Trim(std::string s) { while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back(); size_t i = 0; while (i < s.size() && s[i] == ' ') i++; return s.substr(i); }
inline void Replace(std::string& s, const std::string& a, const std::string& b) { for (size_t i = 0; (i = s.find(a, i)) != std::string::npos; i += b.size()) s.replace(i, a.size(), b); }

inline std::string HashFile(const std::wstring& path, LPCWSTR alg) {
    BCRYPT_ALG_HANDLE a = nullptr; BCRYPT_HASH_HANDLE h = nullptr; std::string hex;
    if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0) != 0) return hex;
    DWORD len = 0, got = 0; BCryptGetProperty(a, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof len, &got, 0);
    std::vector<UCHAR> out(len);
    if (BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
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

inline DWORD RunWait(const std::wstring& cmd, const std::wstring& cwd, Sink* sink, std::wstring* out = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE}; HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{}; std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); if (sink && sink->log) sink->log(L"  could not start: " + cmd + L"\n"); return (DWORD)-1;
    }
    CloseHandle(wr);
    char buf[4096]; DWORD n; std::string all;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) all.append(buf, n);
    if (out) *out = W(all);
    else if (sink && sink->log && !all.empty()) sink->log(W(all));
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return code;
}

inline bool Unzip(const std::wstring& zip, const std::wstring& dir) {   // tar.exe is part of Windows 10 / 11
    Mkdirs(dir);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    return RunWait(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dir + L"\"", L"", nullptr, nullptr) == 0;
}

struct DlProgress : IBindStatusCallback {
    Sink* sink; std::wstring label; int lo, hi, last = -1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { *p = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG done, ULONG total, ULONG, LPCWSTR) override {
        if (total && sink && sink->status) { int pct = lo + (int)((double)(hi - lo) * done / total); if (pct != last) { last = pct; sink->status(label + L" " + std::to_wstring(done >> 20) + L" / " + std::to_wstring(total >> 20) + L" MB", pct); } }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD* f, BINDINFO*) override { *f = BINDF_GETNEWESTVERSION | BINDF_NOWRITECACHE | BINDF_PRAGMA_NO_CACHE; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};
inline bool Download(const std::wstring& url, const std::wstring& dest, Sink& s, const std::wstring& label, int lo, int hi) {
    DlProgress cb; cb.sink = &s; cb.label = label; cb.lo = lo; cb.hi = hi;
    DeleteFileW(dest.c_str());
    std::wstring u = url + (url.find(L'?') == std::wstring::npos ? L"?t=" : L"&t=") + std::to_wstring(GetTickCount64());   // no stale caches
    if (url.rfind(L"https://github.com/", 0) == 0) u = url;   // release downloads: plain URL
    return URLDownloadToFileW(nullptr, u.c_str(), dest.c_str(), 0, &cb) == S_OK && IsFile(dest);
}

inline std::wstring ToolDir(const std::wstring& install) { return install + L"\\_tools\\" + kToolDir; }
inline bool EnsureToolchain(const std::wstring& install, Sink& s, int lo, int hi) {
    std::wstring tc = ToolDir(install);
    if (IsFile(tc + L"\\bin\\x86_64-w64-mingw32-clang++.exe")) return true;
    std::wstring tools = install + L"\\_tools"; Mkdirs(tools);
    std::wstring tz = tools + L"\\toolchain.zip";
    s.status(L"Downloading build tools (about 190 MB, one time)", lo);
    if (!Download(kToolUrl, tz, s, L"Downloading build tools", lo, lo + (hi - lo) * 3 / 4)) { s.log(L"Download failed - check your internet connection.\n"); return false; }
    s.status(L"Checking the download", lo + (hi - lo) * 3 / 4);
    if (HashFile(tz, BCRYPT_SHA256_ALGORITHM) != kToolSha256) { DeleteFileW(tz.c_str()); s.log(L"The build tools download was damaged.\n"); return false; }
    s.status(L"Unpacking build tools", lo + (hi - lo) * 7 / 8);
    bool ok = Unzip(tz, tools);
    DeleteFileW(tz.c_str());
    return ok && IsFile(tc + L"\\bin\\x86_64-w64-mingw32-clang++.exe");
}

// Recompile (your copy -> C++), then compile on all cores into _cache\obj. The C++ is deleted afterwards.
inline bool RebuildGame(const std::wstring& install, const std::wstring& gameDir, Sink& s, int lo, int hi) {
    const std::wstring cache = install + L"\\_cache", pack = cache + L"\\pack", work = cache + L"\\work", obj = cache + L"\\obj";
    std::wstring dll = gameDir + L"\\Arcadegame.dll";
    if (!IsFile(dll)) { s.log(L"Arcadegame.dll was not found in " + gameDir + L"\n"); return false; }
    if (HashFile(dll, BCRYPT_SHA1_ALGORITHM) != kDllSha1) { s.log(L"This copy of the game is a different version.\n"); return false; }
    RemoveTree(work); Mkdirs(work + L"\\input"); Mkdirs(work + L"\\ppc");
    s.status(L"Translating your game for PC", lo);
    if (!CopyFileW(dll.c_str(), (work + L"\\input\\Arcadegame.dll").c_str(), FALSE)) { s.log(L"could not copy Arcadegame.dll\n"); return false; }
    std::string cfg = ReadText(pack + L"\\recomp\\arcade.toml.in");
    Replace(cfg, "@IMAGE_SOURCE@", "input/Arcadegame.dll"); Replace(cfg, "@OUT_DIR@", "ppc"); Replace(cfg, "@SWITCH_TABLES@", "switch_tables.toml");
    if (cfg.empty() || !WriteText(work + L"\\arcade.toml", cfg)) { s.log(L"missing recompiler settings\n"); return false; }
    CopyFileW((pack + L"\\recomp\\switch_tables.toml").c_str(), (work + L"\\switch_tables.toml").c_str(), FALSE);
    std::wstring rout;
    if (RunWait(L"\"" + pack + L"\\tools\\XenonRecomp.exe\" arcade.toml \"" + pack + L"\\include\\XenonUtils\\ppc_context.h\"", work, &s, &rout) != 0 || !IsFile(work + L"\\ppc\\ppc_func_mapping.cpp")) {
        s.log(L"The recompiler did not finish:\n" + rout.substr(rout.size() > 3000 ? rout.size() - 3000 : 0) + L"\n"); return false;
    }
    DeleteFileW((work + L"\\input\\Arcadegame.dll").c_str());
    std::vector<std::wstring> srcs;
    { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((work + L"\\ppc\\ppc_*.cpp").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) { do srcs.push_back(fd.cFileName); while (FindNextFileW(h, &fd)); FindClose(h); } }
    if (srcs.size() < 10) { s.log(L"the recompiler produced no code\n"); return false; }
    RemoveTree(obj); Mkdirs(obj);
    std::wstring cxx = L"\"" + ToolDir(install) + L"\\bin\\x86_64-w64-mingw32-clang++.exe\"";
    std::wstring inc = L" -I\"" + work + L"\\ppc\" -I\"" + pack + L"\\include\\XenonUtils\" -I\"" + pack + L"\\include\\simde\"";
    std::atomic<size_t> next{0}, done{0}; std::atomic<bool> bad{false};
    SYSTEM_INFO si; GetSystemInfo(&si); int jobs = std::max(1, (int)si.dwNumberOfProcessors);
    s.status(L"Building the PC version (" + std::to_wstring(srcs.size()) + L" parts on " + std::to_wstring(jobs) + L" CPU threads)", lo + 4);
    std::vector<std::thread> th;
    for (int j = 0; j < jobs; j++) th.emplace_back([&] {
        for (size_t i; !bad && (i = next++) < srcs.size();) {
            std::wstring o = obj + L"\\" + srcs[i].substr(0, srcs[i].size() - 4) + L".o", outText;
            DWORD rc = RunWait(cxx + L" -std=c++20 -O1 -c" + inc + L" -msse4.1 -mavx2 -fno-strict-aliasing -w \"" + work + L"\\ppc\\" + srcs[i] + L"\" -o \"" + o + L"\"", work, nullptr, &outText);
            if (rc != 0) { bad = true; s.log(L"  failed: " + srcs[i] + L"\n" + outText); }
            size_t d = ++done;
            s.status(L"Building the PC version: " + std::to_wstring(d) + L" / " + std::to_wstring(srcs.size()), lo + 4 + (int)((hi - lo - 4) * (double)d / srcs.size()));
        }
    });
    for (auto& t : th) t.join();
    RemoveTree(work);   // the generated C++ came from the game: not kept
    if (bad) { RemoveTree(obj); return false; }
    return true;
}

// Link _cache\obj with the pack's runtime libraries into <install>\madden_host.exe (replacing the old one).
inline bool LinkGame(const std::wstring& install, Sink& s, int pct) {
    const std::wstring cache = install + L"\\_cache", pack = cache + L"\\pack", obj = cache + L"\\obj";
    s.status(L"Putting it together", pct);
    std::string rsp; int count = 0;
    { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((obj + L"\\*.o").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) { do { rsp += "\"obj/" + N(fd.cFileName) + "\"\n"; count++; } while (FindNextFileW(h, &fd)); FindClose(h); } }
    if (count < 10) { s.log(L"the compiled game is missing\n"); return false; }
    std::string lib = N(pack) + "/lib"; for (char& c : lib) if (c == '\\') c = '/';
    rsp += "\"" + lib + "/libmadden_host.a\"\n-L\"" + lib + "\"\n";
    rsp += "-lglslang\n-lMachineIndependent\n-lGenericCodeGen\n-lOSDependent\n-lSPIRV\n-lglslang-default-resource-limits\n-lavcodec\n-lavutil\n-luser32\n-lgdi32\n-lole32\n-lbcrypt\n-lwinmm\n";
    WriteText(cache + L"\\link.rsp", rsp);
    std::wstring cxx = L"\"" + ToolDir(install) + L"\\bin\\x86_64-w64-mingw32-clang++.exe\"";
    DeleteFileW((cache + L"\\madden_host.exe").c_str());
    std::wstring out;
    if (RunWait(cxx + L" -fuse-ld=lld -static -Wl,--stack,0x4000000 -o madden_host.exe @link.rsp", cache, &s, &out) != 0 || !IsFile(cache + L"\\madden_host.exe")) {
        s.log(L"Linking failed:\n" + out + L"\n"); return false;
    }
    std::wstring dst = install + L"\\madden_host.exe", old = install + L"\\madden_host.old";
    DeleteFileW(old.c_str());
    if (IsFile(dst) && !MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { s.log(L"madden_host.exe is in use - close the game first\n"); return false; }
    if (!MoveFileExW((cache + L"\\madden_host.exe").c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) { MoveFileExW(old.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING); return false; }
    DeleteFileW(old.c_str());
    return true;
}

// Unpack a runtime pack zip as _cache\pack (the old one is replaced only when the new one unpacked fine).
inline bool InstallPack(const std::wstring& install, const std::wstring& zip) {
    const std::wstring cache = install + L"\\_cache", fresh = cache + L"\\pack_new", cur = cache + L"\\pack";
    Mkdirs(cache); RemoveTree(fresh);
    if (!Unzip(zip, fresh) || !IsFile(fresh + L"\\lib\\libmadden_host.a")) { RemoveTree(fresh); return false; }
    RemoveTree(cur);
    return MoveFileW(fresh.c_str(), cur.c_str()) != 0;
}

// Files the pack brings for the install: the launcher (app\) and mod data the project maintains (mods\, copied over).
inline void ApplyPackFiles(const std::wstring& install, const std::wstring& selfExe) {
    const std::wstring pack = install + L"\\_cache\\pack";
    std::wstring app = pack + L"\\app\\MaddenArcadeUnleashed.exe", dst = install + L"\\MaddenArcadeUnleashed.exe";
    if (IsFile(app) && HashFile(app, BCRYPT_SHA256_ALGORITHM) != HashFile(dst, BCRYPT_SHA256_ALGORITHM)) {
        if (!selfExe.empty() && _wcsicmp(selfExe.c_str(), dst.c_str()) == 0) {   // the running launcher: move it aside first
            std::wstring old = dst + L".old"; DeleteFileW(old.c_str()); MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        CopyFileW(app.c_str(), dst.c_str(), FALSE);
    }
    std::function<void(const std::wstring&, const std::wstring&)> copyTree = [&](const std::wstring& from, const std::wstring& to) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        Mkdirs(to);
        do {
            if (fd.cFileName[0] == L'.') continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copyTree(from + L"\\" + fd.cFileName, to + L"\\" + fd.cFileName);
            else CopyFileW((from + L"\\" + fd.cFileName).c_str(), (to + L"\\" + fd.cFileName).c_str(), FALSE);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    copyTree(pack + L"\\mods", install + L"\\mods");   // roster sets and other project mod data (your own files are kept)
}

// "1.2.10" > "1.2.9"
inline bool Newer(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v) { std::vector<int> p; int x = 0; bool any = false; for (char c : v) { if (c >= '0' && c <= '9') { x = x * 10 + (c - '0'); any = true; } else if (c == '.') { p.push_back(x); x = 0; } } if (any) p.push_back(x); return p; };
    auto pa = parts(a), pb = parts(b); size_t n = std::max(pa.size(), pb.size()); pa.resize(n); pb.resize(n);
    return pa > pb;
}
struct Manifest { std::string version, pack, sha256, notes; bool rebuild = false; };
inline bool ParseManifest(const std::string& text, Manifest& m) {
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i); if (e == std::string::npos) e = text.size();
        std::string line = Trim(text.substr(i, e - i)); i = e + 1;
        size_t q = line.find('='); if (q == std::string::npos || line[0] == '#') continue;
        std::string k = Trim(line.substr(0, q)), v = Trim(line.substr(q + 1));
        if (k == "version") m.version = v; else if (k == "pack") m.pack = v; else if (k == "sha256") m.sha256 = v;
        else if (k == "rebuild") m.rebuild = v == "1"; else if (k == "notes") m.notes = v;
    }
    return !m.version.empty() && !m.pack.empty() && m.sha256.size() == 64;
}

}   // namespace bc
