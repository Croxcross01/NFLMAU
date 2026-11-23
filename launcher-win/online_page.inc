// Online page: your gamertag, and playing a friend over the internet in the game's Xbox Live mode.
//   Host: your game runs the online lobby for your friends. The launcher asks your router to let them in (UPnP),
//         finds your internet address and turns it into a join code. Writes mods/online_host.txt ("public=<ip>").
//   Join: type a friend's join code. Writes mods/online_server.txt ("<ip> 25860") - your game logs in to their lobby.
// Neither: your game uses its own private lobby (you can look around Xbox Live mode on your own).
// Included by launcher.cpp after settings_page.inc (which has the gamertag helpers).
#include <winsock2.h>
#include <ws2tcpip.h>

enum { ID_O_GTAG = 600, ID_O_ONLINE, ID_O_GTSAVE, ID_O_HOST, ID_O_STOP, ID_O_COPY, ID_O_CODE, ID_O_JOIN, ID_O_PLAY, WM_HOST_READY = WM_APP + 60, WM_JOIN_CHECKED };
static HWND g_oHead1, g_oHead2, g_oHostLbl, g_oJoinLbl, g_oCodeShow, g_oCodeEdit, g_oStatus, g_oHelp;
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
    struct P { long port; const wchar_t* proto; } ports[] = {{25860, L"TCP"}, {25861, L"TCP"}, {3074, L"UDP"}, {3074, L"TCP"}};
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
        OnlineStatus(L"Joining a friend (code " + CodeFromIp(ip) + L"). Start the game and go to Play Now > Xbox Live.");
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
static void JoinChecked(bool ok, uint32_t ip) {
    OnlineStatus(ok ? L"Your friend's game answered (code " + CodeFromIp(ip) + L"). Press PLAY ONLINE, then go to Play Now > Xbox Live > Quick Match."
                    : L"Set to join code " + CodeFromIp(ip) + L", but their game didn't answer yet. Make sure your friend pressed PLAY ONLINE "
                      L"while hosting (and allowed it through Windows Firewall), then press Join again to re-check.");
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
    g_oHelp = OLabel(hw, L"Online games start with PLAY ONLINE (the Play page's PLAY is always an offline game). One of you hosts and sends the join code; the other "
                         L"types it under Join. Then both press PLAY ONLINE and go to "
                         L"Play Now > Xbox Live and pick Quick Match. Every pair of players can host their own game this way - "
                         L"nobody else can get into yours without your code. The host's PC has to stay on while you play.", f);
    std::wstring code; wchar_t b[32] = L""; GetPrivateProfileStringW(L"online", L"hostcode", L"", b, 32, LauncherIni().c_str()); g_myCode = b;
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
    MoveWindow(g_oStatus, x, y, right - x, 60, TRUE); y += 70;
    MoveWindow(g_oHelp, x, y, right - x, std::max(40, bottom - y), TRUE);
}
static void ShowOnlinePage(bool show) { for (HWND c : g_onlineCtl) ShowWindow(c, show ? SW_SHOW : SW_HIDE); if (show) RefreshOnlinePage(); }
static bool OnlineCommand(WORD id, WORD code) {
    switch (id) {
    case ID_O_GTSAVE: SaveOnline(); return true;
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
        OnlineStatus(L"Checking that your friend's game can be reached...");
        std::thread([ip] { PostMessageW(g_wnd, WM_JOIN_CHECKED, CanReach(ip, kLobbyPort) ? 1 : 0, (LPARAM)ip); }).detach();
        return true; }
    case ID_O_STOP:
        DeleteFileW((OnlineDir() + L"\\online_host.txt").c_str());
        DeleteFileW((OnlineDir() + L"\\online_server.txt").c_str());
        OnlineStatus(L"Stopped. Your game uses its own private lobby again.");
        RefreshOnlinePage();
        return true;
    }
    return false;
}
