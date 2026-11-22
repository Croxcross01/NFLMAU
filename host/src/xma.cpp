// XMA audio decoder contexts.
// The 64-byte context layout and the kernel helper semantics follow Xenia (apu/xma_context.h,
// kernel/xboxkrnl/xboxkrnl_audio_xma.cc; BSD-3-Clause, Copyright Ben Vanik and Xenia contributors).
//
// A worker decodes every enabled context into its output ring (FFmpeg's XMA frame decoder). Without a decoder it
// falls back to consuming packets at real-time speed and writing silence, which still keeps game logic that waits
// for a sound moving.
#include "kernel.h"
#include <algorithm>
#include <memory>
#include <tuple>
#include <vector>
#ifndef MADDEN_NO_FFMPEG   // build without FFmpeg: audio falls back to silence
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif
#include <chrono>
#include <map>
#include <mutex>
#include <condition_variable>
#include <set>
#include <thread>
#include <string>
void PerfName(const std::string& name);
void PerfStart();

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

uint32_t GuestPhysicalAddress(uint32_t a);

namespace {
std::mutex g_xmaLock;
std::condition_variable_any g_xmaWake;   // kicked when the game gives a context new input/output space
std::set<uint32_t> g_enabled;
struct Progress { uint32_t packet = 0; int framesLeft = -1; double samplePos = 0, frameAcc = 0; };
std::map<uint32_t, Progress> g_progress;   // per context (guest address)
bool g_xmaLog = getenv("MADDEN_XMA_LOG") != nullptr;

struct Ctx {
    uint32_t addr; uint32_t d[16];
    explicit Ctx(uint32_t a) : addr(a) { for (int i = 0; i < 16; i++) d[i] = ReadU32(a + i * 4); }
    void Store() { for (int i = 0; i < 16; i++) WriteU32(addr + i * 4, d[i]); }
    uint32_t Get(int dw, int lo, int n) const { return (d[dw] >> lo) & ((1u << n) - 1); }
    void Set(int dw, int lo, int n, uint32_t v) { uint32_t m = ((1u << n) - 1) << lo; d[dw] = (d[dw] & ~m) | ((v << lo) & m); }
    // fields
    uint32_t in0Count() const { return Get(0, 0, 12); }
    uint32_t in1Count() const { return Get(1, 0, 12); }
    bool in0Valid() const { return Get(0, 20, 1); }
    bool in1Valid() const { return Get(0, 21, 1); }
    uint32_t outBlocks() const { return Get(0, 22, 5); }
    uint32_t outWrite() const { return Get(0, 27, 5); }
    bool stereo() const { return Get(1, 29, 1); }
    uint32_t rateIdx() const { return Get(1, 27, 2); }
    bool outValid() const { return Get(1, 31, 1); }
    uint32_t outRead() const { return Get(9, 0, 5); }
    uint32_t curBuf() const { return Get(4, 31, 1); }
};
uint32_t Phys(uint32_t v) { return v ? GuestPhysicalAddress(v) : 0; }
uint32_t SampleRate(uint32_t idx) { switch (idx) { case 0: return 24000; case 1: return 32000; case 2: return 44100; default: return 48000; } }

// ---------------------------------------------------------------- real decoding (FFmpeg "xmaframes")
// XMA streams are 2 KB packets: a 32-bit header (frame count, bit offset of the first frame that starts in the
// packet, metadata, packet skip count) followed by a bitstream of frames. Each frame starts with a 15-bit length and
// can be split across packets. Frames are reassembled into a buffer and handed to the decoder one at a time;
// each yields 512 samples per channel, written to the context's output ring as big-endian 16-bit PCM.
// Packet/frame walking follows Xenia's XmaContext (BSD-3-Clause); this is an independent implementation of it.
constexpr uint32_t kPacketBytes = 2048, kPacketBits = kPacketBytes * 8, kMaxFrameBits = 0x7FFF;
uint32_t FirstFrameOffset(const uint8_t* p) { return ((uint32_t(p[0] & 3) << 13) | (uint32_t(p[1]) << 5) | (p[2] >> 3)) + 32; }
uint32_t SkipCount(const uint8_t* p) { return p[3]; }

struct Bits {
    const uint8_t* buf; size_t size, off = 0;   // size and off in bits
    Bits(const uint8_t* b, size_t bits) : buf(b), size(bits) {}
    size_t Left() const { return off < size ? size - off : 0; }
    void Seek(size_t o) { off = std::min(o, size); }
    void Skip(size_t n) { Seek(off + n); }
    uint64_t Peek(size_t n) const {   // MSB-first; reads past the end return zero bits
        uint64_t v = 0;
        for (size_t i = 0; i < n; i++) {
            size_t b = off + i;
            uint32_t bit = (b < size) ? (buf[b >> 3] >> (7 - (b & 7))) & 1 : 0;
            v = (v << 1) | bit;
        }
        return v;
    }
    uint64_t Read(size_t n) { uint64_t v = Peek(n); Skip(n); return v; }
    // Copy n bits into dst keeping the source's position within the byte (dst bit k%8 == source bit off%8).
    // Returns that starting bit position.
    size_t Copy(uint8_t* dst, size_t n) {
        size_t start = off & 7, d = start;
        for (size_t i = 0; i < n; i++, d++) {
            uint32_t bit = (uint32_t)Read(1);
            uint8_t m = uint8_t(0x80 >> (d & 7));
            if (bit) dst[d >> 3] |= m; else dst[d >> 3] &= ~m;
        }
        return start;
    }
};

#ifndef MADDEN_NO_FFMPEG
struct Decoder {
    AVCodecContext* av = nullptr; AVPacket* pkt = nullptr; AVFrame* frm = nullptr;
    int rate = 0, channels = 0;
    // split-frame reassembly
    int splitLen = 0, splitPartial = 0, packetsSkip = 0; uint32_t padStart = 0;
    std::vector<uint8_t> frame = std::vector<uint8_t>(kMaxFrameBits / 8 + 16);
    bool ok = false;
    bool Init() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
        if (!codec) return false;
        av = avcodec_alloc_context3(codec); pkt = av_packet_alloc(); frm = av_frame_alloc();
        ok = av && pkt && frm;
        return ok;
    }
    bool Prepare(int r, int ch) {
        if (r == rate && ch == channels) return true;
        if (avcodec_is_open(av)) avcodec_close(av);
        av->sample_rate = r; av->channels = ch;
        rate = r; channels = ch;
        return avcodec_open2(av, av->codec ? av->codec : avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES), nullptr) >= 0;
    }
};
std::map<uint32_t, std::unique_ptr<Decoder>> g_decoders;
bool g_codecMissing = false;

