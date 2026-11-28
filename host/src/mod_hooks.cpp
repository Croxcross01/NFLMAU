#include <cmath>
#include <filesystem>
#include <string>
std::string FsGameDir();
bool GeneratePlaybookList(const std::string& gameDir, const std::string& outPath);
// Mod support hooks, called from the recompiled game code (midasm hooks in the recompiler config).
//
// ModArtTeamRemap (at 0x886A95EC, start of the uniform art lookup sub_886A95E0):
//   The game fills a uniform descriptor by querying TEAM (TPFN = art prefix) and TUNI (UANM = uniform asset name) with
//   the descriptor's team id (offset 8). Teams added by mods have ids outside the 32 NFL teams and no uniform rows, so
//   the asset name came back empty ("_Helmet") and the match crashed building the uniform. Teams listed in
//   mods/teams.ini ("<team id>=<art team id>", e.g. 1021=13) use the art of the given team instead.
#include "kernel.h"
#include "lineup_ui.h"
#include "towers_ui.h"
#include "playcall_ui.h"
#include <thread>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <atomic>
#include <cstdlib>
std::string PlaybookEditsStamp();   // playbook_mod.cpp
bool OnlineMode();   // xam.cpp
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {
// detailed mod diagnostics (every team query, renumbering, uniform request): set MADDEN_MOD_DEBUG=1
bool ModDebug() {
    static const bool on = [] {
        const char* v = getenv("MADDEN_MOD_DEBUG");
        if (v && *v && *v != '0') return true;
        const char* dir = getenv("MADDEN_MODS_ROOT");
        FILE* f = fopen((std::string(dir ? dir : "mods") + "/debug.on").c_str(), "rb");   // or a mods/debug.on file
        if (f) { fclose(f); return true; }
        return false;
    }();
    return on;
}
std::map<uint32_t, uint32_t> g_artMap;
std::map<uint32_t, std::string> g_prefixMap;   // team id -> own uniform prefix (teams.ini "100=13,HOF")
uint32_t g_matchHome = 0, g_matchAway = 0, g_matchSeq = 0;
uint32_t g_dcTeams[2] = {1023, 1023};   // match team ids of the last match depth chart copy   // team ids of the match being set up (ModMatchTeams)
// MADDEN_ART_PERSIST=1 restores the old behaviour (descriptor rewritten to the art team)
// Online games: both PCs must play by the same rules (players per side, the extra defensive back, play-action fix,
// the lineup window), so the host's choices are used on both - the launchers pass them on when the two connect
// (mods/online_rules.txt: "key=value" lines). Empty = not an online game or not set: use this PC's own setting.
std::string OnlineRule(const char* key) {
    if (!OnlineMode()) return "";
    static std::map<std::string, std::string> rules = [] {
        std::map<std::string, std::string> r; char b[256];
        if (FILE* f = fopen("mods/online_rules.txt", "rb")) {
            while (fgets(b, sizeof b, f)) {
                std::string t = b; while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
                size_t e = t.find('='); if (e != std::string::npos) r[t.substr(0, e)] = t.substr(e + 1);
            }
            fclose(f);
        }
        for (auto& [k, v] : r) fprintf(stderr, "[mods] online rules (from the host): %s = %s\n", k.c_str(), v.c_str());
        return r; }();
    auto it = rules.find(key);
    return it == rules.end() ? "" : it->second;
}
static int PaFixRule() { std::string r = OnlineRule("pa_fix"); return r.empty() ? -1 : r == "off" ? 0 : 1; }
bool g_artPersist = [] { const char* v = getenv("MADDEN_ART_PERSIST"); return v && *v == '1'; }();
std::mutex g_loadLock;
bool g_loaded = false;

void LoadTeamMap() {
    const char* dir = getenv("MADDEN_MODS_ROOT");
    std::string path = std::string(dir ? dir : "mods") + "/teams.ini";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        unsigned id = 0, art = 0; char pfx[8] = {0};
        int n = sscanf(line, " %u = %u , %7[A-Za-z0-9]", &id, &art, pfx);
        if (n >= 2 && id && art) g_artMap[id] = art;
        if (n == 3 && id && strlen(pfx) == 3) g_prefixMap[id] = pfx;
    }
    fclose(f);
    for (auto& [id, art] : g_artMap) fprintf(stderr, "[mods] team %u uses the uniform art of team %u\n", id, art);
    for (auto& [id, p] : g_prefixMap) fprintf(stderr, "[mods] team %u has its own uniforms (%s_MixAndMatch)\n", id, p.c_str());
}
}   // namespace

static std::string StackChain(uint32_t sp) {
    std::string chain; char buf[16];
    for (int i = 0; i < 14 && sp >= 0x10000 && sp < 0xE0000000u; i++) {
        uint32_t next = ReadU32(sp);
        if (next <= sp || next - sp > 0x100000) break;
        snprintf(buf, sizeof buf, " %08X", ReadU32(next - 8)); chain += buf;
        sp = next;
    }
    return chain;
}


void ModArtTeamRemap(PPCRegister& r1, PPCRegister& r4, PPCRegister& r29) {
    {   // load mods/teams.ini on first use
        std::lock_guard<std::mutex> l(g_loadLock);
        if (!g_loaded) { g_loaded = true; LoadTeamMap(); }
    }
    if (g_artMap.empty()) return;
    uint32_t desc = r4.u32;
    if (desc < 0x40000000u || desc >= 0xE0000000u) return;
    uint32_t id = ReadU32(desc + 8);
    if (ModDebug()) {   // log every uniform request (once per team id and slot) to see which ids reach the art code
        static std::mutex lm; static std::set<uint64_t> seen;
        std::lock_guard<std::mutex> l(lm);
        uint64_t key = (uint64_t(id) << 8) | g_base[desc + 24];
        if (seen.insert(key).second && seen.size() < 64)
            fprintf(stderr, "[mods] uniform request: team %u slot %u flag %u chain:%s\n", id, g_base[desc + 24],
                    g_base[desc + 33], StackChain(r1.u32).c_str());
    }
    auto it = g_artMap.find(id);
    if (it == g_artMap.end()) return;
    // Only this lookup uses the art team (r29 = the id the game already loaded for its queries). The descriptor keeps
    // the team's own id: rewriting it made the uniform setup see a changed team and build each mod team's uniforms a
    // second time (requests for 13 / 29 after 100 / 101); with two mod teams in one match that left the players
    // without models (invisible players).
    if (!g_artPersist) { if (r29.u32 == id) r29.u64 = it->second; }
    else { WriteU32(desc + 8, it->second); if (r29.u32 == id) r29.u64 = it->second; }
    static std::mutex m; static std::set<uint32_t> logged;
    std::lock_guard<std::mutex> l(m);
    if (logged.insert(id).second) fprintf(stderr, "[mods] uniform art: team %u -> team %u\n", id, it->second);
}

// Own uniforms for mod teams. The game names a team's uniform parts through its uniform prefix (TEAM.TPFN, e.g. "EAG"):
// "<prefix>_MixAndMatch" lists the bundles of every helmet / jersey / pants / socks. A team with a prefix in teams.ini
// ("100=13,HOF") gets that prefix right after each of the three places that read it from the database:
//   0x886A965C  uniform art lookup (sub_886A95E0): prefix stored in the uniform descriptor at +38 (team id at +8)
//   0x886A9D1C  sub_886A9CB8: prefix read into r1+80 for team r30, then "<prefix>_MixAndMatch" is built
//   0x886BE48C  uniform setup (sub_886BE778 area): prefix read into r30 (= descriptor+38) for team r29
static std::string it_lower(std::string v) { for (auto& c : v) c = (char)tolower((unsigned char)c); return v; }
static void ApplyPrefix(uint32_t team, uint32_t buf, const char* where) {
    {
        std::lock_guard<std::mutex> l(g_loadLock);
        if (!g_loaded) { g_loaded = true; LoadTeamMap(); }
    }
    if (ModDebug()) {
        static std::mutex dm; static std::set<std::string> seen;
        std::lock_guard<std::mutex> l(dm);
        char b[160];
        snprintf(b, sizeof b, "%s team %u prefix '%.3s'", where, team, buf >= 0x10000 && buf < 0xE0000000u ? (const char*)(g_base + buf) : "?");
        if (seen.size() < 200 && seen.insert(b).second) fprintf(stderr, "[mods] uniform prefix: %s\n", b);
    }
    auto it = g_prefixMap.find(team);
    if (it == g_prefixMap.end() || buf < 0x10000 || buf >= 0xE0000000u) return;
    memcpy(g_base + buf, it->second.c_str(), it->second.size() + 1);
    static std::mutex m; static std::set<std::string> logged;
    std::lock_guard<std::mutex> l(m);
    if (logged.insert(std::string(where) + std::to_string(team)).second)
        fprintf(stderr, "[mods] team %u: own uniforms %s_* (%s)\n", team, it->second.c_str(), where);
}
void ModPrefixUniform(PPCRegister& r31) { ApplyPrefix(ReadU32(r31.u32 + 8), r31.u32 + 38, "uniform lookup"); }
void ModPrefixMixList(PPCRegister& r1, PPCRegister& r30) { ApplyPrefix(r30.u32, r1.u32 + 80, "mix-and-match list"); }
void ModPrefixSetup(PPCRegister& r29, PPCRegister& r30) { ApplyPrefix(r29.u32, r30.u32, "uniform setup"); }

// Debug: when the window closes, every guest thread's call chain (to see where a hang waits).
std::vector<std::pair<uint32_t, PPCContext*>> GuestThreadContexts();
uint32_t GuestThreadKthread(PPCContext* c);
void DumpGuestThreads() {
    if (!ModDebug()) return;
    for (auto& [id, c] : GuestThreadContexts()) {
        uint32_t r3 = c->r3.u32, r31 = c->r31.u32;
        auto words = [](uint32_t a) {
            std::string w; if (a < 0x10000 || a >= 0xE0000000u) return w;
            char b[16]; for (int i = 0; i < 8; i++) { snprintf(b, sizeof b, " %08X", ReadU32(a + 4 * i)); w += b; } return w; };
        fprintf(stderr, "[hang] thread %u (kthread %08X): lr=%08X r1=%08X r3=%08X r31=%08X chain:%s\n", id, GuestThreadKthread(c),
                (uint32_t)c->lr, c->r1.u32, r3, r31, StackChain(c->r1.u32).c_str());
        fprintf(stderr, "[hang]   r3 ->%s | r31 ->%s\n", words(r3).c_str(), words(r31).c_str());
    }
    fflush(stderr);
}

// Debug: which swappable image (library, item) the UI draws (sub_88D6CD50, image object: +4 library, +6 item).
// Logged once per pair, so the log shows how each screen numbers its team pictures.
void ModSwapImage(PPCRegister& r3) {
    if (!ModDebug() || r3.u32 < 0x10000 || r3.u32 >= 0xE0000000u) return;
    uint16_t lib = (uint16_t)((g_base[r3.u32 + 4] << 8) | g_base[r3.u32 + 5]);
    uint16_t item = (uint16_t)((g_base[r3.u32 + 6] << 8) | g_base[r3.u32 + 7]);
    static std::mutex m; static std::set<uint32_t> seen;
    std::lock_guard<std::mutex> l(m);
    if (seen.size() >= 400 || !seen.insert((uint32_t(lib) << 16) | item).second) return;
    fprintf(stderr, "[mods] ui image: library %u item %u (match %u)\n", lib, item, g_matchSeq);
}

// ModDbQuery (at 0x88DCBAE0, the database query function: r6 = query text, r7.. = arguments in order).
//   Query text marks each argument with a byte >= 0x80 ("select 'X' into <arg> ... where 'DIGT' = <arg>"), tables are
//   written reversed ('MAET' = TEAM). For a mod team, queries on art/presentation tables (uniform rows, colours,
//   logos, stadium setup, coaches...) have no rows of their own, so the team id argument is replaced by the team whose
//   art it uses. Identity and roster tables (TEAM, PLAY, DCHT, INJY) keep the team's own id. Every query that carries a
//   mod team id is logged once, so new gaps show up in the log.
// STAD fields (name as written in queries = reversed, kind: 0 string, 1 int, 2 float)
static const struct { const char* q; int kind; } kStadFields[] = {
    {"DIKP", 1},
    {"DOCA", 0},
    {"LUCS", 1},
    {"NOLS", 2},
    {"PIZS", 1},
    {"TALS", 2},
    {"TRTS", 2},
    {"DIUS", 1},
    {"EUCS", 1},
    {"LLCS", 1},
    {"LMCS", 1},
    {"cfTS", 1},
    {"clTS", 1},
    {"cltS", 1},
    {"frTS", 1},
    {"irTS", 1},
    {"lcTS", 1},
    {"llTS", 1},
    {"lrTS", 1},
    {"rRTS", 1},
    {"rcts", 1},
    {"rlTS", 1},
    {"rrTS", 1},
    {"AATS", 0},
    {"ANDS", 0},
    {"BCTS", 1},
    {"BETS", 1},
    {"BJTS", 1},
    {"BSCS", 1},
    {"BSFS", 1},
    {"BSGS", 1},
    {"BSTS", 1},
    {"CITS", 1},
    {"CZES", 1},
    {"DIYC", 1},
    {"ESTS", 1},
    {"FOTS", 1},
    {"GLIS", 1},
    {"GSCS", 1},
    {"GSFS", 1},
    {"GSGS", 1},
    {"ILTS", 1},
    {"LETS", 1},
    {"LMTS", 1},
    {"MANS", 0},
    {"NSES", 1},
    {"NSFS", 0},
    {"NTDS", 0},
    {"PACS", 1},
    {"RSCS", 1},
    {"RSFS", 1},
    {"RSGS", 1},
    {"SSES", 1},
    {"TSTS", 1},
    {"VETS", 1},
    {"WSTS", 1},
    {"1ISS", 1},
    {"2ISS", 1},
    {"3ISS", 1},
    {"BBFS", 1},
    {"BLCS", 1},
    {"BLES", 1},
    {"BLWS", 1},
    {"BPES", 1},
    {"CSCS", 1},
    {"CSFS", 1},
    {"CSGS", 1},
    {"CWES", 1},
    {"DISS", 1},
    {"DLND", 1},
    {"DROS", 1},
    {"DWES", 1},
    {"EWFS", 1},
    {"IPWS", 1},
    {"IROS", 1},
    {"ITFS", 1},
    {"LYSS", 1},
    {"ODES", 1},
    {"ODWS", 1},
    {"ORYS", 1},
    {"PYTS", 1},
    {"RPTS", 1},
    {"RTLS", 1},
    {"RTRS", 1},
    {"SCCS", 1},
    {"STIS", 1},
    {"TARS", 1},
    {"TOIS", 1},
    {"TRGS", 1},
    {"YRTS", 1},
    {"YTFS", 1},
    {"YTKS", 1},
    {"bbFS", 1},
    {"cISS", 1},
    {"fTUS", 1},
    {"ggTS", 1},
    {"tfSS", 1},
    {"txTS", 1},
};

// Own stadiums, added at run time. Rows added to the stadium table in the game file make "Play Now" wait forever, so
// the file keeps its 32 stadiums and the rows are inserted into the loaded database through the game's own query
// calls, copied from a template stadium with a new id, names and asset stem. mods/stadiums.ini:
//   <new id>=<template id>,<asset stem>,<short name>,<stadium name>,<order>
//   63=12,hofhof,Hall Of Fame,Hall of Fame Stadium,36
struct ModStadium { uint32_t id, tmpl, order; std::string stem, shortName, name, tmplStem; };
static std::vector<ModStadium> g_stadiums;
static bool g_stadiumsLoaded = false;
static thread_local bool t_inStadiumAdd = false;
static std::set<std::string> g_stadiumOwnNames;   // names our own stadium bundles provide (mods/stadium_names.txt)
static void LoadStadiums() {
    g_stadiumsLoaded = true;
    {
        const char* dir = getenv("MADDEN_MODS_ROOT");
        if (FILE* nf = fopen((std::string(dir ? dir : "mods") + "/stadium_names.txt").c_str(), "rb")) {
            char ln[128];
            while (fgets(ln, sizeof ln, nf)) {
                if (ln[0] == '#') continue;
                std::string v(ln); while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
                for (auto& c : v) c = (char)tolower((unsigned char)c);
                if (!v.empty()) g_stadiumOwnNames.insert(v);
            }
            fclose(nf);
        }
    }
    const char* dir = getenv("MADDEN_MODS_ROOT");
    FILE* f = fopen((std::string(dir ? dir : "mods") + "/stadiums.ini").c_str(), "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        unsigned id = 0, tmpl = 0, order = 0; char stem[33] = {0}, sh[41] = {0}, nm[41] = {0};
        if (sscanf(line, " %u = %u , %32[^,] , %40[^,] , %40[^,] , %u", &id, &tmpl, stem, sh, nm, &order) == 6)
            g_stadiums.push_back({id, tmpl, order, stem, sh, nm});
    }
    fclose(f);
    for (auto& st : g_stadiums)
        fprintf(stderr, "[mods] stadium %u '%s' (asset %s) from stadium %u\n", st.id, st.name.c_str(), st.stem.c_str(), st.tmpl);
}
void StadiumAdd(PPCContext& live);
PPCContext* CurrentGuestContext();

