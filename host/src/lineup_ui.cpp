// Pre-game lineup picker window (see lineup_ui.h). A borderless, fully custom-drawn window in the style of a game menu,
// shown over the game window while the match loads. Mouse, keyboard and controller (XInput) all work:
//   up/down: move   left/right: other team / footer buttons   A / Enter / click: choose player   B / Esc: back
//   X / B key: best lineup (team in focus)   Y / R key: reset team   START / Space: start game   BACK: skip
#include "lineup_ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

const uint32_t kLineupSlotPos[] = {0, 1, 2, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 12, 13, 14, 15, 16, 16, 17, 18, 19, 20};
const char* const kLineupSlotName[] = {"QB", "HB", "FB", "WR", "WR", "WR", "TE", "LT", "LG", "C", "RG", "RT",
                                       "LE", "RE", "DT", "DT", "LOLB", "MLB", "ROLB", "CB", "CB", "FS", "SS", "K", "P"};
const int kLineupSlots = 25;
const char* const kPosNames[21] = {"QB", "HB", "FB", "WR", "TE", "LT", "LG", "C", "RG", "RT", "LE", "RE", "DT", "LOLB",
                                   "MLB", "ROLB", "CB", "FS", "SS", "K", "P"};

#ifdef _WIN32
namespace {
// ---- layout (base units, scaled to the game window) --------------------------------------------------------------
constexpr int W = 1240, H = 860;
constexpr int PX[2] = {28, 632}, PW = 580;          // team panels
constexpr int PY = 112, BANNER = 70, BODY = PY + BANNER + 10, PB = 772;
constexpr int ROW = 21, GROUP = 19;
constexpr int FY = 790, FH = 46;                    // footer buttons
const int kGroupStart[3] = {0, 12, 23};
const wchar_t* const kGroupName[3] = {L"OFFENSE", L"DEFENSE", L"SPECIAL TEAMS"};

struct Font { HFONT h = nullptr; };
struct Side {
    LineupTeam* team;
    std::vector<uint32_t> start;                    // the game's lineup (reset / changed marks)
};
Side g_side[2];
HWND g_wnd;
double g_s = 1;                                      // scale
bool g_done, g_accepted;
int g_team = 0, g_row = 0;                           // focus: team panel + slot row, or g_row = -1..: footer
int g_footer = -1;                                   // >= 0: footer button in focus (0 best, 1 reset, 2 skip, 3 start)
bool g_pick = false;
int* g_mode = nullptr;                               // players per side: 5, 7 or 11 (full lines)
const int kModes[3] = {5, 7, 11};
const wchar_t* const kModeName[3] = {L"5 ON 5", L"7 ON 7", L"FULL"};
int ModeIndex() { for (int i = 0; i < 3; i++) if (g_mode && *g_mode == kModes[i]) return i; return 0; }                                 // choosing a player for (g_team, g_row)
std::vector<uint32_t> g_cands; int g_cand = 0, g_scroll = 0;
// Two players (each pad on its own team): each team has its own focus, swapped into the globals above while that
// player's input is handled or that panel is drawn; START marks a player ready, the game starts when both are.
struct Focus { int row = 0, footer = -1; bool pick = false; std::vector<uint32_t> cands; int cand = 0, scroll = 0; };
bool g_two = false; bool g_ready[2] = {}; int g_padTeam[4] = {-1, -1, -1, -1}; int g_kbTeam = 0;
Focus g_focus[2]; int g_cur = -1;
void Use(int t) { if (!g_two || g_cur == t) return; if (g_cur >= 0) { Focus& f = g_focus[g_cur]; f.row = g_row; f.footer = g_footer; f.pick = g_pick; f.cands = g_cands; f.cand = g_cand; f.scroll = g_scroll; }
    Focus& f = g_focus[t]; g_team = t; g_row = f.row; g_footer = f.footer; g_pick = f.pick; g_cands = f.cands; g_cand = f.cand; g_scroll = f.scroll; g_cur = t; }
int g_hoverBtn = -1;
DWORD g_openTick;
HFONT g_fTitle, g_fSub, g_fBig, g_fRow, g_fRowB, g_fSmall, g_fBtn, g_fOvr;
std::wstring g_face = L"Segoe UI", g_faceCond = L"Segoe UI";

int S(double v) { return (int)lround(v * g_s); }
RECT ModeRect(int i) {   // selector segment i (base units scaled), right of the title
    const int segW = 76, gap = 4, rbx = W - 28 - 30, top = 34, h = 30;
    int x = rbx - 6 - (3 - i) * segW - (2 - i) * gap;
    return RECT{S(x), S(top), S(x + segW), S(top + h)};
}
COLORREF RGBc(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
uint32_t Mix(uint32_t a, uint32_t b, double t) {
    auto ch = [&](int sh) { return (uint32_t)lround(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}
double Luma(uint32_t c) { return 0.299 * ((c >> 16) & 255) + 0.587 * ((c >> 8) & 255) + 0.114 * (c & 255); }
uint32_t Readable(uint32_t c) {   // team colour lifted so it reads on the dark background
    double l = Luma(c);
    return l < 70 ? Mix(c, 0xFFFFFF, (70 - l) / 160.0) : c;
}
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring Upper(std::wstring s) { for (auto& c : s) c = (wchar_t)towupper(c); return s; }

const LineupPlayer* Find(const Side& s, uint32_t id) {
    for (auto& p : s.team->players) if (p.id == id) return &p;
    return nullptr;
}

// ---- drawing helpers -------------------------------------------------------------------------------------------
void Fill(HDC dc, int x, int y, int w, int h, uint32_t c) {
    RECT r{x, y, x + w, y + h}; HBRUSH b = CreateSolidBrush(RGBc(c)); FillRect(dc, &r, b); DeleteObject(b);
}
void Slant(HDC dc, int x, int y, int w, int h, uint32_t c, int lean) {   // parallelogram, leaning right by `lean`
    POINT p[4] = {{x + lean, y}, {x + w, y}, {x + w - lean, y + h}, {x, y + h}};
    HBRUSH b = CreateSolidBrush(RGBc(c)); HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, p, 4); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
}
void VGrad(HDC dc, int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    for (int i = 0; i < h; i += 2) Fill(dc, x, y + i, w, 2, Mix(top, bot, h > 1 ? double(i) / (h - 1) : 0));
}
void HGrad(HDC dc, int x, int y, int w, int h, uint32_t l, uint32_t r) {
    for (int i = 0; i < w; i += 2) Fill(dc, x + i, y, 2, h, Mix(l, r, w > 1 ? double(i) / (w - 1) : 0));
}
void Text(HDC dc, const std::wstring& t, int x, int y, int w, int h, HFONT f, uint32_t c, UINT fmt = DT_LEFT) {
    SelectObject(dc, f); SetTextColor(dc, RGBc(c)); SetBkMode(dc, TRANSPARENT);
    RECT r{x, y, x + w, y + h}; DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}
uint32_t OvrColor(uint32_t o) { return o >= 90 ? 0x39E27F : o >= 80 ? 0xA6E03A : o >= 70 ? 0xFFC93A : 0xFF7A3A; }
// palette: deep navy with electric-blue glow and silver trim
constexpr uint32_t BG_TOP = 0x081634, BG_BOT = 0x02060F, PANEL = 0x061127, PANEL2 = 0x0A1A38, GLOW = 0x2F9BFF, GLOW_HI = 0x8EE3FF,
                   SILVER_HI = 0xF2F5FA, SILVER_LO = 0x7D8796, TEXT = 0xEAF2FF, TEXT_DIM = 0x8A9BBF, TAG = 0x0B0F1A;
void Frame(HDC dc, int x, int y, int w, int h, uint32_t c, int t) {   // rectangle outline of thickness t
    Fill(dc, x, y, w, t, c); Fill(dc, x, y + h - t, w, t, c); Fill(dc, x, y, t, h, c); Fill(dc, x + w - t, y, t, h, c);
}
void Glow(HDC dc, int x, int y, int w, int h, uint32_t c, uint32_t bg, int layers) {   // soft outer glow + bright edge
    for (int i = layers; i >= 1; i--) Frame(dc, x - i, y - i, w + 2 * i, h + 2 * i, Mix(bg, c, 0.55 * (1.0 - double(i) / (layers + 1))), 1);
    Frame(dc, x, y, w, h, c, std::max(1, S(2)));
    Frame(dc, x + S(2), y + S(2), w - S(4), h - S(4), Mix(c, 0xFFFFFF, 0.5), 1);
}
void Chrome(HDC dc, int x, int y, int w, int h) {   // brushed-silver bar
    int half = h / 2;
    VGrad(dc, x, y, w, half, SILVER_HI, 0xBFC6D1);
    VGrad(dc, x, y + half, w, h - half, 0x9AA3B1, SILVER_LO);
    Fill(dc, x, y, w, 1, 0xFFFFFF); Fill(dc, x, y + h - 1, w, 1, 0x4A5260);
}
void OutlineText(HDC dc, const std::wstring& t, int x, int y, int w, int h, HFONT f, uint32_t fill, uint32_t edge, UINT fmt = DT_LEFT) {
    int o = std::max(1, S(2));
    for (int dx = -o; dx <= o; dx += o)
        for (int dy = -o; dy <= o; dy += o)
            if (dx || dy) Text(dc, t, x + dx, y + dy, w, h, f, edge, fmt);
    Text(dc, t, x, y, w, h, f, fill, fmt);
}
void OvrBox(HDC dc, int x, int y, int h, uint32_t ovr, bool bright) {
    int w = S(42);
    Fill(dc, x, y, w, h, bright ? 0x0D3E8C : 0x0A2350);
    Frame(dc, x, y, w, h, bright ? GLOW_HI : GLOW, 1);
    Fill(dc, x + S(3), y + h - S(3), w - S(6), S(2), OvrColor(ovr));
    Text(dc, std::to_wstring(ovr), x, y - S(1), w, h, g_fOvr, 0xFFFFFF, DT_CENTER);
}
int Key(HDC dc, const std::wstring& k, int x, int cy, bool lit, int minW = 24) {   // dark controller tag ("LB"-style), returns its width
    SelectObject(dc, g_fSmall); SIZE sz; GetTextExtentPoint32W(dc, k.c_str(), (int)k.size(), &sz);
    int w = std::max<int>(sz.cx + S(12), S(minW)), h = S(20);
    Fill(dc, x, cy - h / 2, w, h, TAG);
    Frame(dc, x, cy - h / 2, w, h, lit ? GLOW_HI : 0x5A6478, 1);
    Text(dc, k, x, cy - h / 2, w, h, g_fSmall, 0xFFFFFF, DT_CENTER);
    return w;
}

// ---- rows and hit testing ----------------------------------------------------------------------------------------
int RowY(int k) {   // base-unit y of slot row k
    int g = k >= kGroupStart[2] ? 2 : k >= kGroupStart[1] ? 1 : 0;
    return BODY + (g + 1) * GROUP + k * ROW + g * 6;
}
constexpr int LIST_ROW = 25;
int ListRows() { return (PB - (BODY + 34) - 8) / LIST_ROW; }

void BuildCands() {
    Side& s = g_side[g_team];
    uint32_t pos = kLineupSlotPos[g_row];
    std::vector<const LineupPlayer*> v;
    for (auto& p : s.team->players) v.push_back(&p);
    std::stable_sort(v.begin(), v.end(), [pos](const LineupPlayer* a, const LineupPlayer* b) {
        bool ap = a->pos == pos, bp = b->pos == pos;
        if (ap != bp) return ap;
        if (a->pos != b->pos) return a->pos < b->pos;
        return a->ovr > b->ovr;
    });
    g_cands.clear(); g_cand = 0;
    for (auto* p : v) { if (p->id == s.team->slots[g_row]) g_cand = (int)g_cands.size(); g_cands.push_back(p->id); }
    g_scroll = std::max(0, g_cand - ListRows() / 2);
}
void Choose(uint32_t id) {   // put `id` into the focused slot; if he already starts elsewhere, the two swap
    auto& sl = g_side[g_team].team->slots;
    for (int k = 0; k < kLineupSlots; k++)
        if (k != g_row && sl[k] == id) sl[k] = sl[g_row];
    sl[g_row] = id;
}
void Best(int t) {
    Side& s = g_side[t];
    std::set<uint32_t> used;
    for (int k = 0; k < kLineupSlots; k++) {
        const LineupPlayer* best = nullptr;
        for (auto& p : s.team->players)
            if (p.pos == kLineupSlotPos[k] && !used.count(p.id) && (!best || p.ovr > best->ovr)) best = &p;
        s.team->slots[k] = best ? best->id : 0xFFFFFFFFu;
        if (best) used.insert(best->id);
    }
}
void Invalidate() { InvalidateRect(g_wnd, nullptr, FALSE); }

// ---- paint -------------------------------------------------------------------------------------------------------
void DrawPanel(HDC dc, int t) {
    // two players: player 1's team keeps the blue look, the other player's team is red
    int ownerPort = -1; if (g_two) for (int i = 0; i < 4; i++) if (g_padTeam[i] == t) ownerPort = i;
    bool red = g_two && ownerPort > 0;
    const uint32_t gGl = red ? 0xFF3B3Bu : GLOW, gHi = red ? 0xFFA096u : GLOW_HI, selA = red ? 0xE8323Au : 0x2A8CFFu, selB = red ? 0x8E0E16u : 0x0B3F9Eu;
    Side& s = g_side[t];
    LineupTeam& tm = *s.team;
    uint32_t c1 = tm.color1, c2 = tm.color2;
    int x = S(PX[t]), w = S(PW), focus = (g_team == t && g_footer < 0);
    int y0 = S(PY), h0 = S(PB - PY);
    // panel: navy glass with an electric-blue frame (brighter when this team has the focus)
    VGrad(dc, x, y0, w, h0, PANEL2, PANEL);
    Glow(dc, x, y0, w, h0, focus ? gHi : gGl, BG_BOT, focus ? S(7) : S(3));
    // team plate: team colour with a silver trim, like a scoreboard name plate
    int px = x + S(6), pw = w - S(12), py = y0 + S(6), ph = S(BANNER - 8);
    VGrad(dc, px, py, pw, ph, Mix(c1, 0xFFFFFF, 0.12), Mix(c1, 0x000000, 0.35));
    Fill(dc, px, py, pw, S(2), Mix(c1, 0xFFFFFF, 0.45));
    Slant(dc, px + pw - S(170), py, S(170), ph, Mix(c2, c1, 0.25), S(22));
    Slant(dc, px + pw - S(186), py, S(10), ph, c2, S(22));
    Chrome(dc, px, py + ph, pw, S(5));
    std::wstring city = Upper(Wide(tm.city)), nick = Upper(Wide(tm.nick.empty() ? tm.title : tm.nick));
    uint32_t edge = Mix(c1, 0x000000, 0.6);
    Text(dc, city, px + S(14), py + S(3), pw - S(200), S(20), g_fSub, 0xFFFFFF);
    OutlineText(dc, nick, px + S(12), py + S(19), pw - S(200), S(42), g_fBig, 0xFFFFFF, edge);
    OutlineText(dc, t == 0 ? L"HOME" : L"AWAY", px + pw - S(140), py, S(126), ph, g_fBtn, Luma(Mix(c2, c1, 0.25)) > 150 ? 0x0B1020 : 0xFFFFFF,
                Luma(Mix(c2, c1, 0.25)) > 150 ? 0xFFFFFF : 0x000000, DT_RIGHT);
    if (g_pick && g_team == t) {   // ---- player list
        uint32_t pos = kLineupSlotPos[g_row];
        int ly = S(BODY);
        Fill(dc, x + S(14), ly, S(250), S(26), 0x0D3E8C); Frame(dc, x + S(14), ly, S(250), S(26), gHi, 1);
        Text(dc, L"CHOOSE " + Wide(kLineupSlotName[g_row]), x + S(26), ly, S(230), S(26), g_fRowB, 0xFFFFFF);
        Text(dc, std::to_wstring(g_cand + 1) + L" / " + std::to_wstring(g_cands.size()), x + w - S(150), ly, S(130), S(26), g_fSmall, TEXT_DIM, DT_RIGHT);
        int rows = ListRows();
        for (int i = 0; i < rows && g_scroll + i < (int)g_cands.size(); i++) {
            int ci = g_scroll + i;
            const LineupPlayer* p = Find(s, g_cands[ci]);
            if (!p) continue;
            int y = S(BODY + 34 + i * LIST_ROW), h = S(LIST_ROW - 3), rx = x + S(12), rw = w - S(24);
            bool sel = ci == g_cand, own = p->pos == pos;
            if (sel) { VGrad(dc, rx, y, rw, h, selA, selB); Frame(dc, rx, y, rw, h, gHi, 1); }
            else Fill(dc, rx, y, rw, h, i % 2 ? 0x0B1A36 : 0x08142C);
            uint32_t tc = sel ? 0xFFFFFF : own ? TEXT : TEXT_DIM;
            Key(dc, Wide(kPosNames[std::min<uint32_t>(p->pos, 20)]), rx + S(8), y + h / 2, sel, 54);
            Text(dc, L"#" + std::to_wstring(p->number), x + S(84), y, S(44), h, g_fRow, tc);
            Text(dc, Upper(Wide(p->name)), x + S(128), y, w - S(320), h, sel ? g_fRowB : g_fRow, tc);
            for (int k = 0; k < kLineupSlots; k++)   // already starting somewhere?
                if (tm.slots[k] == p->id) {
                    std::wstring tag = k == g_row ? L"CURRENT" : L"SWAP " + Wide(kLineupSlotName[k]);
                    Text(dc, tag, x + w - S(196), y, S(130), h, g_fSmall, k == g_row ? 0x8EE3FF : 0xFFC93A, DT_RIGHT);
                    break;
                }
            OvrBox(dc, x + w - S(60), y + S(1), h - S(2), p->ovr, sel || own);
        }
        if (g_scroll > 0) Text(dc, L"\x25B2", x, S(BODY + 30) - S(14), w, S(14), g_fSmall, gHi, DT_CENTER);
        if (g_scroll + rows < (int)g_cands.size()) Text(dc, L"\x25BC", x, S(PB - 18), w, S(14), g_fSmall, gHi, DT_CENTER);
        return;
    }
    // ---- starters
    for (int g = 0; g < 3; g++) {
        int y = S(RowY(kGroupStart[g]) - GROUP);
        OutlineText(dc, kGroupName[g], x + S(16), y, S(200), S(GROUP), g_fSmall, gHi, 0x041030);
        int lx = x + S(16) + S(g == 2 ? 110 : 76);
        HGrad(dc, lx, y + S(GROUP / 2), x + w - S(16) - lx, 1, gGl, PANEL);
    }
    for (int k = 0; k < kLineupSlots; k++) {
        int y = S(RowY(k)), h = S(ROW - 2), rx = x + S(10), rw = w - S(20);
        bool sel = focus && g_row == k;
        const LineupPlayer* p = Find(s, tm.slots[k]);
        if (sel) { VGrad(dc, rx, y, rw, h, selA, selB); Frame(dc, rx, y, rw, h, gHi, 1); }
        else if (k % 2) Fill(dc, rx, y, rw, h, 0x0A1833);
        Key(dc, Wide(kLineupSlotName[k]), rx + S(6), y + h / 2, sel, 58);
        if (!p) { Text(dc, L"\x2014  EMPTY  \x2014", x + S(128), y, S(300), h, g_fRow, 0x5A6A8A); continue; }
        uint32_t tc = sel ? 0xFFFFFF : TEXT;
        Text(dc, L"#" + std::to_wstring(p->number), x + S(84), y, S(44), h, g_fRow, sel ? 0xFFFFFF : TEXT_DIM);
        Text(dc, Upper(Wide(p->name)), x + S(128), y, w - S(270), h, sel ? g_fRowB : g_fRow, tc);
        if (p->pos != kLineupSlotPos[k])
            Text(dc, L"(" + Wide(kPosNames[std::min<uint32_t>(p->pos, 20)]) + L")", x + w - S(156), y, S(80), h, g_fSmall, 0xFFC93A, DT_RIGHT);
        if (s.start[k] != tm.slots[k]) Fill(dc, x + w - S(70), y + h / 2 - S(3), S(6), S(6), gHi);   // changed
        OvrBox(dc, x + w - S(60), y + S(1), h - S(2), p->ovr, true);
    }
}
RECT BtnRect(int i) {   // footer buttons: best, reset | skip, start
    int bw[4] = {170, 130, 150, 230};
    int x = i == 0 ? 28 : i == 1 ? 28 + 170 + 12 : i == 2 ? W - 28 - 230 - 12 - 150 : W - 28 - 230;
    return RECT{S(x), S(FY), S(x + bw[i]), S(FY + FH)};
}
void DrawButton(HDC dc, int i) {
    static const wchar_t* label[4] = {L"BEST LINEUP", L"RESET", L"SKIP", L"START GAME"};
    static const wchar_t* key[4] = {L"X", L"Y", L"BACK", L"START"};
    RECT r = BtnRect(i);
    int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
    bool hot = g_footer == i || g_hoverBtn == i, primary = i == 3;
    if (primary) { VGrad(dc, x, y, w, h, hot ? 0x5CB8FF : 0x2A8CFF, hot ? 0x1456C8 : 0x0B3F9E); Glow(dc, x, y, w, h, GLOW_HI, BG_BOT, hot ? S(8) : S(4)); }
    else { VGrad(dc, x, y, w, h, hot ? 0x173A75 : 0x0E1F42, hot ? 0x0B2350 : 0x071330); Glow(dc, x, y, w, h, hot ? GLOW_HI : GLOW, BG_BOT, hot ? S(5) : S(1)); }
    int kw = Key(dc, key[i], x + S(14), y + h / 2, hot || primary);
    int tx = x + S(14) + kw + S(10);
    OutlineText(dc, label[i], tx, y, x + w - tx - S(8), h, g_fBtn, 0xFFFFFF, primary ? 0x07307A : 0x020812);
}
void Paint(HDC dc) {
    int ww = S(W), hh = S(H);
    VGrad(dc, 0, 0, ww, hh, BG_TOP, BG_BOT);
    // stadium-light haze and a faint blue grid
    for (int i = 0; i < S(260); i += 2) Fill(dc, 0, S(110) + i, ww, 2, Mix(BG_TOP, 0x123A7A, 0.35 * (1 - std::fabs(i - S(130)) / double(S(130)))));
    for (int gx = 0; gx < ww; gx += S(40)) Fill(dc, gx, 0, 1, hh, 0x0B1C3C);
    for (int gy = 0; gy < hh; gy += S(40)) Fill(dc, 0, gy, ww, 1, 0x0B1C3C);
    Glow(dc, 0, 0, ww, hh, GLOW, BG_BOT, 0);
    // header: chrome bar with the title on a glowing blue plate
    Chrome(dc, 0, S(18), ww, S(6));
    Chrome(dc, 0, S(100), ww, S(4));
    int tw = S(560), tx = (ww - tw) / 2;
    VGrad(dc, tx, S(28), tw, S(66), 0x1E6FE8, 0x082A6E);
    Glow(dc, tx, S(28), tw, S(66), GLOW_HI, BG_TOP, S(8));
    OutlineText(dc, L"SET YOUR STARTERS", tx, S(28), tw, S(66), g_fTitle, 0xFFFFFF, 0x031A4A, DT_CENTER);
    // players-per-side selector (right of the title): LB / RB
    {
        int sel = ModeIndex();
        for (int i = 0; i < 3; i++) {
            RECT r = ModeRect(i);
            int x = r.left, y0 = r.top, segW = r.right - r.left, h = r.bottom - r.top;
            if (i == sel) { VGrad(dc, x, y0, segW, h, 0x2A8CFF, 0x0B3F9E); Glow(dc, x, y0, segW, h, GLOW_HI, BG_TOP, S(4)); }
            else { Fill(dc, x, y0, segW, h, 0x0A1833); Frame(dc, x, y0, segW, h, 0x2A4A80, 1); }
            OutlineText(dc, kModeName[i], x, y0, segW, h, g_fSub, i == sel ? 0xFFFFFF : TEXT_DIM, i == sel ? 0x031A4A : 0x020812, DT_CENTER);
        }
        RECT r0 = ModeRect(0), r2 = ModeRect(2);
        Text(dc, L"PLAYERS PER SIDE", r0.left, S(12), r2.right - r0.left, S(20), g_fSmall, TEXT_DIM, DT_CENTER);
        Key(dc, L"LB", r0.left - S(36), (r0.top + r0.bottom) / 2, true, 30);
        Key(dc, L"RB", r2.right + S(6), (r0.top + r0.bottom) / 2, true, 30);
    }
    const wchar_t* hint = g_two ? L"each player sets his own team \x2022 START = ready \x2022 BACK = swap teams"
                        : g_pick ? L"A choose \x2022 B back \x2022 a starter you pick swaps"
                                 : L"A change player \x2022 \x2190 \x2192 switch team";
    OutlineText(dc, L"PRE-GAME", S(30), S(30), S(280), S(30), g_fBtn, GLOW_HI, 0x031A4A);
    Text(dc, hint, S(30), S(62), S(300), S(26), g_fSmall, TEXT_DIM);
    if (!g_two) { DrawPanel(dc, 0); DrawPanel(dc, 1); for (int i = 0; i < 4; i++) DrawButton(dc, i); return; }
    int keep = g_cur;
    for (int t = 0; t < 2; t++) {
        Use(t); DrawPanel(dc, t);
        // player tag and ready state under the panel
        int x = S(PX[t]), w = S(PW), y = S(FY), h = S(FH);
        int port = -1; for (int i = 0; i < 4; i++) if (g_padTeam[i] == t) port = i;
        if (g_ready[t]) { VGrad(dc, x, y, w, h, 0x2ECC71, 0x12804A); Glow(dc, x, y, w, h, 0x9CFFC4, BG_BOT, S(6)); }
        else if (port > 0) { VGrad(dc, x, y, w, h, 0x4A0E14, 0x2A060A); Glow(dc, x, y, w, h, 0xFF3B3B, BG_BOT, S(2)); }
        else { VGrad(dc, x, y, w, h, 0x0E1F42, 0x071330); Glow(dc, x, y, w, h, GLOW, BG_BOT, S(2)); }
        std::wstring tag = L"PLAYER " + std::to_wstring(port + 1) + (g_ready[t] ? L"  \x2022  READY" : L"  \x2022  PRESS START WHEN READY");
        OutlineText(dc, tag, x, y, w, h, g_fBtn, 0xFFFFFF, g_ready[t] ? 0x0B5A2E : 0x020812, DT_CENTER);
    }
    if (keep >= 0) Use(keep);
}

// ---- input -------------------------------------------------------------------------------------------------------
enum Act { UP, DOWN, LEFT, RIGHT, OK, BACK, BEST, RESET, START, SKIP, PGUP, PGDN, MODE_PREV, MODE_NEXT };
void Do(Act a);
// two players: an action from the player on team t (his own team only; START = ready / not ready)
void DoFor(int t, Act a) {
    if (!g_two) { Do(a); return; }
    if (t < 0 || t > 1) return;
    Use(t);
    if (a == START) { if (g_pick) Do(OK); g_ready[t] = !g_ready[t]; if (g_ready[0] && g_ready[1]) g_accepted = g_done = true; Invalidate(); return; }
    if (g_ready[t] && a != MODE_PREV && a != MODE_NEXT) return;   // ready: only START (not ready) works
    if (a == SKIP && !g_pick) {   // BACK: the two players swap teams
        for (int k = 0; k < 4; k++) if (g_padTeam[k] >= 0) g_padTeam[k] ^= 1;
        g_ready[0] = g_ready[1] = false; g_kbTeam ^= 1; Use(g_kbTeam); Invalidate(); return;
    }
    if (a == SKIP) return;
    if (!g_pick && (a == LEFT || a == RIGHT)) return;               // no switching to the other team
    if (!g_pick && a == DOWN && g_row == kLineupSlots - 1) return;   // no footer buttons
    if (a == BEST) { Best(t); Invalidate(); return; }
    if (a == RESET) { g_side[t].team->slots = g_side[t].start; Invalidate(); return; }
    Do(a);
}
void Do(Act a) {
    if ((a == MODE_PREV || a == MODE_NEXT) && g_mode) {
        int i = ModeIndex() + (a == MODE_NEXT ? 1 : 2);
        *g_mode = kModes[i % 3];
        Invalidate();
        return;
    }
    if (g_pick) {
        int n = (int)g_cands.size(), rows = ListRows();
        switch (a) {
        case UP: g_cand = std::max(0, g_cand - 1); break;
        case DOWN: g_cand = std::min(n - 1, g_cand + 1); break;
        case PGUP: case LEFT: g_cand = std::max(0, g_cand - rows); break;
        case PGDN: case RIGHT: g_cand = std::min(n - 1, g_cand + rows); break;
        case OK: if (n) Choose(g_cands[g_cand]); g_pick = false; break;
        case BACK: g_pick = false; break;
        case START: if (n) Choose(g_cands[g_cand]); g_pick = false; break;
        default: break;
        }
        if (g_cand < g_scroll) g_scroll = g_cand;
        if (g_cand >= g_scroll + rows) g_scroll = g_cand - rows + 1;
        Invalidate();
        return;
    }
    switch (a) {
    case UP:
        if (g_footer >= 0) { g_footer = -1; g_row = kLineupSlots - 1; }
        else g_row = (g_row + kLineupSlots - 1) % kLineupSlots;
        break;
    case DOWN:
        if (g_footer >= 0) break;
        if (g_row == kLineupSlots - 1) g_footer = 3; else g_row++;
        break;
    case LEFT: if (g_footer >= 0) g_footer = std::max(0, g_footer - 1); else g_team = 0; break;
    case RIGHT: if (g_footer >= 0) g_footer = std::min(3, g_footer + 1); else g_team = 1; break;
    case OK:
        if (g_footer == 0) { Best(0); Best(1); }
        else if (g_footer == 1) { for (auto& s : g_side) s.team->slots = s.start; }
        else if (g_footer == 2) g_done = true;
        else if (g_footer == 3) { g_accepted = g_done = true; }
        else { g_pick = true; BuildCands(); }
        break;
    case BEST: if (g_footer >= 0) { Best(0); Best(1); } else Best(g_team); break;
    case RESET: if (g_footer >= 0) { for (auto& s : g_side) s.team->slots = s.start; } else g_side[g_team].team->slots = g_side[g_team].start; break;
    case START: g_accepted = g_done = true; break;
    case SKIP: g_done = true; break;
    case BACK: if (g_footer >= 0) { g_footer = -1; } break;
    default: break;
    }
    Invalidate();
}
// mouse: which slot / list row / button is under (x, y)
bool HitSlot(int mx, int my, int& team, int& row) {
    for (int t = 0; t < 2; t++) {
        if (mx < S(PX[t]) || mx >= S(PX[t] + PW)) continue;
        if (g_pick && g_team == t) {
            int i = (my - S(BODY + 34)) / S(LIST_ROW);
            if (my < S(BODY + 34) || i >= ListRows() || g_scroll + i >= (int)g_cands.size()) return false;
            team = t; row = g_scroll + i; return true;
        }
        for (int k = 0; k < kLineupSlots; k++)
            if (my >= S(RowY(k)) && my < S(RowY(k) + ROW)) { team = t; row = k; return true; }
    }
    return false;
}
int HitButton(int mx, int my) {
    for (int i = 0; i < 4; i++) { RECT r = BtnRect(i); if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) return i; }
    return -1;
}

typedef DWORD(WINAPI* XGetState)(DWORD, void*);
XGetState g_xget;
struct XPad { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
DWORD g_padPrev = 0; DWORD g_repeatAt = 0; DWORD g_repeatBtn = 0;
DWORD g_portPrev[4], g_portRepAt[4], g_portRep[4];
void PollPad() {
    if (!g_xget) return;
    static const struct { DWORD m; Act a; } map[] = {{0x0001, UP}, {0x0002, DOWN}, {0x0004, LEFT}, {0x0008, RIGHT}, {0x1000, OK},
        {0x2000, BACK}, {0x4000, BEST}, {0x8000, RESET}, {0x0010, START}, {0x0020, SKIP}, {0x10000, PGUP}, {0x20000, PGDN}, {0x0100, MODE_PREV}, {0x0200, MODE_NEXT}};
    auto read = [](DWORD i) -> DWORD {
        XPad st{};
        if (g_xget(i, &st) != 0) return 0;
        DWORD b = st.buttons;
        if (st.ly > 16000) b |= 0x0001; if (st.ly < -16000) b |= 0x0002;   // left stick as d-pad
        if (st.lx < -16000) b |= 0x0004; if (st.lx > 16000) b |= 0x0008;
        if (st.lt > 120) b |= 0x10000; if (st.rt > 120) b |= 0x20000;    // triggers as page up/down
        return b;
    };
    if (g_two) {   // each pad drives its own team
        for (DWORD i = 0; i < 4; i++) {
            DWORD b = read(i);
            if (GetTickCount() - g_openTick < 400) { g_portPrev[i] = b; continue; }
            int t = g_padTeam[i];
            DWORD press = b & ~g_portPrev[i];
            for (auto& m : map) if (press & m.m) { DoFor(t, m.a); g_portRep[i] = m.m; g_portRepAt[i] = GetTickCount() + 380; }
            if ((b & g_portRep[i]) && (g_portRep[i] & 0x3000F) && GetTickCount() >= g_portRepAt[i]) {
                for (auto& m : map) if (m.m == g_portRep[i]) DoFor(t, m.a);
                g_portRepAt[i] = GetTickCount() + 70;
            }
            if (!(b & g_portRep[i])) g_portRep[i] = 0;
            g_portPrev[i] = b;
        }
        Use(g_kbTeam);
        return;
    }
    DWORD b = 0, per[4];
    for (DWORD i = 0; i < 4; i++) { per[i] = read(i); b |= per[i]; }
    if (GetTickCount() - g_openTick < 400) { g_padPrev = b; for (DWORD i = 0; i < 4; i++) g_portPrev[i] = per[i]; return; }   // ignore what was held when the window opened
    if (!g_two) {   // a second controller pressing a button turns on two-player mode: lower port = home (left), the other = away
        static int first = -1;
        if (GetTickCount() - g_openTick < 450) first = -1;
        for (DWORD i = 0; i < 4; i++) {
            DWORD press = per[i] & ~g_portPrev[i];
            g_portPrev[i] = per[i];
            if (!press) continue;
            if (first < 0) first = (int)i;
            else if ((int)i != first) {
                int lo = std::min(first, (int)i), hi = std::max(first, (int)i);
                for (int k = 0; k < 4; k++) g_padTeam[k] = -1;
                g_padTeam[lo] = 0; g_padTeam[hi] = 1;
                g_two = true; g_kbTeam = 0; g_ready[0] = g_ready[1] = false; g_cur = -1;
                g_focus[0] = Focus(); g_focus[1] = Focus(); g_focus[0].row = g_focus[1].row = 0;
                g_pick = false; g_footer = -1;
                Use(0);
                for (DWORD k = 0; k < 4; k++) { g_portPrev[k] = per[k]; g_portRep[k] = 0; }
                Invalidate();
                return;
            }
        }
    }
    DWORD press = b & ~g_padPrev;
    for (auto& m : map) if (press & m.m) { Do(m.a); g_repeatBtn = m.m; g_repeatAt = GetTickCount() + 380; }
    if ((b & g_repeatBtn) && (g_repeatBtn & 0x3000F) && GetTickCount() >= g_repeatAt) {   // hold to scroll
        for (auto& m : map) if (m.m == g_repeatBtn) Do(m.a);
        g_repeatAt = GetTickCount() + 70;
    }
    if (!(b & g_repeatBtn)) g_repeatBtn = 0;
    g_padPrev = b;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        Paint(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        PollPad();
        DWORD dt = GetTickCount() - g_openTick;   // fade in
        if (dt <= 260) SetLayeredWindowAttributes(h, 0, (BYTE)std::min<DWORD>(255, 40 + dt * 215 / 260), LWA_ALPHA);
        else if (dt < 300) SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        return 0;
    }
    case WM_KEYDOWN:
        if (g_two) {
            Act a; bool ok = true;
            switch (w) {
            case VK_UP: a = UP; break; case VK_DOWN: a = DOWN; break; case VK_LEFT: a = LEFT; break; case VK_RIGHT: a = RIGHT; break;
            case VK_PRIOR: a = PGUP; break; case VK_NEXT: a = PGDN; break; case VK_RETURN: a = OK; break;
            case VK_ESCAPE: case VK_BACK: a = BACK; break; case VK_SPACE: a = START; break; case 'B': a = BEST; break;
            case 'Q': a = MODE_PREV; break; case 'E': case 'M': a = MODE_NEXT; break; case 'R': a = RESET; break;
            default: ok = false; a = UP;
            }
            if (ok) { DoFor(g_kbTeam, a); Use(g_kbTeam); }
            return 0;
        }
        switch (w) {
        case VK_UP: Do(UP); break;
        case VK_DOWN: Do(DOWN); break;
        case VK_LEFT: Do(LEFT); break;
        case VK_RIGHT: case VK_TAB: Do(w == VK_TAB ? (g_team ? LEFT : RIGHT) : RIGHT); break;
        case VK_PRIOR: Do(PGUP); break;
        case VK_NEXT: Do(PGDN); break;
        case VK_RETURN: Do(OK); break;
        case VK_ESCAPE: case VK_BACK: if (g_pick || g_footer >= 0) Do(BACK); else Do(SKIP); break;
        case VK_SPACE: Do(START); break;
        case 'B': Do(BEST); break;
        case 'Q': Do(MODE_PREV); break;
        case 'E': case 'M': Do(MODE_NEXT); break;
        case 'R': Do(RESET); break;
        }
        return 0;
    case WM_MOUSEMOVE: {
        int mx = (short)LOWORD(l), my = (short)HIWORD(l), t, r;
        if (g_two) { Use(g_kbTeam); if (g_ready[g_kbTeam] || (HitSlot(mx, my, t, r) && t != g_kbTeam)) return 0; }
        int hb = HitButton(mx, my);
        bool ch = hb != g_hoverBtn; g_hoverBtn = hb;
        if (HitSlot(mx, my, t, r)) {
            if (g_pick) { if (r != g_cand) { g_cand = r; ch = true; } }
            else if (t != g_team || r != g_row || g_footer >= 0) { g_team = t; g_row = r; g_footer = -1; ch = true; }
        }
        if (ch) Invalidate();
        TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, h, 0}; TrackMouseEvent(&te);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hoverBtn >= 0) { g_hoverBtn = -1; Invalidate(); } return 0;
    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(l), my = (short)HIWORD(l), t, r;
        if (g_mode)   // players-per-side selector
            for (int i = 0; i < 3; i++) {
                RECT rr = ModeRect(i);
                if (mx >= rr.left && mx < rr.right && my >= rr.top && my < rr.bottom) { *g_mode = kModes[i]; Invalidate(); return 0; }
            }
        if (g_two) {
            Use(g_kbTeam);
            if (g_ready[g_kbTeam]) return 0;
            if (HitSlot(mx, my, t, r) && t == g_kbTeam) { if (g_pick) { g_cand = r; Do(OK); } else { g_row = r; g_footer = -1; Do(OK); } }
            else if (g_pick) Do(BACK);
            return 0;
        }
        int b = HitButton(mx, my);
        if (b >= 0) { g_footer = b; Do(OK); g_footer = b == 2 || b == 3 ? b : -1; return 0; }
        if (HitSlot(mx, my, t, r)) {
            if (g_pick) { g_cand = r; Do(OK); }
            else { g_team = t; g_row = r; g_footer = -1; Do(OK); }
        } else if (g_pick) Do(BACK);
        else if (my < S(PY)) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }   // drag the window by its header
        return 0;
    }
    case WM_RBUTTONDOWN: if (g_pick) Do(BACK); return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w) > 0 ? -3 : 3;
        if (g_pick) {
            int n = (int)g_cands.size();
            g_scroll = std::clamp(g_scroll + d, 0, std::max(0, n - ListRows()));
            g_cand = std::clamp(g_cand, g_scroll, std::min(n - 1, g_scroll + ListRows() - 1));
            Invalidate();
        } else Do(d < 0 ? UP : DOWN);
        return 0;
    }
    case WM_CLOSE: g_done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int CALLBACK HasFont(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM l) { *(bool*)l = true; return 0; }
