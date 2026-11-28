// Title screen (PRESS START): the project's own picture in place of the game's curtain scene.
// The title is a small bundle (qkl_interface.ast entry 524, with a copy in qkl_boot.ast entry 341): two curtain strips
// drawn side by side at full size (entry 2 at x 0, 652 wide; entry 4 at x 620, 664 wide), a glow strip (6), six
// player cut-outs (8-18, the silhouettes behind the curtain) and the logo (20). At start the bundle is read from your
// game files, the two curtain strips get the picture (mods/title/strip2.bin and strip4.bin: texture data in the
// game's own format, made from the project's art) and the glow, players and logo are made see-through, and the game
// is given the result in place of that entry - nothing in the game folder is changed.
// mods/title.off keeps the game's own title.
#include "archive_overlay.h"
#include "gamedb.h"
#include "deflate_lite.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

bool ReadOriginalEntry(const std::string& path, uint32_t entry, std::vector<uint8_t>& out);   // playbook_gen.cpp

static uint64_t LEr(const uint8_t* p, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i]; return v; }
static void LEw(uint8_t* p, int n, uint64_t v) { for (int i = 0; i < n; i++) { p[i] = (uint8_t)v; v >>= 8; } }
static uint32_t BE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static bool ReadFile(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = fopen(p.c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0); bool ok = n > 0 && fread(out.data(), 1, n, f) == (size_t)n; fclose(f); return ok;
}

// The texture data of an XPR2 picture (after its 0x60-byte header) is replaced; `fill` = nullptr makes it see-through.
static bool NewPixels(std::vector<uint8_t>& xpr, const std::vector<uint8_t>* fill) {
    if (xpr.size() < 0x60 || memcmp(xpr.data(), "XPR2", 4)) return false;
    uint32_t ds = BE32(&xpr[8]); if (0x60 + (size_t)ds > xpr.size()) return false;
    if (fill) { if (fill->size() != ds) return false; memcpy(&xpr[0x60], fill->data(), ds); }
    else memset(&xpr[0x60], 0, ds);   // DXT5 all zero: alpha 0 everywhere
    return true;
}

static bool PatchTitle(const std::string& arcPath, const char* name, uint32_t entry, const std::map<int, const std::vector<uint8_t>*>& edits) {
    std::vector<uint8_t> b;
    if (!ReadOriginalEntry(arcPath, entry, b) || b.size() < 0x40 || memcmp(b.data(), "BGFA1.05", 8)) return false;
    uint32_t cnt = (uint32_t)LEr(&b[0xC], 4); uint64_t toff = LEr(&b[0x10], 8), tsz = LEr(&b[0x18], 8);
    int hs = b[0x22], w1 = b[0x23], w2 = b[0x24], w3 = b[0x25], sh = b[0x26];
    uint32_t pre = 4 * (uint32_t)LEr(&b[0x28], 4), nl = (uint32_t)LEr(&b[0x2C], 4);
    if (cnt != 24 || toff + tsz > b.size() || (tsz - pre) % cnt) return false;   // not the bundle this was made for
    int rec = (int)((tsz - pre) / cnt), fb = rec - hs - w1 - w2 - w3 - (int)nl;
    if (fb < 1 || fb > 2) return false;
    struct E { uint64_t flags; std::vector<uint8_t> data; bool comp; };
    std::vector<E> es(cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t* r = &b[toff + pre + (size_t)i * rec];
        uint64_t flags = LEr(r, fb), off = LEr(r + fb + hs, w1) << sh, sz = LEr(r + fb + hs + w1, w2);
        if (off + sz > b.size()) return false;
        es[i].flags = flags; es[i].comp = flags & 1;
        std::vector<uint8_t> raw(b.begin() + off, b.begin() + off + sz);
        if (es[i].comp) { if (!gdb::Inflate(raw.data(), raw.size(), es[i].data)) return false; } else es[i].data = std::move(raw);
    }
    for (auto& [i, fill] : edits) if (i >= (int)cnt || !NewPixels(es[i].data, fill)) { fprintf(stderr, "[mods] title: picture %d did not match - not changed\n", i); return false; }
    // write the bundle again: same header and table, entries laid out one after another (compressed ones stay compressed)
    std::vector<uint8_t> out(b.begin(), b.begin() + toff + tsz);
    for (uint32_t i = 0; i < cnt; i++) {
        std::vector<uint8_t> stored = es[i].comp ? (edits.count((int)i) ? ZlibCompress(es[i].data) : std::vector<uint8_t>()) : es[i].data;
        if (es[i].comp && !edits.count((int)i)) {   // unchanged: keep the original compressed bytes
            const uint8_t* r = &b[toff + pre + (size_t)i * rec];
            uint64_t off = LEr(r + fb + hs, w1) << sh, sz = LEr(r + fb + hs + w1, w2);
            stored.assign(b.begin() + off, b.begin() + off + sz);
        }
        size_t align = (size_t)1 << sh; while (out.size() % align) out.push_back(0);
        uint8_t* r = &out[toff + pre + (size_t)i * rec];
        if ((out.size() >> sh) >= (1ull << (8 * w1)) || stored.size() >= (1ull << (8 * w2))) return false;
        LEw(r + fb + hs, w1, out.size() >> sh);
        LEw(r + fb + hs + w1, w2, stored.size());
        LEw(r + fb + hs + w1 + w2, w3, es[i].comp ? es[i].data.size() - stored.size() : 0);
        out.insert(out.end(), stored.begin(), stored.end());
    }
    while (out.size() % 64) out.push_back(0);
    OverlaySetEntry(name, entry, std::move(out));
    return true;
}

void InstallTitleMod(const std::string& gameDir, const std::string& modsDir) {
    if (FILE* f = fopen((modsDir + "/title.off").c_str(), "rb")) { fclose(f); return; }
    std::vector<uint8_t> s2, s4;
    if (!ReadFile(modsDir + "/title/strip2.bin", s2) || !ReadFile(modsDir + "/title/strip4.bin", s4)) return;
    std::map<int, const std::vector<uint8_t>*> edits{{2, &s2}, {4, &s4}};
    for (int k : {6, 8, 10, 12, 14, 16, 18, 20}) edits[k] = nullptr;   // glow, players behind the curtain, old logo
    bool a = PatchTitle(gameDir + "/qkl_interface.ast", "qkl_interface.ast", 524, edits);
    bool b = PatchTitle(gameDir + "/qkl_boot.ast", "qkl_boot.ast", 341, edits);
    fprintf(stderr, "[mods] title: %s%s%s\n", a || b ? "new title picture" : "game's own title (bundle not as expected)", a ? " [interface]" : "", b ? " [boot]" : "");
}
