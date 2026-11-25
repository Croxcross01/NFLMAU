// Playbook editing core (see playbook_core.h).
#include "playbook_core.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pb {

static uint32_t BE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void PutBE32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
static void PutBE16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }

// ---------------------------------------------------------------- tables
int Table::F(const std::string& n) const {
    for (size_t i = 0; i < fields.size(); i++) if (fields[i].name == n) return int(i);
    return -1;
}
uint32_t Table::Get(size_t row, int f) const {
    if (f < 0 || row >= rows.size()) return 0;
    const Field& fd = fields[f];
    const uint8_t* r = rows[row].data();
    uint64_t v = 0;
    for (uint32_t q = 0; q < fd.bits && q < 32; q++) { uint32_t b = fd.bit + q; v = (v << 1) | ((r[b >> 3] >> (7 - (b & 7))) & 1); }
    return uint32_t(v);
}
std::string Table::Str(size_t row, int f) const {
    if (f < 0 || row >= rows.size()) return {};
    const Field& fd = fields[f];
    std::string s;
    for (uint32_t q = 0; q < fd.bits / 8; q++) { char c = char(rows[row][fd.bit / 8 + q]); if (!c) break; s += c; }
    return s;
}
bool Table::Set(size_t row, int f, uint32_t v) {
    if (f < 0 || row >= rows.size()) return false;
    const Field& fd = fields[f];
    if (fd.type == 0 || (fd.bits < 32 && v >> fd.bits)) return false;
    uint8_t* r = rows[row].data();
    for (uint32_t q = 0; q < fd.bits; q++) {
        uint32_t b = fd.bit + q; bool on = (v >> (fd.bits - 1 - q)) & 1;
        if (on) r[b >> 3] |= uint8_t(0x80 >> (b & 7)); else r[b >> 3] &= uint8_t(~(0x80 >> (b & 7)));
    }
    return true;
}
bool Table::SetStr(size_t row, int f, const std::string& s) {
    if (f < 0 || row >= rows.size() || fields[f].type != 0) return false;
    size_t n = fields[f].bits / 8;
    if (s.size() > n) return false;
    uint8_t* p = rows[row].data() + fields[f].bit / 8;
    memset(p, 0, n); memcpy(p, s.data(), s.size());
    return true;
}

// ---------------------------------------------------------------- database
Table* Db::T(const char* n) { for (auto& t : tables) if (t.name == n || (t.name.size() == 4 && t.name[3] == 0 && t.name.compare(0, 3, n) == 0 && strlen(n) == 3)) return &t; return nullptr; }
const Table* Db::T(const char* n) const { return const_cast<Db*>(this)->T(n); }

bool Db::Parse(const std::vector<uint8_t>& d, std::string& err) {
    tables.clear(); indexOrder.clear(); trailer.clear();
    if (d.size() < 28 || d[0] != 'D' || d[1] != 'B') { err = "not a TDB database"; return false; }
    head.assign(d.begin(), d.begin() + 24);
    uint32_t n = BE32(&d[16]);
    size_t base = 24 + size_t(n) * 8;
    if (!n || base > d.size()) { err = "bad table count"; return false; }
    struct Ix { std::string name; size_t at; int pos; };
    std::vector<Ix> ix;
    for (uint32_t i = 0; i < n; i++) {
        std::string nm; for (int k = 3; k >= 0; k--) nm += char(d[24 + i * 8 + k]);
        ix.push_back({nm, base + BE32(&d[28 + i * 8]), int(i)});
    }
    std::vector<Ix> st = ix;
    std::sort(st.begin(), st.end(), [](const Ix& a, const Ix& b) { return a.at < b.at; });
    std::map<int, int> posOf;   // index entry -> storage position
    for (size_t k = 0; k < st.size(); k++) {
        size_t at = st[k].at, next = k + 1 < st.size() ? st[k + 1].at : d.size() - 4;
        if (at + 40 > d.size() || next > d.size() || next < at + 40) { err = "bad table offset"; return false; }
        Table t; t.name = st[k].name;
        memcpy(t.header, &d[at], 40);
        t.rb = BE32(&d[at + 8]);
        t.capacity = uint32_t(d[at + 20]) << 8 | d[at + 21];
        uint32_t used = uint32_t(d[at + 22]) << 8 | d[at + 23];
        int nf = d[at + 28];
        size_t p = at + 40;
        if (p + size_t(nf) * 16 > next) { err = "bad field list in " + t.name; return false; }
        t.fieldDefs.assign(d.begin() + p, d.begin() + p + nf * 16);
        for (int q = 0; q < nf; q++, p += 16) {
            Field f; f.type = BE32(&d[p]); f.bit = BE32(&d[p + 4]); f.bits = BE32(&d[p + 12]);
            for (int c = 3; c >= 0; c--) { char ch = char(d[p + 8 + c]); f.name += ch; }
            while (!f.name.empty() && f.name.back() == 0) f.name.pop_back();
            t.fields.push_back(f);
        }
        size_t recEnd = p + size_t(t.capacity) * t.rb;
        if (used > t.capacity || recEnd > next) { err = "bad record area in " + t.name; return false; }
        for (uint32_t r = 0; r < used; r++) t.rows.emplace_back(d.begin() + p + r * t.rb, d.begin() + p + (r + 1) * t.rb);
        if (k + 1 < st.size()) t.after.assign(d.begin() + recEnd, d.begin() + next);
        else if (next >= recEnd + 4) trailer.assign(d.begin() + recEnd + 4, d.begin() + next);   // playbooks: crc, extra block, crc
        posOf[st[k].pos] = int(tables.size());
        tables.push_back(std::move(t));
    }
    for (uint32_t i = 0; i < n; i++) indexOrder.push_back(posOf[int(i)]);
    return true;
}

