// Towers: a ladder mode. Pick Towers on the main menu (menu_mod.cpp adds the button), choose your team on the left
// (home) side the first time; you keep that team for the whole climb and play every other NFL team once, weakest to
// strongest (team overall). Win to move up; lose and you play that team again. Progress is kept in
// mods/towers_state.txt.
//
// How it hooks the game:
//  - Towers vs Play Now: Play Now's menu button carries a game mode (the game looks up GAMEMODE when it is pressed);
//    Towers has none. TowersTeamSelect() is told which one opened team select.
//  - Teams: when team select starts the match it writes each side's team to its match-team table
//    (update MTTS set DIGT/DIOT ... where UNLS = side) and the home/away teams to the game info table
//    (update FNIG set GTHG ... GTAG ...): in a Towers game those are set to your team (home) and the next opponent.
//  - Result: at the final whistle the game reads the home and away scores (select ELQG, CSHG, CSAG ... from FNIG);
//    the values are picked up on the next query.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "archive_overlay.h"
#include "gamedb.h"
#include "host.h"
#include "towers_ui.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

bool ReadOriginalEntry(const std::string& path, uint32_t entry, std::vector<uint8_t>& out);   // playbook_gen.cpp

namespace {
std::mutex g_m;
std::string g_gameDir, g_modsDir;
bool g_enabled = false;     // on unless mods/towers.off
bool g_armed = false;       // team select was opened from the Towers button
bool g_inGame = false;      // the current match is a Towers game
struct State { int team = 0, rung = 0, wins = 0, losses = 0; std::vector<int> order; } g_st;
std::map<int, std::string> g_names, g_abbr, g_tdan;
int g_userSide = 0;   // side your controller is on in the current Towers game (0 home, 1 away)
 std::map<int, int> g_ovr; std::map<int, uint32_t> g_col;
int g_shownRung = -1; bool g_shown = false; std::string g_note;   // ladder screen
std::atomic<bool> g_flow{false};      // the Towers screens / automatic presses are running
std::atomic<bool> g_matchSet{false};  // the match setup took this game's teams (stops the automatic presses)
bool g_preShown = false;
bool g_continue = false;
std::atomic<int> g_diff{-1};             // the skill level the settings screen last set (SetDifficulty; 3 = All-Madden)
std::string g_menuItem;               // the last highlighted menu item (MenuItemChanged): PLAYNOW_TEAMSELECT = Towers
bool g_skipTower = false;             // the tower was just shown after a game: the Towers flow goes straight on              // a Towers game just ended: the next game started from it is a Towers game too (until the main menu)              // the tower was already shown before the match (not again while it loads)
uint32_t g_resPtr[3] = {0, 0, 0}; bool g_resPending = false;

bool Exists(const std::string& p) { if (FILE* f = fopen(p.c_str(), "rb")) { fclose(f); return true; } return false; }
int g_slot = 0;                       // save slot in use (mods/towers/slot1..3.txt; mods/towers/active.txt)
constexpr int kSlots = 3;
std::string Dir() { return g_modsDir + "/towers"; }
std::string SlotPath(int n) { return Dir() + "/slot" + std::to_string(n + 1) + ".txt"; }
std::string StatePath() { return SlotPath(g_slot); }
void MkDir(const std::string& d) {
#ifdef _WIN32
    CreateDirectoryA(d.c_str(), nullptr);
#endif
}

std::string Name(int id);
void Save() {
    MkDir(Dir());
    if (FILE* f = fopen(StatePath().c_str(), "wb")) {
        fprintf(f, "team=%d\nrung=%d\nwins=%d\nlosses=%d\norder=", g_st.team, g_st.rung, g_st.wins, g_st.losses);
        for (size_t i = 0; i < g_st.order.size(); i++) fprintf(f, "%s%d", i ? "," : "", g_st.order[i]);
        fputs("\n", f);
        // names for the launcher's Towers card (read only by the launcher)
        fprintf(f, "name=%s\n", Name(g_st.team).c_str());
        if (g_col.count(g_st.team)) fprintf(f, "color=%06X\n", (unsigned)(g_col[g_st.team] & 0xFFFFFF));
        if (g_st.rung < (int)g_st.order.size()) fprintf(f, "next=%s\n", Name(g_st.order[g_st.rung]).c_str());
        fclose(f);
    }
}
bool ReadState(const std::string& path, State& st);
void Load() { g_st = State(); ReadState(StatePath(), g_st); }
void SaveActive() { MkDir(Dir()); if (FILE* f = fopen((Dir() + "/active.txt").c_str(), "wb")) { fprintf(f, "%d\n", g_slot + 1); fclose(f); } }
bool ReadState(const std::string& path, State& g_st) {
    g_st = State();
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    char b[1024];
    while (fgets(b, sizeof b, f)) {
        std::string l = b; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t e = l.find('='); if (e == std::string::npos) continue;
        std::string k = l.substr(0, e), v = l.substr(e + 1);
        if (k == "team") g_st.team = atoi(v.c_str()); else if (k == "rung") g_st.rung = atoi(v.c_str());
        else if (k == "wins") g_st.wins = atoi(v.c_str()); else if (k == "losses") g_st.losses = atoi(v.c_str());
        else if (k == "order") { size_t p = 0; while (p < v.size()) { size_t c = v.find(',', p); g_st.order.push_back(atoi(v.substr(p, c - p).c_str())); if (c == std::string::npos) break; p = c + 1; } }
    }
    fclose(f);
    return g_st.team != 0;
}
// Team names and overalls from the roster the game is using (a roster set, the mods build, or the game's own).
void LoadTeams() {
    if (!g_names.empty()) return;
    std::string path;
    if (FILE* f = fopen((g_modsDir + "/roster.txt").c_str(), "rb")) {
        char b[256] = {}; if (fgets(b, sizeof b, f)) { std::string s = b; while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty() && Exists(g_modsDir + "/rosters/" + s + "/qkl_boot.ast")) path = g_modsDir + "/rosters/" + s + "/qkl_boot.ast"; }
        fclose(f);
    }
    if (path.empty() && Exists(g_modsDir + "/active/qkl_boot.ast")) path = g_modsDir + "/active/qkl_boot.ast";
    if (path.empty()) path = g_gameDir + "/qkl_boot.ast";
    for (uint32_t e = 347; e <= 383; e++) {
        std::vector<uint8_t> raw; if (!ReadOriginalEntry(path, e, raw)) continue;
        gdb::Db db; if (!db.Load(raw)) continue;
        gdb::Table* T = db.T("TEAM"); if (!T || !T->used) continue;
        int id = (int)db.Get(*T, 0, "TGID"); if (id < 1 || id > 32) continue;
        g_names[id] = db.Str(*T, 0, "TDNA"); g_ovr[id] = (int)db.Get(*T, 0, "TROV"); g_abbr[id] = db.Str(*T, 0, "TSNA"); g_tdan[id] = db.Str(*T, 0, "TDAN");
        g_col[id] = (db.Get(*T, 0, "TBCR") & 255) << 16 | (db.Get(*T, 0, "TBCG") & 255) << 8 | (db.Get(*T, 0, "TBCB") & 255);
    }
    fprintf(stderr, "[towers] %zu teams from %s\n", g_names.size(), path.c_str());
}
std::string Name(int id) { auto it = g_names.find(id); return it == g_names.end() ? "team " + std::to_string(id) : it->second; }
void StartClimb(int team) {
    LoadTeams();
    g_st = State(); g_st.team = team;
    for (auto& [id, o] : g_ovr) if (id != team) g_st.order.push_back(id);
    std::stable_sort(g_st.order.begin(), g_st.order.end(), [](int a, int b) { return g_ovr[a] != g_ovr[b] ? g_ovr[a] < g_ovr[b] : a < b; });
    Save();
    fprintf(stderr, "[towers] new climb with the %s: %zu teams to beat, first up the %s\n", Name(team).c_str(), g_st.order.size(), g_st.order.empty() ? "?" : Name(g_st.order[0]).c_str());
}
bool Done() { return g_st.team && g_st.rung >= (int)g_st.order.size(); }
}   // namespace

