// Unpacks .zip files in this program (no outside tools started): stored and deflate entries, Zip64 sizes.
// Used by the setup and the launcher (via build_core.inc) for the program pack and the compiler download.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace zr {

struct Bits {
    const uint8_t* p; size_t n, pos = 0; uint64_t buf = 0; int cnt = 0; bool bad = false;
    uint32_t get(int k) {
        while (cnt < k) { if (pos >= n) { bad = true; return 0; } buf |= (uint64_t)p[pos++] << cnt; cnt += 8; }
        uint32_t v = (uint32_t)(buf & ((1ull << k) - 1)); buf >>= k; cnt -= k; return v;
    }
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
// Raw deflate (no zlib header), as stored in zip files. `out` should be reserved to the expected size.
inline bool InflateRaw(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    static const uint16_t lb[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t le[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t db[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t de[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    Bits b{src, n};
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

inline uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
inline uint32_t U32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
inline uint64_t U64(const uint8_t* p) { return U32(p) | (uint64_t)U32(p + 4) << 32; }

// Extracts every entry of `zipPath` under `dir`. Names with ".." or a drive are refused.
inline bool ExtractZip(const std::wstring& zipPath, const std::wstring& dir) {
    HANDLE h = CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    HANDLE map = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const uint8_t* z = map ? (const uint8_t*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : nullptr;
    bool ok = false;
    if (z) {
        size_t n = (size_t)sz.QuadPart;
        // end of central directory (and its Zip64 version)
        size_t eocd = 0;
        for (size_t i = n >= 22 ? n - 22 : 0; i + 22 <= n && i + 65557 >= n; i--) { if (U32(z + i) == 0x06054b50) { eocd = i; break; } if (!i) break; }
        if (eocd) {
            uint64_t count = U16(z + eocd + 10), cdOff = U32(z + eocd + 16);
            if ((count == 0xFFFF || cdOff == 0xFFFFFFFF) && eocd >= 20 && U32(z + eocd - 20) == 0x07064b50) {
                uint64_t z64 = U64(z + eocd - 12);
                if (z64 + 56 <= n && U32(z + z64) == 0x06064b50) { count = U64(z + z64 + 32); cdOff = U64(z + z64 + 48); }
            }
            ok = true;
            size_t p = (size_t)cdOff;
            std::vector<uint8_t> out;
            for (uint64_t e = 0; e < count && ok; e++) {
                if (p + 46 > n || U32(z + p) != 0x02014b50) { ok = false; break; }
                uint16_t method = U16(z + p + 10), nl = U16(z + p + 28), xl = U16(z + p + 30), cl = U16(z + p + 32);
                uint64_t csize = U32(z + p + 20), usize = U32(z + p + 24), loc = U32(z + p + 42);
                std::string name((const char*)z + p + 46, nl);
                for (size_t x = p + 46 + nl; x + 4 <= p + 46 + nl + xl;) {   // Zip64 extra field
                    uint16_t id = U16(z + x), len = U16(z + x + 2); size_t q = x + 4;
                    if (id == 1) {
                        if (usize == 0xFFFFFFFF) { usize = U64(z + q); q += 8; }
                        if (csize == 0xFFFFFFFF) { csize = U64(z + q); q += 8; }
                        if (loc == 0xFFFFFFFF) { loc = U64(z + q); q += 8; }
                    }
                    x += 4 + len;
                }
                p += 46 + nl + xl + cl;
                if (name.empty() || name.find("..") != std::string::npos || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) continue;
                std::wstring rel; for (unsigned char c : name) rel += (wchar_t)(c == '/' ? L'\\' : c);
                // names are UTF-8: convert properly
                { int wn = MultiByteToWideChar(CP_UTF8, 0, name.data(), (int)name.size(), nullptr, 0);
                  if (wn > 0) { rel.assign(wn, L'\0'); MultiByteToWideChar(CP_UTF8, 0, name.data(), (int)name.size(), rel.data(), wn); for (auto& c : rel) if (c == L'/') c = L'\\'; } }
                std::wstring full = dir + L"\\" + rel;
                for (size_t s = dir.size() + 1; s < full.size(); s++) if (full[s] == L'\\') CreateDirectoryW(full.substr(0, s).c_str(), nullptr);
                if (name.back() == '/') { CreateDirectoryW(full.c_str(), nullptr); continue; }
                if (loc + 30 > n || U32(z + loc) != 0x04034b50) { ok = false; break; }
                size_t data = (size_t)loc + 30 + U16(z + loc + 26) + U16(z + loc + 28);
                if (data + csize > n) { ok = false; break; }
                const uint8_t* bytes = z + data; size_t len = (size_t)csize;
                if (method == 8) { out.clear(); out.reserve((size_t)usize); if (!InflateRaw(z + data, (size_t)csize, out) || out.size() != usize) { ok = false; break; } bytes = out.data(); len = out.size(); }
                else if (method != 0) { ok = false; break; }
                HANDLE f = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f == INVALID_HANDLE_VALUE) { ok = false; break; }
                for (size_t done = 0; done < len;) {
                    DWORD w = 0, chunk = (DWORD)std::min<size_t>(len - done, 1u << 30);
                    if (!WriteFile(f, bytes + done, chunk, &w, nullptr) || !w) { ok = false; break; }
                    done += w;
                }
                CloseHandle(f);
            }
        }
        UnmapViewOfFile(z);
    }
    if (map) CloseHandle(map);
    CloseHandle(h);
    return ok;
}

}   // namespace zr