std::vector<uint8_t> Db::Build() const {
    size_t n = tables.size(), base = 24 + n * 8;
    std::vector<size_t> offs(n);
    size_t pos = base;
    for (size_t k = 0; k < n; k++) {
        const Table& t = tables[k];
        offs[k] = pos;
        uint32_t cap = std::max<uint32_t>(t.capacity, uint32_t(t.rows.size()));
        pos += 40 + t.fieldDefs.size() + size_t(cap) * t.rb + t.after.size();
    }
    // after the last table: its checksum (the database ends there: size field), then - in playbooks - an extra block
    // kept as read and the checksum again
    size_t total = pos + 4 + (trailer.empty() ? 0 : trailer.size() + 4);
    std::vector<uint8_t> d(total, 0);
    memcpy(d.data(), head.data(), 24);
    PutBE32(d.data() + 8, uint32_t(pos + 4));
    PutBE32(d.data() + 16, uint32_t(n));
    for (size_t i = 0; i < n; i++) {
        const Table& t = tables[indexOrder[i]];
        for (int c = 0; c < 4; c++) d[24 + i * 8 + 3 - c] = c < int(t.name.size()) ? uint8_t(t.name[c]) : 0;
        PutBE32(&d[28 + i * 8], uint32_t(offs[indexOrder[i]] - base));
    }
    for (size_t k = 0; k < n; k++) {
        const Table& t = tables[k];
        uint8_t* p = &d[offs[k]];
        uint32_t cap = std::max<uint32_t>(t.capacity, uint32_t(t.rows.size()));
        memcpy(p, t.header, 40);
        PutBE16(p + 20, cap); PutBE16(p + 22, uint32_t(t.rows.size()));
        p += 40;
        if (!t.fieldDefs.empty()) memcpy(p, t.fieldDefs.data(), t.fieldDefs.size());
        p += t.fieldDefs.size();
        for (auto& r : t.rows) { memcpy(p, r.data(), t.rb); p += t.rb; }
        p += size_t(cap - t.rows.size()) * t.rb;
        if (!t.after.empty()) memcpy(p, t.after.data(), t.after.size());
    }
    if (!trailer.empty()) memcpy(&d[pos + 4], trailer.data(), trailer.size());
    FixChecksums(d);
    return d;
}