void TowersInit(const std::string& gameDir, const std::string& modsDir) {
    std::lock_guard<std::mutex> l(g_m);
    g_gameDir = gameDir; g_modsDir = modsDir; g_enabled = !Exists(modsDir + "/towers.off");
    extern int g_towersHookLinked;   // towers_hook.cpp (menu item / skill level): linked from the runtime library
    if (!*(volatile int*)&g_towersHookLinked) fprintf(stderr, "[towers] hooks missing\n");
    if (!g_enabled) return;
    MkDir(Dir());
    if (FILE* a = fopen((Dir() + "/active.txt").c_str(), "rb")) { int n = 1; if (fscanf(a, "%d", &n) == 1 && n >= 1 && n <= kSlots) g_slot = n - 1; fclose(a); }
    if (Exists(modsDir + "/towers_state.txt") && !Exists(SlotPath(0))) {   // the climb from before save slots: slot 1
        rename((modsDir + "/towers_state.txt").c_str(), SlotPath(0).c_str()); g_slot = 0; SaveActive();
        fprintf(stderr, "[towers] your climb is now in save slot 1\n");
    }
    Load();
    if (g_st.team) { LoadTeams(); fprintf(stderr, "[towers] climb in progress: %s, game %d of %zu\n", Name(g_st.team).c_str(), g_st.rung + 1, g_st.order.size()); }
}

