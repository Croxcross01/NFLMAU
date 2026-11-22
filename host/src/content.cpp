// XAM content: save-game packages, the storage device, license mask and enumeration. A package is a folder
// save/content/<type>/<file name>/ with the original XCONTENT_DATA kept in .xcontent, mounted as the drive name
// the game asks for (e.g. "save:").
#include "kernel.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdio>

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

void FsMount(const std::string& drive, const std::string& hostPath);
void FsUnmount(const std::string& drive);
std::string FsSaveDir();
bool FsExists(const std::string& p);
void FsRemoveAll(const std::string& p);
std::vector<std::string> FsList(const std::string& p);
void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length, uint32_t extError);

namespace {
constexpr uint32_t X_ERROR_SUCCESS = 0, X_ERROR_IO_PENDING = 0x3E5, X_ERROR_NO_MORE_FILES = 0x12, X_ERROR_PATH_NOT_FOUND = 3,
                   X_ERROR_ALREADY_EXISTS = 0xB7, X_ERROR_DEVICE_NOT_CONNECTED = 0x48F, X_ERROR_FUNCTION_FAILED = 0x65B;
constexpr uint32_t kDeviceHdd = 1, kXContentDataSize = 0x134;
constexpr uint64_t kHddSize = 0x10000000000ull;   // pretend 1 TB, plenty free

uint32_t Finish(uint32_t ov, uint32_t result, uint32_t length = 0) {
    if (!ov) return result;
    CompleteOverlapped(ov, result, length, result ? (0x80070000u | result) : 0);
    return X_ERROR_IO_PENDING;
}
std::string FileNameOf(uint32_t cd) {   // XCONTENT_DATA.file_name (char[42] at +0x108)
    std::string n(GuestStr(cd + 0x108)); if (n.size() > 42) n.resize(42);
    for (auto& c : n) if (c == '/' || c == '\\' || c == ':') c = '_';
    return n;
}
std::string PackageDir(uint32_t cd) {
    char t[16]; snprintf(t, sizeof t, "%08X", ReadU32(cd + 4));
    return FsSaveDir() + "/content/" + t + "/" + FileNameOf(cd);
}
void SaveHeader(uint32_t cd, const std::string& dir) {
    if (FILE* f = fopen((dir + "/.xcontent").c_str(), "wb")) { fwrite(g_base + cd, 1, kXContentDataSize, f); fclose(f); }
}

// enumerators: a list of fixed-size records handed out in order
struct Enumerator { std::vector<std::vector<uint8_t>> items; size_t pos = 0; uint32_t itemSize = 0; };
std::mutex g_enumLock;
std::map<uint32_t, Enumerator> g_enums;
uint32_t NewEnumerator(Enumerator e) {
    uint32_t o = KAllocDispatcher(0x13, 0, 0x10);
    auto* k = new KObject{KObject::Dispatcher}; k->guestPtr = o;
    uint32_t h = KNewHandle(k);
    std::lock_guard<std::mutex> l(g_enumLock); g_enums[h] = std::move(e);
    return h;
}
}   // namespace

