// Xenos GPU front end: ring buffer, PM4 command processor, vblank and VdSwap.
// Packet semantics follow Xenia's CommandProcessor (BSD-3-Clause, Copyright Ben Vanik and contributors).
// This is the "null GPU" stage: every packet is executed for its side effects (register writes, memory
// writes, fences, waits, interrupts, swaps) but draws are not rendered yet.
#include "kernel.h"
#include "gpu_vk.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <thread>
#include <chrono>
#include <vector>
#include <map>
#include <mutex>
#include <string>
void PerfName(const std::string& name);
void PerfStart();

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

void PresenterSubmitSwap(const uint32_t fetch[6], uint32_t phys, uint32_t width, uint32_t height);
void PresenterSubmitRenderedClear();
void RendererSetShader(uint32_t type, uint64_t hash, const std::vector<uint32_t>& dwords);

namespace {
constexpr uint32_t kGpuMmio = 0x7FC80000;          // GPU register aperture as the game addresses it
constexpr uint32_t kRegCount = 0x5003;
constexpr uint32_t REG_CP_RB_RPTR = 0x01C4, REG_CP_RB_WPTR = 0x01C5, REG_SCRATCH_UMSK = 0x01DC, REG_SCRATCH_ADDR = 0x01DD,
                   REG_SCRATCH_REG0 = 0x0578, REG_COHER_STATUS_HOST = 0x0A31, REG_PA_SC_VIZ_QUERY_STATUS_0 = 0x0C44,
                   REG_PA_SC_VIZ_QUERY_STATUS_1 = 0x0C45, REG_VGT_EVENT_INITIATOR = 0x21F9, REG_SHADER_CONSTANT_FETCH_00_0 = 0x4800;
enum : uint32_t { PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_DRAW_INDX = 0x22, PM4_VIZ_QUERY = 0x23, PM4_WAIT_FOR_IDLE = 0x26,
    PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B, PM4_SET_CONSTANT = 0x2D, PM4_LOAD_ALU_CONSTANT = 0x2F,
    PM4_DRAW_INDX_2 = 0x36, PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_INVALIDATE_STATE = 0x3B, PM4_WAIT_REG_MEM = 0x3C,
    PM4_MEM_WRITE = 0x3D, PM4_REG_TO_MEM = 0x3E, PM4_INDIRECT_BUFFER = 0x3F, PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46,
    PM4_ME_INIT = 0x48, PM4_SET_BIN_MASK = 0x50, PM4_SET_BIN_SELECT = 0x51, PM4_INTERRUPT = 0x54, PM4_SET_CONSTANT2 = 0x55,
    PM4_SET_SHADER_CONSTANTS = 0x56, PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B,
    PM4_CONTEXT_UPDATE = 0x5E, PM4_SET_BIN_MASK_LO = 0x60, PM4_SET_BIN_MASK_HI = 0x61, PM4_SET_BIN_SELECT_LO = 0x62,
    PM4_SET_BIN_SELECT_HI = 0x63, PM4_XE_SWAP = 0x64 };
constexpr uint32_t kSwapSignature = 0x53574150;    // 'SWAP'

std::atomic<uint32_t> g_regs[kRegCount];
uint32_t g_ringPhys = 0, g_ringDwords = 0, g_rptrWriteback = 0, g_readIndex = 0;
std::atomic<bool> g_cpStarted{false};
std::atomic<uint32_t> g_interruptCallback{0}, g_interruptUser{0};
std::atomic<uint32_t> g_counter{0}, g_frames{0};
uint64_t g_binMask = ~0ull, g_binSelect = ~0ull;
bool g_gpuTrace = getenv("MADDEN_GPU_TRACE") != nullptr;
std::mutex g_interruptLock;   // the game's GPU ISR is not reentrant

uint32_t Reg(uint32_t i) { return i < kRegCount ? g_regs[i].load(std::memory_order_relaxed) : 0; }

// Memory as the GPU sees it: physical addresses; values get an endian swap selected by the address' low bits.
uint32_t GpuSwap(uint32_t v, uint32_t e) {
    switch (e & 3) {
    case 1: return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(v);
    case 3: return (v >> 16) | (v << 16);
    default: return v;
    }
}
// Xenia stores GpuSwap(value) in host (little-endian) order; the guest then reads it big-endian.
void GpuStore(uint32_t physAndEndian, uint32_t value) {
    uint32_t a = GuestFromPhysical(physAndEndian & ~3u);
    if (g_gpuTrace) fprintf(stderr, "[gpu]   store phys %08X = %08X\n", physAndEndian, value);
    WriteU32(a, __builtin_bswap32(GpuSwap(value, physAndEndian & 3)));
}
uint32_t GpuLoad(uint32_t physAndEndian) {
    uint32_t a = GuestFromPhysical(physAndEndian & ~3u);
    return GpuSwap(__builtin_bswap32(ReadU32(a)), physAndEndian & 3);
}

void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegCount) return;
    g_regs[index].store(value, std::memory_order_relaxed);
    if (index >= REG_SCRATCH_REG0 && index <= REG_SCRATCH_REG0 + 7) {
        uint32_t n = index - REG_SCRATCH_REG0;
        if ((1u << n) & Reg(REG_SCRATCH_UMSK)) WriteU32(GuestFromPhysical(Reg(REG_SCRATCH_ADDR) + n * 4), value);
    } else if (index == REG_COHER_STATUS_HOST) {
        g_regs[index].fetch_or(0x80000000u);
    }
}

