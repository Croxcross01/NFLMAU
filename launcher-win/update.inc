// Auto-update. Installs made by the setup (they have version.txt) check the project's GitHub repo when the launcher
// opens: update/manifest.txt names the newest version, its runtime pack (update/pack-<version>.zip, SHA-256 checked)
// and whether the game has to be rebuilt. The pack is unpacked into _cache\pack, the game is re-linked (or rebuilt
// from your own copy if rebuild=1), the launcher and the project's mod data files are replaced, then the launcher
// restarts. Turn it off: launcher.ini [launcher] autoupdate=0 (Mods page switch). Included by launcher.cpp.
#include "../setup-win/build_core.inc"

enum { WM_UPD = WM_APP + 40 };
static std::atomic<bool> g_updating{false};
static std::wstring g_version;   // installed version ("" = not a setup install: no auto-update)

static bool AutoUpdateOn() { return GetPrivateProfileIntW(L"launcher", L"autoupdate", 1, IniPath().c_str()) != 0; }

static void UpdStatus(const std::wstring& text, int pct, COLORREF dot) {   // from the update thread to the window
    struct M { std::wstring t; int p; COLORREF d; };
    PostMessageW(g_wnd, WM_UPD, 0, (LPARAM)new M{text, pct, dot});
}

static void UpdateThread() {
    const std::wstring dir = g_exeDir;
    bc::Sink s;
    s.log = [](const std::wstring& t) { Log(t); };
    s.status = [](const std::wstring& t, int pct) { UpdStatus(t + L"  (" + std::to_wstring(std::clamp(pct, 0, 100)) + L"%)", pct, RGB(47, 143, 255)); };
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring mf = std::wstring(tmp) + L"mau_manifest.txt";
    if (!bc::Download(std::wstring(bc::kRepoRaw) + L"update/manifest.txt", mf, s, L"", 0, 0)) return;   // offline: just play
    bc::Manifest m;
    bool ok = bc::ParseManifest(bc::ReadText(mf), m);
    DeleteFileW(mf.c_str());
    if (!ok || !bc::Newer(m.version, bc::N(g_version))) return;

    g_updating = true;
    std::wstring ver = bc::W(m.version);
    Log(L"\nUpdating to version " + ver + L"...\n");
    UpdStatus(L"Updating to version " + ver + L"...", 0, RGB(47, 143, 255));
    auto fail = [&](const std::wstring& why) {
        Log(L"Update to " + ver + L" stopped: " + why + L"\n");
        UpdStatus(L"Update to " + ver + L" didn't finish (" + why + L") - you can still play; it tries again next time", 0, RGB(255, 193, 58));
        g_updating = false;
    };
    std::wstring zip = dir + L"\\_cache\\update.zip";
    bc::Mkdirs(dir + L"\\_cache");
    if (!bc::Download(std::wstring(bc::kRepoRaw) + bc::W(m.pack), zip, s, L"Downloading update " + ver, 1, 15)) return fail(L"download failed");
    if (bc::HashFile(zip, BCRYPT_SHA256_ALGORITHM) != m.sha256) { DeleteFileW(zip.c_str()); return fail(L"the download was damaged"); }
    if (!bc::InstallPack(dir, zip)) { DeleteFileW(zip.c_str()); return fail(L"could not unpack it"); }
    DeleteFileW(zip.c_str());

    // the game: re-link, or rebuild from your copy when the update needs it (or nothing is compiled yet)
    wchar_t game[MAX_PATH * 2] = L""; GetPrivateProfileStringW(L"launcher", L"game", L"", game, MAX_PATH * 2, IniPath().c_str());
    std::wstring gameDir = game; if (bc::IsFile(gameDir)) gameDir = dir + L"\\game";
    bool haveObj = false;
    { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\_cache\\obj\\*.o").c_str(), &fd); if (h != INVALID_HANDLE_VALUE) { haveObj = true; FindClose(h); } }
    bool full = m.rebuild || !haveObj;
    if (!bc::EnsureToolchain(dir, s, 16, full ? 25 : 80)) return fail(L"could not get the build tools");
    if (full && !bc::RebuildGame(dir, gameDir, s, 25, 95)) return fail(L"rebuilding the game failed - see the Log page");
    if (!bc::LinkGame(dir, s, 96)) return fail(L"linking failed - see the Log page");

    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::string before = bc::HashFile(self, BCRYPT_SHA256_ALGORITHM);
    bc::ApplyPackFiles(dir, self);
    bc::WriteText(dir + L"\\version.txt", m.version + "\n");
    g_version = ver; g_versionLabel = ver;
    bool newLauncher = bc::IsFile(std::wstring(self) + L".old") || bc::HashFile(self, BCRYPT_SHA256_ALGORITHM) != before;
    Log(L"Updated to version " + ver + (m.notes.empty() ? L"" : L": " + bc::W(m.notes)) + L"\n");
    UpdStatus(L"Updated to version " + ver + (m.notes.empty() ? L"" : L" - " + bc::W(m.notes)), 100, RGB(46, 204, 113));
    g_updating = false;
    if (newLauncher) PostMessageW(g_wnd, WM_UPD, 1, 0);   // restart into the new launcher
}

static void StartAutoUpdate() {
    { std::wstring old = std::wstring(g_exeDir) + L"\\MaddenArcadeUnleashed.exe.old"; DeleteFileW(old.c_str()); }
    g_version = bc::W(bc::Trim(bc::ReadText(g_exeDir + L"\\version.txt")));
    g_versionLabel = g_version;
    if (!g_version.empty()) bc::CopyPackMods(g_exeDir);   // repair: the installed version's roster sets / mod data are present
    if (g_version.empty() || !AutoUpdateOn()) return;
    std::thread(UpdateThread).detach();
}

// WM_UPD in the window procedure
static void OnUpdMessage(WPARAM wp, LPARAM lp) {
    if (wp == 1) {   // relaunch the replaced launcher
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
        if (CreateProcessW(self, nullptr, nullptr, nullptr, FALSE, 0, nullptr, g_exeDir.c_str(), &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); DestroyWindow(g_wnd); }
        return;
    }
    struct M { std::wstring t; int p; COLORREF d; };
    auto* m = (M*)lp;
    g_statusText = m->t; g_statusDot = m->d;
    delete m;
    Repaint();
}
