// Online games through the relay server (no router setup): the game's connections to the host's lobby and the
// match's packets travel over one WebSocket per game to the relay, which passes them to the other player's game.
//   Joining PC: the game connects to its own PC (127.0.0.1:25860 / 25861); this file accepts those connections and
//               sends them on. The host shows up there as 127.0.0.3.
//   Hosting PC: connections from the friend are made to the local lobby from 127.0.0.2; the friend shows up as
//               127.0.0.2.
// Match packets (UDP) the game sends to that address are sent on; arriving ones are handed to the game's socket from
// that address, so the game sees an ordinary player on its network.
// Messages: 1 OPEN id(4) port(2) | 2 DATA id(4) bytes | 3 CLOSE id(4) | 4 UDP from-port(2) to-port(2) bytes
#ifdef _WIN32
#include "ws_client.h"
#include "net_win.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kLoop = 0x7F000001, kFriendOnHost = 0x7F000002, kHostOnFriend = 0x7F000003;
std::atomic<bool> g_on{false};
bool g_host = false;
std::wstring g_url;
std::mutex g_m;
wsc::Client* g_ws = nullptr;
std::map<uint32_t, int> g_streams;     // id -> local TCP socket
std::map<uint16_t, int> g_udpFrom;     // from-port -> socket bound to the other player's address (delivery)
std::atomic<uint32_t> g_nextId{1};
// Small named messages between the two games (5 SYNC: key length(1) key value), e.g. the lineup picks.
std::mutex g_syncM; std::condition_variable g_syncCv; std::map<std::string, std::string> g_sync;

void Put16(std::string& m, uint16_t v) { m += (char)(v >> 8); m += (char)(v & 255); }
void Put32(std::string& m, uint32_t v) { for (int i = 3; i >= 0; i--) m += (char)((v >> (8 * i)) & 255); }
uint16_t Get16(const std::string& m, size_t o) { return (uint16_t)((uint8_t)m[o] << 8 | (uint8_t)m[o + 1]); }
uint32_t Get32(const std::string& m, size_t o) { return (uint32_t)(uint8_t)m[o] << 24 | (uint8_t)m[o + 1] << 16 | (uint8_t)m[o + 2] << 8 | (uint8_t)m[o + 3]; }

bool Send(const std::string& m) {
    std::lock_guard<std::mutex> l(g_m);
    return g_ws && g_ws->Send(m.data(), m.size());
}
void Close(uint32_t id, bool tellOther) {
    int s = -1;
    { std::lock_guard<std::mutex> l(g_m); auto it = g_streams.find(id); if (it != g_streams.end()) { s = it->second; g_streams.erase(it); } }
    if (s >= 0) hn::Close(s);
    if (tellOther) { std::string m(1, '\3'); Put32(m, id); Send(m); }
}
// local socket -> relay, until it closes
void Pump(uint32_t id, int s) {
    char buf[16384]; int n;
    while ((n = hn::Recv(s, buf, sizeof buf, 0)) > 0) {
        std::string m(1, '\2'); Put32(m, id); m.append(buf, n);
        if (!Send(m)) break;
    }
    bool mine; { std::lock_guard<std::mutex> l(g_m); mine = g_streams.count(id) != 0; }
    if (mine) Close(id, true);
}
// joining PC: the game's connections to "the lobby" on this PC go to the host
void Listen(uint16_t port) {
    int ls = hn::Socket(2, 1, 6);
    if (ls < 0 || hn::Bind(ls, kLoop, port) < 0 || hn::Listen(ls, 8) < 0) { fprintf(stderr, "[relay] could not listen on %u\n", port); return; }
    for (;;) {
        uint32_t ip; uint16_t pt; int c = hn::Accept(ls, &ip, &pt);
        if (c < 0) return;
        uint32_t id = g_nextId++;
        { std::lock_guard<std::mutex> l(g_m); g_streams[id] = c; }
        std::string m(1, '\1'); Put32(m, id); Put16(m, port);
        Send(m);
        std::thread(Pump, id, c).detach();
    }
}
void OnMessage(const std::string& m) {
    if (m.empty()) return;
    switch (m[0]) {
    case 1: if (m.size() >= 7 && g_host) {   // the friend's game opened a connection to our lobby
        uint32_t id = Get32(m, 1); uint16_t port = Get16(m, 5);
        int s = hn::Socket(2, 1, 6);
        if (s < 0 || hn::Bind(s, kFriendOnHost, 0) < 0 || hn::Connect(s, kLoop, port) < 0) { if (s >= 0) hn::Close(s); std::string c(1, '\3'); Put32(c, id); Send(c); break; }
        { std::lock_guard<std::mutex> l(g_m); g_streams[id] = s; }
        std::thread(Pump, id, s).detach();
        break; }
    case 2: if (m.size() >= 5) {
        int s = -1; { std::lock_guard<std::mutex> l(g_m); auto it = g_streams.find(Get32(m, 1)); if (it != g_streams.end()) s = it->second; }
        if (s >= 0) hn::Send(s, m.data() + 5, (int)m.size() - 5, 0);
        break; }
    case 3: if (m.size() >= 5) Close(Get32(m, 1), false); break;
    case 5: if (m.size() >= 2 && m.size() >= 2u + (uint8_t)m[1]) {
        std::string key = m.substr(2, (uint8_t)m[1]), val = m.substr(2 + (uint8_t)m[1]);
        { std::lock_guard<std::mutex> l(g_syncM); g_sync[key] = val; }
        g_syncCv.notify_all();
        fprintf(stderr, "[relay] got '%s' from the other player\n", key.c_str());
        break; }
    case 4: if (m.size() >= 5) {   // a match packet: hand it to our game from the other player's address
        uint16_t from = Get16(m, 1), to = Get16(m, 3);
        int s;
        {
            std::lock_guard<std::mutex> l(g_m);
            auto it = g_udpFrom.find(from);
            if (it == g_udpFrom.end()) {
                s = hn::Socket(2, 2, 17);
                int one = 1; if (s >= 0) hn::SetOpt(s, 0xFFFF /*SOL_SOCKET*/, 4 /*SO_REUSEADDR*/, &one, sizeof one);
                if (s >= 0 && hn::Bind(s, g_host ? kFriendOnHost : kHostOnFriend, from) < 0) { hn::Close(s); s = -1; }
                if (s < 0) { fprintf(stderr, "[relay] could not deliver match packets from port %u\n", from); break; }
                g_udpFrom[from] = s;
            } else s = it->second;
        }
        int r = hn::SendTo(s, m.data() + 5, (int)m.size() - 5, 0, kLoop, to);
        { static std::atomic<int> k{0}; if (k++ < 5) fprintf(stderr, "[relay] match packet from the other player :%u -> :%u, %d bytes (handed to the game: %d)\n", from, to, (int)m.size() - 5, r); }
        break; }
    }
}
void Run(std::wstring url) {
    bool woke = false;
    while (g_on) {
        auto* ws = new wsc::Client;
        if (!ws->Connect(url, 20000)) {
            delete ws;
            if (!woke) { fprintf(stderr, "[relay] waking up the online server...\n"); size_t p = url.find(L"/room/"); wsc::Wake(url.substr(0, p)); woke = true; continue; }
            fprintf(stderr, "[relay] can't reach the online server - retrying\n");
            Sleep(3000); continue;
        }
        { std::lock_guard<std::mutex> l(g_m); g_ws = ws; }
        fprintf(stderr, "[relay] connected to the online server (%s)\n", g_host ? "hosting" : "joining");
        std::string msg; bool text;
        while (ws->Receive(msg, text)) {
            if (text) { fprintf(stderr, "[relay] %s\n", msg == "peer:up" ? "the other player is connected" : msg == "peer:down" ? "the other player left" : msg.c_str()); continue; }
            OnMessage(msg);
        }
        { std::lock_guard<std::mutex> l(g_m); g_ws = nullptr; }
        ws->Close();
        std::vector<uint32_t> ids; { std::lock_guard<std::mutex> l(g_m); for (auto& [id, s] : g_streams) ids.push_back(id); }
        for (uint32_t id : ids) Close(id, false);   // the game reconnects to the lobby on its own
        fprintf(stderr, "[relay] link to the online server dropped - reconnecting\n");
        Sleep(1000);
    }
}
}   // namespace

