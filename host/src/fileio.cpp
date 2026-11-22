// File system imports. Guest paths (game:\, d:\, \Device\Cdrom0\, ...) are mapped to the folder holding the
// user's own extracted game files (MADDEN_GAME_DIR, default ./game); writable devices (cache:, save data, ...)
// go to MADDEN_SAVE_DIR (default ./save).
#include "kernel.h"
#include "platform.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <mutex>
#include <chrono>
#include <system_error>
namespace fs = std::filesystem;

// minimal stat replacement on top of std::filesystem
struct FStat { bool dir = false; uint64_t size = 0, mtime100ns = 0, ino = 0; };
static bool FsStat(const std::string& p, FStat& st) {
    std::error_code ec; auto s = fs::status(fs::path(p), ec);
    if (ec || !fs::exists(s)) return false;
    st.dir = fs::is_directory(s);
    st.size = st.dir ? 0 : (uint64_t)fs::file_size(fs::path(p), ec);
    auto t = fs::last_write_time(fs::path(p), ec);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    // file_time_type epoch differs per platform; use a fixed plausible date if it looks odd
    st.mtime100ns = (secs > 0 ? (uint64_t)secs : 1262304000ull) * 10000000ull + 116444736000000000ull;
    st.ino = std::hash<std::string>{}(p);
    return true;
}
static std::vector<std::string> ListDir(const std::string& p) {
    std::vector<std::string> out; std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::path(p), ec)) out.push_back(e.path().filename().string());
    return out;
}
static void MakeDirs(const std::string& p) { std::error_code ec; fs::create_directories(fs::path(p), ec); }

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

static std::string GameDir() { const char* e = getenv("MADDEN_GAME_DIR"); return e ? e : "game"; }
static std::string SaveDir() { const char* e = getenv("MADDEN_SAVE_DIR"); return e ? e : "save"; }
// Mods: a file at mods/active/<path inside the game folder> replaces the game's own file (read-only overlay; the
// original game files are never modified). Created by the dev tools' roster editor.
static std::string ModsDir() { const char* e = getenv("MADDEN_MODS_DIR"); return e ? e : "mods/active"; }
// Roster sets: mods/roster.txt names a folder under mods/rosters (e.g. "2027"); files there are used before
// mods/active (only the roster archive differs). No file / "original": the normal set.
static std::string RosterDir() {
    static std::string dir = [] {
        const char* root = getenv("MADDEN_MODS_ROOT");
        std::string base = root ? root : "mods";
        std::string name;
        if (FILE* f = fopen((base + "/roster.txt").c_str(), "rb")) {
            char b[64] = {}; if (fscanf(f, "%63s", b) == 1) name = b;
            fclose(f);
        }
        if (name.empty() || name == "original") { fprintf(stderr, "[mods] roster: original\n"); return std::string(); }
        for (char c : name) if (!isalnum((unsigned char)c) && c != '_' && c != '-') { fprintf(stderr, "[mods] roster: bad name '%s'\n", name.c_str()); return std::string(); }
        fprintf(stderr, "[mods] roster: %s (mods/rosters/%s)\n", name.c_str(), name.c_str());
        return base + "/rosters/" + name;
    }();
    return dir;
}
static bool g_fileTrace = getenv("MADDEN_FILE_TRACE") != nullptr;

std::string GuestAnsiString(uint32_t p) {
    if (!p) return {};
    uint16_t len = (uint16_t)((g_base[p] << 8) | g_base[p + 1]);
    uint32_t buf = ReadU32(p + 4);
    if (!buf) return {};
    return std::string(GuestStr(buf), len);
}
// OBJECT_ATTRIBUTES: RootDirectory(+0), ObjectName ANSI_STRING*(+4), Attributes(+8)
std::string GuestObjectName(uint32_t oa) { return oa ? GuestAnsiString(ReadU32(oa + 4)) : std::string(); }
uint32_t GuestArgStack(PPCContext& ctx, int i) {
    if (i < 8) return (&ctx.r3)[i].u32;
    return ReadU32(ctx.r1.u32 + 0x54 + (i - 8) * 8);
}