extern std::atomic<uint32_t> g_padInject, g_padInjectUntil, g_padInjectPorts, g_padMuteMask, g_padMuteRelease;   // input.cpp
TowerTeam TT(int id) { TowerTeam t; t.name = Name(id); t.abbr = g_abbr[id]; t.ovr = g_ovr[id]; if (g_col.count(id) && g_col[id]) t.color = g_col[id]; return t; }
TowerView ViewNow() {
    TowerView v; v.me = TT(g_st.team);
    for (int id : g_st.order) v.rungs.push_back(TT(id));
    v.rung = g_st.rung; v.fromRung = g_shownRung < 0 ? g_st.rung : std::min(g_shownRung, g_st.rung);
    v.wins = g_st.wins; v.losses = g_st.losses; v.note = g_note;
    g_shownRung = g_st.rung; g_note.clear();
    for (int i = 0; i < kSlots; i++) {
        TowerSlot sl; State st;
        if (i == g_slot ? (st = g_st, g_st.team != 0) : ReadState(SlotPath(i), st)) {
            sl.used = true; sl.team = Name(st.team); sl.wins = st.wins; sl.losses = st.losses; sl.rung = st.rung; sl.total = (int)st.order.size();
            sl.done = st.rung >= (int)st.order.size();
        }
        v.slots.push_back(sl);
    }
    v.slot = g_slot;
    return v;
}
int ShowTowerLadder(const TowerView&, int*, bool);
// Pick your team for a new tower in the current slot (true if one was picked).
bool PickNewTeam(std::unique_lock<std::mutex>& l, int* port) {
    std::vector<int> ids; for (auto& [id, n] : g_names) ids.push_back(id);
    std::sort(ids.begin(), ids.end(), [](int a, int b) { return g_names[a] < g_names[b]; });
    std::vector<TowerTeam> ts; for (int id : ids) ts.push_back(TT(id));
    l.unlock(); int k = ShowTeamPick(ts, port); l.lock();
    if (k < 0 || k >= (int)ids.size()) return false;
    StartClimb(ids[k]); g_shownRung = -1; return true;
}
// The tower, with its Saves panel: loading a slot or starting a new tower shows the tower again.
int TowerWithSaves(std::unique_lock<std::mutex>& l, int* port, bool allowBack) {
    for (int guard = 0; guard < 20; guard++) {
        if (!g_st.team && !PickNewTeam(l, port)) return kTowerBack;
        TowerView v = ViewNow();
        l.unlock(); int r = ShowTowerLadder(v, port, allowBack); l.lock();
        if (r >= kTowerNew) { g_slot = r - kTowerNew; SaveActive(); g_st = State(); remove(StatePath().c_str()); fprintf(stderr, "[towers] new tower in slot %d\n", g_slot + 1); continue; }
        if (r >= kTowerLoad) { g_slot = r - kTowerLoad; SaveActive(); Load(); g_shownRung = -1; fprintf(stderr, "[towers] loaded slot %d: %s\n", g_slot + 1, g_st.team ? Name(g_st.team).c_str() : "empty"); continue; }
        return r;
    }
    return kTowerBack;
}