static uint32_t g_crc[256];
static uint32_t Crc(const uint8_t* p, size_t n) {
    if (!g_crc[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i << 24; for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1; g_crc[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = (c << 8) ^ g_crc[((c >> 24) ^ p[i]) & 0xFF];
    return c;
}
void FixChecksums(std::vector<uint8_t>& d) {
    uint32_t n = BE32(&d[16]);
    size_t base = 24 + size_t(n) * 8;
    std::vector<size_t> offs;
    for (uint32_t i = 0; i < n; i++) offs.push_back(base + BE32(&d[28 + i * 8]));
    for (size_t o : offs) PutBE32(&d[o + 36], Crc(&d[o + 4], 32));   // each table header: crc of its bytes 4..36 (counts)
    PutBE32(&d[20], Crc(d.data(), 20));
    PutBE32(&d[offs[0]], Crc(d.data() + 24, base - 24));
    for (uint32_t i = 1; i < n; i++) PutBE32(&d[offs[i]], Crc(d.data() + offs[i - 1] + 40, offs[i] - offs[i - 1] - 40));
    size_t end = BE32(&d[8]) - 4;   // the last table ends where the database size says (before its checksum)
    if (end < offs[n - 1] + 40 || end + 4 > d.size()) end = d.size() - 4;
    uint32_t c = Crc(d.data() + offs[n - 1] + 40, end - offs[n - 1] - 40);
    PutBE32(&d[end], c);
    PutBE32(&d[d.size() - 4], c);
}

// ---------------------------------------------------------------- reading helpers
int SignedField(uint32_t v, uint32_t bits) { v &= 0xFF; (void)bits; return v > 127 ? int(v) - 256 : int(v); }

std::vector<Step> StepsOf(const Db& db, uint32_t psal) {
    std::vector<std::pair<uint32_t, Step>> v;
    const Table* t = db.T("PSAL");
    if (!t) return {};
    int fP = t->F("PSAL"), fS = t->F("step"), fc = t->F("code"), f1 = t->F("val1"), f2 = t->F("val2"), f3 = t->F("val3");
    for (size_t r = 0; r < t->rows.size(); r++)
        if (t->Get(r, fP) == psal) v.push_back({t->Get(r, fS), Step{t->Get(r, fc), t->Get(r, f1), t->Get(r, f2), t->Get(r, f3)}});
    std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<Step> out;
    for (auto& s : v) { if (s.second.code == 255) break; out.push_back(s.second); }
    return out;
}
static std::string Key2(const char* a, int i) { char b[8]; snprintf(b, sizeof b, "%s%02d", a, i); return b; }
std::vector<ArtPt> ArtOf(const Db& db, uint32_t artl) {
    const Table* t = db.T("ARTL");
    std::vector<ArtPt> out;
    if (!t || !artl) return out;
    int fA = t->F("ARTL");
    for (size_t r = 0; r < t->rows.size(); r++) {
        if (t->Get(r, fA) != artl) continue;
        int cnt = std::min<int>(12, int(t->Get(r, "acnt")));
        for (int i = 1; i <= cnt; i++)
            out.push_back({SignedField(t->Get(r, Key2("ax", i).c_str()), 8), SignedField(t->Get(r, Key2("ay", i).c_str()), 8), t->Get(r, Key2("ct", i).c_str())});
        break;
    }
    return out;
}

const char* StepName(uint32_t c) {
    switch (c) {
    case 1: return "Run to end zone"; case 2: return "Chase ball"; case 3: return "Move"; case 4: return "Move (fixed speed)";
    case 5: return "Face direction"; case 7: return "QB drop / scramble"; case 8: return "Route"; case 9: return "Route cut";
    case 10: return "Get open"; case 11: return "Pitch"; case 12: return "Option handoff"; case 13: return "Take handoff";
    case 14: return "Pass block"; case 15: return "Run block"; case 16: return "Kickoff"; case 18: return "Lead block";
    case 19: return "Man coverage"; case 20: return "Flat zone"; case 21: return "Hook / middle zone"; case 22: return "Curl / seam zone";
    case 23: return "Deep zone"; case 24: return "Pass rush"; case 25: return "Wait"; case 26: return "Start animation";
    case 32: return "Take pitch"; case 34: return "QB spy"; case 35: return "Route (head turn)"; case 36: return "Option route";
    case 37: return "Option route coverages"; case 40: return "Fake handoff (QB)"; case 41: return "Rush QB"; case 42: return "Fake handoff";
    case 43: return "Option follow"; case 45: return "Motion"; case 46: return "Motion at snap"; case 47: return "Defense motion";
    case 48: return "Line up offset"; case 57: return "Line up vs formation"; case 58: return "Set animation"; case 255: return "End";
    }
    return "Step";
}

// ---------------------------------------------------------------- edits
static std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> v; size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) i++;
        if (i >= line.size() || line[i] == '#') break;
        std::string t;
        if (line[i] == '"') { i++; while (i < line.size() && line[i] != '"') t += line[i++]; i++; }
        else while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') t += line[i++];
        v.push_back(t);
    }
    return v;
}
static bool Num(const std::string& s, uint32_t& v) { char* e = nullptr; long long x = strtoll(s.c_str(), &e, 10); if (s.empty() || *e) return false; v = uint32_t(x); return true; }
static int RowWhere(const Table* t, const char* f, uint32_t v) {
    if (!t) return -1; int fi = t->F(f); if (fi < 0) return -1;
    for (size_t r = 0; r < t->rows.size(); r++) if (t->Get(r, fi) == v) return int(r);
    return -1;
}
static uint32_t MaxOf(const Table* t, const char* f) {
    uint32_t m = 0; if (!t) return 0; int fi = t->F(f);
    for (size_t r = 0; r < t->rows.size(); r++) m = std::max(m, t->Get(r, fi));
    return m;
}
// insert a row keeping the table ordered by field f (the game's tables that are sorted by their id stay sorted)
static size_t InsertSorted(Table* t, const char* f, std::vector<uint8_t> rec) {
    int fi = t->F(f);
    Table tmp; tmp.fields = t->fields; tmp.rb = t->rb; tmp.rows.push_back(rec);
    uint32_t key = tmp.Get(0, fi);
    size_t at = t->rows.size();
    while (at > 0 && t->Get(at - 1, fi) > key) at--;
    t->rows.insert(t->rows.begin() + at, std::move(rec));
    return at;
}
static void CopyRows(Table* t, const char* keyField, uint32_t from, uint32_t to, bool sorted, const char* sortField) {
    if (!t) return;
    int fk = t->F(keyField);
    std::vector<std::vector<uint8_t>> add;
    for (size_t r = 0; r < t->rows.size(); r++) if (t->Get(r, fk) == from) add.push_back(t->rows[r]);
    for (auto& rec : add) {
        Table tmp; tmp.fields = t->fields; tmp.rb = t->rb; tmp.rows.push_back(rec); tmp.Set(0, fk, to);
        if (sorted) InsertSorted(t, sortField, tmp.rows[0]); else t->rows.push_back(tmp.rows[0]);
    }
}
static void DeleteWhere(Table* t, const char* f, uint32_t v) {
    if (!t) return; int fi = t->F(f); if (fi < 0) return;
    for (size_t r = t->rows.size(); r-- > 0;) if (t->Get(r, fi) == v) t->rows.erase(t->rows.begin() + r);
}

