// Thread load in the log: every ~20 s the busiest threads and how much of one CPU core each used ("[perf] threads:").
// Shows whether the game logic, the GPU command processor, the renderer or audio is what holds a frame back.
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
std::mutex g_mx;
std::map<unsigned long, std::string> g_names;   // thread id -> label
}

void PerfName(const std::string& name) {
#ifdef _WIN32
    std::lock_guard<std::mutex> l(g_mx); g_names[GetCurrentThreadId()] = name;
#else
    (void)name;
#endif
}

void PerfStart() {
#ifdef _WIN32
    static bool started = false; if (started) return; started = true;
    std::thread([] {
        std::map<unsigned long, unsigned long long> last;
        auto tsec = std::chrono::steady_clock::now();
        for (int round = 0; round < 400; round++) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            auto now = std::chrono::steady_clock::now();
            double wall = std::chrono::duration<double>(now - tsec).count(); tsec = now;
            std::vector<std::pair<double, std::string>> rows;
            std::map<unsigned long, std::string> names; { std::lock_guard<std::mutex> l(g_mx); names = g_names; }
            for (auto& [tid, name] : names) {
                HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
                if (!h) continue;
                FILETIME c, e, k, u;
                if (GetThreadTimes(h, &c, &e, &k, &u)) {
                    unsigned long long t = (((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime);
                    auto it = last.find(tid);
                    if (it != last.end()) rows.push_back({(t - it->second) / 1e7 / wall * 100.0, name});
                    last[tid] = t;
                }
                CloseHandle(h);
            }
            if (rows.empty()) continue;
            std::sort(rows.rbegin(), rows.rend());
            std::string out;
            for (size_t i = 0; i < rows.size() && i < 8; i++) { char b[96]; snprintf(b, sizeof b, "%s%s %.0f%%", i ? ", " : "", rows[i].second.c_str(), rows[i].first); out += b; }
            fprintf(stderr, "[perf] threads (%% of one core): %s\n", out.c_str());
        }
    }).detach();
#endif
}
