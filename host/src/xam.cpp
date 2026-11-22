// XAM: user profile, notifications, input (placeholder), audio config, misc. One local profile "Player" is
// signed in on controller 1, offline.
#include "kernel.h"
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
static std::string UserName(uint32_t u) { return u == 0 ? std::string(kPlayerName) : std::string(kPlayerName) + " " + std::to_string(u + 1); }
IMP(XamUserGetSigninState) { TRACE(XamUserGetSigninState); ctx.r3.u64 = UserIn(ctx.r3.u32) ? 1 : 0; }   // 1 = signed in locally
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
    if (ctx.r5.u32) WriteU64(ctx.r5.u32, kPlayerXuid + u);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// X_USER_SIGNIN_INFO: xuid(8), unk(4), signin_state(4), unk(4), unk(4), name[16], ...
IMP(XamUserGetSigninInfo) {   // (user, flags, info)
    TRACE(XamUserGetSigninInfo);
    uint32_t u = ctx.r3.u32, p = ctx.r5.u32; memset(g_base + p, 0, 0x28);
    if (!UserIn(u)) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    WriteU64(p, kPlayerXuid + u); WriteU32(p + 0xC, 1); strncpy((char*)g_base + p + 0x18, UserName(u).c_str(), 15);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
IMP(XamUserCheckPrivilege) { TRACE(XamUserCheckPrivilege); if (ctx.r5.u32) WriteU32(ctx.r5.u32, 1); ctx.r3.u64 = 0; }
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
    std::lock_guard<std::mutex> l(g_notesM);
    g_listeners.push_back(ctx.r3.u32);
}
IMP(XNotifyGetNext) {   // (handle, id filter (0 = any), pId, pParam) -> TRUE when one was taken
    TRACE(XNotifyGetNext);
    std::lock_guard<std::mutex> l(g_notesM);
    uint32_t filter = ctx.r4.u32;
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

// ---- messages to system apps (XLiveBase 0xFC, XMP music player 0xFA, ...)
IMP(XMsgInProcessCall) {   // (app, message, arg1, arg2)
    TRACE(XMsgInProcessCall);
    uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, a1 = ctx.r5.u32;
    if (app == 0xFC && msg == 0x58004 && a1) WriteU32(a1, 1);   // XLiveBaseGetLogonId
    if (g_trace) fprintf(stderr, "[xam] XMsgInProcessCall app %X msg %X\n", app, msg);
    ctx.r3.u64 = 0;
}
static void StartIORequest(PPCContext& ctx, const char* name) {   // (app, message, overlapped*, buffer, length, ...)
    uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, ov = ctx.r5.u32;
    if (g_trace) fprintf(stderr, "[xam] %s app %X msg %X\n", name, app, msg);
    if (ov) { CompleteOverlapped(ov, 0, 0); ctx.r3.u64 = X_ERROR_IO_PENDING; return; }
    ctx.r3.u64 = 0;
}
IMP(XMsgStartIORequest)   { TRACE(XMsgStartIORequest);   StartIORequest(ctx, "XMsgStartIORequest"); }
IMP(XMsgStartIORequestEx) { TRACE(XMsgStartIORequestEx); StartIORequest(ctx, "XMsgStartIORequestEx"); }
IMP(XMsgCancelIORequest)  { TRACE(XMsgCancelIORequest);  ctx.r3.u64 = 0; }