// Starts the tunnel (once). host: this PC hosts; room: the join code; url: the relay server.
void RelayStart(bool host, const std::string& room, const std::string& url) {
    if (g_on.exchange(true)) return;
    g_host = host;
    std::wstring u(url.begin(), url.end()), r(room.begin(), room.end());
    std::wstring full = u + L"/room/" + r + L"/game?role=" + (host ? L"host" : L"join");
    hn::Init();
    if (!host) for (uint16_t p : {(uint16_t)25860, (uint16_t)42127, (uint16_t)25861, (uint16_t)10899, (uint16_t)25862, (uint16_t)25863}) std::thread(Listen, p).detach();
    std::thread(Run, full).detach();
    fprintf(stderr, "[relay] online through the relay server, room %s (%s)\n", room.c_str(), host ? "hosting" : "joining");
}
bool RelayActive() { return g_on; }
uint32_t RelayPeerIp() { return g_host ? kFriendOnHost : kHostOnFriend; }   // where the other player appears
// a match packet from our game to the other player
bool RelaySendUdp(uint16_t fromPort, uint16_t toPort, const void* data, int n) {
    std::string m(1, '\4'); Put16(m, fromPort); Put16(m, toPort); m.append((const char*)data, n);
    return Send(m);
}
// Named messages to / from the other player's game (relay games only).
bool RelaySyncSend(const std::string& key, const std::string& val) {
    if (!g_on || key.size() > 255) return false;
    std::string m(1, '\5'); m += (char)key.size(); m += key; m += val;
    for (int i = 0; i < 50; i++) { if (Send(m)) return true; Sleep(200); }   // the link may be reconnecting
    return false;
}
bool RelaySyncTake(const std::string& key, std::string& val, int timeoutMs) {
    std::unique_lock<std::mutex> l(g_syncM);
    if (!g_syncCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_sync.count(key) != 0; })) return false;
    val = g_sync[key]; g_sync.erase(key);
    return true;
}
#else
#include <cstdint>
bool RelaySyncSend(const std::string&, const std::string&) { return false; }
bool RelaySyncTake(const std::string&, std::string&, int) { return false; }
#include <string>
void RelayStart(bool, const std::string&, const std::string&) {}
bool RelayActive() { return false; }
uint32_t RelayPeerIp() { return 0; }
bool RelaySendUdp(uint16_t, uint16_t, const void*, int) { return false; }
#endif
