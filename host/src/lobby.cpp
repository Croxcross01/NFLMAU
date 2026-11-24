// A stand-in for EA's online lobby (the 2003-2010 "DirtySock" lobby protocol the game speaks), run inside the game
// on this PC while online play is being built. Messages: 4-byte command, 4-byte subcode (0 = ok), 4-byte total
// length (big-endian, header included), then KEY=value lines ending in a 0 byte.
// Directory: the game sends @dir and is told where the lobby is. Lobby: login (addr, skey, news, auth, pers, sele)
// and everything after. What isn't known yet is answered with an empty reply of the same command, and logged.
bool RelayMode();   // net.cpp
#include "net_win.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <ctime>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
using Fields = std::vector<std::pair<std::string, std::string>>;

std::string Pack(const char cmd[4], const Fields& f, uint32_t sub = 0) {
    std::string body;
    for (auto& kv : f) body += kv.first + "=" + kv.second + "\n";
    body += '\0';
    uint32_t len = 12 + (uint32_t)body.size();
    std::string m(cmd, 4);
    for (int i = 3; i >= 0; i--) m += (char)((sub >> (8 * i)) & 0xFF);
    for (int i = 3; i >= 0; i--) m += (char)((len >> (8 * i)) & 0xFF);
    return m + body;
}
// KEY=value pairs, separated by newlines, tabs or spaces; a value in quotes may hold spaces
std::map<std::string, std::string> Parse(const std::string& body) {
    std::map<std::string, std::string> f; size_t i = 0, n = body.size();
    while (i < n) {
        while (i < n && (body[i] == '\n' || body[i] == '\t' || body[i] == ' ' || body[i] == '\r' || body[i] == '\0')) i++;
        size_t k = i; while (i < n && body[i] != '=' && body[i] != '\n' && body[i] != ' ' && body[i] != '\0') i++;
        if (i >= n || body[i] != '=') continue;
        std::string key = body.substr(k, i - k), val; i++;
        if (i < n && body[i] == '"') { size_t e = body.find('"', i + 1); if (e == std::string::npos) e = n; val = body.substr(i, e - i + 1); i = e + 1; }
        else { size_t v = i; while (i < n && body[i] != '\n' && body[i] != '\t' && body[i] != ' ' && body[i] != '\0') i++; val = body.substr(v, i - v); }
        f[key] = val;
    }
    return f;
}
std::string Printable(const std::string& s) { std::string o; for (char c : s) o += c == '\n' ? '|' : (c >= 32 && c < 127) ? c : '.'; return o; }
std::string IpText(uint32_t ip) { char b[20]; snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }
std::string Hex128() { static std::mt19937_64 r(std::random_device{}()); char b[40]; snprintf(b, sizeof b, "%016llx%016llx", (unsigned long long)r(), (unsigned long long)r()); return b; }

constexpr uint16_t kLobbyPort = 25861;

// ---- everyone connected to this server (one PC hosts it for its friends, see StartLocalLobby)
struct Game;
struct Session {
    int sock; uint32_t ip; int id;
    std::string name, persona, maddr; uint32_t lanIp = 0;
    std::shared_ptr<Game> game; bool searching = false; std::string opparam;
    std::mutex sendM;
    std::string Who() const { return persona.empty() ? name : persona; }
};
struct Game {
    int ident; std::string name, host, params, platparams, custflags = "0", sysflags = "0", minsize = "2", maxsize = "2", sess, when;
    int seed;
    std::vector<std::shared_ptr<Session>> players;
};
std::mutex g_m;   // guards the session list and every game
std::vector<std::shared_ptr<Session>> g_sessions;
std::map<std::string, std::shared_ptr<Game>> g_games;
int g_nextGame = 1;
uint32_t g_hostPublic = 0;   // this PC's internet address when hosting (mods/online_host.txt "public=")

