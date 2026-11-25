// Builds mods/playcall_plays.txt (the Full Playbook list) on the player's own PC from their own game files, the first
// time it is needed: offense = qkl_boot.ast entry 82, defense = qkl_fe2ig.ast entry 260 (the game's playbooks).
// Nothing taken from the game is stored in this repository; this only reads the copy the player supplies.
// Same output as the dev tool make_playlist.py. One line per play:
//   O|D <tab> formation shown <tab> play name <tab> sets <tab> play art <tab> formation (game name) <tab> R|P
#include "archive_overlay.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

namespace {

// ---- zlib stream (RFC 1950/1951) decoder: stored, fixed and dynamic Huffman blocks ----
struct Bits {
    const uint8_t* p; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0; bool bad = false;
    uint32_t get(int k) {
        while (cnt < k) { if (pos >= n) { bad = true; return 0; } buf |= (uint32_t)p[pos++] << cnt; cnt += 8; }
        uint32_t v = buf & ((1u << k) - 1); buf >>= k; cnt -= k; return v;
    }
};
struct Huff { uint16_t count[16] = {}, sym[320] = {}; };
static void Build(Huff& h, const uint8_t* len, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    h.count[0] = 0;
    uint16_t off[16] = {};
    for (int i = 1; i < 16; i++) off[i] = off[i - 1] + h.count[i - 1];
    for (int i = 0; i < n; i++) if (len[i]) h.sym[off[len[i]]++] = (uint16_t)i;
}
static int Decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)b.get(1);
        int c = h.count[l];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
        if (b.bad) return -1;
    }
    return -1;
}
static const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool Inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    if (n < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31) return false;   // zlib header, deflate
    Bits b{src + 2, n - 2};
    for (bool last = false; !last;) {
        last = b.get(1); int type = (int)b.get(2);
        if (b.bad) return false;
        if (type == 0) {   // stored
            b.buf = 0; b.cnt = 0;
            if (b.pos + 4 > b.n) return false;
            uint16_t len = (uint16_t)(b.p[b.pos] | b.p[b.pos + 1] << 8); b.pos += 4;
            if (b.pos + len > b.n) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len); b.pos += len;
            continue;
        }
        if (type == 3) return false;
        Huff lit, dist; uint8_t lens[320] = {};
        if (type == 1) {
            int i = 0;
            for (; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            Build(lit, lens, 288);
            for (i = 0; i < 30; i++) lens[i] = 5;
            Build(dist, lens, 30);
        } else {
            int hlit = (int)b.get(5) + 257, hdist = (int)b.get(5) + 1, hclen = (int)b.get(4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)b.get(3);
            Huff ch; Build(ch, cl, 19);
            int i = 0;
            while (i < hlit + hdist) {
                int s = Decode(b, ch);
                if (s < 0) return false;
                if (s < 16) lens[i++] = (uint8_t)s;
                else {
                    int rep = 0; uint8_t v = 0;
                    if (s == 16) { if (!i) return false; v = lens[i - 1]; rep = 3 + (int)b.get(2); }
                    else if (s == 17) rep = 3 + (int)b.get(3);
                    else rep = 11 + (int)b.get(7);
                    if (i + rep > hlit + hdist) return false;
                    while (rep--) lens[i++] = v;
                }
            }
            Build(lit, lens, hlit);
            Build(dist, lens + hlit, hdist);
        }
        for (;;) {
            int s = Decode(b, lit);
            if (s < 0) return false;
            if (s < 256) { out.push_back((uint8_t)s); continue; }
            if (s == 256) break;
            s -= 257; if (s >= 29) return false;
            int len = kLenBase[s] + (int)b.get(kLenExtra[s]);
            int d = Decode(b, dist); if (d < 0 || d >= 30) return false;
            size_t back = kDistBase[d] + b.get(kDistExtra[d]);
            if (back > out.size() || b.bad) return false;
            size_t from = out.size() - back;
            for (int k = 0; k < len; k++) out.push_back(out[from + k]);
        }
    }
    return true;
}

