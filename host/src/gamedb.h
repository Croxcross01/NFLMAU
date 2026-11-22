// Read / write helpers for the game's BGFA1.05 archives (.ast) and EA TDB databases, used to build roster sets on the
// player's own PC from their own game files (nothing taken from the game is stored in this repository).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

namespace gdb {

// ---- zlib inflate (stored / fixed / dynamic Huffman) ----
struct Bits {
    const uint8_t* p; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0; bool bad = false;
    uint32_t get(int k) { while (cnt < k) { if (pos >= n) { bad = true; return 0; } buf |= (uint32_t)p[pos++] << cnt; cnt += 8; }
                          uint32_t v = buf & ((1u << k) - 1); buf >>= k; cnt -= k; return v; }
};
struct Huff { uint16_t count[16] = {}, sym[320] = {}; };
inline void Build(Huff& h, const uint8_t* len, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    h.count[0] = 0; uint16_t off[16] = {};
    for (int i = 1; i < 16; i++) off[i] = off[i - 1] + h.count[i - 1];
    for (int i = 0; i < n; i++) if (len[i]) h.sym[off[len[i]]++] = (uint16_t)i;
}
inline int Decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)b.get(1); int c = h.count[l];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
        if (b.bad) return -1;
    }
    return -1;
}
inline bool Inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    static const uint16_t lb[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t le[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t db[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t de[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    if (n < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31) return false;
    Bits b{src + 2, n - 2};
    for (bool last = false; !last;) {
        last = b.get(1); int type = (int)b.get(2);
        if (b.bad) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;
            if (b.pos + 4 > b.n) return false;
            uint16_t len = (uint16_t)(b.p[b.pos] | b.p[b.pos + 1] << 8); b.pos += 4;
            if (b.pos + len > b.n) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len); b.pos += len; continue;
        }
        if (type == 3) return false;
        Huff lit, dist; uint8_t lens[320] = {};
        if (type == 1) {
            int i = 0; for (; i < 144; i++) lens[i] = 8; for (; i < 256; i++) lens[i] = 9; for (; i < 280; i++) lens[i] = 7; for (; i < 288; i++) lens[i] = 8;
            Build(lit, lens, 288); for (i = 0; i < 30; i++) lens[i] = 5; Build(dist, lens, 30);
        } else {
            int hlit = (int)b.get(5) + 257, hdist = (int)b.get(5) + 1, hclen = (int)b.get(4) + 4;
            static const uint8_t ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t cl[19] = {}; for (int i = 0; i < hclen; i++) cl[ord[i]] = (uint8_t)b.get(3);
            Huff ch; Build(ch, cl, 19);
            for (int i = 0; i < hlit + hdist;) {
                int s = Decode(b, ch); if (s < 0) return false;
                if (s < 16) lens[i++] = (uint8_t)s;
                else { int rep; uint8_t v = 0;
                    if (s == 16) { if (!i) return false; v = lens[i - 1]; rep = 3 + (int)b.get(2); }
                    else if (s == 17) rep = 3 + (int)b.get(3); else rep = 11 + (int)b.get(7);
                    if (i + rep > hlit + hdist) return false;
                    while (rep--) lens[i++] = v; }
            }
            Build(lit, lens, hlit); Build(dist, lens + hlit, hdist);
        }
        for (;;) {
            int s = Decode(b, lit); if (s < 0) return false;
            if (s < 256) { out.push_back((uint8_t)s); continue; }
            if (s == 256) break;
            s -= 257; if (s >= 29) return false;
            int len = lb[s] + (int)b.get(le[s]);
            int d = Decode(b, dist); if (d < 0 || d >= 30) return false;
            size_t back = db[d] + b.get(de[d]);
            if (back > out.size() || b.bad) return false;
            size_t from = out.size() - back;
            for (int k = 0; k < len; k++) out.push_back(out[from + k]);
        }
    }
    return true;
}

// ---- BGFA1.05 archive ----
inline uint64_t LE(const uint8_t* p, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i]; return v; }
inline void PutLE(uint8_t* p, int n, uint64_t v) { for (int i = 0; i < n; i++) { p[i] = (uint8_t)v; v >>= 8; } }
inline bool Seek(FILE* f, uint64_t o) {
#ifdef _WIN32
    return _fseeki64(f, (long long)o, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)o, SEEK_SET) == 0;
