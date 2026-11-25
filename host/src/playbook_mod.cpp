// Edited playbooks: the dev tools' Playbooks tab saves your changes as operations in mods/playbook_edits.txt (and
// updates can bring shared ones in mods/playbook_edits_update.txt). At start the game's own playbooks are read from
// your game files, the edits are applied (playbook_core.cpp) and the game is given the result in place of
// qkl_boot.ast entry 82 / qkl_fe2ig.ast entry 261 (offense) and qkl_fe2ig.ast entry 260 (defense) - nothing in the
// game folder is changed. Online games use only the shared edits, so both players have the same plays.
#include "archive_overlay.h"
#include "playbook_core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

bool ReadOriginalEntry(const std::string& path, uint32_t entry, std::vector<uint8_t>& out);   // playbook_gen.cpp

static std::string ReadText(const std::string& p) {
    std::string s;
    if (FILE* f = fopen(p.c_str(), "rb")) { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); }
    return s;
}
static std::string g_editsText;
// A short fingerprint of the edits in use ("" = none), so the Full Playbook list is rebuilt when they change.
std::string PlaybookEditsStamp() {
    if (g_editsText.empty()) return "";
    uint64_t h = 1469598103934665603ull; for (unsigned char c : g_editsText) { h ^= c; h *= 1099511628211ull; }
    char b[24]; snprintf(b, sizeof b, "%016llx", (unsigned long long)h); return b;
}

void InstallPlaybookEdits(const std::string& gameDir, const std::string& modsDir) {
    const char* on = getenv("MADDEN_ONLINE");
    bool online = on && !strcmp(on, "1");
    std::string text = ReadText(modsDir + "/playbook_edits_update.txt");
    if (!online) { std::string mine = ReadText(modsDir + "/playbook_edits.txt"); if (!mine.empty()) text += "\nbook off\n" + mine; }
    bool any = false;
    for (size_t i = 0; i < text.size(); i++) if (text[i] != ' ' && text[i] != '\n' && text[i] != '\r' && text[i] != '\t') { any = true; break; }
    if (!any) return;
    std::vector<uint8_t> offRaw, defRaw;
    if (!ReadOriginalEntry(gameDir + "/qkl_boot.ast", 82, offRaw) || !ReadOriginalEntry(gameDir + "/qkl_fe2ig.ast", 260, defRaw)) {
        fprintf(stderr, "[mods] playbooks: could not read the game's playbooks - edits not used\n"); return;
    }
    pb::Db off, def; std::string err, log;
    if (!off.Parse(offRaw, err) || !def.Parse(defRaw, err)) { fprintf(stderr, "[mods] playbooks: %s - edits not used\n", err.c_str()); return; }
    int n = pb::ApplyEdits(off, def, text, log);
    if (!log.empty()) fprintf(stderr, "[mods] %s", log.c_str());
    if (n <= 0) { fprintf(stderr, "[mods] playbooks: no edits applied\n"); return; }
    std::vector<uint8_t> o = off.Build(), d = def.Build();
    OverlaySetEntry("qkl_boot.ast", 82, o);
    OverlaySetEntry("qkl_fe2ig.ast", 261, std::move(o));
    OverlaySetEntry("qkl_fe2ig.ast", 260, std::move(d));
    g_editsText = text;
    fprintf(stderr, "[mods] playbooks: %d edits applied%s\n", n, online ? " (online: shared edits only)" : "");
}
