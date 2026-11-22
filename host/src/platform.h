// Small portability layer so the host builds on Windows (MSVC / clang-cl) and Linux.
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <chrono>
// Current time as a Windows FILETIME (100 ns ticks since 1601).
inline uint64_t PlatFileTimeNow() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return (uint64_t)(ns / 100) + 116444736000000000ull;
}

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
// Reserve the 4 GB guest address space and commit 64 KB chunks on first touch (keeps the commit charge small).
inline uint8_t* g_platGuestBase = nullptr; inline size_t g_platGuestSize = 0;
inline LONG CALLBACK PlatCommitOnTouch(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
    if (a < g_platGuestBase || a >= g_platGuestBase + g_platGuestSize) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* page = g_platGuestBase + ((size_t)(a - g_platGuestBase) & ~(size_t)0xFFFF);
    return VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
// Guest layout: [0, 0xA0000000) private reserve; 0xA0000000, 0xC0000000 and 0xE0000000 are three views of one
// 512 MB "physical memory" section, so all three Xbox physical windows alias like on the console.
constexpr uint64_t kPhysWindowSize = 0x20000000;
inline void* PlatReserveGuestMemory(size_t size) {
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, (DWORD)kPhysWindowSize, nullptr);
    if (!sec) return nullptr;
    for (int attempt = 0; attempt < 16; attempt++) {
        uint8_t* b = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);   // find a free 4 GB hole
        if (!b) return nullptr;
        VirtualFree(b, 0, MEM_RELEASE);
        void* low = VirtualAlloc(b, 0xA0000000, MEM_RESERVE, PAGE_NOACCESS);
        void* v1 = low ? MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, kPhysWindowSize, b + 0xA0000000) : nullptr;
        void* v2 = v1 ? MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, kPhysWindowSize, b + 0xC0000000) : nullptr;
        void* v3 = v2 ? MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, kPhysWindowSize, b + 0xE0000000) : nullptr;
        if (v3) {
            g_platGuestBase = b; g_platGuestSize = size;
            AddVectoredExceptionHandler(1, PlatCommitOnTouch);
            return b;
        }
        if (v2) UnmapViewOfFile(v2); if (v1) UnmapViewOfFile(v1); if (low) VirtualFree(low, 0, MEM_RELEASE);   // raced: retry
    }
    return nullptr;
}
// Kernel-mode writes (ReadFile) don't trigger the commit-on-touch handler, so commit the range up front.
inline void PlatEnsureCommitted(void* p, size_t len) {
    if (!len || (uint8_t*)p < g_platGuestBase || (uint8_t*)p >= g_platGuestBase + g_platGuestSize) return;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)0xFFFF, e = ((uintptr_t)p + len + 0xFFFF) & ~(uintptr_t)0xFFFF;
    VirtualAlloc((void*)a, e - a, MEM_COMMIT, PAGE_READWRITE);
}
inline int PlatFseek(FILE* f, uint64_t off) { return _fseeki64(f, (long long)off, SEEK_SET); }
inline int PlatTruncate(FILE* f, uint64_t size) { fflush(f); return _chsize_s(_fileno(f), (long long)size); }
inline void PlatGmTime(time_t t, struct tm* out) { gmtime_s(out, &t); }
inline time_t PlatTimeGm(struct tm* t) { return _mkgmtime(t); }
#else
#  include <sys/mman.h>
#  include <unistd.h>
#  include <sys/syscall.h>
constexpr uint64_t kPhysWindowSize = 0x20000000;
inline void* PlatReserveGuestMemory(size_t size) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    // one 512 MB "physical memory" object mapped at all three physical windows
    int fd = (int)syscall(SYS_memfd_create, "xenon_phys", 0);
    if (fd < 0 || ftruncate(fd, (off_t)kPhysWindowSize) != 0) return nullptr;
    for (uint64_t w : {0xA0000000ull, 0xC0000000ull, 0xE0000000ull})
        if (mmap((uint8_t*)p + w, kPhysWindowSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) == MAP_FAILED) return nullptr;
    close(fd);
    return p;
}
inline void PlatEnsureCommitted(void*, size_t) {}
inline int PlatFseek(FILE* f, uint64_t off) { return fseeko(f, (off_t)off, SEEK_SET); }
inline int PlatTruncate(FILE* f, uint64_t size) { fflush(f); return ftruncate(fileno(f), (off_t)size); }
inline void PlatGmTime(time_t t, struct tm* out) { gmtime_r(&t, out); }
inline time_t PlatTimeGm(struct tm* t) { return timegm(t); }
#endif