uint32_t RegPublic(uint32_t i) { return Reg(i); }
// ---------------------------------------------------------------- interrupts (run game code on a host thread)
struct GuestCaller {
    PPCContext ctx{}; bool ready = false;
    void Call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t cpu) {
        if (!ready) { KInitHostGuestThread(ctx, 0x40000); ready = true; }
        g_base[ctx.r13.u32 + 0x10C] = (uint8_t)cpu;   // KPCR current processor number (the ISR acks 1 << cpu)
        uint32_t sp = ctx.r1.u32;
        ctx.r3.u64 = a0; ctx.r4.u64 = a1;
        (PPC_LOOKUP_FUNC(g_base, fn))(ctx, g_base);
        ctx.r1.u64 = sp;
    }
};
void DispatchInterrupt(GuestCaller& caller, uint32_t source, uint32_t cpu) {
    uint32_t cb = g_interruptCallback.load();
    if (!cb) return;
    std::lock_guard<std::mutex> l(g_interruptLock);
    caller.Call(cb, source, g_interruptUser.load(), cpu);
}

// ---------------------------------------------------------------- packet execution
struct Reader {   // walks dwords of a buffer in guest memory, wrapping at `size`
    uint32_t base, size, pos, end;   // guest address of dword 0, size/pos/end in dwords
    uint32_t avail() const { return (end + size - pos) % size + ((end == pos && full) ? size : 0); }
    bool full = false;
    uint32_t Read() { uint32_t v = ReadU32(base + pos * 4); pos = (pos + 1) % size; full = false; return v; }
    void Skip(uint32_t n) { pos = (pos + n) % size; full = false; }
};
GuestCaller g_cpCaller;
bool ExecutePacket(Reader& r);

void ExecuteIndirect(uint32_t phys, uint32_t dwords) {
    if (!dwords) return;
    Reader r{GuestFromPhysical(phys), dwords + 1, 0, dwords};   // +1 so pos==end only when finished
    while (r.pos != r.end) if (!ExecutePacket(r)) { fprintf(stderr, "[gpu] bad packet in indirect buffer %08X\n", phys); break; }
}

bool Compare(uint32_t func, uint32_t v, uint32_t ref) {
    switch (func & 7) {
    case 1: return v < ref; case 2: return v <= ref; case 3: return v == ref; case 4: return v != ref;
    case 5: return v >= ref; case 6: return v > ref; case 7: return true; default: return false;
    }
}
uint32_t ConstantBase(uint32_t type) {
    switch (type) { case 0: return 0x4000; case 1: return 0x4800; case 2: return 0x4900; case 3: return 0x4908; case 4: return 0x2000; }
    return 0xFFFFFFFF;
}


