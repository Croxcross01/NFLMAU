// The Towers ladder screen ("Gridiron Tower"): shown while a Towers game loads. Your team at the foot of the tower,
// every other team a plate above it (weakest at the bottom), the CHAMPION crown at the top. Beaten teams are lit green,
// the next opponent glows; after a win your marker climbs to the next plate. A / Enter / Start / click: play.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct TowerTeam { std::string name, abbr; int ovr = 0; uint32_t color = 0x2F8FFF; };
struct TowerSlot { bool used = false; std::string team; int wins = 0, losses = 0, rung = 0, total = 0; bool done = false; };
struct TowerView {
    TowerTeam me;
    std::vector<TowerTeam> rungs;   // bottom (first opponent) to top (last)
    int rung = 0;                   // next opponent (rungs.size() = climb complete)
    int fromRung = 0;               // where the marker starts (animates up to rung)
    int wins = 0, losses = 0;
    std::string note;               // e.g. "WIN! 34-16 over the Cowboys"
    std::vector<TowerSlot> slots;   // save slots (Y on the tower: load one / start a new tower)
    int slot = 0;                   // the one in use
};
// Both block until the player chooses; *port = the controller port that pressed (0 for keyboard / mouse).
// Returns what the player chose: kTowerPlay (A / Start / Enter / click), kTowerBack (B / Esc, only with allowBack),
// kTowerLoad + slot (Saves: load that slot), kTowerNew + slot (Saves: start a new tower in that slot).
enum { kTowerPlay = 0, kTowerBack = -1, kTowerLoad = 100, kTowerNew = 200 };
int ShowTowerLadder(const TowerView& v, int* port = nullptr, bool allowBack = false);
// "Pick your team": one grid of teams; returns the index chosen.
int ShowTeamPick(const std::vector<TowerTeam>& teams, int* port = nullptr);