#endif
}
struct Archive {
    std::string path; uint64_t toff = 0, tsz = 0, pre = 0, rec = 0; int hs = 0, w1 = 0, w2 = 0, w3 = 0, sh = 0, fb = 0; uint32_t cnt = 0;
    std::vector<uint8_t> table;
    bool Open(const std::string& p) {
        path = p; FILE* f = fopen(p.c_str(), "rb"); if (!f) return false;
        uint8_t h[0x40]; bool ok = false;
        if (fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "BGFA1.05", 8)) {
            cnt = (uint32_t)LE(h + 0xC, 4); toff = LE(h + 0x10, 8); tsz = LE(h + 0x18, 8);
            hs = h[0x22]; w1 = h[0x23]; w2 = h[0x24]; w3 = h[0x25]; sh = h[0x26];
            pre = 4 * LE(h + 0x28, 4); uint64_t nl = LE(h + 0x2C, 4);
            if (cnt && tsz > pre && tsz < (64u << 20)) {
                rec = (tsz - pre) / cnt; fb = int(rec - hs - w1 - w2 - w3 - nl);
                table.resize(tsz);
                ok = (fb == 1 || fb == 2) && Seek(f, toff) && fread(table.data(), 1, tsz, f) == tsz;
            }
        }
        fclose(f); return ok;
    }
    uint8_t* Rec(uint32_t i) { return table.data() + pre + i * rec; }
    bool Read(uint32_t i, std::vector<uint8_t>& out) {
        if (i >= cnt) return false;
        uint8_t* r = Rec(i); int x = fb + hs;
        uint64_t o = LE(r + x, w1) << sh; x += w1; uint64_t sz = LE(r + x, w2);
        bool comp = LE(r, fb) & 1;
        FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
        std::vector<uint8_t> raw(sz); bool ok = sz < (256u << 20) && Seek(f, o) && fread(raw.data(), 1, sz, f) == sz;
        fclose(f);
        if (!ok) return false;
        out.clear();
        if (comp) return Inflate(raw.data(), raw.size(), out);
        out = std::move(raw); return true;
    }
    // A copy of the archive with some entries replaced (stored uncompressed at the end; everything else byte-identical).
    bool WritePatched(const std::string& dst, const std::map<uint32_t, std::vector<uint8_t>>& repl) {
        std::string tmp = dst + ".tmp";
        FILE* in = fopen(path.c_str(), "rb"); if (!in) return false;
        FILE* out = fopen(tmp.c_str(), "wb"); if (!out) { fclose(in); return false; }
        std::vector<uint8_t> buf(1 << 20); size_t n; uint64_t pos = 0;
        while ((n = fread(buf.data(), 1, buf.size(), in)) > 0) { fwrite(buf.data(), 1, n, out); pos += n; }
        fclose(in);
        std::vector<uint8_t> t = table;
        for (auto& [idx, data] : repl) {
            if (idx >= cnt) continue;
            uint64_t align = 1ull << sh, pad = (align - pos % align) % align;
            static const uint8_t zero[4096] = {};
            while (pad) { uint64_t k = std::min<uint64_t>(pad, sizeof zero); fwrite(zero, 1, k, out); pos += k; pad -= k; }
            fwrite(data.data(), 1, data.size(), out);
            uint8_t* r = t.data() + pre + idx * rec;
            uint64_t flags = LE(r, fb) & ~1ull; PutLE(r, fb, flags);   // stored uncompressed
            int x = fb + hs;
            if ((pos >> sh) >= (1ull << (8 * w1)) || data.size() >= (1ull << (8 * w2))) { fclose(out); remove(tmp.c_str()); return false; }
            PutLE(r + x, w1, pos >> sh); x += w1; PutLE(r + x, w2, data.size()); x += w2;
            if (w3) PutLE(r + x, w3, 0);
            pos += data.size();
        }
        Seek(out, toff); fwrite(t.data(), 1, t.size(), out);
        bool ok = fclose(out) == 0;
        if (!ok) { remove(tmp.c_str()); return false; }
        remove(dst.c_str());
        return rename(tmp.c_str(), dst.c_str()) == 0;
    }
};