// ---------------------------------------------------------------- capture (MADDEN_GPU_CAPTURE=<dir>): shaders + draw statistics
const char* g_captureDir = getenv("MADDEN_GPU_CAPTURE");
struct ShaderBlob { uint32_t type; std::vector<uint32_t> dw; uint64_t hash; };
std::map<uint64_t, ShaderBlob> g_shaderBlobs;
uint64_t g_activeShader[2] = {0, 0};
struct DrawStats { uint64_t draws = 0, copies = 0, depthOnly = 0, inlineIdx = 0, dmaIdx = 0, autoIdx = 0;
    std::map<uint32_t, uint64_t> prims, colorFmt, depthFmt, copyFmt, surfPitch;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> pairs; std::map<uint32_t, uint64_t> texFmt; } g_stats;
uint64_t Fnv(const uint32_t* d, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; } return h; }
void CaptureShader(uint32_t type, const std::vector<uint32_t>& dw) {
    uint64_t h = Fnv(dw.data(), dw.size()) ^ type;
    g_activeShader[type & 1] = h;
    RendererSetShader(type, h, dw);
    if (!g_captureDir || g_shaderBlobs.count(h)) return;
    g_shaderBlobs[h] = {type, dw, h};
    char path[512]; snprintf(path, sizeof path, "%s/%s_%016llx.bin", g_captureDir, (type & 1) ? "ps" : "vs", (unsigned long long)h);
    if (FILE* f = fopen(path, "wb")) { fwrite(dw.data(), 4, dw.size(), f); fclose(f); }
}
void CaptureDraw(uint32_t initiator) {
    if (!g_captureDir) return;
    uint32_t mode = Reg(0x2208) & 7;
    if (mode == 6) {
        g_stats.copies++; g_stats.copyFmt[(Reg(0x231B) >> 7) & 0x3F]++;   // RB_COPY_DEST_INFO format
        return;
    }
    if (mode == 5) g_stats.depthOnly++;
    g_stats.draws++;
    g_stats.prims[initiator & 0x3F]++;
    uint32_t src = (initiator >> 6) & 3;
    if (src == 0) g_stats.dmaIdx++; else if (src == 1) g_stats.inlineIdx++; else g_stats.autoIdx++;
    g_stats.colorFmt[(Reg(0x2001) >> 16) & 0xF]++;
    g_stats.depthFmt[(Reg(0x2002) >> 16) & 1]++;
    g_stats.surfPitch[Reg(0x2000) & 0x3FFF]++;
    g_stats.pairs[{g_activeShader[0], g_activeShader[1]}]++;
    // texture fetch constants: format of every enabled texture slot (type 2 = texture)
    for (int i = 0; i < 32; i++) { uint32_t f0 = Reg(0x4800 + i * 6); if ((f0 & 3) == 2) g_stats.texFmt[Reg(0x4800 + i * 6 + 1) & 0x3F]++; }
}
void DumpStats() {
    if (!g_captureDir) return;
    char path[512]; snprintf(path, sizeof path, "%s/stats.txt", g_captureDir);
    FILE* f = fopen(path, "w"); if (!f) return;
    auto& s = g_stats;
    fprintf(f, "draws %llu copies %llu depthOnly %llu idx dma %llu inline %llu auto %llu shaders %zu pairs %zu\n",
            (unsigned long long)s.draws, (unsigned long long)s.copies, (unsigned long long)s.depthOnly, (unsigned long long)s.dmaIdx,
            (unsigned long long)s.inlineIdx, (unsigned long long)s.autoIdx, g_shaderBlobs.size(), s.pairs.size());
    auto dump = [&](const char* n, std::map<uint32_t, uint64_t>& m) { fprintf(f, "%s:", n); for (auto& [k, v] : m) fprintf(f, " %u=%llu", k, (unsigned long long)v); fprintf(f, "\n"); };
    dump("prim", s.prims); dump("colorFmt", s.colorFmt); dump("depthFmt", s.depthFmt); dump("copyFmt", s.copyFmt); dump("pitch", s.surfPitch); dump("texFmt", s.texFmt);
    for (auto& [k, v] : s.pairs) fprintf(f, "pair vs %016llx ps %016llx : %llu\n", (unsigned long long)k.first, (unsigned long long)k.second, (unsigned long long)v);
    fclose(f);
}

