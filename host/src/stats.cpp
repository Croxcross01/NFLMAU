// Game stats: at the final whistle of every game (Play Now and Towers) each player's offence is read with the game's
// own database queries and added to mods/stats/games.txt, which the launcher's Stats page reads.
//
// The match database keeps PGOF (each player's offence this game; queried as 'FOGP', fields reversed: 'ayag' = gaya
// passing yards ...), PLGA (the players in the game) and GINF (the game). The players' ids come from the roster the
// game is using (its PLAY table: id, team, name, position); for the two teams' players the game is asked
//   select 'ayag' into ? and 'taag' into ? ... from 'FOGP' where 'DIGP' = ?
//
// games.txt, one game after another:
//   G <unix time> <mode: playnow|towers> <slot> <climb> <home id> <home score> <away id> <away score> <your team id or 0> <home name>|<away name>
//   P <team id> <pass yds> <att> <comp> <pass td> <int> <rush yds> <rush att> <rush td> <rec yds> <rec> <rec td> <pos> <first> <last>
#ifdef _WIN32
#include <windows.h>
#endif
#include "gamedb.h"
#include "host.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

bool TowersCurrent(int& slot, long long& climb, int& team, int& opp, int& rung);   // towers.cpp
std::string TowersTeamName(int id);
uint32_t ModGuestQuery(const std::string& text, std::initializer_list<uint32_t> args);   // mod_hooks.cpp
uint32_t ModGuestAlloc(uint32_t n);
bool ReadOriginalEntry(const std::string& path, uint32_t entry, std::vector<uint8_t>& out);   // playbook_gen.cpp

namespace {
std::mutex g_m;
std::string g_gameDir, g_modsDir;
bool g_pending = false, g_done = false;
uint32_t g_res[3] = {};
thread_local bool t_busy = false;   // our own queries pass through the query hook too

struct Pl { uint32_t id; int team, pos; std::string first, last; };
std::map<int, std::vector<Pl>> g_players;   // team id -> players (from the roster the game uses)

bool Exists(const std::string& p) { if (FILE* f = fopen(p.c_str(), "rb")) { fclose(f); return true; } return false; }
void LoadPlayers() {
    if (!g_players.empty()) return;
    std::string path;
    if (FILE* f = fopen((g_modsDir + "/roster.txt").c_str(), "rb")) {
        char b[256] = {}; if (fgets(b, sizeof b, f)) { std::string s = b; while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty() && Exists(g_modsDir + "/rosters/" + s + "/qkl_boot.ast")) path = g_modsDir + "/rosters/" + s + "/qkl_boot.ast"; }
        fclose(f);
    }
    if (path.empty() && Exists(g_modsDir + "/active/qkl_boot.ast")) path = g_modsDir + "/active/qkl_boot.ast";
    if (path.empty()) path = g_gameDir + "/qkl_boot.ast";
    std::vector<uint8_t> raw; gdb::Db db;
    if (!ReadOriginalEntry(path, 70, raw) || !db.Load(raw)) { fprintf(stderr, "[stats] could not read the players from %s\n", path.c_str()); return; }
    gdb::Table* P = db.T("PLAY"); if (!P) return;
    for (int i = 0; i < P->used; i++) {
        Pl p{db.Get(*P, i, "PGID"), (int)db.Get(*P, i, "TGID"), (int)db.Get(*P, i, "PPOS"), db.Str(*P, i, "PFNA"), db.Str(*P, i, "PLNA")};
        if (p.team >= 1 && p.team <= 40) g_players[p.team].push_back(p);
    }
    fprintf(stderr, "[stats] %zu teams of players from %s\n", g_players.size(), path.c_str());
}

