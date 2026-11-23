// Online page: your gamertag, and playing a friend over the internet in the game's Xbox Live mode.
//   Host: your game runs the online lobby for your friends. The launcher asks your router to let them in (UPnP),
//         finds your internet address and turns it into a join code. Writes mods/online_host.txt ("public=<ip>").
//   Join: type a friend's join code. Writes mods/online_server.txt ("<ip> 25860") - your game logs in to their lobby.
// Neither: your game uses its own private lobby (you can look around Xbox Live mode on your own).
// Included by launcher.cpp after settings_page.inc (which has the gamertag helpers).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <mutex>

enum { ID_O_GTAG = 600, ID_O_ONLINE, ID_O_GTSAVE, ID_O_HOST, ID_O_STOP, ID_O_COPY, ID_O_CODE, ID_O_JOIN, ID_O_PLAY, ID_O_READY, WM_HOST_READY = WM_APP + 60, WM_JOIN_CHECKED, WM_WHO, WM_PARTY };
static HWND g_oParty, g_oWho, g_oHead1, g_oHead2, g_oHostLbl, g_oJoinLbl, g_oCodeShow, g_oCodeEdit, g_oStatus, g_oHelp;
static std::vector<HWND> g_onlineCtl;
static std::wstring g_myCode;
static constexpr uint16_t kLobbyPort = 25860;