void SendTo(Session& s, const std::string& m) {
    std::lock_guard<std::mutex> l(s.sendM);
    hn::Send(s.sock, m.data(), (int)m.size(), 0);
    if (m.compare(0, 4, "mau?") != 0 && m.compare(0, 4, "~png") != 0) fprintf(stderr, "[lobby] #%d <- %s %s\n", s.id, Printable(m.substr(0, 4)).c_str(), Printable(m.substr(12)).c_str());
}
bool Loopback(uint32_t ip) { return ip == 0x7F000001; }   // this PC's own game (relay players come from 127.0.0.2)
// Relay games: the friend's game reaches this lobby from 127.0.0.2, but is shown as 192.0.2.2 (the game treats any
// 127.x address as this PC); see RelayPeerIp in relay_tunnel.cpp.
uint32_t Shown(uint32_t ip) { return RelayMode() && ip == 0x7F000002 ? 0xC0000202 : ip; }
// Where `who` can be reached from `viewer`'s PC: the host's own game shows up as 127.0.0.1 here, so others get
// the host's internet address instead.
std::string AddrFor(const Session& who, const Session& viewer) {
    if (Loopback(who.ip) && !Loopback(viewer.ip)) return IpText(g_hostPublic ? g_hostPublic : who.lanIp);
    return IpText(Shown(who.ip));
}
std::string LanFor(const Session& who, const Session& viewer) {
    if (RelayMode()) return AddrFor(who, viewer);   // relay games have no home-network addresses
    if (who.lanIp && (Loopback(who.ip) == Loopback(viewer.ip) || who.ip == viewer.ip)) return IpText(who.lanIp);
    return AddrFor(who, viewer);
}

std::string Record(const Session& s, const char* cmd = "+who") {
    return Pack(cmd, {{"I", std::to_string(s.id)}, {"M", s.name}, {"N", s.Who()}, {"MA", s.maddr},
                      {"A", IpText(Shown(s.ip))}, {"LA", IpText(RelayMode() ? Shown(s.ip) : s.lanIp ? s.lanIp : s.ip)}, {"G", s.game ? std::to_string(s.game->ident) : "0"},
                      {"P", "1"}, {"F", "U"}, {"LO", "enUS"}, {"PRES", "1"}, {"X", ""}});
}
// The game record as one player sees it (SELF, and everyone's address from that player's PC).
Fields GameRecord(const Game& g, const Session& viewer) {
    Fields f = {{"IDENT", std::to_string(g.ident)}, {"NAME", g.name}, {"SELF", viewer.Who()}, {"HOST", g.host}, {"PARAMS", g.params},
                {"PLATPARAMS", g.platparams}, {"ROOM", "1"}, {"CUSTFLAGS", g.custflags}, {"SYSFLAGS", g.sysflags},
                {"COUNT", std::to_string(g.players.size())}, {"PRIV", "0"}, {"MINSIZE", g.minsize}, {"MAXSIZE", g.maxsize},
                {"NUMPART", "1"}, {"PARTSIZE0", g.maxsize}, {"PARTPARAMS0", ""}, {"SEED", std::to_string(g.seed)}, {"WHEN", g.when},
                {"AUTH", ""}, {"SESS", g.sess}};
    for (size_t i = 0; i < g.players.size(); i++) {
        auto& p = *g.players[i]; std::string n = std::to_string(i);
        f.push_back({"OPPO" + n, p.Who()}); f.push_back({"OPID" + n, std::to_string(p.id)});
        f.push_back({"ADDR" + n, AddrFor(p, viewer)}); f.push_back({"LADDR" + n, LanFor(p, viewer)});
        f.push_back({"MADDR" + n, p.maddr}); f.push_back({"OPPART" + n, "0"}); f.push_back({"OPPARAM" + n, p.opparam});
        f.push_back({"OPFLAG" + n, "0"}); f.push_back({"PRES" + n, "0"});
    }
    return f;
}
void Notify(Game& g, const char* cmd, const Session* skip = nullptr) {   // g_m held
    for (auto& p : g.players) if (p.get() != skip) SendTo(*p, Pack(cmd, GameRecord(g, *p)));
}
std::shared_ptr<Game> NewGame(Session& host, std::map<std::string, std::string>& f) {   // g_m held
    auto g = std::make_shared<Game>();
    g->ident = g_nextGame++;
    g->name = f.count("NAME") ? f["NAME"] : host.Who() + "-" + std::to_string(g->ident);
    g->host = host.Who();
    auto take = [&](const char* k, std::string& v) { if (f.count(k)) v = f[k]; };
    take("PARAMS", g->params); take("PLATPARAMS", g->platparams); take("CUSTFLAGS", g->custflags); take("SYSFLAGS", g->sysflags);
    take("MINSIZE", g->minsize); take("MAXSIZE", g->maxsize); take("SESS", g->sess);
    g->seed = 1 + rand() % 0x7FFFFFFE;
    time_t t = time(nullptr); char b[32]; strftime(b, sizeof b, "%Y.%m.%d-%H:%M:%S", localtime(&t)); g->when = b;
    g_games[g->name] = g;
    return g;
}
void LeaveGame(Session& s) {   // g_m held
    auto g = s.game; if (!g) return;
    s.game.reset();
    auto& v = g->players;
    for (size_t i = 0; i < v.size(); i++) if (v[i].get() == &s) { v.erase(v.begin() + i); break; }
    if (v.empty() || g->host == s.Who()) {
        g_games.erase(g->name);
        for (auto& p : v) p->game.reset();
        fprintf(stderr, "[lobby] game %s closed\n", g->name.c_str());
    } else Notify(*g, "+mgm");
}