// Towers pressed: our screens over the game's team select (its input muted), then the game is pressed through team
// select (A) and the settings screen (A) - the match record then gets your team and the tower's opponent.
void Flow() {
    g_padMuteMask = 0xF;
    int port = 0;
    {
        std::unique_lock<std::mutex> l(g_m);
        LoadTeams();
        if (!g_skipTower) TowerWithSaves(l, &port, false);
        if (g_st.team) g_preShown = true;
        g_skipTower = false;
    }
    g_padMuteMask = 0; g_padMuteRelease = 0xF;   // the press that closed our screen does not reach the game
    if (Done() || !g_st.team) { g_flow = false; return; }
    // the game's team select, then its settings screen: A on your controller until the match setup has the teams
    g_matchSet = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    auto press = [&](uint32_t b) { g_padInjectPorts = 1u << (port & 3); g_padInject = b; g_padInjectUntil = uint32_t(GetTickCount() + 130); std::this_thread::sleep_for(std::chrono::milliseconds(700)); };
    // team select starts on arcade controls: Y once = standard controls (mods/towers_arcade.on keeps arcade)
    if (!Exists(g_modsDir + "/towers_arcade.on")) { press(0x8000); fprintf(stderr, "[towers] pressed Y on team select (standard controls)\n"); }
    // A on team select -> the settings screen (Skill Level row selected): Right until the game reports All-Madden (3)
    press(0x1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    if (!g_matchSet) {
        g_diff = -1;
        for (int i = 0; i < 4 && g_diff != 3; i++) { press(0x0008); if (g_diff < 0 && i == 0) { fprintf(stderr, "[towers] skill level did not answer - left as it is\n"); break; } }
        fprintf(stderr, "[towers] skill level now %d (3 = All-Madden)\n", int(g_diff));
    }
    for (int i = 0; i < 10 && !g_matchSet; i++) {
        g_padInjectPorts = 1u << (port & 3); g_padInject = 0x1000; g_padInjectUntil = uint32_t(GetTickCount() + 130);
        std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    }
    g_padInject = 0;
    fprintf(stderr, "[towers] automatic presses done (%s)\n", g_matchSet ? "match set up" : "match did not start - press A yourself");
    g_flow = false;
}

// The settings screen set the skill level (towers_hook.cpp).
void TowersDifficultySet(int value) { g_diff = value; fprintf(stderr, "[towers] skill level set: %d\n", value); }
// The highlighted menu item changed (towers_hook.cpp).
void TowersMenuItem(const std::string& item) {
    std::lock_guard<std::mutex> l(g_m);
    if (item != g_menuItem) { static int n = 0; if (n++ < 300) fprintf(stderr, "[towers] menu item: %s\n", item.c_str()); }
    g_menuItem = item;
}
// Team select opened (the game reads FNIG.GPTY then, every time): Towers if the Towers item was the one highlighted.
void TowersTeamSelectOpened() {
    std::string item; { std::lock_guard<std::mutex> l(g_m); item = g_menuItem; }
    bool towers = item.find("PLAYNOW_TEAMSELECT") != std::string::npos || item.find("Towers") != std::string::npos;
    fprintf(stderr, "[towers] team select opened (menu item '%s') - %s\n", item.c_str(), towers ? "Towers" : "not Towers");
    void TowersTeamSelect(bool);
    TowersTeamSelect(towers);
}
// Team select opened: from the Towers button (true) or Play Now / another mode (false).
void TowersTeamSelect(bool towers) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_enabled) return;
    if (g_flow) return;   // (our own presses on the game's team select)
    if (!towers && g_continue) { towers = true; fprintf(stderr, "[towers] next game after a Towers game - still Towers\n"); }
    g_armed = towers; g_inGame = false;
    if (towers) { g_flow = true; g_preShown = false; std::thread(Flow).detach(); }
    if (towers) {
        if (!g_st.team) fprintf(stderr, "[towers] Towers: pick YOUR team on the left (home) side - you keep it for the whole climb\n");
        else if (Done()) fprintf(stderr, "[towers] Towers: climb complete with the %s (%d wins) - delete towers_state.txt to start again\n", Name(g_st.team).c_str(), g_st.wins);
        else fprintf(stderr, "[towers] Towers: %s vs %s (game %d of %zu)\n", Name(g_st.team).c_str(), Name(g_st.order[g_st.rung]).c_str(), g_st.rung + 1, g_st.order.size());
    }
}

