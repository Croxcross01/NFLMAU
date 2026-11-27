// Archive entry overrides (see archive_overlay.h).
#include "archive_overlay.h"
#include "platform.h"
#include "deflate_lite.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

struct ArchiveOverlay {
    uint64_t realSize = 0;
    uint64_t tableOff = 0; std::vector<uint8_t> table;   // the archive's entry table, with the replaced records
    uint64_t tailOff = 0; std::vector<uint8_t> tail;     // replaced entries, past the end of the real file
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> inPlace;   // replaced entries that fit where the original was
};

static std::mutex g_m;
static std::map<std::string, std::map<uint32_t, std::vector<uint8_t>>> g_entries;   // archive name (lower case) -> entries
static std::map<std::string, std::unique_ptr<ArchiveOverlay>> g_overlays;           // host path -> overlay (null: none)

static std::string Lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }
static std::string BaseName(const std::string& p) { size_t k = p.find_last_of("/\\"); return Lower(k == std::string::npos ? p : p.substr(k + 1)); }
static uint64_t LE(const uint8_t* p, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static void PutLE(uint8_t* p, int n, uint64_t v) { for (int i = 0; i < n; i++) { p[i] = uint8_t(v); v >>= 8; } }

void OverlaySetEntry(const std::string& archive, uint32_t entry, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> l(g_m);
    g_entries[Lower(archive)][entry] = std::move(data);
    g_overlays.clear();
}
const std::vector<uint8_t>* OverlayEntry(const std::string& archive, uint32_t entry) {
    std::lock_guard<std::mutex> l(g_m);
    auto a = g_entries.find(Lower(archive));
    if (a == g_entries.end()) return nullptr;
    auto e = a->second.find(entry);
    return e == a->second.end() ? nullptr : &e->second;
}

ArchiveOverlay* OverlayFor(const std::string& hostPath) {
    std::lock_guard<std::mutex> l(g_m);
    auto it = g_overlays.find(hostPath);
    if (it != g_overlays.end()) return it->second.get();
    std::unique_ptr<ArchiveOverlay> ov;
    auto ents = g_entries.find(BaseName(hostPath));
    if (ents != g_entries.end() && !ents->second.empty()) {
        if (FILE* fp = fopen(hostPath.c_str(), "rb")) {
            uint8_t h[0x40];
            if (fread(h, 1, sizeof h, fp) == sizeof h && !memcmp(h, "BGFA1.05", 8)) {
                uint32_t cnt = uint32_t(LE(h + 0xC, 4)); uint64_t toff = LE(h + 0x10, 8), tsz = LE(h + 0x18, 8);
                int hs = h[0x22], w1 = h[0x23], w2 = h[0x24], w3 = h[0x25], sh = h[0x26];
                uint64_t pre = 4 * LE(h + 0x28, 4), nl = LE(h + 0x2C, 4);
                PlatFseek(fp, 0); fseek(fp, 0, SEEK_END);
#ifdef _WIN32
                uint64_t size = uint64_t(_ftelli64(fp));
#else
                uint64_t size = uint64_t(ftello(fp));
#endif
                if (cnt && tsz > pre && tsz < (64u << 20)) {
                    auto o = std::make_unique<ArchiveOverlay>();
                    o->realSize = size; o->tableOff = toff; o->table.resize(tsz);
                    PlatFseek(fp, toff);
                    uint64_t rec = (tsz - pre) / cnt; int fb = int(rec - hs - w1 - w2 - w3 - nl);
                    if (fread(o->table.data(), 1, tsz, fp) == tsz && (fb == 1 || fb == 2)) {
                        uint64_t align = 1ull << sh;
                        // where each entry's space ends (the next entry, the table, or the end of the file)
                        std::vector<uint64_t> starts;
                        for (uint32_t k = 0; k < cnt; k++) { const uint8_t* q = o->table.data() + pre + k * rec; starts.push_back(LE(q + fb + hs, w1) << sh); }
                        starts.push_back(toff); starts.push_back(size);
                        std::sort(starts.begin(), starts.end());
                        o->tailOff = (size + align - 1) / align * align;
                        for (auto& [idx, data] : ents->second) {
                            if (idx >= cnt) continue;
                            uint64_t at = 0;
                            uint8_t* r = o->table.data() + pre + idx * rec;
                            // an entry the game keeps compressed stays compressed (some loaders always inflate)
                            bool packed = (LE(r, fb) & 1) != 0;
                            std::vector<uint8_t> z; if (packed) z = ZlibCompress(data);
                            if (packed && z.size() > data.size()) packed = false;   // (delta would be negative)
                            const std::vector<uint8_t>& stored = packed ? z : data;
                            // written where the original was when it fits (archives the game loads whole into memory,
                            // like qkl_ram.ast, never see anything past the original end of the file)
                            uint64_t orig = LE(r + fb + hs, w1) << sh;
                            uint64_t room = *std::upper_bound(starts.begin(), starts.end(), orig) - orig;
                            if (stored.size() <= room) {
                                o->inPlace.push_back({orig, stored});
                                at = orig;
                                fprintf(stderr, "[mods] %s entry %u: edited version fits in place (%zu of %llu bytes)\n", BaseName(hostPath).c_str(), idx, stored.size(), (unsigned long long)room);
                            } else {   // past the end of the real file
                                at = o->tailOff + o->tail.size();
                                at = (at + align - 1) / align * align;
                                o->tail.resize(at - o->tailOff);
                                o->tail.insert(o->tail.end(), stored.begin(), stored.end());
                            }
                            if (!packed) PutLE(r, fb, LE(r, fb) & ~1ull);   // stored uncompressed
                            int x = fb + hs;
                            PutLE(r + x, w1, at >> sh); x += w1;
                            PutLE(r + x, w2, stored.size()); x += w2;
                            PutLE(r + x, w3, packed ? data.size() - stored.size() : 0);
                            fprintf(stderr, "[mods] %s entry %u: your edited version (%zu KB)\n", BaseName(hostPath).c_str(), idx, (data.size() + 1023) / 1024);
                        }
                        ov = std::move(o);
                    }
                }
            }
            fclose(fp);
        }
    }
    ArchiveOverlay* p = ov.get();
    g_overlays[hostPath] = std::move(ov);
    return p;
}

uint64_t OverlaySize(ArchiveOverlay* o, uint64_t realSize) { return o && !o->tail.empty() ? o->tailOff + o->tail.size() : realSize; }

size_t OverlayRead(ArchiveOverlay* o, FILE* fp, uint64_t off, uint8_t* dst, size_t len) {
    uint64_t end = OverlaySize(o, o->realSize);
    if (off >= end) return 0;
    if (off + len > end) len = size_t(end - off);
    size_t got = 0;
    if (off < o->realSize) {   // the real file
        size_t n = size_t(std::min<uint64_t>(len, o->realSize - off));
        PlatFseek(fp, off);
        got = fread(dst, 1, n, fp);
        if (got < n) memset(dst + got, 0, n - got);
        got = n;
    }
    if (got < len) {   // between the end of the file and the appended entries: zeros; then the entries
        for (size_t i = got; i < len; i++) {
            uint64_t p = off + i;
            dst[i] = p >= o->tailOff && p - o->tailOff < o->tail.size() ? o->tail[size_t(p - o->tailOff)] : 0;
        }
        got = len;
    }
    for (auto& [po, pb] : o->inPlace) {   // edited entries written where the originals were
        uint64_t a = std::max(off, po), b = std::min<uint64_t>(off + len, po + pb.size());
        if (a < b) memcpy(dst + (a - off), pb.data() + (a - po), size_t(b - a));
    }
    // the replaced entry table
    uint64_t t0 = o->tableOff, t1 = o->tableOff + o->table.size();
    uint64_t a = std::max(off, t0), b = std::min<uint64_t>(off + len, t1);
    if (a < b) memcpy(dst + (a - off), o->table.data() + (a - t0), size_t(b - a));
    return got;
}
