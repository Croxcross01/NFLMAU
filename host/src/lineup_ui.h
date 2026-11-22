// Pre-game lineup picker window (Windows): shown while a match loads, after the match depth charts are made.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct LineupPlayer { uint32_t id = 0, pos = 0, ovr = 0, number = 0; std::string name; };
struct LineupTeam {
    std::string title;                       // "Home team" / "Away team" (fallback when the names are unknown)
    std::string city, nick;                  // team city / nickname (TEAM.TLNA / TDNA), may be empty
    uint32_t color1 = 0x1D3F8A, color2 = 0xC8CDD2;   // primary / secondary team colours, 0xRRGGBB
    std::vector<LineupPlayer> players;
    std::vector<uint32_t> slots;             // in: current starter per slot (player id or 0xFFFFFFFF); out: chosen
};
// Slot layout shared by the window and the depth chart writer: position index (PPOS) per slot.
extern const uint32_t kLineupSlotPos[];
extern const char* const kLineupSlotName[];
extern const int kLineupSlots;
extern const char* const kPosNames[21];

// Returns true when the user pressed "Start game" (slots updated), false for "Skip" / closed. playersMode (5 / 7 / 11,
// may be null): the players-per-side choice shown at the top (LB / RB); changed in place.
// padTeam: per controller port, the team (0 home / 1 away) it plays, or -1; two pads on different teams = two-player
// mode (each sets his own team, START = ready, the game starts when both are ready). On return: the sides as they ended
// (a second pad joining / BACK swapping), all -1 when it was not two-player.
bool ShowLineupPicker(LineupTeam& home, LineupTeam& away, int* playersMode = nullptr, int* padTeam = nullptr);