int FramePacket(size_t sizeBytes, size_t bitOff) { return bitOff >= sizeBytes * 8 ? -1 : int((bitOff >> 3) / kPacketBytes); }
// number of frames that start in a packet, and whether the last one continues in a later packet
std::pair<int, bool> PacketFrames(const uint8_t* packet) {
    uint32_t first = FirstFrameOffset(packet);
    if (first > kPacketBits - 33) return {0, false};
    Bits b(packet, kPacketBits); b.Seek(first);
    int n = 0;
    for (;;) {
        n++;
        if (b.Left() < 15) return {n, true};
        uint64_t len = b.Read(15);
        if (len < 16 || len - 15 > b.Left() || len == kMaxFrameBits) return {n, true};
        b.Skip(len - 16);
        if (b.Read(1) == 0) return {n, false};
    }
}
bool ValidFrame(const uint8_t* block, size_t sizeBytes, size_t bitOff) {
    int p = FramePacket(sizeBytes, bitOff);
    if (p < 0) return false;
    const uint8_t* packet = block + size_t(p) * kPacketBytes;
    size_t rel = bitOff % kPacketBits;
    uint32_t first = FirstFrameOffset(packet);
    if (first > kPacketBits) return false;
    Bits b(packet, kPacketBits); b.Seek(first);
    for (;;) {
        if (b.off == rel) return true;
        if (b.Left() < 15) return false;
        uint64_t len = b.Read(15);
        if (len < 16 || len - 15 > b.Left() || len == kMaxFrameBits) return false;
        b.Skip(len - 16);
        if (b.Read(1) == 0) return false;
    }
}
std::pair<int, int> FrameIndex(const uint8_t* block, size_t sizeBytes, size_t bitOff) {
    int p = FramePacket(sizeBytes, bitOff);
    if (p < 0 || size_t(p + 1) * kPacketBytes > sizeBytes) return {p, -2};
    if (bitOff == 0) return {p, -1};
    Bits b(block, sizeBytes * 8); b.Seek(size_t(p) * kPacketBits + FirstFrameOffset(block + size_t(p) * kPacketBytes));
    int idx = 0;
    for (;;) {
        if (b.Left() < 15 || b.off == bitOff) break;
        uint64_t len = b.Read(15);
        if (len < 16 || len - 15 > b.Left() || len == kMaxFrameBits) break;
        b.Skip(len - 16);
        if (b.Read(1) == 0) break;
        idx++;
    }
    return {p, idx};
}
size_t NextFrame(const uint8_t* block, size_t sizeBytes, size_t bitOff) {
    int p = FramePacket(sizeBytes, bitOff);
    Bits b(block, sizeBytes * 8); b.Seek(bitOff);
    if (b.Left() < 15) return 0;
    uint64_t len = b.Read(15);
    if (len < 16 || len - 15 > b.Left() || len >= kMaxFrameBits) return 0;
    b.Skip(len - 16);
    if (b.Read(1) == 0) return 0;
    bitOff += len;
    if (p < FramePacket(sizeBytes, bitOff)) return 0;
    return bitOff;
}

