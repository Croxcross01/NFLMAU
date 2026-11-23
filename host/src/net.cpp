// The game's network calls (NetDll_*: Winsock-like sockets and Xbox XNet), passed to real sockets on this PC.
// Online (Xbox Live) mode only - offline everything still reports "no network cable", as before. Guest structures
// are big-endian; sockaddr_in is family(2, BE) port(2, network order) addr(4, network order).
// Every NetDll call has an extra first argument (the caller type) in r3.
#include "kernel.h"
#include "net_win.h"
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

bool OnlineMode();
bool KSetEventHandle(uint32_t h, bool state);

namespace {
constexpr int kWsaNetDown = 10050, kWsaIoPending = 997, kWsaIoIncomplete = 996, kWsaNotSock = 10038;
thread_local int g_err = 0;
void W16(uint32_t a, uint16_t v) { g_base[a] = v >> 8; g_base[a + 1] = v & 0xFF; }
uint16_t R16(uint32_t a) { return (uint16_t)(g_base[a] << 8 | g_base[a + 1]); }
std::atomic<int> g_logged{0};
bool Log() { return g_logged.fetch_add(1) < 400; }
std::string Ip(uint32_t ip) { char b[20]; snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }
void Hex(const char* what, const uint8_t* p, int n) {   // first bytes of traffic, for working out the protocol
    if (!Log()) return;
    std::string s; char b[4];
    for (int i = 0; i < n && i < 96; i++) { snprintf(b, sizeof b, "%02X", p[i]); s += b; }
    fprintf(stderr, "[net] %s %d bytes: %s%s\n", what, n, s.c_str(), n > 96 ? "..." : "");
}
// This PC's internet address: when hosting, from mods/online_host.txt; when joining, the friend's server tells it
// (XIP= in its first reply). Goes into the Xbox address other players connect to.
std::atomic<uint32_t> g_publicIp{0};
uint32_t g_joinServer = 0;   // joining a friend: their server (mods/online_server.txt)
std::mutex g_obsM; std::map<uint32_t, uint16_t> g_observedPort;   // peer address -> the port its packets really come from
void SniffXip(const uint8_t* p, int n) {
    if (n < 16 || memcmp(p, "@dir", 4) != 0) return;
    std::string body((const char*)p + 12, n - 12);
    size_t i = body.find("XIP="); if (i == std::string::npos) return;
    std::string v = body.substr(i + 4); v = v.substr(0, v.find_first_of("\n \0"));
    unsigned a, b, c, d; if (sscanf(v.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a != 127) { g_publicIp = a << 24 | b << 16 | c << 8 | d; fprintf(stderr, "[net] internet address %s\n", v.c_str()); }
}
int Ret(PPCContext& ctx, int r) { if (r < 0) { g_err = hn::LastError(); ctx.r3.u64 = 0xFFFFFFFFull; } else ctx.r3.u64 = (uint32_t)r; return r; }
void ReadAddr(uint32_t a, uint32_t& ip, uint16_t& port) { ip = a ? ReadU32(a + 4) : 0; port = a ? R16(a + 2) : 0; }
void WriteAddr(uint32_t a, uint32_t lenPtr, uint32_t ip, uint16_t port) {
    if (!a) return;
    memset(g_base + a, 0, 16); W16(a, 2); W16(a + 2, port); WriteU32(a + 4, ip);
    if (lenPtr) WriteU32(lenPtr, 16);
}
}   // namespace

// ---- where the game's Live services are (XOnlineGetServiceInfo). mods/online_server.txt: "host port"; without it a
// stand-in on this PC (127.0.0.1:42127) just records what the game sends.
static std::atomic<bool> g_standIn{false};
void StartLocalLobby();   // lobby.cpp
static void StandInServer(uint16_t port) {
    int ls = hn::Socket(2, 1, 6);
    if (ls < 0 || hn::Bind(ls, 0x7F000001, port) < 0 || hn::Listen(ls, 8) < 0) { fprintf(stderr, "[net] stand-in server: could not listen on %u\n", port); return; }
    fprintf(stderr, "[net] stand-in server listening on 127.0.0.1:%u\n", port);
    for (;;) {
        uint32_t ip; uint16_t pt; int c = hn::Accept(ls, &ip, &pt);
        if (c < 0) return;
        std::thread([c, pt] {
            fprintf(stderr, "[net] stand-in: game connected (port %u)\n", pt);
            uint8_t buf[4096]; int n;
            while ((n = hn::Recv(c, buf, sizeof buf, 0)) > 0) Hex("stand-in got", buf, n);
            fprintf(stderr, "[net] stand-in: connection closed\n");
            hn::Close(c);
        }).detach();
    }
}
bool LiveServiceAddr(uint32_t serviceId, uint32_t& ip, uint16_t& port) {
    static uint32_t sip = 0; static uint16_t sport = 0;
    static bool once = [] {
        char host[256] = ""; unsigned p = 0;
        if (FILE* f = fopen("mods/online_server.txt", "r")) { if (fscanf(f, "%255s %u", host, &p) != 2) host[0] = 0; fclose(f); }
        std::vector<uint32_t> ips;
        if (host[0] && p && hn::Resolve(host, ips)) { sip = ips[0]; sport = (uint16_t)p; g_joinServer = sip; fprintf(stderr, "[net] joining the server at %s\n", host); }
        else {   // the game connects to the service address on its own port (25860 seen), so listen on both
            sip = 0x7F000001; sport = 42127;
            if (!g_standIn.exchange(true)) StartLocalLobby();
        }
        fprintf(stderr, "[net] Live services at %s:%u\n", Ip(sip).c_str(), sport);
        return true; }();
    (void)once;
    ip = sip; port = sport;
    fprintf(stderr, "[net] service %08X -> %s:%u\n", serviceId, Ip(ip).c_str(), port);
    return true;
}

// ---- Winsock
IMP(NetDll_WSAGetLastError) { TRACE(NetDll_WSAGetLastError); ctx.r3.u64 = OnlineMode() ? (uint32_t)g_err : (uint32_t)kWsaNetDown; }
IMP(NetDll_socket) {   // (caller, af, type, protocol); VDP (254) is UDP with a length header - plain UDP between our PCs
    TRACE(NetDll_socket);
    if (!OnlineMode()) { g_err = kWsaNetDown; ctx.r3.u64 = 0xFFFFFFFF; return; }
    int af = ctx.r4.s32, type = ctx.r5.s32, proto = ctx.r6.s32;
    if (proto == 254) proto = 17;
    int s = Ret(ctx, hn::Socket(af, type, proto));
    if (Log()) fprintf(stderr, "[net] socket(af %d, type %d, proto %d) -> %d\n", af, type, ctx.r6.s32, s);
}
IMP(NetDll_closesocket) { TRACE(NetDll_closesocket); Ret(ctx, hn::Close(ctx.r4.s32)); }
IMP(NetDll_shutdown) { TRACE(NetDll_shutdown); Ret(ctx, hn::Shutdown(ctx.r4.s32, ctx.r5.s32)); }
IMP(NetDll_setsockopt) {   // (caller, s, level, opt, value*, len)
    TRACE(NetDll_setsockopt);
    int s = ctx.r4.s32, level = ctx.r5.s32, opt = ctx.r6.s32; uint32_t v = ctx.r7.u32; int len = ctx.r8.s32;
    uint8_t buf[64] = {}; int n = len > 64 ? 64 : len;
    if (n == 4) { uint32_t x = ReadU32(v); memcpy(buf, &x, 4); }
    else if (n > 0) memcpy(buf, g_base + v, n);
    int r = hn::SetOpt(s, level, opt, buf, n);
    if (r < 0) { if (Log()) fprintf(stderr, "[net] setsockopt(%d, level %X, opt %X) not supported here - ignored\n", s, level, opt); r = 0; }
    Ret(ctx, r);
}
IMP(NetDll_getsockopt) {   // (caller, s, level, opt, value*, len*)
    TRACE(NetDll_getsockopt);
    uint8_t buf[64] = {}; int len = ctx.r8.u32 ? (int)ReadU32(ctx.r8.u32) : 0; if (len > 64) len = 64;
    int r = hn::GetOpt(ctx.r4.s32, ctx.r5.s32, ctx.r6.s32, buf, &len);
    if (r >= 0 && ctx.r7.u32) { if (len == 4) { uint32_t x; memcpy(&x, buf, 4); WriteU32(ctx.r7.u32, x); } else memcpy(g_base + ctx.r7.u32, buf, len); if (ctx.r8.u32) WriteU32(ctx.r8.u32, len); }
    Ret(ctx, r);
}
IMP(NetDll_ioctlsocket) {   // (caller, s, cmd, arg*)
    TRACE(NetDll_ioctlsocket);
    uint32_t a = ctx.r6.u32 ? ReadU32(ctx.r6.u32) : 0;
    int r = Ret(ctx, hn::Ioctl(ctx.r4.s32, ctx.r5.u32, &a));
    if (r >= 0 && ctx.r6.u32) WriteU32(ctx.r6.u32, a);
}
IMP(NetDll_bind) {
    TRACE(NetDll_bind);
    uint32_t ip; uint16_t port; ReadAddr(ctx.r5.u32, ip, port);
    int r = hn::Bind(ctx.r4.s32, ip, port);
    if (r < 0 && port) { r = hn::Bind(ctx.r4.s32, ip, 0); }   // port taken (a second copy on this PC): any port
    if (Log()) fprintf(stderr, "[net] bind(%d, %s:%u) -> %d\n", ctx.r4.s32, Ip(ip).c_str(), port, r);
    Ret(ctx, r);
}
IMP(NetDll_connect) {
    TRACE(NetDll_connect);
    uint32_t ip; uint16_t port; ReadAddr(ctx.r5.u32, ip, port);
    if (g_joinServer && (ip >> 24) == 127) ip = g_joinServer;   // the lobby is on the friend's PC, not this one
    int r = hn::Connect(ctx.r4.s32, ip, port);
    if (Log()) fprintf(stderr, "[net] connect(%d, %s:%u) -> %d (error %d)\n", ctx.r4.s32, Ip(ip).c_str(), port, r, r < 0 ? hn::LastError() : 0);
    Ret(ctx, r);
}
IMP(NetDll_accept) {
    TRACE(NetDll_accept);
    uint32_t ip = 0; uint16_t port = 0;
    int r = Ret(ctx, hn::Accept(ctx.r4.s32, &ip, &port));
    if (r >= 0) WriteAddr(ctx.r5.u32, ctx.r6.u32, ip, port);
}
IMP(NetDll_send) {   // (caller, s, buf, len, flags)
    TRACE(NetDll_send);
    int r = Ret(ctx, hn::Send(ctx.r4.s32, g_base + ctx.r5.u32, ctx.r6.s32, ctx.r7.s32));
    if (r > 0) Hex("send", g_base + ctx.r5.u32, r);
}
IMP(NetDll_recv) {
    TRACE(NetDll_recv);
    int r = Ret(ctx, hn::Recv(ctx.r4.s32, g_base + ctx.r5.u32, ctx.r6.s32, ctx.r7.s32));
    if (r > 0) { Hex("recv", g_base + ctx.r5.u32, r); SniffXip(g_base + ctx.r5.u32, r); }
}
IMP(NetDll_sendto) {   // (caller, s, buf, len, flags, to, tolen)
    TRACE(NetDll_sendto);
    uint32_t ip; uint16_t port; ReadAddr(ctx.r8.u32, ip, port);
    { std::lock_guard<std::mutex> l(g_obsM); auto it = g_observedPort.find(ip); if (it != g_observedPort.end()) port = it->second; }   // through the peer's router
    int r = Ret(ctx, hn::SendTo(ctx.r4.s32, g_base + ctx.r5.u32, ctx.r6.s32, ctx.r7.s32, ip, port));
    if (r > 0 && Log()) fprintf(stderr, "[net] sendto %s:%u\n", Ip(ip).c_str(), port);
    if (r > 0) Hex("sendto", g_base + ctx.r5.u32, r);
}
IMP(NetDll_recvfrom) {   // (caller, s, buf, len, flags, from, fromlen*)
    TRACE(NetDll_recvfrom);
    uint32_t ip = 0; uint16_t port = 0;
    int r = Ret(ctx, hn::RecvFrom(ctx.r4.s32, g_base + ctx.r5.u32, ctx.r6.s32, ctx.r7.s32, &ip, &port));
    if (r >= 0) WriteAddr(ctx.r8.u32, ctx.r9.u32, ip, port);
    if (r > 0 && (ip >> 24) != 127) { std::lock_guard<std::mutex> l(g_obsM); g_observedPort[ip] = port; }
    if (r > 0) Hex("recvfrom", g_base + ctx.r5.u32, r);
}
IMP(NetDll_getsockname) { TRACE(NetDll_getsockname); uint32_t ip = 0; uint16_t p = 0; if (Ret(ctx, hn::SockName(ctx.r4.s32, &ip, &p)) >= 0) WriteAddr(ctx.r5.u32, ctx.r6.u32, ip, p); }
IMP(NetDll_getpeername) { TRACE(NetDll_getpeername); uint32_t ip = 0; uint16_t p = 0; if (Ret(ctx, hn::PeerName(ctx.r4.s32, &ip, &p)) >= 0) WriteAddr(ctx.r5.u32, ctx.r6.u32, ip, p); }
// select(caller, nfds, read*, write*, except*, timeout*): fd_set = count(u32) + 64 sockets(u32)
IMP(NetDll_select) {
    TRACE(NetDll_select);
    uint32_t sets[3] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32}; std::vector<int> v[3];
    for (int k = 0; k < 3; k++) if (sets[k]) { uint32_t n = ReadU32(sets[k]); for (uint32_t i = 0; i < n && i < 64; i++) v[k].push_back((int)ReadU32(sets[k] + 4 + 4 * i)); }
    int ms = -1; if (ctx.r8.u32) ms = (int)(ReadU32(ctx.r8.u32) * 1000 + ReadU32(ctx.r8.u32 + 4) / 1000);
    int r = Ret(ctx, hn::Select(sets[0] ? &v[0] : nullptr, sets[1] ? &v[1] : nullptr, sets[2] ? &v[2] : nullptr, ms));
    if (r >= 0) for (int k = 0; k < 3; k++) if (sets[k]) { WriteU32(sets[k], (uint32_t)v[k].size()); for (size_t i = 0; i < v[k].size(); i++) WriteU32(sets[k] + 4 + 4 * (uint32_t)i, (uint32_t)v[k][i]); }
}
// WSARecv(caller, s, WSABUF* {len, buf}, count, received*, flags*, overlapped*, completion). WSAOVERLAPPED:
// Internal(status) +0, InternalHigh(bytes) +4, hEvent +0x10. With an overlapped it finishes on its own thread.
IMP(NetDll_WSARecv) {
    TRACE(NetDll_WSARecv);
    int s = ctx.r4.s32; uint32_t bufs = ctx.r5.u32, pRecv = ctx.r7.u32, pFlags = ctx.r8.u32, ov = ctx.r9.u32;
    uint32_t len = ReadU32(bufs), buf = ReadU32(bufs + 4);
    if (!ov) { int r = Ret(ctx, hn::Recv(s, g_base + buf, (int)len, 0)); if (r >= 0) { if (pRecv) WriteU32(pRecv, r); if (pFlags) WriteU32(pFlags, 0); ctx.r3.u64 = 0; } return; }
    WriteU32(ov, 0x103); WriteU32(ov + 4, 0);
    std::thread([s, len, buf, ov] {
        int r = hn::Recv(s, g_base + buf, (int)len, 0);
        if (r > 0) { Hex("recv (overlapped)", g_base + buf, r); SniffXip(g_base + buf, r); }
        WriteU32(ov + 4, r > 0 ? (uint32_t)r : 0); WriteU32(ov, r >= 0 ? 0 : 0xC0000001);
        if (uint32_t ev = ReadU32(ov + 0x10)) KSetEventHandle(ev, true);
    }).detach();
    g_err = kWsaIoPending; ctx.r3.u64 = 0xFFFFFFFF;
}
IMP(NetDll_WSAGetOverlappedResult) {   // (caller, s, overlapped*, transferred*, wait, flags*)
    TRACE(NetDll_WSAGetOverlappedResult);
    uint32_t ov = ctx.r5.u32;
    if (ctx.r7.u32) while (ReadU32(ov) == 0x103) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (ReadU32(ov) == 0x103) { g_err = kWsaIoIncomplete; ctx.r3.u64 = 0; return; }
    if (ctx.r6.u32) WriteU32(ctx.r6.u32, ReadU32(ov + 4));
    if (ctx.r8.u32) WriteU32(ctx.r8.u32, 0);
    ctx.r3.u64 = ReadU32(ov) == 0 ? 1 : 0;
}
IMP(NetDll_WSARecvFrom) { TRACE(NetDll_WSARecvFrom); if (Log()) fprintf(stderr, "[net] WSARecvFrom not supported yet\n"); g_err = kWsaNotSock; ctx.r3.u64 = 0xFFFFFFFF; }

