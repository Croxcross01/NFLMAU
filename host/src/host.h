#pragma once
#include "ppc_config.h"
#include "ppc_context.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

extern uint8_t* g_base;   // host pointer to guest address 0

inline uint32_t ReadU32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline void WriteU32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline uint64_t ReadU64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void WriteU64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }
inline const char* GuestStr(uint32_t a) { return reinterpret_cast<const char*>(g_base + a); }

uint32_t GuestAlloc(size_t size, size_t align = 16);       // virtual-memory style heap (zeroed)
uint32_t GuestAllocPhys(size_t size, size_t align = 4096); // "physical" heap (zeroed), returns a 0xC0000000-window address
uint32_t GuestPhysicalAddress(uint32_t guestVirtual);       // virtual (any physical window) -> physical address
inline uint32_t GuestFromPhysical(uint32_t phys) { return 0xA0000000u + (phys & 0x1FFFFFFF); }
// Prepare the calling host thread to run guest code (own stack, PCR, KTHREAD). Used by GPU/vblank threads.
void KInitHostGuestThread(PPCContext& ctx, uint32_t stackSize);
void HostUnimplemented(const char* name, PPCContext& ctx);
void HostTrace(const char* name, PPCContext& ctx);
extern bool g_trace;