void SwapInput(Ctx& c) {
    if (c.curBuf()) c.Set(0, 21, 1, 0); else c.Set(0, 20, 1, 0);
    c.Set(4, 31, 1, c.curBuf() ^ 1);
    c.Set(2, 0, 26, 0);
}
bool NextLoop(Ctx& c, bool ignoreOffset) {
    uint32_t count = c.Get(0, 12, 8), ls = c.Get(3, 0, 26), le = c.Get(4, 0, 26);
    if (count > 0 && ls < le && (ignoreOffset || c.Get(2, 0, 26) >= le)) {
        c.Set(2, 0, 26, ls);
        if (count < 255) c.Set(0, 12, 8, count - 1);
        return true;
    }
    return false;
}

// Decode as much as the output ring has room for. Called with g_xmaLock held. Returns false when there is no
// decoder (the caller then falls back to timed silence).
bool Decode(uint32_t addr) {
    auto& dp = g_decoders[addr];
    if (!dp) {
        dp = std::make_unique<Decoder>();
        if (!dp->Init()) { g_codecMissing = true; fprintf(stderr, "[xma] no XMA decoder available - silence\n"); return false; }
        static bool once = (fprintf(stderr, "[xma] decoding XMA audio\n"), true); (void)once;
    }
    Decoder& D = *dp;
    Ctx c(addr);
    if (!c.outValid()) return true;
    if (!c.in0Valid() && !c.in1Valid()) { c.Set(1, 31, 1, 0); c.Store(); return true; }
    uint32_t outBlocks = c.outBlocks();
    if (!outBlocks || !c.d[7]) return true;
    bool stereo = c.stereo();
    size_t frameBytes = size_t(512 * 2) << (stereo ? 1 : 0);
    size_t capacity = size_t(outBlocks) * 256;
    uint8_t* out = g_base + GuestFromPhysical(c.d[7]);
    size_t rd = size_t(c.outRead()) * 256, wr = size_t(c.outWrite()) * 256;
    size_t room = rd > wr ? rd - wr : capacity - (wr - rd);   // read == write means empty
    room -= room % frameBytes;
    bool reuse = false;
    auto endOfBuffer = [&](bool stop) {
        if (!reuse) reuse = NextLoop(c, true);
        if (!reuse) SwapInput(c);
        (void)stop;
    };
    while (room > 0) {
        if (!c.in0Valid() && !c.in1Valid()) break;
        uint32_t cur = c.curBuf();
        const uint8_t* in = g_base + GuestFromPhysical(cur ? c.d[6] : c.d[5]);
        size_t inSize = size_t(cur ? c.in1Count() : c.in0Count()) * kPacketBytes;
        size_t inPackets = inSize / kPacketBytes;
        if (!inSize || !(cur ? c.d[6] : c.d[5])) { SwapInput(c); continue; }
        reuse = NextLoop(c, false);
        int pIdx = 0, fIdx = 0, fCount = 0; bool lastSplit = false;
        const uint8_t* packet = nullptr;
        size_t rdOff = c.Get(2, 0, 26);
        if (D.packetsSkip > 0) {
            int p = FramePacket(inSize, rdOff);
            if (p < 0) p = 0;
            bool ended = false;
            while (D.packetsSkip > 0) { D.packetsSkip--; if (size_t(++p) >= inPackets) { ended = true; break; } }
            if (ended) { endOfBuffer(true); break; }
            rdOff = size_t(p) * kPacketBits; c.Set(2, 0, 26, uint32_t(rdOff));
        }
        if (D.splitLen) {
            // finish a frame whose start was in an earlier packet
            pIdx = FramePacket(inSize, rdOff);
            if (pIdx < 0) { SwapInput(c); break; }
            packet = in + size_t(pIdx) * kPacketBytes;
            std::tie(fCount, lastSplit) = PacketFrames(packet);
            fIdx = -1;
            Bits b(in, size_t(pIdx + 1) * kPacketBits); b.Seek(size_t(pIdx) * kPacketBits + 32);
            if (D.splitLen > int(kMaxFrameBits)) {   // even the 15-bit length was split
                size_t save = b.off;
                b.Copy(D.frame.data() + 1 + ((D.splitPartial + D.padStart) / 8), size_t(15 - D.splitPartial));
                b.Seek(save);
                Bits L(D.frame.data() + 1, 15 + D.padStart); L.Seek(D.padStart);
                D.splitLen = int(L.Read(15));
            }
            if (D.splitLen < D.splitPartial || D.splitLen > int(kMaxFrameBits)) {   // corrupt stream: drop it
                D.splitLen = D.splitPartial = 0; D.padStart = 0; D.packetsSkip = 0; SwapInput(c); break;
            }
            b.Copy(D.frame.data() + 1 + ((D.splitPartial + D.padStart) / 8), size_t(D.splitLen - D.splitPartial));
        } else {
            if (rdOff % kPacketBits == 0) {
                int p = FramePacket(inSize, rdOff);
                if (p < 0) { SwapInput(c); break; }
                rdOff += FirstFrameOffset(in + size_t(p) * kPacketBytes);
                c.Set(2, 0, 26, uint32_t(rdOff));
            }
            if (!ValidFrame(in, inSize, rdOff)) { SwapInput(c); break; }
            std::tie(pIdx, fIdx) = FrameIndex(in, inSize, rdOff);
            if (pIdx < 0 || fIdx < -1) { SwapInput(c); break; }
            packet = in + size_t(pIdx) * kPacketBytes;
            std::tie(fCount, lastSplit) = PacketFrames(packet);
            if (!D.Prepare(SampleRate(c.rateIdx()), stereo ? 2 : 1)) return false;
            bool isSplit = lastSplit && fIdx >= fCount - 1;
            Bits b(in, size_t(pIdx + 1) * kPacketBits); b.Seek(rdOff);
            D.splitPartial = int(b.Left());
            D.splitLen = D.splitPartial >= 15 ? int(b.Peek(15)) : int(kMaxFrameBits) + 1;
            if (D.splitLen < 16 || (!isSplit && D.splitLen > D.splitPartial)) {   // bad frame length: skip the buffer
                D.splitLen = D.splitPartial = 0; D.padStart = 0; SwapInput(c); break;
            }
            std::fill(D.frame.begin(), D.frame.end(), 0);
            D.padStart = uint32_t(b.Copy(D.frame.data() + 1, size_t(std::min(D.splitLen, D.splitPartial))));
            if (isSplit) {
                D.packetsSkip = int(SkipCount(packet)) + 1;
                bool ended = false;
                while (D.packetsSkip > 0) { D.packetsSkip--; if (size_t(++pIdx) >= inPackets) { ended = true; break; } }
                if (ended) { endOfBuffer(true); break; }
                c.Set(2, 0, 26, uint32_t(size_t(pIdx) * kPacketBits));
                continue;
            }
        }
        // hand the assembled frame to the decoder: byte 0 carries the leading/trailing pad bit counts
        size_t bits = D.padStart + size_t(D.splitLen);
        D.pkt->data = D.frame.data();
        D.pkt->size = int(1 + bits / 8 + ((bits % 8) ? 1 : 0));
        uint32_t padEnd = uint32_t(D.pkt->size * 8 - (8 + bits));
        D.frame[0] = uint8_t(((D.padStart & 7) << 5) | ((padEnd & 7) << 2));
        D.splitLen = D.splitPartial = 0; D.padStart = 0;
        bool got = avcodec_send_packet(D.av, D.pkt) >= 0 && avcodec_receive_frame(D.av, D.frm) >= 0;
        // write 512 samples per channel as interleaved big-endian int16 (silence if the frame failed)
        {
            const float* ch0 = got ? (const float*)D.frm->data[0] : nullptr;
            const float* ch1 = got && stereo ? (const float*)D.frm->data[1] : nullptr;
            int ns = got ? std::min(D.frm->nb_samples, 512) : 0;
            uint32_t nch = stereo ? 2 : 1;
            for (uint32_t i = 0; i < 512; i++) for (uint32_t k = 0; k < nch; k++) {
                float v = i < uint32_t(ns) ? (k ? ch1[i] : ch0[i]) : 0.0f;
                int32_t sv = int32_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
                uint16_t u = uint16_t(int16_t(sv));
                size_t o = (wr + (size_t(i) * nch + k) * 2) % capacity;
                out[o] = uint8_t(u >> 8); out[o + 1] = uint8_t(u);
            }
            wr = (wr + frameBytes) % capacity;
            room -= frameBytes;
            c.Set(0, 27, 5, uint32_t(wr / 256));
        }
        // advance to the next frame
        size_t next = NextFrame(in, inSize, c.Get(2, 0, 26));
        if (fIdx + 1 >= fCount) {
            D.packetsSkip = int(SkipCount(packet)) + 1;
            bool ended = false;
            while (D.packetsSkip > 0) { D.packetsSkip--; if (size_t(++pIdx) >= inPackets) { ended = true; break; } }
            if (ended) { endOfBuffer(true); break; }
            packet = in + size_t(pIdx) * kPacketBytes;
            next = FirstFrameOffset(packet) + size_t(pIdx) * kPacketBits;
        }
        if (next == 0 || fIdx == -1) {
            if (size_t(pIdx) >= inPackets) { endOfBuffer(true); break; }
            next = FirstFrameOffset(packet) + size_t(pIdx) * kPacketBits;
        }
        c.Set(2, 0, 26, uint32_t(next));
    }
    if (wr == rd) c.Set(1, 31, 1, 0);   // ring full: the game must consume before more is decoded
    c.Store();
    return true;
}