// Every database query the game runs (mod_hooks.cpp): sets the teams of a Towers game and reads its result.
void TowersQuery(const std::string& t, size_t nargs, const std::function<uint32_t(size_t)>& get, const std::function<void(size_t, uint32_t)>& set) {
    if (!g_enabled) return;
    std::lock_guard<std::mutex> l(g_m);
    if (g_resPending) {   // the final score, read by the previous query
        g_resPending = false;
        bool ok = true; uint32_t v[3];
        for (int i = 0; i < 3; i++) { if (g_resPtr[i] < 0x10000 || g_resPtr[i] >= 0xE0000000u) ok = false; else v[i] = ReadU32(g_resPtr[i]); }
        if (ok && g_inGame) {
            int mine = int(g_userSide == 0 ? v[1] : v[2]), theirs = int(g_userSide == 0 ? v[2] : v[1]);
            int home = mine, away = theirs;   // (home/away below: you / them)
            int opp = g_st.order[g_st.rung];
            fprintf(stderr, "[towers] final: %s %d, %s %d (you were %s; end flag %d)\n", Name(g_st.team).c_str(), mine, Name(opp).c_str(), theirs, g_userSide ? "away" : "home", int(v[0]));
            char nb[160];
            snprintf(nb, sizeof nb, "%s %d-%d %s the %s", home > away ? "WIN!" : "LOSS", home, away, home > away ? "over" : "to", Name(opp).c_str());
            g_note = nb;
            if (home > away) {
                g_st.wins++; g_st.rung++;
                if (Done()) fprintf(stderr, "[towers] TOWERS COMPLETE - the %s beat all %zu teams!\n", Name(g_st.team).c_str(), g_st.order.size());
                else fprintf(stderr, "[towers] win! next up: the %s (game %d of %zu)\n", Name(g_st.order[g_st.rung]).c_str(), g_st.rung + 1, g_st.order.size());
            } else { g_st.losses++; fprintf(stderr, "[towers] no win - play the %s again\n", Name(opp).c_str()); }
            Save();
            g_inGame = false; g_continue = true;
        }
    }
    if (nargs >= 3 && g_inGame && t.find("select 'ELQG' into ? and 'CSHG' into ? and 'CSAG' into ?") == 0) {
        for (int i = 0; i < 3; i++) g_resPtr[i] = get(size_t(i));
        g_resPending = true;
    }
}

