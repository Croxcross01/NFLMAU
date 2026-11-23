// A stand-in for EA's online lobby (the 2003-2010 "DirtySock" lobby protocol the game speaks), run inside the game
// on this PC while online play is being built. Messages: 4-byte command, 4-byte subcode (0 = ok), 4-byte total
// length (big-endian, header included), then KEY=value lines ending in a 0 byte.
// Directory: the game sends @dir and is told where the lobby is. Lobby: login (addr, skey, news, auth, pers, sele)
// and everything after. What isn't known yet is answered with an empty reply of the same command, and logged.
#include "net_win.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
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

struct Session {
    int sock; uint32_t ip; std::string name, persona, maddr; int id;
};

std::string Record(const Session& s, const char* cmd = "+who") {
    return Pack(cmd, {{"I", std::to_string(s.id)}, {"M", s.name}, {"N", s.persona.empty() ? s.name : s.persona}, {"MA", s.maddr},
                      {"A", IpText(s.ip)}, {"LA", IpText(s.ip)}, {"G", "0"}, {"P", "1"}, {"F", "U"}, {"LO", "enUS"}, {"PRES", "1"}, {"X", ""}});
}

std::vector<std::string> Answer(Session& s, const std::string& cmd, std::map<std::string, std::string>& f, const std::string& rawBody) {
    std::string ip = IpText(s.ip);
    if (cmd == "@dir") return {Pack("@dir", {{"ADDR", "127.0.0.1"}, {"PORT", std::to_string(kLobbyPort)}, {"SESS", std::to_string(1000000000 + rand() % 900000000)}, {"MASK", Hex128()}})};
    if (cmd == "@tic" || cmd == "addr" || cmd == "~png") return {};   // no answer expected
    if (cmd == "skey") return {Pack("skey", {{"SKEY", "$" + Hex128()}})};
    if (cmd == "news" && f.count("NAME") && f["NAME"] == "osdk_ticker.cfg")   // the news ticker and telemetry servers (quiet stand-ins)
        return {Pack("news", {{"EX-ticker", "127.0.0.1,25862,enUS"}, {"EX-telemetry", "127.0.0.1,25863,enUS"}})};
    if (cmd == "news" && f.count("NAME") && f["NAME"] != "client.cfg") return {Pack("news", {})};   // other config files: none yet (logged)
    if (cmd == "news") return {Pack("news", {{"BUDDY_SERVER", "127.0.0.1"}, {"BUDDY_PORT", "10899"}, {"EX-ticker", "127.0.0.1,25862,enUS"}, {"EX-telemetry", "127.0.0.1,25863,enUS"}, {"TOSAC_URL", ""}, {"TOSA_URL", ""}, {"TOS_URL", ""}})};
    if (cmd == "auth") {
        s.name = f.count("NAME") ? f["NAME"] : f.count("GTAG") ? f["GTAG"] : s.name;
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
        return {Pack("move", {{"IDENT", ident}, {"NAME", f.count("NAME") ? f["NAME"] : "LVL.1"}, {"COUNT", "1"}, {"FLAGS", "A"}, {"LIMIT", "50"}, {"DESC", ""}}),
                Record(s), Pack("+pop", {{"Z", ident + "/1"}})};
    }
    (void)rawBody;
    return {Pack(cmd.c_str(), {})};
}

void Serve(int c, uint32_t ip, int id) {
    Session s{c, ip, "", "", "", id};
    auto openP = std::make_shared<std::atomic<bool>>(true); auto& open = *openP;
    std::thread([openP, c] {   // ~png every 20 s: without traffic the game drops the connection after 60 s
        auto& open = *openP;
        for (int t = 0; open; t++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (open && t % 20 == 19) { std::string m = Pack("~png", {{"REF", "2010.01.01-00:00:00"}, {"TIME", "2"}}); hn::Send(c, m.data(), (int)m.size(), 0); }
        }
    }).detach();
    std::string buf; char tmp[8192]; int n;
    while ((n = hn::Recv(c, tmp, sizeof tmp, 0)) > 0) {
        buf.append(tmp, n);
        while (buf.size() >= 12) {
            uint32_t len = (uint8_t)buf[8] << 24 | (uint8_t)buf[9] << 16 | (uint8_t)buf[10] << 8 | (uint8_t)buf[11];
            if (len < 12 || len > 1 << 20) { fprintf(stderr, "[lobby] bad message length %u - closing\n", len); hn::Close(c); open = false; return; }
            if (buf.size() < len) break;
            std::string cmd = buf.substr(0, 4), body = buf.substr(12, len - 12);
            buf.erase(0, len);
            auto f = Parse(body);
            fprintf(stderr, "[lobby] #%d -> %s %s\n", id, Printable(cmd).c_str(), Printable(body).c_str());
            for (auto& m : Answer(s, cmd, f, body)) {
                hn::Send(c, m.data(), (int)m.size(), 0);
                fprintf(stderr, "[lobby] #%d <- %s %s\n", id, Printable(m.substr(0, 4)).c_str(), Printable(m.substr(12)).c_str());
            }
        }
    }
    open = false;
    fprintf(stderr, "[lobby] #%d closed\n", id);
    hn::Close(c);
}

void Listen(uint16_t port) {
    int ls = hn::Socket(2, 1, 6);
    if (ls < 0 || hn::Bind(ls, 0x7F000001, port) < 0 || hn::Listen(ls, 16) < 0) { fprintf(stderr, "[lobby] could not listen on port %u\n", port); return; }
    fprintf(stderr, "[lobby] listening on 127.0.0.1:%u\n", port);
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

void StartLocalLobby() {
    static bool started = false; if (started) return; started = true;
    for (uint16_t p : {(uint16_t)25860, (uint16_t)42127, kLobbyPort, (uint16_t)10899, (uint16_t)25862, (uint16_t)25863}) std::thread(Listen, p).detach();
}