// ---- XNet: the Xbox's secure layer. Between our PCs the "secure" addresses are just the real ones.
// XNADDR: ina(4) inaOnline(4) wPortOnline(2) abEnet[6] abOnline[20]
IMP(NetDll_XNetGetEthernetLinkStatus) { TRACE(NetDll_XNetGetEthernetLinkStatus); ctx.r3.u64 = OnlineMode() ? 0x0B : 0; }   // up, 100 Mbps, full duplex
IMP(NetDll_XNetGetTitleXnAddr) {
    TRACE(NetDll_XNetGetTitleXnAddr);
    uint32_t a = ctx.r4.u32; if (a) memset(g_base + a, 0, 0x24);
    if (!OnlineMode()) { ctx.r3.u64 = 0x1; return; }   // XNET_GET_XNADDR_PENDING
    static const uint32_t ip = hn::LocalIp();
    static const uint32_t hostPub = [] {   // hosting: the launcher wrote this PC's internet address
        uint32_t v = 0; char b[128];
        if (FILE* f = fopen("mods/online_host.txt", "r")) { while (fgets(b, sizeof b, f)) { unsigned x1, x2, x3, x4; if (sscanf(b, "public=%u.%u.%u.%u", &x1, &x2, &x3, &x4) == 4) v = x1 << 24 | x2 << 16 | x3 << 8 | x4; } fclose(f); }
        return v; }();
    uint32_t pub = g_publicIp ? g_publicIp.load() : hostPub ? hostPub : ip;
    if (a) { WriteU32(a, ip); WriteU32(a + 4, pub); W16(a + 8, 3074); for (int i = 0; i < 6; i++) g_base[a + 10 + i] = (uint8_t)(0x10 + i); }
    ctx.r3.u64 = 0x2 | 0x8 | 0x20 | 0x40 | 0x80;   // ethernet, DHCP, gateway, DNS, online
}
// A player's Xbox address -> where to send: their internet address, or their home-network address when they are
// behind the same router as this PC (same internet address).
IMP(NetDll_XNetXnAddrToInAddr) {
    TRACE(NetDll_XNetXnAddrToInAddr);
    uint32_t lan = ReadU32(ctx.r4.u32), online = ReadU32(ctx.r4.u32 + 4), out = online ? online : lan;
    if (online && lan && online == g_publicIp.load()) out = lan;
    if (ctx.r6.u32) WriteU32(ctx.r6.u32, out);
    if (Log()) fprintf(stderr, "[net] player address %s / %s -> %s\n", Ip(lan).c_str(), Ip(online).c_str(), Ip(out).c_str());
    ctx.r3.u64 = 0;
}
IMP(NetDll_XNetInAddrToXnAddr) {   // (caller, ina, xnaddr*, xnkid*)
    TRACE(NetDll_XNetInAddrToXnAddr);
    if (ctx.r5.u32) { memset(g_base + ctx.r5.u32, 0, 0x24); WriteU32(ctx.r5.u32, ctx.r4.u32); WriteU32(ctx.r5.u32 + 4, ctx.r4.u32); W16(ctx.r5.u32 + 8, 3074); }
    ctx.r3.u64 = 0;
}
IMP(NetDll_XNetServerToInAddr) { TRACE(NetDll_XNetServerToInAddr); if (ctx.r6.u32) WriteU32(ctx.r6.u32, ctx.r4.u32); if (Log()) fprintf(stderr, "[net] XNetServerToInAddr(%s, service %08X)\n", Ip(ctx.r4.u32).c_str(), ctx.r5.u32); ctx.r3.u64 = 0; }
IMP(NetDll_XNetCreateKey) {   // (caller, xnkid*, xnkey*) - session key ids (online peer-to-peer flag 0xE0 in the first byte)
    TRACE(NetDll_XNetCreateKey);
    for (int i = 0; i < 8; i++) g_base[ctx.r4.u32 + i] = (uint8_t)rand();
    g_base[ctx.r4.u32] = 0xE0;
    if (ctx.r5.u32) for (int i = 0; i < 16; i++) g_base[ctx.r5.u32 + i] = (uint8_t)rand();
    ctx.r3.u64 = 0;
}
IMP(NetDll_XNetRegisterKey) { TRACE(NetDll_XNetRegisterKey); ctx.r3.u64 = 0; }
IMP(NetDll_XNetUnregisterKey) { TRACE(NetDll_XNetUnregisterKey); ctx.r3.u64 = 0; }
IMP(NetDll_XNetConnect) { TRACE(NetDll_XNetConnect); ctx.r3.u64 = 0; }
IMP(NetDll_XNetGetConnectStatus) { TRACE(NetDll_XNetGetConnectStatus); ctx.r3.u64 = 2; }   // XNET_CONNECT_STATUS_CONNECTED
// XNetDnsLookup(caller, name, event, XNDNS**): XNDNS = status(4) count(4) addresses[8]
IMP(NetDll_XNetDnsLookup) {
    TRACE(NetDll_XNetDnsLookup);
    std::string name = GuestStr(ctx.r4.u32);
    std::vector<uint32_t> ips; bool ok = OnlineMode() && hn::Resolve(name.c_str(), ips);
    uint32_t d = GuestAlloc(4 + 4 + 32);
    WriteU32(d, ok ? 0 : 11001); WriteU32(d + 4, (uint32_t)std::min<size_t>(ips.size(), 8));
    for (size_t i = 0; i < ips.size() && i < 8; i++) WriteU32(d + 8 + 4 * (uint32_t)i, ips[i]);
    if (ctx.r6.u32) WriteU32(ctx.r6.u32, d);
    if (ctx.r5.u32) KSetEventHandle(ctx.r5.u32, true);
    fprintf(stderr, "[net] DNS %s -> %s\n", name.c_str(), ok ? Ip(ips[0]).c_str() : "not found");
    ctx.r3.u64 = 0;
}
IMP(NetDll_XNetDnsRelease) { TRACE(NetDll_XNetDnsRelease); ctx.r3.u64 = 0; }