// ---- EA TDB database (big-endian, bit-packed records) ----
inline uint32_t BE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline void PutBE32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
struct Field { std::string name; uint32_t type, bit, bits; };
struct Table {
    std::string name; size_t header = 0, records = 0; uint32_t recBytes = 0; uint16_t capacity = 0, used = 0;
    std::vector<Field> fields;
    const Field* F(const char* n) const { for (auto& f : fields) if (f.name == n) return &f; return nullptr; }
};
struct Db {
    std::vector<uint8_t> d; std::vector<Table> tables;
    bool Load(std::vector<uint8_t> data) {
        d = std::move(data); tables.clear();
        if (d.size() < 24 || d[0] != 'D' || d[1] != 'B') return false;
        uint32_t n = BE32(&d[16]); size_t base = 24 + (size_t)n * 8; if (base > d.size()) return false;
        for (uint32_t i = 0; i < n; i++) {
            Table t; for (int k = 3; k >= 0; k--) t.name += (char)d[24 + i * 8 + k];
            t.header = base + BE32(&d[28 + i * 8]); if (t.header + 40 > d.size()) return false;
            t.recBytes = BE32(&d[t.header + 8]); t.capacity = (uint16_t)(d[t.header + 20] << 8 | d[t.header + 21]); t.used = (uint16_t)(d[t.header + 22] << 8 | d[t.header + 23]);
            int nf = d[t.header + 28]; size_t p = t.header + 40;
            for (int k = 0; k < nf; k++, p += 16) {
                if (p + 16 > d.size()) return false;
                Field f; for (int q = 3; q >= 0; q--) f.name += (char)d[p + 8 + q];
                f.type = BE32(&d[p]); f.bit = BE32(&d[p + 4]); f.bits = BE32(&d[p + 12]); t.fields.push_back(f);
            }
            t.records = p;
            if (t.records + (size_t)t.used * t.recBytes > d.size()) return false;
            tables.push_back(t);
        }
        return true;
    }
    Table* T(const char* n) { for (auto& t : tables) if (t.name == n) return &t; return nullptr; }
    uint32_t Get(const Table& t, int row, const char* field) const {
        const Field* f = t.F(field); if (!f || f->type == 0) return 0;
        size_t base = (t.records + (size_t)row * t.recBytes) * 8 + f->bit; uint64_t v = 0;
        for (uint32_t q = 0; q < f->bits; q++) { size_t b = base + q; v = (v << 1) | ((d[b >> 3] >> (7 - (b & 7))) & 1); }
        return (uint32_t)v;
    }
    std::string Str(const Table& t, int row, const char* field) const {
        const Field* f = t.F(field); if (!f || f->type != 0) return std::string();
        size_t at = t.records + (size_t)row * t.recBytes + f->bit / 8; std::string s;
        for (uint32_t q = 0; q < f->bits / 8 && d[at + q]; q++) s += (char)d[at + q];
        return s;
    }
    void Set(const Table& t, int row, const char* field, uint32_t v) {
        const Field* f = t.F(field); if (!f || f->type == 0) return;
        if (f->bits < 32) v = std::min<uint32_t>(v, (1u << f->bits) - 1);
        size_t base = (t.records + (size_t)row * t.recBytes) * 8 + f->bit;
        for (uint32_t q = 0; q < f->bits; q++) {
            size_t b = base + q; uint8_t m = (uint8_t)(0x80 >> (b & 7));
            if ((v >> (f->bits - 1 - q)) & 1) d[b >> 3] |= m; else d[b >> 3] &= (uint8_t)~m;
        }
    }
    void SetStr(const Table& t, int row, const char* field, const std::string& s) {
        const Field* f = t.F(field); if (!f || f->type != 0) return;
        size_t at = t.records + (size_t)row * t.recBytes + f->bit / 8, n = f->bits / 8;
        for (size_t q = 0; q < n; q++) d[at + q] = q < s.size() ? (uint8_t)s[q] : 0;
    }
    uint8_t* Row(const Table& t, int row) { return d.data() + t.records + (size_t)row * t.recBytes; }
    static uint32_t Crc(const uint8_t* p, size_t n) {   // MSB-first CRC-32, poly 0x04C11DB7, init ~0, no final xor
        static uint32_t tab[256]; static bool init = false;
        if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i << 24; for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1; tab[i] = c; } init = true; }
        uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < n; i++) c = (c << 8) ^ tab[((c >> 24) ^ p[i]) & 0xFF]; return c;
    }
    void FixChecksums() {   // the checksum chain the game checks
        uint32_t n = BE32(&d[16]); size_t base = 24 + (size_t)n * 8; std::vector<size_t> offs;
        for (uint32_t i = 0; i < n; i++) offs.push_back(base + BE32(&d[28 + i * 8]));
        PutBE32(&d[20], Crc(&d[0], 20));
        PutBE32(&d[offs[0]], Crc(&d[24], base - 24));
        for (uint32_t i = 1; i < n; i++) PutBE32(&d[offs[i]], Crc(&d[offs[i - 1] + 40], offs[i] - offs[i - 1] - 40));
        PutBE32(&d[d.size() - 4], Crc(&d[offs[n - 1] + 40], d.size() - 4 - offs[n - 1] - 40));
    }
};

}   // namespace gdb
