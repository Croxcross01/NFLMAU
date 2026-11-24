// XAM: user profile, notifications, input (placeholder), audio config, misc. One local profile "Player" is
// signed in on controller 1, offline.
#include "kernel.h"
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

constexpr uint32_t X_ERROR_SUCCESS = 0, X_ERROR_NO_SUCH_USER = 0x525, X_ERROR_DEVICE_NOT_CONNECTED = 0x48F,
                   X_ERROR_NO_MORE_FILES = 0x12, X_ERROR_FUNCTION_FAILED = 0x65B;
static const char* kPlayerName = "Player";
static const uint64_t kPlayerXuid = 0xE000000000000001ull;   // offline XUID

// ---- users: a local offline profile is signed in on every connected controller ("Player", "Player 2", ...), so a
// second player pressing START on his own pad is signed in at once (the game would otherwise ask for the sign-in UI).
bool PadConnected(uint32_t user);
static bool UserIn(uint32_t u) { return u < 4 && PadConnected(u); }
// Online (Xbox Live) mode, being built: mods/online.txt holds the gamertag. Player 1 is then signed in to "Live".
const std::string& OnlineGamertag() {
    static const std::string tag = [] {
        // only for a game started with PLAY ONLINE: signed in to "Live", the game treats player 1 differently (profile,
        // voice, online services), which upset regular games (throws to the wrong receiver) - so offline games never are
        const char* on = getenv("MADDEN_ONLINE");
        std::string t; if (on && !strcmp(on, "1")) if (FILE* f = fopen("mods/online.txt", "r")) { char b[64] = ""; if (fgets(b, sizeof b, f)) t = b; fclose(f); }
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (t.size() > 15) t.resize(15);
        if (!t.empty()) fprintf(stderr, "[live] online mode: signed in as %s\n", t.c_str());
        return t; }();
    return tag;
}
bool OnlineMode() { return !OnlineGamertag().empty(); }
uint64_t OnlineXuid() {   // mods/online.txt line "xuid=<hex>" (set once by the launcher), else from the gamertag
    static const uint64_t x = [] {
        uint64_t v = 0;
        if (FILE* f = fopen("mods/online.txt", "r")) { char b[128]; while (fgets(b, sizeof b, f)) if (!strncmp(b, "xuid=", 5)) v = strtoull(b + 5, nullptr, 16); fclose(f); }
        if (!v) { uint64_t h = 1469598103934665603ull; for (char c : OnlineGamertag()) h = (h ^ (uint8_t)c) * 1099511628211ull; v = 0x0009000000000000ull | (h & 0xFFFFFFFFFFFull); }
        return v; }();
    return x;
}
static bool UserLive(uint32_t u) { return u == 0 && OnlineMode(); }
static std::string UserName(uint32_t u) { if (UserLive(u)) return OnlineGamertag(); return u == 0 ? std::string(kPlayerName) : std::string(kPlayerName) + " " + std::to_string(u + 1); }
IMP(XamUserGetSigninState) {   // 1 = signed in locally, 2 = signed in to Live
    TRACE(XamUserGetSigninState);
    uint32_t u = ctx.r3.u32; ctx.r3.u64 = !UserIn(u) ? 0 : UserLive(u) ? 2 : 1;
    if (OnlineMode()) { static int n = 0; if (n++ < 3) fprintf(stderr, "[live] XamUserGetSigninState(%u) -> %u\n", u, ctx.r3.u32); }
}
IMP(XamUserGetName) {   // (user, buffer, size)
    TRACE(XamUserGetName);
    uint32_t u = ctx.r3.u32;
    if (!UserIn(u)) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    uint32_t n = ctx.r5.u32; if (n) { memset(g_base + ctx.r4.u32, 0, n); strncpy((char*)g_base + ctx.r4.u32, UserName(u).c_str(), n - 1); }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
IMP(XamUserGetXUID) {   // (user, type, pXuid)
    TRACE(XamUserGetXUID);
    uint32_t u = ctx.r3.u32;
    if (!UserIn(u)) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (ctx.r5.u32) WriteU64(ctx.r5.u32, UserLive(u) ? OnlineXuid() : kPlayerXuid + u);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// X_USER_SIGNIN_INFO: xuid(8), unk(4), signin_state(4), unk(4), unk(4), name[16], ...
IMP(XamUserGetSigninInfo) {   // (user, flags, info)
    TRACE(XamUserGetSigninInfo);
    uint32_t u = ctx.r3.u32, p = ctx.r5.u32; memset(g_base + p, 0, 0x28);
    if (!UserIn(u)) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    WriteU64(p, UserLive(u) ? OnlineXuid() : kPlayerXuid + u); WriteU32(p + 0xC, UserLive(u) ? 2 : 1); strncpy((char*)g_base + p + 0x18, UserName(u).c_str(), 15);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
IMP(XamUserCheckPrivilege) {
    TRACE(XamUserCheckPrivilege);
    if (OnlineMode()) { static int n = 0; if (n++ < 10) fprintf(stderr, "[live] XamUserCheckPrivilege(user %u, privilege %u)\n", ctx.r3.u32, ctx.r4.u32); }
    if (ctx.r5.u32) WriteU32(ctx.r5.u32, 1); ctx.r3.u64 = 0;
}
// XamUserReadProfileSettings(title, user, xuidCount, xuids*, count, ids*, size*, buffer*, overlapped (on the stack)):
// every setting comes back "not set" (source 0), so the game uses its defaults. Result: count, pointer to the
// settings, then 40 bytes per setting (source, -, user/xuid, id, -, 16 data bytes).
void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length, uint32_t extError);
// No headset support: voice is "not available" (the game then skips voice chat).
IMP(XamVoiceCreate) { TRACE(XamVoiceCreate); if (ctx.r5.u32) WriteU32(ctx.r5.u32, 0); ctx.r3.u64 = 0x8007048F; }   // device not connected
IMP(XamUserReadProfileSettings) {
    TRACE(XamUserReadProfileSettings);
    uint32_t user = ctx.r4.u32, nx = ctx.r5.u32, xuids = ctx.r6.u32, n = ctx.r7.u32, ids = ctx.r8.u32, pSize = ctx.r9.u32, buf = ctx.r10.u32;
    uint32_t ov = ReadU32(ctx.r1.u32 + 0x54);
    uint32_t per = nx ? nx : 1, need = 8 + 40 * n * per;
    auto done = [&](uint32_t r) { if (ov) { CompleteOverlapped(ov, r, 0, 0); ctx.r3.u64 = 0x3E5; } else ctx.r3.u64 = r; };
    if (n < 1 || n > 32 || !pSize) { ctx.r3.u64 = 0x57; return; }
    if (!buf || ReadU32(pSize) < need) { if (!ReadU32(pSize) || !buf) WriteU32(pSize, need); ctx.r3.u64 = 0x7A; return; }   // insufficient buffer
    memset(g_base + buf, 0, need);
    WriteU32(buf, n * per); WriteU32(buf + 4, buf + 8);
    uint32_t o = buf + 8;
    for (uint32_t x = 0; x < per; x++)
        for (uint32_t i = 0; i < n; i++, o += 40) {
            if (nx) WriteU64(o + 8, ReadU64(xuids + 8 * x)); else WriteU32(o + 8, user);
            WriteU32(o + 16, ReadU32(ids + 4 * i));
        }
    done(0);
}
IMP(XamUserAreUsersFriends) { TRACE(XamUserAreUsersFriends); if (ctx.r6.u32) WriteU32(ctx.r6.u32, 0); ctx.r3.u64 = 0; }

// ---- notifications: listener handles the game polls; the sign-in UI posts its system notifications here
bool KSetEventHandle(uint32_t h, bool state);
static std::vector<uint32_t> g_listeners;
static std::vector<std::pair<uint32_t, uint32_t>> g_notes;   // (id, param)
static std::mutex g_notesM;
static void PostNote(uint32_t id, uint32_t param) {
    std::lock_guard<std::mutex> l(g_notesM);
    g_notes.push_back({id, param});
    for (uint32_t h : g_listeners) KSetEventHandle(h, true);
}
IMP(XamNotifyCreateListener) {
    TRACE(XamNotifyCreateListener);
    uint32_t o = KAllocDispatcher(kEventNotification, 0, 0x10);
    auto* k = new KObject{KObject::Dispatcher}; k->guestPtr = o;
    ctx.r3.u64 = KNewHandle(k);
    {
        std::lock_guard<std::mutex> l(g_notesM);
        g_listeners.push_back(ctx.r3.u32);
    }
    if (OnlineMode()) {   // Live is up: XN_LIVE_LINK_STATE_CHANGED (up) and XN_LIVE_CONNECTIONCHANGED (logon connection established)
        fprintf(stderr, "[live] listener %08X (areas %08X%08X): posting Live connected\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
        PostNote(0x00020003, 1); PostNote(0x00020001, 0x01510000);
    }
}
IMP(XNotifyGetNext) {   // (handle, id filter (0 = any), pId, pParam) -> TRUE when one was taken
    TRACE(XNotifyGetNext);
    std::lock_guard<std::mutex> l(g_notesM);
    uint32_t filter = ctx.r4.u32;
    if (OnlineMode()) { static std::vector<uint32_t> seen; bool k = false; for (auto x : seen) k |= x == filter; if (!k && seen.size() < 32) { seen.push_back(filter); fprintf(stderr, "[live] XNotifyGetNext filter %08X\n", filter); } }
    for (size_t i = 0; i < g_notes.size(); i++) {
        if (filter && g_notes[i].first != filter) continue;
        if (ctx.r5.u32) WriteU32(ctx.r5.u32, g_notes[i].first);
        if (ctx.r6.u32) WriteU32(ctx.r6.u32, g_notes[i].second);
        g_notes.erase(g_notes.begin() + i);
        if (g_notes.empty()) KSetEventHandle(ctx.r3.u32, false);
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}
// The sign-in UI: every connected controller is already signed in, so it "opens and closes" at once, telling the
// game the sign-in state changed (XN_SYS_UI 9 on/off, XN_SYS_SIGNINCHANGED 10 with the signed-in users' bits).
IMP(XamShowSigninUI) {   // (panes, flags)
    TRACE(XamShowSigninUI);
    uint32_t mask = 0;
    for (uint32_t u = 0; u < 4; u++) if (UserIn(u)) mask |= 1u << u;
    PostNote(9, 1); PostNote(10, mask); PostNote(9, 0);
    fprintf(stderr, "[xam] sign-in UI asked for: users %X are signed in\n", mask);
    ctx.r3.u64 = 0;
}

IMP(XamResetInactivity) { TRACE(XamResetInactivity); ctx.r3.u64 = 0; }
IMP(XamEnableInactivityProcessing) { TRACE(XamEnableInactivityProcessing); ctx.r3.u64 = 0; }

// ---- audio / camera / misc
IMP(XAudioGetSpeakerConfig) { TRACE(XAudioGetSpeakerConfig); if (ctx.r3.u32) WriteU32(ctx.r3.u32, 0x00010001); ctx.r3.u64 = 0; }   // stereo
IMP(XUsbcamGetState) { TRACE(XUsbcamGetState); ctx.r3.u64 = 0; }   // no camera
IMP(XamVoiceHeadsetPresent) { TRACE(XamVoiceHeadsetPresent); ctx.r3.u64 = 0; }
IMP(KiApcNormalRoutineNop) { TRACE(KiApcNormalRoutineNop); }
IMP(VdCallGraphicsNotificationRoutines) { TRACE(VdCallGraphicsNotificationRoutines); ctx.r3.u64 = 0; }


// ---- overlapped (async) XAM requests: we complete everything immediately
// XOVERLAPPED: Result +0, Length +4, Context +8, hEvent +0xC, CompletionRoutine +0x10, CompletionContext +0x14, ExtendedError +0x18
bool KSetEventHandle(uint32_t h, bool state);
constexpr uint32_t X_ERROR_IO_PENDING = 0x3E5;
void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length, uint32_t extError = 0) {
    WriteU32(ov + 0, result); WriteU32(ov + 4, length); WriteU32(ov + 0x18, extError);
    if (uint32_t ev = ReadU32(ov + 0xC)) KSetEventHandle(ev, true);
}

bool LiveServiceAddr(uint32_t serviceId, uint32_t& ip, uint16_t& port);
// ---- messages to system apps (XLiveBase 0xFC, XMP music player 0xFA, ...)
IMP(XMsgInProcessCall) {   // (app, message, arg1, arg2)
    TRACE(XMsgInProcessCall);
    uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, a1 = ctx.r5.u32;
    if (app == 0xFC && msg == 0x58004 && a1) WriteU32(a1, 1);   // XLiveBaseGetLogonId
    if (app == 0xFC && msg == 0x58006 && a1) WriteU32(a1, 1);   // NAT type: open
    if (app == 0xFC && msg == 0x58007 && OnlineMode() && ctx.r6.u32) {   // XOnlineGetServiceInfo(id, XONLINE_SERVICE_INFO*): id, ip, port
        uint32_t ip; uint16_t port; LiveServiceAddr(a1, ip, port);
        uint32_t p = ctx.r6.u32; memset(g_base + p, 0, 12); WriteU32(p, a1); WriteU32(p + 4, ip); g_base[p + 8] = port >> 8; g_base[p + 9] = port & 0xFF;
    }
    if (g_trace || OnlineMode()) fprintf(stderr, "[live] XMsgInProcessCall app %X msg %X a1 %08X a2 %08X\n", app, msg, a1, ctx.r6.u32);
    ctx.r3.u64 = 0;
}
// XSessionCreate (app FB msg B0010), buffer: session object, flags, user, public slots, private slots, XSESSION_INFO*,
// nonce*. XSESSION_INFO: session id (8), host's Xbox address (36), key (16). A hosted session gets a new id and key
// and this PC's address (the game shares it with the other player through the lobby).
void FillXnAddr(uint32_t a);   // net.cpp
static void SessionCreate(uint32_t buf) {
    uint32_t flags = ReadU32(buf + 4), info = ReadU32(buf + 20), nonce = ReadU32(buf + 24);
    static uint32_t seed = 0x9E3779B9u ^ (uint32_t)time(nullptr);
    auto rnd = [] { seed = seed * 1664525u + 1013904223u; return (uint8_t)(seed >> 24); };
    if (nonce) for (int i = 0; i < 8; i++) g_base[nonce + i] = rnd();
    bool host = flags & 1;
    if (info && host) {
        for (int i = 0; i < 8; i++) g_base[info + i] = rnd();
        g_base[info] = (g_base[info] & 0x0F) | 0xE0;   // online peer-to-peer session id
        FillXnAddr(info + 8);
        for (int i = 0; i < 16; i++) g_base[info + 44 + i] = rnd();
    }
    fprintf(stderr, "[live] Xbox Live session created (%s, flags %X, %u+%u players)\n", host ? "hosting" : "joining", flags, ReadU32(buf + 12), ReadU32(buf + 16));
}
static void StartIORequest(PPCContext& ctx, const char* name) {   // (app, message, overlapped*, buffer, length, ...)
    uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, ov = ctx.r5.u32;
    if (g_trace || OnlineMode()) fprintf(stderr, "[live] %s app %X msg %X buf %08X len %X\n", name, app, msg, ctx.r6.u32, ctx.r7.u32);
    if (app == 0xFB && msg == 0xB0010 && ctx.r6.u32) SessionCreate(ctx.r6.u32);
    if (ov) { CompleteOverlapped(ov, 0, 0); ctx.r3.u64 = X_ERROR_IO_PENDING; return; }
    ctx.r3.u64 = 0;
}
IMP(XMsgStartIORequest)   { TRACE(XMsgStartIORequest);   StartIORequest(ctx, "XMsgStartIORequest"); }
IMP(XMsgStartIORequestEx) { TRACE(XMsgStartIORequestEx); StartIORequest(ctx, "XMsgStartIORequestEx"); }
IMP(XMsgCancelIORequest)  { TRACE(XMsgCancelIORequest);  ctx.r3.u64 = 0; }

// Xbox Live session handles (XSessionCreate makes one; the game keeps a reference to its object).
IMP(XamSessionCreateHandle) {   // (handle*)
    TRACE(XamSessionCreateHandle);
    KObject* o = new KObject{KObject::Dispatcher};
    o->guestPtr = KAllocDispatcher(0x11, 0, 0x40);
    uint32_t h = KNewHandle(o);
    if (ctx.r3.u32) WriteU32(ctx.r3.u32, h);
    ctx.r3.u64 = 0;
}
IMP(XamSessionRefObjByHandle) {   // (handle, object*)
    TRACE(XamSessionRefObjByHandle);
    uint32_t o = KHandleToGuestObject(ctx.r3.u32);
    if (!o) { KObject* k = KLookup(ctx.r3.u32); if (k) { if (!k->guestPtr) k->guestPtr = KAllocDispatcher(0x11, 0, 0x40); o = k->guestPtr; } }
    if (!o) { ctx.r3.u64 = 0xC0000008; return; }   // invalid handle
    if (ctx.r4.u32) WriteU32(ctx.r4.u32, o);
    ctx.r3.u64 = 0;
}