#else
struct Decoder {};
std::map<uint32_t, std::unique_ptr<Decoder>> g_decoders;
bool g_codecMissing = true;
bool Decode(uint32_t) { return false; }
#endif

// Fallback without a decoder: advance one context by `seconds` of playback, writing silence.
// Advance one context by `seconds` of playback. Called with g_xmaLock held.
void Advance(uint32_t addr, double seconds) {
    Ctx c(addr);
    if (!c.outValid() || !c.outBlocks()) return;
    Progress& p = g_progress[addr];
    uint32_t channels = c.stereo() ? 2 : 1;
    double samplesPerBlock = 256.0 / (2.0 * channels);
    p.samplePos += seconds * SampleRate(c.rateIdx());
    uint32_t count = c.outBlocks(), w = c.outWrite(), r = c.outRead();
    uint32_t outPhys = c.d[7];
    bool changed = false;
    while (p.samplePos >= samplesPerBlock) {
        // need input to produce output
        bool cur = c.curBuf();
        bool valid = cur ? c.in1Valid() : c.in0Valid();
        if (!valid) {
            bool other = cur ? c.in0Valid() : c.in1Valid();
            if (!other) { p.samplePos = 0; break; }   // starved: nothing queued
            c.Set(4, 31, 1, cur ? 0 : 1); cur = !cur; p.packet = 0; p.framesLeft = -1; changed = true;
        }
        // output ring space (keep one block free so full and empty differ)
        uint32_t used = (w + count - r) % count;
        if (used + 1 >= count) { p.samplePos = std::min(p.samplePos, samplesPerBlock * 2); break; }
        if (outPhys) memset(g_base + GuestFromPhysical(outPhys) + w * 256, 0, 256);
        w = (w + 1) % count;
        p.samplePos -= samplesPerBlock;
        changed = true;
        // input consumption: 512 samples per XMA frame, frame count from each 2 KB packet header
        p.frameAcc += samplesPerBlock;
        while (p.frameAcc >= 512) {
            p.frameAcc -= 512;
            uint32_t pkts = cur ? c.in1Count() : c.in0Count(), base = cur ? c.d[6] : c.d[5];
            if (p.framesLeft < 0) {
                uint32_t hdr = (base && p.packet < pkts) ? ReadU32(GuestFromPhysical(base) + p.packet * 2048) : 0;
                p.framesLeft = std::max<int>(1, int(hdr >> 26));
            }
            if (--p.framesLeft <= 0) {
                p.framesLeft = -1;
                if (++p.packet >= std::max<uint32_t>(pkts, 1)) {   // buffer finished: hand it back to the game
                    if (cur) c.Set(0, 21, 1, 0); else c.Set(0, 20, 1, 0);
                    c.Set(4, 31, 1, cur ? 0 : 1);
                    c.Set(2, 0, 26, 32);
                    p.packet = 0;
                    if (g_xmaLog) fprintf(stderr, "[xma] context %08X finished input buffer %d\n", addr, (int)cur);
                    break;
                }
                c.Set(2, 0, 26, p.packet * 2048 * 8 + 32);
            }
        }
    }
    if (changed) { c.Set(0, 27, 5, w); c.Store(); }
}

