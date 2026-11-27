// Main menu additions. The game's main menu is an XML list of buttons (qkl_interface.ast entry 121). At start the
// game's own list is read from your game files, our buttons are added to it and the game is given the result in place
// of that entry - nothing in the game folder is changed, and no game data ships with the project.
//   Towers (ladder mode, towers.cpp): added under Play Now (mods/towers.off leaves the menu as it is).
#include "archive_overlay.h"
#include <cstdio>
#include <string>
#include <vector>

bool ReadOriginalEntry(const std::string& path, uint32_t entry, std::vector<uint8_t>& out);   // playbook_gen.cpp

static bool Exists(const std::string& p) { if (FILE* f = fopen(p.c_str(), "rb")) { fclose(f); return true; } return false; }

// The menu list is in two places: qkl_ram.ast entry 234 (the copy the game reads) and qkl_interface.ast entry 121.
static bool AddTowers(const std::string& arc, const char* name, uint32_t entry) {   // name: the archive file name (how overlays are keyed)
    std::vector<uint8_t> raw;
    if (!ReadOriginalEntry(arc, entry, raw)) { fprintf(stderr, "[mods] menu: could not read %s entry %u\n", arc.c_str(), entry); return false; }
    std::string xml(raw.begin(), raw.end());
    if (xml.find("<menu name=\"MAIN\"") == std::string::npos) { fprintf(stderr, "[mods] menu: %s entry %u is not the main menu - not changed\n", arc.c_str(), entry); return false; }
    // The game only knows its own button ids (a new id crashes its menu reader), so Towers uses the menu's hidden
    // second Play Now button (PLAYNOW_TEAMSELECT): renamed and made visible, nothing added.
    size_t at = xml.find("buttonID=\"PLAYNOW_TEAMSELECT\"");
    if (at == std::string::npos) { fprintf(stderr, "[mods] menu: hidden Play Now button not found\n"); return false; }
    size_t end = xml.find("/>", at); if (end == std::string::npos) return false;
    std::string item = xml.substr(at, end - at);
    auto swap = [&](const std::string& from, const std::string& to) { size_t k = item.find(from); if (k != std::string::npos) item.replace(k, from.size(), to); };
    swap("name=\"PLAYNOW_HIDDEN\"", "name=\"Towers\"");
    swap("visible=\"false\"", "visible=\"true\"");
    xml.replace(at, end - at, item);
    OverlaySetEntry(name, entry, std::vector<uint8_t>(xml.begin(), xml.end()));
    return true;
}

void InstallMenuMods(const std::string& gameDir, const std::string& modsDir) {
    void TowersInit(const std::string&, const std::string&); TowersInit(gameDir, modsDir);
    if (Exists(modsDir + "/towers.off")) return;   // Towers is on unless mods/towers.off
    bool a = AddTowers(gameDir + "/qkl_ram.ast", "qkl_ram.ast", 234), b = AddTowers(gameDir + "/qkl_interface.ast", "qkl_interface.ast", 121);
    if (a || b) fprintf(stderr, "[mods] menu: Towers button added under Play Now%s%s\n", a ? " [ram]" : "", b ? " [interface]" : "");
}