// ---- QoS: the "how good is the connection" probes. Answered at once: reached, 30 ms, 10 Mbit/s both ways.
// XNQOS: count(4) pending(4) then per target 24 bytes: flags(1: 1 complete, 2 contacted) -(1) probesSent(2)
// probesReceived(2) dataLength(2) data*(4) rttMin(2) rttMedian(2) upBits(4) downBits(4)
static uint32_t StackArg(PPCContext& ctx, int i) { return ReadU32(ctx.r1.u32 + 0x54 + 8 * (i - 8)); }
static uint32_t MakeQos(uint32_t n) {
    if (n < 1) n = 1;
    uint32_t q = GuestAlloc(8 + 24 * n);
    WriteU32(q, n); WriteU32(q + 4, 0);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = q + 8 + 24 * i;
        g_base[e] = 0x03; W16(e + 2, 4); W16(e + 4, 4); W16(e + 12, 30); W16(e + 14, 30); WriteU32(e + 16, 10000000); WriteU32(e + 20, 10000000);
    }
    return q;
}
IMP(NetDll_XNetQosServiceLookup) {   // (caller, flags, event, XNQOS**)
    TRACE(NetDll_XNetQosServiceLookup);
    if (ctx.r6.u32) WriteU32(ctx.r6.u32, MakeQos(1));
    if (ctx.r5.u32) KSetEventHandle(ctx.r5.u32, true);
    ctx.r3.u64 = 0;
}
// XNetQosLookup(caller, cxna, xnaddrs, xnkids, xnkeys, cina, inas, serviceIds, probes, bits, flags, event, XNQOS**)
IMP(NetDll_XNetQosLookup) {
    TRACE(NetDll_XNetQosLookup);
    uint32_t n = ctx.r4.u32 + ctx.r8.u32, ev = StackArg(ctx, 11), out = StackArg(ctx, 12);
    if (out) WriteU32(out, MakeQos(n));
    if (ev) KSetEventHandle(ev, true);
    ctx.r3.u64 = 0;
}
IMP(NetDll_XNetQosListen) { TRACE(NetDll_XNetQosListen); ctx.r3.u64 = 0; }
IMP(NetDll_XNetQosRelease) { TRACE(NetDll_XNetQosRelease); ctx.r3.u64 = 0; }
IMP(XNetLogonGetTitleID) { TRACE(XNetLogonGetTitleID); ctx.r3.u64 = 0x584109ED; }   // this game's title ID
IMP(XNetLogonGetMachineID) { TRACE(XNetLogonGetMachineID); if (ctx.r3.u32) WriteU64(ctx.r3.u32, 0xFA00000012345678ull); ctx.r3.u64 = 0; }

