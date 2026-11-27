// Small zlib/deflate encoder (LZ77 with lazy matching + dynamic Huffman codes), close to zlib level 9 on small
// text. Used to store an edited archive entry compressed the way the game expects, ideally in the space the original
// entry had. Output is a standard zlib stream.
#pragma once
#include <algorithm>
#include <cstdint>
#include <queue>
#include <vector>

namespace deflate_lite {
struct Tok { uint16_t lit; uint16_t len; uint16_t dist; };   // len 0: literal 'lit'

inline void Lengths(const std::vector<uint32_t>& freq, int limit, std::vector<uint8_t>& len) {
    int n = int(freq.size()); len.assign(n, 0);
    std::vector<int> used; for (int i = 0; i < n; i++) if (freq[i]) used.push_back(i);
    if (used.empty()) return;
    if (used.size() == 1) { len[used[0]] = 1; return; }
    struct Node { uint64_t w; int l, r, sym; };
    std::vector<Node> nodes;
    using P = std::pair<uint64_t, int>;
    std::priority_queue<P, std::vector<P>, std::greater<P>> q;
    for (int s : used) { nodes.push_back({freq[s], -1, -1, s}); q.push({freq[s], int(nodes.size()) - 1}); }
    while (q.size() > 1) {
        auto a = q.top(); q.pop(); auto b = q.top(); q.pop();
        nodes.push_back({a.first + b.first, a.second, b.second, -1}); q.push({a.first + b.first, int(nodes.size()) - 1});
    }
    std::vector<std::pair<int, int>> st = {{q.top().second, 0}};
    while (!st.empty()) {
        auto [k, d] = st.back(); st.pop_back();
        if (nodes[k].sym >= 0) len[nodes[k].sym] = uint8_t(std::max(1, d));
        else { st.push_back({nodes[k].l, d + 1}); st.push_back({nodes[k].r, d + 1}); }
    }
    // length limit: clamp, then lengthen shorter codes until the code is valid again (Kraft sum <= 1)
    for (int s : used) if (len[s] > limit) len[s] = uint8_t(limit);
    auto kraft = [&]() { uint64_t k = 0; for (int s : used) k += 1ull << (limit - len[s]); return k; };
    while (kraft() > (1ull << limit)) {
        int best = -1;
        for (int s : used) if (len[s] < limit && (best < 0 || len[s] > len[best] || (len[s] == len[best] && freq[s] < freq[best]))) best = s;
        if (best < 0) break;
        len[best]++;
    }
}
inline std::vector<uint32_t> Codes(const std::vector<uint8_t>& len) {
    std::vector<uint32_t> code(len.size(), 0); int bl[16] = {0}, next[16] = {0};
    for (uint8_t l : len) if (l) bl[l]++;
    int c = 0; for (int b = 1; b < 16; b++) { c = (c + bl[b - 1]) << 1; next[b] = c; }
    for (size_t i = 0; i < len.size(); i++) if (len[i]) code[i] = uint32_t(next[len[i]]++);
    return code;
}
}   // namespace deflate_lite

