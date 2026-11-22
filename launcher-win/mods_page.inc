// Mods tab: roster set, players per side and the pre-game lineup window. Included by launcher.cpp.
// Everything is stored as small files in the mods folder next to the launcher, read by madden_host.exe at start:
//   mods/roster.txt            roster set: "original" or a folder name under mods/rosters (e.g. "2027")
//   mods/lines.txt             players per side: 5 / 7 / full (also changeable in the game's pre-game window)
//   mods/lineup_picker.off     present = no pre-game lineup window
//   mods/cpu_playbook.txt      "off" = the CPU calls only the arcade plays (default: the full playbook)
//   mods/extra_db.txt          "off" = no extra defender against Shotgun (default "ss")
//   mods/pa_fix.txt            "off" = the game's own play action (user switched to the back after the fake)
//   mods/pass_icons.txt        "game" = the game's own pass icons (default: every receiver gets a button)
//   mods/playcall_plays.txt    the Full Playbook list, built by the game from your game files (delete to rebuild)
//   launcher_bg.jpg            the launcher's background picture (your own; any JPG)

static HWND g_mRoster, g_mRosterNote, g_mLines, g_mPicker, g_mSaved;
static HWND g_mRatings, g_mRatingsNote, g_mAuto, g_mHead1, g_mHead2, g_mCpu, g_mExtra, g_mPa, g_mIcons, g_mRebuild, g_mBgNote, g_mBgPick, g_mBgClear;
static std::vector<HWND> g_modsCtl;
static std::vector<std::wstring> g_rosterSets;   // folder names, index 0 = original ("")
enum { ID_M_ROSTER = 400, ID_M_LINES, ID_M_PICKER, ID_M_CPU, ID_M_EXTRA, ID_M_PA, ID_M_ICONS, ID_M_REBUILD, ID_M_BGPICK, ID_M_BGCLEAR, ID_M_AUTO, ID_M_RATINGS };

static std::wstring ModsDir() { return g_exeDir + L"\\mods"; }
static std::wstring ReadWord(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb"); if (!f) return L"";
    char b[64] = {}; fscanf(f, "%63s", b); fclose(f); return Widen(b);
}
static void WriteWord(const std::wstring& path, const std::string& w) {
    FILE* f = _wfopen(path.c_str(), L"wb"); if (!f) return; fprintf(f, "%s\n", w.c_str()); fclose(f);
}
// display name of a roster set: mods/rosters/<set>/name.txt (first line) or "<set> Roster"
static std::wstring RosterDisplayName(const std::wstring& set) {
    FILE* f = _wfopen((ModsDir() + L"\\rosters\\" + set + L"\\name.txt").c_str(), L"rb");
    if (f) { char b[128] = {}; fgets(b, sizeof b, f); fclose(f); std::string s = b; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.empty()) return Widen(s); }
    return set + L" Roster";
}

