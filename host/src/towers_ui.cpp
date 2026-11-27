// The Towers ladder screen (see towers_ui.h). A borderless custom-drawn window over the game window, like the lineup
// picker: navy night sky, a tower of lit plates (one per opponent) rising to a gold crown, the climb shown on the left.
#include "towers_ui.h"
#include <algorithm>
#include <cmath>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" DWORD WINAPI MauXInputGetState(DWORD slot, void* out);   // sony_pad.cpp: Xbox + PlayStation controllers

namespace {
constexpr int W = 1240, H = 880;
const TowerView* g_v = nullptr;
HWND g_wnd = nullptr; double g_s = 1; DWORD g_open = 0; bool g_done = false; WORD g_prevBtn = 0xFFFF; int g_port = 0; bool g_allowBack = false, g_back = false;
int g_result = 0; bool g_panel = false; int g_panelSel = 0, g_confirmNew = -1;   // tower: Saves panel
// team pick screen
const std::vector<TowerTeam>* g_pick = nullptr; int g_sel = 0, g_hover = -1; WORD g_prevPort[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; DWORD g_repeat[4] = {0, 0, 0, 0};
constexpr int PC = 8, PCW = 140, PCH = 120, PGX = 50, PGY = 200, PGAP = 10;
HFONT g_fHuge, g_fBig, g_fMid, g_fRow, g_fSmall;
std::wstring g_face = L"Segoe UI", g_cond = L"Segoe UI";

int S(double v) { return (int)lround(v * g_s); }
COLORREF C(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
uint32_t Mix(uint32_t a, uint32_t b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto ch = [&](int sh) { return (uint32_t)lround(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}
uint32_t Lift(uint32_t c) {   // team colour made readable on the dark sky
    double l = 0.299 * ((c >> 16) & 255) + 0.587 * ((c >> 8) & 255) + 0.114 * (c & 255);
    return l < 90 ? Mix(c, 0xFFFFFF, (90 - l) / 160.0) : c;
}
std::wstring Wd(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring Up(std::wstring s) { for (auto& c : s) c = (wchar_t)towupper(c); return s; }
void Fill(HDC dc, int x, int y, int w, int h, uint32_t c) { RECT r{S(x), S(y), S(x + w), S(y + h)}; HBRUSH b = CreateSolidBrush(C(c)); FillRect(dc, &r, b); DeleteObject(b); }
void VGrad(HDC dc, int x, int y, int w, int h, uint32_t top, uint32_t bot) {   // vertical gradient in bands
    const int bands = 48;
    for (int i = 0; i < bands; i++) {
        int y0 = y + h * i / bands, y1 = y + h * (i + 1) / bands;
        RECT r{S(x), S(y0), S(x + w), S(y1) + 1}; HBRUSH b = CreateSolidBrush(C(Mix(top, bot, (i + 0.5) / bands))); FillRect(dc, &r, b); DeleteObject(b);
    }
}
void Poly(HDC dc, const POINT* p, int n, uint32_t fill, uint32_t edge, int pen = 1) {
    std::vector<POINT> q(p, p + n); for (auto& pt : q) { pt.x = S(pt.x); pt.y = S(pt.y); }
    HBRUSH b = CreateSolidBrush(C(fill)); HPEN pp = CreatePen(PS_SOLID, std::max(1, S(pen)), C(edge));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pp); Polygon(dc, q.data(), n);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pp);
}
// a plate: a wide bevelled block, narrower at the top of the tower
void Plate(HDC dc, int cx, int y, int w, int h, uint32_t fill, uint32_t edge) {
    int b = h / 3;
    POINT p[6] = {{cx - w / 2 + b, y}, {cx + w / 2 - b, y}, {cx + w / 2, y + h / 2}, {cx + w / 2 - b, y + h}, {cx - w / 2 + b, y + h}, {cx - w / 2, y + h / 2}};
    Poly(dc, p, 6, fill, edge, 2);
}
void Text(HDC dc, const std::wstring& t, int x, int y, int w, int h, HFONT f, uint32_t c, UINT fmt = DT_LEFT) {
    RECT r{S(x), S(y), S(x + w), S(y + h)}; SelectObject(dc, f); SetTextColor(dc, C(c)); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}
void Glow(HDC dc, int x, int y, int w, int h, const std::wstring& t, HFONT f, uint32_t c, uint32_t halo, UINT fmt) {
    for (int dx = -2; dx <= 2; dx += 2) for (int dy = -2; dy <= 2; dy += 2) if (dx || dy) Text(dc, t, x + dx, y + dy, w, h, f, halo, fmt);
    Text(dc, t, x, y, w, h, f, c, fmt);
}
uint32_t OvrCol(int o) { return o >= 90 ? 0xFFC440 : o >= 80 ? 0x3BE27F : o >= 70 ? 0x4FA8FF : 0xA0AECB; }

void PaintPick(HDC dc) {
    double t = (GetTickCount() - g_open) / 1000.0, pulse = 0.5 + 0.5 * sin(t * 4.0);
    VGrad(dc, 0, 0, W, H, 0x050A18, 0x0C1830);
    for (int i = 0; i < 6; i++) Fill(dc, 0, 640 + i * 40, W, 2, Mix(0x0C1830, 0x1D3A66, 0.5));
    Glow(dc, 50, 30, 700, 90, L"TOWERS", g_fHuge, 0xFFFFFF, 0x1E5FB8, DT_LEFT);
    Text(dc, L"PICK YOUR TEAM", 56, 116, 600, 34, g_fMid, 0x4FA8FF, DT_LEFT);
    Text(dc, L"You keep this team for the whole climb.", 56, 148, 700, 28, g_fRow, 0xB8C4DE, DT_LEFT);
    const auto& ts = *g_pick;
    for (int i = 0; i < (int)ts.size(); i++) {
        const TowerTeam& tm = ts[i];
        int x = PGX + (i % PC) * (PCW + PGAP), y = PGY + (i / PC) * (PCH + PGAP);
        bool sel = i == g_sel;
        uint32_t col = Lift(tm.color);
        Fill(dc, x, y, PCW, PCH, sel ? Mix(0x123A72, 0x1E5FB8, pulse) : 0x101A30);
        Fill(dc, x, y, PCW, 8, col);
        if (sel) { Fill(dc, x - 3, y - 3, PCW + 6, 3, 0xBEE6FF); Fill(dc, x - 3, y + PCH, PCW + 6, 3, 0xBEE6FF); Fill(dc, x - 3, y, 3, PCH, 0xBEE6FF); Fill(dc, x + PCW, y, 3, PCH, 0xBEE6FF); }
        Text(dc, Up(Wd(tm.abbr)), x, y + 14, PCW, 44, g_fBig, sel ? 0xFFFFFF : col, DT_CENTER);
        Text(dc, Up(Wd(tm.name)), x + 4, y + 60, PCW - 8, 26, g_fRow, sel ? 0xFFFFFF : 0xB8C4DE, DT_CENTER);
        Text(dc, L"OVR " + std::to_wstring(tm.ovr), x, y + 86, PCW, 26, g_fSmall, OvrCol(tm.ovr), DT_CENTER);
    }
    if (g_sel >= 0 && g_sel < (int)ts.size()) {
        const TowerTeam& tm = ts[g_sel];
        Text(dc, Up(Wd(tm.name)), 760, 40, 440, 60, g_fBig, Lift(tm.color), DT_RIGHT);
        Text(dc, L"TEAM OVR " + std::to_wstring(tm.ovr), 760, 100, 440, 30, g_fMid, OvrCol(tm.ovr), DT_RIGHT);
    }
    uint32_t pc = Mix(0x2F8FFF, 0xBEE6FF, pulse);
    Fill(dc, 50, 760, 520, 64, 0x0E1A33); Fill(dc, 50, 760, 520, 3, pc);
    POINT btn[4] = {{82, 776}, {114, 776}, {114, 808}, {82, 808}}; Poly(dc, btn, 4, 0x2E9E4F, 0xA6F0BE, 2);
    Text(dc, L"A", 82, 776, 32, 32, g_fMid, 0xFFFFFF, DT_CENTER);
    Text(dc, L"CHOOSE TEAM", 132, 760, 420, 64, g_fBig, 0xFFFFFF, DT_LEFT);
    Text(dc, L"D-pad / stick / arrows to move", 600, 776, 600, 32, g_fRow, 0x7F8FB0, DT_LEFT);
}
void Paint(HDC dc) {
    if (g_pick) { PaintPick(dc); return; }
    const TowerView& v = *g_v;
    double t = (GetTickCount() - g_open) / 1000.0;
    double pulse = 0.5 + 0.5 * sin(t * 4.0);
    int n = (int)v.rungs.size();
    bool done = v.rung >= n;
    // sky + stadium light haze + yard lines
    VGrad(dc, 0, 0, W, H, 0x050A18, 0x0C1830);
    for (int i = 0; i < 6; i++) Fill(dc, 0, 620 + i * 44, W, 2, Mix(0x0C1830, 0x1D3A66, 0.5));
    for (int i = 0; i < 3; i++) { POINT beam[3] = {{960 + i * 90, 0}, {760 + i * 160, H}, {980 + i * 160, H}}; Poly(dc, beam, 3, Mix(0x0C1830, 0x16305A, 0.35), Mix(0x0C1830, 0x16305A, 0.35)); }
    // ---- the tower (right) ----
    const int cx = 900, top = 120, bottom = 800;
    int rowH = n ? std::min(26, (bottom - top) / std::max(1, n)) : 26, plateH = rowH - 4;
    auto rungY = [&](int i) { return bottom - (i + 1) * rowH; };   // rung 0 at the bottom
    // spine
    Fill(dc, cx - 4, top - 40, 8, bottom - top + 60, 0x1A2C4E);
    // crown at the top: a gold five-point crown plate
    {
        uint32_t gold = done ? Mix(0xFFC440, 0xFFF1B8, pulse) : 0xC8962E;
        int cy = top - 58;
        POINT crown[9] = {{cx - 70, cy + 44}, {cx - 70, cy + 8}, {cx - 40, cy + 26}, {cx - 18, cy}, {cx, cy + 22}, {cx + 18, cy}, {cx + 40, cy + 26}, {cx + 70, cy + 8}, {cx + 70, cy + 44}};
        Poly(dc, crown, 9, gold, 0xFFF1B8, 2);
        Text(dc, L"CHAMPION", cx - 120, cy + 46, 240, 22, g_fSmall, done ? 0xFFF1B8 : 0xC8962E, DT_CENTER);
    }
    // marker position (animated climb from fromRung to rung)
    double a = std::clamp((t - 0.35) / 1.1, 0.0, 1.0); a = a * a * (3 - 2 * a);
    double mrk = v.fromRung + (std::min(v.rung, n) - v.fromRung) * a;
    for (int i = 0; i < n; i++) {
        const TowerTeam& tm = v.rungs[i];
        int y = rungY(i), w = 520 - i * 240 / std::max(1, n);   // the tower narrows as it rises
        bool beaten = i < v.rung, next = i == v.rung;
        uint32_t base = beaten ? 0x0F3A2A : next ? Mix(0x123A72, 0x1E5FB8, pulse) : 0x111B30;
        uint32_t edge = beaten ? 0x3BE27F : next ? Mix(0x4FA8FF, 0xBEE6FF, pulse) : 0x243656;
        Plate(dc, cx, y, w, plateH, base, edge);
        Fill(dc, cx - w / 2 + plateH / 3 + 6, y + 4, 5, plateH - 8, Lift(tm.color));   // team colour stripe
        Text(dc, std::to_wstring(i + 1), cx - w / 2 + plateH / 3 + 16, y, 30, plateH, g_fSmall, beaten ? 0x3BE27F : 0x7F8FB0, DT_LEFT);
        Text(dc, Up(Wd(tm.name)), cx - w / 2 + plateH / 3 + 46, y, w - plateH - 120, plateH, g_fRow, beaten ? 0x9FE8C0 : next ? 0xFFFFFF : 0xB8C4DE, DT_LEFT);
        Text(dc, beaten ? L"✔" : std::to_wstring(tm.ovr), cx + w / 2 - plateH / 3 - 52, y, 40, plateH, g_fRow, beaten ? 0x3BE27F : OvrCol(tm.ovr), DT_RIGHT);
    }
    // your marker: a chevron beside the tower at the current height
    {
        double my = done && a >= 1 ? top - 40 : bottom - (mrk + 0.5) * rowH;
        int w = 520 - (int)(std::min(mrk, (double)std::max(0, n - 1)) * 240 / std::max(1, n));
        int mx = cx - w / 2 - 22;
        POINT ch[3] = {{mx - 26, (int)my - 14}, {mx, (int)my}, {mx - 26, (int)my + 14}};
        Poly(dc, ch, 3, Lift(v.me.color), 0xFFFFFF, 2);
        Text(dc, Up(Wd(v.me.abbr.empty() ? v.me.name : v.me.abbr)), mx - 140, (int)my - 14, 108, 28, g_fRow, 0xFFFFFF, DT_RIGHT);
    }
    // base: your team
    Plate(dc, cx, bottom + 8, 600, 44, Mix(0x0C1830, Lift(v.me.color), 0.35), Lift(v.me.color));
    Text(dc, Up(Wd(v.me.name)), cx - 280, bottom + 8, 560, 44, g_fMid, 0xFFFFFF, DT_CENTER);

    // ---- the climb (left) ----
    Glow(dc, 60, 40, 560, 90, L"TOWERS", g_fHuge, 0xFFFFFF, 0x1E5FB8, DT_LEFT);
    Text(dc, L"THE GRIDIRON CLIMB", 66, 126, 560, 30, g_fMid, 0x4FA8FF, DT_LEFT);
    Fill(dc, 64, 166, 470, 3, 0x2F8FFF);
    Text(dc, L"YOUR TEAM", 64, 196, 470, 24, g_fSmall, 0x7F8FB0, DT_LEFT);
    Text(dc, Up(Wd(v.me.name)), 64, 220, 470, 48, g_fBig, Lift(v.me.color), DT_LEFT);
    wchar_t rec[96]; swprintf(rec, 96, L"RECORD  %d - %d        OVR %d", v.wins, v.losses, v.me.ovr);
    Text(dc, rec, 64, 270, 470, 28, g_fRow, 0xDDE6F7, DT_LEFT);
    if (!v.note.empty()) Text(dc, Wd(v.note), 64, 316, 520, 30, g_fMid, v.note.rfind("WIN", 0) == 0 ? 0x3BE27F : 0xFF9A5A, DT_LEFT);
    // next opponent card
    Fill(dc, 64, 370, 470, 230, 0x0E1A33); Fill(dc, 64, 370, 6, 230, done ? 0xFFC440 : 0x2F8FFF);
    if (done) {
        Glow(dc, 90, 400, 430, 70, L"CHAMPION!", g_fBig, 0xFFC440, 0x5A3E0A, DT_LEFT);
        Text(dc, L"You beat every team. Delete towers_state.txt", 92, 480, 430, 26, g_fRow, 0xDDE6F7, DT_LEFT);
        Text(dc, L"in the mods folder to climb again.", 92, 506, 430, 26, g_fRow, 0xDDE6F7, DT_LEFT);
    } else {
        const TowerTeam& op = v.rungs[v.rung];
        wchar_t g[64]; swprintf(g, 64, L"GAME %d OF %d", v.rung + 1, n);
        Text(dc, g, 92, 386, 420, 26, g_fSmall, 0x4FA8FF, DT_LEFT);
        Text(dc, L"NEXT OPPONENT", 92, 412, 420, 26, g_fSmall, 0x7F8FB0, DT_LEFT);
        Text(dc, Up(Wd(op.name)), 92, 440, 420, 60, g_fBig, Lift(op.color), DT_LEFT);
        wchar_t o[48]; swprintf(o, 48, L"TEAM OVR %d", op.ovr);
        Text(dc, o, 92, 500, 420, 30, g_fMid, OvrCol(op.ovr), DT_LEFT);
        Text(dc, L"Win to climb.  Lose and you face them again.", 92, 540, 430, 26, g_fRow, 0xB8C4DE, DT_LEFT);
    }
    // continue prompt
    uint32_t pc = Mix(0x2F8FFF, 0xBEE6FF, pulse);
    Fill(dc, 64, 650, 470, 64, 0x0E1A33); Fill(dc, 64, 650, 470, 3, pc);
    POINT btn[4] = {{96, 666}, {128, 666}, {128, 698}, {96, 698}};
    Poly(dc, btn, 4, 0x2E9E4F, 0xA6F0BE, 2);
    Text(dc, L"A", 96, 666, 32, 32, g_fMid, 0xFFFFFF, DT_CENTER);
    Text(dc, done ? L"CONTINUE" : L"PLAY", 146, 650, 360, 64, g_fBig, 0xFFFFFF, DT_LEFT);
    Text(dc, g_allowBack ? L"or Enter / Start / click        B / Esc: back to the main menu" : L"or Enter / Start / click", 66, 724, 520, 24, g_fSmall, 0x7F8FB0, DT_LEFT);
    if (!v.slots.empty()) {
        POINT yb[4] = {{66, 758}, {94, 758}, {94, 786}, {66, 786}}; Poly(dc, yb, 4, 0xB8860B, 0xFFE08A, 2);
        Text(dc, L"Y", 66, 758, 28, 28, g_fRow, 0xFFFFFF, DT_CENTER);
        wchar_t sl[96]; swprintf(sl, 96, L"SAVES / NEW TOWER      (slot %d, saved after every game)", v.slot + 1);
        Text(dc, sl, 104, 758, 480, 28, g_fRow, 0xDDE6F7, DT_LEFT);
    }
    if (g_panel && !v.slots.empty()) {   // the Saves panel
        Fill(dc, 0, 0, W, H, 0x050A18);
        Fill(dc, 240, 150, 760, 560, 0x0E1A33); Fill(dc, 240, 150, 760, 4, 0x2F8FFF);
        Glow(dc, 280, 170, 680, 70, L"SAVES", g_fBig, 0xFFFFFF, 0x1E5FB8, DT_LEFT);
        Text(dc, L"Every climb is saved after each game. Pick one to play, or start a new tower.", 282, 236, 700, 26, g_fRow, 0xB8C4DE, DT_LEFT);
        for (int i = 0; i < (int)v.slots.size(); i++) {
            const TowerSlot& sl = v.slots[i];
            int y = 290 + i * 110; bool sel = i == g_panelSel;
            Fill(dc, 280, y, 680, 96, sel ? Mix(0x123A72, 0x1E5FB8, pulse) : 0x111B30);
            if (sel) { Fill(dc, 280, y, 6, 96, 0xBEE6FF); }
            Text(dc, L"SLOT " + std::to_wstring(i + 1) + (i == v.slot ? L"  (current)" : L""), 304, y + 8, 400, 26, g_fSmall, 0x4FA8FF, DT_LEFT);
            if (sl.used) {
                Text(dc, Up(Wd(sl.team)), 304, y + 32, 420, 50, g_fBig, 0xFFFFFF, DT_LEFT);
                wchar_t r[96];
                if (sl.done) swprintf(r, 96, L"CHAMPION  -  %d-%d", sl.wins, sl.losses);
                else swprintf(r, 96, L"%d-%d   GAME %d OF %d", sl.wins, sl.losses, sl.rung + 1, sl.total);
                Text(dc, r, 640, y + 32, 300, 50, g_fMid, sl.done ? 0xFFC440 : 0xDDE6F7, DT_RIGHT);
            } else Text(dc, L"EMPTY  -  START A NEW TOWER", 304, y + 32, 640, 50, g_fMid, 0x7F8FB0, DT_LEFT);
        }
        std::wstring hint = L"A  PLAY THIS SLOT      X  NEW TOWER HERE      B  CLOSE";
        if (g_confirmNew >= 0) hint = L"PRESS X AGAIN to start over in slot " + std::to_wstring(g_confirmNew + 1) + L" (its climb is replaced)";
        Text(dc, hint, 282, 640, 700, 40, g_fMid, g_confirmNew >= 0 ? 0xFF9A5A : 0xDDE6F7, DT_LEFT);
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC wdc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc); HBITMAP bm = CreateCompatibleBitmap(wdc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(dc, bm);
        Paint(dc);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob); DeleteObject(bm); DeleteDC(dc); EndPaint(h, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        DWORD el = GetTickCount() - g_open;
        SetLayeredWindowAttributes(h, 0, (BYTE)std::min<DWORD>(255, 40 + el), LWA_ALPHA);   // fade in
        struct XS { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st{};
        for (DWORD p = 0; p < 4; p++) {
            if (MauXInputGetState(p, &st) != 0) { g_prevPort[p] = 0xFFFF; continue; }
            WORD b = st.buttons;
            if (st.lx < -16000) b |= 0x0004; if (st.lx > 16000) b |= 0x0008; if (st.ly > 16000) b |= 0x0001; if (st.ly < -16000) b |= 0x0002;
            WORD prev = g_prevPort[p]; g_prevPort[p] = b;
            if (prev == 0xFFFF || el < 500) continue;
            WORD down = b & ~prev;
            if (g_pick) {   // d-pad / stick: move (repeats while held)
                WORD dir = b & 0x000F;
                if ((down & 0x000F) || (dir && GetTickCount() - g_repeat[p] > 140 && g_repeat[p])) {
                    int n = (int)g_pick->size(), r = g_sel / PC, c = g_sel % PC;
                    if (dir & 0x0004) c = (c + PC - 1) % PC; if (dir & 0x0008) c = (c + 1) % PC;
                    if (dir & 0x0001) r = r - 1; if (dir & 0x0002) r = r + 1;
                    int rows = (n + PC - 1) / PC; r = (r + rows) % rows; g_sel = std::min(n - 1, r * PC + c);
                    g_repeat[p] = GetTickCount() + ((down & 0x000F) ? 250 : 0);
                }
                if (!dir) g_repeat[p] = 0;
            }
            if (!g_pick && g_v) {   // tower: Y opens the Saves panel; in it up/down, A load / new, X new tower, B close
                int ns = (int)g_v->slots.size();
                if (g_panel) {
                    if (down & 0x0001) { g_panelSel = (g_panelSel + ns - 1) % ns; g_confirmNew = -1; }
                    if (down & 0x0002) { g_panelSel = (g_panelSel + 1) % ns; g_confirmNew = -1; }
                    if (down & 0x1000) { g_result = g_v->slots[g_panelSel].used ? kTowerLoad + g_panelSel : kTowerNew + g_panelSel; g_done = true; g_port = (int)p; }
                    if (down & 0x4000) { if (!g_v->slots[g_panelSel].used || g_confirmNew == g_panelSel) { g_result = kTowerNew + g_panelSel; g_done = true; g_port = (int)p; } else g_confirmNew = g_panelSel; }
                    if (down & (0x2000 | 0x8000)) { g_panel = false; g_confirmNew = -1; }
                    continue;
                }
                if ((down & 0x8000) && ns) { g_panel = true; g_panelSel = g_v->slot; g_confirmNew = -1; continue; }
            }
            if (down & (0x1000 | 0x0010)) { g_done = true; g_port = (int)p; g_result = kTowerPlay; }   // A or Start
            if (g_allowBack && (down & 0x2000)) { g_done = true; g_back = true; g_port = (int)p; g_result = kTowerBack; }   // B: back
        }
        InvalidateRect(h, nullptr, FALSE); return 0;
    }
    case WM_KEYDOWN:
        if (g_pick) {
            int n = (int)g_pick->size();
            if (w == VK_LEFT) g_sel = (g_sel + n - 1) % n; if (w == VK_RIGHT) g_sel = (g_sel + 1) % n;
            if (w == VK_UP) g_sel = (g_sel + n - PC) % n; if (w == VK_DOWN) g_sel = (g_sel + PC) % n;
        }
        if (!g_pick && g_v && !g_v->slots.empty()) {
            int ns = (int)g_v->slots.size();
            if (!g_panel && (w == 'S' || w == 'Y')) { g_panel = true; g_panelSel = g_v->slot; g_confirmNew = -1; return 0; }
            if (g_panel) {
                if (w == VK_UP) g_panelSel = (g_panelSel + ns - 1) % ns; if (w == VK_DOWN) g_panelSel = (g_panelSel + 1) % ns;
                if (w == VK_RETURN || w == VK_SPACE) { g_result = g_v->slots[g_panelSel].used ? kTowerLoad + g_panelSel : kTowerNew + g_panelSel; g_done = true; }
                if (w == 'N' || w == 'X') { if (!g_v->slots[g_panelSel].used || g_confirmNew == g_panelSel) { g_result = kTowerNew + g_panelSel; g_done = true; } else g_confirmNew = g_panelSel; }
                if (w == VK_ESCAPE || w == VK_BACK || w == 'S' || w == 'Y') { g_panel = false; g_confirmNew = -1; }
                return 0;
            }
        }
        if ((w == VK_RETURN || w == VK_SPACE) && GetTickCount() - g_open > 400) { g_done = true; g_port = 0; g_result = kTowerPlay; }
        if (g_allowBack && (w == VK_ESCAPE || w == VK_BACK) && GetTickCount() - g_open > 400) { g_done = true; g_back = true; g_port = 0; g_result = kTowerBack; }
        return 0;
    case WM_MOUSEMOVE:
        if (g_pick) { int mx = (int)(short)LOWORD(l), my = (int)(short)HIWORD(l);
            for (int i = 0; i < (int)g_pick->size(); i++) { int x = PGX + (i % PC) * (PCW + PGAP), y = PGY + (i / PC) * (PCH + PGAP);
                if (mx >= S(x) && mx < S(x + PCW) && my >= S(y) && my < S(y + PCH)) { g_sel = i; break; } } }
        return 0;
    case WM_LBUTTONUP: if (!g_panel && GetTickCount() - g_open > 400) { g_done = true; g_port = 0; g_result = kTowerPlay; } return 0;
    case WM_CLOSE: g_done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
int CALLBACK HasFont(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM l) { *(bool*)l = true; return 0; }
bool FontExists(const wchar_t* face) { bool f = false; HDC dc = GetDC(nullptr); LOGFONTW lf{}; wcsncpy(lf.lfFaceName, face, 31); lf.lfCharSet = DEFAULT_CHARSET; EnumFontFamiliesExW(dc, &lf, HasFont, (LPARAM)&f, 0); ReleaseDC(nullptr, dc); return f; }
HFONT Mk(const std::wstring& face, double px, int weight, bool italic = false) {
    return CreateFontW(-S(px), 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, face.c_str());
}
BOOL CALLBACK FindGame(HWND h, LPARAM l) {
    wchar_t t[128]; GetWindowTextW(h, t, 128);
    if (IsWindowVisible(h) && wcsstr(t, L"Madden Arcade Unleashed")) { DWORD pid; GetWindowThreadProcessId(h, &pid); if (pid == GetCurrentProcessId()) { *(HWND*)l = h; return FALSE; } }
    return TRUE;
}

void Run() {
    g_done = false; g_back = false; g_result = kTowerPlay; g_panel = false; g_confirmNew = -1; g_prevBtn = 0xFFFF; g_port = 0; for (int i = 0; i < 4; i++) { g_prevPort[i] = 0xFFFF; g_repeat[i] = 0; }
    static bool reg = false; HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"MaddenTowers"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc); reg = true; }
    HWND game = nullptr; EnumWindows(FindGame, (LPARAM)&game);
    RECT area; if (game) GetWindowRect(game, &area); else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    HMONITOR mon = MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST); MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi);
    IntersectRect(&area, &area, &mi.rcMonitor);
    int aw = area.right - area.left, ah = area.bottom - area.top;
    g_s = std::clamp(std::min(aw * 0.96 / W, ah * 0.96 / H), 0.5, 3.0);
    if (FontExists(L"Bahnschrift")) { g_face = L"Bahnschrift"; g_cond = FontExists(L"Bahnschrift SemiBold Condensed") ? L"Bahnschrift SemiBold Condensed" : L"Bahnschrift"; }
    g_fHuge = Mk(g_cond, 92, FW_BOLD, true); g_fBig = Mk(g_cond, 44, FW_BOLD, true); g_fMid = Mk(g_face, 22, FW_SEMIBOLD);
    g_fRow = Mk(g_face, 16, FW_SEMIBOLD); g_fSmall = Mk(g_face, 13, FW_BOLD);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"MaddenTowers", L"Towers", WS_POPUP,
                            area.left + (aw - S(W)) / 2, area.top + (ah - S(H)) / 2, S(W), S(H), nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_wnd, 0, 40, LWA_ALPHA);
    g_open = GetTickCount();
    ShowWindow(g_wnd, SW_SHOW); SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    SetTimer(g_wnd, 1, 16, nullptr);
    MSG msg;
    while (!g_done && GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    KillTimer(g_wnd, 1); DestroyWindow(g_wnd); g_wnd = nullptr;
    for (HFONT f : {g_fHuge, g_fBig, g_fMid, g_fRow, g_fSmall}) DeleteObject(f);
    if (game) SetForegroundWindow(game);
}
}   // namespace

int ShowTowerLadder(const TowerView& v, int* port, bool allowBack) { g_v = &v; g_pick = nullptr; g_allowBack = allowBack; Run(); g_allowBack = false; g_v = nullptr; if (port) *port = g_port; return g_result; }
int ShowTeamPick(const std::vector<TowerTeam>& teams, int* port) {
    if (teams.empty()) return -1;
    g_pick = &teams; g_v = nullptr; g_sel = 0; Run(); g_pick = nullptr;
    if (port) *port = g_port;
    return g_sel;
}
#else
int ShowTowerLadder(const TowerView&, int*, bool) { return 0; }
int ShowTeamPick(const std::vector<TowerTeam>&, int*) { return -1; }
#endif