std::vector<std::string> Answer(const std::shared_ptr<Session>& sp, const std::string& cmd, std::map<std::string, std::string>& f) {
    Session& s = *sp;
    std::string ip = IpText(Shown(s.ip));
    if (cmd == "@dir") return {Pack("@dir", {{"ADDR", "127.0.0.1"}, {"PORT", std::to_string(kLobbyPort)}, {"SESS", std::to_string(1000000000 + rand() % 900000000)},
                                             {"MASK", Hex128()}, {"XIP", ip}})};   // XIP: the player's own internet address, as this server sees it
    if (cmd == "mau?") {   // the launcher asking who is here (not the game): host first, then everyone logged in
        std::lock_guard<std::mutex> l(g_m);
        std::string list, host;
        for (auto& o : g_sessions) {
            if (o->Who().empty()) continue;
            if (Loopback(o->ip) && host.empty()) host = o->Who();
            list += (list.empty() ? "" : ",") + o->Who();
        }
        return {Pack("mau?", {{"HOST", host}, {"PLAYERS", list}})};
    }
    if (cmd == "@tic" || cmd == "~png") return {};   // no answer expected
    if (cmd == "addr") { std::vector<uint32_t> v; if (f.count("ADDR") && hn::Resolve(f["ADDR"].c_str(), v)) s.lanIp = v[0]; return {}; }
    if (cmd == "skey") return {Pack("skey", {{"SKEY", "$" + Hex128()}})};
    if (cmd == "news" && f.count("NAME") && f["NAME"] == "osdk_ticker.cfg")   // the news ticker and telemetry servers (quiet stand-ins)
        return {Pack("news", {{"EX-ticker", "127.0.0.1,25862,enUS"}, {"EX-telemetry", "127.0.0.1,25863,enUS"}})};
    if (cmd == "news" && f.count("NAME") && f["NAME"] != "client.cfg") return {Pack("news", {})};   // other config files: none yet (logged)
    if (cmd == "news") return {Pack("news", {{"BUDDY_SERVER", "127.0.0.1"}, {"BUDDY_PORT", "10899"}, {"EX-ticker", "127.0.0.1,25862,enUS"}, {"EX-telemetry", "127.0.0.1,25863,enUS"}, {"TOSAC_URL", ""}, {"TOSA_URL", ""}, {"TOS_URL", ""}})};
    if (cmd == "auth") {
        s.name = f.count("NAME") ? f["NAME"] : f.count("GTAG") ? f["GTAG"] : s.name;
        fprintf(stderr, "[lobby] %s is online%s\n", s.name.c_str(), Loopback(s.ip) ? " (this PC)" : (" from " + IpText(s.ip)).c_str());
        if (f.count("MADDR")) s.maddr = f["MADDR"];
        return {Pack("auth", {{"NAME", s.name}, {"ADDR", ip}, {"PERSONAS", s.name}, {"LOC", "enUS"}, {"MAIL", "player@unleashed"}, {"SPAM", "NN"},
                              {"TOS", "1"}, {"SINCE", "2010.01.01-00:00:00"}, {"LAST", "2010.01.01-00:00:00"}})};
    }
    if (cmd == "pers") {
        s.persona = f.count("PERS") ? f["PERS"] : s.name;
        if (f.count("MADDR")) s.maddr = f["MADDR"];
        return {Pack("pers", {{"PERS", s.persona}, {"LKEY", Hex128()}, {"LOC", "enUS"}, {"A", ip}, {"LA", ip}, {"IDLE", "600000"},
                              {"EX-ticker", "127.0.0.1,25862,enUS"}, {"EX-telemetry", "127.0.0.1,25863,enUS"}})};   // read with LKEY from the same place
    }
    if (cmd == "sele") {   // "yes, all of that", then the player's own record
        Fields echo; for (auto& kv : f) echo.push_back(kv);
        std::lock_guard<std::mutex> l(g_m);
        return {Pack("sele", echo), Record(s)};
    }
    if (cmd == "qdef" || cmd == "usld") {   // quick messages and settings; usld also carries the player's id
        Fields r = {{"IMGATE", "0"}, {"QMSG0", "\"Want to play?\""}, {"QMSG1", "Yes"}, {"QMSG2", "No"}, {"QMSG3", "\"Let's go\""}, {"QMSG4", "\"Good luck\""},
                    {"QMSG5", "\"Good game\""}, {"QMSG6", "Thanks"}, {"QMSG7", "\"Later\""}, {"QMSG8", "Bye"}, {"SPM_EA", "0"}, {"SPM_PART", "0"}};
        if (cmd == "usld") r.push_back({"UID", std::to_string(s.id)});
        return {Pack(cmd.c_str(), r)};
    }
    if (cmd == "cate") {   // stats categories for a view: counts, names (quoted, comma-separated) and their length
        std::string syms = "\"Top Players\"";
        return {Pack("cate", {{"CC", "1"}, {"IC", "1"}, {"VC", "1"}, {"SYMS", syms}, {"SS", std::to_string(syms.size() + 1)}, {"R", "0,1,1,1,1,1,1,1"}})};
    }
    if (cmd == "slst") return {Pack("slst", {{"COUNT", "1"}, {"VIEW0", "Career,\"Career\""}})};
    if (cmd == "rcat") return {Pack("rcat", {{"CAT0", "Default,Global,LVL,,,,,1"}}),
                               Pack("+rom", {{"I", "1"}, {"N", "LVL.1"}, {"DN", "Unleashed"}, {"D", ""}, {"F", "A"}, {"T", "0"}, {"L", "50"}})};
    if (cmd == "arom") {   // auto-join a room by category: how many category pairs, then the room
        int c = 0; while (f.count("cat" + std::to_string(c))) c++;
        return {Pack("arom", {{"COUNT", std::to_string(c)}}), Pack("+rom", {{"I", "1"}, {"N", "LVL.1"}, {"DN", "Unleashed"}, {"D", ""}, {"F", "A"}, {"T", "1"}, {"L", "50"}})};
    }
    if (cmd == "move") {   // into a room (or out of it with no name)
        std::string ident = f.count("IDENT") ? f["IDENT"] : "1";
        std::lock_guard<std::mutex> l(g_m);
        return {Pack("move", {{"IDENT", ident}, {"NAME", f.count("NAME") ? f["NAME"] : "LVL.1"}, {"COUNT", "1"}, {"FLAGS", "A"}, {"LIMIT", "50"}, {"DESC", ""}}),
                Record(s), Pack("+pop", {{"Z", ident + "/" + std::to_string(g_sessions.size())}})};
    }
    // ---- games. gqwk (Quick Match and Create Session): MODE=2 starts looking, MODE=3 stops. Two players looking at
    // the same time are put in one game: the one who was waiting hosts it. Both get the record (+mgm), then the start (+ses).
    if (cmd == "gqwk") {
        std::lock_guard<std::mutex> l(g_m);
        std::string mode = f.count("MODE") ? f["MODE"] : "";
        if (mode == "3") { s.searching = false; return {Pack("gqwk", {})}; }
        if (mode != "2") return {Pack("gqwk", {})};
        if (f.count("PARAMS")) s.opparam = "";
        std::shared_ptr<Session> other;
        for (auto& o : g_sessions) if (o != sp && o->searching && !o->game) { other = o; break; }
        if (!other) { s.searching = true; s.game.reset(); fprintf(stderr, "[lobby] %s is looking for a game\n", s.Who().c_str()); return {Pack("gqwk", {})}; }
        other->searching = false; s.searching = false;
        auto g = NewGame(*other, f);
        g->players = {other, sp}; other->game = g; s.game = g;
        fprintf(stderr, "[lobby] matched %s (host) with %s in game %s\n", other->Who().c_str(), s.Who().c_str(), g->name.c_str());
        SendTo(s, Pack("gqwk", {}));
        for (auto& p : g->players) SendTo(*p, Record(*p));   // each player's own record now shows the game (G=)
        Notify(*g, "+mgm"); Notify(*g, "+ses");
        return {};
    }
    if (cmd == "gcre") {
        std::lock_guard<std::mutex> l(g_m);
        LeaveGame(s);
        auto g = NewGame(s, f); g->players = {sp}; s.game = g;
        return {Pack("gcre", GameRecord(*g, s)), Record(s)};
    }
    if (cmd == "gjoi") {
        std::lock_guard<std::mutex> l(g_m);
        auto it = g_games.find(f.count("NAME") ? f["NAME"] : "");
        if (it == g_games.end()) return {Pack("gjoi", {}, 0x6E666E64)};   // "nfnd"
        auto g = it->second;
        bool in = false; for (auto& p : g->players) in |= p == sp;
        if (!in) g->players.push_back(sp);
        s.game = g;
        Notify(*g, "+mgm", &s);
        return {Pack("gjoi", GameRecord(*g, s)), Record(s)};
    }
    if (cmd == "gset") {
        std::lock_guard<std::mutex> l(g_m);
        auto g = s.game; if (!g) return {Pack("gset", {})};
        auto take = [&](const char* k, std::string& v) { if (f.count(k)) v = f[k]; };
        take("PARAMS", g->params); take("PLATPARAMS", g->platparams); take("CUSTFLAGS", g->custflags); take("SYSFLAGS", g->sysflags); take("SESS", g->sess);
        Notify(*g, "+mgm", &s);
        return {Pack("gset", GameRecord(*g, s))};
    }
    if (cmd == "gsta") {
        std::lock_guard<std::mutex> l(g_m);
        if (s.game) Notify(*s.game, "+ses");
        return {Pack("gsta", {})};
    }
    if (cmd == "glea" || cmd == "gdel") {
        std::lock_guard<std::mutex> l(g_m);
        s.searching = false; LeaveGame(s);
        return {Pack(cmd.c_str(), {})};
    }
    return {Pack(cmd.c_str(), {})};
}

