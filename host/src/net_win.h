// Host sockets for the game's network calls (see net_win.cpp). Addresses/ports in host byte order.
#pragma once
#include <cstdint>
#include <vector>
namespace hn {
bool Init();
int LastError();
int Socket(int af, int type, int proto);
int Close(int s);
int Shutdown(int s, int how);
int SetOpt(int s, int level, int opt, const void* v, int len);
int GetOpt(int s, int level, int opt, void* v, int* len);
int Ioctl(int s, uint32_t cmd, uint32_t* arg);
int Bind(int s, uint32_t ip, uint16_t port);
int Listen(int s, int backlog);
int Connect(int s, uint32_t ip, uint16_t port);
int Send(int s, const void* b, int n, int flags);
int Recv(int s, void* b, int n, int flags);
int SendTo(int s, const void* b, int n, int flags, uint32_t ip, uint16_t port);
int RecvFrom(int s, void* b, int n, int flags, uint32_t* ip, uint16_t* port);
int SockName(int s, uint32_t* ip, uint16_t* port);
int PeerName(int s, uint32_t* ip, uint16_t* port);
int Accept(int s, uint32_t* ip, uint16_t* port);
int Select(std::vector<int>* rd, std::vector<int>* wr, std::vector<int>* ex, int timeoutMs);   // keeps the ready ones
bool Resolve(const char* name, std::vector<uint32_t>& out);
uint32_t LocalIp();
}