// XamContentCreateEx(user, rootName, contentData, flags, disposition*, licenseMask*, cacheSize, contentSize, overlapped)
IMP(XamContentCreateEx) {
    TRACE(XamContentCreateEx);
    std::string root = GuestStr(ctx.r4.u32);
    uint32_t cd = ctx.r5.u32, flags = ctx.r6.u32, pDisp = ctx.r7.u32, pLic = ctx.r8.u32, ov = GuestArgStack(ctx, 8);
    std::string dir = PackageDir(cd);
    bool exists = FsExists(dir);
    uint32_t result = X_ERROR_SUCCESS, disp = 0;
    switch (flags & 0xF) {
    case 1: if (exists) result = X_ERROR_ALREADY_EXISTS; else disp = 1; break;          // CREATE_NEW
    case 2: if (exists) FsRemoveAll(dir); disp = 1; break;                               // CREATE_ALWAYS
    case 3: if (!exists) result = X_ERROR_PATH_NOT_FOUND; else disp = 2; break;          // OPEN_EXISTING
    case 4: disp = exists ? 2 : 1; break;                                                // OPEN_ALWAYS
    case 5: if (!exists) result = X_ERROR_PATH_NOT_FOUND; else { FsRemoveAll(dir); disp = 1; } break;   // TRUNCATE_EXISTING
    default: result = 0x57;
    }
    if (result == X_ERROR_SUCCESS) {
        FsMount(root, dir);
        if (disp == 1) SaveHeader(cd, dir);
    }
    fprintf(stderr, "[content] %s '%s' as %s: -> %s (%s)\n", disp == 1 ? "create" : "open", FileNameOf(cd).c_str(), root.c_str(),
            result ? "failed" : "ok", dir.c_str());
    if (pDisp) WriteU32(pDisp, disp);
    if (pLic && result == X_ERROR_SUCCESS) WriteU32(pLic, 0);
    ctx.r3.u64 = Finish(ov, result, disp);
}
IMP(XamContentClose) { TRACE(XamContentClose); FsUnmount(GuestStr(ctx.r3.u32)); ctx.r3.u64 = Finish(ctx.r4.u32, X_ERROR_SUCCESS); }
IMP(XamContentDelete) {   // (user, contentData, overlapped)
    TRACE(XamContentDelete);
    std::string dir = PackageDir(ctx.r4.u32);
    uint32_t r = FsExists(dir) ? (FsRemoveAll(dir), X_ERROR_SUCCESS) : X_ERROR_PATH_NOT_FOUND;
    ctx.r3.u64 = Finish(ctx.r5.u32, r);
}
// XamContentCreateEnumerator(user, device, type, flags, itemsPerEnumerate, bufferSize*, handle*)
IMP(XamContentCreateEnumerator) {
    TRACE(XamContentCreateEnumerator);
    uint32_t device = ctx.r4.u32, type = ctx.r5.u32, per = ctx.r7.u32, pSize = ctx.r8.u32, pHandle = ctx.r9.u32;
    if (device && device != kDeviceHdd) { if (pSize) WriteU32(pSize, 0); ctx.r3.u64 = 0x80070057; return; }
    if (pSize) WriteU32(pSize, kXContentDataSize * per);
    Enumerator e; e.itemSize = kXContentDataSize;
    char t[16]; snprintf(t, sizeof t, "%08X", type);
    std::string dirBase = FsSaveDir() + "/content/" + t;
    for (auto& name : FsList(dirBase)) {
        std::vector<uint8_t> rec(kXContentDataSize);
        if (FILE* f = fopen((dirBase + "/" + name + "/.xcontent").c_str(), "rb")) { size_t n = fread(rec.data(), 1, rec.size(), f); fclose(f); if (n != rec.size()) continue; }
        else continue;
        rec[0] = rec[1] = rec[2] = 0; rec[3] = kDeviceHdd;   // device id
        e.items.push_back(rec);
    }
    if (g_trace || !e.items.empty()) fprintf(stderr, "[content] enumerating type %08X: %zu package(s)\n", type, e.items.size());
    if (pHandle) WriteU32(pHandle, NewEnumerator(std::move(e)));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
IMP(XamCreateEnumeratorHandle) {   // generic enumerator with no items
    TRACE(XamCreateEnumeratorHandle);
    uint32_t pHandle = GuestArgStack(ctx, 7);
    if (pHandle) WriteU32(pHandle, NewEnumerator({}));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamEnumerate(handle, flags, buffer, bufferLength, itemsReturned*, overlapped*)
IMP(XamEnumerate) {
    TRACE(XamEnumerate);
    uint32_t h = ctx.r3.u32, buf = ctx.r5.u32, len = ctx.r6.u32, pCount = ctx.r7.u32, ov = ctx.r8.u32;
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> l(g_enumLock);
        auto it = g_enums.find(h);
        if (it != g_enums.end()) {
            auto& e = it->second;
            while (e.pos < e.items.size() && (count + 1) * e.itemSize <= len) {
                memcpy(g_base + buf + count * e.itemSize, e.items[e.pos].data(), e.itemSize); e.pos++; count++;
            }
        }
    }
    uint32_t r = count ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
    if (!ov && pCount) WriteU32(pCount, count);
    ctx.r3.u64 = Finish(ov, r, count);
}
// XamContentGetLicenseMask(mask*, overlapped): the full game is unlocked
IMP(XamContentGetLicenseMask) { TRACE(XamContentGetLicenseMask); if (ctx.r3.u32) WriteU32(ctx.r3.u32, 0xFFFFFFFF); ctx.r3.u64 = Finish(ctx.r4.u32, X_ERROR_SUCCESS); }
// XamShowDeviceSelectorUI(user, type, flags, totalRequested(u64), deviceId*, overlapped): pick the hard drive
IMP(XamShowDeviceSelectorUI) { TRACE(XamShowDeviceSelectorUI); if (ctx.r7.u32) WriteU32(ctx.r7.u32, kDeviceHdd); ctx.r3.u64 = Finish(ctx.r8.u32, X_ERROR_SUCCESS); }
IMP(XamContentGetDeviceState) { TRACE(XamContentGetDeviceState); uint32_t r = ctx.r3.u32 == kDeviceHdd ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED; ctx.r3.u64 = Finish(ctx.r4.u32, r); }
// XamContentGetDeviceData(device, X_CONTENT_DEVICE_DATA*): id, type, total, free, name (wide)
IMP(XamContentGetDeviceData) {
    TRACE(XamContentGetDeviceData);
    if (ctx.r3.u32 != kDeviceHdd) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    uint32_t p = ctx.r4.u32; memset(g_base + p, 0, 0x50);
    WriteU32(p, kDeviceHdd); WriteU32(p + 4, 1); WriteU64(p + 8, kHddSize); WriteU64(p + 0x10, kHddSize - 0x100000000ull);
    const char* n = "Hard Drive"; for (int i = 0; n[i]; i++) g_base[p + 0x18 + 2 * i + 1] = (uint8_t)n[i];
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamContentGetCreator(user, contentData, isCreator*, xuid*, overlapped)
IMP(XamContentGetCreator) {
    TRACE(XamContentGetCreator);
    if (ctx.r5.u32) WriteU32(ctx.r5.u32, 1);
    if (ctx.r6.u32) WriteU64(ctx.r6.u32, 0xE000000000000001ull);
    ctx.r3.u64 = Finish(ctx.r7.u32, X_ERROR_SUCCESS);
}
IMP(XamContentSetThumbnail) { TRACE(XamContentSetThumbnail); ctx.r3.u64 = Finish(GuestArgStack(ctx, 4), X_ERROR_SUCCESS); }