// ---- join codes: an IPv4 address as 7 base-32 letters plus a check letter, shown as XXXX-XXXX
static const char kB32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // no I, L, O, U
static std::wstring CodeFromIp(uint32_t ip) {
    std::string s; int sum = 0;
    for (int i = 6; i >= 0; i--) { int v = (int)((ip >> (i * 5)) & 31); s += kB32[v]; sum += v * (i + 1); }
    s += kB32[sum % 32];
    return std::wstring(s.begin(), s.begin() + 4) + L"-" + std::wstring(s.begin() + 4, s.end());
}
static bool IpFromCode(std::wstring c, uint32_t& ip) {
    std::string s;
    for (wchar_t ch : c) {
        ch = towupper(ch);
        if (ch == L'-' || ch == L' ') continue;
        if (ch == L'O') ch = L'0'; if (ch == L'I' || ch == L'L') ch = L'1';
        s += (char)ch;
    }
    if (s.size() != 8) return false;
    uint64_t v = 0; int sum = 0;
    for (int i = 0; i < 7; i++) { const char* p = strchr(kB32, s[i]); if (!p || !*p) return false; int d = (int)(p - kB32); v = v << 5 | d; sum += d * (7 - i); }
    const char* k = strchr(kB32, s[7]); if (!k || !*k || (k - kB32) != sum % 32 || v > 0xFFFFFFFFull) return false;
    ip = (uint32_t)v;
    return true;
}
static std::wstring IpW(uint32_t ip) { wchar_t b[20]; swprintf(b, 20, L"%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }

// ---- this PC's home-network address (the one the router forwards to)
static uint32_t LanIp() {
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    uint32_t ip = 0; SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = htonl(0x08080808);
        if (connect(s, (sockaddr*)&a, sizeof a) == 0) { sockaddr_in m{}; int l = sizeof m; if (getsockname(s, (sockaddr*)&m, &l) == 0) ip = ntohl(m.sin_addr.s_addr); }
        closesocket(s);
    }
    return ip;
}

// ---- UPnP: Windows' own router-port opener (NATUPnP), declared here because the toolchain has no natupnp.h
struct IStaticPortMapping_ : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_ExternalIPAddress(BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExternalPort(long*) = 0;
};
struct IStaticPortMappingCollection_ : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Item(long, BSTR, IStaticPortMapping_**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Count(long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove(long, BSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE Add(long, BSTR, long, BSTR, VARIANT_BOOL, BSTR, IStaticPortMapping_**) = 0;
};
struct IUPnPNAT_ : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_StaticPortMappingCollection(IStaticPortMappingCollection_**) = 0;
};
static const GUID kClsidUPnPNAT = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
static const GUID kIidUPnPNAT = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};

// Asks the router to send these ports to this PC. Returns how many it opened; sets the internet address if the
// router reports it.
static int OpenRouterPorts(uint32_t lan, uint32_t& publicIp) {
    struct P { long port; const wchar_t* proto; } ports[] = {{25860, L"TCP"}, {25861, L"TCP"}, {3074, L"UDP"}, {3074, L"TCP"}, {25864, L"TCP"}};
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int opened = 0;
    IUPnPNAT_* nat = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidUPnPNAT, nullptr, CLSCTX_ALL, kIidUPnPNAT, (void**)&nat)) && nat) {
        IStaticPortMappingCollection_* col = nullptr;
        for (int tries = 0; tries < 3 && !col; tries++) {   // the router list can take a moment to fill in
            nat->get_StaticPortMappingCollection(&col);
            if (!col) Sleep(1500);
        }
        if (col) {
            BSTR client = SysAllocString(IpW(lan).c_str()), desc = SysAllocString(L"Madden Arcade Unleashed");
            for (auto& p : ports) {
                BSTR proto = SysAllocString(p.proto);
                col->Remove(p.port, proto);
                IStaticPortMapping_* m = nullptr;
                if (SUCCEEDED(col->Add(p.port, proto, p.port, client, VARIANT_TRUE, desc, &m)) && m) {
                    opened++;
                    BSTR ext = nullptr;
                    if (!publicIp && SUCCEEDED(m->get_ExternalIPAddress(&ext)) && ext) {
                        unsigned a, b, c, d; if (swscanf(ext, L"%u.%u.%u.%u", &a, &b, &c, &d) == 4) publicIp = a << 24 | b << 16 | c << 8 | d;
                        SysFreeString(ext);
                    }
                    m->Release();
                }
                SysFreeString(proto);
            }
            SysFreeString(client); SysFreeString(desc);
            col->Release();
        }
        nat->Release();
    }
    CoUninitialize();
    return opened;
}
// The internet address as the rest of the world sees it (a plain-text "what is my IP" service).
static uint32_t AskPublicIp() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring f = std::wstring(tmp) + L"mau_myip.txt";
    bc::Sink s; s.log = [](const std::wstring&) {}; s.status = [](const std::wstring&, int) {};
    uint32_t ip = 0;
    for (const wchar_t* url : {L"https://api.ipify.org", L"https://ipv4.icanhazip.com"}) {
        if (!bc::Download(url, f, s, L"", 0, 0)) continue;
        unsigned a, b, c, d; if (sscanf(bc::ReadText(f).c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) { ip = a << 24 | b << 16 | c << 8 | d; break; }
    }
    DeleteFileW(f.c_str());
    return ip;
}
static bool PrivateIp(uint32_t ip) { return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == (100u << 2 | 1) || (ip >> 24) == 127; }

struct HostResult { uint32_t pub, lan, routerIp; int opened; };
static void HostThread() {
    HostResult* r = new HostResult{0, LanIp(), 0, 0};
    r->opened = OpenRouterPorts(r->lan, r->pub);
    r->routerIp = r->pub;
    uint32_t asked = AskPublicIp();
    if (asked && (!r->pub || PrivateIp(r->pub))) r->pub = asked;   // a router behind another router reports a private address
    PostMessageW(g_wnd, WM_HOST_READY, 0, (LPARAM)r);
}