static void FillModsControls() {
    SendMessageW(g_mRoster, CB_RESETCONTENT, 0, 0);
    g_rosterSets.assign(1, L"");
    SendMessageW(g_mRoster, CB_ADDSTRING, 0, (LPARAM)L"Original roster");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((ModsDir() + L"\\rosters\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring set = fd.cFileName;
            if (!FileExists(ModsDir() + L"\\rosters\\" + set + L"\\qkl_boot.ast") && !FileExists(ModsDir() + L"\\rosters\\" + set + L"\\roster.tsv")) continue;
            g_rosterSets.push_back(set);
            SendMessageW(g_mRoster, CB_ADDSTRING, 0, (LPARAM)RosterDisplayName(set).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::wstring cur = ReadWord(ModsDir() + L"\\roster.txt");
    int sel = 0;
    for (size_t i = 1; i < g_rosterSets.size(); i++) if (_wcsicmp(g_rosterSets[i].c_str(), cur.c_str()) == 0) sel = (int)i;
    SendMessageW(g_mRoster, CB_SETCURSEL, sel, 0);
    {   // the selected roster's ratings: the player's own file, or the game's own ratings
        int r = (int)SendMessageW(g_mRoster, CB_GETCURSEL, 0, 0);
        bool set = r > 0 && r < (int)g_rosterSets.size();
        bool has = set && FileExists(ModsDir() + L"\\rosters\\" + g_rosterSets[r] + L"\\ratings.csv");
        EnableWindow(g_mRatings, set);
        SetWindowTextW(g_mRatingsNote, !set ? L"" : has ? L"Ratings: from your ratings file." : L"Ratings: the game's own (new players take their spot's ratings). Add a ratings CSV of your own for exact ratings.");
    }
    std::wstring lines = ReadWord(ModsDir() + L"\\lines.txt");
    SendMessageW(g_mLines, CB_SETCURSEL, lines == L"7" ? 1 : (lines == L"full" || lines == L"11") ? 2 : 0, 0);
    SendMessageW(g_mPicker, BM_SETCHECK, FileExists(ModsDir() + L"\\lineup_picker.off") ? BST_UNCHECKED : BST_CHECKED, 0);
    auto onUnless = [](const wchar_t* f, const wchar_t* offWord) { std::wstring w = ReadWord(ModsDir() + f); return (w == offWord || w == L"0") ? BST_UNCHECKED : BST_CHECKED; };
    SendMessageW(g_mCpu, BM_SETCHECK, onUnless(L"\\cpu_playbook.txt", L"off"), 0);
    SendMessageW(g_mExtra, BM_SETCHECK, onUnless(L"\\extra_db.txt", L"off"), 0);
    SendMessageW(g_mPa, BM_SETCHECK, onUnless(L"\\pa_fix.txt", L"off"), 0);
    SendMessageW(g_mIcons, BM_SETCHECK, onUnless(L"\\pass_icons.txt", L"game"), 0);
    SendMessageW(g_mAuto, BM_SETCHECK, GetPrivateProfileIntW(L"launcher", L"autoupdate", 1, (g_exeDir + L"\\launcher.ini").c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_mBgNote, FileExists(g_exeDir + L"\\launcher_bg.jpg") ? L"Using your picture (launcher_bg.jpg next to the launcher)." : L"No picture chosen - the Madden Arcade Unleashed backdrop is shown.");
    SetWindowTextW(g_mSaved, L"");
}

static void SaveMods() {
    CreateDirectoryW(ModsDir().c_str(), nullptr);
    int r = (int)SendMessageW(g_mRoster, CB_GETCURSEL, 0, 0);
    WriteWord(ModsDir() + L"\\roster.txt", r > 0 && r < (int)g_rosterSets.size() ? Narrow(g_rosterSets[r]) : "original");
    int l = (int)SendMessageW(g_mLines, CB_GETCURSEL, 0, 0);
    WriteWord(ModsDir() + L"\\lines.txt", l == 1 ? "7" : l == 2 ? "full" : "5");
    std::wstring off = ModsDir() + L"\\lineup_picker.off";
    if (SendMessageW(g_mPicker, BM_GETCHECK, 0, 0) == BST_CHECKED) DeleteFileW(off.c_str());
    else WriteWord(off, "off");
    auto onOff = [](HWND c, const wchar_t* f, const char* on, const char* offw) { WriteWord(ModsDir() + f, SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED ? on : offw); };
    onOff(g_mCpu, L"\\cpu_playbook.txt", "on", "off");
    onOff(g_mExtra, L"\\extra_db.txt", "ss", "off");
    onOff(g_mPa, L"\\pa_fix.txt", "on", "off");
    onOff(g_mIcons, L"\\pass_icons.txt", "all", "game");
    SetWindowTextW(g_mSaved, L"Saved - used the next time you press Play.");
}

static void CreateModsPage(HWND hw, HFONT ui) {
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | style, 0, 0, 0, 0, hw, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)ui, TRUE); g_modsCtl.push_back(c); return c;
    };
    mk(L"STATIC", L"Roster", 0, 0);
    g_mRoster = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_M_ROSTER);
    g_mRosterNote = mk(L"STATIC", L"The original roster is never changed. Other rosters come with updates (mods\\rosters) and are built from your game when you press Play.", 0, 0);
    mk(L"STATIC", L"Players per side", 0, 0);
    g_mLines = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_M_LINES);
    for (const wchar_t* s : {L"5 on 5 (normal)", L"7 on 7", L"Full lines"}) SendMessageW(g_mLines, CB_ADDSTRING, 0, (LPARAM)s);
    mk(L"STATIC", L"Can also be changed before each match in the pre-game window (LB / RB).", 0, 0);
    g_mPicker = mk(L"BUTTON", L"Show the pre-game lineup window before each match", BS_AUTOCHECKBOX | WS_TABSTOP, ID_M_PICKER);
    g_mSaved = mk(L"STATIC", L"", 0, 0);
    g_mHead1 = mk(L"STATIC", L"GAMEPLAY MODS", 0, 0);
    g_mCpu = mk(L"BUTTON", L"CPU calls plays from the full playbook (not only the arcade picks)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_M_CPU);
    g_mExtra = mk(L"BUTTON", L"Extra defender (SS) comes on against Shotgun plays", BS_AUTOCHECKBOX | WS_TABSTOP, ID_M_EXTRA);
    g_mPa = mk(L"BUTTON", L"Play action: stay on the QB after the fake handoff", BS_AUTOCHECKBOX | WS_TABSTOP, ID_M_PA);
    g_mIcons = mk(L"BUTTON", L"Pass icons on every receiver (A / Y added where the game left them out)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_M_ICONS);
    g_mRebuild = mk(L"BUTTON", L"Rebuild playbook list", WS_TABSTOP, ID_M_REBUILD);
    g_mHead2 = mk(L"STATIC", L"LAUNCHER BACKGROUND", 0, 0);
    g_mBgNote = mk(L"STATIC", L"", 0, 0);
    g_mBgPick = mk(L"BUTTON", L"Choose picture...", WS_TABSTOP, ID_M_BGPICK);
    g_mBgClear = mk(L"BUTTON", L"Remove", WS_TABSTOP, ID_M_BGCLEAR);
    g_mRatings = mk(L"BUTTON", L"Ratings file...", WS_TABSTOP, ID_M_RATINGS);
    g_mRatingsNote = mk(L"STATIC", L"", 0, 0);
    g_mAuto = mk(L"BUTTON", L"Install updates automatically (rosters, mods and fixes from the project)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_M_AUTO);
}

static void LayoutModsPage(int x, int t, int right) {
    const int lw = 170;
    if (g_modsCtl.size() < 8) return;
    MoveWindow(g_modsCtl[0], x, t + 5, lw, 20, TRUE);           // Roster
    MoveWindow(g_mRoster, x + lw, t, 340, 300, TRUE);
    MoveWindow(g_mRosterNote, x + lw, t + 34, right - x - lw, 20, TRUE);
    MoveWindow(g_mRatings, x + lw + 352, t - 1, 140, 30, TRUE);
    MoveWindow(g_mRatingsNote, x + lw, t + 54, right - x - lw, 20, TRUE);
    MoveWindow(g_modsCtl[3], x, t + 105, lw, 20, TRUE);          // Players per side
    MoveWindow(g_mLines, x + lw, t + 100, 340, 200, TRUE);
    MoveWindow(g_modsCtl[5], x + lw, t + 134, right - x - lw, 20, TRUE);
    MoveWindow(g_mPicker, x, t + 166, 520, 24, TRUE);
    MoveWindow(g_mHead1, x, t + 210, 300, 20, TRUE);
    MoveWindow(g_mCpu, x, t + 236, 620, 24, TRUE);
    MoveWindow(g_mExtra, x, t + 264, 620, 24, TRUE);
    MoveWindow(g_mPa, x, t + 292, 620, 24, TRUE);
    MoveWindow(g_mIcons, x, t + 320, 620, 24, TRUE);
    MoveWindow(g_mRebuild, x, t + 352, 190, 32, TRUE);
    MoveWindow(g_mHead2, x, t + 404, 300, 20, TRUE);
    MoveWindow(g_mBgNote, x, t + 428, right - x, 20, TRUE);
    MoveWindow(g_mBgPick, x, t + 454, 170, 32, TRUE);
    MoveWindow(g_mBgClear, x + 182, t + 454, 110, 32, TRUE);
    MoveWindow(g_mAuto, x, t + 500, 620, 24, TRUE);
    MoveWindow(g_mSaved, x, t + 534, right - x, 20, TRUE);
}

static void ShowModsPage(bool show) {
    for (HWND c : g_modsCtl) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
    if (show) FillModsControls();
}

static bool ModsCommand(int id, int code) {
    if ((id == ID_M_ROSTER || id == ID_M_LINES) && code == CBN_SELCHANGE) { SaveMods(); if (id == ID_M_ROSTER) FillModsControls(); return true; }
    if ((id == ID_M_PICKER || id == ID_M_CPU || id == ID_M_EXTRA || id == ID_M_PA || id == ID_M_ICONS) && code == BN_CLICKED) { SaveMods(); return true; }
    if (id == ID_M_RATINGS && code == BN_CLICKED) {   // the player's own ratings CSV for the selected roster
        int r = (int)SendMessageW(g_mRoster, CB_GETCURSEL, 0, 0);
        if (r <= 0 || r >= (int)g_rosterSets.size()) return true;
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW of{}; of.lStructSize = sizeof of; of.hwndOwner = g_wnd; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
        of.lpstrFilter = L"Ratings (*.csv)\0*.csv\0"; of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&of)) {
            std::wstring dst = ModsDir() + L"\\rosters\\" + g_rosterSets[r] + L"\\ratings.csv";
            SetWindowTextW(g_mSaved, CopyFileW(file, dst.c_str(), FALSE) ? L"Ratings file added - the roster is rebuilt with it the next time you press Play." : L"Could not copy that file.");
        }
        FillModsControls(); return true;
    }
    if (id == ID_M_AUTO && code == BN_CLICKED) {
        WritePrivateProfileStringW(L"launcher", L"autoupdate", SendMessageW(g_mAuto, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", (g_exeDir + L"\\launcher.ini").c_str());
        SetWindowTextW(g_mSaved, L"Saved."); return true;
    }
    if (id == ID_M_REBUILD && code == BN_CLICKED) {   // the game builds it again from the game files at the next Play
        DeleteFileW((ModsDir() + L"\\playcall_plays.txt").c_str());
        SetWindowTextW(g_mSaved, L"The playbook list will be rebuilt from your game files the next time you press Play.");
        return true;
    }
    if (id == ID_M_BGPICK && code == BN_CLICKED) {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW of{}; of.lStructSize = sizeof of; of.hwndOwner = g_wnd; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
        of.lpstrFilter = L"JPEG pictures (*.jpg; *.jpeg)\0*.jpg;*.jpeg\0"; of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&of)) {
            std::wstring dst = g_exeDir + L"\\launcher_bg.jpg";
            if (_wcsicmp(file, dst.c_str()) == 0 || CopyFileW(file, dst.c_str(), FALSE)) ReloadBackground();
            else SetWindowTextW(g_mSaved, L"Could not copy that picture.");
        }
        FillModsControls();
        return true;
    }
    if (id == ID_M_BGCLEAR && code == BN_CLICKED) { DeleteFileW((g_exeDir + L"\\launcher_bg.jpg").c_str()); ReloadBackground(); FillModsControls(); return true; }
    return false;
}