void Capture(int homeScore, int awayScore) {
    LoadPlayers();
    uint32_t buf = ModGuestAlloc(64);
    // the two teams (GINF: GHTG home, GATG away)
    WriteU32(buf, 0); WriteU32(buf + 4, 0);
    ModGuestQuery("select 'GTHG' into \x85 and 'GTAG' into \x85 from 'FNIG'\n", {buf, buf + 4});
    int homeId = (int)ReadU32(buf), awayId = (int)ReadU32(buf + 4);
    if (homeId < 1 || homeId > 40 || awayId < 1 || awayId > 40) { fprintf(stderr, "[stats] could not read the game's teams (%d, %d)\n", homeId, awayId); return; }
    static const char* kF[] = {"ayag", "taag", "ccag", "dtag", "niag", "ayug", "taug", "dtug", "aycg", "accg", "dtcg"};
    std::string q = "select ";
    for (int k = 0; k < 11; k++) q += std::string(k ? " and '" : "'") + kF[k] + "' into \x82";
    q += " from 'FOGP' where 'DIGP' = \x85\n";
    struct Row { const Pl* p; int v[11]; };
    std::vector<Row> rows;
    for (int team : {homeId, awayId}) {
        auto it = g_players.find(team); if (it == g_players.end()) continue;
        for (auto& p : it->second) {
            for (int k = 0; k < 11; k++) WriteU32(buf + 4 * k, 0);
            ModGuestQuery(q, {buf, buf + 4, buf + 8, buf + 12, buf + 16, buf + 20, buf + 24, buf + 28, buf + 32, buf + 36, buf + 40, p.id});
            Row r{&p, {}}; bool any = false;
            for (int k = 0; k < 11; k++) { r.v[k] = (int)ReadU32(buf + 4 * k); any |= r.v[k] != 0; }
            if (any) rows.push_back(r);
        }
    }
    int slot = 0, team = 0, opp = 0, rung = 0; long long climb = 0;
    bool towers = TowersCurrent(slot, climb, team, opp, rung);
    std::string dir = g_modsDir + "/stats";
#ifdef _WIN32
    CreateDirectoryA(dir.c_str(), nullptr);
#endif
    FILE* f = fopen((dir + "/games.txt").c_str(), "ab");
    if (!f) return;
    fprintf(f, "G %lld %s %d %lld %d %d %d %d %d %s|%s\n", (long long)time(nullptr), towers ? "towers" : "playnow", slot, climb,
            homeId, homeScore, awayId, awayScore, towers ? team : 0, TowersTeamName(homeId).c_str(), TowersTeamName(awayId).c_str());
    for (auto& r : rows) {
        fprintf(f, "P %d", r.p->team);
        for (int k = 0; k < 11; k++) fprintf(f, " %d", r.v[k]);
        fprintf(f, " %d %s %s\n", r.p->pos, r.p->first.empty() ? "-" : r.p->first.c_str(), r.p->last.empty() ? "-" : r.p->last.c_str());
        fprintf(stderr, "[stats] %s %s: pass %d yds (%d/%d, %d td, %d int), rush %d yds (%d, %d td), catch %d yds (%d, %d td)\n",
                r.p->first.c_str(), r.p->last.c_str(), r.v[0], r.v[2], r.v[1], r.v[3], r.v[4], r.v[5], r.v[6], r.v[7], r.v[8], r.v[9], r.v[10]);
    }
    fclose(f);
    fprintf(stderr, "[stats] game saved: %s %d, %s %d (%s, %zu players with stats)\n", TowersTeamName(homeId).c_str(), homeScore, TowersTeamName(awayId).c_str(), awayScore, towers ? "Towers" : "Play Now", rows.size());
}
}   // namespace

void StatsInit(const std::string& gameDir, const std::string& modsDir) { std::lock_guard<std::mutex> l(g_m); g_gameDir = gameDir; g_modsDir = modsDir; }
void StatsMatchStart() { std::lock_guard<std::mutex> l(g_m); g_done = false; g_pending = false; }

// Every database query (mod_hooks.cpp, before the Towers hook): the final score is read with
// select 'ELQG' into ? and 'CSHG' into ? and 'CSAG' into ? ... and is in place by the next query.
void StatsQuery(const std::string& t, size_t nargs, const std::function<uint32_t(size_t)>& get) {
    if (t_busy) return;
    std::lock_guard<std::mutex> l(g_m);
    if (g_modsDir.empty()) return;
    if (g_pending) {
        g_pending = false;
        bool ok = true;
        for (int i = 0; i < 3; i++) if (g_res[i] < 0x10000 || g_res[i] >= 0xE0000000u) ok = false;
        if (ok && !g_done) { g_done = true; t_busy = true; Capture((int)ReadU32(g_res[1]), (int)ReadU32(g_res[2])); t_busy = false; }
    }
    if (nargs >= 3 && t.find("select 'ELQG' into ? and 'CSHG' into ? and 'CSAG' into ?") == 0) {
        for (int i = 0; i < 3; i++) g_res[i] = get(size_t(i));
        g_pending = true;
    }
}