void InstallLinesHook();
uint32_t GameStateWord();
void ModDbQuery(PPCRegister& r1, PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10) {
    InstallLinesHook();
    {
        std::lock_guard<std::mutex> l(g_loadLock);
        if (!g_loaded) { g_loaded = true; LoadTeamMap(); }
    }
    // (the match teams for the lineup window and the two-player sides are read below with or without mod teams;
    // only the mod-team remapping further down needs mods/teams.ini)
    uint32_t q = r6.u32;
    if (q < 0x80000000u || q >= 0xE0000000u) return;   // query strings live in the image / heap
    // read the query text (bounded) and find argument markers
    char text[400]; int n = 0;
    std::vector<int> argPos;   // index in text of each argument marker
    for (; n < 399; n++) {
        uint8_t c = g_base[q + n];
        if (!c) break;
        if (c >= 0x80) argPos.push_back(n);
        text[n] = c >= 0x80 ? '?' : (c == '\n' ? ' ' : char(c));
    }
    text[n] = 0;
    if (!g_stadiumsLoaded) LoadStadiums();
    if (!g_stadiums.empty() && !t_inStadiumAdd && strstr(text, "'DATS'")) {
        if (PPCContext* live = CurrentGuestContext()) StadiumAdd(*live);
    }
    if (argPos.empty()) return;
    PPCRegister* regs[4] = {&r7, &r8, &r9, &r10};
    // arguments 5+ are passed on the caller's stack (8-byte slots, value in the low word at r1+84, +92, ...)
    auto argGet = [&](size_t i) -> uint32_t { return i < 4 ? regs[i]->u32 : ReadU32(r1.u32 + 84 + 8 * uint32_t(i - 4)); };
    auto argSet = [&](size_t i, uint32_t v) { if (i < 4) regs[i]->u64 = v; else WriteU32(r1.u32 + 84 + 8 * uint32_t(i - 4), v); };
    size_t nargs = argPos.size() < 16 ? argPos.size() : 16;
    {   // Towers ladder: teams of a Towers game, its result
        void TowersQuery(const std::string&, size_t, const std::function<uint32_t(size_t)>&, const std::function<void(size_t, uint32_t)>&);
        void StatsQuery(const std::string&, size_t, const std::function<uint32_t(size_t)>&);
        StatsQuery(std::string(text), nargs, argGet);   // stats of every game (before Towers counts the result)
        TowersQuery(std::string(text), nargs, argGet, argSet);
        if (strncmp(text, "select 'YTPG' into ? from 'FNIG'", 32) == 0 && StackChain(r1.u32).compare(1, 8, "886191B0") == 0) { void TowersTeamSelectOpened(); TowersTeamSelectOpened(); }
    }
    if (ModDebug()) {   // diagnostics: team / match-setup queries with their arguments
        static std::mutex dm; static int dn = 0;
        std::string t(text);
        bool write = t.compare(0, 6, "update") == 0 || t.compare(0, 6, "insert") == 0;
        bool watch = t.compare(0, 14, "select 'DIOT' ") == 0 ||
                     (write && (t.find("'FNIG'") != std::string::npos || t.find("'MTTS'") != std::string::npos ||
                                t.find("'MAET'") != std::string::npos ||
                                (t.find("'YALP'") != std::string::npos && t.find("set 'DIGT'") != std::string::npos))) ||
                     t.find("from 'MTTS' where") != std::string::npos ||
                     t.find("'THCD'") != std::string::npos || t.find("'AGCD'") != std::string::npos ||   // depth charts
                     t.find("'UABP'") != std::string::npos || t.find("'MFBP'") != std::string::npos ||   // playbooks
                     (t.find("'YALP'") != std::string::npos && t.find("'NCSI'") == std::string::npos);   // players
        std::string ch = StackChain(r1.u32);
        if (ch.substr(0, 63).find("88BFECF4") != std::string::npos) watch = true;   // incl. the renumber calls inside   // the match team setup (sub_88BFE630): every query
        static uint32_t pbSeq = ~0u; static int pbN = 0;
        if (t.find("'UABP'") != std::string::npos || t.find("'MFBP'") != std::string::npos) {
            if (pbSeq != g_matchSeq) { pbSeq = g_matchSeq; pbN = 0; }
            if (pbN++ >= 20) watch = false;
        }
        // Towers probe (TestBuild, mods/towers.on): every write to the game's tables, to find where the match
        // teams and the final score are kept
        static const bool probe = [] { const char* d = getenv("MADDEN_MODS_ROOT"); FILE* f = fopen((std::string(d ? d : "mods") + "/towers_probe.on").c_str(), "rb"); if (f) { fclose(f); return true; } return false; }();   // mods/towers_probe.on
        if (probe && (write || t.find("'FNIG'") != std::string::npos || t.find("'MTTS'") != std::string::npos)) watch = true;
        // Towers probe: values the game reads from its game info table, logged when they change (score, quarter...);
        // the per-frame reads themselves are not logged (they filled the log before the game ended)
        if (probe) {
            static std::vector<std::pair<std::string, uint32_t>> pending; static std::map<std::string, uint32_t> last;
            std::lock_guard<std::mutex> l(dm);
            for (auto& [key, ptr] : pending) {
                if (ptr < 0x10000 || ptr >= 0xE0000000u) continue;
                uint32_t v = ReadU32(ptr);
                auto it = last.find(key);
                if (it == last.end() || it->second != v) { last[key] = v; static int vn = 0; if (vn++ < 20000) fprintf(stderr, "[towers] value %s = %d\n", key.c_str(), int(v)); }
            }
            pending.clear();
            if (t.compare(0, 6, "select") == 0 && (t.find("from 'FNIG'") != std::string::npos || t.find("from 'MAET'") != std::string::npos || t.find("from 'MTTS'") != std::string::npos)) {
                std::vector<std::string> fields; size_t k = 0;
                while ((k = t.find("'", k)) != std::string::npos && k < t.find(" from")) { size_t e = t.find("'", k + 1); if (e == std::string::npos) break; fields.push_back(t.substr(k + 1, e - k - 1)); k = e + 1; }
                size_t ninto = 0; for (size_t q = 0; (q = t.find("into ?", q)) != std::string::npos; q += 6) ninto++;
                std::string table = t.substr(t.find(" from ") + 6, 6);
                for (size_t i = 0; i < ninto && i < nargs; i++) pending.push_back({table + "." + (i < fields.size() ? fields[i] : "?" + std::to_string(i)), argGet(i)});
                if (t.find("select 'GTHG' into ? from 'FNIG'") == 0 || t.find("select 'GTAG' into ? from 'FNIG'") == 0) watch = false;
            }
        }
        if (watch) {
            std::lock_guard<std::mutex> l(dm);
            if (dn++ < (probe ? 40000 : 6000)) {
                fprintf(stderr, "[mods] team query:");
                for (size_t i = 0; i < nargs && i < 12; i++) fprintf(stderr, " %X", argGet(i));
                fprintf(stderr, " | %.150s | from%s\n", text, ch.substr(0, 36).c_str());
            }
        }
    }
    // Match depth chart: "insert into DCGA select * from DCHT where TGID = home or TGID = away" copies both teams'
    // depth charts for the match. Teams added by mods have no depth chart rows (rows cannot be added to the
    // databases); one such team is filled in automatically, but with two the match depth chart was empty and no
    // player was put on the field (invisible players, field players with no player id). The away side then copies
    // its look team's rows (other team id, ignored for the match) so the table is not empty.
    // "update MTTS set DIGU = <user> where UNLS = <side>": the profile of the user who picked that side (team select)
    if (nargs == 2 && std::string(text).find("update 'MTTS' set 'DIGU' = ? where ('UNLS' = ?)") != std::string::npos) {
        extern uint32_t g_sideUser[2]; uint32_t side = argGet(1);
        if (side < 2) g_sideUser[side] = argGet(0);
    }
    if (nargs == 2 && std::string(text).find("insert into 'AGCD' * select") != std::string::npos) {
        g_dcTeams[0] = argGet(0); g_dcTeams[1] = argGet(1);
    }
    if (false && nargs == 2 && g_artMap.count(g_matchHome) && g_artMap.count(g_matchAway) &&
        std::string(text).find("insert into 'AGCD'") != std::string::npos) {
        uint32_t look = g_artMap[g_matchAway];
        fprintf(stderr, "[mods] match depth chart: both teams are mod teams, adding team %u's rows (args %u %u)\n",
                look, argGet(0), argGet(1));
        argSet(1, look);
        return;
    }
    if (g_artMap.empty()) return;   // no mod teams: nothing to remap
    // any argument holding a mod team id?
    bool hit = false;
    for (size_t i = 0; i < nargs; i++) if (g_artMap.count(argGet(i))) hit = true;
    if (!hit) return;
    std::string s(text);
    size_t from = s.find("from '");
    std::string table = from != std::string::npos && from + 10 <= s.size() ? s.substr(from + 6, 4) : "?";
    std::string tableName(table.rbegin(), table.rend());
    size_t where = s.find("where");
    bool identity = tableName == "TEAM" || tableName == "PLAY" || tableName == "DCHT" || tableName == "INJY";
    // The uniform setup (sub_886BE778) asks the TEAM table for the team's art id: "select TOID, TSHO from TEAM where
    // TGID = ?". A mod team that is missing from the team table in use (Legends is not in the master list) got no row,
    // so the uniform id stayed at the "none" value 1023 and the helmet came out unnamed. That query uses the art team.
    if (tableName == "TEAM" && s.compare(0, 14, "select 'DIOT' ") == 0) identity = false;
    static std::mutex m; static std::set<std::string> logged;
    for (size_t i = 0; i < nargs; i++) {
        auto it = g_artMap.find(argGet(i));
        if (it == g_artMap.end() || where == std::string::npos || size_t(argPos[i]) < where) continue;   // only 'where' values
        // only a value compared with the team id column ('DIGT' = TGID reversed), never other numbers
        // ('DIOT' = TOID, the team's original id, used the same way)
        size_t col = s.rfind("'DIGT'", argPos[i]), col2 = s.rfind("'DIOT'", argPos[i]);
        if (col2 != std::string::npos && (col == std::string::npos || col2 > col)) col = col2;
        if (col == std::string::npos || argPos[i] - col > 12) continue;
        uint32_t from_id = argGet(i);
        if (!identity) argSet(i, it->second);
        std::lock_guard<std::mutex> l(m);
        std::string key = tableName + "/" + std::to_string(from_id) + (identity ? "/keep" : "");
        if (logged.insert(key).second)
            fprintf(stderr, "[mods] query %s team %u%s: %.200s\n", tableName.c_str(), from_id,
                    identity ? " (own id kept)" : (" -> " + std::to_string(it->second)).c_str(), text);
    }
    (void)r1;
}

// ModTextureGuard (at 0x88EBFBC0, in the material texture binding loop of sub_88EBFAEC).
//   For each of 16 texture slots the game takes the texture object and calls its vtable+44 method. Mod teams have
//   left a slot pointing at a texture object that was never created or was already freed (zero or garbage vtable),
//   which crashed the match right as the field appeared. A slot whose object is not valid is treated as empty (the
//   game's own path for an unused slot, 0x88EBFBAC), and the first few are logged with the guest call chain.
bool ModTextureGuard(PPCRegister& r1, PPCRegister& r3, PPCRegister& r24, PPCRegister& r30) {
    uint32_t obj = r3.u32;
    bool ok = false;
    uint32_t vt = 0, fn = 0;
    if (obj >= 0x40000000u && obj < 0xE0000000u && !(obj & 3)) {
        vt = ReadU32(obj);
        if (vt >= PPC_IMAGE_BASE && vt < PPC_IMAGE_BASE + PPC_IMAGE_SIZE && !(vt & 3)) {
            fn = ReadU32(vt + 44);
            ok = fn >= PPC_CODE_BASE && fn < PPC_CODE_BASE + PPC_CODE_SIZE && !(fn & 3) && PPC_LOOKUP_FUNC(g_base, fn) != nullptr;
        }
    }
    if (ok) return false;
    static std::mutex m; static int logged = 0;
    std::lock_guard<std::mutex> l(m);
    if (logged < 12) {
        ++logged;
        std::string chain;
        char buf[16];
        uint32_t sp = r1.u32;
        for (int i = 0; i < 14 && sp >= 0x10000 && sp < 0xE0000000u; i++) {
            uint32_t next = ReadU32(sp);
            if (next <= sp || next - sp > 0x100000) break;
            snprintf(buf, sizeof buf, " %08X", ReadU32(next - 8)); chain += buf;
            sp = next;
        }
        fprintf(stderr, "[mods] texture slot %u of set %08X has an invalid texture object %08X (vtable %08X, fn %08X); "
                "treated as empty. chain:%s\n", r30.u32, r24.u32, obj, vt, fn, chain.c_str());
    }
    return true;
}

// ModTeamSetupFail (at 0x88BFEC30, the error exit of the match team setup sub_88BFE630): logs the error code.
void ModTeamSetupFail(PPCRegister& r31) {
    static int n = 0;
    if (ModDebug() && n++ < 8) fprintf(stderr, "[mods] match team setup stopped with code %d\n", (int)r31.s32);
}

// ModTeamRenumber (at 0x88BEB850, renumbers a team id in a database: r4 = old id, r5 = new id, r6 = database name).
void ModTeamRenumber(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7) {
    static int n = 0;
    if (ModDebug() && n++ < 24) {
        uint32_t nm = r6.u32; char name[5] = {0};
        for (int i = 0; i < 4; i++) { char c = char(nm >> (24 - 8 * i)); name[i] = (c >= 32 && c < 127) ? c : '.'; }
        fprintf(stderr, "[mods] renumber team %u -> %u in db %08X '%s' (r3 %X r7 %u)\n", r4.u32, r5.u32, nm, name, r3.u32, r7.u32);
    }
}

// Reference-counted object release (sub_88310450, and the same code inlined at 0x886B277C in sub_886B24E0).
//   During matches the game sometimes releases a handle that holds 0x40000008 - the first block of the heap, which is
//   live data (counters), not an object. The release decremented its "reference count" and then called a destructor
//   through a garbage vtable: the mid-game crash (lr 886B27B8 / 8839410C). Releases of something that is not an
//   object (vtable outside the game image or destructor outside the code) are skipped and logged.
static bool IsGameObject(uint32_t obj) {
    if (obj < 0x40000000u || obj >= 0xE0000000u || (obj & 3)) return false;
    uint32_t vt = ReadU32(obj);
    if (vt < PPC_IMAGE_BASE || vt >= PPC_IMAGE_BASE + PPC_IMAGE_SIZE || (vt & 3)) return false;
    uint32_t fn = ReadU32(vt);
    return fn >= PPC_CODE_BASE && fn < PPC_CODE_BASE + PPC_CODE_SIZE && !(fn & 3) && PPC_LOOKUP_FUNC(g_base, fn) != nullptr;
}
static void LogBadRelease(const char* where, uint32_t obj, uint32_t sp) {
    static std::mutex m; static int n = 0;
    std::lock_guard<std::mutex> l(m);
    if (n++ < 10)
        fprintf(stderr, "[fix] skipped release of a non-object %08X (%s), chain:%s\n", obj, where, StackChain(sp).c_str());
}
bool GuardRelease(PPCRegister& r1, PPCRegister& r3) {   // sub_88310450 entry; true = return 0 without releasing
    if (IsGameObject(r3.u32)) return false;
    LogBadRelease("release", r3.u32, r1.u32);
    r3.u64 = 0;
    return true;
}
static void DumpObj(const char* label, uint32_t a) {
    if (a < 0x40000000u || a >= 0xE0000000u) { fprintf(stderr, "    %s %08X (not heap)\n", label, a); return; }
    fprintf(stderr, "    %s %08X:", label, a);
    for (int i = 0; i < 24; i++) fprintf(stderr, " %08X", ReadU32(a + 4 * i));
    uint32_t vt = ReadU32(a);
    if (vt >= PPC_IMAGE_BASE && vt < PPC_IMAGE_BASE + PPC_IMAGE_SIZE) {
        fprintf(stderr, "\n      vtable %08X:", vt);
        for (int i = 0; i < 28; i++) fprintf(stderr, " %08X", ReadU32(vt + 4 * i));
    }
    fprintf(stderr, "\n");
}
bool GuardReleaseInline(PPCRegister& r1, PPCRegister& r11, PPCRegister& r26, PPCRegister& r30, PPCRegister& r31) {
    if (r11.u32 == 0 || IsGameObject(r11.u32)) return false;   // 0x886B277C; true = skip to 0x886B27B8
    LogBadRelease("inline release", r11.u32, r1.u32);
    static int dumped = 0;
    if (ModDebug() && dumped++ < 3) {   // what the per-frame player code (sub_886B24E0) was working on
        DumpObj("this", r31.u32);
        DumpObj("entity A (+244)", r26.u32);
        DumpObj("entity B (+248)", r30.u32);
        uint32_t ent = r30.u32 ? r30.u32 : r26.u32;
        if (ent >= 0x40000000u && ent < 0xE0000000u) {   // the entity's visual component (entity+692, 192 bytes)
            uint32_t x = ReadU32(ent + 692);
            DumpObj("component (+692)", x);
            if (x >= 0x40000000u && x < 0xE0000000u) {
                fprintf(stderr, "    component bytes:");
                for (int i = 0; i < 48; i++) fprintf(stderr, " %08X", ReadU32(x + 4 * i));
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "    entity bytes 96..760:");
            for (int i = 24; i < 190; i++) fprintf(stderr, " %08X", ReadU32(ent + 4 * i));
            fprintf(stderr, "\n");
        }
        if (ent >= 0x40000000u && ent < 0xE0000000u) {
            for (int k = 0; k < 24; k++) {   // follow pointer fields of the entity one level
                uint32_t q = ReadU32(ent + 4 * k);
                if (q >= 0x40000000u && q < 0xE0000000u && !(q & 3)) { char b[32]; snprintf(b, sizeof b, "entity+%d", 4 * k); DumpObj(b, q); }
            }
        }
    }
    return true;
}

