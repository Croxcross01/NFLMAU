// Builds mods/lines7.bin (the 7-on-7 alignment patch, see LoadAlign in mod_hooks.cpp) from the player's own game files
// the first time it is needed: the defensive playbook (qkl_fe2ig 260) with the ends moved inside, the offensive one
// (qkl_boot 82) with the tackles at the guard spots, stored as anchored regions of changed bytes. Same output as the
// dev tool make_lines_patch.py. Nothing from the game is in this repository.
#include "gamedb.h"
#include <set>
#include <cmath>

namespace {
float U2F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
uint32_t F2U(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

std::vector<uint8_t> Clamp(const std::vector<uint8_t>& data, std::set<uint32_t> epos, float maxX, std::set<std::string> skip) {
    gdb::Db db; if (!db.Load(data)) return data;
    gdb::Table *F = db.T("FORM"), *SL = db.T("SETL"), *SP = db.T("SETP"), *SG = db.T("SETG");
    if (!F || !SL || !SP || !SG) return data;
    std::map<uint32_t, std::string> form; for (int i = 0; i < F->used; i++) form[db.Get(*F, i, "FORM")] = db.Str(*F, i, "name");
    std::map<uint32_t, uint32_t> setForm; for (int i = 0; i < SL->used; i++) setForm[db.Get(*SL, i, "SETL")] = db.Get(*SL, i, "FORM");
    std::set<uint32_t> slots;
    for (int i = 0; i < SP->used; i++) {
        if (!epos.count(db.Get(*SP, i, "EPos"))) continue;
        auto sf = setForm.find(db.Get(*SP, i, "SETL")); std::string fname = sf != setForm.end() && form.count(sf->second) ? form[sf->second] : "";
        if (!skip.count(fname)) slots.insert(db.Get(*SP, i, "SETP"));
    }
    for (int i = 0; i < SG->used; i++) {
        if (!slots.count(db.Get(*SG, i, "SETP"))) continue;
        for (const char* fld : {"x___", "fx__"}) {
            float x = U2F(db.Get(*SG, i, fld));
            if (std::fabs(x) > maxX) db.Set(*SG, i, fld, F2U(x > 0 ? maxX : -maxX));
        }
    }
    return db.d;
}
struct Reg { size_t at; std::vector<uint8_t> a, b; };
std::vector<Reg> Diff(const std::vector<uint8_t>& o, const std::vector<uint8_t>& m) {
    std::vector<Reg> out; size_t i = 0, n = std::min(o.size(), m.size());
    auto same8 = [&](size_t j) { size_t e = std::min(n, j + 8); for (size_t k = j; k < e; k++) if (o[k] != m[k]) return false; return (j + 8 <= n) || true; };
    while (i < n) {
        if (o[i] == m[i]) { i++; continue; }
        size_t j = i;
        while (j < n && (o[j] != m[j] || !same8(j))) j++;
        out.push_back({i, std::vector<uint8_t>(o.begin() + i, o.begin() + j), std::vector<uint8_t>(m.begin() + i, m.begin() + j)});
        i = j;
    }
    return out;
}
void Put32(std::vector<uint8_t>& f, uint32_t v) { for (int k = 0; k < 4; k++) f.push_back((uint8_t)(v >> (8 * k))); }
}   // namespace

bool GenerateLines7(const std::string& gameDir, const std::string& outPath) {
    gdb::Archive boot, fe2ig;
    if (!boot.Open(gameDir + "/qkl_boot.ast") || !fe2ig.Open(gameDir + "/qkl_fe2ig.ast")) return false;
    std::vector<uint8_t> def, off;
    if (!fe2ig.Read(260, def) || !boot.Read(82, off)) return false;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> blocks = {
        {def, Clamp(def, {10, 11}, 2.6f, {"Special", "Kick Return", "Safety Kick Return"})},
        {off, Clamp(off, {5, 9}, 1.67f, {"Kickoff", "Safety Kickoff", "Onside", "Special"})}};
    std::vector<uint8_t> file; Put32(file, (uint32_t)blocks.size());
    for (auto& [o, m] : blocks) {
        auto regs = Diff(o, m);
        if (regs.empty()) return false;
        size_t a0 = regs[0].at, from = a0 > 64 ? a0 - 64 : 0;
        Put32(file, (uint32_t)(a0 - from)); file.insert(file.end(), o.begin() + from, o.begin() + a0);
        Put32(file, (uint32_t)regs.size());
        for (auto& r : regs) { Put32(file, (uint32_t)(r.at - a0)); Put32(file, (uint32_t)r.a.size()); file.insert(file.end(), r.a.begin(), r.a.end()); file.insert(file.end(), r.b.begin(), r.b.end()); }
    }
    std::string tmp = outPath + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb"); if (!f) return false;
    bool ok = fwrite(file.data(), 1, file.size(), f) == file.size(); ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(outPath.c_str());
    return rename(tmp.c_str(), outPath.c_str()) == 0;
}
