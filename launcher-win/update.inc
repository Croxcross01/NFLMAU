// Auto-update. Installs made by the setup (they have version.txt) check the project's GitHub repo when the launcher
// opens: update/manifest.txt names the newest version, its runtime pack (update/pack-<version>.zip, SHA-256 checked)
// and whether the game has to be rebuilt. The pack is unpacked into _cache\pack, the game is re-linked (or rebuilt
// from your own copy if rebuild=1), the launcher and the project's mod data files are replaced, then the launcher
// restarts. Turn it off: launcher.ini [launcher] autoupdate=0 (Mods page switch). Included by launcher.cpp.
#include "../setup-win/build_core.inc"

enum { WM_UPD = WM_APP + 40 };
static std::atomic<bool> g_updating{false};
static std::wstring g_version;   // installed version ("" = not a setup install: no auto-update)
static std::atomic<bool> g_checking{false};
static std::atomic<bool> g_updWaiting{false};   // a newer version is out but the game is running: install after it closes

// A notification box that does not stop the update (its own thread, on top of the launcher).
static void Notify(const std::wstring& text) {
    std::thread([text] { MessageBoxW(nullptr, text.c_str(), L"Madden Arcade Unleashed - Update", MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST); }).detach();
}

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
    if (!bc::DownloadManifest(mf, s)) return;   // offline: just play
    bc::Manifest m;
    bool ok = bc::ParseManifest(bc::ReadText(mf), m);
    DeleteFileW(mf.c_str());
    if (!ok || !bc::Newer(m.version, bc::N(g_version))) return;

    std::wstring ver = bc::W(m.version);
    std::wstring notes = m.notes.empty() ? L"" : L"\n\nWhat's new: " + bc::W(m.notes);
    if (g_proc) {   // the game is running (its files are in use): install when it closes
        if (!g_updWaiting.exchange(true)) {
            UpdStatus(L"Update " + ver + L" is ready - it installs when you close the game", 0, RGB(255, 193, 58));
            Notify(L"Update " + ver + L" is ready." + notes + L"\n\nIt will install as soon as you close the game.");
        }
        return;
    }
    g_updWaiting = false;
    g_updating = true;
    Notify(L"Update " + ver + L" is installing now." + notes + L"\n\nThis takes about a minute. PLAY will be ready again when it's done.");
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
    bc::WriteText(dir + L"\\_cache\\updated.txt", bc::N(L"Update " + ver + L" is installed - you're up to date." + notes) + "\n");
    if (newLauncher) PostMessageW(g_wnd, WM_UPD, 1, 0);   // restart into the new launcher (it shows the 'installed' box)
    else PostMessageW(g_wnd, WM_UPD, 2, 0);
}

// "Installed" box: shown once, after the update (and after the restart into a new launcher).
static void ShowUpdatedBox() {
    std::wstring f = g_exeDir + L"\\_cache\\updated.txt";
    std::string t = bc::Trim(bc::ReadText(f));
    if (t.empty()) return;
    DeleteFileW(f.c_str());
    Notify(bc::W(t));
}

// Runs a check now (launcher start, every few minutes while it is open, and when the game closes).
static void CheckForUpdate() {
    if (g_version.empty() || !AutoUpdateOn() || g_updating || g_checking.exchange(true)) return;
    std::thread([] { UpdateThread(); g_checking = false; }).detach();
}

static void StartAutoUpdate() {
    { std::wstring old = std::wstring(g_exeDir) + L"\\MaddenArcadeUnleashed.exe.old"; DeleteFileW(old.c_str()); }
    g_version = bc::W(bc::Trim(bc::ReadText(g_exeDir + L"\\version.txt")));
    g_versionLabel = g_version;
    if (!g_version.empty()) bc::CopyPackMods(g_exeDir);   // repair: the installed version's roster sets / mod data are present
    // Installs from an older setup were left on the original roster: move them to the current one once (a roster
    // picked on the Mods page is kept - saving there sets rosterpicked).
    if (!g_version.empty() && !GetPrivateProfileIntW(L"launcher", L"rosterpicked", 0, IniPath().c_str())) {
        std::string cur = bc::Trim(bc::ReadText(g_exeDir + L"\\mods\\roster.txt"));
        if ((cur.empty() || cur == "original") && bc::IsFile(g_exeDir + L"\\mods\\rosters\\2027\\roster.tsv")) {
            bc::WriteText(g_exeDir + L"\\mods\\roster.txt", "2027\n");
            Log(L"Roster set to the 2027 roster (change it on the Mods page).\n");
        }
        WritePrivateProfileStringW(L"launcher", L"rosterpicked", L"1", IniPath().c_str());
    }
    ShowUpdatedBox();
    CheckForUpdate();
}

// WM_UPD in the window procedure
static void OnUpdMessage(WPARAM wp, LPARAM lp) {
    if (wp == 1) {   // relaunch the replaced launcher
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
        if (CreateProcessW(self, nullptr, nullptr, nullptr, FALSE, 0, nullptr, g_exeDir.c_str(), &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); DestroyWindow(g_wnd); }
        return;
    }
    if (wp == 2) { ShowUpdatedBox(); return; }
    struct M { std::wstring t; int p; COLORREF d; };
    auto* m = (M*)lp;
    g_statusText = m->t; g_statusDot = m->d;
    delete m;
    Repaint();
}