static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// Case-insensitive resolution of each path component (game files are case-insensitive on the console).
static std::string ResolveCase(const std::string& root, const std::string& rel, bool forCreate) {
    std::string cur = root; size_t i = 0;
    while (i < rel.size()) {
        size_t j = rel.find('/', i); if (j == std::string::npos) j = rel.size();
        std::string part = rel.substr(i, j - i); i = j + 1;
        if (part.empty() || part == ".") continue;
        std::string cand = cur + "/" + part; FStat st;
        if (!FsStat(cand, st)) {
            std::string lp = Lower(part);
            for (auto& e : ListDir(cur)) if (Lower(e) == lp) { cand = cur + "/" + e; break; }
        }
        cur = cand;
    }
    (void)forCreate;
    return cur;
}

// Mounted content packages (save games): drive name -> host folder.
static std::mutex g_mountLock;
static std::map<std::string, std::string> g_mounts;
void FsMount(const std::string& drive, const std::string& hostPath) { std::lock_guard<std::mutex> l(g_mountLock); MakeDirs(hostPath); g_mounts[Lower(drive)] = hostPath; }
void FsUnmount(const std::string& drive) { std::lock_guard<std::mutex> l(g_mountLock); g_mounts.erase(Lower(drive)); }
std::string FsSaveDir() { return SaveDir(); }
std::string FsGameDir() { return GameDir(); }
bool FsExists(const std::string& p) { FStat st; return FsStat(p, st); }
void FsRemoveAll(const std::string& p) { std::error_code ec; fs::remove_all(fs::path(p), ec); }
std::vector<std::string> FsList(const std::string& p) { return ListDir(p); }

// Translate a guest path into a host path. Returns false for paths we don't map.
static bool MapPath(std::string p, std::string& host, bool& writableDevice) {
    std::replace(p.begin(), p.end(), '\\', '/');
    for (const char* pre : {"/??/", "//?/"}) if (p.rfind(pre, 0) == 0) p = p.substr(strlen(pre));
    std::string lp = Lower(p), dev, rest;
    struct { const char* prefix; const char* dev; } devs[] = {
        {"/device/cdrom0", "game"}, {"/device/harddisk0/partition1", "game"}, {"/device/harddisk0/cache0", "cache"},
        {"/device/harddisk0/cache1", "cache"}, {"/device/flash", "flash"}, {"/systemroot", "game"},
    };
    for (auto& d : devs) if (lp.rfind(d.prefix, 0) == 0) { dev = d.dev; rest = p.substr(strlen(d.prefix)); break; }
    if (dev.empty()) {
        size_t c = p.find(':');
        if (c != std::string::npos) { dev = Lower(p.substr(0, c)); rest = p.substr(c + 1); }
        else { dev = "game"; rest = p; }
    }
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    {
        std::lock_guard<std::mutex> l(g_mountLock);
        auto m = g_mounts.find(dev);
        if (m != g_mounts.end()) { writableDevice = true; host = ResolveCase(m->second, rest, true); return true; }
    }
    if (dev == "game" || dev == "d" || dev == "media" || dev == "dvd" || dev == "cdrom0") {
        writableDevice = false;
        FStat ms;
        if (!rest.empty() && !RosterDir().empty()) {
            std::string rf = ResolveCase(RosterDir(), rest, false);
            if (FsStat(rf, ms) && !ms.dir) {
                static std::set<std::string> announcedR;
                if (announcedR.insert(rf).second) fprintf(stderr, "[mods] using %s\n", rf.c_str());
                host = rf; return true;
            }
        }
        std::string mod = ResolveCase(ModsDir(), rest, false);
        if (!rest.empty() && FsStat(mod, ms) && !ms.dir) {
            static std::set<std::string> announced;
            if (announced.insert(mod).second) fprintf(stderr, "[mods] using %s\n", mod.c_str());
            host = mod; return true;
        }
        host = ResolveCase(GameDir(), rest, false); return true;
    }
    writableDevice = true;
    std::string root = SaveDir() + "/" + dev;
    MakeDirs(root);
    host = ResolveCase(root, rest, true); return true;
}