void Serve(int c, uint32_t ip, int id) {
    auto sp = std::make_shared<Session>(); sp->sock = c; sp->ip = ip; sp->id = id;
    { std::lock_guard<std::mutex> l(g_m); g_sessions.push_back(sp); }
    auto openP = std::make_shared<std::atomic<bool>>(true); auto& open = *openP;
    std::thread([openP, sp] {   // ~png every 20 s: without traffic the game drops the connection after 60 s
        auto& open = *openP;
        for (int t = 0; open; t++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (open && t % 20 == 19) SendTo(*sp, Pack("~png", {{"REF", "2010.01.01-00:00:00"}, {"TIME", "2"}}));
        }
    }).detach();
    std::string buf; char tmp[8192]; int n;
    while ((n = hn::Recv(c, tmp, sizeof tmp, 0)) > 0) {
        buf.append(tmp, n);
        bool bad = false;
        while (buf.size() >= 12) {
            uint32_t len = (uint8_t)buf[8] << 24 | (uint8_t)buf[9] << 16 | (uint8_t)buf[10] << 8 | (uint8_t)buf[11];
            if (len < 12 || len > 1 << 20) { fprintf(stderr, "[lobby] bad message length %u - closing\n", len); bad = true; break; }
            if (buf.size() < len) break;
            std::string cmd = buf.substr(0, 4), body = buf.substr(12, len - 12);
            buf.erase(0, len);
            auto f = Parse(body);
            if (cmd != "~png" && cmd != "mau?") fprintf(stderr, "[lobby] #%d -> %s %s\n", id, Printable(cmd).c_str(), Printable(body).c_str());
            for (auto& m : Answer(sp, cmd, f)) SendTo(*sp, m);
        }
        if (bad) break;
    }
    open = false;
    {
        std::lock_guard<std::mutex> l(g_m);
        LeaveGame(*sp);
        for (size_t i = 0; i < g_sessions.size(); i++) if (g_sessions[i] == sp) { g_sessions.erase(g_sessions.begin() + i); break; }
    }
    if (!sp->Who().empty()) fprintf(stderr, "[lobby] %s left\n", sp->Who().c_str());
    fprintf(stderr, "[lobby] #%d closed\n", id);
    hn::Close(c);
}