// ---- the game's online log (EA's online SDK writes its progress through sub_88F43878(logger, level, format, ...)).
// Retail builds throw the text away; while online play is being built it goes to our log.
extern "C" PPC_FUNC(__imp__sub_88F43878);
static std::string GuestFormat(PPCContext& ctx, uint32_t fmt, int firstArg) {
    auto arg = [&](int n) -> uint64_t {   // n-th argument (0 = r3); r3..r10, then the caller's parameter area
        if (n <= 7) { const PPCRegister* r[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10}; return r[n]->u64; }
        return ReadU64(ctx.r1.u32 + 16 + 8 * n);
    };
    std::string out; int a = firstArg;
    for (const char* p = GuestStr(fmt); *p && out.size() < 600; p++) {
        if (*p != '%') { out += *p; continue; }
        std::string spec = "%"; p++;
        while (*p && strchr("-+ #0123456789.lhzjt", *p)) { if (*p != 'l' && *p != 'h' && *p != 'z' && *p != 'j' && *p != 't') spec += *p; p++; }
        char c = *p; if (!c) break;
        char b[256];
        switch (c) {
        case '%': out += '%'; continue;
        case 's': { uint32_t s = (uint32_t)arg(a++); out += s ? std::string(GuestStr(s)).substr(0, 200) : "(null)"; continue; }
        case 'c': out += (char)arg(a++); continue;
        case 'd': case 'i': snprintf(b, sizeof b, (spec + "d").c_str(), (int)arg(a++)); break;
        case 'u': case 'x': case 'X': case 'o': snprintf(b, sizeof b, (spec + c).c_str(), (unsigned)arg(a++)); break;
        case 'p': snprintf(b, sizeof b, "%08X", (unsigned)arg(a++)); break;
        case 'f': case 'g': case 'e': { uint64_t v = arg(a++); double d; memcpy(&d, &v, 8); snprintf(b, sizeof b, (spec + c).c_str(), d); break; }
        default: b[0] = c; b[1] = 0; break;
        }
        out += b;
    }
    return out;
}
PPC_FUNC(sub_88F43878) {
    if (OnlineMode() && ctx.r5.u32 >= 0x80000000u) {
        static std::atomic<int> n{0};
        if (n++ < 3000) { std::string t = GuestFormat(ctx, ctx.r5.u32, 3); while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back(); fprintf(stderr, "[osdk %u] %s\n", ctx.r4.u32, t.c_str()); }
    }
    __imp__sub_88F43878(ctx, base);
}
