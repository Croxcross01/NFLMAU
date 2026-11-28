// Game stats: at the final whistle of every game (Play Now and Towers) the players' stats are copied out of the game's
// own match database and added to mods/stats/games.txt, which the launcher's Stats page reads.
//
// The match database is the game's in-game TDB (qkl_boot.ast entry 74 is its empty copy): PGOF holds each player's
// offence for this game, PLGA the players in the game (name, team, position), GINF the game (teams and score). The
// game keeps it in memory in the same layout as the file, so it is found by its "DB" header in the game's memory.
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
#include <map>
#include <mutex>
#include <string>
#include <vector>

bool TowersCurrent(int& slot, long long& climb, int& team, int& opp, int& rung);   // towers.cpp
std::string TowersTeamName(int id);

namespace {
std::mutex g_m;
std::string g_modsDir;
bool g_pending = false, g_done = false;
uint32_t g_res[3] = {};

// every committed, readable stretch of the game's memory
std::vector<std::pair<uint32_t, uint32_t>> Regions() {
    std::vector<std::pair<uint32_t, uint32_t>> v;
#ifdef _WIN32
    uint64_t a = 0x10000;
    while (a < 0x100000000ull) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g_base + a, &mi, sizeof mi)) break;
        uint64_t start = (uint8_t*)mi.BaseAddress - g_base, end = start + mi.RegionSize;
        bool ok = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        if (ok && end > start) v.push_back({(uint32_t)start, (uint32_t)std::min<uint64_t>(end, 0xFFFFFFFFull)});
        if (end <= a) break;
        a = end;
    }
#endif
    return v;
}

struct Found { gdb::Db db; uint32_t at = 0; int score = 0; };

// The match databases in memory: every "DB" header whose tables include PGOF, PLGA and GINF.
std::vector<Found> FindMatchDbs(int home, int away) {
    std::vector<Found> out;
    for (auto [s, e] : Regions()) {
        for (uint32_t p = s; p + 64 < e; p += 4) {
            const uint8_t* q = g_base + p;
            if (q[0] != 'D' || q[1] != 'B' || q[2] != 0 || q[3] != 8) continue;
            uint32_t n = gdb::BE32(q + 16); if (n < 3 || n > 64) continue;
            bool has = false;
            for (uint32_t i = 0; i < n && 24 + i * 8 + 4 <= e - p; i++) if (!memcmp(q + 24 + i * 8, "FOGP", 4)) has = true;
            if (!has) continue;
            size_t len = std::min<uint64_t>((uint64_t)e - p, 16u << 20);
            Found f; f.at = p;
            if (!f.db.Load(std::vector<uint8_t>(q, q + len))) continue;
            gdb::Table *O = f.db.T("PGOF"), *G = f.db.T("GINF"), *L = f.db.T("PLGA");
            if (!O || !G || !L || !G->used) continue;
            int hs = (int)f.db.Get(*G, 0, "GHSC"), as = (int)f.db.Get(*G, 0, "GASC");
            f.score = (O->used > 0) * 1 + (hs == home && as == away) * 2;
            fprintf(stderr, "[stats] match database at %08X: %u players with stats, score %d-%d\n", p, O->used, hs, as);
            out.push_back(std::move(f));
        }
    }
    return out;
}

void Capture(int homeScore, int awayScore) {
    auto dbs = FindMatchDbs(homeScore, awayScore);
    Found* best = nullptr;
    for (auto& f : dbs) if (!best || f.score > best->score) best = &f;
    if (!best || best->score < 1) { fprintf(stderr, "[stats] no player stats found for this game (%zu databases looked at)\n", dbs.size()); return; }
    gdb::Db& db = best->db;
    gdb::Table &O = *db.T("PGOF"), &G = *db.T("GINF"), &L = *db.T("PLGA");
    int homeId = (int)db.Get(G, 0, "GHTG"), awayId = (int)db.Get(G, 0, "GATG");
    std::map<uint32_t, int> row;   // player id -> PLGA row
    for (int i = 0; i < L.used; i++) row[db.Get(L, i, "PGID")] = i;
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
    static const char* kF[] = {"gaya", "gaat", "gacc", "gatd", "gain", "guya", "guat", "gutd", "gcya", "gcca", "gctd"};
    int logged = 0;
    for (int i = 0; i < O.used; i++) {
        uint32_t id = db.Get(O, i, "PGID");
        uint32_t v[11]; bool any = false;
        for (int k = 0; k < 11; k++) { v[k] = db.Get(O, i, kF[k]); any |= v[k] != 0; }
        if (!any) continue;
        auto it = row.find(id);
        std::string first = it != row.end() ? db.Str(L, it->second, "PFNA") : "?", last = it != row.end() ? db.Str(L, it->second, "PLNA") : "?";
        int tid = it != row.end() ? (int)db.Get(L, it->second, "TGID") : 0, pos = it != row.end() ? (int)db.Get(L, it->second, "PPOS") : -1;
        // yards can be negative: stored with an offset? log the raw numbers of the first few players to check
        fprintf(f, "P %d", tid);
        for (int k = 0; k < 11; k++) fprintf(f, " %d", (int)v[k]);
        fprintf(f, " %d %s %s\n", pos, first.empty() ? "-" : first.c_str(), last.empty() ? "-" : last.c_str());
        if (logged++ < 6) {
            std::string raw; char b[48];
            for (auto& fl : O.fields) { snprintf(b, sizeof b, " %s=%u", fl.name.c_str(), db.Get(O, i, fl.name.c_str())); raw += b; }
            fprintf(stderr, "[stats] %s %s (team %d):%s\n", first.c_str(), last.c_str(), tid, raw.c_str());
        }
    }
    fclose(f);
    fprintf(stderr, "[stats] game saved: %s %d, %s %d (%s)\n", TowersTeamName(homeId).c_str(), homeScore, TowersTeamName(awayId).c_str(), awayScore, towers ? "Towers" : "Play Now");
}
}   // namespace

void StatsInit(const std::string& modsDir) { std::lock_guard<std::mutex> l(g_m); g_modsDir = modsDir; }
void StatsMatchStart() { std::lock_guard<std::mutex> l(g_m); g_done = false; g_pending = false; }

// Every database query (mod_hooks.cpp, before the Towers hook): the final score is read with
// select 'ELQG' into ? and 'CSHG' into ? and 'CSAG' into ? ... and is in place by the next query.
void StatsQuery(const std::string& t, size_t nargs, const std::function<uint32_t(size_t)>& get) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_modsDir.empty()) return;
    if (g_pending) {
        g_pending = false;
        bool ok = true;
        for (int i = 0; i < 3; i++) if (g_res[i] < 0x10000 || g_res[i] >= 0xE0000000u) ok = false;
        if (ok && !g_done) { g_done = true; Capture((int)ReadU32(g_res[1]), (int)ReadU32(g_res[2])); }
    }
    if (nargs >= 3 && t.find("select 'ELQG' into ? and 'CSHG' into ? and 'CSAG' into ?") == 0) {
        for (int i = 0; i < 3; i++) g_res[i] = get(size_t(i));
        g_pending = true;
    }
}