// FILE_NETWORK_OPEN_INFORMATION / basic timestamps
static void WriteTimes(uint32_t p, const FStat& st) { for (int i = 0; i < 4; i++) WriteU64(p + i * 8, st.mtime100ns); }
static uint32_t Attr(const FStat& st) { return st.dir ? 0x10 : 0x80; }
static void IoStatus(uint32_t iosb, uint32_t status, uint32_t info) { if (iosb) { WriteU32(iosb, status); WriteU32(iosb + 4, info); } }

// CreateDisposition: 0 SUPERSEDE, 1 OPEN, 2 CREATE, 3 OPEN_IF, 4 OVERWRITE, 5 OVERWRITE_IF
static uint32_t OpenCommon(uint32_t pHandle, uint32_t access, uint32_t oa, uint32_t iosb, uint32_t disposition, uint32_t options) {
    std::string name = GuestObjectName(oa);
    uint32_t root = oa ? ReadU32(oa) : 0;
    if (root) { KObject* r = KLookup(root); if (r && r->file) name = r->file->hostPath + "/" + name; }
    std::string host; bool writable;
    if (root && KLookup(root) && KLookup(root)->file) { host = name; std::replace(host.begin(), host.end(), '\\', '/'); writable = true; }
    else if (!MapPath(name, host, writable)) { IoStatus(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0); return STATUS_OBJECT_PATH_NOT_FOUND; }
    FStat st; bool exists = FsStat(host, st);
    bool wantDir = (options & 1) != 0;   // FILE_DIRECTORY_FILE
    bool wantWrite = (access & (0x40000000 | 0x2 | 0x4 | 0x100)) != 0;   // GENERIC_WRITE / FILE_WRITE_DATA / APPEND / WRITE_ATTRIBUTES
    uint32_t info = 1;   // FILE_OPENED
    if (!exists) {
        if (disposition == 1 || disposition == 4 || !writable) {
            if (g_fileTrace || g_trace) fprintf(stderr, "[file] open %s -> %s : NOT FOUND\n", name.c_str(), host.c_str());
            IoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0); return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (wantDir) MakeDirs(host);
        else { FILE* f = fopen(host.c_str(), "wb"); if (f) fclose(f); }
        info = 2;   // FILE_CREATED
        exists = FsStat(host, st);
        if (!exists) { IoStatus(iosb, STATUS_ACCESS_DENIED, 0); return STATUS_ACCESS_DENIED; }
    } else if (disposition == 2) { IoStatus(iosb, STATUS_OBJECT_NAME_COLLISION, 0); return STATUS_OBJECT_NAME_COLLISION; }
    auto* hf = new HostFile(); hf->hostPath = host; hf->isDir = st.dir;
    if (!hf->isDir) {
        bool truncate = writable && (disposition == 0 || disposition == 4 || disposition == 5);
        const char* mode = (writable && wantWrite) ? (truncate ? "w+b" : "r+b") : "rb";
        hf->fp = fopen(host.c_str(), mode);
        if (!hf->fp) hf->fp = fopen(host.c_str(), "rb");
        if (!hf->fp) { delete hf; IoStatus(iosb, STATUS_ACCESS_DENIED, 0); return STATUS_ACCESS_DENIED; }
        if (truncate) info = 3;   // FILE_OVERWRITTEN
    }
    auto* o = new KObject{KObject::File}; o->file = hf;
    uint32_t h = KNewHandle(o);
    if (pHandle) WriteU32(pHandle, h);
    if (g_fileTrace || g_trace) fprintf(stderr, "[file] open %s -> %s : handle %X%s\n", name.c_str(), host.c_str(), h, hf->isDir ? " (dir)" : "");
    IoStatus(iosb, STATUS_SUCCESS, info);
    return STATUS_SUCCESS;
}

// NtCreateFile(pHandle, DesiredAccess, ObjAttr, IoStatus, AllocationSize*, FileAttributes, ShareAccess, CreateDisposition, CreateOptions)
IMP(NtCreateFile) {
    TRACE(NtCreateFile);
    ctx.r3.u64 = OpenCommon(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, GuestArgStack(ctx, 8));
}
// NtOpenFile(pHandle, DesiredAccess, ObjAttr, IoStatus, ShareAccess, OpenOptions)
IMP(NtOpenFile) {
    TRACE(NtOpenFile);
    ctx.r3.u64 = OpenCommon(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32);
}

