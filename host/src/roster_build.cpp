// Roster sets built on the player's PC (mods/rosters/<set>/) from the player's own qkl_boot.ast:
//   roster.tsv     the roster: first, last, position, team id, jersey, height (in), weight (lb), age, years pro
//                  (real-world facts; shipped with the project's updates)
//   roster_edits.txt  look / detail edits: "<player id> <field> <value>" (shipped with updates too)
//   ratings.csv    optional, the player's own ratings file (columns First Name, Last Name, OVERALL, SPEED, ...);
//                  without it a returning player keeps the game's own ratings and a new player takes the ratings of
//                  the spot he fills
// The result is mods/rosters/<set>/qkl_boot.ast (used by the game instead of the original), rebuilt when any of
// these inputs change (built.txt keeps their fingerprint). Nothing from the game is in this repository.
#include "gamedb.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <set>
#include <functional>
#include <filesystem>

namespace {
using gdb::Db; using gdb::Table;
const char* kPos[] = {"QB","HB","FB","WR","TE","LT","LG","C","RG","RT","LE","RE","DT","LOLB","MLB","ROLB","CB","FS","SS","K","P"};
int PosId(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    static const std::map<std::string, int> m = {{"QB",0},{"HB",1},{"RB",1},{"FB",2},{"WR",3},{"TE",4},{"LT",5},{"LG",6},{"C",7},{"RG",8},{"RT",9},
        {"LEDG",10},{"LE",10},{"REDG",11},{"RE",11},{"DT",12},{"SAM",13},{"LOLB",13},{"MIKE",14},{"MLB",14},{"WILL",15},{"ROLB",15},
        {"CB",16},{"FS",17},{"SS",18},{"K",19},{"P",20}};
    auto it = m.find(s); return it == m.end() ? -1 : it->second;
}
const int kMinimum[21] = {2,2,1,5,2,1,1,1,1,1,1,1,2,1,1,1,4,1,1,1,1};
const std::vector<std::vector<int>> kNear = {{0},{1,2,3},{2,1,4},{3,1,4},{4,2,3,5},{5,9,6,8,7},{6,8,7,5,9},{7,6,8},{8,6,7,9,5},{9,5,8,6,7},
    {10,11,12,13,15},{11,10,12,15,13},{12,10,11},{13,15,14,10},{14,13,15},{15,13,14,11},{16,17,18},{17,18,16},{18,17,16,14},{19,20},{20,19}};
// game field <- ratings-file column(s) (averaged)
const std::vector<std::pair<const char*, std::vector<const char*>>> kRatings = {
    {"POVR",{"OVERALL"}},{"PSPD",{"SPEED"}},{"PACC",{"ACCELERATION"}},{"PAGI",{"AGILITY"}},{"PSTR",{"STRENGTH"}},{"PAWR",{"AWARENESS"}},
    {"PJMP",{"JUMPING"}},{"PSTA",{"STAMINA"}},{"PINJ",{"INJURY"}},{"PTGH",{"TOUGHNESS"}},{"PCTH",{"CATCHING"}},{"PCAR",{"CARRYING"}},
    {"PTHP",{"THROWPOWER"}},{"PTHA",{"THROWACCURACYSHORT","THROWACCURACYMID","THROWACCURACYDEEP"}},{"PTAS",{"THROWACCURACYSHORT"}},
    {"PTAM",{"THROWACCURACYMID"}},{"PTAD",{"THROWACCURACYDEEP"}},{"PTOR",{"THROWONTHERUN"}},{"PPLA",{"PLAYACTION"}},{"PKPR",{"KICKPOWER"}},
    {"PKAC",{"KICKACCURACY"}},{"PKRT",{"KICKRETURN"}},{"PTAK",{"TACKLE"}},{"PLHT",{"HITPOWER"}},{"PLPU",{"PURSUIT"}},{"PLPE",{"PLAYRECOGNITION"}},
    {"PLMC",{"MANCOVERAGE"}},{"PLZC",{"ZONECOVERAGE"}},{"PLPR",{"PRESS"}},{"PLRL",{"RELEASE"}},
    {"PLRR",{"SHORTROUTERUNNING","MEDIUMROUTERUNNING","DEEPROUTERUNNING"}},{"PLSC",{"SPECTACULARCATCH"}},{"PLCI",{"CATCHINTRAFFIC"}},
    {"PBCV",{"BCVISION"}},{"PLJM",{"JUKEMOVE"}},{"PLSM",{"SPINMOVE"}},{"PLSA",{"STIFFARM"}},{"PLTR",{"TRUCKING"}},
    {"PELU",{"JUKEMOVE","SPINMOVE","CHANGEOFDIRECTION"}},{"PBSG",{"BLOCKSHEDDING"}},{"PFMS",{"FINESSEMOVES"}},{"PLPm",{"POWERMOVES"}},
    {"PPBK",{"PASSBLOCK"}},{"PPBF",{"PASSBLOCKFINESSE"}},{"PPBS",{"PASSBLOCKPOWER"}},{"PRBK",{"RUNBLOCK"}},{"PRBF",{"RUNBLOCKFINESSE"}},
    {"PRBS",{"RUNBLOCKPOWER"}},{"PLIB",{"IMPACTBLOCKING"}},{"PLPO",{"OVERALL"}}};

std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string Key(const std::string& f, const std::string& l) {
    auto clean = [](std::string s) { std::string o; for (char c : s) if (c != '.') o += (char)tolower((unsigned char)c);
                                     size_t a = o.find_first_not_of(' '), b = o.find_last_not_of(' '); return a == std::string::npos ? std::string() : o.substr(a, b - a + 1); };
    return clean(f) + "|" + clean(l);
}
std::string Latin1(const std::string& s, size_t n) {   // UTF-8 -> Latin-1, cut to the field size
    std::string o;
    for (size_t i = 0; i < s.size() && o.size() < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) o += (char)c;
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { unsigned v = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i++; if (v < 256) o += (char)v; }
        else { while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80) i++; }
    }
    return o;
}
std::vector<std::string> SplitLine(const std::string& line, char sep) {   // CSV with quotes / TSV
    std::vector<std::string> out; std::string cur; bool q = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (sep == ',' && c == '"') { if (q && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; } else q = !q; }
        else if (c == sep && !q) { out.push_back(cur); cur.clear(); }
        else if (c != '\r' && c != '\n') cur += c;
    }
    out.push_back(cur); return out;
}
std::vector<std::map<std::string, std::string>> ReadTable(const std::string& path, char sep) {
    std::vector<std::map<std::string, std::string>> rows;
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return rows;
    std::string all; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) all.append(b, n); fclose(f);
    if (all.size() >= 3 && (unsigned char)all[0] == 0xEF) all.erase(0, 3);   // UTF-8 BOM
    std::vector<std::string> head; size_t i = 0;
    while (i < all.size()) {
        size_t e = all.find('\n', i); if (e == std::string::npos) e = all.size();
        std::string line = all.substr(i, e - i); i = e + 1;
        if (line.empty() || line[0] == '#') continue;
        auto cols = SplitLine(line, sep);
        if (head.empty()) { head = cols; continue; }
        std::map<std::string, std::string> r; for (size_t k = 0; k < head.size() && k < cols.size(); k++) r[head[k]] = cols[k];
        rows.push_back(std::move(r));
    }
    return rows;
}
int Num(const std::map<std::string, std::string>& r, const char* k, int def = 0) { auto it = r.find(k); if (it == r.end() || it->second.empty()) return def; return atoi(it->second.c_str()); }
std::string S(const std::map<std::string, std::string>& r, const char* k) { auto it = r.find(k); return it == r.end() ? std::string() : it->second; }