// ModMatchTeams (at 0x882E31BC in sub_882E30B0, after the game looked up both teams' roster file names): logs the two
// team ids of the match and their roster file names (TDAN), once per match setup.
void ModMatchTeams(PPCRegister& r1, PPCRegister& r31) {
    auto str = [](uint32_t a) { std::string o; for (int i = 0; i < 40; i++) { char c = (char)g_base[a + i]; if (!c) break; o += (c >= 32 && c < 127) ? c : '?'; } return o; };
    { void StatsMatchStart(); StatsMatchStart(); void TowersMatchTeams(uint32_t, uint32_t); TowersMatchTeams(r31.u32, r1.u32); }   // Towers: your team vs the tower's next opponent
    g_matchHome = ReadU32(r31.u32 + 192); g_matchAway = ReadU32(r31.u32 + 200); g_matchSeq++;
    { extern std::atomic<int64_t> g_matchLoadStart; g_matchLoadStart = std::chrono::steady_clock::now().time_since_epoch().count(); }
    fprintf(stderr, "[mods] match teams: home %u (%s), away %u (%s)\n", g_matchHome, str(r1.u32 + 176).c_str(),
            g_matchAway, str(r1.u32 + 208).c_str());
}

// ModNameHash (at 0x89027BC0, the resource name hash: FNV-1a 64 of the upper-cased name). Debug only: logs each
// distinct resource name the game looks up, in order, to see which asset a match is waiting for.
// Own stadiums. Hall Of Fame / Legends have their own rows in the stadium table (STAD, added by add_stadiums.py) whose
// asset name (STAA) is a same-length stem made from the team prefix: "hofhof" (Eagles' "eagles"), "leglegle"
// ("steelers"). Their field ("<stem>_field_big" with "<STEM>_FIELD" / "<STEM>_FIELD_GRASS") is our own bundle in
// qkl_stream.ast; every other asset of such a stadium (building, backdrop, crowd ...) is renamed in place back to the
// art team's stem, so those parts of the Eagles' / Steelers' stadium are borrowed. The Eagles' and Steelers' own
// stadiums are never changed.
static void StadiumRename(uint32_t a) {
    if (g_stadiums.empty()) return;
    char name[64] = {0};
    for (int i = 0; i < 63; i++) { char c = (char)g_base[a + i]; name[i] = c; if (!c) break; }
    size_t len = strlen(name);
    for (auto& st : g_stadiums) {
        size_t nl = st.stem.size();
        if (st.tmplStem.size() != nl || nl < 3 || len < nl) continue;
        bool match = true;
        for (size_t i = 0; i < nl && match; i++) match = tolower((unsigned char)name[i]) == tolower((unsigned char)st.stem[i]);
        if (!match) continue;
        std::string rest; for (size_t i = nl; i < len; i++) rest += (char)tolower((unsigned char)name[i]);
        if (rest == "_field_big" || rest == "_field" || rest == "_field_grass") return;   // our own field
        {   // names our own stadium bundles provide (building and its textures) stay ours
            std::string low; for (size_t i = 0; i < len; i++) low += (char)tolower((unsigned char)name[i]);
            if (g_stadiumOwnNames.count(low)) return;
        }
        for (size_t i = 0; i < nl; i++) {
            char c = (char)tolower((unsigned char)st.tmplStem[i]);
            g_base[a + i] = isupper((unsigned char)name[i]) ? (uint8_t)toupper((unsigned char)c) : (uint8_t)c;
        }
        if (ModDebug()) {
            static std::mutex m; static std::set<std::string> logged;
            std::lock_guard<std::mutex> l(m);
            if (logged.size() < 100 && logged.insert(name).second)
                fprintf(stderr, "[mods] stadium %u: %s uses the %s stadium\n", st.id, name, st.tmplStem.c_str());
        }
        return;
    }
}

void ModNameHash(PPCRegister& r3) {
    uint32_t a = r3.u32;
    if (a < 0x10000 || a >= 0xE0000000u) return;
    StadiumRename(a);
    std::string n;
    for (int i = 0; i < 160; i++) { char c = (char)g_base[a + i]; if (!c) break; n += (c >= 32 && c < 127) ? c : '?'; }
    {   // Towers: a main-menu button with a game mode (Play Now) makes the game look up GAMEMODE; Towers has none.
        // Seen before team select opens = Play Now, not seen = Towers.
        static bool modeSeen = false;
        void TowersTeamSelect(bool towers);
        if (n == "GAMEMODE") modeSeen = true;
        else if (n.size() > 18 && (n.find("madden_scrmainmenu") != std::string::npos || n.find("MADDEN_SCRMAINMENU") != std::string::npos)) { void TowersMainMenu(); TowersMainMenu(); }
        // (team select itself is now seen through its FNIG.GPTY read, every time it opens - see TowersQuery below)
    }
    if (!ModDebug()) return;
    static std::mutex m; static std::set<std::string> seen;
    std::lock_guard<std::mutex> l(m);
    bool screen = n.find("Strategy=") != std::string::npos || n.find("TeamSelect") != std::string::npos || n.find("TEAMSELECT") != std::string::npos;
    if (screen) { static int sn = 0; if (sn++ < 400) fprintf(stderr, "[name] (screen) %s\n", n.c_str()); return; }   // every time: which button opened team select
    if (seen.size() < 20000 && seen.insert(n).second) fprintf(stderr, "[name] %s\n", n.c_str());
}

// GuardSetModel (at 0x88394060, the visual component's "set model" method: stores r4 at component+16 with a
// reference). In Hall Of Fame vs Legends every player component got 0x40000008 - not an object - as its model, so no
// player was drawn. A non-object is stored as "no model" instead, and the callers are logged to find the source.
void GuardSetModel(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4) {
    uint32_t m = r4.u32;
    if (m == 0 || IsGameObject(m)) return;
    static std::mutex mx; static int n = 0;
    std::lock_guard<std::mutex> l(mx);
    if (n++ < 12) {
        fprintf(stderr, "[fix] model %08X for component %08X is not an object (stored as none), chain:%s\n", m, r3.u32,
                StackChain(r1.u32).c_str());
        if (ModDebug() && n <= 2) {
            fprintf(stderr, "    at %08X:", m & ~3u);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %08X", ReadU32((m & ~3u) + 4 * i));
            fprintf(stderr, "\n");
        }
    }
    r4.u64 = 0;
}

// Player model table diagnostics (debug): sub_886BAE98 builds each side's player models (team info r4: +0 side,
// +38 team id, +44 player count, player records of 364 bytes with the id at +52); sub_886B2E48 looks a player's id up
// in that table when giving it its model (0x886B2EA8 = not found).
void ModTeamModels(PPCRegister& r4) {
    if (!ModDebug()) return;
    uint32_t t = r4.u32;
    uint32_t n = ReadU32(t + 44);
    fprintf(stderr, "[mods] player models: side %u team %u, %u players, ids:", ReadU32(t), (uint32_t)(ReadU32(t + 36) & 0xFFFF), n);
    for (uint32_t k = 0; k < n && k < 60; k++) fprintf(stderr, " %X", ReadU32(t + 52 + k * 364));
    fprintf(stderr, "\n");
}
void ModModelLookupFail(PPCRegister& r8, PPCRegister& r31) {
    static int n = 0;
    if (n++ < 12) fprintf(stderr, "[fix] player id %X (side %u) has no model in the match's player model table\n", r8.u32,
                          (ReadU32(r31.u32 + 264) >> 8) & 0xFF);
}

// ModPlayerInfo (debug, at 0x886B2E48 entry: giving a field player entity (r4) its model). Dumps the entity's player
// info block (entity+932) for the first players of each match, to compare a working match with Hall Of Fame vs Legends.
void ModPlayerInfo(PPCRegister& r4) {
    if (!ModDebug()) return;
    static uint32_t lastHome = ~0u; static int n = 0;
    if (g_matchSeq != lastHome) { lastHome = g_matchSeq; n = 0; }
    if (n++ >= 4) return;
    uint32_t e = r4.u32, info = ReadU32(e + 932);
    fprintf(stderr, "[mods] field player %08X side %u info %08X:", e, (ReadU32(e + 264) >> 8) & 0xFF, info);
    if (info >= 0x40000000u && info < 0xE0000000u)
        for (int i = 0; i < 64; i++) fprintf(stderr, " %08X", ReadU32(info + 4 * i));
    fprintf(stderr, "\n");
}

// ModPlayerLoad (debug, at 0x886ACD30: player info block (r3) loads a player (r5 = player id) from the database).
void ModPlayerLoad(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    if (!ModDebug()) return;
    static uint32_t lastHome = ~0u; static int n = 0;
    if (g_matchSeq != lastHome) { lastHome = g_matchSeq; n = 0; }
    if (n++ >= 8) return;
    fprintf(stderr, "[mods] player load: info %08X id %X (r4 %08X) chain:%s\n", r3.u32, r5.u32 & 0xFFFF, r4.u32,
            StackChain(r1.u32).c_str());
}

// ModPlaybookState (debug, at 0x88535578: picks the side's play database; side struct = *(r3+4) + side*0x296B0,
// +32 offense playbook db, +36 defense playbook db, +40 current, +44 type). Logged once per side per match.
void InstallLinesHook();
void ModPlaybookState(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    InstallLinesHook();
    if (!ModDebug()) return;
    static uint32_t lastHome = ~0u; static int seen = 0;
    if (g_matchSeq != lastHome) { lastHome = g_matchSeq; seen = 0; }
    uint32_t side = r4.u32 & 1;
    if (seen & (1 << side)) return;
    seen |= 1 << side;
    uint32_t s = ReadU32(r3.u32 + 4) + r4.u32 * 0x296B0;
    fprintf(stderr, "[mods] playbooks side %u (mode %u): offense %08X defense %08X current %08X type %08X\n", r4.u32, r5.u32,
            ReadU32(s + 32), ReadU32(s + 36), ReadU32(s + 40), ReadU32(s + 44));
}

// ---------------------------------------------------------------------------------------------------------------
// Match depth charts for teams added by mods (ModBuildDepthCharts, at 0x88BF5400 = right after sub_88BF51D8 copied
// both teams' depth charts into the match depth chart table DCGA).
//   The on-field lineup is read from DCGA ("select * from DCGA where TGID = <team> order by ddep"). Mod teams have no
//   depth chart rows in the game's databases (rows cannot be added to them), so their DCGA part was empty: with two
//   mod teams no player was ever put on the field (empty player info blocks, invisible players). For every match
//   team with no DCGA rows a depth chart is built from the match player table PLGA: per position, players ordered by
//   overall rating (depth 0 = starter), inserted with the game's own database calls.
PPC_EXTERN_FUNC(sub_88DCBAE0);
PPCContext* CurrentGuestContext();
namespace {
uint32_t GuestString(const char* s) {
    size_t n = strlen(s) + 1;
    uint32_t a = GuestAlloc(n, 16);
    memcpy(g_base + a, s, n);
    return a;
}
// Calls the game's query function (sub_88DCBAE0) with a query string and its arguments (as the game does: r3..r5 = 0,
// r6 = query, r7..r10 = first four arguments, the rest in 8-byte slots at r1+84).
uint32_t GuestQuery(PPCContext& live, uint32_t q, std::initializer_list<uint32_t> args) {
    PPCContext c = live;
    c.r1.u64 = (live.r1.u32 - 0x1000) & ~15u;
    c.r3.u64 = 0; c.r4.u64 = 0; c.r5.u64 = 0; c.r6.u64 = q;
    PPCRegister* regs[4] = {&c.r7, &c.r8, &c.r9, &c.r10};
    size_t i = 0;
    for (uint32_t v : args) {
        if (i < 4) regs[i]->u64 = v;
        else { WriteU32(c.r1.u32 + 80 + 8 * uint32_t(i - 4), 0); WriteU32(c.r1.u32 + 84 + 8 * uint32_t(i - 4), v); }
        i++;
    }
    sub_88DCBAE0(c, g_base);
    return c.r3.u32;
}
}   // namespace

// The game's own query, run from the host while the game is inside its query code (stats.cpp): text uses the game's
// markers (\x85 a number / a pointer to one, \x88 a string). Query texts are kept (one copy each).
uint32_t ModGuestQuery(const std::string& text, std::initializer_list<uint32_t> args) {
    PPCContext* live = CurrentGuestContext(); if (!live) return 0xFFFFFFFFu;
    static std::map<std::string, uint32_t> qs;
    auto it = qs.find(text); uint32_t q = it != qs.end() ? it->second : (qs[text] = GuestString(text.c_str()));
    return GuestQuery(*live, q, args);
}
uint32_t ModGuestAlloc(uint32_t n) { return GuestAlloc(n, 16); }

void StadiumAdd(PPCContext& live) {
    if (g_stadiums.empty() || t_inStadiumAdd) return;
    t_inStadiumAdd = true;
    static uint32_t qCount = 0, qInsert, buf, num;
    if (!qCount) {
        qCount = GuestString("select count(*) into \x85 from 'DATS' where 'DIGS' = \x85\n");
        qInsert = GuestString("insert into 'DATS' set 'DIGS' = \x85\n");
        buf = GuestAlloc(128, 16); num = GuestAlloc(16, 16);
    }
    static std::map<std::string, uint32_t> qs;   // cached query strings
    auto Q = [&](const std::string& t) { auto it = qs.find(t); if (it != qs.end()) return it->second; return qs[t] = GuestString(t.c_str()); };
    for (auto& st : g_stadiums) {
        WriteU32(num, 0);
        GuestQuery(live, qCount, {num, st.id});
        if (ReadU32(num)) continue;
        WriteU32(num, 0);
        GuestQuery(live, qCount, {num, st.tmpl});
        if (!ReadU32(num)) continue;   // template not in this database
        uint32_t rc = GuestQuery(live, qInsert, {st.id});
        int copied = 0;
        for (auto& fd : kStadFields) {
            const char mk = fd.kind == 0 ? '\x88' : fd.kind == 2 ? '\x86' : '\x85';
            std::string f = fd.q;
            memset(g_base + buf, 0, 128);
            std::string sel = std::string("select '") + f + "' into " + mk + " from 'DATS' where 'DIGS' = \x85\n";
            std::string upd = std::string("update 'DATS' set '") + f + "' = " + mk + " where 'DIGS' = \x85\n";
            GuestQuery(live, Q(sel), {buf, st.tmpl});
            const char* over = nullptr; uint32_t iv = 0; bool ov = false;
            if (f == "AATS") {   // remember the template's asset stem: StadiumRename borrows its building
                st.tmplStem.assign((const char*)(g_base + buf), strnlen((const char*)(g_base + buf), 32));
                over = st.stem.c_str();
            }
            else if (f == "ANDS") over = st.shortName.c_str();
            else if (f == "MANS") over = st.name.c_str();
            else if (f == "DROS") { iv = st.order; ov = true; }
            if (over) { memset(g_base + buf, 0, 64); memcpy(g_base + buf, over, strlen(over)); }
            if (ov) WriteU32(buf, iv);
            uint32_t arg = fd.kind == 0 ? buf : (fd.kind == 2 ? ReadU32(buf) : ReadU32(buf));
            GuestQuery(live, Q(upd), {arg, st.id});
            copied++;
        }
        WriteU32(num, 0);
        GuestQuery(live, qCount, {num, st.id});
        fprintf(stderr, "[mods] stadium %u '%s' added to the loaded database (insert rc %u, %d fields copied, rows now %u)\n",
                st.id, st.name.c_str(), rc, copied, ReadU32(num));
    }
    t_inStadiumAdd = false;
}