// The match setup has read its two teams (mod_hooks.cpp ModMatchTeams, sub_882E30B0): in a Towers game your team goes
// on the side your controller picked and the tower's next opponent on the other. rec = the match record (team ids at
// +192 home / +200 away, the controller profile of each side at +300 / +304); stack = the setup's own copies of the ids
// (+124 / +128) and the teams' roster file names (+176 / +208, 32 bytes each).
void TowersMatchTeams(uint32_t rec, uint32_t stack) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_enabled && !g_armed && g_continue) { g_armed = true; g_preShown = false; fprintf(stderr, "[towers] play-again from a Towers game - next tower opponent\n"); }
    if (!g_enabled || !g_armed) return;
    uint32_t user[2] = {ReadU32(rec + 300), ReadU32(rec + 304)};
    auto human = [](uint32_t u) { return u != 0xFFFFFFFFu && u != 0; };
    g_userSide = human(user[1]) && !human(user[0]) ? 1 : 0;
    uint32_t picked[2] = {ReadU32(rec + 192), ReadU32(rec + 200)};
    fprintf(stderr, "[towers] match record: home %u away %u, controllers %08X %08X -> you are %s\n", picked[0], picked[1], user[0], user[1], g_userSide ? "away" : "home");
    if (!g_st.team) StartClimb(int(picked[g_userSide]));
    if (Done()) return;
    LoadTeams();
    int team[2]; team[g_userSide] = g_st.team; team[1 - g_userSide] = g_st.order[g_st.rung];
    for (int s = 0; s < 2; s++) {
        WriteU32(rec + 192 + 8 * s, uint32_t(team[s]));
        WriteU32(stack + 124 + 4 * s, uint32_t(team[s]));
        std::string fn = g_tdan[team[s]]; uint32_t dst = stack + 176 + 32 * s;
        if (!fn.empty()) { for (size_t i = 0; i < 31; i++) g_base[dst + i] = i < fn.size() ? uint8_t(fn[i]) : 0; g_base[dst + 31] = 0; }
    }
    g_inGame = true; g_shown = g_preShown; g_matchSet = true;
    fprintf(stderr, "[towers] game %d of %zu: %s (you) vs %s\n", g_st.rung + 1, g_st.order.size(), Name(g_st.team).c_str(), Name(g_st.order[g_st.rung]).c_str());
}

// The ladder screen for a Towers game that is loading (once per game): false if this is not one.
bool TowersLadder(TowerView& v) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_enabled || !g_inGame || g_shown || !g_st.team) return false;
    g_shown = true; LoadTeams();
    v = ViewNow();
    return true;
}

// The main menu came up: games from here on are Towers games only through the Towers button.
// After a Towers game the main menu is not where you land: the tower comes up (A: next game - the game is pressed to
// the Towers button and on; B: stay at the main menu).
void AfterGameTower() {
    g_padMuteMask = 0xF;
    int port = 0; bool play;
    std::this_thread::sleep_for(std::chrono::milliseconds(800));   // the main menu finishes coming up
    { std::unique_lock<std::mutex> l(g_m); LoadTeams(); play = TowerWithSaves(l, &port, true) == kTowerPlay && g_st.team && !Done(); }
    g_padMuteMask = 0; g_padMuteRelease = 0xF;
    if (!play) { g_flow = false; fprintf(stderr, "[towers] back to the main menu\n"); return; }
    { std::lock_guard<std::mutex> l(g_m); g_skipTower = true; }
    g_flow = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    auto press = [&](uint32_t b) { g_padInjectPorts = 1u << (port & 3); g_padInject = b; g_padInjectUntil = uint32_t(GetTickCount() + 130); std::this_thread::sleep_for(std::chrono::milliseconds(600)); };
    press(0x0002);   // down: Play Now -> Towers
    press(0x1000);   // A
    fprintf(stderr, "[towers] next game: pressed Towers on the main menu\n");
}
void TowersMainMenu() {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_continue) return;
    g_continue = false;
    if (!g_enabled || !g_st.team || g_flow) return;
    fprintf(stderr, "[towers] back at the main menu after a Towers game - showing the tower\n");
    g_flow = true; std::thread(AfterGameTower).detach();
}
// A match is being set up without team select (e.g. play again from the end-of-game screen): a Towers game if the
// last game was one.
bool TowersContinueArm() { std::lock_guard<std::mutex> l(g_m); if (g_enabled && g_continue && !g_armed) { g_armed = true; g_preShown = false; return true; } return false; }

// For the window title: where the climb stands.
std::string TowersStatus() {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_enabled || !g_armed) return "";
    if (!g_st.team) return "Towers: pick your team (left side)";
    if (Done()) return "Towers: COMPLETE with the " + Name(g_st.team);
    return "Towers: game " + std::to_string(g_st.rung + 1) + " of " + std::to_string(g_st.order.size()) + " - " + Name(g_st.team) + " vs " + Name(g_st.order[g_st.rung]);
}