uint64_t Fnv(const std::string& s, uint64_t h = 1469598103934665603ull) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } return h; }
std::string FileText(const std::string& p) { std::string s; FILE* f = fopen(p.c_str(), "rb"); if (!f) return s; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }

struct Player {
    std::map<std::string, std::string> fact; const std::map<std::string, std::string>* rate = nullptr;
    int pos = -1, team = 0, ovr = 0; std::string key;
};
}   // namespace

// Build (if needed) the roster set <set> from <gameDir>/qkl_boot.ast. Returns false on a problem (the game then uses the
// set's previous build, if any, or the original roster).
// outDir (default setDir): where the result goes; withRatings=false leaves the player's ratings file out (online
// games: every PC builds the identical roster from the shared facts).
bool BuildRosterSet(const std::string& gameDir, const std::string& setDir, const std::string& outDir, bool withRatings) {
    namespace fs = std::filesystem;
    std::string factsPath = setDir + "/roster.tsv", ratingsPath = withRatings ? setDir + "/ratings.csv" : std::string(), editsPath = setDir + "/roster_edits.txt";
    std::string out = outDir + "/qkl_boot.ast", src = gameDir + "/qkl_boot.ast";
    std::error_code mk; fs::create_directories(outDir, mk);
    if (!fs::exists(factsPath)) return fs::exists(out);   // a set made with the dev tools: nothing to build
    std::error_code ec; auto gsz = fs::file_size(src, ec);
    char stamp[64]; uint64_t h = Fnv(FileText(factsPath)); h = Fnv(FileText(ratingsPath), h); h = Fnv(FileText(editsPath), h); h = Fnv(std::to_string(gsz), h);
    snprintf(stamp, sizeof stamp, "v2 %016llx", (unsigned long long)h);
    if (fs::exists(out) && FileText(outDir + "/built.txt").rfind(stamp, 0) == 0) return true;

    fprintf(stderr, "[mods] roster: building %s from your game files...\n", setDir.c_str());
    gdb::Archive ar; if (!ar.Open(src)) { fprintf(stderr, "[mods] roster: cannot read %s\n", src.c_str()); return false; }
    auto facts = ReadTable(factsPath, '\t');
    auto ratings = ReadTable(ratingsPath, ',');
    std::map<std::string, const std::map<std::string, std::string>*> rateBy;
    for (auto& r : ratings) {   // by name + team (two players can share a name), and by name alone
        rateBy[Key(S(r, "First Name"), S(r, "Last Name")) + "#" + S(r, "Team ID")] = &r;
        rateBy.emplace(Key(S(r, "First Name"), S(r, "Last Name")), &r);
    }

    std::vector<uint8_t> raw;
    if (!ar.Read(70, raw)) return false;
    Db master; if (!master.Load(raw)) return false;
    Table* MP = master.T("PLAY"); if (!MP) return false;
    std::map<std::string, int> known; std::map<uint32_t, int> mindex;
    for (int i = 0; i < MP->used; i++) { known.emplace(Key(master.Str(*MP, i, "PFNA"), master.Str(*MP, i, "PLNA")), i); mindex[master.Get(*MP, i, "PGID")] = i; }

    std::map<int, std::vector<Player>> byTeam;
    for (auto& r : facts) {
        Player p; p.fact = r; p.team = Num(r, "team"); p.pos = PosId(S(r, "position")); p.key = Key(S(r, "first"), S(r, "last"));
        if (p.team <= 0 || p.pos < 0) continue;
        auto rt = rateBy.find(p.key + "#" + std::to_string(p.team)); if (rt == rateBy.end()) rt = rateBy.find(p.key);
        if (rt != rateBy.end()) p.rate = rt->second;
        if (p.rate) p.ovr = Num(*p.rate, "OVERALL");
        else { auto k = known.find(p.key); p.ovr = k != known.end() ? (int)master.Get(*MP, k->second, "POVR") : 55 + 2 * std::min(8, Num(r, "years_pro")); }
        byTeam[p.team].push_back(p);
    }
    int matchedRatings = 0; for (auto& [t, v] : byTeam) for (auto& p : v) if (p.rate) matchedRatings++;
    fprintf(stderr, "[mods] roster: %zu players, %d with ratings from your ratings file\n", facts.size(), matchedRatings);

    std::map<uint32_t, std::vector<uint8_t>> repl;
    std::map<int, std::map<std::string, double>> teamRatings;
    auto applyEdits = [&](Db& db, Table& T) {
        std::string ed = FileText(editsPath); size_t i = 0;
        while (i < ed.size()) {
            size_t e = ed.find('\n', i); if (e == std::string::npos) e = ed.size();
            std::string line = ed.substr(i, e - i); i = e + 1;
            if (line.empty() || line[0] == '#') continue;
            char fld[8] = {}; unsigned id = 0; char val[64] = {};
            if (sscanf(line.c_str(), "%u %7s %63[^\r\n]", &id, fld, val) != 3) continue;
            for (int r = 0; r < T.used; r++) if (db.Get(T, r, "PGID") == id) {
                const gdb::Field* f = T.F(fld); if (!f) break;
                if (f->type == 0) db.SetStr(T, r, fld, val); else db.Set(T, r, fld, (uint32_t)strtoul(val, nullptr, 10));
            }
        }
    };
    for (uint32_t e = 347; e <= 383; e++) {
        if (!ar.Read(e, raw)) continue;
        Db db; if (!db.Load(raw)) continue;
        Table* TT = db.T("TEAM"); Table* TP = db.T("PLAY"); Table* D = db.T("DCHT");
        if (!TT || !TP || !D || TP->used == 0) continue;
        int tid = (int)db.Get(*TT, 0, "TGID");
        auto bt = byTeam.find(tid); if (bt == byTeam.end()) continue;
        int spots = TP->used;
        // the roster: a minimum per position (best first), then the best of the rest
        std::vector<Player*> all; for (auto& p : bt->second) all.push_back(&p);
        std::stable_sort(all.begin(), all.end(), [](Player* a, Player* b) { return a->ovr > b->ovr; });
        std::vector<Player*> roster; std::set<Player*> taken;
        for (int pos = 0; pos < 21; pos++) { int got = 0; for (Player* p : all) if (p->pos == pos && got < kMinimum[pos] && !taken.count(p)) { roster.push_back(p); taken.insert(p); got++; } }
        for (Player* p : all) { if ((int)roster.size() >= spots) break; if (!taken.count(p)) { roster.push_back(p); taken.insert(p); } }
        if ((int)roster.size() > spots) roster.resize(spots);
        // spots: same player first, then the closest position
        std::vector<int> oldPos(spots); std::vector<std::string> oldKey(spots); std::vector<uint32_t> oldId(spots);
        for (int i = 0; i < spots; i++) { oldPos[i] = (int)db.Get(*TP, i, "PPOS"); oldKey[i] = Key(db.Str(*TP, i, "PFNA"), db.Str(*TP, i, "PLNA")); oldId[i] = db.Get(*TP, i, "PGID"); }
        std::vector<int> freeSpots; for (int i = 0; i < spots; i++) freeSpots.push_back(i);
        std::map<Player*, int> spotOf;
        for (Player* p : roster) for (size_t k = 0; k < freeSpots.size(); k++) if (oldKey[freeSpots[k]] == p->key) { spotOf[p] = freeSpots[k]; freeSpots.erase(freeSpots.begin() + k); break; }
        for (Player* p : roster) {
            if (spotOf.count(p)) continue;
            int best = -1;
            for (int nr : kNear[p->pos]) { for (size_t k = 0; k < freeSpots.size(); k++) if (oldPos[freeSpots[k]] == nr) { best = (int)k; break; } if (best >= 0) break; }
            if (best < 0) best = 0;
            spotOf[p] = freeSpots[best]; freeSpots.erase(freeSpots.begin() + best);
        }
        uint32_t rb = TP->recBytes;
        std::map<uint32_t, Player*> placed;
        for (Player* p : roster) {
            int i = spotOf[p]; uint32_t pgid = oldId[i];
            auto k = known.find(p->key);
            if (k != known.end() && MP->recBytes == rb) memcpy(db.Row(*TP, i), master.Row(*MP, k->second), rb);   // his own record (look)
            db.Set(*TP, i, "PGID", pgid); db.Set(*TP, i, "TGID", tid);
            db.SetStr(*TP, i, "PFNA", Latin1(S(p->fact, "first"), 12)); db.SetStr(*TP, i, "PLNA", Latin1(S(p->fact, "last"), 17));
            db.Set(*TP, i, "PPOS", p->pos); db.Set(*TP, i, "POPS", p->pos);
            db.Set(*TP, i, "PJEN", std::min(127, Num(p->fact, "jersey")));
            db.Set(*TP, i, "PHGT", std::min(127, Num(p->fact, "height", 72)));
            db.Set(*TP, i, "PWGT", std::max(0, std::min(255, Num(p->fact, "weight", 220) - 160)));
            db.Set(*TP, i, "PAGE", std::min(63, Num(p->fact, "age", 25)));
            db.Set(*TP, i, "PYRP", std::min(31, Num(p->fact, "years_pro")));
            if (p->rate) for (auto& [field, cols] : kRatings) {
                int sum = 0; for (auto c : cols) sum += Num(*p->rate, c);
                db.Set(*TP, i, field, (uint32_t)std::max(0, std::min(99, (int)std::lround((double)sum / cols.size()))));
            }
            placed[pgid] = p;
        }
        applyEdits(db, *TP);
        // depth chart: keep its rows, re-rank each position's players
        auto rating = [&](uint32_t pg, const char* field) { for (int i = 0; i < TP->used; i++) if (db.Get(*TP, i, "PGID") == pg) return (int)db.Get(*TP, i, field); return 0; };
        std::map<int, std::vector<int>> groups;
        for (int k = 0; k < D->used; k++) groups[(int)db.Get(*D, k, "PPOS")].push_back(k);
        for (auto& [pos, ks] : groups) std::stable_sort(ks.begin(), ks.end(), [&](int a, int b) { return db.Get(*D, a, "ddep") < db.Get(*D, b, "ddep"); });
        auto nStart = [](int pos) { return pos == 3 ? 3 : pos == 12 ? 2 : pos == 16 ? 2 : 1; };
        std::map<int, std::vector<uint32_t>> own;
        for (auto& [pg, p] : placed) own[p->pos].push_back(pg);
        for (auto& [pos, v] : own) std::stable_sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return rating(a, "POVR") > rating(b, "POVR"); });
        std::map<int, std::vector<uint32_t>> starters; std::set<uint32_t> starting;
        for (int pos = 0; pos < 21; pos++) {
            int need = std::min(nStart(pos), (int)groups[pos].size());
            for (uint32_t pg : own[pos]) { if ((int)starters[pos].size() >= need) break; starters[pos].push_back(pg); starting.insert(pg); }
        }
        for (int pos = 0; pos < 21; pos++) {
            int need = std::min(nStart(pos), (int)groups[pos].size());
            for (size_t n = 1; n < kNear[pos].size(); n++) for (uint32_t pg : own[kNear[pos][n]]) {
                if ((int)starters[pos].size() >= need) break;
                if (!starting.count(pg)) { starters[pos].push_back(pg); starting.insert(pg); }
            }
        }
        for (auto& [pos, ks] : groups) {
            std::vector<uint32_t> ordered;
            if (pos <= 20) {
                ordered = starters[pos];
                for (int nr : kNear[pos]) for (uint32_t pg : own[nr]) if (std::find(ordered.begin(), ordered.end(), pg) == ordered.end()) ordered.push_back(pg);
            } else {   // 21 KR, 22 PR, 23 KOS, 24 LS, 25 3DRB
                const char* col = pos <= 22 ? "PKRT" : pos == 23 ? "PKPR" : "POVR";
                std::vector<int> nr = pos == 21 ? std::vector<int>{1,3,16,17} : pos == 22 ? std::vector<int>{3,1,16,17} : pos == 23 ? std::vector<int>{19,20} : pos == 24 ? std::vector<int>{7,6,8} : std::vector<int>{1,2};
                std::set<uint32_t> c; for (int n : nr) for (uint32_t pg : own[n]) c.insert(pg);
                ordered.assign(c.begin(), c.end());
                std::stable_sort(ordered.begin(), ordered.end(), [&](uint32_t a, uint32_t b) { return rating(a, col) > rating(b, col); });
            }
            for (size_t depth = 0; depth < ks.size(); depth++) {
                if (depth < ordered.size()) db.Set(*D, ks[depth], "PGID", ordered[depth]);
                db.Set(*D, ks[depth], "ddep", (uint32_t)depth);
            }
        }
        // the same records in the main player table
        for (int i = 0; i < TP->used; i++) {
            auto mi = mindex.find(db.Get(*TP, i, "PGID"));
            if (mi != mindex.end() && MP->recBytes == rb) memcpy(master.Row(*MP, mi->second), db.Row(*TP, i), rb);
        }
        // team ratings: averages of the starters' overalls
        std::map<std::string, std::vector<int>> st;
        for (int k = 0; k < D->used; k++) {
            int pos = (int)db.Get(*D, k, "PPOS"), dep = (int)db.Get(*D, k, "ddep");
            if (pos <= 20 && dep < nStart(pos)) { uint32_t pg = db.Get(*D, k, "PGID"); if (placed.count(pg)) st[kPos[pos]].push_back(rating(pg, "POVR")); }
        }
        auto tavg = [&](std::vector<const char*> ps) { double s = 0; int n = 0; for (auto q : ps) for (int x : st[q]) { s += x; n++; } return n ? s / n : 0.0; };
        teamRatings[tid] = {{"TRQB", tavg({"QB"})}, {"TRRB", tavg({"HB","FB"})}, {"TRWR", tavg({"WR"})}, {"TRTE", tavg({"TE"})},
            {"TROL", tavg({"LT","LG","C","RG","RT"})}, {"TRDL", tavg({"LE","RE","DT"})}, {"TRLB", tavg({"LOLB","MLB","ROLB"})},
            {"TRDB", tavg({"CB","FS","SS"})}, {"TROF", tavg({"QB","HB","FB","WR","TE","LT","LG","C","RG","RT"})},
            {"TRDE", tavg({"LE","RE","DT","LOLB","MLB","ROLB","CB","FS","SS"})},
            {"TROV", tavg({"QB","HB","FB","WR","TE","LT","LG","C","RG","RT","LE","RE","DT","LOLB","MLB","ROLB","CB","FS","SS","K","P"})}};
        db.FixChecksums();
        repl[e] = db.d;
    }
    applyEdits(master, *MP);
    // scale team ratings to the spread of the game's own team list
    std::map<std::string, std::vector<double>> orig;
    if (ar.Read(384, raw)) { Db odb; if (odb.Load(raw)) if (Table* T = odb.T("TEAM")) for (int i = 0; i < T->used; i++) {
        int t = (int)odb.Get(*T, i, "TGID"); if (t < 1 || t > 32) continue;
        for (auto& [f, v] : teamRatings.begin()->second) if (T->F(f.c_str())) orig[f].push_back(odb.Get(*T, i, f.c_str()));
    } }
    if (!teamRatings.empty()) for (auto& [f, ov] : orig) {
        std::vector<double> nv; for (auto& [t, m] : teamRatings) nv.push_back(m[f]);
        if (ov.size() < 8 || nv.size() < 8) continue;
        auto stats = [](const std::vector<double>& v) { double m = 0; for (double x : v) m += x; m /= v.size(); double sd = 0; for (double x : v) sd += (x - m) * (x - m); return std::make_pair(m, std::sqrt(sd / v.size())); };
        auto [om, osd] = stats(ov); auto [nm, nsd] = stats(nv);
        if (nsd <= 0 || osd <= 0) continue;
        for (auto& [t, m] : teamRatings) m[f] = std::max(40.0, std::min(99.0, std::round(om + (m[f] - nm) * osd / nsd)));
    }
    // team ratings in every database with a TEAM table
    for (uint32_t e = 0; e < std::min<uint32_t>(400, ar.cnt); e++) {
        Db db; bool isMaster = e == 70;
        if (isMaster) db = master;
        else if (repl.count(e)) { if (!db.Load(repl[e])) continue; }
        else { if (!ar.Read(e, raw) || raw.size() < 2 || raw[0] != 'D' || raw[1] != 'B' || !db.Load(raw)) continue; }
        Table* T = db.T("TEAM"); if (!T) continue;
        int hit = 0;
        for (int i = 0; i < T->used; i++) {
            auto tr = teamRatings.find((int)db.Get(*T, i, "TGID")); if (tr == teamRatings.end()) continue;
            for (auto& [f, v] : tr->second) if (T->F(f.c_str())) db.Set(*T, i, f.c_str(), (uint32_t)std::lround(v));
            hit++;
        }
        if (!hit) continue;
        db.FixChecksums();
        if (isMaster) master = db; else repl[e] = db.d;
    }
    master.FixChecksums();
    repl[70] = master.d;
    if (!ar.WritePatched(out, repl)) { fprintf(stderr, "[mods] roster: could not write %s\n", out.c_str()); return false; }
    if (FILE* f = fopen((outDir + "/built.txt").c_str(), "wb")) { fprintf(f, "%s\n", stamp); fclose(f); }
    fprintf(stderr, "[mods] roster: built %s (%zu teams)\n", out.c_str(), teamRatings.size());
    return true;
}

bool BuildRosterSet(const std::string& gameDir, const std::string& setDir) { return BuildRosterSet(gameDir, setDir, setDir, true); }
