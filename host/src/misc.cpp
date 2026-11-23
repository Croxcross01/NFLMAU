// Miscellaneous kernel/XAM imports: guest printf-family, Rtl string/time helpers, loader, system info, network.
#include "kernel.h"
#include "platform.h"
#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

// ---------------------------------------------------------------- guest printf
// Variadic args: GPR r3..r10 then stack at r1+0x54 (8-byte slots); doubles travel as raw 64-bit GPR values.
struct ArgSrc {
    PPCContext* ctx = nullptr; int next = 0; uint32_t va = 0;   // va != 0 -> read 8-byte slots from va_list
    uint64_t get() {
        if (va) { uint64_t v = ReadU64(va); va += 8; return v; }
        int i = next++;
        if (i < 8) return (&ctx->r3)[i].u64;
        return ReadU32(ctx->r1.u32 + 0x54 + (i - 8) * 8);   // stack spill area (32-bit values)
    }
};
static std::string GuestFormat(const char* fmt, ArgSrc& a) {
    std::string out;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { out += *p; continue; }
        std::string spec = "%"; ++p;
        if (*p == '%') { out += '%'; continue; }
        while (*p && strchr("-+ #0", *p)) spec += *p++;
        if (*p == '*') { spec += std::to_string((int32_t)a.get()); ++p; } else while (isdigit((unsigned char)*p)) spec += *p++;
        if (*p == '.') { spec += *p++; if (*p == '*') { spec += std::to_string((int32_t)a.get()); ++p; } else while (isdigit((unsigned char)*p)) spec += *p++; }
        int lng = 0; bool wide = false;
        for (;;) {
            if (*p == 'h') { ++p; } else if (*p == 'l') { ++p; lng++; wide = true; } else if (*p == 'L' || *p == 'q' || *p == 'j') { ++p; lng = 2; }
            else if (*p == 'z' || *p == 't') { ++p; }
            else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { p += 3; lng = 2; } else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
            else if (*p == 'w') { ++p; wide = true; }
            else break;
        }
        char conv = *p; if (!conv) break;
        char buf[512];
        switch (conv) {
        case 'd': case 'i': {
            uint64_t v = a.get();
            if (lng >= 2) snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)v); else snprintf(buf, sizeof buf, (spec + "d").c_str(), (int32_t)v);
            out += buf; break; }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = a.get(); std::string s2 = spec + (lng >= 2 ? "ll" : "") + conv;
            if (lng >= 2) snprintf(buf, sizeof buf, s2.c_str(), (unsigned long long)v); else snprintf(buf, sizeof buf, s2.c_str(), (uint32_t)v);
            out += buf; break; }
        case 'p': snprintf(buf, sizeof buf, "%08X", (uint32_t)a.get()); out += buf; break;
        case 'c': case 'C': out += (char)a.get(); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t bits = a.get(); double d; memcpy(&d, &bits, 8);
            snprintf(buf, sizeof buf, (spec + conv).c_str(), d); out += buf; break; }
        case 's': case 'S': {
            uint32_t ptr = (uint32_t)a.get(); std::string s;
            bool w = (conv == 'S') ? !wide : wide;
            if (!ptr) s = "(null)";
            else if (w) { for (uint32_t q = ptr; ; q += 2) { uint16_t ch = (uint16_t)((g_base[q] << 8) | g_base[q + 1]); if (!ch) break; s += ch < 0x80 ? (char)ch : '?'; } }
            else s = GuestStr(ptr);
            std::string fmt2 = spec + "s";
            std::string tmp(s.size() + 256, '\0'); int n = snprintf(tmp.data(), tmp.size(), fmt2.c_str(), s.c_str());
            out.append(tmp.data(), n > 0 ? std::min<size_t>(n, tmp.size() - 1) : 0); break; }
        case 'n': { uint32_t ptr = (uint32_t)a.get(); if (ptr) WriteU32(ptr, (uint32_t)out.size()); break; }
        default: out += spec; out += conv; break;
        }
    }
    return out;
}
static uint32_t PutResult(uint32_t buf, const std::string& s, int64_t cap) {
    if (cap < 0) { memcpy(g_base + buf, s.c_str(), s.size() + 1); return (uint32_t)s.size(); }
    if ((int64_t)s.size() < cap) { memcpy(g_base + buf, s.c_str(), s.size() + 1); return (uint32_t)s.size(); }
    memcpy(g_base + buf, s.data(), (size_t)cap); return 0xFFFFFFFF;   // MSVC _snprintf: -1 when truncated
}
IMP(sprintf)    { ArgSrc a{&ctx, 2}; std::string s = GuestFormat(GuestStr(ctx.r4.u32), a); ctx.r3.u64 = PutResult(ctx.r3.u32, s, -1); }
IMP(_snprintf)  { ArgSrc a{&ctx, 3}; std::string s = GuestFormat(GuestStr(ctx.r5.u32), a); ctx.r3.u64 = (int64_t)(int32_t)PutResult(ctx.r3.u32, s, ctx.r4.u32); }
IMP(_vsnprintf) { ArgSrc a{&ctx, 0, ctx.r6.u32}; std::string s = GuestFormat(GuestStr(ctx.r5.u32), a); ctx.r3.u64 = (int64_t)(int32_t)PutResult(ctx.r3.u32, s, ctx.r4.u32); }
IMP(DbgPrint)   { ArgSrc a{&ctx, 1}; std::string s = GuestFormat(GuestStr(ctx.r3.u32), a); fprintf(stderr, "[game] %s%s", s.c_str(), (!s.empty() && s.back() == '\n') ? "" : "\n"); ctx.r3.u64 = 0; }
IMP(DbgBreakPoint) { fprintf(stderr, "[game] DbgBreakPoint (ignored)\n"); }