// ---- BGFA1.05 archive (.ast): one entry, decompressed ----
static uint64_t LE(const uint8_t* p, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static bool ReadEntry(const std::string& path, uint32_t want, std::vector<uint8_t>& out, bool edited = true) {
    if (edited) {   // the playbooks as edited (playbook_mod.cpp), when there are edits
        size_t k = path.find_last_of("/\\");
        if (const std::vector<uint8_t>* o = OverlayEntry(k == std::string::npos ? path : path.substr(k + 1), want)) { out = *o; return true; }
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    uint8_t h[0x40];
    if (fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "BGFA1.05", 8)) {
        uint32_t cnt = (uint32_t)LE(h + 0xC, 4); uint64_t toff = LE(h + 0x10, 8), tsz = LE(h + 0x18, 8);
        int hs = h[0x22], w1 = h[0x23], w2 = h[0x24], w3 = h[0x25], sh = h[0x26];
        uint64_t pre = 4 * LE(h + 0x28, 4), nl = LE(h + 0x2C, 4);
        if (want < cnt && tsz > pre && tsz < (64u << 20)) {
            std::vector<uint8_t> t(tsz);
#ifdef _WIN32
            _fseeki64(f, (long long)toff, SEEK_SET);
#else
            fseeko(f, (off_t)toff, SEEK_SET);
#endif
            if (fread(t.data(), 1, tsz, f) == tsz) {
                uint64_t rec = (tsz - pre) / cnt; int fb = int(rec - hs - w1 - w2 - w3 - nl);
                if (fb == 1 || fb == 2) {
                    const uint8_t* r = t.data() + pre + want * rec; int x = fb + hs;
                    uint64_t o = LE(r + x, w1) << sh; x += w1; uint64_t sz = LE(r + x, w2);
                    bool comp = LE(r, fb) & 1;
                    std::vector<uint8_t> raw(sz);
#ifdef _WIN32
                    _fseeki64(f, (long long)o, SEEK_SET);
#else
                    fseeko(f, (off_t)o, SEEK_SET);
#endif
                    if (sz && sz < (256u << 20) && fread(raw.data(), 1, sz, f) == sz) {
                        if (comp) ok = Inflate(raw.data(), raw.size(), out);
                        else { out = std::move(raw); ok = true; }
                    }
                }
            }
        }
    }
    fclose(f);
    return ok;
}

// ---- EA TDB database (big-endian): tables of bit-packed records ----
struct Row { std::map<std::string, uint32_t> v; std::map<std::string, std::string> s;
    uint32_t operator[](const char* k) const { auto it = v.find(k); return it == v.end() ? 0 : it->second; }
    std::string str(const char* k) const { auto it = s.find(k); return it == s.end() ? std::string() : it->second; } };
using Tables = std::map<std::string, std::vector<Row>>;
static uint32_t BE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static bool LoadTdb(const std::vector<uint8_t>& d, Tables& out) {
    if (d.size() < 24 || d[0] != 'D' || d[1] != 'B') return false;
    uint32_t n = BE32(&d[16]); size_t base = 24 + (size_t)n * 8;
    if (base > d.size()) return false;
    for (uint32_t i = 0; i < n; i++) {
        std::string nm; for (int k = 3; k >= 0; k--) nm += (char)d[24 + i * 8 + k];
        size_t at = base + BE32(&d[28 + i * 8]);
        if (at + 40 > d.size()) return false;
        uint32_t rb = BE32(&d[at + 8]); uint16_t used = (uint16_t)(d[at + 22] << 8 | d[at + 23]); int nf = d[at + 28];
        struct F { std::string name; uint32_t type, bit, bits; }; std::vector<F> fs;
        size_t p = at + 40;
        for (int k = 0; k < nf; k++, p += 16) {
            if (p + 16 > d.size()) return false;
            std::string fn; for (int q = 3; q >= 0; q--) fn += (char)d[p + 8 + q];
            fs.push_back({fn, BE32(&d[p]), BE32(&d[p + 4]), BE32(&d[p + 12])});
        }
        auto& rows = out[nm];
        for (uint32_t r = 0; r < used; r++) {
            size_t ra = p + (size_t)r * rb;
            if (ra + rb > d.size()) return false;
            Row row;
            for (auto& f : fs) {
                if (f.type == 0) {
                    std::string s; for (uint32_t q = 0; q < f.bits / 8 && d[ra + f.bit / 8 + q]; q++) s += (char)d[ra + f.bit / 8 + q];
                    row.s[f.name] = s;
                } else {
                    uint64_t v = 0;
                    for (uint32_t q = 0; q < f.bits; q++) { uint32_t bit = f.bit + q; v = (v << 1) | ((d[ra + (bit >> 3)] >> (7 - (bit & 7))) & 1); }
                    row.v[f.name] = (uint32_t)v;
                }
            }
            rows.push_back(std::move(row));
        }
    }
    return true;
}

static int S8(uint32_t v) { v &= 255; return v > 127 ? (int)v - 256 : (int)v; }
static std::string Key2(const char* a, int i) { char b[8]; snprintf(b, sizeof b, "%s%02d", a, i); return b; }

static bool BuildSide(char side, const std::string& path, uint32_t entry, std::vector<std::string>& out) {
    std::vector<uint8_t> raw; Tables t;
    if (!ReadEntry(path, entry, raw) || !LoadTdb(raw, t)) return false;
    static const std::set<std::string> skip = {"Kickoff", "Safety Punt", "Special Teams", "Kick Return", "Safety Kick Return", "Normal"};
    static const std::map<std::string, std::string> shown = {{"Gun", "Shotgun"}, {"I-Form", "I Form"}, {"Weak", "Weak I"}, {"Strong", "Strong I"}, {"Pahokee", "Wildcat"}, {"Dollar", "Dollar (3-2-6)"}};
    static const std::set<uint32_t> runTypes = {11, 13, 14, 15, 17, 19, 151, 152, 153, 155};
    std::vector<std::string> order = side == 'O' ? std::vector<std::string>{"Gun", "Singleback", "I-Form", "Weak", "Strong", "Goal Line", "Pahokee"}
                                                 : std::vector<std::string>{"4-3", "Nickel", "Dime", "Dollar", "Quarter", "Goal Line"};
    std::map<uint32_t, const Row*> plays, pbst, pbfm, lines;
    std::map<std::pair<uint32_t, uint32_t>, const Row*> spot;
    std::map<uint32_t, std::vector<const Row*>> steps;
    for (auto& r : t["PLYL"]) plays[r["PLYL"]] = &r;
    for (auto& r : t["PBST"]) pbst[r["PBST"]] = &r;
    for (auto& r : t["PBFM"]) pbfm[r["PBFM"]] = &r;
    for (auto& r : t["ARTL"]) lines[r["ARTL"]] = &r;
    for (auto& r : t["SETP"]) spot[{r["SETL"], r["poso"]}] = &r;
    for (auto& r : t["PLYS"]) steps[r["PLYL"]].push_back(&r);
    auto art = [&](const Row* p) {
        std::vector<const Row*> st = steps[(*p)["PLYL"]];
        std::stable_sort(st.begin(), st.end(), [](const Row* a, const Row* b) { return (*a)["poso"] < (*b)["poso"]; });
        std::string res;
        for (const Row* s : st) {
            auto sp = spot.find({(*p)["SETL"], (*s)["poso"]});
            if (sp == spot.end()) continue;
            char g[64]; snprintf(g, sizeof g, "%u,%u,%u", (*sp->second)["artx"], (*sp->second)["arty"], (*sp->second)["EPos"]);
            std::string grp = g;
            auto a = lines.find((*s)["ARTL"]);
            if (a != lines.end()) {
                std::string pts;
                int cnt = std::min<int>(12, (int)(*a->second)["acnt"]);
                for (int i = 1; i <= cnt; i++) {
                    snprintf(g, sizeof g, "%d,%d,%u", S8((*a->second)[Key2("ax", i).c_str()]), S8((*a->second)[Key2("ay", i).c_str()]), (*a->second)[Key2("ct", i).c_str()]);
                    if (!pts.empty()) pts += ' ';
                    pts += g;
                }
                if (!pts.empty()) grp += ":" + pts;
            }
            if (!res.empty()) res += ';';
            res += grp;
        }
        return res;
    };
    struct E { std::vector<std::string> sets; std::string art; std::pair<uint32_t, uint32_t> ord; char kind; };
    std::map<std::string, std::map<std::string, E>> book;
    std::vector<const Row*> pbpl; for (auto& r : t["PBPL"]) pbpl.push_back(&r);
    std::stable_sort(pbpl.begin(), pbpl.end(), [](const Row* a, const Row* b) {
        return std::make_pair((*a)["PBST"], (*a)["ord_"]) < std::make_pair((*b)["PBST"], (*b)["ord_"]); });
    for (const Row* r : pbpl) {
        auto st = pbst.find((*r)["PBST"]); if (st == pbst.end()) continue;
        auto fm = pbfm.find((*st->second)["PBFM"]); if (fm == pbfm.end()) continue;
        auto p = plays.find((*r)["PLYL"]); if (p == plays.end()) continue;
        std::string fname = fm->second->str("name"), name = r->str("name");
        if (skip.count(fname) || name.empty()) continue;
        auto& fb = book[fname];
        auto it = fb.find(name);
        if (it == fb.end()) it = fb.emplace(name, E{{}, art(p->second), {(*st->second)["ord_"], (*r)["ord_"]}, runTypes.count((*p->second)["PLYT"]) ? 'R' : 'P'}).first;
        std::string sn = st->second->str("name");
        if (std::find(it->second.sets.begin(), it->second.sets.end(), sn) == it->second.sets.end()) it->second.sets.push_back(sn);
    }
    std::vector<std::string> extra; for (auto& kv : book) if (std::find(order.begin(), order.end(), kv.first) == order.end()) extra.push_back(kv.first);
    std::sort(extra.begin(), extra.end()); order.insert(order.end(), extra.begin(), extra.end());
    for (auto& f : order) {
        auto bf = book.find(f); if (bf == book.end()) continue;
        std::vector<std::pair<std::string, const E*>> items; for (auto& kv : bf->second) items.push_back({kv.first, &kv.second});
        std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return std::make_pair(a.second->ord, a.first) < std::make_pair(b.second->ord, b.first); });
        for (auto& [name, e] : items) {
            std::string sets; for (auto& s : e->sets) if (s != "no_name") { if (!sets.empty()) sets += ','; sets += s; }
            auto sh = shown.find(f);
            out.push_back(std::string(1, side) + "\t" + (sh != shown.end() ? sh->second : f) + "\t" + name + "\t" + sets + "\t" + e->art + "\t" + f + "\t" + e->kind);
        }
    }
    return true;
}

}   // namespace

// Writes the list from <gameDir>/qkl_boot.ast and qkl_fe2ig.ast; false (and nothing written) if they can't be read.
bool GeneratePlaybookList(const std::string& gameDir, const std::string& outPath) {
    std::vector<std::string> lines;
    if (!BuildSide('O', gameDir + "/qkl_boot.ast", 82, lines) || !BuildSide('D', gameDir + "/qkl_fe2ig.ast", 260, lines)) return false;
    if (lines.size() < 50) return false;
    std::string tmp = outPath + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    for (auto& l : lines) { fputs(l.c_str(), f); fputc('\n', f); }
    fclose(f);
    remove(outPath.c_str());
    return rename(tmp.c_str(), outPath.c_str()) == 0;
}

// The game's own copy of an archive entry (no edits applied), for playbook_mod.cpp.
bool ReadOriginalEntry(const std::string& path, uint32_t entry, std::vector<uint8_t>& out) { return ReadEntry(path, entry, out, false); }