PPC_EXTERN_FUNC(sub_8850E318);
// the lineup window's sides (see LineupPicker): team db ids of this match, and "team -> controller" memory
uint32_t g_lineupTeams[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
void SaveLineupSide(int side, int port) {
    if (side < 0 || side > 1 || port < 0 || port > 3 || g_lineupTeams[side] == 0xFFFFFFFFu) return;
    std::vector<std::string> keep;
    if (FILE* f = fopen("mods/lineup_sides.txt", "rb")) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char kind[16]; unsigned a, p;
            if (sscanf(line, "%15s %u %u", kind, &a, &p) != 3) continue;
            if (!strcmp(kind, "team") && a == g_lineupTeams[side]) continue;
            if (!strcmp(kind, "side") && (int)a == side) continue;
            keep.push_back(line);
        }
        fclose(f);
    }
    if (FILE* f = fopen("mods/lineup_sides.txt", "wb")) {
        for (auto& l : keep) fputs(l.c_str(), f);
        fprintf(f, "team %u %d\nside %d %d\n", g_lineupTeams[side], port, side, port);
        fclose(f);
    }
}
uint32_t g_sideUser[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
// which controller a user profile object belongs to: look in it for the sign-in our XAM gave that pad
// (XUID E000000000000001 + port, big-endian), else a pointer to it one level down
static int PortOfProfile(uint32_t obj) {
    auto ok = [](uint32_t a) {   // a readable guest range (committed host memory), never a fault
        bool GuestReadable(uint32_t a, uint32_t len);
        return a >= 0x10000 && a < 0xF0000000u && GuestReadable(a, 0x400);
    };
    auto scan = [&](uint32_t a, uint32_t len) -> int {
        if (!ok(a)) return -1;
        for (uint32_t o = 0; o + 8 <= len; o += 4) {
            uint32_t hi = __builtin_bswap32(*(uint32_t*)(g_base + a + o)), lo = __builtin_bswap32(*(uint32_t*)(g_base + a + o + 4));
            if (hi == 0xE0000000u && lo >= 1 && lo <= 4) return (int)lo - 1;
        }
        return -1;
    };
    if (!ok(obj)) return -1;
    int r = scan(obj, 0x400);
    if (r >= 0) return r;
    for (uint32_t o = 0; o < 0x100; o += 4) {
        uint32_t p = __builtin_bswap32(*(uint32_t*)(g_base + obj + o));
        if ((r = scan(p, 0x200)) >= 0) return r;
    }
    {   // log a bit of it to learn the layout
        fprintf(stderr, "[mods] profile %08X:", obj);
        for (uint32_t o = 0; o < 0x80; o += 4) fprintf(stderr, " %08X", __builtin_bswap32(*(uint32_t*)(g_base + obj + o)));
        fprintf(stderr, "\n");
    }
    return -1;
}
static void LineupPicker(PPCContext& live);
void LinesApplyAlignment(int mode, bool force);
int LinesGetMode();
void LinesSetMode(int mode);
// Lineup picker open: the game's loading-screen UI update (sub_88DDF730, ModUiUpdateGate) is paused meanwhile. On the
// 2nd match the UI job kept walking menu objects that were freed while the match setup was held by the picker.
static std::atomic<bool> g_pickerOpen{false};
bool ModUiUpdateGate() { return g_pickerOpen.load(std::memory_order_relaxed); }
void InstallLinesHook();
void LinesNewDown();
void ModBuildDepthCharts() {
    InstallLinesHook();
    PPCContext* live = CurrentGuestContext();
    if (!live) return;
    {   // Towers: the ladder screen while the match loads (the loading-screen UI is held meanwhile, as for the lineup picker)
        bool TowersLadder(TowerView&); TowerView tv;
        if (TowersLadder(tv)) { g_pickerOpen = true; ShowTowerLadder(tv); g_pickerOpen = false; }
    }
    struct Pick { PPCContext* l; ~Pick() { LineupPicker(*l); } } pick{live};   // after the build below (any return)
    if (g_artMap.empty()) return;
    static uint32_t qCount = 0, qPlayer, qInsert, out;
    if (!qCount) {
        qCount = GuestString("select count(*) into \x82 from 'AGCD' where 'DIGT' = \x82\n");
        qPlayer = GuestString("select 'DIGT' into \x82 and 'SOPP' into \x82 and 'RVOP' into \x82 from 'AGLP' where 'DIGP' = \x82\n");
        qInsert = GuestString("insert into 'AGCD' set 'DIGP' = \x82 and 'DIGT' = \x82 and 'SOPP' = \x82 and 'pedd' = \x82\n");
        out = GuestAlloc(64, 16);
    }
    uint32_t teams[2] = {g_dcTeams[0], g_dcTeams[1]};   // match team ids of the depth chart copy just made
    fprintf(stderr, "[mods] match depth chart check: teams %u %u\n", teams[0], teams[1]);
    struct P { uint32_t id, pos, ovr; };
    std::vector<P> players;
    for (uint32_t p = 0; p < 0x200; p++) {   // match player ids
        WriteU32(out, 0xFFFFFFFF); WriteU32(out + 4, 0); WriteU32(out + 8, 0);
        uint32_t rc = GuestQuery(*live, qPlayer, {out, out + 4, out + 8, p});
        if (rc != 0) continue;
        players.push_back({p, ReadU32(out + 4), ReadU32(out + 8)});
        players.back().id = p;
        players.back().pos = ReadU32(out + 4);
        // team is checked per side below
        WriteU32(out + 16 + 0, 0);
        players.back().ovr = ReadU32(out + 8) | (ReadU32(out) << 16);   // team id kept in the high half
    }
    for (int s = 0; s < 2; s++) {
        uint32_t t = teams[s];
        if (t >= 1023) continue;
        WriteU32(out + 32, 0xFFFFFFFF);
        uint32_t rc = GuestQuery(*live, qCount, {out + 32, t});
        uint32_t have = ReadU32(out + 32);
        if (rc != 0 || have != 0) {
            fprintf(stderr, "[mods] match depth chart: team %u has %u rows (rc %u)\n", t, have, rc);
            continue;
        }
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> byPos;   // pos -> (ovr, id)
        for (auto& p : players)
            if ((p.ovr >> 16) == t) byPos[p.pos].push_back({p.ovr & 0xFFFF, p.id});
        int rows = 0, fails = 0;
        for (auto& kv : byPos) {
            auto v = kv.second;
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
            for (size_t d = 0; d < v.size(); d++) {
                uint32_t r = GuestQuery(*live, qInsert, {v[d].second, t, kv.first, (uint32_t)d});
                if (r == 0) rows++; else fails++;
            }
        }
        fprintf(stderr, "[mods] match depth chart: built %d rows for team %u (%zu positions, %d failed, %zu match players)\n",
                rows, t, byPos.size(), fails, players.size());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Pre-game lineup picker (LineupPicker, run from ModBuildDepthCharts: the match depth charts DCGA are complete and
// the game is on its loading screen). Shows a window with both teams' starters (QB ... P, 25 slots) and lets the user
// pick a player for each; the choice is written back into DCGA: per position the picked players get depth 0..n-1
// and the rest of that position's depth chart follows in its old order. Positions above P (returners etc.) are left
// alone. Off with a mods/lineup_picker.off file.
std::atomic<int64_t> g_matchLoadStart{0}, g_pickerTime{0};   // match load timing (steady clock ticks)
static void LineupPicker(PPCContext& live) {
    struct PickTimer { int64_t t0 = std::chrono::steady_clock::now().time_since_epoch().count();
        ~PickTimer() { g_pickerTime += std::chrono::steady_clock::now().time_since_epoch().count() - t0; } } pickTimer;
    {
        std::string rule = OnlineRule("lineup");
        if (rule == "off") return;
        const char* dir = getenv("MADDEN_MODS_ROOT");
        FILE* f = rule.empty() ? fopen((std::string(dir ? dir : "mods") + "/lineup_picker.off").c_str(), "rb") : nullptr;
        if (f) { fclose(f); return; }
    }
    static uint32_t qPlayer = 0, qDepth, qInsert, out, fn, ln;
    if (!qPlayer) {
        qPlayer = GuestString("select 'DIGT' into \x82 and 'SOPP' into \x82 and 'RVOP' into \x82 and 'NEJP' into \x82 and 'ANFP' into \x88 and 'ANLP' into \x88 from 'AGLP' where 'DIGP' = \x82\n");
        qDepth = GuestString("select 'DIGP' into \x82 from 'AGCD' where ('DIGT' = \x82) and ('SOPP' = \x82) and ('pedd' = \x82)\n");
        qInsert = GuestString("insert into 'AGCD' set 'DIGP' = \x82 and 'DIGT' = \x82 and 'SOPP' = \x82 and 'pedd' = \x82\n");
        out = GuestAlloc(64, 16); fn = GuestAlloc(64, 16); ln = GuestAlloc(64, 16);
    }
    uint32_t teams[2] = {g_dcTeams[0], g_dcTeams[1]};
    if (teams[0] >= 1023 || teams[1] >= 1023) return;
    LineupTeam side[2];
    side[0].title = "Home team"; side[1].title = "Away team";
    {   // team names and colours for the window
        static uint32_t qTeam = 0, cbuf, nb1, nb2;
        if (!qTeam) {
            qTeam = GuestString("select 'ANLT' into \x88 and 'ANDT' into \x88 and 'RCBT' into \x82 and 'GCBT' into \x82 and 'BCBT' into \x82 and 'R2BT' into \x82 and 'G2BT' into \x82 and 'B2BT' into \x82 from 'MAET' where 'DIGT' = \x82\n");
            cbuf = GuestAlloc(32, 16); nb1 = GuestAlloc(160, 16); nb2 = GuestAlloc(160, 16);
        }
        auto str = [](uint32_t a) { std::string o; for (int i = 0; i < 60; i++) { char c = (char)g_base[a + i]; if (!c) break; o += (c >= 32 && c < 127) ? c : '?'; } return o; };
        uint32_t real[2] = {g_matchHome, g_matchAway};
        for (int s = 0; s < 2; s++)
            for (uint32_t id : {real[s], teams[s]}) {
                memset(g_base + nb1, 0, 160); memset(g_base + nb2, 0, 160); memset(g_base + cbuf, 0, 32);
                uint32_t c = cbuf;
                if (GuestQuery(live, qTeam, {nb1, nb2, c, c + 4, c + 8, c + 12, c + 16, c + 20, id}) != 0 || !g_base[nb2]) continue;
                side[s].city = str(nb1); side[s].nick = str(nb2);
                auto rgb = [&](uint32_t o) { return (std::min<uint32_t>(ReadU32(o), 255) << 16) | (std::min<uint32_t>(ReadU32(o + 4), 255) << 8) | std::min<uint32_t>(ReadU32(o + 8), 255); };
                side[s].color1 = rgb(c); side[s].color2 = rgb(c + 12);
                fprintf(stderr, "[mods] lineup picker: side %d = team %u %s %s, colours %06X %06X\n", s, id, side[s].city.c_str(),
                        side[s].nick.c_str(), side[s].color1, side[s].color2);
                break;
            }
    }
    for (uint32_t p = 0; p < 0x200; p++) {
        WriteU32(out, 0xFFFFFFFF); WriteU32(out + 4, 0); WriteU32(out + 8, 0); WriteU32(out + 12, 0);
        memset(g_base + fn, 0, 64); memset(g_base + ln, 0, 64);
        if (GuestQuery(live, qPlayer, {out, out + 4, out + 8, out + 12, fn, ln, p}) != 0) continue;
        uint32_t t = ReadU32(out);
        int s = t == teams[0] ? 0 : t == teams[1] ? 1 : -1;
        if (s < 0) continue;
        LineupPlayer pl;
        pl.id = p; pl.pos = ReadU32(out + 4); pl.ovr = ReadU32(out + 8); pl.number = ReadU32(out + 12);
        auto str = [](uint32_t a) { std::string o; for (int i = 0; i < 40; i++) { char c = (char)g_base[a + i]; if (!c) break; o += (c >= 32 && c < 127) ? c : '?'; } return o; };
        pl.name = str(fn) + " " + str(ln);
        side[s].players.push_back(pl);
    }
    // current depth chart per position (0..20)
    std::vector<uint32_t> depth[2][21];
    for (int s = 0; s < 2; s++)
        for (uint32_t pos = 0; pos < 21; pos++)
            for (uint32_t d = 0; d < 64; d++) {
                WriteU32(out + 32, 0xFFFFFFFF);
                if (GuestQuery(live, qDepth, {out + 32, teams[s], pos, d}) != 0 || ReadU32(out + 32) >= 0x200) break;
                depth[s][pos].push_back(ReadU32(out + 32));
            }
    for (int s = 0; s < 2; s++) {
        side[s].slots.assign(kLineupSlots, 0xFFFFFFFFu);
        int used[21] = {};
        for (int k = 0; k < kLineupSlots; k++) {
            uint32_t pos = kLineupSlotPos[k];
            if (used[pos] < (int)depth[s][pos].size()) side[s].slots[k] = depth[s][pos][used[pos]];
            used[pos]++;
        }
    }
    fprintf(stderr, "[mods] lineup picker: home team %u (%zu players), away team %u (%zu players)\n", teams[0],
            side[0].players.size(), teams[1], side[1].players.size());
    if (side[0].players.empty() && side[1].players.empty()) return;   // one side can be empty (team setup edge cases): still pick for the other
    std::vector<uint32_t> before[2] = {side[0].slots, side[1].slots};
    g_pickerOpen = true;
    int playersMode = LinesGetMode();
    // which controller plays which team: the game only knows once the match runs, so the sides are remembered from
    // matches (mods/lineup_sides.txt: "team <db id> <port>" lines, and "side <0/1> <port>" for the last match)
    int padTeam[4] = {-1, -1, -1, -1};
    {
        int byTeam[2] = {-1, -1}, bySide[2] = {-1, -1};
        if (FILE* f = fopen("mods/lineup_sides.txt", "rb")) {
            char kind[16]; unsigned a, port;
            while (fscanf(f, "%15s %u %u", kind, &a, &port) == 3) {
                if (!strcmp(kind, "team")) { for (int s2 = 0; s2 < 2; s2++) if (a == teams[s2] && port < 4) byTeam[s2] = (int)port; }
                else if (!strcmp(kind, "side") && a < 2 && port < 4) bySide[a] = (int)port;
            }
            fclose(f);
        }
        int use[2] = {byTeam[0], byTeam[1]};
        {   // best: the side's user profile (set at team select) holds that controller's sign-in (XUID / "Player N")
            extern uint32_t g_sideUser[2]; int found[2] = {-1, -1};
            for (int s2 = 0; s2 < 2; s2++) found[s2] = PortOfProfile(g_sideUser[s2]);
            fprintf(stderr, "[mods] lineup picker: side users %08X %08X -> controllers %d %d\n",
                    g_sideUser[0], g_sideUser[1], found[0] + 1, found[1] + 1);
            if (found[0] >= 0 && found[1] >= 0 && found[0] != found[1]) { byTeam[0] = use[0] = found[0]; byTeam[1] = use[1] = found[1]; }
        }
        if (use[0] < 0 && use[1] >= 0) use[0] = bySide[0] != use[1] ? bySide[0] : bySide[1];
        if (use[1] < 0 && use[0] >= 0) use[1] = bySide[1] != use[0] ? bySide[1] : bySide[0];
        if (use[0] < 0 && use[1] < 0) { use[0] = bySide[0]; use[1] = bySide[1]; }
        extern bool PadConnected(uint32_t user);
        if (use[0] >= 0 && use[1] >= 0 && use[0] != use[1] && PadConnected(use[0]) && PadConnected(use[1])) { padTeam[use[0]] = 0; padTeam[use[1]] = 1; }
        fprintf(stderr, "[mods] lineup picker: remembered sides: home team %u -> controller %d, away team %u -> controller %d\n",
                teams[0], use[0] + 1, teams[1], use[1] + 1);
        g_lineupTeams[0] = teams[0]; g_lineupTeams[1] = teams[1];
    }
    // Online: each player sets his own team's lineup; the picks are swapped through the online server so both games
    // start the match with the same lineups (else they would play different games).
    extern bool RelayActive(); extern bool RelaySyncSend(const std::string&, const std::string&);
    extern bool RelaySyncTake(const std::string&, std::string&, int);
    int mySide = -1;
    if (OnlineMode()) {
        extern uint32_t g_sideUser[2];
        int f0 = PortOfProfile(g_sideUser[0]), f1 = PortOfProfile(g_sideUser[1]);
        mySide = (f0 >= 0) != (f1 >= 0) ? (f0 >= 0 ? 0 : 1) : -1;
        fprintf(stderr, "[mods] lineup picker (online): this PC plays the %s team\n", mySide == 0 ? "home" : mySide == 1 ? "away" : "? (unknown)");
        if (mySide < 0 || !RelayActive()) {
            fprintf(stderr, "[mods] lineup picker (online): %s - lineups stay as they are\n", mySide < 0 ? "can't tell which team is yours" : "only works through the online server");
            g_pickerOpen = false; LinesApplyAlignment(LinesGetMode(), true); return;
        }
        side[1 - mySide].players.clear();   // the other player sets that team
        for (int i = 0; i < 4; i++) padTeam[i] = padTeam[i] >= 0 ? mySide : -1;
    }
    bool accepted = ShowLineupPicker(side[0], side[1], mySide >= 0 ? nullptr : &playersMode, padTeam);
    if (mySide >= 0) {
        std::string mine;   // "slot,slot,..." or "skip"
        if (accepted && side[mySide].slots != before[mySide]) for (uint32_t v : side[mySide].slots) mine += std::to_string((int)v) + ",";
        else { mine = "skip"; side[mySide].slots = before[mySide]; }
        side[1 - mySide].slots = before[1 - mySide];
        RelaySyncSend("lineup" + std::to_string(mySide), mine);
        fprintf(stderr, "[mods] lineup picker (online): sent my lineup, waiting for the other player's...\n");
        std::string theirs;
        if (RelaySyncTake("lineup" + std::to_string(1 - mySide), theirs, 180000)) {
            if (theirs != "skip") {
                std::vector<uint32_t> v; size_t p = 0, e;
                while ((e = theirs.find(',', p)) != std::string::npos) { v.push_back((uint32_t)std::stol(theirs.substr(p, e - p))); p = e + 1; }
                if (v.size() == side[1 - mySide].slots.size()) side[1 - mySide].slots = v;
            }
            fprintf(stderr, "[mods] lineup picker (online): got the other player's lineup (%s)\n", theirs == "skip" ? "unchanged" : "changed");
        } else fprintf(stderr, "[mods] lineup picker (online): the other player's lineup didn't arrive - theirs stays as it was\n");
        accepted = true;
    }
    for (int i = 0; i < 4; i++) if (padTeam[i] >= 0) SaveLineupSide(padTeam[i], i);   // as the players set it (BACK swaps)
    g_pickerOpen = false;
    LinesSetMode(playersMode);   // players per side chosen in the window (kept for the next match too)
    LinesApplyAlignment(playersMode, true);
    if (!accepted) { fprintf(stderr, "[mods] lineup picker: skipped\n"); return; }
    // Apply. The player column (DIGP) of the match depth chart is indexed by the game, so rows are never re-pointed at
    // another player (that left stale index entries: empty spots on the field and a crash later). Instead each
    // player's depth number (ddep) is changed in place, and a row is only added when a player is picked at a position
    // he has no row for.
    static uint32_t qSetDepth = 0;
    if (!qSetDepth)
        qSetDepth = GuestString("update 'AGCD' set 'pedd' = \x82 where ('DIGT' = \x82) and ('DIGP' = \x82) and ('SOPP' = \x82)\n");
    for (int s = 0; s < 2; s++) {
        if (side[s].slots == before[s]) continue;
        int changed = 0, added = 0, failed = 0;
        for (uint32_t pos = 0; pos < 21; pos++) {
            std::vector<uint32_t> order;
            for (int k = 0; k < kLineupSlots; k++)
                if (kLineupSlotPos[k] == pos && side[s].slots[k] != 0xFFFFFFFFu) order.push_back(side[s].slots[k]);
            if (order.empty()) continue;
            const std::vector<uint32_t>& cur = depth[s][pos];
            for (uint32_t id : cur)
                if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
            if (order == cur) continue;
            for (uint32_t d = 0; d < order.size(); d++) {
                auto it = std::find(cur.begin(), cur.end(), order[d]);
                uint32_t rc;
                if (it == cur.end()) { rc = GuestQuery(live, qInsert, {order[d], teams[s], pos, d}); added++; }
                else if (uint32_t(it - cur.begin()) != d) { rc = GuestQuery(live, qSetDepth, {d, teams[s], order[d], pos}); changed++; }
                else continue;
                if (rc != 0) failed++;
            }
        }
        // read the starters back the way the game does (position + depth) and compare with the picks
        int ok = 0, bad = 0;
        int used[21] = {};
        for (int k = 0; k < kLineupSlots; k++) {
            uint32_t pos = kLineupSlotPos[k], d = used[pos]++;
            if (side[s].slots[k] == 0xFFFFFFFFu) continue;
            WriteU32(out + 32, 0xFFFFFFFF);
            GuestQuery(live, qDepth, {out + 32, teams[s], pos, d});
            uint32_t got = ReadU32(out + 32);
            if (got == side[s].slots[k]) ok++;
            else {
                bad++;
                fprintf(stderr, "[mods] lineup picker: team %u slot %s depth %u: picked %u, depth chart has %d\n", teams[s],
                        kLineupSlotName[k], d, side[s].slots[k], (int)got);
            }
        }
        fprintf(stderr, "[mods] lineup picker: team %u lineup set (%d depths changed, %d rows added, %d failed; %d/%d starters verified)\n",
                teams[s], changed, added, failed, ok, ok + bad);
        for (int k = 0; k < kLineupSlots; k++)
            for (auto& pl : side[s].players)
                if (pl.id == side[s].slots[k])
                    fprintf(stderr, "[mods]   %-4s #%-2u %s (%s %u, id %u)\n", kLineupSlotName[k], pl.number, pl.name.c_str(),
                            kPosNames[std::min<uint32_t>(pl.pos, 20)], pl.ovr, pl.id);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 7-on-7 research. ModPowerupRoll (0x88717DE4 in sub_887179A0): r30 = the game changer just rolled for a team (r29),
// about to be stored in its power-up slot. Logged; mods/force_powerup.txt with a number replaces the roll (to find
// the value of Entourage, the power-up that sends the rest of the linemen onto the field).
void ModPowerupRoll(PPCRegister& r29, PPCRegister& r30) {
    LinesNewDown();
    static int forced = -2;
    if (forced == -2) {
        forced = -1;
        const char* dir = getenv("MADDEN_MODS_ROOT");
        if (FILE* f = fopen((std::string(dir ? dir : "mods") + "/force_powerup.txt").c_str(), "rb")) {
            if (fscanf(f, "%d", &forced) != 1) forced = -1;
            fclose(f);
        }
        if (forced >= 0) fprintf(stderr, "[mods] game changers forced to %d (mods/force_powerup.txt)\n", forced);
    }
    uint32_t rolled = r30.u32;
    if (forced >= 0 && rolled != 17) r30.u64 = (uint32_t)forced;   // 17 = no game changer this down
    fprintf(stderr, "[mods] game changer roll: side %u rolled %u%s\n", r29.u32, rolled,
            forced >= 0 && rolled != 17 ? (" -> forced " + std::to_string(forced)).c_str() : "");
}

// ---------------------------------------------------------------------------------------------------------------
uint32_t GameStateWord();
// Lines mode: 5-on-5 (normal), 7-on-7 or full lines, from mods/lines.txt ("5" / "7" / "full").
// The game changer Entourage (power-up 9) brings the rest of the linemen onto the field: sub_8871B230 (run every
// frame for both teams) sees a slot holding 9, marks it used (slot = 17) and walks the team's players, bringing in
// offensive linemen (roles LT..RT, 5-9) for the offense and defensive linemen (LE/RE/DT, 10-12) for the defense.
// Lines mode runs that step once per down for each team without the power-up:
//   ModLinesTrigger (0x8871B47C, r11 = power-up slots, r27 = team): after a new down (ModPowerupRoll), the team's
//                   slot reads 9 for this pass; the real power-up is remembered,
//   ModLinesRestore (0x8871B4AC, right after the game marked the slot used): the real power-up is put back,
//   ModLinesFilter  (0x8871B52C, r11 = player role, r3 = offense team): in 7-on-7 only LG/RG (offense) and LE/RE
//                   (defense) are brought in; with "full" the game's own ranges are used.
namespace {
int g_linesMode = -1;               // 5 normal, 7, 11 full
bool g_linesPending[2] = {false, false};
bool g_linesFaked[2] = {false, false};
uint32_t g_linesSaved[2] = {0, 0};
int g_linesLogged = 0;
int g_linesTicks = -1;
 // frames since the last play pick (-1: none pending)
int g_filterLogged = 0;
}   // namespace
// A play pick rebuilds the formation a moment later: the linemen step is requested again a few times over the next
// frames (once the new formation is in place) - the game's step itself does nothing for players already in.
extern bool g_gunDb;
void LinesRequest() {
    if (g_linesMode == 5 && !g_gunDb) return;
    g_linesPending[0] = g_linesPending[1] = true;
    if (ReadU32(0x894D8188) != 0) WriteU32(0x894D818C, ReadU32(0x894D818C) | 0x200);
}
void ModLinesOffensePicked() { InstallLinesHook(); if (g_linesMode != 5) { g_linesTicks = 0; if (g_linesLogged < 40) { g_linesLogged++; fprintf(stderr, "[mods] lines: offense picked a play\n"); } } }
void ModLinesDefensePicked() { InstallLinesHook(); if (g_linesMode != 5) { g_linesTicks = 0; if (g_linesLogged < 40) { g_linesLogged++; fprintf(stderr, "[mods] lines: defense picked a play\n"); } } }
// Game state word ([[0x894AF670]+32]): 1 = pre-play formation, 3 = a play is picked / running. The linemen step
// adds players differently per state: in state 1 only to the formation (a play pick rebuilds it and drops them),
// in state 3 into the chosen play with an assignment (how a real Entourage lasts through the snap). The step is
// requested whenever the state changes to 1 or 3.
uint32_t GameStateWord() {
    uint32_t p = ReadU32(0x894AF670);
    return (p >= 0x40000000u && p < 0xE0000000u) ? ReadU32(p + 32) : 0xFFFFFFFFu;
}
void PcTick();
void ModLinesTick() {
    PcTick();
    {   // a new play call: no extra defender until a Shotgun play is called
        static uint32_t prevSt = 0xFFFFFFFFu;
        uint32_t s = GameStateWord();
        if (s == 1 && prevSt != 1) g_gunDb = false;
        prevSt = s;
    }
    if (g_linesMode != 5 || g_gunDb) {
        static uint32_t last = 0xFFFFFFFFu; static int logged = 0;
        uint32_t st = GameStateWord();
        if (st != last) {
            if (logged < 80) { logged++; fprintf(stderr, "[mods] lines: game state %d -> %d\n", (int)last, (int)st); }
            last = st;
            if (st == 1 || st == 3) LinesRequest();
        }
        // during the pre-snap state the play pick rebuilds the formation at some point: keep asking every few
        // frames (players already in are left alone by the game's step)
        static int tick = 0;
        if (st == 1 && (++tick % 8) == 0) LinesRequest();
    }
    if (g_linesTicks < 0) return;
    int t = ++g_linesTicks;
    if (t == 4 || t == 12 || t == 24 || t == 40) LinesRequest();
    if (t >= 40) g_linesTicks = -1;
}
void PlayCallStart();
void InstallLinesHook() {
    PlayCallStart();
    if (g_linesMode >= 0) return;
    g_linesMode = 5;
    const char* dir = getenv("MADDEN_MODS_ROOT");
    std::string rule = OnlineRule("lines");
    if (!rule.empty()) g_linesMode = rule == "7" ? 7 : (rule == "full" || rule == "11") ? 11 : 5;
    else if (FILE* f = fopen((std::string(dir ? dir : "mods") + "/lines.txt").c_str(), "rb")) {
        char b[16] = {};
        if (fscanf(f, "%15s", b) == 1) {
            if (!strcmp(b, "7")) g_linesMode = 7;
            else if (!strcmp(b, "full") || !strcmp(b, "11")) g_linesMode = 11;
        }
        fclose(f);
    }
    fprintf(stderr, "[mods] lines mode: %s\n", g_linesMode == 7 ? "7-on-7" : g_linesMode == 11 ? "full lines" : "5-on-5 (normal)");
}
// The linemen step in sub_8871B230 only runs while bit 0x200 of the game-changer phase word (0x894D818C) is set -
// the game sets it when an Entourage is used and clears it after the step. Lines mode sets it at each new down.
int LinesGetMode() { InstallLinesHook(); return g_linesMode; }
void LinesSetMode(int mode) {
    InstallLinesHook();
    if (!OnlineRule("lines").empty()) return;   // online: the host's choice stays
    if (mode != 5 && mode != 7 && mode != 11) return;
    if (mode == g_linesMode) return;
    g_linesMode = mode;
    g_linesPending[0] = g_linesPending[1] = false;
    const char* dir = getenv("MADDEN_MODS_ROOT");
    if (FILE* f = fopen((std::string(dir ? dir : "mods") + "/lines.txt").c_str(), "wb")) {
        fputs(mode == 7 ? "7\n" : mode == 11 ? "full\n" : "5\n", f);
        fclose(f);
    }
    LinesApplyAlignment(mode, false);
    fprintf(stderr, "[mods] lines mode set in the pre-game window: %s\n", mode == 7 ? "7-on-7" : mode == 11 ? "full lines" : "5-on-5");
}
void LinesApplyAlignment(int mode, bool force);
void LinesNewDown() {
    InstallLinesHook();
    LinesApplyAlignment(g_linesMode, false);
    if (g_linesMode == 5) return;
    g_linesPending[0] = g_linesPending[1] = true;
    if (ReadU32(0x894D8188) != 0) WriteU32(0x894D818C, ReadU32(0x894D818C) | 0x200);
    if (g_linesLogged < 40) fprintf(stderr, "[mods] lines: new down, linemen step requested (phase word %08X)\n", ReadU32(0x894D818C));
}
void ModLinesTrigger(PPCRegister& r11, PPCRegister& r27) {
    uint32_t t = r27.u32;
    if (t > 1 || (g_linesMode == 5 && !g_gunDb)) return;
    uint32_t slot = r11.u32 + 12;
    if (g_linesFaked[t]) {   // activation was skipped last pass: put the real power-up back
        WriteU32(slot, g_linesSaved[t]); g_linesFaked[t] = false;
    }
    if (!g_linesPending[t]) return;
    g_linesPending[t] = false;
    uint32_t cur = ReadU32(slot);
    if (cur == 9) return;    // a real Entourage: let the game do it (full lines)
    g_linesSaved[t] = cur; g_linesFaked[t] = true;
    WriteU32(slot, 9);
}
void ModLinesRestore(PPCRegister& r9, PPCRegister& r27) {
    uint32_t t = r27.u32;
    if (t > 1 || !g_linesFaked[t]) return;
    WriteU32(r9.u32 + 12, g_linesSaved[t]);   // the game just wrote 17 (used); the real power-up stays available
    g_linesFaked[t] = false;
    if (g_linesLogged < 80) { g_linesLogged += 2; fprintf(stderr, "[mods] lines: bringing in linemen for team %u (power-up %u kept, game state %d)\n", t, g_linesSaved[t], (int)GameStateWord()); }
}
void ModLinesCanUse(PPCRegister& r3, PPCRegister& r27) {
    if (r27.u32 <= 1 && g_linesFaked[r27.u32]) r3.u64 = 0;   // our step: do not let the held power-up block it
}
// Extra defender against Shotgun: when the offense's play is from the Shotgun ("Gun") formation, the same step also
// brings in one defensive back (mods/extra_db.txt: ss (default), fs, cb, or off), as the offense has an extra
// receiver there. Works in every players-per-side mode (in 5 on 5 the step then runs for that defender only).
bool g_gunDb = false;
int g_extraDbRole = -2;   // -2 not read yet, -1 off
int ExtraDbRole() {
    if (g_extraDbRole != -2) return g_extraDbRole;
    g_extraDbRole = 18;
    std::string rule = OnlineRule("extra_db");
    if (!rule.empty()) { g_extraDbRole = rule == "off" ? -1 : rule == "fs" ? 17 : rule == "cb" ? 16 : 18; return g_extraDbRole; }
    if (FILE* f = fopen("mods/extra_db.txt", "rb")) {
        char b[16] = {};
        if (fscanf(f, "%15s", b) == 1) {
            for (char* c = b; *c; c++) *c = (char)tolower((unsigned char)*c);
            if (!strcmp(b, "off") || !strcmp(b, "0")) g_extraDbRole = -1;
            else if (!strcmp(b, "fs")) g_extraDbRole = 17;
            else if (!strcmp(b, "cb")) g_extraDbRole = 16;
        }
        fclose(f);
    }
    return g_extraDbRole;
}
bool ModLinesFilter(PPCRegister& r3, PPCRegister& r11, PPCRegister& r15, PPCRegister& r27, PPCRegister& r30, PPCRegister& r31);
bool ModLinesFilterDefense(PPCRegister& r3, PPCRegister& r11, PPCRegister& r15, PPCRegister& r27, PPCRegister& r30, PPCRegister& r31) {
    return ModLinesFilter(r3, r11, r15, r27, r30, r31);   // same rule; offense/defense decided from r27 vs r3
}
bool ModLinesFilter(PPCRegister& r3, PPCRegister& r11, PPCRegister& r15, PPCRegister& r27, PPCRegister& r30, PPCRegister& r31) {
    bool db = g_gunDb && ExtraDbRole() >= 0;
    if (g_linesMode != 7 && !(db && g_linesMode == 5)) {
        if (!(db && g_linesMode == 11)) return false;
    }
    bool offense = r27.u32 == r3.u32;
    uint32_t role = r11.u32;
    bool in = false;
    if (g_linesMode == 7) in = offense ? (role == 5 || role == 9) : (role == 10 || role == 11);   // LT/RT (at the guard spots), LE/RE
    else if (g_linesMode == 11) in = offense ? (role >= 5 && role <= 9) : (role >= 10 && role <= 12);   // full lines
    if (db && !offense && (int)role == ExtraDbRole()) in = true;
    r31.u64 = in ? r15.u64 : 0;
    if (g_filterLogged < 400) {
        g_filterLogged++;
        uint32_t p = r30.u32; uint64_t fl = 0;
        if (p >= 0x40000000u && p < 0xE0000000u) fl = ((uint64_t)ReadU32(p + 256) << 32) | ReadU32(p + 260);
        fprintf(stderr, "[mods] lines filter: team %u %s, player role %u (flags %016llX) -> %s%s\n", r27.u32, offense ? "offense" : "defense",
                role, (unsigned long long)fl, in ? "IN" : "out", db ? " [shotgun: extra defender]" : "");
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// 7-on-7 alignments. The game files keep the real (11-man) alignments, used by 5-on-5 and full lines; in 7-on-7 the
// ends line up inside and the tackles at the guard spots. mods/lines7.bin (made from the user's own files by
// devtools tools/lines/make_lines_patch.py) holds, per playbook, an anchor (original bytes) and the changed regions
// (original and 7-on-7 bytes). Once the playbooks are in the game's memory the anchors are found (committed guest
// memory only) and the regions are switched to the 7-on-7 bytes or back to the originals for the chosen mode.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#ifdef _WIN32
extern "C" DWORD WINAPI MauXInputGetState(DWORD slot, void* out);   // sony_pad.cpp: Xbox + PlayStation controllers
#endif
// guest range [a, a+len) is committed, readable host memory (reading it cannot fault)
bool GuestReadable(uint32_t a, uint32_t len) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(g_base + a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uint8_t*)mi.BaseAddress + mi.RegionSize >= g_base + a + len;
#else
    (void)a; (void)len; return true;
#endif
}
bool GenerateLines7(const std::string& gameDir, const std::string& outPath);
namespace {
struct AlignRegion { uint32_t off; std::vector<uint8_t> orig, alt; };
struct AlignBlock { std::vector<uint8_t> anchor; std::vector<AlignRegion> regs; std::vector<uint32_t> found; };
std::vector<AlignBlock> g_align;
bool g_alignLoaded = false;
int g_alignApplied = -1;   // mode the regions are currently set for
int g_alignScans = 0;

void LoadAlign() {
    g_alignLoaded = true;
    const char* dir = getenv("MADDEN_MODS_ROOT");
    std::string path = std::string(dir ? dir : "mods") + "/lines7.bin";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {   // first time: build it from your own game files
        const char* gd = getenv("MADDEN_GAME_DIR");
        bool made = GenerateLines7(gd ? gd : "game", path);
        fprintf(stderr, "[mods] lines: %s mods/lines7.bin from your game's playbooks\n", made ? "built" : "could not build");
        f = fopen(path.c_str(), "rb");
    }
    if (!f) { fprintf(stderr, "[mods] lines: no mods/lines7.bin - 7-on-7 uses the normal alignments\n"); return; }
    auto u32 = [&](uint32_t& v) { return fread(&v, 4, 1, f) == 1; };
    uint32_t nb = 0;
    if (u32(nb) && nb < 16)
        for (uint32_t b = 0; b < nb; b++) {
            AlignBlock blk; uint32_t al = 0, nr = 0;
            if (!u32(al) || al > 4096) break;
            blk.anchor.resize(al); if (fread(blk.anchor.data(), 1, al, f) != al) break;
            if (!u32(nr) || nr > 100000) break;
            bool ok = true;
            for (uint32_t r = 0; r < nr && ok; r++) {
                AlignRegion rg; uint32_t len = 0;
                ok = u32(rg.off) && u32(len) && len < 65536;
                if (!ok) break;
                rg.orig.resize(len); rg.alt.resize(len);
                ok = fread(rg.orig.data(), 1, len, f) == len && fread(rg.alt.data(), 1, len, f) == len;
                if (ok) blk.regs.push_back(std::move(rg));
            }
            if (!ok) break;
            g_align.push_back(std::move(blk));
        }
    fclose(f);
    fprintf(stderr, "[mods] lines: %zu 7-on-7 alignment blocks loaded\n", g_align.size());
}

// every committed range of guest memory where the game keeps its heaps (virtual 0x40000000.., physical window)
template <class F> void ForCommitted(F fn) {
#ifdef _WIN32
    const std::pair<uint32_t, uint32_t> ranges[] = {{0x40000000u, 0x7F000000u}, {0xC0000000u, 0xE0000000u}};
    for (auto& rg : ranges) {
        uint8_t* p = g_base + rg.first; uint8_t* end = g_base + rg.second;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(p, &mi, sizeof mi)) break;
            uint8_t* rb = (uint8_t*)mi.BaseAddress; uint8_t* re = rb + mi.RegionSize;
            if (re > end) re = end;
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) fn(std::max(rb, p), re);
            p = re;
        }
    }
#else
    (void)fn;
#endif
}

void FindAlign() {
    g_alignScans++;
    for (auto& b : g_align) b.found.clear();
    ForCommitted([&](uint8_t* lo, uint8_t* hi) {
        for (auto& b : g_align) {
            const auto& a = b.anchor; if (a.empty()) continue;
            for (uint8_t* p = lo; p + a.size() <= hi;) {
                p = (uint8_t*)memchr(p, a[0], hi - p);
                if (!p || p + a.size() > hi) break;
                if (!memcmp(p, a.data(), a.size())) b.found.push_back(uint32_t(p + a.size() - g_base));
                p++;
            }
        }
    });
    for (size_t i = 0; i < g_align.size(); i++)
        fprintf(stderr, "[mods] lines: alignment block %zu found %zu time(s) in memory\n", i, g_align[i].found.size());
}

// true if every region at `at` holds the original or the 7-on-7 bytes (the playbook is still there)
bool AlignValid(const AlignBlock& b, uint32_t at) {
    for (auto& r : b.regs) {
        const uint8_t* m = g_base + at + r.off;
        if (memcmp(m, r.orig.data(), r.orig.size()) && memcmp(m, r.alt.data(), r.alt.size())) return false;
    }
    return true;
}
}   // namespace

int g_alignFails = 0;   // scans without a result since the last pre-game window (stop after a few: no stutter)
void LinesApplyAlignment(int mode, bool force) {
    if (!g_alignLoaded) LoadAlign();
    if (g_align.empty()) return;
    if (force) g_alignFails = 0;
    // the files hold the normal alignments: nothing to do unless 7-on-7 is chosen or was applied before
    if (mode != 7 && g_alignApplied != 7) return;
    bool valid = false, inState = true;
    for (auto& b : g_align)
        for (uint32_t at : b.found)
            if (AlignValid(b, at)) {
                valid = true;
                const auto& r = b.regs[0];
                if (memcmp(g_base + at + r.off, (mode == 7 ? r.alt : r.orig).data(), r.orig.size())) inState = false;
            }
    if (valid && inState && mode == g_alignApplied) return;   // already set (cheap check every down)
    if (!valid) {
        if (g_alignFails >= 3) return;
        FindAlign();
        bool got = false;
        for (auto& b : g_align) if (!b.found.empty()) got = true;
        if (!got) { g_alignFails++; return; }
    }
    int done = 0;
    for (auto& b : g_align)
        for (uint32_t at : b.found) {
            if (!AlignValid(b, at)) continue;
            for (auto& r : b.regs) memcpy(g_base + at + r.off, (mode == 7 ? r.alt : r.orig).data(), r.orig.size());
            done++;
        }
    if (done) {
        g_alignApplied = mode;
        fprintf(stderr, "[mods] lines: %d playbook copies set to %s alignments\n", done, mode == 7 ? "7-on-7" : "normal");
    }
}


// ---------------------------------------------------------------------------------------------------------------
// Full playbook (extra to the arcade play call). Each down sub_8871BFD0 runs the play call: per frame it asks who
// controls the offense (r3 at 0x8871C094, 255 = CPU) and the defense (r3 at 0x8871C0C4); its state object (r26) holds
// the arcade button per side (+388 offense, +392 defense, 255 = none yet) and "called" flags (+396 / +397). Once both
// are called it sets the plays with sub_88834460(obj, team, formation name, play name): offense at 0x8871C1F4, defense
// at 0x8871C23C. During the play call an on-screen FULL PLAYBOOK button shows (click it or press BACK / P); the box
// lists the user's side only. Calling a play there marks that side as called and puts that play in place of the
// arcade one; the game then lines up and waits for the hike (input is held back until the call button is let go).
extern std::atomic<int> g_padMute;
extern std::atomic<bool> g_padMaskBack;
extern std::atomic<uint32_t> g_padInject, g_padInjectUntil, g_padInjectPorts, g_padMuteMask, g_padMuteRelease;
namespace {
std::vector<PcPlay> g_pcList[2];
bool g_pcLoaded = false;
std::atomic<uint32_t> g_pcObj{0};
std::atomic<int> g_pcUserSide{-1};           // side the user calls for this down (0 offense, 1 defense), -1 unknown
std::atomic<int> g_pcPick[2] = {-1, -1};     // the user's chosen play per side (index), -1 none
std::atomic<int> g_pcPort[2] = {255, 255};   // controller playing offense / defense this down (255 = CPU)
int g_pcForm[2], g_pcPlay[2];
uint32_t g_pcBuf = 0;
void PcLoad() {
    if (g_pcLoaded) return;
    g_pcLoaded = true;
    // with edited playbooks (playbook_mod.cpp) the list is mods/playcall_plays_edited.txt, rebuilt when the edits change
    std::string stamp = PlaybookEditsStamp();
    std::string listPath = stamp.empty() ? "mods/playcall_plays.txt" : "mods/playcall_plays_edited.txt";
    FILE* f = nullptr;
    if (!stamp.empty()) {
        char b[32] = {};
        if (FILE* s = fopen("mods/playcall_plays_edited.stamp", "rb")) { if (!fgets(b, sizeof b, s)) b[0] = 0; fclose(s); }
        if (stamp == b) f = fopen(listPath.c_str(), "rb");
    } else f = fopen(listPath.c_str(), "rb");
    if (!f) {   // first run (or new edits): build the list from the player's own game files
        std::error_code ec; std::filesystem::create_directories("mods", ec);
        bool made = GeneratePlaybookList(FsGameDir(), listPath);
        fprintf(stderr, "[mods] play call: %s %s from the game's playbooks\n", made ? "built" : "could not build", listPath.c_str());
        if (made && !stamp.empty()) if (FILE* s = fopen("mods/playcall_plays_edited.stamp", "wb")) { fputs(stamp.c_str(), s); fclose(s); }
        f = fopen(listPath.c_str(), "rb");
    }
    if (!f) { fprintf(stderr, "[mods] play call: mods/playcall_plays.txt not found (full playbook off)\n"); return; }
    static char line[16384];
    while (fgets(line, sizeof line, f)) {
        std::string l(line); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        std::vector<std::string> c; size_t p = 0;
        for (;;) { size_t q = l.find('\t', p); c.push_back(l.substr(p, q == std::string::npos ? q : q - p)); if (q == std::string::npos) break; p = q + 1; }
        if (c.size() < 3 || (c[0] != "O" && c[0] != "D") || c[1].size() > 60 || c[2].size() > 60) continue;
        PcPlay play;
        if (PcParse(std::vector<std::string>(c.begin() + 1, c.end()), play)) g_pcList[c[0] == "D"].push_back(play);
    }
    fclose(f);
    fprintf(stderr, "[mods] play call: %zu offense and %zu defense plays for the full playbook\n", g_pcList[0].size(), g_pcList[1].size());
}
std::string GuestCStr(uint32_t a) {
    std::string s; if (a < 0x10000 || a >= 0xE0000000u) return s;
    for (int i = 0; i < 64; i++) { char c = (char)g_base[a + i]; if (!c) break; s += c; }
    return s;
}
bool PcCalled(int side) {
    uint32_t o = g_pcObj.load();
    return o >= 0x40000000u && o < 0xE0000000u && g_base[o + 396 + side] != 0;
}
// two players: a player's pick from his own playbook - it is his call at once: once his buttons are let go, his pad
// presses a play-call button (X) for the game (the arcade play call closes, the play set for it is replaced)
void PcOnPick(int side, int result, int port) {
    if (result < 0 || side < 0 || side > 1 || GameStateWord() != 1) { fprintf(stderr, "[mods] play call: player %d closed his playbook\n", port + 1); return; }
    g_pcPick[side] = result;
    const PcPlay& p = g_pcList[side][result];
    fprintf(stderr, "[mods] play call: player %d (%s) calls %s - %s from the full playbook\n", port + 1, side ? "defense" : "offense", p.form.c_str(), p.name.c_str());
    std::thread([port] {
        uint32_t bit = 1u << (port & 3);
        for (int i = 0; i < 200 && (g_padMuteRelease.load() & bit); i++) Sleep(10);
        Sleep(60);
        g_padInjectPorts = bit; g_padInject = 0x4000; g_padInjectUntil = GetTickCount() + 130;
        Sleep(200);
        g_padInject = 0;
    }).detach();
}
// on its own thread: the button, BACK / P, and the box
void PcWatch() {
    typedef DWORD(WINAPI* XGet)(DWORD, void*);
    XGet xget = nullptr;
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryA(dll)) { xget = (XGet)GetProcAddress(m, "XInputGetState"); if (xget) { xget = (XGet)(void*)MauXInputGetState; break; } }
    struct Pad { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    bool prev = true;
    for (;;) {
        Sleep(16);
        uint32_t st = GameStateWord();
        int side = g_pcUserSide.load();
        static int lastSide = 0;
        if (side >= 0) lastSide = side;
        if (st != 1) { g_pcUserSide = -1; side = -1; }
        else if (side < 0) side = lastSide;   // not asked yet this down: the side of the last down
        if (st != 1) { if (st == 4) { g_pcPick[0] = -1; g_pcPick[1] = -1; } }
        // two players: a controller on each side (then both sides get the box at once, each on its own pad)
        int po = g_pcPort[0].load(), pd = g_pcPort[1].load();
        bool two = st == 1 && po < 4 && pd < 4 && po != pd;
        bool canSide[2] = {false, false};
        for (int sd = 0; sd < 2; sd++) canSide[sd] = st == 1 && g_pcPick[sd].load() < 0 && !PcCalled(sd) && !g_pcList[sd].empty();
        bool can = two ? (canSide[0] || canSide[1]) : (st == 1 && side >= 0 && canSide[side]);
        g_padMaskBack = st == 1;
        bool down = false;
        static bool prevPort[4];
        int pressedPort = -1;
        if (xget) for (DWORD i = 0; i < 4; i++) {
            Pad ps{}; bool d = xget(i, &ps) == 0 && (ps.buttons & 0x0020);
            if (d) down = true;
            if (d && !prevPort[i] && pressedPort < 0) pressedPort = (int)i;
            prevPort[i] = d;
        }
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId() && (GetAsyncKeyState('P') & 0x8000)) { if (!down && !prev && pressedPort < 0) pressedPort = 0; down = true; }
        bool clicked = PlayBadge(can);
        if (clicked && pressedPort < 0) pressedPort = 0;
        if (two && pressedPort >= 0 && pressedPort != po && pressedPort != pd) pressedPort = -1;   // a pad not in this game
        if (two && pressedPort >= 0 && !canSide[pressedPort == po ? 0 : 1]) pressedPort = -1;     // that side has called
        if (can && (clicked || (down && !prev)) && (!two || pressedPort >= 0)) {
            if (!two) g_padMute = 1;   // two players: only the pad whose playbook is open is held back
            PlayBadge(false);
            uint32_t ports = 0;   // whose pads press the call button afterwards
            if (two) {
                // each player's own playbook: the one who pressed SELECT gets his opened, the other can open his with
                // SELECT while it is up; a pick counts at once (PcOnPick: that player's pad presses the call button)
                PcBoxReq rq[2];
                int nb = 0, sideOf[2];
                for (int sd = 0; sd < 2; sd++) {
                    if (!canSide[sd]) continue;
                    int port = sd ? pd : po;
                    rq[nb] = PcBoxReq{&g_pcList[sd], sd, port, g_pcForm[sd], g_pcPlay[sd], -1, port == pressedPort, PcOnPick};
                    sideOf[nb++] = sd;
                }
                g_padMute = 0;
                ShowPlayBoxes(rq, nb);
                for (int k = 0; k < nb; k++) { int sd = sideOf[k]; g_pcForm[sd] = rq[k].form; g_pcPlay[sd] = rq[k].play; }
                ports = 0;   // each pick already pressed its own call button
            } else {
                int pick = ShowPlayBox(g_pcList[side], side, g_pcForm[side], g_pcPlay[side]);
                if (pick >= 0 && GameStateWord() == 1) {
                    g_pcPick[side] = pick;
                    int port = g_pcPort[side].load();
                    ports |= 1u << (port < 4 ? port : 0);
                    const PcPlay& p = g_pcList[side][pick];
                    fprintf(stderr, "[mods] play call: %s calls %s - %s from the full playbook\n", side ? "defense" : "offense", p.form.c_str(), p.name.c_str());
                } else fprintf(stderr, "[mods] play call: full playbook closed\n");
            }
            if (!two) g_padMute = 2;   // nothing reaches the game until the buttons are let go
            if (ports) {
                // then each caller's pad gets one press of a play-call button (X), as if called on the game's own
                // screen: the arcade play call closes as usual and the play set for it is replaced by the chosen one
                for (int i = 0; i < 200 && g_padMute.load() != 0; i++) Sleep(10);
                g_padMute = 0;
                Sleep(60);
                g_padInjectPorts = ports; g_padInject = 0x4000; g_padInjectUntil = GetTickCount() + 130;
                Sleep(200);
                g_padInject = 0;
            }
            down = true;
        }
        prev = down;
    }
}
void PcSet(int side, int idx, PPCRegister& r5, PPCRegister& r6, const char* who) {
    if (idx < 0 || idx >= (int)g_pcList[side].size()) return;
    const PcPlay& p = g_pcList[side][idx];
    if (!g_pcBuf) g_pcBuf = GuestAlloc(256);
    uint32_t a = g_pcBuf + side * 128;
    auto put = [&](uint32_t at, const std::string& s) { for (size_t i = 0; i < 63; i++) g_base[at + i] = i < s.size() ? (uint8_t)s[i] : 0; g_base[at + 63] = 0; };
    put(a, p.gform); put(a + 64, p.name);
    r5.u64 = a; r6.u64 = a + 64;
    fprintf(stderr, "[mods] play call: %s play set to %s - %s (%s)\n", side ? "defense" : "offense", p.gform.c_str(), p.name.c_str(), who);
}
// the user's play from the full playbook, if it is for this side (used once)
bool PcReplace(int side, PPCRegister& r5, PPCRegister& r6) {
    int pick = g_pcPick[side].exchange(-1);
    if (pick < 0) return false;
    PcSet(side, pick, r5, r6, "user");
    return true;
}
}   // namespace
void PlayCallStart() {
    PcLoad();
    static bool started = false;
    if (!started && !g_pcList[0].empty()) { started = true; std::thread(PcWatch).detach(); }
}
// per frame in the play call: who controls the offense (r3, 255 = CPU; if the CPU, the defense is asked next)
static void PcSetSide(int side) {
    if (side == g_pcUserSide.load()) return;
    static int logged = 0;
    if (logged < 40) { logged++; fprintf(stderr, "[mods] play call: user calls for the %s\n", side == 0 ? "offense" : side == 1 ? "defense" : "(nobody)"); }
    g_pcUserSide = side;
}
void PlayCallWhoOffense(PPCRegister& r3, PPCRegister& r26) { g_pcObj = r26.u32; if ((r3.u32 & 0xFF) != 255) PcSetSide(0); }
void PlayCallWhoDefense(PPCRegister& r3, PPCRegister& r26) { g_pcObj = r26.u32; PcSetSide((r3.u32 & 0xFF) != 255 ? 1 : -1); }
// (hooked at the per-frame check of the two "called" flags; the call now goes through the game's own play call)
void PlayCallCommit(PPCRegister& r26) { g_pcObj = r26.u32; }
// Every few frames of the play call (game thread): ask the game who controls the offense / defense, as it does itself:
// the teams from the game-flow object's methods +124 / +128, the user from sub_8850E318 (255 = CPU).
PPC_EXTERN_FUNC(sub_8850E318);
PPC_EXTERN_FUNC(sub_8850F680);
void PaFlags(uint32_t users, uint32_t qb, uint32_t rb, int uidx);
static std::string g_offForm;   // this down's offense formation (playbook name)
static std::string g_offPlay;   // and play name
void PaScan();
void PcTick() {
    PaScan();
    if (GameStateWord() != 1 || g_pcList[0].empty()) return;
    static int t = 0;
    if (++t % 6) return;
    PPCContext* live = CurrentGuestContext();
    if (!live) return;
    auto obj = [](uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; };
    auto code = [](uint32_t f) { return f >= PPC_CODE_BASE && f < PPC_CODE_BASE + PPC_CODE_SIZE && !(f & 3) && PPC_LOOKUP_FUNC(g_base, f) != nullptr; };
    uint32_t o = ReadU32(0x894D6BA4); if (!obj(o)) return;
    uint32_t m = ReadU32(o + 8); if (!obj(m)) return;
    uint32_t vt = ReadU32(m); if (!obj(vt)) return;
    uint32_t f124 = ReadU32(vt + 124), f128 = ReadU32(vt + 128), users = ReadU32(0x894C07D0);
    if (!code(f124) || !code(f128) || !obj(users)) return;
    auto call = [&](uint32_t fn, uint32_t a3, uint32_t a4, bool direct) {
        PPCContext c = *live; c.r1.u64 = (live->r1.u32 - 0x1000) & ~15u; c.r3.u64 = a3; c.r4.u64 = a4;
        if (direct) sub_8850E318(c, g_base); else (PPC_LOOKUP_FUNC(g_base, fn))(c, g_base);
        return c.r3.u32;
    };
    uint32_t offT = call(f124, m, 0, false), defT = call(f128, m, 0, false);
    uint32_t offU = call(0, users, offT, true) & 0xFF, defU = call(0, users, defT, true) & 0xFF;
    g_pcPort[0] = (int)offU; g_pcPort[1] = (int)defU;
    if (int64_t t0 = g_matchLoadStart.exchange(0)) {   // the match is loaded: first play call
        int64_t ticks = std::chrono::steady_clock::now().time_since_epoch().count() - t0 - g_pickerTime.exchange(0);
        double sec = (double)ticks * std::chrono::steady_clock::period::num / std::chrono::steady_clock::period::den;
        fprintf(stderr, "[perf] match loaded in %.1f s (team select to the first play call, lineup window not counted)\n", sec);
    }
    // (1.0.10-1.0.22 moved players not in the play and took player snapshots here every frame, calling into the
    // game from this hook during live plays; removed - the test build never did it and throws went wrong with it)
    {   // remember which controller played which team (for the next lineup window)
        extern uint32_t g_lineupTeams[2]; void SaveLineupSide(int side, int port);
        static uint32_t savedFor[2] = {0xFFFFFFFFu, 0xFFFFFFFFu}; static int savedPort[2] = {-1, -1};
        uint32_t tm[2] = {offT, defT}, pt[2] = {offU, defU};
        for (int k = 0; k < 2; k++) {
            if (tm[k] > 1 || pt[k] > 3) continue;
            if (savedFor[tm[k]] == g_lineupTeams[tm[k]] && savedPort[tm[k]] == (int)pt[k]) continue;
            savedFor[tm[k]] = g_lineupTeams[tm[k]]; savedPort[tm[k]] = (int)pt[k];
            SaveLineupSide((int)tm[k], (int)pt[k]);
        }
    }
    PcSetSide(offU != 255 ? 0 : defU != 255 ? 1 : -1);
}
// The CPU calls from the full playbook too (mods/cpu_playbook.txt = off turns it off): the arcade call's kind is kept
// (its run button -> a run play, a pass button -> a pass play), any formation but Goal Line / Wildcat. On defense the
// CPU answers Shotgun with Nickel / Dime / Dollar and anything else with 4-3 / Nickel / Dime.
static int g_cpuBook = -1;
static bool CpuBook() {
    if (g_cpuBook < 0) {
        g_cpuBook = 1;
        if (FILE* f = fopen("mods/cpu_playbook.txt", "rb")) { char b[16] = {}; if (fscanf(f, "%15s", b) == 1 && (!strcmp(b, "off") || !strcmp(b, "0"))) g_cpuBook = 0; fclose(f); }
        fprintf(stderr, "[mods] play call: CPU calls from the full playbook: %s\n", g_cpuBook ? "on" : "off");
    }
    return g_cpuBook == 1;
}
static void CpuPick(int side, bool wantPass, PPCRegister& r5, PPCRegister& r6) {
    std::vector<int> c;
    for (int i = 0; i < (int)g_pcList[side].size(); i++) {
        const PcPlay& p = g_pcList[side][i];
        if (side == 0) {
            if (p.gform == "Goal Line" || p.gform == "Pahokee" || p.pass != wantPass) continue;
        } else {
            bool gun = g_offForm == "Gun";
            bool ok = gun ? (p.gform == "Nickel" || p.gform == "Dime" || p.gform == "Dollar") : (p.gform == "4-3" || p.gform == "Nickel" || p.gform == "Dime");
            if (!ok) continue;
        }
        c.push_back(i);
    }
    if (c.empty()) return;
    static uint32_t seed = GetTickCount() | 1;
    seed = seed * 1103515245u + 12345u;
    PcSet(side, c[(seed >> 8) % c.size()], r5, r6, "CPU");   // never through g_pcPick: that holds the user's call
}
// Play action for the user: the arcade game (which has no play-action plays) treats the play-action type (PLYT 4)
// like a run when a user is the QB, so the fake becomes a real handoff and the user ends up on the back. While the
// user has the ball, the playbooks' play-action plays are made type 105 (a pass type, used otherwise only by the fake
// field goal pass); when the CPU has it, they go back to 4. The playbook databases used in a match are "PBT0/1",
// "PBD0/1" etc.; each is tried (a missing one does nothing). (mods/pa_fix.txt = off turns it off)
void PaPlayType(bool userOffense) {
    static int state = -1;   // -1 unknown, 0 game's (4), 1 changed (105)
    static int fix = -1;
    if (fix < 0) { fix = PaFixRule(); if (fix < 0 && (fix = 1)) if (FILE* f = fopen("mods/pa_fix.txt", "rb")) { char b[16] = {}; if (fscanf(f, "%15s", b) == 1 && !strcmp(b, "off")) fix = 0; fclose(f); } }
    int want = (fix && userOffense) ? 1 : 0;
    if (want == state) return;
    PPCContext* live = CurrentGuestContext(); if (!live) return;
    static uint32_t q = 0;
    if (!q) q = GuestString("use \x82 update 'LYLP' set 'TYLP' = \x82 where 'TYLP' = \x82");
    std::string done;
    for (uint32_t h : {0x30544250u, 0x31544250u, 0x32544250u, 0x30444250u, 0x31444250u, 0x32444250u, 0x30424250u, 0x31424250u}) {
        uint32_t rc = GuestQuery(*live, q, {h, want ? 105u : 4u, want ? 4u : 105u});
        char t[32]; snprintf(t, sizeof t, " %c%c%c%c=%d", (char)(h & 255), (char)((h >> 8) & 255), (char)((h >> 16) & 255), (char)(h >> 24), (int)rc); done += t;
    }
    state = want;
    fprintf(stderr, "[mods] play action: play-action plays %s (results:%s)\n", want ? "made pass plays while the user has the ball" : "back to the game's type", done.c_str());
}
void PlayCallOffense(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    PlayCallStart();
    static int logged = 0;
    if (logged < 200) { logged++; fprintf(stderr, "[mods] play call: offense (team %u) %s - %s\n", r4.u32, GuestCStr(r5.u32).c_str(), GuestCStr(r6.u32).c_str()); }
    if (PcReplace(0, r5, r6)) { void StepRecStart(); StepRecStart(); }
    else if (g_pcPort[0].load() >= 4 && g_pcUserSide.load() != 0 && CpuBook()) {   // the CPU has the ball
        std::string arc = GuestCStr(r6.u32);
        CpuPick(0, arc.find("Pass") != std::string::npos, r5, r6);
    }
    std::string f = GuestCStr(r5.u32);
    { extern uint32_t g_iconPlayId; g_iconPlayId++; }
    g_offForm = f;
    g_offPlay = GuestCStr(r6.u32);
    {   // the user's play-action play: its fake is changed while it loads (PaStepFetched)
        extern std::atomic<bool> g_paConvert;
        const std::string& n = g_offPlay;
        bool pa = n.rfind("PA ", 0) == 0 || n.find(" PA ") != std::string::npos;
        static int fix = -1;
        if (fix < 0) { fix = PaFixRule(); if (fix < 0 && (fix = 1)) if (FILE* ff = fopen("mods/pa_fix.txt", "rb")) { char bb[16] = {}; if (fscanf(ff, "%15s", bb) == 1 && !strcmp(bb, "off")) fix = 0; fclose(ff); } }
        g_paConvert = fix && pa && g_pcPort[0].load() < 4;   // a controller has the ball (also two players: the side value flips to the defense)
    }
    g_gunDb = (f == "Gun" || f == "Shotgun") && ExtraDbRole() >= 0;
    if (g_gunDb) fprintf(stderr, "[mods] play call: Shotgun play - the defense brings in an extra defender\n");
}
void PlayCallDefense(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    { extern std::atomic<bool> g_paConvert; g_paConvert = false; void StepRecEnd(); StepRecEnd(); }   // the offense's play has loaded
    static int logged = 0;
    if (logged < 200) { logged++; fprintf(stderr, "[mods] play call: defense (team %u) %s - %s\n", r4.u32, GuestCStr(r5.u32).c_str(), GuestCStr(r6.u32).c_str()); }
    if (PcReplace(1, r5, r6)) {}
    else if (g_pcPort[1].load() >= 4 && g_pcUserSide.load() != 1 && CpuBook()) CpuPick(1, false, r5, r6);   // the CPU is on defense
}

// Pass study (temporary): which pass icons (0..4) the game asks a receiver for during a play, with the caller.
// the pass-icon study logs (from finding the icon rules) only run with MADDEN_STUDY=1
static bool StudyLogs() { static const bool on = getenv("MADDEN_STUDY") != nullptr; return on; }
void PassIconLog(PPCRegister& r1, PPCRegister& r4) {
    if (!StudyLogs()) return;
    static std::mutex m; static int n = 0; static std::string last; static int rep = 0;
    std::lock_guard<std::mutex> l(m);
    if (n >= 4000) return;
    char b[200]; snprintf(b, sizeof b, "st %u icon %u from%s", GameStateWord(), r4.u32, StackChain(r1.u32).substr(0, 54).c_str());
    if (last == b) { rep++; return; }
    if (rep) fprintf(stderr, "[pass]   (x%d)\n", rep);
    rep = 0; last = b; n++;
    fprintf(stderr, "[pass] %s\n", b);
}

// Pass study (temporary): the pass-icon users (entries), with the call chain
static void PassFnLog(const char* fn, PPCRegister& r1, PPCRegister& r3, PPCRegister& r4) {
    if (!StudyLogs()) return;
    static std::mutex m; static std::map<std::string, int> n; static std::map<std::string, std::string> last;
    std::lock_guard<std::mutex> l(m);
    auto role = [](uint32_t o) -> int { if (o < 0x40000000u || o >= 0xE0000000u) return -1; uint32_t q = ReadU32(o + 932); return (q >= 0x40000000u && q < 0xE0000000u) ? (int)ReadU32(q + 136) : -1; };
    std::string icons;
    if (!strcmp(fn, "88457AE8") && r3.u32 >= 0x40000000u && r3.u32 < 0xE0000000u) {   // icon list (from the last build)
        char t[8]; icons = " icons:";
        for (int i = 0; i < 24; i++) { snprintf(t, sizeof t, " %02X", g_base[r3.u32 + 117 + i]); icons += t; }
    }
    char b[400]; snprintf(b, sizeof b, "st %u r3=%08X r4=%08X(role %d)%s from%s", GameStateWord(), r3.u32, r4.u32, role(r4.u32), icons.c_str(), StackChain(r1.u32).substr(0, 45).c_str());
    if (last[fn] == b || n[fn] >= 300) return;
    last[fn] = b; n[fn]++;
    fprintf(stderr, "[passfn] %s %s\n", fn, b);
}
#define PASSFN(a) void PassFn_##a(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4) { PassFnLog(#a, r1, r3, r4); }
PASSFN(8844C7D8) PASSFN(8844C840) PASSFN(88456C98) PASSFN(88457AE8) PASSFN(884738A0) PASSFN(88536EE0)

// Pass study: each receiver's icon slot (r29) when the icons are drawn, and the button slot it gets (r3, 255 = none)
void PassIconSlot(PPCRegister& r29, PPCRegister& r31) {
    if (!StudyLogs()) return;
    static int n = 0; if (n >= 600) return;
    uint32_t q = (r31.u32 >= 0x40000000u && r31.u32 < 0xE0000000u) ? ReadU32(r31.u32 + 932) : 0;
    int role = (q >= 0x40000000u && q < 0xE0000000u) ? (int)ReadU32(q + 136) : -1;
    static std::string last; char b[96]; snprintf(b, sizeof b, "player %08X role %d icon slot %u", r31.u32, role, r29.u32 & 0xFF);
    if (last == b) return; last = b; n++;
    fprintf(stderr, "[icon] st %u %s\n", GameStateWord(), b);
}
void PassIconButton(PPCRegister& r3, PPCRegister& r30, PPCRegister& r31) {
    if (!StudyLogs()) return;
    static int n = 0; if (n >= 600) return;
    uint32_t q = (r31.u32 >= 0x40000000u && r31.u32 < 0xE0000000u) ? ReadU32(r31.u32 + 932) : 0;
    int role = (q >= 0x40000000u && q < 0xE0000000u) ? (int)ReadU32(q + 136) : -1;
    static std::string last; char b[96]; snprintf(b, sizeof b, "role %d icon slot %u -> button %u", role, r30.u32 & 0xFF, r3.u32 & 0xFF);
    if (last == b) return; last = b; n++;
    fprintf(stderr, "[icon] st %u %s\n", GameStateWord(), b);
}

// Pass icons: the icon drawer (sub_88496018) asks [0x894AF630]'s method +4 whether a receiver's icon slot is hidden;
// in this arcade game it hid some slots (no A icon, Y on some plays) though the receiver has a slot. The answer is
// forced to "shown" (mods/pass_icons.txt = game keeps the game's own choice). r30 = icon slot, r3 = hidden.
void PassIconShown(PPCRegister& r3, PPCRegister& r30) {
    static int mode = -1;
    if (mode < 0) {
        mode = 1;
        if (FILE* f = fopen("mods/pass_icons.txt", "rb")) { char b[16] = {}; if (fscanf(f, "%15s", b) == 1 && !strcmp(b, "game")) mode = 0; fclose(f); }
    }
    if ((r3.u32 & 0xFF) == 0) return;
    static int n = 0;
    if (n < 60) { n++; fprintf(stderr, "[icon] slot %u was hidden by the game%s\n", r30.u32 & 0xFF, mode ? " - shown" : ""); }
    if (mode) r3.u64 = 0;
}
// Play action: the users table ([0x894C07D0]) holds each user's controlled player as a player slot at +72 + user.
// On a play-action play (name "PA ..." / "... PA ..." / fakes) the game moves the user from the QB to the back he
// fakes to, as if it were a real handoff. Until the user throws (B / X / Y / RB / LB, or A after the snap) the user is
// put back on the QB whenever that happens. (mods/pa_fix.txt = off turns it off)
extern std::atomic<uint32_t> g_padPressMask[4];
void PaScan() {
    static uint32_t lastSt = 0; static bool active = false; static int qbSlot = -1, uidx = -1; static uint32_t qbP = 0, rbP = 0;
    uint32_t st = GameStateWord();
    if (st != 3) { lastSt = st; active = false; return; }
    PPCContext* live = CurrentGuestContext(); if (!live) return;
    auto obj = [](uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; };
    uint32_t users = ReadU32(0x894C07D0), plist = ReadU32(0x894C07D4);
    if (!obj(users) || !obj(plist)) return;
    if (lastSt != 3) {   // a new play: is it the user's play-action play?
        lastSt = 3; active = false; qbSlot = uidx = -1; qbP = rbP = 0;
        static int fix = -1;
        if (fix < 0) { fix = PaFixRule(); if (fix < 0 && (fix = 1)) if (FILE* f = fopen("mods/pa_fix.txt", "rb")) { char b[16] = {}; if (fscanf(f, "%15s", b) == 1 && !strcmp(b, "off")) fix = 0; fclose(f); } }
        const std::string& n = g_offPlay;
        bool pa = n.rfind("PA ", 0) == 0 || n.find(" PA ") != std::string::npos || n.find("Fake") != std::string::npos || n.find("Flea") != std::string::npos;
        if (!fix || !pa || g_pcPort[0].load() >= 4) return;
        uint32_t o = ReadU32(0x894D6BA4); if (!obj(o)) return;
        uint32_t m = ReadU32(o + 8); if (!obj(m)) return;
        uint32_t f124 = ReadU32(ReadU32(m) + 124);
        if (!(f124 >= PPC_CODE_BASE && f124 < PPC_CODE_BASE + PPC_CODE_SIZE && PPC_LOOKUP_FUNC(g_base, f124))) return;
        PPCContext c = *live; c.r1.u64 = (live->r1.u32 - 0x1000) & ~15u; c.r3.u64 = m;
        (PPC_LOOKUP_FUNC(g_base, f124))(c, g_base);
        uint32_t team = c.r3.u32;
        c = *live; c.r1.u64 = (live->r1.u32 - 0x1000) & ~15u; c.r3.u64 = users; c.r4.u64 = team;
        sub_8850E318(c, g_base);
        uidx = (int)(c.r3.u32 & 0xFF);
        if (uidx >= 4) return;
        for (uint32_t i = 0; i < 11; i++) {
            PPCContext d = *live; d.r1.u64 = (live->r1.u32 - 0x1000) & ~15u; d.r3.u64 = plist; d.r4.u64 = i; d.r5.u64 = team;
            sub_8850F680(d, g_base);
            uint32_t p = d.r3.u32, q = obj(p) ? ReadU32(p + 932) : 0;
            if (obj(q) && ReadU32(q + 136) == 0) { qbSlot = (int)i; qbP = p; }
            if (obj(q) && (ReadU32(q + 136) == 1 || ReadU32(q + 136) == 2) && !rbP) rbP = p;
        }
        if (qbSlot < 0) return;
        active = true;
        { extern uint32_t g_paPlayId; g_paPlayId++; }
        for (auto& mk : g_padPressMask) mk = 0;
        fprintf(stderr, "[mods] play action: %s - user %d kept on the QB (slot %d) until the throw\n", n.c_str(), uidx, qbSlot);
    }
    if (!active) return;
    if (false) {   // study: where the QB / backs are pointed at during the play (users, game flow, the game's data area)
        static int t2 = 0;
        if (++t2 % 4 == 0) {
            std::map<uint32_t, int> who;
            for (uint32_t i = 0; i < 11; i++) {
                for (uint32_t tm = 0; tm < 2; tm++) {
                    PPCContext d = *live; d.r1.u64 = (live->r1.u32 - 0x1000) & ~15u; d.r3.u64 = plist; d.r4.u64 = i; d.r5.u64 = tm;
                    sub_8850F680(d, g_base);
                    uint32_t p = d.r3.u32, q = obj(p) ? ReadU32(p + 932) : 0;
                    int role = obj(q) ? (int)ReadU32(q + 136) : -1;
                    if (obj(p) && (role == 0 || role == 1 || role == 2)) who[p] = role + tm * 100;
                }
            }
            uint32_t o2 = ReadU32(0x894D6BA4), m2 = obj(o2) ? ReadU32(o2 + 8) : 0;
            struct R { const char* n; uint32_t a, len; } regs[] = {{"users", users, 0x2000}, {"flow", m2, 0x2000}, {"data", 0x894A0000u, 0x50000}};
            std::string cur;
            for (auto& r : regs) {
                if (!obj(r.a)) continue;
                for (uint32_t off = 0; off < r.len; off += 4) {
                    auto it = who.find(ReadU32(r.a + off));
                    if (it != who.end()) { char sb[48]; snprintf(sb, sizeof sb, " %s+%X=%s%d", r.n, off, it->second >= 100 ? "def" : "", it->second % 100); cur += sb; }
                }
            }
            static std::string lastScan; static int ns = 0;
            if (cur != lastScan && ns < 400) { ns++; lastScan = cur; fprintf(stderr, "[pa] f%d:%s\n", t2, cur.c_str()); }
        }
    }
    uint32_t pressed = 0; for (auto& mk : g_padPressMask) pressed |= mk.load();
    static int aSnaps = 0;
    if (pressed & 0xE300) { active = false; fprintf(stderr, "[mods] play action: throw button pressed - control left to the game\n"); return; }
    uint8_t cur = g_base[users + 72 + uidx];
    if (cur == (uint8_t)qbSlot || cur == 255) return;
    // moved off the QB: back to him if it was to a back (HB / FB)
    uint32_t team = 0;
    for (uint32_t t = 0; t < 2; t++) {
        PPCContext d = *live; d.r1.u64 = (live->r1.u32 - 0x1000) & ~15u; d.r3.u64 = plist; d.r4.u64 = cur; d.r5.u64 = t;
        sub_8850F680(d, g_base);
        uint32_t p = d.r3.u32, q = obj(p) ? ReadU32(p + 932) : 0;
        int role = obj(q) ? (int)ReadU32(q + 136) : -1;
        if (role == 1 || role == 2) { team = 1; break; }
    }
    (void)aSnaps;
    if (!team) { active = false; return; }
    g_base[users + 72 + uidx] = (uint8_t)qbSlot;
    static int n = 0; if (n++ < 50) fprintf(stderr, "[mods] play action: control moved to the back (slot %u) - put back on the QB\n", cur);
}

// Pass icons: the button picture for an icon slot (r3 after sub_8844C850, r30 = slot). The game finds the picture
// from the button bound to that receiver's pass action; for slot 1 the bound button (A, which does throw to him) has
// no picture in its list, so it came back -1 (no icon) and stayed that way (the drawer caches it). Slot 1 gets the
// A picture (0; B / X / Y are 1 / 2 / 3).
void PassIconGlyph(PPCRegister& r3, PPCRegister& r30) {
    uint32_t slot = r30.u32 & 0xFF;
    int pic = (int)r3.u32;
    static int n = 0;
    if (StudyLogs() && n < 100) { n++; fprintf(stderr, "[icon] slot %u -> picture %d\n", slot, pic); }
}
// Pass icons: the raw button bound to an icon slot's pass action (r3), r29 = slot. B / X / Y come as 11 / 12 / 13 (A
// would be 10); slot 1, the receiver A throws to, came as 16 (no button bound), so it is given A's picture (10).
void PassIconCode(PPCRegister& r3, PPCRegister& r29) {
    if (!StudyLogs()) return;
    int code = (int)r3.u32;
    uint32_t slot = r29.u32 & 0xFF;
    bool a = false;   // pictures for unbound slots are set per play by PassIconCache
    static std::string last; char b[80]; snprintf(b, sizeof b, "slot %u -> button code %d%s", slot, code, a ? " (given the A picture)" : "");
    static int n = 0; if (last == b || n >= 200) return; last = b; n++;
    fprintf(stderr, "[icon] %s\n", b);
}

// Pass icons, per play: the drawer keeps each slot's picture (r28 + 20 + 4*slot, flag r28 + 40 + slot) for the whole
// game. Slots 1 and 3 have no button bound in this arcade game (picture -1); the game gives them the spare buttons,
// Y (when slot 4 is empty) then A, in slot order (1, then 3). Each play the pictures follow that. Hooked where the drawer looks at the kept picture (r28 = drawer, r30 = slot).
uint32_t g_iconPlayId = 0;   // bumped when the offense's play is set
void PassIconCache(PPCRegister& r28, PPCRegister& r30) {
    static uint32_t lastPlay = 0xFFFFFFFFu; static uint8_t seen = 0; static int aSlot = -1;
    extern uint32_t g_iconPlayId;
    if (g_iconPlayId != lastPlay) { lastPlay = g_iconPlayId; seen = 0; aSlot = -1; }   // a new play call (not the snap)
    uint32_t d = r28.u32, slot = r30.u32 & 0xFF;
    if (d < 0x40000000u || d >= 0xE0000000u || slot > 4) return;
    seen |= (uint8_t)(1u << slot);
    // the spare buttons - Y (only when slot 4 is empty), then A - go to the receivers in slot 1, then slot 3
    int spare[2], ns = 0;
    if (!(seen & 16)) spare[ns++] = 3;
    spare[ns++] = 0;
    int k = 0, pic1 = -1, pic3 = -1;
    if (seen & 2) pic1 = k < ns ? spare[k++] : -1;
    if (seen & 8) pic3 = k < ns ? spare[k++] : -1;
    for (uint32_t s2 : {1u, 3u}) {
        int pic = s2 == 1 ? pic1 : pic3;
        if (g_base[d + 40 + s2] != 1 || (int)ReadU32(d + 20 + 4 * s2) != pic) { WriteU32(d + 20 + 4 * s2, (uint32_t)pic); g_base[d + 40 + s2] = 1; }
    }
    int want = pic1 * 10 + pic3;
    if (want != aSlot) { aSlot = want; static int n = 0; if (n++ < 100) fprintf(stderr, "[icon] this play: slot 1 picture %d, slot 3 picture %d (0 = A, 3 = Y)\n", pic1, pic3); }
}
// Play-action study: at the start of the user's play-action play, every word in committed guest memory that points
// at the QB is listed; during the play, any of them that comes to point at the back is logged (the user's control
// moving to him). Then the fix writes the QB back there.
uint32_t g_paPlayId = 0;
std::vector<uint32_t> g_paCand;
uint32_t g_paCtl = 0;   // found: where the user's controlled player is kept (this play)
void PaFlags(uint32_t users, uint32_t qb, uint32_t rb, int uidx) {
    (void)users; (void)uidx;
    static uint32_t lastQb = 0, lastRb = 0; static int frames = 0;
    static uint32_t playId = 0;
    extern uint32_t g_paPlayId;
    if (!qb || !rb) return;
    if (playId != g_paPlayId) {
        playId = g_paPlayId; lastQb = qb; lastRb = rb; frames = 0; g_paCand.clear(); g_paCtl = 0;
        uint64_t a = 0x40000000ull;
        while (a < 0x100000000ull && g_paCand.size() < 200000) {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(g_base + a, &mi, sizeof mi)) break;
            uint64_t start = (uint64_t)((uint8_t*)mi.BaseAddress - g_base), end = start + mi.RegionSize;
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE))) {
                for (uint64_t x = std::max(start, a); x + 4 <= end && x < 0x100000000ull; x += 4)
                    if (ReadU32((uint32_t)x) == qb) g_paCand.push_back((uint32_t)x);
            }
            a = end > a ? end : a + 0x1000;
        }
        fprintf(stderr, "[pa] QB %08X back %08X: %zu places point at the QB\n", qb, rb, g_paCand.size());
        return;
    }
    frames++;
    if (g_paCtl) {   // keep the user on the QB
        if (ReadU32(g_paCtl) == rb) { WriteU32(g_paCtl, qb); static int n = 0; if (n++ < 50) fprintf(stderr, "[mods] play action: control put back on the QB (%08X)\n", g_paCtl); }
        return;
    }
    for (uint32_t c : g_paCand) {
        if (ReadU32(c) == rb) {
            fprintf(stderr, "[pa] frame %d: %08X now points at the back (was the QB)\n", frames, c);
            static std::set<uint32_t> told;
            if (!told.count(c)) told.insert(c);
            g_paCtl = c;   // try it: the first place that moved
            WriteU32(c, qb);
            break;
        }
    }
}