uint32_t g_listenIp = 0x7F000001;   // 0 (every network) when hosting for friends
void Listen(uint16_t port) {
    int ls = hn::Socket(2, 1, 6);
    if (ls < 0 || hn::Bind(ls, g_listenIp, port) < 0 || hn::Listen(ls, 16) < 0) { fprintf(stderr, "[lobby] could not listen on port %u\n", port); return; }
    fprintf(stderr, "[lobby] listening on %s:%u\n", IpText(g_listenIp).c_str(), port);
    static std::atomic<int> ids{0};
    for (;;) {
        uint32_t ip; uint16_t pt; int c = hn::Accept(ls, &ip, &pt);
        if (c < 0) return;
        int id = ++ids;
        fprintf(stderr, "[lobby] #%d connected on port %u\n", id, port);
        std::thread(Serve, c, ip, id).detach();
    }
}
}   // namespace

// Hosting for friends (mods/online_host.txt, written by the launcher: "public=<this PC's internet address>"): the
// lobby takes connections from other PCs too. Otherwise it only serves this PC's own game.
void StartLocalLobby() {
    static bool started = false; if (started) return; started = true;
    if (RelayMode()) {   // through the relay: friends arrive on this PC (127.0.0.2) and see the host as 127.0.0.3
        g_hostPublic = 0xC0000203;
        fprintf(stderr, "[lobby] hosting for friends through the relay server\n");
    } else if (FILE* fh = fopen("mods/online_host.txt", "r")) {
        g_listenIp = 0; char b[128];
        while (fgets(b, sizeof b, fh)) if (!strncmp(b, "public=", 7)) { std::vector<uint32_t> v; std::string t = b + 7; while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back(); if (hn::Resolve(t.c_str(), v)) g_hostPublic = v[0]; }
        fclose(fh);
        fprintf(stderr, "[lobby] hosting for friends (internet address %s)\n", g_hostPublic ? IpText(g_hostPublic).c_str() : "unknown");
    }
    for (uint16_t p : {(uint16_t)25860, (uint16_t)42127, kLobbyPort, (uint16_t)10899, (uint16_t)25862, (uint16_t)25863}) std::thread(Listen, p).detach();
}
