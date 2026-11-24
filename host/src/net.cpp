// The game's network calls (NetDll_*: Winsock-like sockets and Xbox XNet), passed to real sockets on this PC.
// Online (Xbox Live) mode only - offline everything still reports "no network cable", as before. Guest structures
// are big-endian; sockaddr_in is family(2, BE) port(2, network order) addr(4, network order).
// Every NetDll call has an extra first argument (the caller type) in r3.
#include "kernel.h"
#include "net_win.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

bool OnlineMode();
uint64_t OnlineXuid();   // xam.cpp
bool KSetEventHandle(uint32_t h, bool state);

namespace {
constexpr int kWsaNetDown = 10050, kWsaIoPending = 997, kWsaIoIncomplete = 996, kWsaNotSock = 10038;
thread_local int g_err = 0;
void W16(uint32_t a, uint16_t v) { g_base[a] = v >> 8; g_base[a + 1] = v & 0xFF; }
uint16_t R16(uint32_t a) { return (uint16_t)(g_base[a] << 8 | g_base[a + 1]); }
std::atomic<int> g_logged{0};
bool Log() { return g_logged.fetch_add(1) < 400; }   // traffic dumps (the lobby uses most of these)
std::atomic<int> g_info{0};
bool Info() { return g_info.fetch_add(1) < 3000; }   // socket events (UDP sockets, binds, player addresses)
// Match traffic counters, printed every few seconds while they change.
std::atomic<uint64_t> g_udpOut{0}, g_udpIn{0}, g_udpOutB{0}, g_udpInB{0};
void UdpStats() {
    static std::atomic<bool> on{false};
    if (on.exchange(true)) return;
    std::thread([] {
        uint64_t lo = 0, li = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            uint64_t o = g_udpOut, i = g_udpIn;
            if (o != lo || i != li) { fprintf(stderr, "[net] match traffic: sent %llu packets (%llu KB), received %llu packets (%llu KB)\n", (unsigned long long)o, (unsigned long long)(g_udpOutB / 1024), (unsigned long long)i, (unsigned long long)(g_udpInB / 1024)); lo = o; li = i; }
        }
    }).detach();
}
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
void RelayStart(bool host, const std::string& room, const std::string& url);   // relay_tunnel.cpp
bool RelayActive();
uint32_t RelayPeerIp();
bool RelaySendUdp(uint16_t fromPort, uint16_t toPort, const void* data, int n);
// Online through the relay server (the launcher's codes when a relay is set up): hosting writes online_host.txt
// "relay=ROOM" + "url=URL", joining writes online_server.txt "relay ROOM URL".
struct RelayCfg { bool on = false, host = false; std::string room, url; };
static const RelayCfg& Relay() {
    static RelayCfg c = [] {
        RelayCfg r; char b[512];
        if (FILE* f = fopen("mods/online_host.txt", "r")) {
            while (fgets(b, sizeof b, f)) {
                std::string t = b; while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
                if (!t.compare(0, 6, "relay=")) r.room = t.substr(6); else if (!t.compare(0, 4, "url=")) r.url = t.substr(4);
            }
            fclose(f); r.host = true;
        } else if (FILE* f = fopen("mods/online_server.txt", "r")) {
            char room[64] = "", url[400] = "";
            if (fscanf(f, "relay %63s %399s", room, url) == 2) { r.room = room; r.url = url; }
            fclose(f);
        }
        r.on = !r.room.empty() && !r.url.empty();
        if (!r.on) r.host = false;
        return r; }();
    return c;
}
bool RelayMode() { return OnlineMode() && Relay().on; }
// In relay games each player's Xbox address carries an id made from their gamertag id, so the game can tell them apart.
static uint32_t MyRelayId() { uint64_t x = OnlineXuid(); uint32_t v = (uint32_t)(x ^ (x >> 32)) * 2654435761u; return 0x0A000000 | (v & 0x00FFFFFF); }
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
        if (RelayMode()) {   // through the relay: the lobby is reached on this PC either way
            sip = 0x7F000001; sport = 25860;
            if (Relay().host) { if (!g_standIn.exchange(true)) StartLocalLobby(); }
            RelayStart(Relay().host, Relay().room, Relay().url);
            fprintf(stderr, "[net] Live services through the relay (room %s)\n", Relay().room.c_str());
            return true;
        }
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
    if (type != 1 ? Info() : Log()) fprintf(stderr, "[net] socket(af %d, type %d, proto %d) -> %d\n", af, type, ctx.r6.s32, s);
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
    if (Info()) fprintf(stderr, "[net] bind(%d, %s:%u) -> %d\n", ctx.r4.s32, Ip(ip).c_str(), port, r);
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
    if (RelayActive() && ip == RelayPeerIp()) {   // to the other player: through the relay
        uint32_t mip = 0; uint16_t mport = 0; hn::SockName(ctx.r4.s32, &mip, &mport);
        int n = ctx.r6.s32;
        Ret(ctx, RelaySendUdp(mport, port, g_base + ctx.r5.u32, n) ? n : -1);
        g_udpOut++; g_udpOutB += n; UdpStats();
        { static std::atomic<int> k{0}; if (k++ < 5) fprintf(stderr, "[net] match packet to the other player (through the relay) :%u -> :%u, %d bytes\n", mport, port, n); }
        return;
    }
    { std::lock_guard<std::mutex> l(g_obsM); auto it = g_observedPort.find(ip); if (it != g_observedPort.end()) port = it->second; }   // through the peer's router
    int r = Ret(ctx, hn::SendTo(ctx.r4.s32, g_base + ctx.r5.u32, ctx.r6.s32, ctx.r7.s32, ip, port));
    if (r > 0 && (ip >> 24) != 127) { g_udpOut++; g_udpOutB += r; UdpStats(); }
    if (r > 0 && Info() && g_udpOut < 6) fprintf(stderr, "[net] sendto %s:%u\n", Ip(ip).c_str(), port);
    if (r > 0) Hex("sendto", g_base + ctx.r5.u32, r);
}
IMP(NetDll_recvfrom) {   // (caller, s, buf, len, flags, from, fromlen*)
    TRACE(NetDll_recvfrom);
    uint32_t ip = 0; uint16_t port = 0;
    int r = Ret(ctx, hn::RecvFrom(ctx.r4.s32, g_base + ctx.r5.u32, ctx.r6.s32, ctx.r7.s32, &ip, &port));
    if (r >= 0) WriteAddr(ctx.r8.u32, ctx.r9.u32, ip, port);
    if (r > 0 && (ip >> 24) != 127) { std::lock_guard<std::mutex> l(g_obsM); g_observedPort[ip] = port; }
    if (r > 0 && (ip != 0x7F000001)) { g_udpIn++; g_udpInB += r; UdpStats(); if (g_udpIn < 6) fprintf(stderr, "[net] match packet from %s:%u, %d bytes\n", Ip(ip).c_str(), port, r); }
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
// WSARecvFrom(caller, s, WSABUF* {len, buf}, count, received*, flags*, from*, fromlen*, overlapped* [stack], completion)
static void GotFrom(uint32_t ip, uint16_t port, int r) {
    if (r <= 0 || ip == 0x7F000001) return;
    if ((ip >> 24) != 127) { std::lock_guard<std::mutex> l(g_obsM); g_observedPort[ip] = port; }
    g_udpIn++; g_udpInB += r; UdpStats();
    if (g_udpIn < 6) fprintf(stderr, "[net] match packet from %s:%u, %d bytes\n", Ip(ip).c_str(), port, r);
}
IMP(NetDll_WSARecvFrom) {
    TRACE(NetDll_WSARecvFrom);
    int s = ctx.r4.s32; uint32_t bufs = ctx.r5.u32, pRecv = ctx.r7.u32, pFlags = ctx.r8.u32, from = ctx.r9.u32, fromLen = ctx.r10.u32;
    uint32_t ov = ReadU32(ctx.r1.u32 + 0x54);
    uint32_t len = ReadU32(bufs), buf = ReadU32(bufs + 4);
    { static std::atomic<int> k{0}; if (k++ < 3) fprintf(stderr, "[net] WSARecvFrom(%d, %u bytes, overlapped %08X)\n", s, len, ov); }
    if (!ov) {
        uint32_t ip = 0; uint16_t port = 0;
        int r = Ret(ctx, hn::RecvFrom(s, g_base + buf, (int)len, 0, &ip, &port));
        if (r >= 0) { WriteAddr(from, fromLen, ip, port); if (pRecv) WriteU32(pRecv, r); if (pFlags) WriteU32(pFlags, 0); GotFrom(ip, port, r); ctx.r3.u64 = 0; }
        return;
    }
    WriteU32(ov, 0x103); WriteU32(ov + 4, 0);
    std::thread([s, len, buf, ov, from, fromLen] {
        int r = -1; uint32_t ip = 0; uint16_t port = 0;
        for (;;) {   // the socket may be non-blocking: wait until a packet is there (or the socket is closed)
            std::vector<int> rd{s};
            int n = hn::Select(&rd, nullptr, nullptr, 1000);
            if (n < 0) break;
            if (n == 0 || rd.empty()) { if (ReadU32(ov) != 0x103) return; continue; }
            r = hn::RecvFrom(s, g_base + buf, (int)len, 0, &ip, &port);
            break;
        }
        if (r >= 0) { WriteAddr(from, fromLen, ip, port); GotFrom(ip, port, r); }
        WriteU32(ov + 4, r > 0 ? (uint32_t)r : 0); WriteU32(ov, r >= 0 ? 0 : 0xC0000001);
        if (uint32_t ev = ReadU32(ov + 0x10)) KSetEventHandle(ev, true);
    }).detach();
    g_err = kWsaIoPending; ctx.r3.u64 = 0xFFFFFFFF;
}

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
    if (RelayMode() && a) {   // relay games: an id instead of real addresses (XnAddrToInAddr sorts them out)
        uint32_t id = MyRelayId(); uint64_t x = OnlineXuid();
        WriteU32(a, id); WriteU32(a + 4, id); W16(a + 8, 3074);
        for (int i = 0; i < 6; i++) g_base[a + 10 + i] = (uint8_t)(x >> (8 * i));
        g_base[a + 10] = (uint8_t)((g_base[a + 10] & 0xFE) | 0x02);
        ctx.r3.u64 = 0x2 | 0x8 | 0x20 | 0x40 | 0x80; return;
    }
    if (a) { WriteU32(a, ip); WriteU32(a + 4, pub); W16(a + 8, 3074); for (int i = 0; i < 6; i++) g_base[a + 10 + i] = (uint8_t)(0x10 + i); }
    ctx.r3.u64 = 0x2 | 0x8 | 0x20 | 0x40 | 0x80;   // ethernet, DHCP, gateway, DNS, online
}
// A player's Xbox address -> where to send: their internet address, or their home-network address when they are
// behind the same router as this PC (same internet address).
IMP(NetDll_XNetXnAddrToInAddr) {
    TRACE(NetDll_XNetXnAddrToInAddr);
    uint32_t lan = ReadU32(ctx.r4.u32), online = ReadU32(ctx.r4.u32 + 4), out = online ? online : lan;
    if (online && lan && online == g_publicIp.load()) out = lan;
    if (RelayMode()) {   // relay games: this PC's own player -> 127.0.0.1, the other player -> where the relay delivers them
        uint32_t v = online ? online : lan, selfOnHost = 0x7F000002;   // the lobby shows a joiner its own address as 127.0.0.2
        bool mine = v == MyRelayId() || lan == MyRelayId() || v == 0x7F000001 || (!Relay().host && v == selfOnHost);
        out = mine ? 0x7F000001 : RelayPeerIp();
    }
    if (ctx.r6.u32) WriteU32(ctx.r6.u32, out);
    if (Info()) fprintf(stderr, "[net] player address %s / %s -> %s\n", Ip(lan).c_str(), Ip(online).c_str(), Ip(out).c_str());
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

// At start: when hosting for friends (PLAY ONLINE with mods/online_host.txt) the lobby opens right away, so a friend
// can connect before the host reaches the Xbox Live menu (and Windows asks about the firewall before the game window).
void OnlineStartup() {
    if (!OnlineMode()) return;
    if (FILE* f = fopen("mods/online_host.txt", "r")) {
        fclose(f); g_standIn = true; StartLocalLobby(); fprintf(stderr, "[net] hosting: lobby open for friends\n");
        if (RelayMode()) RelayStart(true, Relay().room, Relay().url);   // friends come in through the relay
    }
}

// Quick Match progress (the game's matchmaking step, called every frame while searching): logged when it changes.
// Flags: 8813 waiting for lobby news, 8815 my player record shows the game, 8816 the game record arrived; 8796 state.
extern "C" PPC_FUNC(__imp__sub_88F9E308);
static thread_local bool g_qmForceIdle = false;
PPC_FUNC(sub_88F9E308) {
    static uint64_t last = ~0ull;
    static std::chrono::steady_clock::time_point readySince{};
    uint32_t o = ctx.r3.u32;
    // Both players are in the match (my record shows it, the match record arrived) but the game still waits for its
    // Xbox Live session helper to go idle. There is no real Xbox Live session here (those calls finish at once), so
    // after a few seconds of that the wait is ended.
    bool ready = ReadU32(o + 8796) == 1 && base[o + 8815] && base[o + 8816];
    auto tnow = std::chrono::steady_clock::now();
    if (!ready) readySince = {};
    else if (readySince == std::chrono::steady_clock::time_point{}) readySince = tnow;
    g_qmForceIdle = ready && tnow - readySince > std::chrono::seconds(4);
    uint64_t now = (uint64_t)ReadU32(o + 8796) << 32 | (uint32_t)(base[o + 8813] << 24 | base[o + 8814] << 16 | base[o + 8815] << 8 | base[o + 8816]);
    if (now != last) { last = now; fprintf(stderr, "[net] quick match: state %u, flags %u%u%u%u, game %d\n", ReadU32(o + 8796), base[o + 8813], base[o + 8814], base[o + 8815], base[o + 8816], (int)ReadU32(o + 8808)); }
    __imp__sub_88F9E308(ctx, base);
    g_qmForceIdle = false;
}
// The game-session helper's status ('idle' etc.), checked by Quick Match before it starts the game: logged on change.
extern "C" PPC_FUNC(__imp__sub_8929DFE0);
PPC_FUNC(sub_8929DFE0) {
    uint32_t sel = ctx.r4.u32, o = ctx.r3.u32;
    __imp__sub_8929DFE0(ctx, base);
    if (OnlineMode()) {
        static std::map<uint32_t, uint32_t> last; uint32_t r = ctx.r3.u32;
        auto it = last.find(sel);
        if (it == last.end() || it->second != r) { last[sel] = r; fprintf(stderr, "[net] game session status '%c%c%c%c' = %d (state %d)\n", sel >> 24, sel >> 16 & 255, sel >> 8 & 255, sel & 255, (int)r, (int)ReadU32(o + 27772)); }
    }
}

// Online roster check. When a match is found the game checks its roster against EA's latest online roster
// (osdk_roster.cfg from the lobby). EA's roster server is gone, and everyone here plays the same 2027 roster (the
// launcher keeps it the same on every PC), so "not current" is turned into "current" (the roster screen's
// CheckRoster result: sub_88E63400 = success, sub_88E63478(reason) = failure, 1 = not current).
extern "C" PPC_FUNC(__imp__sub_88E63400);
extern "C" PPC_FUNC(__imp__sub_88E63478);
PPC_FUNC(sub_88E63478) {
    if (OnlineMode() && ctx.r4.s32 != 3) {   // 3 = the player cancelled
        fprintf(stderr, "[net] online roster check (result %d): this PC's roster is current\n", ctx.r4.s32);
        __imp__sub_88E63400(ctx, base);
        return;
    }
    __imp__sub_88E63478(ctx, base);
}
// The game's Xbox Live session helper (create / join / search a session): its state, logged when it changes
// (0 idle, 1 creating, 2 searching, 3 created, 5 joining, 11-13 deleting, 17 failed).
static void HelperState(uint32_t o, const char* where) {
    static std::map<uint32_t, uint32_t> last; static std::mutex m;
    uint32_t st = ReadU32(o + 116);
    std::lock_guard<std::mutex> l(m);
    auto it = last.find(o);
    if (it == last.end() || it->second != st) { last[o] = st; fprintf(stderr, "[net] xbox session helper %08X: state %u (%s, pending %u, handle %08X)\n", o, st, where, ReadU32(o + 72), ReadU32(o + 100)); }
}
extern "C" PPC_FUNC(__imp__sub_88D82368);
PPC_FUNC(sub_88D82368) {
    if (OnlineMode()) HelperState(ctx.r3.u32, "status");
    if (g_qmForceIdle && ctx.r4.u32 == 0x69646C65 /*idle*/) {
        static std::atomic<int> k{0}; if (k++ < 3) fprintf(stderr, "[net] quick match: both players are in - not waiting for the session helper any longer\n");
        ctx.r3.u64 = 1; return;
    }
    __imp__sub_88D82368(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_88D815D8);
PPC_FUNC(sub_88D815D8) {
    uint32_t o = ctx.r3.u32;
    __imp__sub_88D815D8(ctx, base);
    if (OnlineMode()) HelperState(o, "update");
}
// Which local player an Xbox Live session is for (by gamertag id): logged in online games while working this out.
extern "C" PPC_FUNC(__imp__sub_88D728E8);
PPC_FUNC(sub_88D728E8) {
    uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
    __imp__sub_88D728E8(ctx, base);
    if (OnlineMode()) {
        static std::atomic<int> k{0};
        if (k++ < 12) {
            uint32_t tbl = ReadU32(0x89528978);
            std::string t;
            char x[48];
            for (int i = 0; i < 4 && tbl; i++) { snprintf(x, sizeof x, " [%d] %08X%08X", i, ReadU32(tbl + 96 + 32 * i), ReadU32(tbl + 100 + 32 * i)); t += x; }
            snprintf(x, sizeof x, "%08X%08X", b ? ReadU32(b) : 0, b ? ReadU32(b + 4) : 0);
            fprintf(stderr, "[net] session player lookup (name %08X, id %s) -> %d; players:%s\n", a, x, ctx.r3.s32, t.c_str());
        }
    }
}
// Creating the Xbox Live session for a match: logged.
extern "C" PPC_FUNC(__imp__sub_88D80F70);
PPC_FUNC(sub_88D80F70) {
    uint32_t o = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
    __imp__sub_88D80F70(ctx, base);
    if (OnlineMode()) fprintf(stderr, "[net] xbox session create (helper %08X, host %u, %08X) -> %d, state %u\n", o, r4, r5, ctx.r3.s32, ReadU32(o + 116));
}