bool ExecuteType3(Reader& r, uint32_t packet) {
    uint32_t op = (packet >> 8) & 0x7F, count = ((packet >> 16) & 0x3FFF) + 1;
    uint32_t start = r.pos;
    if ((packet & 1) && (!(g_binSelect & g_binMask) || op == PM4_XE_SWAP)) { r.Skip(count); return true; }   // predicated
    if (g_gpuTrace) {
        fprintf(stderr, "[gpu] type3 op %02X count %u:", op, count);
        for (uint32_t i = 0; i < count && i < 8; i++) fprintf(stderr, " %08X", ReadU32(r.base + ((r.pos + i) % r.size) * 4));
        fprintf(stderr, "\n");
    }
    switch (op) {
    case PM4_IM_LOAD: {
        uint32_t at = r.Read(), ss = r.Read(); r.Skip(count - 2);
        std::vector<uint32_t> dw(ss & 0xFFFF); uint32_t a = GuestFromPhysical(at & ~3u);
        for (size_t i = 0; i < dw.size(); i++) dw[i] = ReadU32(a + i * 4);
        CaptureShader(at & 3, dw);
        break; }
    case PM4_IM_LOAD_IMMEDIATE: {
        uint32_t type = r.Read(), ss = r.Read();
        std::vector<uint32_t> dw(ss & 0xFFFF);
        for (auto& d : dw) d = r.Read();
        CaptureShader(type & 3, dw);
        break; }
    case PM4_ME_INIT: case PM4_NOP: case PM4_INVALIDATE_STATE:
    case PM4_CONTEXT_UPDATE: case PM4_WAIT_FOR_IDLE:
        r.Skip(count); break;
    case PM4_INTERRUPT: {
        uint32_t mask = r.Read();
        for (int n = 0; n < 6; n++) if (mask & (1u << n)) DispatchInterrupt(g_cpCaller, 1, n);
        break; }
    case PM4_XE_SWAP: {
        uint32_t magic = r.Read(); uint32_t fb = r.Read(), w = r.Read(), h = r.Read(); r.Skip(count - 4);
        uint32_t n = ++g_frames; ++g_counter;
        {
            bool rendered = RendererSwap(fb, w, h);
            if (!rendered) PresenterSubmitRenderedClear();
            uint32_t fetch[6]; for (int i = 0; i < 6; i++) fetch[i] = Reg(REG_SHADER_CONSTANT_FETCH_00_0 + i);
            PresenterSubmitSwap(fetch, fb, w, h);
        }
        if (magic != kSwapSignature) fprintf(stderr, "[gpu] swap packet with bad signature %08X\n", magic);
        if (n % 120 == 0) DumpStats();
        if (n == 1 || n % 300 == 0) fprintf(stderr, "[gpu] frame %u presented (front buffer phys %08X, %ux%u)\n", n, fb, w, h);
        if (const char* e = getenv("MADDEN_EXIT_AFTER_FRAMES")) if (n >= (uint32_t)atoi(e)) { fprintf(stderr, "[gpu] %u frames done, exiting\n", n); fflush(stderr); _Exit(0); }
        break; }
    case PM4_INDIRECT_BUFFER: case PM4_INDIRECT_BUFFER_PFD: {
        uint32_t ptr = r.Read(), len = r.Read() & 0xFFFFF;
        ExecuteIndirect(ptr & 0x1FFFFFFF, len);
        break; }
    case PM4_WAIT_REG_MEM: {
        uint32_t info = r.Read(), addr = r.Read(), ref = r.Read(), mask = r.Read(), wait = r.Read();
        bool mem = info & 0x10;
        for (int spins = 0;; spins++) {
            uint32_t v;
            if (mem) v = GpuLoad(addr);
            else {
                if (addr == REG_COHER_STATUS_HOST) g_regs[REG_COHER_STATUS_HOST] = 0;   // caches are always coherent here
                v = Reg(addr);
            }
            if (Compare(info, v & mask, ref)) break;
            if (spins == 20000) fprintf(stderr, "[gpu] WAIT_REG_MEM still waiting on %s %08X (value %08X, ref %08X, mask %08X)\n", mem ? "memory" : "register", addr, v, ref, mask);
            if (wait >= 0x100) std::this_thread::sleep_for(std::chrono::milliseconds(wait / 0x100)); else std::this_thread::yield();
        }
        break; }
    case PM4_REG_RMW: {
        uint32_t info = r.Read(), andm = r.Read(), orm = r.Read();
        uint32_t v = Reg(info & 0x1FFF);
        v &= (info >> 31) ? Reg(andm & 0x1FFF) : andm;
        v |= ((info >> 30) & 1) ? Reg(orm & 0x1FFF) : orm;
        WriteRegister(info & 0x1FFF, v);
        break; }
    case PM4_REG_TO_MEM: { uint32_t reg = r.Read(), mem = r.Read(); GpuStore(mem, Reg(reg)); break; }
    case PM4_MEM_WRITE: {
        uint32_t addr = r.Read();
        for (uint32_t i = 0; i + 1 < count; i++, addr += 4) GpuStore(addr, r.Read());
        break; }
    case PM4_COND_WRITE: {
        uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(), waddr = r.Read(), data = r.Read();
        uint32_t v = (info & 0x10) ? GpuLoad(poll) : Reg(poll);
        if (Compare(info, v & mask, ref)) { if (info & 0x100) GpuStore(waddr, data); else WriteRegister(waddr, data); }
        break; }
    case PM4_EVENT_WRITE: { uint32_t init = r.Read(); WriteRegister(REG_VGT_EVENT_INITIATOR, init & 0x3F); r.Skip(count - 1); break; }
    case PM4_EVENT_WRITE_SHD: {
        uint32_t init = r.Read(), addr = r.Read(), value = r.Read();
        WriteRegister(REG_VGT_EVENT_INITIATOR, init & 0x3F);
        GpuStore(addr, (init >> 31) ? g_counter.load() : value);
        break; }
    case PM4_EVENT_WRITE_EXT: {
        uint32_t init = r.Read(), addr = r.Read();
        WriteRegister(REG_VGT_EVENT_INITIATOR, init & 0x3F);
        uint16_t ext[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};   // "the whole screen was touched"
        uint32_t a = GuestFromPhysical(addr & ~3u);
        for (int i = 0; i < 6; i++) { g_base[a + i * 2] = ext[i] >> 8; g_base[a + i * 2 + 1] = ext[i] & 0xFF; }
        break; }
    case PM4_EVENT_WRITE_ZPD: {   // occlusion query: report a fixed sample count when the query ends
        uint32_t init = r.Read(); WriteRegister(REG_VGT_EVENT_INITIATOR, init & 0x3F);
        uint32_t p = Reg(0x2325);   // RB_SAMPLE_COUNT_ADDR
        if (p) {
            uint32_t a = GuestFromPhysical(p);
            // layout: ZPass_A, ZFail_A, Total_A(?), ... ZPass_B etc; D3D writes 0xFFFFFEED (BE) into the pass slots at query end
            bool end = ReadU32(a) == 0xFFFFFEED || ReadU32(a + 4) == 0xFFFFFEED;
            memset(g_base + a, 0, 0x20);
            if (end) { WriteU32(a, __builtin_bswap32(1000)); WriteU32(a + 8, __builtin_bswap32(1000)); }
        }
        break; }
    case PM4_DRAW_INDX: {
        r.Read(); uint32_t init = r.Read();
        GpuDrawCall call{init};
        if (((init >> 6) & 3) == 0 && count >= 4) { call.indexBase = r.Read(); call.indexSizeInfo = r.Read(); r.Skip(count - 4); }
        else r.Skip(count - 2);
        WriteRegister(0x21FC, init); CaptureDraw(init); RendererDraw(call); break; }
    case PM4_DRAW_INDX_2: {
        uint32_t init = r.Read(); r.Skip(count - 1);
        WriteRegister(0x21FC, init); CaptureDraw(init);
        if (((init >> 6) & 3) != 1) RendererDraw(GpuDrawCall{init});   // inline indices not supported yet
        break; }
    case PM4_SET_CONSTANT: {
        uint32_t ot = r.Read(); uint32_t b = ConstantBase((ot >> 16) & 0xFF);
        if (b == 0xFFFFFFFF) { r.Skip(count - 1); break; }
        uint32_t idx = b + (ot & 0x7FF);
        for (uint32_t n = 0; n + 1 < count; n++) WriteRegister(idx++, r.Read());
        break; }
    case PM4_SET_CONSTANT2: case PM4_SET_SHADER_CONSTANTS: {
        uint32_t idx = r.Read() & 0xFFFF;
        for (uint32_t n = 0; n + 1 < count; n++) WriteRegister(idx++, r.Read());
        break; }
    case PM4_LOAD_ALU_CONSTANT: {
        uint32_t addr = r.Read() & 0x3FFFFFFF, ot = r.Read(), sz = r.Read() & 0xFFF;
        uint32_t b = ConstantBase((ot >> 16) & 0xFF); if (b == 0xFFFFFFFF) break;
        uint32_t idx = b + (ot & 0x7FF);
        for (uint32_t n = 0; n < sz; n++) WriteRegister(idx++, ReadU32(GuestFromPhysical(addr + n * 4)));
        break; }
    case PM4_VIZ_QUERY: {
        uint32_t d = r.Read(), id = d & 0x3F;
        if (!(d & 0x100)) WriteRegister(REG_VGT_EVENT_INITIATOR, 7);
        else { WriteRegister(REG_VGT_EVENT_INITIATOR, 8);
               if (id < 32) g_regs[REG_PA_SC_VIZ_QUERY_STATUS_0] |= 1u << id; else g_regs[REG_PA_SC_VIZ_QUERY_STATUS_1] |= 1u << (id - 32); }
        break; }
    case PM4_SET_BIN_MASK_LO: g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | r.Read(); break;
    case PM4_SET_BIN_MASK_HI: g_binMask = (g_binMask & 0xFFFFFFFFull) | ((uint64_t)r.Read() << 32); break;
    case PM4_SET_BIN_SELECT_LO: g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | r.Read(); break;
    case PM4_SET_BIN_SELECT_HI: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | ((uint64_t)r.Read() << 32); break;
    case PM4_SET_BIN_MASK: { uint64_t hi = r.Read(), lo = r.Read(); g_binMask = (hi << 32) | lo; r.Skip(count - 2); break; }
    case PM4_SET_BIN_SELECT: { uint64_t hi = r.Read(), lo = r.Read(); g_binSelect = (hi << 32) | lo; r.Skip(count - 2); break; }
    default: {
        static std::atomic<int> warned{0};
        if (warned++ < 32) fprintf(stderr, "[gpu] unhandled PM4 opcode %02X (count %u)\n", op, count);
        r.Skip(count); break; }
    }
    // keep the reader in sync whatever the handler consumed
    r.pos = (start + count) % r.size;
    return true;
}