int ApplyOne(Db& db, const std::string& line, std::string& log) {
    std::vector<std::string> a = Tokens(line);
    if (a.empty()) return 0;
    auto bad = [&](const std::string& why) { log += "playbook edit skipped (" + why + "): " + line + "\n"; return 0; };
    const std::string& op = a[0];
    Table *PBPL = db.T("PBPL"), *PLYL = db.T("PLYL"), *PLYS = db.T("PLYS"), *PSAL = db.T("PSAL"), *ARTL = db.T("ARTL");
    if (!PBPL || !PLYL || !PLYS || !PSAL || !ARTL) return bad("not a playbook");
    if (op == "copy") {
        uint32_t src, nP, nB, set;
        if (a.size() < 6 || !Num(a[1], src) || !Num(a[2], nP) || !Num(a[3], nB) || !Num(a[4], set)) return bad("copy <PBPL> <newPLYL> <newPBPL> <PBST> <name>");
        int br = RowWhere(PBPL, "PBPL", src);
        if (br < 0) return bad("no such play");
        if (RowWhere(PLYL, "PLYL", nP) >= 0 || RowWhere(PBPL, "PBPL", nB) >= 0) return bad("new id already used");
        uint32_t play = PBPL->Get(br, "PBPL") == src ? PBPL->Get(br, "PLYL") : 0;
        int pr = RowWhere(PLYL, "PLYL", play);
        if (pr < 0) return bad("play data missing");
        {   // the play
            std::vector<uint8_t> rec = PLYL->rows[pr];
            Table tmp; tmp.fields = PLYL->fields; tmp.rb = PLYL->rb; tmp.rows.push_back(rec);
            if (!tmp.Set(0, "PLYL", nP)) return bad("play id too big");
            InsertSorted(PLYL, "PLYL", tmp.rows[0]);
        }
        CopyRows(PLYS, "PLYL", play, nP, false, nullptr);
        CopyRows(db.T("PLPD"), "PLYL", play, nP, true, "PLYL");
        CopyRows(db.T("PLRD"), "PLYL", play, nP, false, nullptr);
        CopyRows(db.T("PLCM"), "PLYL", play, nP, false, nullptr);
        {   // the playbook entry, last in its set
            uint32_t ord = 0; int fS = PBPL->F("PBST"), fO = PBPL->F("ord_");
            for (size_t r = 0; r < PBPL->rows.size(); r++) if (PBPL->Get(r, fS) == set) ord = std::max(ord, PBPL->Get(r, fO));
            std::vector<uint8_t> rec = PBPL->rows[br];
            PBPL->rows.push_back(rec);
            size_t r = PBPL->rows.size() - 1;
            PBPL->Set(r, "PBPL", nB); PBPL->Set(r, "PLYL", nP); PBPL->Set(r, "PBST", set);
            PBPL->Set(r, fO, std::min<uint32_t>(ord + 1, 63));
            PBPL->SetStr(r, PBPL->F("name"), a[5].substr(0, 23));
        }
        CopyRows(db.T("PBAI"), "PBPL", src, nB, true, "PBPL");
        return 1;
    }
    if (op == "name") {
        uint32_t id; if (a.size() < 3 || !Num(a[1], id)) return bad("name <PBPL> <name>");
        int r = RowWhere(PBPL, "PBPL", id); if (r < 0) return bad("no such play");
        if (!PBPL->SetStr(r, PBPL->F("name"), a[2].substr(0, 23))) return bad("name too long");
        return 1;
    }
    if (op == "move") {
        uint32_t id, set, ord; if (a.size() < 4 || !Num(a[1], id) || !Num(a[2], set) || !Num(a[3], ord)) return bad("move <PBPL> <PBST> <ord>");
        int r = RowWhere(PBPL, "PBPL", id); if (r < 0) return bad("no such play");
        PBPL->Set(r, "PBST", set); PBPL->Set(r, "ord_", std::min<uint32_t>(ord, 63));
        return 1;
    }
    if (op == "remove") {
        uint32_t id; if (a.size() < 2 || !Num(a[1], id)) return bad("remove <PBPL>");
        if (RowWhere(PBPL, "PBPL", id) < 0) return bad("no such play");
        DeleteWhere(PBPL, "PBPL", id); DeleteWhere(db.T("PBAI"), "PBPL", id); DeleteWhere(db.T("PBAU"), "PBPL", id);
        return 1;
    }
    if (op == "steps" || op == "art") {
        uint32_t play, poso; if (a.size() < 3 || !Num(a[1], play) || !Num(a[2], poso)) return bad(op + " <PLYL> <poso> ...");
        int fPl = PLYS->F("PLYL"), fPo = PLYS->F("poso"), row = -1;
        for (size_t r = 0; r < PLYS->rows.size(); r++) if (PLYS->Get(r, fPl) == play && PLYS->Get(r, fPo) == poso) { row = int(r); break; }
        if (row < 0) return bad("no such player in the play");
        std::vector<std::vector<uint32_t>> items;
        std::string list = a.size() > 3 ? a[3] : "";
        for (size_t i = 0; i < list.size();) {
            size_t e = list.find(';', i); if (e == std::string::npos) e = list.size();
            std::string it = list.substr(i, e - i); i = e + 1;
            if (it.empty()) continue;
            std::vector<uint32_t> v; size_t j = 0;
            while (j <= it.size()) { size_t c = it.find(',', j); if (c == std::string::npos) c = it.size(); uint32_t x = 0; if (!Num(it.substr(j, c - j), x)) return bad("bad number"); v.push_back(x); j = c + 1; }
            items.push_back(v);
        }
        if (op == "steps") {
            if (items.size() > 15) return bad("at most 15 steps");
            uint32_t id = MaxOf(PSAL, "PSAL") + 1;
            if (id > 4095) return bad("no free script ids");
            uint32_t k = 0;
            for (auto& s : items) {
                if (s.size() != 4) return bad("a step is code,v1,v2,v3");
                size_t r = PSAL->AddRow();
                if (!PSAL->Set(r, "PSAL", id) || !PSAL->Set(r, "step", k++) || !PSAL->Set(r, "code", s[0]) || !PSAL->Set(r, "val1", s[1]) ||
                    !PSAL->Set(r, "val2", s[2]) || !PSAL->Set(r, "val3", s[3])) { PSAL->rows.pop_back(); DeleteWhere(PSAL, "PSAL", id); return bad("value out of range"); }
            }
            size_t r = PSAL->AddRow(); PSAL->Set(r, "PSAL", id); PSAL->Set(r, "step", k); PSAL->Set(r, "code", 255);
            PLYS->Set(row, "PSAL", id);
            return 1;
        }
        if (items.size() > 12) return bad("at most 12 art points");
        if (items.empty()) { PLYS->Set(row, "ARTL", 0); return 1; }
        uint32_t id = MaxOf(ARTL, "ARTL") + 1;
        if (id > 4095) return bad("no free art ids");
        std::vector<uint8_t> rec(ARTL->rb, 0);
        Table tmp; tmp.fields = ARTL->fields; tmp.rb = ARTL->rb; tmp.rows.push_back(rec);
        tmp.Set(0, "ARTL", id); tmp.Set(0, "acnt", uint32_t(items.size()));
        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].size() != 3) return bad("an art point is x,y,ct");
            int n = int(i) + 1;
            for (const char* k : {"ax", "ay"}) {
                int f = tmp.F(Key2(k, n)); uint32_t bits = tmp.fields[f].bits;
                int32_t v = int32_t(k[1] == 'x' ? items[i][0] : items[i][1]);
                tmp.Set(0, f, uint32_t(v) & ((1u << bits) - 1));
            }
            tmp.Set(0, Key2("ct", n).c_str(), std::min<uint32_t>(items[i][2], 2));
            tmp.Set(0, Key2("sp", n).c_str(), 15);
        }
        InsertSorted(ARTL, "ARTL", tmp.rows[0]);
        PLYS->Set(row, "ARTL", id);
        return 1;
    }
    if (op == "field") {
        if (a.size() < 4) return bad("field <TABLE> <key>=<v>[,..] <f>=<v> ...");
        Table* t = db.T(a[1].c_str()); if (!t) return bad("no table " + a[1]);
        std::vector<std::pair<int, uint32_t>> keys;
        for (size_t i = 0; i < a[2].size();) {
            size_t e = a[2].find(',', i); if (e == std::string::npos) e = a[2].size();
            std::string kv = a[2].substr(i, e - i); i = e + 1;
            size_t q = kv.find('='); uint32_t v;
            if (q == std::string::npos || t->F(kv.substr(0, q)) < 0 || !Num(kv.substr(q + 1), v)) return bad("bad key " + kv);
            keys.push_back({t->F(kv.substr(0, q)), v});
        }
        int n = 0;
        for (size_t r = 0; r < t->rows.size(); r++) {
            bool m = true; for (auto& k : keys) if (t->Get(r, k.first) != k.second) { m = false; break; }
            if (!m) continue;
            for (size_t i = 3; i < a.size(); i++) {
                size_t q = a[i].find('='); if (q == std::string::npos) return bad("bad value " + a[i]);
                int f = t->F(a[i].substr(0, q)); if (f < 0) return bad("no field " + a[i].substr(0, q));
                if (t->fields[f].type == 0) t->SetStr(r, f, a[i].substr(q + 1));
                else { uint32_t v; if (!Num(a[i].substr(q + 1), v) || !t->Set(r, f, v)) return bad("value out of range"); }
            }
            n++;
        }
        return n ? 1 : bad("no matching row");
    }
    return bad("unknown operation");
}

int ApplyEdits(Db& off, Db& def, const std::string& text, std::string& log) {
    Db* cur = &off; int n = 0;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i); if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i); i = e + 1;
        std::vector<std::string> a = Tokens(line);
        if (a.empty()) continue;
        if (a[0] == "book") { cur = a.size() > 1 && a[1] == "def" ? &def : &off; continue; }
        n += ApplyOne(*cur, line, log);
    }
    return n;
}

}   // namespace pb