bool FontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr); LOGFONTW lf{}; wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1); lf.lfCharSet = DEFAULT_CHARSET;
    bool found = false; EnumFontFamiliesExW(dc, &lf, HasFont, (LPARAM)&found, 0); ReleaseDC(nullptr, dc);
    return found;
}
HFONT MakeFont(const std::wstring& face, double px, int weight, bool italic = false) {
    return CreateFontW(-S(px), 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face.c_str());
}
BOOL CALLBACK FindGameWindow(HWND h, LPARAM l) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && h != g_wnd) {
        RECT r; GetWindowRect(h, &r);
        if (r.right - r.left > 300) { *(HWND*)l = h; return FALSE; }
    }
    return TRUE;
}
}   // namespace

bool ShowLineupPicker(LineupTeam& home, LineupTeam& away, int* playersMode, int* padTeam) {
    g_mode = playersMode;
    for (int i = 0; i < 4; i++) g_padTeam[i] = padTeam ? padTeam[i] : -1;
    { bool has0 = false, has1 = false; for (int i = 0; i < 4; i++) { has0 |= g_padTeam[i] == 0; has1 |= g_padTeam[i] == 1; } g_two = has0 && has1; }
    g_kbTeam = g_padTeam[0] >= 0 ? g_padTeam[0] : 0;
    g_ready[0] = g_ready[1] = false; g_cur = -1; g_focus[0] = Focus(); g_focus[1] = Focus();
    for (int i = 0; i < 4; i++) { g_portPrev[i] = 0xFFFFFFFF; g_portRep[i] = 0; }
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"MaddenLineups";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc); reg = true;
    }
    if (!g_xget)
        for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryA(dll)) { g_xget = (XGetState)GetProcAddress(m, "XInputGetState"); if (g_xget) break; }
    // size: fit the game window (or the screen), keep the aspect
    HWND game = nullptr; g_wnd = nullptr; EnumWindows(FindGameWindow, (LPARAM)&game);
    RECT area;
    if (game) GetWindowRect(game, &area);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    HMONITOR mon = MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST); MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi);
    IntersectRect(&area, &area, &mi.rcWork);
    int aw = area.right - area.left, ah = area.bottom - area.top;
    g_s = std::clamp(std::min(aw * 0.94 / W, ah * 0.94 / H), 0.6, 2.0);
    int ww = S(W), wh = S(H);
    // fonts: Bahnschrift (Windows 10+) for the sporty look, Segoe UI otherwise
    if (FontExists(L"Bahnschrift")) { g_face = L"Bahnschrift"; g_faceCond = FontExists(L"Bahnschrift SemiBold Condensed") ? L"Bahnschrift SemiBold Condensed" : L"Bahnschrift"; }
    g_fTitle = MakeFont(g_faceCond, 48, FW_BOLD, true);
    g_fBig = MakeFont(g_faceCond, 38, FW_BOLD, true);
    g_fSub = MakeFont(g_face, 15, FW_SEMIBOLD);
    g_fRow = MakeFont(g_face, 15, FW_NORMAL);
    g_fRowB = MakeFont(g_face, 15, FW_BOLD);
    g_fSmall = MakeFont(g_face, 12, FW_BOLD);
    g_fBtn = MakeFont(g_faceCond, 20, FW_BOLD, true);
    g_fOvr = MakeFont(g_faceCond, 16, FW_BOLD);

    LineupTeam* teams[2] = {&home, &away};
    for (int t = 0; t < 2; t++) { g_side[t].team = teams[t]; g_side[t].start = teams[t]->slots; }
    g_team = 0; g_row = 0; g_footer = -1; g_pick = false; g_hoverBtn = -1; g_padPrev = 0; g_repeatBtn = 0;
    g_done = g_accepted = false;
    if (g_two) Use(g_kbTeam);

    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"MaddenLineups", L"Set your starters", WS_POPUP,
                            area.left + (aw - ww) / 2, area.top + (ah - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_wnd, 0, 40, LWA_ALPHA);
    g_openTick = GetTickCount();
    ShowWindow(g_wnd, SW_SHOW); SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    SetTimer(g_wnd, 1, 16, nullptr);
    MSG msg;
    while (!g_done && GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    KillTimer(g_wnd, 1);
    DestroyWindow(g_wnd); g_wnd = nullptr;
    for (HFONT f : {g_fTitle, g_fBig, g_fSub, g_fRow, g_fRowB, g_fSmall, g_fBtn, g_fOvr}) DeleteObject(f);
    if (game) SetForegroundWindow(game);
    if (padTeam) for (int i = 0; i < 4; i++) padTeam[i] = g_two ? g_padTeam[i] : -1;   // the sides as they ended (after any swap)
    return g_accepted;
}
#else
bool ShowLineupPicker(LineupTeam&, LineupTeam&, int*, int*) { return false; }
#endif
