// Host side of the game's network (Winsock, loaded at run time so nothing new has to be linked). Plain types only:
// net.cpp turns the game's calls (big-endian structures in guest memory) into these. Addresses and ports are in
// host byte order. Socket numbers given to the game are small integers (index into a table here).
#include "net_win.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mutex>
#include <cstdio>

namespace hn {
namespace {
inline uint16_t B16(uint16_t v) { return (uint16_t)(v >> 8 | v << 8); }
inline uint32_t B32(uint32_t v) { return __builtin_bswap32(v); }
struct Api {
    decltype(&::WSAStartup) startup; decltype(&::socket) socket_; decltype(&::closesocket) close; decltype(&::shutdown) shutdown_;
    decltype(&::setsockopt) setopt; decltype(&::getsockopt) getopt; decltype(&::ioctlsocket) ioctl; decltype(&::bind) bind_;
    decltype(&::connect) connect_; decltype(&::send) send_; decltype(&::recv) recv_; decltype(&::sendto) sendto_;
    decltype(&::recvfrom) recvfrom_; decltype(&::select) select_; decltype(&::getsockname) sockname; decltype(&::getpeername) peername;
    decltype(&::accept) accept_; decltype(&::listen) listen_; decltype(&::WSAGetLastError) lasterr; decltype(&::getaddrinfo) gai;
    decltype(&::freeaddrinfo) fai; decltype(&::gethostname) hostname; decltype(&::__WSAFDIsSet) isset;
} A;
bool g_ok = false;
std::mutex g_m;
SOCKET g_tab[1024];
thread_local int g_err = 0;

template <class T> void Get(HMODULE m, T& f, const char* n) { f = (T)GetProcAddress(m, n); if (!f) g_ok = false; }
SOCKET S(int s) { std::lock_guard<std::mutex> l(g_m); return s >= 0 && s < 1024 ? g_tab[s] : INVALID_SOCKET; }
int Err(int r) { if (r == SOCKET_ERROR) g_err = A.lasterr(); return r == SOCKET_ERROR ? -1 : r; }
sockaddr_in Addr(uint32_t ip, uint16_t port) { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = B16(port); a.sin_addr.s_addr = B32(ip); return a; }
}   // namespace

bool Init() {
    static bool done = [] {
        HMODULE m = LoadLibraryA("ws2_32.dll"); if (!m) return false;
        g_ok = true;
        Get(m, A.startup, "WSAStartup"); Get(m, A.socket_, "socket"); Get(m, A.close, "closesocket"); Get(m, A.shutdown_, "shutdown");
        Get(m, A.setopt, "setsockopt"); Get(m, A.getopt, "getsockopt"); Get(m, A.ioctl, "ioctlsocket"); Get(m, A.bind_, "bind");
        Get(m, A.connect_, "connect"); Get(m, A.send_, "send"); Get(m, A.recv_, "recv"); Get(m, A.sendto_, "sendto");
        Get(m, A.recvfrom_, "recvfrom"); Get(m, A.select_, "select"); Get(m, A.sockname, "getsockname"); Get(m, A.peername, "getpeername");
        Get(m, A.accept_, "accept"); Get(m, A.listen_, "listen"); Get(m, A.lasterr, "WSAGetLastError"); Get(m, A.gai, "getaddrinfo");
        Get(m, A.fai, "freeaddrinfo"); Get(m, A.hostname, "gethostname"); Get(m, A.isset, "__WSAFDIsSet");
        if (!g_ok) return false;
        WSADATA d; if (A.startup(MAKEWORD(2, 2), &d) != 0) { g_ok = false; return false; }
        for (auto& s : g_tab) s = INVALID_SOCKET;
        return true;
    }();
    return done;
}
int LastError() { return g_err; }

int Socket(int af, int type, int proto) {
    if (!Init()) { g_err = WSAENETDOWN; return -1; }
    SOCKET s = A.socket_(af, type, proto);
    if (s == INVALID_SOCKET) { g_err = A.lasterr(); return -1; }
    std::lock_guard<std::mutex> l(g_m);
    for (int i = 1; i < 1024; i++) if (g_tab[i] == INVALID_SOCKET) { g_tab[i] = s; return i; }
    A.close(s); g_err = WSAEMFILE; return -1;
}
int Close(int s) {
    SOCKET h;
    { std::lock_guard<std::mutex> l(g_m); if (s <= 0 || s >= 1024 || g_tab[s] == INVALID_SOCKET) { g_err = WSAENOTSOCK; return -1; } h = g_tab[s]; g_tab[s] = INVALID_SOCKET; }
    return Err(A.close(h));
}
int Shutdown(int s, int how) { return Err(A.shutdown_(S(s), how)); }
int SetOpt(int s, int level, int opt, const void* v, int len) { return Err(A.setopt(S(s), level, opt, (const char*)v, len)); }
int GetOpt(int s, int level, int opt, void* v, int* len) { return Err(A.getopt(S(s), level, opt, (char*)v, len)); }
int Ioctl(int s, uint32_t cmd, uint32_t* arg) { u_long a = *arg; int r = Err(A.ioctl(S(s), (long)cmd, &a)); *arg = (uint32_t)a; return r; }
int Bind(int s, uint32_t ip, uint16_t port) { auto a = Addr(ip, port); return Err(A.bind_(S(s), (sockaddr*)&a, sizeof a)); }
int Listen(int s, int backlog) { return Err(A.listen_(S(s), backlog)); }
int Connect(int s, uint32_t ip, uint16_t port) { auto a = Addr(ip, port); return Err(A.connect_(S(s), (sockaddr*)&a, sizeof a)); }
int Send(int s, const void* b, int n, int flags) { return Err(A.send_(S(s), (const char*)b, n, flags)); }
int Recv(int s, void* b, int n, int flags) { return Err(A.recv_(S(s), (char*)b, n, flags)); }
int SendTo(int s, const void* b, int n, int flags, uint32_t ip, uint16_t port) { auto a = Addr(ip, port); return Err(A.sendto_(S(s), (const char*)b, n, flags, (sockaddr*)&a, sizeof a)); }
int RecvFrom(int s, void* b, int n, int flags, uint32_t* ip, uint16_t* port) {
    sockaddr_in a{}; int l = sizeof a;
    int r = Err(A.recvfrom_(S(s), (char*)b, n, flags, (sockaddr*)&a, &l));
    if (r >= 0) { if (ip) *ip = B32(a.sin_addr.s_addr); if (port) *port = B16(a.sin_port); }
    return r;
}
static int Name(int r, sockaddr_in& a, uint32_t* ip, uint16_t* port) { if (r >= 0) { *ip = B32(a.sin_addr.s_addr); *port = B16(a.sin_port); } return r; }
int SockName(int s, uint32_t* ip, uint16_t* port) { sockaddr_in a{}; int l = sizeof a; return Name(Err(A.sockname(S(s), (sockaddr*)&a, &l)), a, ip, port); }
int PeerName(int s, uint32_t* ip, uint16_t* port) { sockaddr_in a{}; int l = sizeof a; return Name(Err(A.peername(S(s), (sockaddr*)&a, &l)), a, ip, port); }
int Accept(int s, uint32_t* ip, uint16_t* port) {
    sockaddr_in a{}; int l = sizeof a;
    SOCKET c = A.accept_(S(s), (sockaddr*)&a, &l);
    if (c == INVALID_SOCKET) { g_err = A.lasterr(); return -1; }
    if (ip) *ip = B32(a.sin_addr.s_addr); if (port) *port = B16(a.sin_port);
    std::lock_guard<std::mutex> lk(g_m);
    for (int i = 1; i < 1024; i++) if (g_tab[i] == INVALID_SOCKET) { g_tab[i] = c; return i; }
    A.close(c); g_err = WSAEMFILE; return -1;
}
int Select(std::vector<int>* rd, std::vector<int>* wr, std::vector<int>* ex, int timeoutMs) {
    fd_set f[3]; std::vector<int>* v[3] = {rd, wr, ex};
    for (int k = 0; k < 3; k++) { FD_ZERO(&f[k]); if (v[k]) for (int s : *v[k]) { SOCKET h = S(s); if (h != INVALID_SOCKET && f[k].fd_count < FD_SETSIZE) f[k].fd_array[f[k].fd_count++] = h; } }
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int r = Err(A.select_(0, rd ? &f[0] : nullptr, wr ? &f[1] : nullptr, ex ? &f[2] : nullptr, timeoutMs < 0 ? nullptr : &tv));
    if (r < 0) return r;
    for (int k = 0; k < 3; k++) if (v[k]) { std::vector<int> keep; for (int s : *v[k]) { SOCKET h = S(s); if (h != INVALID_SOCKET && A.isset(h, &f[k])) keep.push_back(s); } *v[k] = keep; }
    return r;
}
bool Resolve(const char* name, std::vector<uint32_t>& out) {
    if (!Init()) return false;
    addrinfo hint{}; hint.ai_family = AF_INET; addrinfo* res = nullptr;
    if (A.gai(name, nullptr, &hint, &res) != 0) return false;
    for (addrinfo* p = res; p; p = p->ai_next) out.push_back(B32(((sockaddr_in*)p->ai_addr)->sin_addr.s_addr));
    A.fai(res);
    return !out.empty();
}
uint32_t LocalIp() {   // the address other PCs on the network reach this one at (the route to the internet)
    if (!Init()) return 0x7F000001;
    SOCKET s = A.socket_(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s == INVALID_SOCKET) return 0x7F000001;
    auto a = Addr(0x08080808, 53); uint32_t ip = 0x7F000001;
    if (A.connect_(s, (sockaddr*)&a, sizeof a) == 0) { sockaddr_in m{}; int l = sizeof m; if (A.sockname(s, (sockaddr*)&m, &l) == 0) ip = B32(m.sin_addr.s_addr); }
    A.close(s);
    return ip;
}
}   // namespace hn
#else
namespace hn {
bool Init() { return false; }
int LastError() { return 10050; }
int Socket(int, int, int) { return -1; }
int Close(int) { return -1; }
int Shutdown(int, int) { return -1; }
int SetOpt(int, int, int, const void*, int) { return -1; }
int GetOpt(int, int, int, void*, int*) { return -1; }
int Ioctl(int, uint32_t, uint32_t*) { return -1; }
int Bind(int, uint32_t, uint16_t) { return -1; }
int Listen(int, int) { return -1; }
int Connect(int, uint32_t, uint16_t) { return -1; }
int Send(int, const void*, int, int) { return -1; }
int Recv(int, void*, int, int) { return -1; }
int SendTo(int, const void*, int, int, uint32_t, uint16_t) { return -1; }
int RecvFrom(int, void*, int, int, uint32_t*, uint16_t*) { return -1; }
int SockName(int, uint32_t*, uint16_t*) { return -1; }
int PeerName(int, uint32_t*, uint16_t*) { return -1; }
int Accept(int, uint32_t*, uint16_t*) { return -1; }
int Select(std::vector<int>*, std::vector<int>*, std::vector<int>*, int) { return -1; }
bool Resolve(const char*, std::vector<uint32_t>&) { return false; }
uint32_t LocalIp() { return 0x7F000001; }
}
#endif