static std::wstring OnlineDir() { return g_exeDir + L"\\mods"; }
static void OnlineStatus(const std::wstring& t) { SetWindowTextW(g_oStatus, t.c_str()); }
static void RefreshOnlinePage() {
    bool hosting = GetFileAttributesW((OnlineDir() + L"\\online_host.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    std::string join = bc::Trim(bc::ReadText(OnlineDir() + L"\\online_server.txt"));
    if (hosting && !g_myCode.empty()) SetWindowTextW(g_oCodeShow, (L"Your join code:  " + g_myCode).c_str());
    else if (hosting) SetWindowTextW(g_oCodeShow, L"Your join code:  (open this page again to show it)");
    else SetWindowTextW(g_oCodeShow, L"Not hosting.");
    EnableWindow(GetDlgItem(g_wnd, ID_O_COPY), hosting && !g_myCode.empty());
    EnableWindow(GetDlgItem(g_wnd, ID_O_STOP), hosting || !join.empty());
    if (!join.empty() && !hosting) {
        unsigned a, b, c, d; uint32_t ip = 0; if (sscanf(join.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) ip = a << 24 | b << 16 | c << 8 | d;
        OnlineStatus(L"Joining a friend (code " + CodeFromIp(ip) + L"). When you see your friend below, press READY.");
    }
}
// Can this PC open a connection to the host's lobby? (their game must be running with PLAY ONLINE)
static bool CanReach(uint32_t ip, uint16_t port) {
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (s == INVALID_SOCKET) return false;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
    connect(s, (sockaddr*)&a, sizeof a);
    fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
    timeval tv{5, 0};
    bool ok = select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w);
    closesocket(s);
    return ok;
}
// Who is in the lobby right now: asks the lobby ("mau?" - answered by our lobby, never by the game) on the host's
// PC (this PC when hosting, the friend's when joining). Polled every few seconds on the Online page.
static std::wstring* QueryWho(uint32_t ip) {
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (s == INVALID_SOCKET) return nullptr;
    DWORD to = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(kLobbyPort); a.sin_addr.s_addr = htonl(ip);
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); connect(s, (sockaddr*)&a, sizeof a);
    fd_set w; FD_ZERO(&w); FD_SET(s, &w); timeval tv{3, 0};
    if (select(0, nullptr, &w, nullptr, &tv) <= 0) { closesocket(s); return nullptr; }
    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    const char q[13] = {'m', 'a', 'u', '?', 0, 0, 0, 0, 0, 0, 0, 13, 0};
    send(s, q, 13, 0);
    std::string buf; char b[2048]; int n;
    while ((n = recv(s, b, sizeof b, 0)) > 0) {
        buf.append(b, n);
        if (buf.size() >= 12) { uint32_t len = (uint8_t)buf[8] << 24 | (uint8_t)buf[9] << 16 | (uint8_t)buf[10] << 8 | (uint8_t)buf[11]; if (buf.size() >= len) break; }
    }
    closesocket(s);
    if (buf.size() < 12 || buf.compare(0, 4, "mau?") != 0) return nullptr;
    std::string host, players;
    for (size_t i = 12; i < buf.size();) {
        size_t e = buf.find('\n', i); if (e == std::string::npos) e = buf.size();
        std::string line = buf.substr(i, e - i); i = e + 1;
        if (line.rfind("HOST=", 0) == 0) host = line.substr(5);
        if (line.rfind("PLAYERS=", 0) == 0) players = line.substr(8);
    }
    std::wstring out;
    size_t p = 0;
    while (p <= players.size() && !players.empty()) {
        size_t c = players.find(',', p); std::string name = players.substr(p, c == std::string::npos ? std::string::npos : c - p);
        if (!name.empty()) { if (!out.empty()) out += L",  "; out += std::wstring(name.begin(), name.end()) + (name == host ? L" (host)" : L""); }
        if (c == std::string::npos) break; p = c + 1;
    }
    return new std::wstring(out.empty() ? L"Nobody has signed in yet - players show up once they reach Play Now > Xbox Live." : L"Connected now:  " + out);
}
static void PollWho() {   // WM_TIMER on the Online page
    static std::atomic<bool> busy{false};
    if (busy) return;
    bool hosting = GetFileAttributesW((OnlineDir() + L"\\online_host.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    std::string join = bc::Trim(bc::ReadText(OnlineDir() + L"\\online_server.txt"));
    uint32_t ip = 0;
    if (hosting) ip = 0x7F000001;
    else { unsigned a, b, c, d2; if (sscanf(join.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d2) == 4) ip = a << 24 | b << 16 | c << 8 | d2; }
    if (!ip) { SetWindowTextW(g_oWho, L""); return; }
    busy = true;
    std::thread([ip, hosting] {
        std::wstring* r = QueryWho(ip);
        if (!r) r = new std::wstring(hosting ? L"Your lobby isn't open - press PLAY ONLINE to host." : L"The host's lobby isn't answering yet.");
        PostMessageW(g_wnd, WM_WHO, 0, (LPARAM)r);
        busy = false;
    }).detach();
}
static void ShowWho(std::wstring* t) { SetWindowTextW(g_oWho, t->c_str()); delete t; }
static void JoinChecked(bool ok, uint32_t ip) {
    OnlineStatus(ok ? L"Your friend's game answered (code " + CodeFromIp(ip) + L"). Press PLAY ONLINE, then go to Play Now > Xbox Live > Quick Match."
                    : L"Set to join code " + CodeFromIp(ip) + L", but their game didn't answer yet. Make sure your friend pressed PLAY ONLINE "
                      L"while hosting (and allowed it through Windows Firewall), then press Join again to re-check.");
}

// ---- Ready up: before any game starts, the two launchers connect to each other (TCP 25864, opened on the router
// with the others) so each side sees the other's gamertag and whether he is ready. When both are ready the host's game
// starts at once and the friend's a few seconds later (once the host's lobby is up) - nobody starts too early.
// Lines: "HELLO <gamertag>", "READY 0|1", "START", "BYE".
static constexpr uint16_t kPartyPort = 25864;
static std::mutex g_partyM;
static SOCKET g_partySock = INVALID_SOCKET;        // the connection to the other launcher
static std::atomic<int> g_partyRole{0};            // 0 none, 1 host, 2 joining
static std::atomic<uint32_t> g_partyGen{0};        // bumped to stop the current host/join thread
static std::wstring g_peerName; static bool g_peerReady = false, g_meReady = false, g_partyStarted = false;
static std::wstring W8(const std::string& s) { std::wstring w; for (unsigned char c : s) w += (wchar_t)c; return w; }
static void PartyUi() { PostMessageW(g_wnd, WM_PARTY, 0, 0); }
static void PartySend(const std::string& line) {
    std::lock_guard<std::mutex> l(g_partyM);
    if (g_partySock != INVALID_SOCKET) { std::string m = line + "\n"; send(g_partySock, m.data(), (int)m.size(), 0); }
}
static void PartyHello() { PartySend("HELLO " + Utf8(OnlineGet(L"gamertag"))); PartySend(std::string("READY ") + (g_meReady ? "1" : "0")); }
// both ready: the host starts now and tells the friend; the friend starts 4 seconds after hearing it
static void PartyMaybeStart() {
    if (g_partyRole != 1 || g_partyStarted || !g_meReady || !g_peerReady) return;
    g_partyStarted = true;
    PartySend("START");
    PostMessageW(g_wnd, WM_PARTY, 1, 0);   // start the game (UI thread)
}
// reads lines from the other launcher until it goes away
static void PartyRead(SOCKET s, uint32_t gen) {
    std::string buf; char b[512]; int n;
    while (g_partyGen == gen && (n = recv(s, b, sizeof b, 0)) > 0) {
        buf.append(b, n);
        for (size_t e; (e = buf.find('\n')) != std::string::npos; buf.erase(0, e + 1)) {
            std::string line = buf.substr(0, e); if (!line.empty() && line.back() == '\r') line.pop_back();
            { std::lock_guard<std::mutex> l(g_partyM);
              if (line.rfind("HELLO ", 0) == 0) g_peerName = W8(line.substr(6, 15));
              else if (line.rfind("READY ", 0) == 0) g_peerReady = line[6] == '1'; }
            if (line == "START" && g_partyRole == 2 && !g_partyStarted) { g_partyStarted = true; PostMessageW(g_wnd, WM_PARTY, 2, 0); }
            PartyMaybeStart();
            PartyUi();
        }
    }
    { std::lock_guard<std::mutex> l(g_partyM); if (g_partySock == s) g_partySock = INVALID_SOCKET; g_peerName.clear(); g_peerReady = false; }
    closesocket(s);
    PartyUi();
}
static void PartyHostThread(uint32_t gen) {
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL yes = TRUE; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(kPartyPort);
    if (ls == INVALID_SOCKET || bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) { if (ls != INVALID_SOCKET) closesocket(ls); return; }
    while (g_partyGen == gen) {
        fd_set r; FD_ZERO(&r); FD_SET(ls, &r); timeval tv{1, 0};
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(ls, nullptr, nullptr); if (c == INVALID_SOCKET) continue;
        { std::lock_guard<std::mutex> l(g_partyM);
          if (g_partySock != INVALID_SOCKET) { closesocket(g_partySock); }   // a reconnect replaces the old connection
          g_partySock = c; g_peerName.clear(); g_peerReady = false; }
        PartyHello();
        std::thread(PartyRead, c, gen).detach();
        PartyUi();
    }
    closesocket(ls);
}
static void PartyJoinThread(uint32_t ip, uint32_t gen) {   // keeps trying until connected (the host may open later)
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    while (g_partyGen == gen) {
        bool connected; { std::lock_guard<std::mutex> l(g_partyM); connected = g_partySock != INVALID_SOCKET; }
        if (!connected) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(kPartyPort); a.sin_addr.s_addr = htonl(ip);
            u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); connect(s, (sockaddr*)&a, sizeof a);
            fd_set w; FD_ZERO(&w); FD_SET(s, &w); timeval tv{4, 0};
            if (select(0, nullptr, &w, nullptr, &tv) > 0) {
                nb = 0; ioctlsocket(s, FIONBIO, &nb);
                { std::lock_guard<std::mutex> l(g_partyM); g_partySock = s; }
                PartyHello();
                std::thread(PartyRead, s, gen).detach();
                PartyUi();
            } else closesocket(s);
        }
        for (int i = 0; i < 3 && g_partyGen == gen; i++) Sleep(1000);
    }
}
static void PartyStop() {
    g_partyGen++;
    { std::lock_guard<std::mutex> l(g_partyM); if (g_partySock != INVALID_SOCKET) { send(g_partySock, "BYE\n", 4, 0); closesocket(g_partySock); g_partySock = INVALID_SOCKET; }
      g_peerName.clear(); g_peerReady = false; g_meReady = false; g_partyStarted = false; }
    g_partyRole = 0;
    PartyUi();
}
// a game ended: everyone is "not ready" again for the next one (the link stays)
static void PartyAfterGame() {
    if (!g_partyRole) return;
    { std::lock_guard<std::mutex> l(g_partyM); g_meReady = false; g_partyStarted = false; }
    PartySend("READY 0");
    PartyUi();
}
static void PartyStartHost() { PartyStop(); g_partyRole = 1; uint32_t gen = g_partyGen; std::thread(PartyHostThread, gen).detach(); PartyUi(); }
static void PartyStartJoin(uint32_t ip) { PartyStop(); g_partyRole = 2; uint32_t gen = g_partyGen; std::thread(PartyJoinThread, ip, gen).detach(); PartyUi(); }
// the "who / ready" line and the Ready button text
static void PartyShow() {
    std::wstring t, me = OnlineGet(L"gamertag");
    std::wstring peer; bool pr, mr, conn;
    { std::lock_guard<std::mutex> l(g_partyM); peer = g_peerName; pr = g_peerReady; mr = g_meReady; conn = g_partySock != INVALID_SOCKET; }
    int role = g_partyRole;
    if (!role) t = L"Host a game or join one with a code to play a friend.";
    else if (!conn) t = role == 1 ? L"Waiting for your friend to join with your code..." : L"Connecting to your friend... (they need to have pressed Host a game)";
    else {
        std::wstring other = peer.empty() ? L"your friend" : peer;
        t = L"Connected:  " + me + (role == 1 ? L" (host) " : L" ") + (mr ? L"- READY" : L"- not ready") + L"     " + other + (role == 2 ? L" (host) " : L" ") + (pr ? L"- READY" : L"- not ready");
        if (mr && pr) t += role == 1 ? L"     Starting..." : L"     Starting when the host's game is up...";
        else if (!mr) t += L"     Press READY when you are.";
        else t += L"     Waiting for " + other + L" to press READY.";
    }
    SetWindowTextW(g_oParty, t.c_str());
    SetWindowTextW(GetDlgItem(g_wnd, ID_O_READY), mr ? L"NOT READY" : L"READY");
    EnableWindow(GetDlgItem(g_wnd, ID_O_READY), conn);
}
static void OnParty(WPARAM what) {
    if (what == 1) { OnlineStatus(L"Both ready - starting the game (you host)."); if (!g_proc) OnPlay(true); }
    if (what == 2) {   // the friend: give the host's game a head start so its lobby is open
        OnlineStatus(L"Both ready - your game starts in a few seconds...");
        SetTimer(g_wnd, 10, 4000, nullptr);
    }
    PartyShow();
}
static void HostReady(HostResult* r) {
    if (!r->pub) { OnlineStatus(L"Couldn't find your internet address - check your connection and press Host again."); delete r; return; }
    CreateDirectoryW(OnlineDir().c_str(), nullptr);
    bc::WriteText(OnlineDir() + L"\\online_host.txt", "public=" + bc::N(IpW(r->pub)) + "\nlan=" + bc::N(IpW(r->lan)) + "\n");
    DeleteFileW((OnlineDir() + L"\\online_server.txt").c_str());
    g_myCode = CodeFromIp(r->pub);
    WritePrivateProfileStringW(L"online", L"hostcode", g_myCode.c_str(), LauncherIni().c_str());
    // the router's own internet address differs from the one the world sees: the internet provider shares one
    // address among many homes (CGNAT) - nobody outside can connect in, whatever the router does
    bool shared = r->routerIp && r->pub && r->routerIp != r->pub;
    std::wstring st = shared
        ? L"Hosting is set up, but your router sits behind another router or your internet provider shares your address (CGNAT), so your friend most "
          L"likely can't connect to you. Let your friend host instead (you join with their code)."
        : r->opened >= 3
        ? L"Hosting. Your router opened the ports automatically. Send your friend the code, then press PLAY ONLINE (allow it if Windows Firewall asks) - your friend joins with the code."
        : L"Hosting, but your router didn't open the ports automatically (UPnP is off or not supported). Your friend may not be able to connect - "
          L"in your router's settings, forward TCP 25860-25861 and UDP 3074 to " + IpW(r->lan) + L", or turn on UPnP.";
    OnlineStatus(st);
    RefreshOnlinePage();
    PartyStartHost();
    delete r;
}

static HWND OLabel(HWND p, const wchar_t* t, HFONT f) { HWND h = CreateWindowW(L"STATIC", t, WS_CHILD, 0, 0, 0, 0, p, nullptr, nullptr, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); g_onlineCtl.push_back(h); return h; }
static HWND OButton(HWND p, const wchar_t* t, int id, HFONT f, DWORD style = 0) { HWND h = CreateWindowW(L"BUTTON", t, WS_CHILD | style, 0, 0, 0, 0, p, (HMENU)(INT_PTR)id, nullptr, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); g_onlineCtl.push_back(h); return h; }
static HWND OEdit(HWND p, int id, HFONT f, int limit) { HWND h = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, p, (HMENU)(INT_PTR)id, nullptr, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(h, EM_LIMITTEXT, limit, 0); g_onlineCtl.push_back(h); return h; }

static void CreateOnlinePage(HWND hw, HFONT f) {
    g_sGtagLabel = OLabel(hw, L"Gamertag", f);
    g_sGtag = OEdit(hw, ID_O_GTAG, f, 15);
    g_sOnline = OButton(hw, L"Play online (Xbox Live)", ID_O_ONLINE, f, BS_AUTOCHECKBOX);
    OButton(hw, L"Save", ID_O_GTSAVE, f);
    g_sGtInfo = OLabel(hw, L"", f);
    OButton(hw, L"PLAY ONLINE", ID_O_PLAY, f);
    g_oHead2 = OLabel(hw, L"PLAY A FRIEND", f);
    g_oHostLbl = OLabel(hw, L"Host", f);
    OButton(hw, L"Host a game", ID_O_HOST, f);
    g_oCodeShow = OLabel(hw, L"Not hosting.", f);
    OButton(hw, L"Copy code", ID_O_COPY, f);
    g_oJoinLbl = OLabel(hw, L"Join", f);
    g_oCodeEdit = OEdit(hw, ID_O_CODE, f, 12);
    OButton(hw, L"Join", ID_O_JOIN, f);
    OButton(hw, L"Stop hosting / joining", ID_O_STOP, f);
    g_oStatus = OLabel(hw, L"", f);
    g_oWho = OLabel(hw, L"", f);
    g_oParty = OLabel(hw, L"", f);
    OButton(hw, L"READY", ID_O_READY, f);
    g_oHelp = OLabel(hw, L"One of you presses Host a game and sends the join code; the other types it under Join. You'll both see each other here - press READY, and "
                         L"when both are ready the games start (the host's first). Then go to "
                         L"Play Now > Xbox Live and pick Quick Match. Every pair of players can host their own game this way - "
                         L"nobody else can get into yours without your code. The host's PC has to stay on while you play.", f);
    std::wstring code; wchar_t b[32] = L""; GetPrivateProfileStringW(L"online", L"hostcode", L"", b, 32, LauncherIni().c_str()); g_myCode = b;
    // hosting / joining from last time: reconnect the ready-up link
    if (GetFileAttributesW((OnlineDir() + L"\\online_host.txt").c_str()) != INVALID_FILE_ATTRIBUTES) PartyStartHost();
    else { std::string j = bc::Trim(bc::ReadText(OnlineDir() + L"\\online_server.txt")); unsigned a1, a2, a3, a4;
           if (sscanf(j.c_str(), "%u.%u.%u.%u", &a1, &a2, &a3, &a4) == 4) PartyStartJoin(a1 << 24 | a2 << 16 | a3 << 8 | a4); }
}
static void LayoutOnlinePage(int x, int y, int right, int bottom) {
    MoveWindow(g_sGtagLabel, x, y + 5, 160, 20, TRUE);
    MoveWindow(g_sGtag, x + 180, y, 220, 28, TRUE);
    MoveWindow(g_sOnline, x + 416, y + 2, 230, 24, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_O_GTSAVE), x + 656, y - 1, 90, 30, TRUE); y += 36;
    MoveWindow(g_sGtInfo, x + 180, y, right - x - 180, 20, TRUE); y += 28;
    MoveWindow(GetDlgItem(g_wnd, ID_O_PLAY), x + 180, y, 220, 36, TRUE); y += 52;
    MoveWindow(g_oHead2, x, y, 300, 20, TRUE); y += 32;
    MoveWindow(g_oHostLbl, x, y + 7, 160, 20, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_O_HOST), x + 180, y, 150, 34, TRUE);
    MoveWindow(g_oCodeShow, x + 346, y + 7, 330, 20, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_O_COPY), x + 686, y, 110, 34, TRUE); y += 46;
    MoveWindow(g_oJoinLbl, x, y + 7, 160, 20, TRUE);
    MoveWindow(g_oCodeEdit, x + 180, y + 3, 150, 28, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_O_JOIN), x + 346, y, 110, 34, TRUE);
    MoveWindow(GetDlgItem(g_wnd, ID_O_STOP), x + 470, y, 220, 34, TRUE); y += 50;
    MoveWindow(GetDlgItem(g_wnd, ID_O_READY), x, y, 150, 36, TRUE);
    MoveWindow(g_oParty, x + 166, y + 2, right - x - 166, 40, TRUE); y += 48;
    MoveWindow(g_oWho, x, y, right - x, 22, TRUE); y += 30;
    MoveWindow(g_oStatus, x, y, right - x, 60, TRUE); y += 66;
    MoveWindow(g_oHelp, x, y, right - x, std::max(40, bottom - y), TRUE);
}
static void ShowOnlinePage(bool show) {
    for (HWND c : g_onlineCtl) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
    if (show) { RefreshOnlinePage(); PollWho(); PartyShow(); SetTimer(g_wnd, 9, 3000, nullptr); } else KillTimer(g_wnd, 9);
}
static bool OnlineCommand(WORD id, WORD code) {
    switch (id) {
    case ID_O_GTSAVE: SaveOnline(); return true;
    case ID_O_READY:
        { std::lock_guard<std::mutex> l(g_partyM); g_meReady = !g_meReady; }
        PartySend(std::string("READY ") + (g_meReady ? "1" : "0"));
        PartyMaybeStart();
        PartyShow();
        return true;
    case ID_O_PLAY:   // the game signed in to Xbox Live (PLAY on the Play page is always an offline game)
        if (OnlineGet(L"gamertag").empty() || OnlineGet(L"enabled") != L"1") { OnlineStatus(L"Pick a gamertag, tick Play online and press Save first."); return true; }
        OnPlay(true);
        return true;
    case ID_O_GTAG: case ID_O_ONLINE: case ID_O_CODE: return true;
    case ID_O_HOST:
        if (OnlineGet(L"gamertag").empty() || OnlineGet(L"enabled") != L"1") { OnlineStatus(L"Pick a gamertag, tick Play online and press Save first."); return true; }
        OnlineStatus(L"Setting up hosting: asking your router to let your friends in and finding your internet address...");
        std::thread(HostThread).detach();
        return true;
    case ID_O_COPY:
        if (!g_myCode.empty() && OpenClipboard(g_wnd)) {
            EmptyClipboard();
            size_t n = (g_myCode.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(h), g_myCode.c_str(), n); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
            CloseClipboard();
            OnlineStatus(L"Join code copied - paste it to your friend.");
        }
        return true;
    case ID_O_JOIN: {
        if (OnlineGet(L"gamertag").empty() || OnlineGet(L"enabled") != L"1") { OnlineStatus(L"Pick a gamertag, tick Play online and press Save first."); return true; }
        wchar_t b[32] = L""; GetWindowTextW(g_oCodeEdit, b, 32);
        uint32_t ip;
        if (!IpFromCode(b, ip)) { OnlineStatus(L"That code doesn't look right - it's 8 letters and numbers, like 7K3Q-9XPD."); return true; }
        CreateDirectoryW(OnlineDir().c_str(), nullptr);
        bc::WriteText(OnlineDir() + L"\\online_server.txt", bc::N(IpW(ip)) + " " + std::to_string(kLobbyPort) + "\n");
        DeleteFileW((OnlineDir() + L"\\online_host.txt").c_str());
        RefreshOnlinePage();
        PartyStartJoin(ip);
        OnlineStatus(L"Connecting to your friend...");
        return true; }
    case ID_O_STOP:
        PartyStop();
        DeleteFileW((OnlineDir() + L"\\online_host.txt").c_str());
        DeleteFileW((OnlineDir() + L"\\online_server.txt").c_str());
        OnlineStatus(L"Stopped. Your game uses its own private lobby again.");
        RefreshOnlinePage();
        return true;
    }
    return false;
}
