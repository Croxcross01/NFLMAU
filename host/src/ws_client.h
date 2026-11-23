// A small WebSocket client on Windows' own WinHTTP (loaded at run time, nothing extra to link). Used by the launcher
// (ready-up) and the game (the match) to talk to the relay server. One thread receives; sending is thread-safe.
#pragma once
#include <windows.h>
#include <winhttp.h>
#include <mutex>
#include <string>
#include <vector>

namespace wsc {

struct Api {
    decltype(&::WinHttpOpen) Open; decltype(&::WinHttpConnect) Connect; decltype(&::WinHttpOpenRequest) OpenRequest;
    decltype(&::WinHttpSetOption) SetOption; decltype(&::WinHttpSendRequest) SendRequest; decltype(&::WinHttpReceiveResponse) ReceiveResponse;
    decltype(&::WinHttpQueryHeaders) QueryHeaders; decltype(&::WinHttpCloseHandle) Close; decltype(&::WinHttpCrackUrl) CrackUrl;
    HINTERNET(WINAPI* Upgrade)(HINTERNET, DWORD_PTR);
    DWORD(WINAPI* WsSend)(HINTERNET, int, PVOID, DWORD);
    DWORD(WINAPI* WsReceive)(HINTERNET, PVOID, DWORD, DWORD*, int*);
    DWORD(WINAPI* WsClose)(HINTERNET, USHORT, PVOID, DWORD);
    bool ok = false;
};
inline Api& A() {
    static Api a = [] {
        Api x{};
        HMODULE m = LoadLibraryW(L"winhttp.dll");
        if (!m) return x;
        auto g = [&](const char* n) { return (void*)GetProcAddress(m, n); };
        x.Open = (decltype(x.Open))g("WinHttpOpen"); x.Connect = (decltype(x.Connect))g("WinHttpConnect");
        x.OpenRequest = (decltype(x.OpenRequest))g("WinHttpOpenRequest"); x.SetOption = (decltype(x.SetOption))g("WinHttpSetOption");
        x.SendRequest = (decltype(x.SendRequest))g("WinHttpSendRequest"); x.ReceiveResponse = (decltype(x.ReceiveResponse))g("WinHttpReceiveResponse");
        x.QueryHeaders = (decltype(x.QueryHeaders))g("WinHttpQueryHeaders"); x.Close = (decltype(x.Close))g("WinHttpCloseHandle");
        x.CrackUrl = (decltype(x.CrackUrl))g("WinHttpCrackUrl");
        x.Upgrade = (decltype(x.Upgrade))g("WinHttpWebSocketCompleteUpgrade"); x.WsSend = (decltype(x.WsSend))g("WinHttpWebSocketSend");
        x.WsReceive = (decltype(x.WsReceive))g("WinHttpWebSocketReceive"); x.WsClose = (decltype(x.WsClose))g("WinHttpWebSocketClose");
        x.ok = x.Open && x.Connect && x.OpenRequest && x.SetOption && x.SendRequest && x.ReceiveResponse && x.Close && x.CrackUrl &&
               x.Upgrade && x.WsSend && x.WsReceive && x.WsClose && x.QueryHeaders;
        return x;
    }();
    return a;
}

class Client {
public:
    ~Client() { Close(); }
    // url: https://host/path or http://host:port/path (the WebSocket upgrade is done on it)
    bool Connect(const std::wstring& url, int timeoutMs = 15000) {
        Api& a = A(); if (!a.ok) return false;
        URL_COMPONENTS c{}; c.dwStructSize = sizeof c;
        wchar_t host[256] = L"", path[1024] = L"";
        c.lpszHostName = host; c.dwHostNameLength = 256; c.lpszUrlPath = path; c.dwUrlPathLength = 1024; c.dwSchemeLength = (DWORD)-1;
        if (!a.CrackUrl(url.c_str(), 0, 0, &c)) return false;
        std::wstring full = std::wstring(path) + (c.lpszExtraInfo && c.dwExtraInfoLength ? std::wstring(c.lpszExtraInfo, c.dwExtraInfoLength) : L"");
        ses_ = a.Open(L"MaddenArcadeUnleashed/1.0", 4 /*WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY*/, nullptr, nullptr, 0);
        if (!ses_) ses_ = a.Open(L"MaddenArcadeUnleashed/1.0", 0, nullptr, nullptr, 0);
        if (!ses_) return false;
        DWORD to = (DWORD)timeoutMs;
        a.SetOption(ses_, WINHTTP_OPTION_CONNECT_TIMEOUT, &to, sizeof to);
        a.SetOption(ses_, WINHTTP_OPTION_SEND_TIMEOUT, &to, sizeof to);
        a.SetOption(ses_, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &to, sizeof to);
        con_ = a.Connect(ses_, host, c.nPort, 0);
        if (!con_) { Close(); return false; }
        HINTERNET req = a.OpenRequest(con_, L"GET", full.c_str(), nullptr, nullptr, nullptr, c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { Close(); return false; }
        bool ok = a.SetOption(req, 114 /*WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET*/, nullptr, 0) &&
                  a.SendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && a.ReceiveResponse(req, nullptr);
        DWORD status = 0, len = sizeof status;
        if (ok) a.QueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
        if (ok && status == 101) ws_ = a.Upgrade(req, 0);
        a.Close(req);
        if (!ws_) { Close(); return false; }
        DWORD inf = 0;   // receiving waits as long as it takes (the relay pings every 20 s)
        a.SetOption(ws_, WINHTTP_OPTION_RECEIVE_TIMEOUT, &inf, sizeof inf);
        return true;
    }
    bool Send(const void* data, size_t n) {
        std::lock_guard<std::mutex> l(sendM_);
        return ws_ && A().WsSend(ws_, 0 /*binary message*/, (PVOID)data, (DWORD)n) == 0;
    }
    // One whole message. text = true for the relay's own notices ("peer:up" / "peer:down"). false = closed.
    bool Receive(std::string& msg, bool& text) {
        msg.clear(); if (!ws_) return false;
        char buf[16384];
        for (;;) {
            DWORD got = 0; int type = 0;
            if (A().WsReceive(ws_, buf, sizeof buf, &got, &type) != 0) return false;
            if (type == 4 /*close*/) return false;
            msg.append(buf, got);
            if (type == 0 || type == 2) { text = type == 2; return true; }   // binary / utf8 message complete
        }
    }
    void Close() {
        Api& a = A();
        if (ws_) { a.WsClose(ws_, 1000, nullptr, 0); a.Close(ws_); ws_ = nullptr; }
        if (con_) { a.Close(con_); con_ = nullptr; }
        if (ses_) { a.Close(ses_); ses_ = nullptr; }
    }
    bool Open() const { return ws_ != nullptr; }
private:
    HINTERNET ses_ = nullptr, con_ = nullptr, ws_ = nullptr;
    std::mutex sendM_;
};

// GET the relay's /health (wakes a sleeping free server; up to ~60 s). True when it answered.
inline bool Wake(const std::wstring& base) {
    Client c; (void)c;
    Api& a = A(); if (!a.ok) return false;
    URL_COMPONENTS u{}; u.dwStructSize = sizeof u; wchar_t host[256] = L"";
    u.lpszHostName = host; u.dwHostNameLength = 256; u.dwSchemeLength = (DWORD)-1; u.dwUrlPathLength = (DWORD)-1;
    if (!a.CrackUrl(base.c_str(), 0, 0, &u)) return false;
    HINTERNET s = a.Open(L"MaddenArcadeUnleashed/1.0", 4, nullptr, nullptr, 0); if (!s) s = a.Open(L"MaddenArcadeUnleashed/1.0", 0, nullptr, nullptr, 0);
    if (!s) return false;
    DWORD to = 70000; a.SetOption(s, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &to, sizeof to);
    bool ok = false;
    if (HINTERNET cn = a.Connect(s, host, u.nPort, 0)) {
        if (HINTERNET r = a.OpenRequest(cn, L"GET", L"/health", nullptr, nullptr, nullptr, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)) {
            if (a.SendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && a.ReceiveResponse(r, nullptr)) {
                DWORD st = 0, len = sizeof st; a.QueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &st, &len, nullptr);
                ok = st == 200;
            }
            a.Close(r);
        }
        a.Close(cn);
    }
    a.Close(s);
    return ok;
}

}   // namespace wsc