void Worker() {
    { PerfName("xma decode"); }
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        std::unique_lock<std::mutex> l(g_xmaLock);
        g_xmaWake.wait_for(l, std::chrono::milliseconds(1));
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count(); last = now;
        for (uint32_t a : g_enabled) { if (g_codecMissing || !Decode(a)) Advance(a, std::min(dt, 0.05)); }
    }
}
void EnsureWorker() { static bool started = (std::thread(Worker).detach(), true); (void)started; }
}  // namespace

IMP(XMAInitializeContext) {
    TRACE(XMAInitializeContext);
    std::lock_guard<std::mutex> l(g_xmaLock);
    uint32_t a = ctx.r3.u32, in = ctx.r4.u32;
    auto f = [&](int i) { return ReadU32(in + i * 4); };
    memset(g_base + a, 0, 64);
    Ctx c(a);
    c.d[5] = Phys(f(0)); c.Set(0, 0, 12, f(1));
    c.d[6] = Phys(f(2)); c.Set(1, 0, 12, f(3));
    c.Set(2, 0, 26, f(4));
    c.d[7] = Phys(f(5)); c.Set(0, 22, 5, f(6));
    c.Set(1, 20, 4, f(8));
    c.Set(1, 29, 1, f(9) >= 1 ? 1 : 0);
    c.Set(1, 27, 2, f(10));
    // loop data (XMA_LOOP_DATA at +44): start, end, count, subframe end, subframe skip
    c.Set(3, 0, 26, f(11)); c.Set(4, 0, 26, f(12));
    uint32_t lb = f(13);
    c.Set(0, 12, 8, lb >> 24); c.Set(1, 14, 3, (lb >> 16) & 0xFF); c.Set(1, 17, 3, (lb >> 8) & 0xFF);
    c.Store();
    g_decoders.erase(a);
    g_progress[a] = Progress{};
    EnsureWorker();
    ctx.r3.u64 = 0;
}
IMP(XMASetInputBufferReadOffset) { std::lock_guard<std::mutex> l(g_xmaLock); Ctx c(ctx.r3.u32); c.Set(2, 0, 26, ctx.r4.u32); c.Store(); ctx.r3.u64 = 0; }
IMP(XMASetInputBuffer0) { std::lock_guard<std::mutex> l(g_xmaLock); Ctx c(ctx.r3.u32); c.d[5] = Phys(ctx.r4.u32); c.Set(0, 0, 12, ctx.r5.u32); c.Store(); ctx.r3.u64 = 0; }
IMP(XMASetInputBuffer1) { std::lock_guard<std::mutex> l(g_xmaLock); Ctx c(ctx.r3.u32); c.d[6] = Phys(ctx.r4.u32); c.Set(1, 0, 12, ctx.r5.u32); c.Store(); ctx.r3.u64 = 0; }
IMP(XMAIsInputBuffer0Valid) { std::lock_guard<std::mutex> l(g_xmaLock); ctx.r3.u64 = Ctx(ctx.r3.u32).in0Valid(); }
IMP(XMAIsInputBuffer1Valid) { std::lock_guard<std::mutex> l(g_xmaLock); ctx.r3.u64 = Ctx(ctx.r3.u32).in1Valid(); }
IMP(XMASetInputBuffer0Valid) { std::lock_guard<std::mutex> l(g_xmaLock); Ctx c(ctx.r3.u32); c.Set(0, 20, 1, 1); c.Store(); ctx.r3.u64 = 0; g_xmaWake.notify_one(); }
IMP(XMASetInputBuffer1Valid) { std::lock_guard<std::mutex> l(g_xmaLock); Ctx c(ctx.r3.u32); c.Set(0, 21, 1, 1); c.Store(); ctx.r3.u64 = 0; g_xmaWake.notify_one(); }
IMP(XMAIsOutputBufferValid) { std::lock_guard<std::mutex> l(g_xmaLock); ctx.r3.u64 = Ctx(ctx.r3.u32).outValid(); }
IMP(XMASetOutputBufferValid) { std::lock_guard<std::mutex> l(g_xmaLock); Ctx c(ctx.r3.u32); c.Set(1, 31, 1, 1); c.Store(); ctx.r3.u64 = 0; g_xmaWake.notify_one(); }
IMP(XMAGetOutputBufferReadOffset) { std::lock_guard<std::mutex> l(g_xmaLock); ctx.r3.u64 = Ctx(ctx.r3.u32).outRead(); }
IMP(XMASetOutputBufferReadOffset) { std::lock_guard<std::mutex> l(g_xmaLock); Ctx c(ctx.r3.u32); c.Set(9, 0, 5, ctx.r4.u32); c.Store(); ctx.r3.u64 = 0; g_xmaWake.notify_one(); }
IMP(XMAGetOutputBufferWriteOffset) { std::lock_guard<std::mutex> l(g_xmaLock); ctx.r3.u64 = Ctx(ctx.r3.u32).outWrite(); }
IMP(XMAEnableContext) { std::lock_guard<std::mutex> l(g_xmaLock); g_enabled.insert(ctx.r3.u32); EnsureWorker(); ctx.r3.u64 = 0; g_xmaWake.notify_one(); }
IMP(XMADisableContext) { std::lock_guard<std::mutex> l(g_xmaLock); g_enabled.erase(ctx.r3.u32); ctx.r3.u64 = 0; }