bool ExecutePacket(Reader& r) {
    uint32_t packet = r.Read();
    if (packet == 0) return true;
    switch (packet >> 30) {
    case 0: {   // write N registers
        uint32_t count = ((packet >> 16) & 0x3FFF) + 1, base = packet & 0x7FFF; bool one = (packet >> 15) & 1;
        for (uint32_t m = 0; m < count; m++) WriteRegister(one ? base : base + m, r.Read());
        return true; }
    case 1: {   // write two registers
        uint32_t a = r.Read(), b = r.Read();
        WriteRegister(packet & 0x7FF, a); WriteRegister((packet >> 11) & 0x7FF, b);
        return true; }
    case 2: return true;   // nop
    default: return ExecuteType3(r, packet);
    }
}

void PublishReadPointer() {
    WriteU32(kGpuMmio + REG_CP_RB_RPTR * 4, g_readIndex);
    g_regs[REG_CP_RB_RPTR] = g_readIndex;
    if (g_rptrWriteback) WriteU32(GuestFromPhysical(g_rptrWriteback), g_readIndex);
}

void CommandProcessorThread() {
    { PerfName("gpu commands"); }
    KInitHostGuestThread(g_cpCaller.ctx, 0x40000); g_cpCaller.ready = true;
    fprintf(stderr, "[gpu] command processor running (ring %u dwords at phys %08X)\n", g_ringDwords, g_ringPhys);
    uint32_t idle = 0;
    for (;;) {
        uint32_t wptr = __atomic_load_n((uint32_t*)(g_base + kGpuMmio + REG_CP_RB_WPTR * 4), __ATOMIC_ACQUIRE);
        wptr = __builtin_bswap32(wptr) % g_ringDwords;
        if (wptr == g_readIndex) {
            if (++idle > 2000) std::this_thread::sleep_for(std::chrono::microseconds(500)); else std::this_thread::yield();
            continue;
        }
        idle = 0;
        Reader r{GuestFromPhysical(g_ringPhys), g_ringDwords, g_readIndex, wptr};
        while (r.pos != r.end) if (!ExecutePacket(r)) { fprintf(stderr, "[gpu] bad packet in ring at %u\n", r.pos); r.pos = r.end; }
        g_readIndex = wptr;
        PublishReadPointer();
    }
}