// ---------------------------------------------------------------- Rtl strings / memory
IMP(RtlInitAnsiString) {
    TRACE(RtlInitAnsiString);
    uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
    uint16_t len = s ? (uint16_t)strlen(GuestStr(s)) : 0;
    g_base[d] = len >> 8; g_base[d + 1] = len & 0xFF;
    uint16_t mx = s ? len + 1 : 0; g_base[d + 2] = mx >> 8; g_base[d + 3] = mx & 0xFF;
    WriteU32(d + 4, s);
}
IMP(RtlCompareMemoryUlong) {   // (Source, Length, Pattern) -> bytes matching
    TRACE(RtlCompareMemoryUlong);
    uint32_t n = 0; for (; n + 4 <= ctx.r4.u32; n += 4) if (ReadU32(ctx.r3.u32 + n) != ctx.r5.u32) break;
    ctx.r3.u64 = n;
}
IMP(RtlFillMemoryUlong) { TRACE(RtlFillMemoryUlong); for (uint32_t n = 0; n + 4 <= ctx.r4.u32; n += 4) WriteU32(ctx.r3.u32 + n, ctx.r5.u32); }
IMP(RtlCompareStringN) {   // (Str1, Len1, Str2, Len2, CaseInsensitive)
    TRACE(RtlCompareStringN);
    const char* a = GuestStr(ctx.r3.u32); const char* b = GuestStr(ctx.r5.u32);
    uint32_t n = std::min(ctx.r4.u32, ctx.r6.u32); int r = 0;
    for (uint32_t i = 0; i < n && !r; i++) { int x = a[i], y = b[i]; if (ctx.r7.u32) { x = toupper(x); y = toupper(y); } r = x - y; }
    if (!r) r = (int)ctx.r4.u32 - (int)ctx.r6.u32;
    ctx.r3.u64 = (int64_t)r;
}
IMP(RtlComputeCrc32) {   // (InitialCrc, Buffer, Length)
    TRACE(RtlComputeCrc32);
    uint32_t c = ~ctx.r3.u32;
    for (uint32_t i = 0; i < ctx.r5.u32; i++) { c ^= g_base[ctx.r4.u32 + i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    ctx.r3.u64 = ~c;
}
// RtlMultiByteToUnicodeN(UnicodeString, MaxBytesInUnicodeString, BytesInUnicodeString*, MultiByteString, BytesInMultiByteString)
IMP(RtlMultiByteToUnicodeN) {
    TRACE(RtlMultiByteToUnicodeN);
    uint32_t n = std::min(ctx.r4.u32 / 2, ctx.r7.u32);
    for (uint32_t i = 0; i < n; i++) { g_base[ctx.r3.u32 + 2 * i] = 0; g_base[ctx.r3.u32 + 2 * i + 1] = g_base[ctx.r6.u32 + i]; }
    if (ctx.r5.u32) WriteU32(ctx.r5.u32, n * 2);
    ctx.r3.u64 = 0;
}
// RtlUnicodeToMultiByteN(MultiByteString, MaxBytes, BytesInMultiByte*, UnicodeString, BytesInUnicodeString)
IMP(RtlUnicodeToMultiByteN) {
    TRACE(RtlUnicodeToMultiByteN);
    uint32_t n = std::min(ctx.r4.u32, ctx.r7.u32 / 2);
    for (uint32_t i = 0; i < n; i++) { uint16_t ch = (uint16_t)((g_base[ctx.r6.u32 + 2 * i] << 8) | g_base[ctx.r6.u32 + 2 * i + 1]); g_base[ctx.r3.u32 + i] = ch < 0x100 ? (uint8_t)ch : '?'; }
    if (ctx.r5.u32) WriteU32(ctx.r5.u32, n);
    ctx.r3.u64 = 0;
}
IMP(RtlUpcaseUnicodeChar) { TRACE(RtlUpcaseUnicodeChar); uint16_t c = ctx.r3.u16; ctx.r3.u64 = (c < 0x80) ? (uint16_t)toupper(c) : c; }
IMP(RtlNtStatusToDosError) {
    TRACE(RtlNtStatusToDosError);
    uint32_t s = ctx.r3.u32, e;
    switch (s) {
    case 0: e = 0; break; case STATUS_OBJECT_NAME_NOT_FOUND: case STATUS_NO_SUCH_FILE: e = 2; break;
    case STATUS_OBJECT_PATH_NOT_FOUND: e = 3; break; case STATUS_ACCESS_DENIED: e = 5; break; case STATUS_INVALID_HANDLE: e = 6; break;
    case STATUS_END_OF_FILE: e = 38; break; case STATUS_NO_MORE_FILES: e = 18; break; case STATUS_OBJECT_NAME_COLLISION: e = 183; break;
    case STATUS_TIMEOUT: e = 1460; break; case STATUS_PENDING: e = 997; break; case STATUS_NOT_IMPLEMENTED: e = 1; break;
    default: e = (s & 0xFFFF0000) == 0x80070000 ? (s & 0xFFFF) : 317;
    }
    ctx.r3.u64 = e;
}

// ---------------------------------------------------------------- time
// TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (u16 each)
static void W16(uint32_t a, uint16_t v) { g_base[a] = v >> 8; g_base[a + 1] = v & 0xFF; }
static uint16_t R16(uint32_t a) { return (uint16_t)((g_base[a] << 8) | g_base[a + 1]); }
IMP(RtlTimeToTimeFields) {
    TRACE(RtlTimeToTimeFields);
    uint64_t t = ReadU64(ctx.r3.u32);
    time_t secs = (time_t)((t - 116444736000000000ull) / 10000000ull);
    struct tm tm; PlatGmTime(secs, &tm);
    uint32_t f = ctx.r4.u32;
    W16(f, tm.tm_year + 1900); W16(f + 2, tm.tm_mon + 1); W16(f + 4, tm.tm_mday); W16(f + 6, tm.tm_hour);
    W16(f + 8, tm.tm_min); W16(f + 10, tm.tm_sec); W16(f + 12, (uint16_t)((t / 10000) % 1000)); W16(f + 14, tm.tm_wday);
}
IMP(RtlTimeFieldsToTime) {
    TRACE(RtlTimeFieldsToTime);
    uint32_t f = ctx.r3.u32; struct tm tm{};
    tm.tm_year = R16(f) - 1900; tm.tm_mon = R16(f + 2) - 1; tm.tm_mday = R16(f + 4); tm.tm_hour = R16(f + 6); tm.tm_min = R16(f + 8); tm.tm_sec = R16(f + 10);
    time_t s = PlatTimeGm(&tm);
    WriteU64(ctx.r4.u32, (uint64_t)s * 10000000ull + R16(f + 12) * 10000ull + 116444736000000000ull);
    ctx.r3.u64 = 1;
}

// ---------------------------------------------------------------- loader / system
constexpr uint32_t X_ERROR_NOT_FOUND = 0x490;
IMP(XamLoaderGetLaunchDataSize) { TRACE(XamLoaderGetLaunchDataSize); if (ctx.r3.u32) WriteU32(ctx.r3.u32, 0); ctx.r3.u64 = X_ERROR_NOT_FOUND; }
IMP(XamLoaderGetLaunchData) { TRACE(XamLoaderGetLaunchData); ctx.r3.u64 = X_ERROR_NOT_FOUND; }
IMP(XamLoaderSetLaunchData) { TRACE(XamLoaderSetLaunchData); ctx.r3.u64 = 0; }
IMP(XamLoaderLaunchTitle) { fprintf(stderr, "[host] game asked to launch another title (%s) - exiting\n", ctx.r3.u32 ? GuestStr(ctx.r3.u32) : "dashboard"); exit(0); }
IMP(XamGetSystemVersion) { TRACE(XamGetSystemVersion); ctx.r3.u64 = 0x20446700; }   // 2.0.17511.0
IMP(XamGetExecutionId) { TRACE(XamGetExecutionId); uint32_t p = GuestAlloc(0x18); WriteU32(p + 0xC, 0x584109C5); if (ctx.r3.u32) WriteU32(ctx.r3.u32, p); ctx.r3.u64 = 0; }
IMP(XGetGameRegion) { TRACE(XGetGameRegion); ctx.r3.u64 = 0xFF; }
IMP(XexCheckExecutablePrivilege) { TRACE(XexCheckExecutablePrivilege); ctx.r3.u64 = 0; }
// ExGetXConfigSetting(Category, Setting, Buffer, BufferSize, RequiredSize*)
IMP(ExGetXConfigSetting) {
    TRACE(ExGetXConfigSetting);
    uint16_t cat = ctx.r3.u16, set = ctx.r4.u16; uint32_t buf = ctx.r5.u32, sz = ctx.r6.u32, req = ctx.r7.u32;
    uint32_t val = 0, n = 4;
    if (cat == 3) {   // XCONFIG_USER_CATEGORY
        switch (set) { case 9: val = 1; break;            // language: English
                       case 10: val = 0x00400000; break;  // video flags: widescreen
                       case 14: val = 0x00000001; break;  // audio flags
                       case 15: val = 0; break;           // retail flags
                       default: val = 0; }
    } else if (cat == 2) { val = 0x103; n = 2; }          // console/region info
    if (buf && sz) { memset(g_base + buf, 0, sz); if (n == 4 && sz >= 4) WriteU32(buf, val); else if (sz >= 2) W16(buf, (uint16_t)val); }
    if (req) W16(req, (uint16_t)n);
    if (g_trace) fprintf(stderr, "[host] XConfig cat=%u setting=%u -> %X\n", cat, set, val);
    ctx.r3.u64 = 0;
}
// XexGetModuleHandle(ModuleName, pHandle) / XexGetProcedureAddress(Handle, Ordinal, pAddress) / XexLoadImage(Name, Flags, MinVersion, pHandle)
IMP(XexGetModuleHandle) {
    TRACE(XexGetModuleHandle);
    const char* n = ctx.r3.u32 ? GuestStr(ctx.r3.u32) : "(self)";
    fprintf(stderr, "[host] XexGetModuleHandle(%s)\n", n);
    if (ctx.r4.u32) WriteU32(ctx.r4.u32, 0x88000000);
    ctx.r3.u64 = 0;
}
IMP(XexLoadImage) {
    TRACE(XexLoadImage);
    fprintf(stderr, "[host] XexLoadImage(%s, flags=%X) - not supported, failing\n", GuestStr(ctx.r3.u32), ctx.r4.u32);
    ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
}
IMP(XexUnloadImage) { TRACE(XexUnloadImage); ctx.r3.u64 = 0; }
IMP(XexGetProcedureAddress) {
    TRACE(XexGetProcedureAddress);
    fprintf(stderr, "[host] XexGetProcedureAddress(module=%08X, ordinal=%u) - returning not found\n", ctx.r3.u32, ctx.r4.u32);
    if (ctx.r5.u32) WriteU32(ctx.r5.u32, 0);
    ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
}
IMP(ExRegisterTitleTerminateNotification) { TRACE(ExRegisterTitleTerminateNotification); }
IMP(KeBugCheck) { fprintf(stderr, "[host] KeBugCheck(%08X)\n", ctx.r3.u32); abort(); }
IMP(KeBugCheckEx) { fprintf(stderr, "[host] KeBugCheckEx(%08X, %08X, %08X, %08X, %08X)\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); abort(); }
IMP(RtlRaiseException) { fprintf(stderr, "[host] RtlRaiseException(record=%08X code=%08X)\n", ctx.r3.u32, ctx.r3.u32 ? ReadU32(ctx.r3.u32) : 0); abort(); }

// ---------------------------------------------------------------- network: offline for now (startup succeeds, nothing connects)
IMP(NetDll_WSAStartup)  { TRACE(NetDll_WSAStartup); if (ctx.r5.u32) { memset(g_base + ctx.r5.u32, 0, 0x190); W16(ctx.r5.u32, 0x0202); W16(ctx.r5.u32 + 2, 0x0202); } ctx.r3.u64 = 0; }
IMP(NetDll_WSACleanup)  { TRACE(NetDll_WSACleanup); ctx.r3.u64 = 0; }
IMP(NetDll_XNetStartup) { TRACE(NetDll_XNetStartup); ctx.r3.u64 = 0; }
IMP(NetDll_XNetCleanup) { TRACE(NetDll_XNetCleanup); ctx.r3.u64 = 0; }
IMP(NetDll_XNetRandom) { TRACE(NetDll_XNetRandom); for (uint32_t i = 0; i < ctx.r5.u32; i++) g_base[ctx.r4.u32 + i] = (uint8_t)rand(); ctx.r3.u64 = 0; }

// ---- WSA events (the network threads block on these; nothing signals them while offline)
uint32_t KWaitHandlesMs(const std::vector<uint32_t>& handles, bool waitAll, uint32_t ms);
uint32_t KCreateEventHandle(bool manualReset, bool initial);
bool KSetEventHandle(uint32_t h, bool state);
IMP(NetDll_WSACreateEvent) { TRACE(NetDll_WSACreateEvent); ctx.r3.u64 = KCreateEventHandle(true, false); }
IMP(NetDll_WSACloseEvent) { TRACE(NetDll_WSACloseEvent); ctx.r3.u64 = KCloseHandle(ctx.r3.u32) ? 1 : 0; }
IMP(NetDll_WSASetEvent)   { TRACE(NetDll_WSASetEvent);   ctx.r3.u64 = KSetEventHandle(ctx.r3.u32, true) ? 1 : 0; }
IMP(NetDll_WSAResetEvent) { TRACE(NetDll_WSAResetEvent); ctx.r3.u64 = KSetEventHandle(ctx.r3.u32, false) ? 1 : 0; }
// WSAWaitForMultipleEvents(count, events*, waitAll, timeoutMs, alertable) -> index or WSA_WAIT_TIMEOUT
IMP(NetDll_WSAWaitForMultipleEvents) {
    TRACE(NetDll_WSAWaitForMultipleEvents);
    std::vector<uint32_t> hs; for (uint32_t i = 0; i < ctx.r3.u32 && i < 64; i++) hs.push_back(ReadU32(ctx.r4.u32 + 4 * i));
    uint32_t r = KWaitHandlesMs(hs, ctx.r5.u32 != 0, ctx.r6.u32);
    ctx.r3.u64 = r == STATUS_TIMEOUT ? 0x102 : r;
}
