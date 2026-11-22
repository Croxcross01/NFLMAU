// Loads an Xbox 360 XEX2 (retail-encrypted, uncompressed or "basic" compressed) straight into guest memory,
// so the user only needs their own Arcadegame.dll - no separate decryption step.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "aes.h"
}

static uint32_t BE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t BE16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static const uint8_t kRetailKey[16] = {0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91};

static bool OptHeader(const std::vector<uint8_t>& d, uint32_t key, uint32_t& val) {
    uint32_t cnt = BE32(&d[0x14]);
    for (uint32_t i = 0; i < cnt; i++) {
        size_t o = 0x18 + i * 8; if (o + 8 > d.size()) return false;
        if (BE32(&d[o]) == key) { val = BE32(&d[o + 4]); return true; }
    }
    return false;
}

// Import records whose slot holds a "variable" descriptor (type 0). The loader must write the variable's address
// into each slot; functions are handled by the recompiled thunks.
struct XexImportSlot { uint32_t slot; std::string lib; uint32_t ordinal; };
std::vector<XexImportSlot> g_xexImportSlots;

static void ReadImportSlots(const std::vector<uint8_t>& d, const uint8_t* img, uint32_t base, uint32_t imgSize) {
    uint32_t o = 0;
    if (!OptHeader(d, 0x103FF, o) || o + 12 > d.size()) return;
    uint32_t strSize = BE32(&d[o + 4]), nlibs = BE32(&d[o + 8]);
    std::vector<std::string> names;
    for (size_t p = o + 12; p < o + 12 + strSize;) {
        std::string n((const char*)&d[p]); if (!n.empty()) names.push_back(n);
        p += n.size() + 1; while (p < o + 12 + strSize && d[p] == 0) p++;
    }
    size_t p = o + 12 + strSize;
    for (uint32_t li = 0; li < nlibs && p + 0x28 <= d.size(); li++) {
        uint32_t lsize = BE32(&d[p]); uint16_t nameIdx = BE16(&d[p + 0x24]), count = BE16(&d[p + 0x26]);
        std::string lib = nameIdx < names.size() ? names[nameIdx] : "?";
        lib = lib.substr(0, lib.find('.'));
        for (uint32_t k = 0; k < count; k++) {
            uint32_t a = BE32(&d[p + 0x28 + 4 * k]);
            if (a < base || a + 4 > base + imgSize) continue;
            uint32_t v = BE32(img + (a - base));
            if ((v >> 24) == 0) g_xexImportSlots.push_back({a, lib, v & 0xFFFF});
        }
        p += lsize;
    }
}

// Returns empty string on success, otherwise an error message.
std::string LoadXexImage(const std::string& path, uint8_t* dst, size_t maxSize, uint32_t& baseOut, size_t& sizeOut) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "cannot open " + path;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    d.resize((size_t)n); size_t got = fread(d.data(), 1, d.size(), f); fclose(f);
    if (got != d.size() || d.size() < 0x200 || memcmp(d.data(), "XEX2", 4) != 0) return path + " is not an XEX2 file";
    uint32_t hdrSize = BE32(&d[8]), sec = BE32(&d[0x10]);
    if (sec + 0x160 > d.size() || hdrSize > d.size()) return "corrupt XEX header";
    uint32_t imgSize = BE32(&d[sec + 4]);
    if (imgSize > maxSize) return "image too large";
    uint32_t ff = 0, base = 0;
    if (!OptHeader(d, 0x3FF, ff) || !OptHeader(d, 0x10201, base)) return "XEX is missing required headers";
    uint32_t infoSize = BE32(&d[ff]); uint16_t enc = BE16(&d[ff + 4]), comp = BE16(&d[ff + 6]);

    std::vector<uint8_t> body(d.begin() + hdrSize, d.end());
    if (enc == 1) {
        uint8_t key[16]; memcpy(key, &d[sec + 0x150], 16);
        AES_ctx c; AES_init_ctx(&c, kRetailKey); AES_ECB_decrypt(&c, key);            // session key
        uint8_t iv[16] = {0}; AES_init_ctx_iv(&c, key, iv);
        AES_CBC_decrypt_buffer(&c, body.data(), body.size() & ~(size_t)15);
    } else if (enc != 0) return "unsupported XEX encryption";

    memset(dst, 0, imgSize);
    if (comp == 0) {
        memcpy(dst, body.data(), std::min<size_t>(imgSize, body.size()));
    } else if (comp == 1) {
        uint32_t blocks = infoSize / 8 - 1; size_t src = 0, out = 0;
        for (uint32_t i = 0; i < blocks; i++) {
            uint32_t dataSize = BE32(&d[ff + 8 + i * 8]), zeroSize = BE32(&d[ff + 12 + i * 8]);
            if (src + dataSize > body.size() || out + dataSize + zeroSize > imgSize) return "corrupt compression table";
            memcpy(dst + out, body.data() + src, dataSize); src += dataSize; out += dataSize + zeroSize;
        }
    } else return "XEX uses LZX compression, which this loader does not support";
    if (dst[0] != 'M' || dst[1] != 'Z') return "decryption failed (wrong key or damaged file)";
    ReadImportSlots(d, dst, base, imgSize);
    baseOut = base; sizeOut = imgSize;
    return {};
}