static HostFile* FileOf(uint32_t h) { KObject* o = KLookup(h); return o ? o->file : nullptr; }
static void SignalEventHandle(uint32_t ev) {
    if (!ev) return;
    uint32_t o = KHandleToGuestObject(ev); if (!o) return;
    std::lock_guard<std::mutex> l(g_kLock); WriteU32(o + 4, 1); g_kCond.notify_all();
}


// ---------------------------------------------------------------- archive entry read log
// For .ast (BGFA) archives, log the first read of each entry: "[asset] qkl_boot.ast entry 350 (22 KB)". This shows
// which roster databases and other assets the game actually loads (e.g. during a match).
struct ArchiveIndex { struct E { uint64_t off, size; uint32_t idx; }; std::vector<E> ents; std::vector<bool> seen; bool ok = false; };
static std::map<std::string, ArchiveIndex> g_archives;
static std::mutex g_archiveLock;
static ArchiveIndex& IndexFor(const std::string& path) {
    auto it = g_archives.find(path);
    if (it != g_archives.end()) return it->second;
    ArchiveIndex& ai = g_archives[path];
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return ai;
    uint8_t h[0x40];
    auto le = [](const uint8_t* p, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i]; return v; };
    if (fread(h, 1, sizeof h, fp) == sizeof h && !memcmp(h, "BGFA1.05", 8)) {
        uint32_t cnt = (uint32_t)le(h + 0xC, 4); uint64_t toff = le(h + 0x10, 8), tsz = le(h + 0x18, 8);
        int hs = h[0x22], w1 = h[0x23], w2 = h[0x24], w3 = h[0x25], sh = h[0x26];
        uint64_t pre = 4 * le(h + 0x28, 4), nl = le(h + 0x2C, 4);
        if (cnt && tsz > pre && tsz < (64u << 20)) {
            std::vector<uint8_t> t(tsz);
            PlatFseek(fp, toff);
            if (fread(t.data(), 1, tsz, fp) == tsz) {
                uint64_t rec = (tsz - pre) / cnt; int fb = int(rec - hs - w1 - w2 - w3 - nl);
                if (fb == 1 || fb == 2) {
                    for (uint32_t i = 0; i < cnt; i++) {
                        const uint8_t* r = t.data() + pre + i * rec; int x = fb + hs;
                        uint64_t o = le(r + x, w1) << sh; x += w1; uint64_t sz = le(r + x, w2);
                        ai.ents.push_back({o, sz, i});
                    }
                    std::sort(ai.ents.begin(), ai.ents.end(), [](auto& a, auto& b) { return a.off < b.off; });
                    ai.seen.assign(cnt, false); ai.ok = true;
                }
            }
        }
    }
    fclose(fp);
    return ai;
}
PPCContext* CurrentGuestContext();
// guest call chain of the current thread (Xbox 360 ABI back chain), to see which game code requested an asset
static std::string GuestChain() {
    std::string out;
    PPCContext* c = CurrentGuestContext();
    if (!c) return out;
    char buf[16];
    snprintf(buf, sizeof buf, " lr=%08X", (uint32_t)c->lr); out += buf;
    uint32_t sp = c->r1.u32;
    for (int i = 0; i < 14 && sp >= 0x10000 && sp < 0xE0000000u; i++) {
        uint32_t next = ReadU32(sp);
        if (next <= sp || next - sp > 0x100000) break;
        snprintf(buf, sizeof buf, " %08X", ReadU32(next - 8)); out += buf;
        sp = next;
    }
    return out;
}
static void LogArchiveRead(const std::string& path, uint64_t off, uint64_t len) {
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".ast") != 0 || !len) return;
    std::lock_guard<std::mutex> l(g_archiveLock);
    ArchiveIndex& ai = IndexFor(path);
    if (!ai.ok) return;
    auto it = std::upper_bound(ai.ents.begin(), ai.ents.end(), off, [](uint64_t v, const ArchiveIndex::E& e) { return v < e.off; });
    if (it != ai.ents.begin()) --it;
    std::string base = path.substr(path.find_last_of("/\\") + 1);
    for (; it != ai.ents.end() && it->off < off + len; ++it) {
        if (it->off + it->size <= off || ai.seen[it->idx]) continue;
        ai.seen[it->idx] = true;
        bool chain = base.find("fe2ig") != std::string::npos;   // uniform / helmet bundles: who asked for it
        fprintf(stderr, "[asset] %s entry %u (%llu KB)%s%s\n", base.c_str(), it->idx, (unsigned long long)((it->size + 1023) / 1024),
                chain ? "  from" : "", chain ? GuestChain().c_str() : "");
    }
}

