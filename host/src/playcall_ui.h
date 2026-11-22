// Full playbook (Windows): a play-call box with play-art cards, an extra to the arcade play call, plus the on-screen
// "FULL PLAYBOOK" button that opens it during the play call.
#pragma once
#include <string>
#include <vector>

struct PcArtPoint { int dx, dy, end; };              // offset from the player's start spot; end 1 block, 2-3 arrow, 4 zone
struct PcArtPlayer { int x, y, pos; std::vector<PcArtPoint> pts; };
struct PcPlay {
    std::string form, name, sets;                     // form: shown; name: the playbook's name (may hold %L markers)
    std::string gform;                                // the playbook formation name the game knows ("Gun")
    std::string shown;                                // name for display
    std::vector<PcArtPlayer> art;
    bool pass = false;                                // two or more receivers run routes (from the play art)
};
// Parses one line of mods/playcall_plays.txt (after the side column): formation, name, sets, art, game formation.
bool PcParse(const std::vector<std::string>& cols, PcPlay& out);

// The play box for one side (0 offense, 1 defense): formations with LB / RB, 4 play cards at a time.
// form / play: where it opens, updated to the last position. Returns the chosen play (index into list) or -1.
int ShowPlayBox(const std::vector<PcPlay>& list, int side, int& form, int& play);
// One or two boxes in one window (two players: offense left, defense right); each box answers only to its pad (port).
// open: shown from the start (otherwise its player opens it with SELECT while the window is up). onPick is called as
// soon as a box is finished (result -1 = closed, keep the arcade call); each open box holds back only its own pad.
struct PcBoxReq { const std::vector<PcPlay>* list; int side, port, form, play, result; bool open; void (*onPick)(int side, int result, int port); };
void ShowPlayBoxes(PcBoxReq* req, int n);

// The on-screen button. Call often from the same thread (it pumps its messages). Returns true once when clicked.
bool PlayBadge(bool show);
