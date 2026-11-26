// Madden Arcade Unleashed's own overall formula, shared by the game's roster builder and the dev tools.
// A player's overall is a weighted mix of the ratings that matter for his position (the project's own weights), put on
// the game's scale by a straight-line fit against the original game's players at that position (so a 90 here feels
// like a 90 in this game). Fields are the game's PLAY table rating fields.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ovr {

struct W { const char* field; double w; };

// Position ids: 0 QB 1 HB 2 FB 3 WR 4 TE 5-9 OL 10 LE 11 RE 12 DT 13 LOLB 14 MLB 15 ROLB 16 CB 17 FS 18 SS 19 K 20 P
inline const std::vector<W>& Weights(int pos) {
    static const std::vector<W> qb = {{"PTHA", 0.24}, {"PTHP", 0.18}, {"PAWR", 0.22}, {"PTAD", 0.08}, {"PTOR", 0.08}, {"PPLA", 0.05},
                                      {"PSPD", 0.05}, {"PAGI", 0.04}, {"PACC", 0.03}, {"PSTR", 0.03}};
    static const std::vector<W> hb = {{"PSPD", 0.18}, {"PACC", 0.13}, {"PAGI", 0.10}, {"PBCV", 0.12}, {"PCAR", 0.10}, {"PELU", 0.09},
                                      {"PLTR", 0.06}, {"PCTH", 0.07}, {"PAWR", 0.10}, {"PSTR", 0.05}};
    static const std::vector<W> fb = {{"PRBK", 0.22}, {"PPBK", 0.12}, {"PSTR", 0.16}, {"PCAR", 0.10}, {"PCTH", 0.10}, {"PAWR", 0.12},
                                      {"PLIB", 0.10}, {"PSPD", 0.08}};
    static const std::vector<W> wr = {{"PSPD", 0.18}, {"PCTH", 0.18}, {"PLRR", 0.17}, {"PCLI", 0.09}, {"PACC", 0.09}, {"PAGI", 0.06},
                                      {"PLRL", 0.06}, {"PAWR", 0.10}, {"PLSC", 0.04}, {"PJMP", 0.03}};
    static const std::vector<W> te = {{"PCTH", 0.17}, {"PRBK", 0.14}, {"PPBK", 0.06}, {"PSPD", 0.12}, {"PLRR", 0.12}, {"PSTR", 0.10},
                                      {"PAWR", 0.12}, {"PCLI", 0.09}, {"PACC", 0.05}, {"PLIB", 0.03}};
    static const std::vector<W> ol = {{"PPBK", 0.27}, {"PRBK", 0.27}, {"PSTR", 0.20}, {"PAWR", 0.14}, {"PLIB", 0.05}, {"PAGI", 0.04}, {"PACC", 0.03}};
    static const std::vector<W> dl = {{"PBSG", 0.20}, {"PLPm", 0.13}, {"PFMS", 0.13}, {"PSTR", 0.14}, {"PLPU", 0.10}, {"PTAK", 0.09},
                                      {"PACC", 0.08}, {"PSPD", 0.05}, {"PAWR", 0.08}};
    static const std::vector<W> lb = {{"PTAK", 0.18}, {"PLPU", 0.14}, {"PLPE", 0.14}, {"PSPD", 0.10}, {"PBSG", 0.09}, {"PLZC", 0.09},
                                      {"PLMC", 0.05}, {"PLHT", 0.06}, {"PAWR", 0.10}, {"PACC", 0.05}};
    static const std::vector<W> cb = {{"PLMC", 0.20}, {"PLZC", 0.14}, {"PSPD", 0.18}, {"PACC", 0.09}, {"PAGI", 0.06}, {"PLPR", 0.08},
                                      {"PLPE", 0.10}, {"PAWR", 0.09}, {"PCTH", 0.03}, {"PJMP", 0.03}};
    static const std::vector<W> sf = {{"PLZC", 0.19}, {"PSPD", 0.14}, {"PLPE", 0.15}, {"PTAK", 0.13}, {"PLHT", 0.08}, {"PLMC", 0.09},
                                      {"PAWR", 0.13}, {"PLPU", 0.05}, {"PACC", 0.04}};
    static const std::vector<W> k = {{"PKPR", 0.5}, {"PKAC", 0.5}};
    static const std::vector<W> none;
    switch (pos) {
    case 0: return qb; case 1: return hb; case 2: return fb; case 3: return wr; case 4: return te;
    case 5: case 6: case 7: case 8: case 9: return ol;
    case 10: case 11: case 12: return dl;
    case 13: case 14: case 15: return lb;
    case 16: return cb; case 17: case 18: return sf;
    case 19: case 20: return k;
    }
    return none;
}

// Weighted score of one player (get(field) returns his rating).
inline double Score(int pos, const std::function<double(const char*)>& get) {
    double s = 0, tw = 0;
    for (auto& w : Weights(pos)) { s += w.w * get(w.field); tw += w.w; }
    return tw > 0 ? s / tw : 0;
}

// Fit: overall = a + b * score, per position, from players whose overall is known (the original game's own players).
struct Fit {
    std::map<int, std::pair<double, double>> ab;
    void Add(int pos, double score, double overall) { auto& v = pts[pos]; v.push_back({score, overall}); }
    void Solve() {
        for (auto& [pos, v] : pts) {
            double n = double(v.size()), sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (auto& [x, y] : v) { sx += x; sy += y; sxx += x * x; sxy += x * y; }
            double d = n * sxx - sx * sx;
            if (n >= 5 && d > 1e-6) { double b = (n * sxy - sx * sy) / d; ab[pos] = {(sy - b * sx) / n, b}; }
        }
        pts.clear();
    }
    int Overall(int pos, double score) const {
        auto it = ab.find(pos);
        double v = it == ab.end() ? score : it->second.first + it->second.second * score;
        return std::max(25, std::min(99, int(std::lround(v))));
    }
private:
    std::map<int, std::vector<std::pair<double, double>>> pts;
};

}   // namespace ovr