// Play action for the user: the arcade game turns the QB's play-action fake of step code 40 ("fake handoff", with
// the back's matching step 26) into a real switch of the user to the back. Play-action plays with the other fake
// (QB step 12 + back step 25, e.g. "PA QB Choice") work. While the user's play-action play is being loaded (set by
// PlayCallOffense, until the defense's play is set), the steps read from the playbook (sub_884478F8, after each
// fetch: code at r1+96, values at +98/+99/+100) are changed: 40 -> 12 (1, 5, 2), 26 -> 25 (5, 0, 0).
std::atomic<bool> g_paConvert{false};
static void PaStepAt(uint32_t b);
void PaStepFetched(PPCRegister& r1) { if (g_paConvert.load()) PaStepAt(r1.u32 + 96); }
// the loader the play call uses (sub_88519F48, cursor rows: code at r1+112, values at +114/+115/+116)
// Which play the game really loaded for a Full Playbook pick: some formation + name pairs have two variants, and the
// game picks one itself. The loaded steps (code and values) are logged once per pick as a fingerprint, so the playbook
// list can be checked against what runs (devtools tools/playcall/audit_playbook.py --fingerprints <log>).
std::atomic<bool> g_stepRec{false};
static std::string g_stepBuf; static std::mutex g_stepMx;
void StepRecStart() { std::lock_guard<std::mutex> l(g_stepMx); g_stepBuf.clear(); g_stepRec = true; }
void StepRecEnd() {
    if (!g_stepRec.exchange(false)) return;
    std::lock_guard<std::mutex> l(g_stepMx);
    static int n = 0;
    if (n++ < 300) fprintf(stderr, "[mods] play loaded: %s - %s steps %s\n", g_offForm.c_str(), g_offPlay.c_str(), g_stepBuf.c_str());
}
static void StepRecord(uint32_t b) {
    if (!g_stepRec.load()) return;
    std::lock_guard<std::mutex> l(g_stepMx);
    if (g_stepBuf.size() > 2400) return;
    char t[16]; snprintf(t, sizeof t, "%02X%02X%02X%02X.", g_base[b], g_base[b + 2], g_base[b + 3], g_base[b + 4]); g_stepBuf += t;
}
void PaStepFetched2(PPCRegister& r1) { StepRecord(r1.u32 + 112); if (g_paConvert.load()) PaStepAt(r1.u32 + 112); }
static void PaStepAt(uint32_t b) {
    uint8_t code = g_base[b];
    if (code == 40) { g_base[b] = 12; g_base[b + 2] = 1; g_base[b + 3] = 5; g_base[b + 4] = 2; }
    else if (code == 26) { g_base[b] = 25; g_base[b + 2] = 5; g_base[b + 3] = 0; g_base[b + 4] = 0; }
    else return;
    static int n = 0; if (n++ < 40) fprintf(stderr, "[mods] play action: step %u changed to %u (the fake the game handles)\n", code, g_base[b]);
}