// Vblank: the game paces itself on these (a 30 fps frame = 2 vblanks). Locked to the display's real refresh when it
// is 60 Hz or a multiple of it (DwmFlush returns at each composition = refresh), so every game frame is shown for the
// same number of refreshes; a free-running 60 Hz timer drifts against the display and every few seconds a frame was
// held one refresh too long or too short (judder). Other refresh rates (75, 144 Hz ...) keep the precise timer.
#ifdef _WIN32
static int DisplayHz() {
    DEVMODEW dm{}; dm.dmSize = sizeof dm;
    return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
}
#endif
void VblankThread() {
    { PerfName("vblank"); }
    GuestCaller caller;
    auto tick = [&] { ++g_counter; DispatchInterrupt(caller, 0, 2); };
#ifdef _WIN32
    typedef HRESULT(WINAPI * DwmFlushFn)();
    DwmFlushFn dwmFlush = nullptr;
    if (HMODULE d = LoadLibraryW(L"dwmapi.dll")) dwmFlush = (DwmFlushFn)(void*)GetProcAddress(d, "DwmFlush");
    int hz = DisplayHz();
    int per = 0;   // display refreshes per guest vblank
    if (hz >= 58 && hz <= 62) per = 1;
    else if (hz > 62) { int k = (int)std::lround(hz / 60.0); if (k >= 2 && std::abs(hz - 60 * k) <= 2 * k) per = k; }
    bool lockToDisplay = dwmFlush && per > 0 && !getenv("MADDEN_FREE_VBLANK");
    fprintf(stderr, "[gpu] display %d Hz - vblank %s\n", hz, lockToDisplay ? (per == 1 ? "locked to the display refresh" : "locked to the display (every few refreshes)") : "from a 60 Hz timer");
    int fastReturns = 0;
    while (lockToDisplay) {   // falls back to the timer if DwmFlush stops waiting (composition off) or stalls
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < per; i++) dwmFlush();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms < 4.0 * per) { if (++fastReturns > 30) { fprintf(stderr, "[gpu] vblank: display sync not waiting - using the 60 Hz timer\n"); break; } std::this_thread::sleep_for(std::chrono::microseconds(16667 - (int)(ms * 1000))); }
        else fastReturns = 0;
        if (ms > 100.0) {   // window hidden / display asleep: keep the game going on the timer for a while
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2), next = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() < until) { next += std::chrono::microseconds(16667); std::this_thread::sleep_until(next); tick(); }
            continue;
        }
        tick();
    }