// NtReadFile(Handle, Event, ApcRoutine, ApcContext, IoStatus, Buffer, Length, ByteOffset*)
IMP(NtReadFile) {
    TRACE(NtReadFile);
    HostFile* f = FileOf(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, pOff = ctx.r10.u32;
    if (!f || !f->fp) { IoStatus(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t off = f->pos;
    if (pOff) { int64_t v = (int64_t)ReadU64(pOff); if (v >= 0) off = (uint64_t)v; }   // -2 = FILE_USE_FILE_POINTER_POSITION
    PlatFseek(f->fp, off);
    PlatEnsureCommitted(g_base + buf, len);
    size_t n = fread(g_base + buf, 1, len, f->fp);
    f->pos = off + n;
    LogArchiveRead(f->hostPath, off, n);
    uint32_t st = (n == 0 && len != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    if (g_fileTrace) fprintf(stderr, "[file] read h=%X off=%llX len=%X -> %zX\n", ctx.r3.u32, (unsigned long long)off, len, n);
    IoStatus(iosb, st, (uint32_t)n);
    SignalEventHandle(ctx.r4.u32);
    ctx.r3.u64 = st;
}
// NtReadFileScatter(Handle, Event, Apc, ApcCtx, IoStatus, SegmentArray, Length, ByteOffset*) - segments are 4KB pages
IMP(NtReadFileScatter) {
    TRACE(NtReadFileScatter);
    HostFile* f = FileOf(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, seg = ctx.r8.u32, len = ctx.r9.u32, pOff = ctx.r10.u32;
    if (!f || !f->fp) { IoStatus(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t off = f->pos; if (pOff) { int64_t v = (int64_t)ReadU64(pOff); if (v >= 0) off = (uint64_t)v; }
    PlatFseek(f->fp, off);
    size_t total = 0;
    for (uint32_t i = 0; total < len; i++) {
        uint32_t page = ReadU32(seg + i * 8) & ~0xFFFu;   // FILE_SEGMENT_ELEMENT (64-bit, low word = address)
        if (!page) page = ReadU32(seg + i * 8 + 4) & ~0xFFFu;
        size_t chunk = std::min<size_t>(0x1000, len - total);
        PlatEnsureCommitted(g_base + page, chunk);
        size_t n = fread(g_base + page, 1, chunk, f->fp); total += n; if (n < chunk) break;
    }
    f->pos = off + total;
    LogArchiveRead(f->hostPath, off, total);
    uint32_t st = (total == 0 && len) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    IoStatus(iosb, st, (uint32_t)total); SignalEventHandle(ctx.r4.u32); ctx.r3.u64 = st;
}
// NtWriteFile(Handle, Event, Apc, ApcCtx, IoStatus, Buffer, Length, ByteOffset*)
IMP(NtWriteFile) {
    TRACE(NtWriteFile);
    HostFile* f = FileOf(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, pOff = ctx.r10.u32;
    if (!f || !f->fp) { IoStatus(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t off = f->pos; if (pOff) { int64_t v = (int64_t)ReadU64(pOff); if (v >= 0) off = (uint64_t)v; }
    PlatFseek(f->fp, off);
    size_t n = fwrite(g_base + buf, 1, len, f->fp); fflush(f->fp);
    f->pos = off + n;
    IoStatus(iosb, STATUS_SUCCESS, (uint32_t)n); SignalEventHandle(ctx.r4.u32); ctx.r3.u64 = STATUS_SUCCESS;
}
IMP(NtFlushBuffersFile) { TRACE(NtFlushBuffersFile); if (HostFile* f = FileOf(ctx.r3.u32)) if (f->fp) fflush(f->fp); IoStatus(ctx.r4.u32, 0, 0); ctx.r3.u64 = 0; }

static bool StatHandle(HostFile* f, FStat& st) { return FsStat(f->hostPath, st); }

// NtQueryInformationFile(Handle, IoStatus, FileInformation, Length, FileInformationClass)
IMP(NtQueryInformationFile) {
    TRACE(NtQueryInformationFile);
    HostFile* f = FileOf(ctx.r3.u32); uint32_t iosb = ctx.r4.u32, p = ctx.r5.u32, len = ctx.r6.u32, cls = ctx.r7.u32;
    FStat st;
    if (!f || !StatHandle(f, st)) { IoStatus(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    memset(g_base + p, 0, len);
    uint32_t info = 0;
    switch (cls) {
    case 4:  WriteTimes(p, st); WriteU32(p + 32, Attr(st)); info = 40; break;                         // FileBasicInformation
    case 5:  WriteU64(p, (st.size + 0xFFF) & ~0xFFFull); WriteU64(p + 8, st.size); WriteU32(p + 16, 1);
             g_base[p + 21] = st.dir; info = 24; break;                                  // FileStandardInformation
    case 6:  WriteU64(p, st.ino); info = 8; break;                                                  // FileInternalInformation
    case 14: WriteU64(p, f->pos); info = 8; break;                                                     // FilePositionInformation
    case 20: WriteU64(p, st.size); info = 8; break;                                                 // FileEndOfFileInformation
    case 34: WriteTimes(p, st); WriteU64(p + 32, (st.size + 0xFFF) & ~0xFFFull); WriteU64(p + 40, st.size);
             WriteU32(p + 48, Attr(st)); info = 56; break;                                             // FileNetworkOpenInformation
    default:
        fprintf(stderr, "[file] NtQueryInformationFile class %u not implemented\n", cls);
        IoStatus(iosb, STATUS_NOT_IMPLEMENTED, 0); ctx.r3.u64 = STATUS_NOT_IMPLEMENTED; return;
    }
    IoStatus(iosb, STATUS_SUCCESS, info); ctx.r3.u64 = STATUS_SUCCESS;
}
// NtSetInformationFile(Handle, IoStatus, FileInformation, Length, FileInformationClass)
IMP(NtSetInformationFile) {
    TRACE(NtSetInformationFile);
    HostFile* f = FileOf(ctx.r3.u32); uint32_t iosb = ctx.r4.u32, p = ctx.r5.u32, cls = ctx.r7.u32;
    if (!f) { IoStatus(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    switch (cls) {
    case 14: f->pos = ReadU64(p); break;                                                               // position
    case 19: case 20: if (f->fp) PlatTruncate(f->fp, ReadU64(p)); break;   // allocation / EOF
    case 4: case 13: break;                                                                            // basic / disposition: ignore
    default: fprintf(stderr, "[file] NtSetInformationFile class %u ignored\n", cls);
    }
    IoStatus(iosb, STATUS_SUCCESS, 0); ctx.r3.u64 = STATUS_SUCCESS;
}
// NtQueryFullAttributesFile(ObjAttr, FILE_NETWORK_OPEN_INFORMATION*)
IMP(NtQueryFullAttributesFile) {
    TRACE(NtQueryFullAttributesFile);
    std::string name = GuestObjectName(ctx.r3.u32), host; bool w;
    FStat st;
    if (!MapPath(name, host, w) || !FsStat(host, st)) {
        if (g_fileTrace || g_trace) fprintf(stderr, "[file] attrs %s -> NOT FOUND\n", name.c_str());
        ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND; return;
    }
    uint32_t p = ctx.r4.u32;
    WriteTimes(p, st); WriteU64(p + 32, (st.size + 0xFFF) & ~0xFFFull); WriteU64(p + 40, st.size); WriteU32(p + 48, Attr(st));
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtQueryVolumeInformationFile(Handle, IoStatus, FsInformation, Length, FsInformationClass)
IMP(NtQueryVolumeInformationFile) {
    TRACE(NtQueryVolumeInformationFile);
    uint32_t iosb = ctx.r4.u32, p = ctx.r5.u32, len = ctx.r6.u32, cls = ctx.r7.u32, info = 0;
    memset(g_base + p, 0, len);
    switch (cls) {
    case 1: { WriteU32(p + 8, 0x12345678); const char* lbl = "GAME"; WriteU32(p + 12, 4); g_base[p + 16] = 0; memcpy(g_base + p + 18, lbl, 4); info = 22; break; }   // volume
    case 3: WriteU64(p, 0x100000); WriteU64(p + 8, 0x80000); WriteU32(p + 16, 32); WriteU32(p + 20, 512); info = 24; break;   // size
    case 5: { WriteU32(p, 0x6); WriteU32(p + 4, 255); const char* fs = "FATX"; WriteU32(p + 8, 4); memcpy(g_base + p + 12, fs, 4); info = 16; break; }   // attribute
    default: fprintf(stderr, "[file] NtQueryVolumeInformationFile class %u not implemented\n", cls);
             IoStatus(iosb, STATUS_NOT_IMPLEMENTED, 0); ctx.r3.u64 = STATUS_NOT_IMPLEMENTED; return;
    }
    IoStatus(iosb, STATUS_SUCCESS, info); ctx.r3.u64 = STATUS_SUCCESS;
}

static bool WildMatch(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return WildMatch(p + 1, s) || (*s && WildMatch(p, s + 1));
    if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) return *s && WildMatch(p + 1, s + 1);
    return false;
}
// NtQueryDirectoryFile(Handle, Event, Apc, ApcCtx, IoStatus, FileInformation, Length, FileMask ANSI_STRING*, RestartScan)
// writes FILE_DIRECTORY_INFORMATION records (one per call, like the console's FATX driver)
IMP(NtQueryDirectoryFile) {
    TRACE(NtQueryDirectoryFile);
    HostFile* f = FileOf(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, p = ctx.r8.u32, len = ctx.r9.u32, pMask = ctx.r10.u32;
    bool restart = GuestArgStack(ctx, 8) != 0;
    if (!f || !f->isDir) { IoStatus(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (!f->dirListed || restart) {
        std::string mask = pMask ? GuestAnsiString(pMask) : "*"; if (mask.empty()) mask = "*";
        f->dirEntries.clear(); f->dirPos = 0; f->dirListed = true;
        for (auto& e : ListDir(f->hostPath)) if (WildMatch(mask.c_str(), e.c_str())) f->dirEntries.push_back(e);
        std::sort(f->dirEntries.begin(), f->dirEntries.end());
    }
    if (f->dirPos >= f->dirEntries.size()) { IoStatus(iosb, STATUS_NO_MORE_FILES, 0); ctx.r3.u64 = STATUS_NO_MORE_FILES; return; }
    const std::string& n = f->dirEntries[f->dirPos];
    if (64 + n.size() > len) { IoStatus(iosb, 0x80000005, 0); ctx.r3.u64 = 0x80000005; return; }   // buffer overflow
    f->dirPos++;
    FStat st; FsStat(f->hostPath + "/" + n, st);
    memset(g_base + p, 0, 64 + n.size());
    WriteU32(p, 0); WriteU32(p + 4, 0);
    WriteTimes(p + 8, st); WriteU64(p + 40, st.size); WriteU64(p + 48, (st.size + 0xFFF) & ~0xFFFull);
    WriteU32(p + 56, Attr(st)); WriteU32(p + 60, (uint32_t)n.size()); memcpy(g_base + p + 64, n.data(), n.size());
    IoStatus(iosb, STATUS_SUCCESS, 64 + (uint32_t)n.size()); SignalEventHandle(ctx.r4.u32); ctx.r3.u64 = STATUS_SUCCESS;
}
IMP(NtDeviceIoControlFile) { TRACE(NtDeviceIoControlFile); ctx.r3.u64 = STATUS_NOT_IMPLEMENTED; }
// Symbolic links (e.g. "game:" -> device). Paths are already mapped, so just accept them.
IMP(ObCreateSymbolicLink) { TRACE(ObCreateSymbolicLink); if (g_trace) fprintf(stderr, "[file] symlink %s -> %s\n", GuestAnsiString(ctx.r3.u32).c_str(), GuestAnsiString(ctx.r4.u32).c_str()); ctx.r3.u64 = 0; }
IMP(ObDeleteSymbolicLink) { TRACE(ObDeleteSymbolicLink); ctx.r3.u64 = 0; }