inline std::vector<uint8_t> ZlibCompress(const std::vector<uint8_t>& in) {
    using namespace deflate_lite;
    static const int lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    auto lcode = [&](int l) { int c = 0; while (c < 28 && lbase[c + 1] <= l) c++; return c; };
    auto dcode = [&](int d) { int c = 0; while (c < 29 && dbase[c + 1] <= d) c++; return c; };
    // LZ77 with lazy matching
    const size_t n = in.size();
    std::vector<int> head(1 << 15, -1), prev(n + 1, -1);
    auto hash = [&](size_t i) { return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & 0x7FFF; };
    auto insert = [&](size_t k) { if (k + 2 < n) { int h = hash(k); prev[k] = head[h]; head[h] = int(k); } };
    auto find = [&](size_t i, int& bestDist) {
        int bestLen = 0; bestDist = 0;
        if (i + 2 >= n) return 0;
        int cand = head[hash(i)], tries = 4096;
        while (cand >= 0 && tries-- > 0 && i - size_t(cand) <= 32768) {
            int l = 0; while (l < 258 && i + l < n && in[cand + l] == in[i + l]) l++;
            if (l > bestLen) { bestLen = l; bestDist = int(i - cand); if (l == 258) break; }
            cand = prev[cand];
        }
        return bestLen >= 3 ? bestLen : 0;
    };
    std::vector<Tok> toks;
    size_t i = 0;
    while (i < n) {
        int d1, l1 = find(i, d1);
        if (l1) {
            insert(i);
            int d2, l2 = find(i + 1, d2);
            if (l2 > l1) { toks.push_back({in[i], 0, 0}); i++; continue; }   // a longer match starts next: emit a literal
            toks.push_back({0, uint16_t(l1), uint16_t(d1)});
            for (int k = 1; k < l1; k++) insert(i + k);
            i += l1;
        } else { toks.push_back({in[i], 0, 0}); insert(i); i++; }
    }
    // Huffman codes for this data
    std::vector<uint32_t> lf(286, 0), df(30, 0);
    for (auto& t : toks) { if (!t.len) lf[t.lit]++; else { lf[257 + lcode(t.len)]++; df[dcode(t.dist)]++; } }
    lf[256] = 1;
    bool anyDist = false; for (auto f : df) if (f) anyDist = true;
    if (!anyDist) df[0] = 1;
    std::vector<uint8_t> ll, dl; Lengths(lf, 15, ll); Lengths(df, 15, dl);
    int hlit = 286; while (hlit > 257 && !ll[hlit - 1]) hlit--;
    int hdist = 30; while (hdist > 1 && !dl[hdist - 1]) hdist--;
    std::vector<uint8_t> all(ll.begin(), ll.begin() + hlit); all.insert(all.end(), dl.begin(), dl.begin() + hdist);
    struct CL { int sym, extra, ebits; };
    std::vector<CL> cls;
    for (size_t k = 0; k < all.size();) {
        size_t r = 1; while (k + r < all.size() && all[k + r] == all[k]) r++;
        if (all[k] == 0 && r >= 3) { size_t m = std::min<size_t>(r, 138); if (m >= 11) cls.push_back({18, int(m - 11), 7}); else cls.push_back({17, int(m - 3), 3}); k += m; }
        else if (all[k] != 0 && r >= 4) { cls.push_back({all[k], 0, 0}); size_t m = std::min<size_t>(r - 1, 6); cls.push_back({16, int(m - 3), 2}); k += 1 + m; }
        else { cls.push_back({all[k], 0, 0}); k++; }
    }
    std::vector<uint32_t> cf(19, 0); for (auto& c : cls) cf[c.sym]++;
    std::vector<uint8_t> cll; Lengths(cf, 7, cll);
    static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hclen = 19; while (hclen > 4 && !cll[order[hclen - 1]]) hclen--;
    auto lc = Codes(ll), dc = Codes(dl), cc = Codes(cll);
    // write
    std::vector<uint8_t> out = {0x78, 0xDA};
    uint32_t bitbuf = 0; int bits = 0;
    auto put = [&](uint32_t v, int nb) { bitbuf |= v << bits; bits += nb; while (bits >= 8) { out.push_back(uint8_t(bitbuf)); bitbuf >>= 8; bits -= 8; } };
    auto huff = [&](uint32_t c, int nb) { uint32_t r = 0; for (int k = 0; k < nb; k++) { r = (r << 1) | (c & 1); c >>= 1; } put(r, nb); };
    put(1, 1); put(2, 2);   // final block, dynamic Huffman
    put(uint32_t(hlit - 257), 5); put(uint32_t(hdist - 1), 5); put(uint32_t(hclen - 4), 4);
    for (int k = 0; k < hclen; k++) put(cll[order[k]], 3);
    for (auto& c : cls) { huff(cc[c.sym], cll[c.sym]); if (c.ebits) put(uint32_t(c.extra), c.ebits); }
    for (auto& t : toks) {
        if (!t.len) { huff(lc[t.lit], ll[t.lit]); continue; }
        int c = lcode(t.len); huff(lc[257 + c], ll[257 + c]); if (lext[c]) put(uint32_t(t.len - lbase[c]), lext[c]);
        int d = dcode(t.dist); huff(dc[d], dl[d]); if (dext[d]) put(uint32_t(t.dist - dbase[d]), dext[d]);
    }
    huff(lc[256], ll[256]);
    if (bits) put(0, 8 - bits);
    uint32_t a = 1, b = 0; for (uint8_t c : in) { a = (a + c) % 65521; b = (b + a) % 65521; }
    uint32_t ad = (b << 16) | a;
    out.push_back(uint8_t(ad >> 24)); out.push_back(uint8_t(ad >> 16)); out.push_back(uint8_t(ad >> 8)); out.push_back(uint8_t(ad));
    return out;
}