#endif
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        tick();
    }
}

void InitGpuRegisters() {
    // values the game may read straight from the register aperture (Xenia's GraphicsSystem::ReadRegister)
    struct { uint32_t r, v; } init[] = { {0x0F00, 0x08100748}, {0x0F01, 0x0000200E}, {0x194C, 0x000002D0}, {0x1951, 1}, {0x1961, 0x050002D0} };
    for (auto& i : init) { WriteU32(kGpuMmio + i.r * 4, i.v); g_regs[i.r] = i.v; }
}
}   // namespace

// ---------------------------------------------------------------- kernel video exports
IMP(VdInitializeRingBuffer) {   // (physical address, log2 of size in qwords)
    TRACE(VdInitializeRingBuffer);
    g_ringPhys = ctx.r3.u32 & 0x1FFFFFFF; g_ringDwords = (1u << (ctx.r4.u32 + 3)) / 4; g_readIndex = 0;
    fprintf(stderr, "[gpu] ring buffer at phys %08X, %u bytes\n", g_ringPhys, g_ringDwords * 4);
    InitGpuRegisters();
    if (!g_cpStarted.exchange(true)) {
        std::thread(CommandProcessorThread).detach();
        std::thread(VblankThread).detach();
    }
}
IMP(VdEnableRingBufferRPtrWriteBack) {
    TRACE(VdEnableRingBufferRPtrWriteBack);
    g_rptrWriteback = ctx.r3.u32 & 0x1FFFFFFF;
    fprintf(stderr, "[gpu] read-pointer write-back at phys %08X\n", g_rptrWriteback);
}
IMP(VdSetGraphicsInterruptCallback) {
    TRACE(VdSetGraphicsInterruptCallback);
    g_interruptCallback = ctx.r3.u32; g_interruptUser = ctx.r4.u32;
    fprintf(stderr, "[gpu] interrupt callback %08X (user %08X)\n", ctx.r3.u32, ctx.r4.u32);
}
IMP(VdGetSystemCommandBuffer) {
    TRACE(VdGetSystemCommandBuffer);
    if (ctx.r3.u32) { memset(g_base + ctx.r3.u32, 0, 0x94); WriteU32(ctx.r3.u32, 0xBEEF0000); }
    if (ctx.r4.u32) WriteU32(ctx.r4.u32, 0xBEEF0001);
}
// VdSwap(buffer_ptr, fetch_ptr, unk, unk, unk, frontbuffer_ptr*, format*, color_space*, width*, height*)
// The caller reserves 64 dwords in the ring; like Xenia we fill them with a fetch-constant write, a SWAP packet
// and NOPs, so the command processor sees the swap in order with the frame's commands.
IMP(VdSwap) {
    TRACE(VdSwap);
    uint32_t buf = ctx.r3.u32, fetch = ctx.r4.u32, pFront = ctx.r8.u32;
    uint32_t pW = GuestArgStack(ctx, 8), pH = GuestArgStack(ctx, 9);
    uint32_t f[6]; for (int i = 0; i < 6; i++) f[i] = ReadU32(fetch + i * 4);
    uint32_t frontVirt = f[1] & 0xFFFFF000;
    uint32_t frontPhys = GuestPhysicalAddress(frontVirt);
    f[1] = (f[1] & 0xFFF) | (frontPhys & 0xFFFFF000);
    uint32_t w = pW ? ReadU32(pW) : 1280, h = pH ? ReadU32(pH) : 720;
    (void)pFront;
    uint32_t o = 0;
    auto put = [&](uint32_t v) { WriteU32(buf + o * 4, v); o++; };
    put((REG_SHADER_CONSTANT_FETCH_00_0 & 0x7FFF) | (5u << 16));   // type 0, 6 registers
    for (int i = 0; i < 6; i++) put(f[i]);
    put((3u << 30) | (3u << 16) | (PM4_XE_SWAP << 8));             // type 3, 4 dwords
    put(kSwapSignature); put(frontPhys); put(w); put(h);
    while (o < 64) put(0x80000000);                                // type 2 nops
    ctx.r3.u64 = 0;
}

uint32_t GpuRegister(uint32_t index) { return RegPublic(index); }
uint32_t GpuSwapValue(uint32_t v, uint32_t endian) { return GpuSwap(v, endian); }
