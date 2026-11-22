// Implementations of the Xbox 360 kernel/XAM imports the game uses. Anything not implemented here
// falls back to a logging stub (stubs_gen.cpp) that returns 0.
#include "host.h"
#include <map>
#include <mutex>
#include "platform.h"
#include <unordered_map>
#include <ctime>
#include <cstdlib>
#include <set>
#include <string>

static std::set<std::string> g_seen;

void HostTrace(const char* name, PPCContext& ctx) {
    if (g_trace) fprintf(stderr, "[import] %s(r3=%08X r4=%08X r5=%08X r6=%08X) lr=%08X\n", name + 6, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
}
void HostUnimplemented(const char* name, PPCContext& ctx) {
    if (g_seen.insert(name).second)
        fprintf(stderr, "[UNIMPLEMENTED import] %s(r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X) -> returning 0\n", name + 6, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    if (getenv("MADDEN_STRICT")) { fprintf(stderr, "strict mode: aborting\n"); abort(); }
    ctx.r3.u64 = 0;
}
#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

// ---- memory ----
IMP(XamAlloc)  { TRACE(XamAlloc); uint32_t p = GuestAlloc(ctx.r4.u32); if (ctx.r5.u32) WriteU32(ctx.r5.u32, p); ctx.r3.u64 = 0; }
IMP(XamFree)   { TRACE(XamFree); ctx.r3.u64 = 0; }
IMP(ExAllocatePool)            { TRACE(ExAllocatePool); ctx.r3.u32 = GuestAlloc(ctx.r3.u32); }
IMP(ExAllocatePoolWithTag)     { TRACE(ExAllocatePoolWithTag); ctx.r3.u32 = GuestAlloc(ctx.r3.u32); }
IMP(ExAllocatePoolTypeWithTag) { TRACE(ExAllocatePoolTypeWithTag); ctx.r3.u32 = GuestAlloc(ctx.r3.u32); }
IMP(ExFreePool) { TRACE(ExFreePool); ctx.r3.u64 = 0; }
IMP(NtAllocateVirtualMemory) {
    TRACE(NtAllocateVirtualMemory);
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32;
    uint32_t size = ReadU32(pSize), want = ReadU32(pBase);
    uint32_t addr = want ? want : GuestAlloc((size + 0xFFFF) & ~0xFFFFu, 0x10000);
    WriteU32(pBase, addr); WriteU32(pSize, (size + 0xFFF) & ~0xFFFu);
    ctx.r3.u64 = 0;
}
IMP(NtFreeVirtualMemory) { TRACE(NtFreeVirtualMemory); ctx.r3.u64 = 0; }
// MmAllocatePhysicalMemoryEx(Flags, Size, Protect, MinAddress, MaxAddress, Alignment)
static std::mutex g_protLock;
static std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_physAllocs;   // address -> (size, protect)
IMP(MmAllocatePhysicalMemoryEx) {
    TRACE(MmAllocatePhysicalMemoryEx);
    uint32_t size = ctx.r4.u32, prot = ctx.r5.u32, al = ctx.r8.u32 ? ctx.r8.u32 : 4096;
    uint32_t a = GuestAllocPhys(size, al);
    { std::lock_guard<std::mutex> l(g_protLock); g_physAllocs[a] = {size, prot}; }
    ctx.r3.u32 = a;
}
IMP(MmFreePhysicalMemory) { TRACE(MmFreePhysicalMemory); ctx.r3.u64 = 0; }
IMP(MmGetPhysicalAddress) { TRACE(MmGetPhysicalAddress); ctx.r3.u32 = GuestPhysicalAddress(ctx.r3.u32); }
static bool FindPhysAlloc(uint32_t a, uint32_t& base, uint32_t& size, uint32_t& prot) {
    std::lock_guard<std::mutex> l(g_protLock);
    uint32_t pa = GuestPhysicalAddress(a);
    auto it = g_physAllocs.upper_bound(0xC0000000u + pa);
    if (it == g_physAllocs.begin()) return false;
    --it;
    if (0xC0000000u + pa >= it->first + it->second.first) return false;
    base = it->first; size = it->second.first; prot = it->second.second; return true;
}
IMP(MmQueryAddressProtect) {
    TRACE(MmQueryAddressProtect);
    uint32_t b, sz, prot;
    ctx.r3.u64 = (ctx.r3.u32 >= 0xA0000000u && FindPhysAlloc(ctx.r3.u32, b, sz, prot)) ? (prot & 0xFFFF) : 0x4;   // PAGE_READWRITE
}

// ---- time ----
IMP(KeQuerySystemTime) {
    TRACE(KeQuerySystemTime);
    uint64_t ft = PlatFileTimeNow();
    WriteU64(ctx.r3.u32, ft);
}
