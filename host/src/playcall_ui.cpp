// Full playbook box and on-screen button (see playcall_ui.h). Custom-drawn, in the style of the lineup picker.
//   LB / RB (Q / E, up / down): formation   left / right: play   LT / RT: 4 plays back / on
//   A / Enter / click: call the play   B / Esc: close (the arcade play call stays)
#include "playcall_ui.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>

bool PcParse(const std::vector<std::string>& c, PcPlay& p) {
    if (c.size() < 3) return false;
    p.form = c[0]; p.name = c[1]; p.sets = c.size() > 2 ? c[2] : "";
    p.gform = c.size() > 4 && !c[4].empty() ? c[4] : c[0];
    p.shown.clear();
    for (size_t i = 0; i < p.name.size(); i++) {   // "%L" / "%R" style markers are not shown
        if (p.name[i] == '%' && i + 1 < p.name.size()) { i++; continue; }
        p.shown += p.name[i];
    }
    p.art.clear();
    if (c.size() > 3) {
        const std::string& a = c[3];
        size_t s = 0;
        while (s < a.size()) {
            size_t e = a.find(';', s); if (e == std::string::npos) e = a.size();
            std::string g = a.substr(s, e - s); s = e + 1;
            PcArtPlayer pl{};
            size_t colon = g.find(':');
            if (sscanf(g.substr(0, colon).c_str(), "%d,%d,%d", &pl.x, &pl.y, &pl.pos) != 3) continue;
            if (colon != std::string::npos) {
                const char* q = g.c_str() + colon + 1;
                while (*q) {
                    PcArtPoint pt{}; int n = 0;
                    if (sscanf(q, "%d,%d,%d%n", &pt.dx, &pt.dy, &pt.end, &n) != 3) break;
                    pl.pts.push_back(pt); q += n; while (*q == ' ') q++;
                }
            }
            p.art.push_back(pl);
        }
    }
    int routes = 0;   // pass play: two or more backs / receivers / tight ends run a route (an arrow 15+ units long)
    for (auto& pl : p.art) {
        if (pl.pos < 1 || pl.pos > 4) continue;
        for (auto& q : pl.pts) if (q.end == 2 && std::abs(q.dx) + std::abs(q.dy) >= 15) { routes++; break; }
    }
    p.pass = routes >= 2;
    if (c.size() > 5 && !c[5].empty()) p.pass = c[5] == "P";   // the play type from the playbook
    return !p.form.empty() && !p.name.empty();
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
constexpr int W = 1240, H = 474;
constexpr int CX = 28, CY = 84, CW = 284, CH = 318, CG = 18, ART_H = 246, NCARD = 4;
constexpr uint32_t BG_TOP = 0x081634, BG_BOT = 0x02060F, PANEL = 0x061127, GLOW = 0x2F9BFF, GLOW_HI = 0x8EE3FF,
                   SILVER_HI = 0xF2F5FA, SILVER_LO = 0x7D8796, TEXT = 0xEAF2FF, TEXT_DIM = 0x8A9BBF, TAG = 0x0B0F1A,
                   FIELD = 0x0E3B2A, FIELD2 = 0x0B3123;
const std::vector<PcPlay>* g_list;
std::vector<std::string> g_forms; std::vector<std::vector<int>> g_fplays;
int g_side, g_form, g_sel, g_first, g_result;
double g_bx0, g_bx1, g_by0, g_by1;   // play-art bounds of this side (art units)
HWND g_wnd; double g_s = 1; bool g_done;   // g_done: the current box is done
// One box, or two next to each other (two players: offense left, defense right, each on its own pad). Each box's state
// is swapped into the globals above while it is drawn or its input is handled.
struct Box {
    const std::vector<PcPlay>* list; std::vector<std::string> forms; std::vector<std::vector<int>> fplays;
    int side, form, sel, first, result, port; double bx0, bx1, by0, by1; bool done, open, told;
    void (*onPick)(int, int, int);
};
Box g_boxes[2]; int g_nbox = 1, g_cur = -1;
int g_ncard = NCARD, g_bw = W, g_ox = 0, g_oy = 0, g_cw = CW;
constexpr int BOXH = 410;   // two players: each playbook gets the full width, one above the other
int TH() { return g_nbox > 1 ? 2 * BOXH + 46 : H; }
void SaveBox() {
    if (g_cur < 0) return;
    Box& b = g_boxes[g_cur];
    b.list = g_list; b.forms = g_forms; b.fplays = g_fplays; b.side = g_side; b.form = g_form; b.sel = g_sel; b.first = g_first;
    b.result = g_result; b.bx0 = g_bx0; b.bx1 = g_bx1; b.by0 = g_by0; b.by1 = g_by1; b.done = g_done;
}
void UseBox(int i) {
    if (g_cur == i) return;
    SaveBox();
    Box& b = g_boxes[i];
    g_list = b.list; g_forms = b.forms; g_fplays = b.fplays; g_side = b.side; g_form = b.form; g_sel = b.sel; g_first = b.first;
    g_result = b.result; g_bx0 = b.bx0; g_bx1 = b.bx1; g_by0 = b.by0; g_by1 = b.by1; g_done = b.done;
    g_cur = i;
    g_bw = W; g_ox = 0; g_oy = g_nbox > 1 ? i * BOXH : 0; g_ncard = g_nbox == 1 ? NCARD : 3;
    g_cw = g_nbox == 1 ? CW : (g_bw - 2 * CX - 2 * CG) / 3;
}
bool AllDone() { SaveBox(); for (int i = 0; i < g_nbox; i++) if (!g_boxes[i].done) return false; return true; }
}   // namespace
extern std::atomic<uint32_t> g_padMuteMask, g_padMuteRelease;
namespace {
// a box finished: close it, give its pad back (once its buttons are let go), tell the caller
void Finish() {
    for (int i = 0; i < g_nbox; i++) {
        SaveBox();
        Box& b = g_boxes[i];
        if (!b.done || b.told) continue;
        b.told = true; b.open = false;
        if (b.port >= 0 && b.port < 4) { g_padMuteRelease |= 1u << b.port; g_padMuteMask &= ~(1u << b.port); }
        if (b.onPick) b.onPick(b.side, b.result, b.port);
    }
}
bool AnyOpen() { SaveBox(); for (int i = 0; i < g_nbox; i++) if (g_boxes[i].open && !g_boxes[i].done) return true; return false; }
void OpenBox(int i) {
    Box& b = g_boxes[i];
    if (b.open || b.done) return;
    b.open = true;
    if (b.port >= 0 && b.port < 4) g_padMuteMask |= 1u << b.port;
}
DWORD g_openTick;
HFONT g_fTitle, g_fForm, g_fName, g_fSmall, g_fSub;

int S(double v) { return (int)lround(v * g_s); }
COLORREF RGBc(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
uint32_t Mix(uint32_t a, uint32_t b, double t) {
    auto ch = [&](int sh) { return (uint32_t)lround(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring Upper(std::wstring s) { for (auto& c : s) c = (wchar_t)towupper(c); return s; }
void Fill(HDC dc, int x, int y, int w, int h, uint32_t c) { RECT r{x, y, x + w, y + h}; HBRUSH b = CreateSolidBrush(RGBc(c)); FillRect(dc, &r, b); DeleteObject(b); }
void VGrad(HDC dc, int x, int y, int w, int h, uint32_t a, uint32_t b) { for (int i = 0; i < h; i += 2) Fill(dc, x, y + i, w, 2, Mix(a, b, h > 1 ? double(i) / (h - 1) : 0)); }
void HGrad(HDC dc, int x, int y, int w, int h, uint32_t a, uint32_t b) { for (int i = 0; i < w; i += 2) Fill(dc, x + i, y, 2, h, Mix(a, b, w > 1 ? double(i) / (w - 1) : 0)); }
void Frame(HDC dc, int x, int y, int w, int h, uint32_t c, int t) { Fill(dc, x, y, w, t, c); Fill(dc, x, y + h - t, w, t, c); Fill(dc, x, y, t, h, c); Fill(dc, x + w - t, y, t, h, c); }
void Text(HDC dc, const std::wstring& t, int x, int y, int w, int h, HFONT f, uint32_t c, UINT fmt = DT_LEFT) {
    SelectObject(dc, f); SetTextColor(dc, RGBc(c)); SetBkMode(dc, TRANSPARENT);
    RECT r{x, y, x + w, y + h}; DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}
void OutlineText(HDC dc, const std::wstring& t, int x, int y, int w, int h, HFONT f, uint32_t fill, uint32_t edge, UINT fmt = DT_LEFT) {
    int o = std::max(1, S(2));
    for (int dx = -o; dx <= o; dx += o) for (int dy = -o; dy <= o; dy += o) if (dx || dy) Text(dc, t, x + dx, y + dy, w, h, f, edge, fmt);
    Text(dc, t, x, y, w, h, f, fill, fmt);
}
void Glow(HDC dc, int x, int y, int w, int h, uint32_t c, uint32_t bg, int layers) {
    for (int i = layers; i >= 1; i--) Frame(dc, x - i, y - i, w + 2 * i, h + 2 * i, Mix(bg, c, 0.6 * (1.0 - double(i) / (layers + 1))), 1);
    Frame(dc, x, y, w, h, c, std::max(1, S(2)));
}
void Chrome(HDC dc, int x, int y, int w, int h) {
    int half = h / 2; VGrad(dc, x, y, w, half, SILVER_HI, 0xBFC6D1); VGrad(dc, x, y + half, w, h - half, 0x9AA3B1, SILVER_LO);
    Fill(dc, x, y, w, 1, 0xFFFFFF); Fill(dc, x, y + h - 1, w, 1, 0x4A5260);
}
int Key(HDC dc, const std::wstring& k, int x, int cy, HFONT f = nullptr) {
    if (!f) f = g_fSmall;
    SelectObject(dc, f); SIZE sz; GetTextExtentPoint32W(dc, k.c_str(), (int)k.size(), &sz);
    int w = std::max<int>(sz.cx + S(12), S(26)), h = S(22);
    Fill(dc, x, cy - h / 2, w, h, TAG); Frame(dc, x, cy - h / 2, w, h, 0x6A7690, 1);
    Text(dc, k, x, cy - h / 2, w, h, f, 0xFFFFFF, DT_CENTER);
    return w;
}
void Line(HDC dc, int x0, int y0, int x1, int y1, uint32_t c, int w) {
    HPEN p = CreatePen(PS_SOLID, std::max(1, w), RGBc(c)); HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1); SelectObject(dc, op); DeleteObject(p);
}
void Disc(HDC dc, int x, int y, int r, uint32_t fill, uint32_t edge) {
    HBRUSH b = CreateSolidBrush(RGBc(fill)); HPEN p = CreatePen(PS_SOLID, std::max(1, S(1.5)), RGBc(edge));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
void Arrow(HDC dc, double x0, double y0, double x1, double y1, uint32_t c, int w) {
    double dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy);
    if (l < 0.5) return;
    dx /= l; dy /= l;
    double a = S(7), b = S(4.5);
    POINT p[3] = {{(LONG)lround(x1 + dx * S(2)), (LONG)lround(y1 + dy * S(2))},
                  {(LONG)lround(x1 - dx * a - dy * b), (LONG)lround(y1 - dy * a + dx * b)},
                  {(LONG)lround(x1 - dx * a + dy * b), (LONG)lround(y1 - dy * a - dx * b)}};
    HBRUSH br = CreateSolidBrush(RGBc(c)); HPEN pn = CreatePen(PS_SOLID, 1, RGBc(c));
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pn); Polygon(dc, p, 3);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pn);
    (void)w;
}
const uint32_t kRoute[] = {0xFFD23F, 0x36C9FF, 0xFF6B6B, 0x7CFF6B, 0xD08CFF, 0xFF9F43};

// one play diagram in the box (x, y, w, h)
void DrawArt(HDC dc, const PcPlay& p, int x, int y, int w, int h) {
    VGrad(dc, x, y, w, h, FIELD, FIELD2);
    double pad = S(16);
    double sx = (w - 2 * pad) / std::max(1.0, g_bx1 - g_bx0), sy = (h - 2 * pad) / std::max(1.0, g_by1 - g_by0);
    double sc = std::min(sx, sy);
    double ox = x + (w - (g_bx1 - g_bx0) * sc) / 2 - g_bx0 * sc, oy = y + (h - (g_by1 - g_by0) * sc) / 2 - g_by0 * sc;
    auto X = [&](double v) { return ox + v * sc; };
    auto Y = [&](double v) { return oy + v * sc; };
    // yard lines and the line of scrimmage
    for (int k = 1; k < 5; k++) Fill(dc, x, y + h * k / 5, w, 1, 0x1C5A40);
    int los = 0, nl = 0;
    for (auto& pl : p.art) { bool line = g_side == 0 ? (pl.pos >= 5 && pl.pos <= 9) : (pl.pos >= 10 && pl.pos <= 12); if (line) { los += pl.y; nl++; } }
    if (nl) { double ly = g_side == 0 ? los / double(nl) - 5 : los / double(nl) + 6; Fill(dc, x + S(6), (int)Y(ly), w - S(12), std::max(1, S(2)), 0x9FC3FF); }
    int ci = 0;
    for (auto& pl : p.art) {
        bool lineman = g_side == 0 ? (pl.pos >= 5 && pl.pos <= 9) : false;
        uint32_t col = 0xD8E2F0;
        bool route = false;
        for (auto& pt : pl.pts) if (pt.end >= 2 || std::abs(pt.dy) > 8 || std::abs(pt.dx) > 8) route = true;
        if (route && !lineman) col = g_side == 0 ? kRoute[ci++ % 6] : 0xFF5A5A;
        double px = X(pl.x), py = Y(pl.y);
        int lw = std::max(2, S(2.4));
        for (size_t i = 0; i < pl.pts.size(); i++) {
            const PcArtPoint& q = pl.pts[i];
            double qx = X(pl.x + q.dx), qy = Y(pl.y + q.dy);
            uint32_t c = col;
            if (g_side == 1 && q.end == 4) c = q.dy < -18 ? 0x3FA8FF : 0xFFD23F;   // zone drop: deep blue / short yellow
            if (std::abs(qx - px) + std::abs(qy - py) > 0.5) Line(dc, (int)px, (int)py, (int)qx, (int)qy, c, lw);
            if (i + 1 == pl.pts.size() || q.end) {
                if (q.end == 1) {   // block: a bar across the end
                    double dx = qx - px, dy = qy - py, l = std::sqrt(dx * dx + dy * dy);
                    if (l < 0.5) { dx = 0; dy = -1; l = 1; }
                    dx /= l; dy /= l;
                    Line(dc, (int)(qx - dy * S(6)), (int)(qy + dx * S(6)), (int)(qx + dy * S(6)), (int)(qy - dx * S(6)), c, lw);
                } else if (q.end == 2 || q.end == 3) Arrow(dc, px, py, qx, qy, c, lw);
                else if (q.end == 4) {   // zone bubble
                    HBRUSH b = CreateSolidBrush(RGBc(Mix(FIELD, c, 0.45))); HPEN pn = CreatePen(PS_SOLID, std::max(1, S(1.5)), RGBc(c));
                    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pn);
                    Ellipse(dc, (int)qx - S(13), (int)qy - S(8), (int)qx + S(13), (int)qy + S(8));
                    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pn);
                }
            }
            px = qx; py = qy;
        }
        int r = S(5.5);
        if (g_side == 0) Disc(dc, (int)X(pl.x), (int)Y(pl.y), r, pl.pos == 0 ? 0xFFFFFF : (lineman ? 0x9AA8BF : col), 0x0A1A38);
        else { int cx = (int)X(pl.x), cy = (int)Y(pl.y); Line(dc, cx - r, cy - r, cx + r, cy + r, col == 0xD8E2F0 ? 0xFFFFFF : col, S(2.6)); Line(dc, cx - r, cy + r, cx + r, cy - r, col == 0xD8E2F0 ? 0xFFFFFF : col, S(2.6)); }
    }
}

const std::vector<int>& Plays() { static std::vector<int> none; return g_fplays.empty() ? none : g_fplays[g_form]; }
void Keep() {
    int n = (int)Plays().size();
    g_sel = std::clamp(g_sel, 0, std::max(0, n - 1));
    if (g_sel < g_first) g_first = g_sel;
    if (g_sel >= g_first + g_ncard) g_first = g_sel - g_ncard + 1;
    g_first = std::clamp(g_first, 0, std::max(0, n - g_ncard));
}
int FW() { return 460; }   // formation selector width
RECT CardRect(int k) { int x = g_ox + CX + k * (g_cw + CG); return RECT{S(x), S(g_oy + CY), S(x + g_cw), S(g_oy + CY + CH)}; }
RECT FormArrow(int dir) { int c = g_ox + g_bw / 2, h = FW() / 2; return dir < 0 ? RECT{S(c - h - 70), S(g_oy + 16), S(c - h), S(g_oy + 60)} : RECT{S(c + h), S(g_oy + 16), S(c + h + 70), S(g_oy + 60)}; }

void PaintBox(HDC dc) {
    int ox = g_ox, bw = g_bw;
    if (g_nbox > 1 && !g_boxes[g_cur].open && !g_done) {   // this player has not opened his playbook
        int y = S(g_oy + CY + CH / 2 - 40), h = S(80), x = S(ox + 40), w = S(bw - 80);
        Fill(dc, x, y, w, h, 0x061127); Frame(dc, x, y, w, h, 0x2A3A5C, 1);
        std::wstring t = L"PLAYER " + std::to_wstring(g_boxes[g_cur].port + 1) + (g_side ? L" (DEFENSE)" : L" (OFFENSE)");
        OutlineText(dc, t, x, y + S(6), w, S(34), g_fName, 0xFFFFFF, 0x0A2350, DT_CENTER);
        Text(dc, L"PRESS SELECT FOR YOUR PLAYBOOK", x, y + S(42), w, S(26), g_fSub, TEXT_DIM, DT_CENTER);
        return;
    }
    if (g_nbox == 1) {
        OutlineText(dc, L"FULL PLAYBOOK", S(ox + 26), S(g_oy + 10), S(360), S(40), g_fTitle, 0xFFFFFF, 0x0A2350);
        Text(dc, g_side ? L"DEFENSE" : L"OFFENSE", S(ox + 30), S(g_oy + 46), S(300), S(20), g_fSub, GLOW_HI);
    } else {
        std::wstring who = L"PLAYER " + std::to_wstring(g_boxes[g_cur].port + 1);
        OutlineText(dc, g_side ? L"DEFENSE" : L"OFFENSE", S(ox + 20), S(g_oy + 10), S(200), S(36), g_fForm, 0xFFFFFF, 0x0A2350);
        Text(dc, who, S(ox + 22), S(g_oy + 46), S(200), S(20), g_fSub, GLOW_HI);
    }
    // formation selector
    int fc = ox + bw / 2, fw = FW();
    Chrome(dc, S(fc - fw / 2), S(g_oy + 16), S(fw), S(44));
    Text(dc, g_forms.empty() ? L"-" : Upper(Wide(g_forms[g_form])), S(fc - fw / 2 + 10), S(g_oy + 16), S(fw - 20), S(44), g_fForm, 0x0A1A38, DT_CENTER);
    Key(dc, L"LB", S(fc - fw / 2 - 60), S(g_oy + 38), g_fSub); Key(dc, L"RB", S(fc + fw / 2 + 20), S(g_oy + 38), g_fSub);
    Text(dc, L"FORMATION " + std::to_wstring(g_form + 1) + L" / " + std::to_wstring(g_forms.size()), S(fc - fw / 2), S(g_oy + 60), S(fw), S(14), g_fSmall, TEXT_DIM, DT_CENTER);
    const auto& pl = Plays();
    if (g_nbox == 1) Text(dc, L"PLAY " + std::to_wstring(pl.empty() ? 0 : g_sel + 1) + L" / " + std::to_wstring(pl.size()), S(ox + bw - 260), S(g_oy + 22), S(232), S(30), g_fSub, TEXT, DT_RIGHT);
    if (g_nbox > 1 && g_done) {   // two players: the pick is not shown, only READY
        int y = S(g_oy + CY + CH / 2 - 34), h = S(68), x = S(ox + 40), w = S(bw - 80);
        VGrad(dc, x, y, w, h, 0x2ECC71, 0x12804A);
        Glow(dc, x, y, w, h, 0x9CFFC4, BG_BOT, S(6));
        OutlineText(dc, L"READY", x, y, w, h, g_fForm, 0xFFFFFF, 0x0B2A1A, DT_CENTER);
        return;
    }
    // cards
    for (int k = 0; k < g_ncard; k++) {
        int i = g_first + k; RECT r = CardRect(k);
        int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
        if (i >= (int)pl.size()) { Fill(dc, x, y, w, h, 0x050D1E); Frame(dc, x, y, w, h, 0x15223C, 1); continue; }
        const PcPlay& p = (*g_list)[pl[i]];
        bool sel = i == g_sel && !g_done && g_nbox == 1;   // two players: nothing highlighted (picked by button)
        Fill(dc, x, y, w, h, PANEL);
        DrawArt(dc, p, x + S(6), y + S(6), w - S(12), S(ART_H) - S(6));
        if (sel) { HGrad(dc, x, y + S(ART_H), w, h - S(ART_H), 0x1E6FE0, 0x0A2A60); Glow(dc, x, y, w, h, GLOW_HI, BG_BOT, 7); }
        else Frame(dc, x, y, w, h, 0x2A3A5C, 1);
        int tx = x + S(12);
        if (g_nbox > 1) {   // the button that calls this card: X, A, Y
            static const wchar_t* L3[3] = {L"X", L"A", L"Y"};
            static const uint32_t C3[3] = {0x2D6CDF, 0x3FAE49, 0xE8B92E};
            int r0 = S(13), cx = x + S(8) + r0, cy = y + S(ART_H + 22);
            Disc(dc, cx, cy, r0, C3[k], 0xFFFFFF);
            Text(dc, L3[k], cx - r0, cy - r0, 2 * r0, 2 * r0, g_fSub, 0xFFFFFF, DT_CENTER);
            tx = cx + r0 + S(8);
        }
        Text(dc, Upper(Wide(p.shown)), tx, y + S(ART_H + 6), x + w - S(12) - tx, S(32), g_fName, sel ? 0xFFFFFF : TEXT);
        Text(dc, Wide(p.sets), tx, y + S(ART_H + 38), x + w - S(12) - tx, S(22), g_fSmall, sel ? 0xCFE6FF : TEXT_DIM);
    }
    // more-plays arrows
    if (g_first > 0) Text(dc, L"\x25C0", S(ox + 4), S(g_oy + CY + CH / 2 - 20), S(22), S(40), g_fForm, GLOW_HI, DT_CENTER);
    if (g_first + g_ncard < (int)pl.size()) Text(dc, L"\x25B6", S(ox + bw - 26), S(g_oy + CY + CH / 2 - 20), S(22), S(40), g_fForm, GLOW_HI, DT_CENTER);
}
void Paint(HDC dc) {
    VGrad(dc, 0, 0, S(W), S(TH()), BG_TOP, BG_BOT);
    Frame(dc, 0, 0, S(W), S(TH()), GLOW, std::max(1, S(2)));
    int keep = g_cur;
    for (int i = 0; i < g_nbox; i++) { UseBox(i); HGrad(dc, 0, S(g_oy + 72), S(W), S(3), GLOW, BG_BOT); PaintBox(dc); }
    if (g_nbox > 1) Fill(dc, S(10), S(BOXH) - 1, S(W - 20), std::max(2, S(3)), GLOW);
    if (keep >= 0) UseBox(keep);
    // footer
    int fy = S(TH() - 34), x = S(28);
    struct H { const wchar_t* k; const wchar_t* t; };
    H h1[] = {{L"A", L"CALL PLAY"}, {L"B", L"CLOSE"}, {L"LB RB", L"FORMATION"}, {L"\x25C0 \x25B6", L"PLAYS"}, {L"LT RT", L"NEXT 4"}};
    H h2[] = {{L"X A Y", L"CALL THAT PLAY"}, {L"B", L"KEEP ARCADE CALL"}, {L"LB RB", L"FORMATION"}, {L"\x25C0 \x25B6", L"NEXT 3"}};
    std::vector<H> hints = g_nbox > 1 ? std::vector<H>(h2, h2 + 4) : std::vector<H>(h1, h1 + 5);
    for (auto& hnt : hints) {
        x += Key(dc, hnt.k, x, fy) + S(8);
        SIZE sz; SelectObject(dc, g_fSmall); GetTextExtentPoint32W(dc, hnt.t, (int)wcslen(hnt.t), &sz);
        Text(dc, hnt.t, x, fy - S(12), sz.cx + S(4), S(24), g_fSmall, TEXT_DIM); x += sz.cx + S(26);
    }
}
void Invalidate() { InvalidateRect(g_wnd, nullptr, FALSE); }
enum Act { LEFT, RIGHT, FPREV, FNEXT, OK, BACK, PGUP, PGDN, PICK0, PICK1, PICK2 };
void SetForm(int f) {
    int n = (int)g_forms.size(); if (!n) return;
    g_form = (f % n + n) % n; g_sel = g_first = 0;
}
void Do(Act a) {
    switch (a) {
    case LEFT: g_sel--; Keep(); break;
    case RIGHT: g_sel++; Keep(); break;
    case PGUP: g_sel -= g_ncard; g_first -= g_ncard; Keep(); break;
    case PGDN: g_sel += g_ncard; g_first += g_ncard; Keep(); break;
    case FPREV: SetForm(g_form - 1); break;
    case FNEXT: SetForm(g_form + 1); break;
    case OK: if (!Plays().empty()) { g_result = Plays()[g_sel]; g_done = true; } break;
    case BACK: g_done = true; break;
    case PICK0: case PICK1: case PICK2: {
        int i = g_first + (a - PICK0);
        if (i < (int)Plays().size()) { g_result = Plays()[i]; g_done = true; }
        break;
    }
    }
    Invalidate();
}
void DoBox(int b, Act a) { if (b < 0 || b >= g_nbox) return; UseBox(b); if (g_nbox > 1 && !g_boxes[b].open) return; if (!g_done) Do(a); SaveBox(); Finish(); }
int BoxOfPort(int port) { if (g_nbox == 1) return 0; for (int i = 0; i < g_nbox; i++) if (g_boxes[i].port == port) return i; return -1; }
int KeyBox() { int b = BoxOfPort(0); return b >= 0 ? b : 0; }   // DoBox ignores it while closed

typedef DWORD(WINAPI* XGetState)(DWORD, void*);
XGetState g_xget;
struct XPad { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
DWORD g_padPrev, g_repeatAt, g_repeatBtn;
void LoadX() {
    if (!g_xget)
        for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryA(dll)) { g_xget = (XGetState)GetProcAddress(m, "XInputGetState"); if (g_xget) break; }
}
DWORD g_portPrev[4], g_portRepAt[4], g_portRep[4];
void PollPad() {
    if (!g_xget) return;
    static const struct { DWORD m; Act a; } map[] = {{0x0004, LEFT}, {0x0008, RIGHT}, {0x0001, FPREV}, {0x0002, FNEXT}, {0x0100, FPREV},
        {0x0200, FNEXT}, {0x1000, OK}, {0x2000, BACK}, {0x0020, BACK}, {0x10000, PGUP}, {0x20000, PGDN}};
    for (DWORD i = 0; i < 4; i++) {
        XPad st{}; DWORD b = 0;
        if (g_xget(i, &st) == 0) {
            b = st.buttons;
            if (st.ly > 16000) b |= 0x0001; if (st.ly < -16000) b |= 0x0002;
            if (st.lx < -16000) b |= 0x0004; if (st.lx > 16000) b |= 0x0008;
            if (st.lt > 120) b |= 0x10000; if (st.rt > 120) b |= 0x20000;
        }
        if (GetTickCount() - g_openTick < 350) { g_portPrev[i] |= b; if (!b) g_portPrev[i] = 0; continue; }   // ignore what opened it
        int box = BoxOfPort((int)i);
        DWORD press = b & ~g_portPrev[i];
        if (g_nbox > 1 && box >= 0 && !g_boxes[box].open) {   // his playbook is not open: SELECT opens it
            SaveBox();
            if ((press & 0x0020) && !g_boxes[box].done) { OpenBox(box); Invalidate(); }
            g_portPrev[i] = b;
            continue;
        }
        if (g_nbox > 1) {   // two players: X / A / Y call the three cards, left / right page, LB / RB formation, B arcade call
            static const struct { DWORD m; Act a; } map2[] = {{0x4000, PICK0}, {0x1000, PICK1}, {0x8000, PICK2}, {0x0004, PGUP}, {0x0008, PGDN},
                {0x0100, FPREV}, {0x0200, FNEXT}, {0x0001, FPREV}, {0x0002, FNEXT}, {0x2000, BACK}, {0x0020, BACK}, {0x10000, PGUP}, {0x20000, PGDN}};
            for (auto& m : map2) if (press & m.m) DoBox(box, m.a);
            g_portPrev[i] = b;
            continue;
        }
        for (auto& m : map) if (press & m.m) { DoBox(box, m.a); g_portRep[i] = m.m; g_portRepAt[i] = GetTickCount() + 380; }
        if ((b & g_portRep[i]) && (g_portRep[i] & 0x3000C) && GetTickCount() >= g_portRepAt[i]) {
            for (auto& m : map) if (m.m == g_portRep[i]) DoBox(box, m.a);
            g_portRepAt[i] = GetTickCount() + 110;
        }
        if (!(b & g_portRep[i])) g_portRep[i] = 0;
        g_portPrev[i] = b;
    }
}
LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bmp);
        Paint(mem); BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem); EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        PollPad();
        DWORD dt = GetTickCount() - g_openTick;
        if (dt <= 180) SetLayeredWindowAttributes(h, 0, (BYTE)std::min<DWORD>(245, 40 + dt * 205 / 180), LWA_ALPHA);
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_LEFT: DoBox(KeyBox(), LEFT); break; case VK_RIGHT: DoBox(KeyBox(), RIGHT); break; case VK_UP: case 'Q': DoBox(KeyBox(), FPREV); break;
        case VK_DOWN: case 'E': case VK_TAB: DoBox(KeyBox(), FNEXT); break; case VK_PRIOR: DoBox(KeyBox(), PGUP); break; case VK_NEXT: DoBox(KeyBox(), PGDN); break;
        case VK_RETURN: case VK_SPACE: DoBox(KeyBox(), OK); break; case VK_ESCAPE: case VK_BACK: case 'P': DoBox(KeyBox(), BACK); break;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(l), my = (short)HIWORD(l);
        UseBox(std::clamp(my / std::max(1, S(g_nbox > 1 ? BOXH : H)), 0, g_nbox - 1));
        if (g_done || (g_nbox > 1 && !g_boxes[g_cur].open)) return 0;
        for (int k = 0; k < g_ncard; k++) {
            RECT r = CardRect(k);
            if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom && g_first + k < (int)Plays().size()) { g_sel = g_first + k; Do(OK); SaveBox(); Finish(); return 0; }
        }
        for (int d : {-1, 1}) { RECT r = FormArrow(d); if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) { Do(d < 0 ? FPREV : FNEXT); return 0; } }
        if (my < S(g_oy + 72) && my >= S(g_oy)) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int mx = (short)LOWORD(l), my = (short)HIWORD(l);
        UseBox(std::clamp(my / std::max(1, S(g_nbox > 1 ? BOXH : H)), 0, g_nbox - 1));
        if (g_done) return 0;
        for (int k = 0; k < g_ncard; k++) {
            RECT r = CardRect(k);
            if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom && g_first + k < (int)Plays().size() && g_sel != g_first + k) { g_sel = g_first + k; Invalidate(); }
        }
        return 0;
    }
    case WM_RBUTTONDOWN: DoBox(std::clamp((short)HIWORD(l) / std::max(1, S(g_nbox > 1 ? BOXH : H)), 0, g_nbox - 1), BACK); return 0;
    case WM_MOUSEWHEEL: DoBox(KeyBox(), GET_WHEEL_DELTA_WPARAM(w) > 0 ? LEFT : RIGHT); return 0;
    case WM_CLOSE: for (int i = 0; i < g_nbox; i++) { UseBox(i); g_done = true; } SaveBox(); Finish(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
int CALLBACK HasFont(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM l) { *(bool*)l = true; return 0; }
bool FontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr); LOGFONTW lf{}; wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1); lf.lfCharSet = DEFAULT_CHARSET;
    bool found = false; EnumFontFamiliesExW(dc, &lf, HasFont, (LPARAM)&found, 0); ReleaseDC(nullptr, dc); return found;
}
std::wstring g_face = L"Segoe UI", g_cond = L"Segoe UI"; bool g_facesSet = false;
void Faces() {
    if (g_facesSet) return; g_facesSet = true;
    if (FontExists(L"Bahnschrift")) { g_face = L"Bahnschrift"; g_cond = FontExists(L"Bahnschrift SemiBold Condensed") ? L"Bahnschrift SemiBold Condensed" : L"Bahnschrift"; }
}
HFONT MakeFont(const std::wstring& face, double px, int weight, bool italic = false) {
    return CreateFontW(-S(px), 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face.c_str());
}
HWND g_badgeWnd;
BOOL CALLBACK FindGameWindow(HWND h, LPARAM l) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && h != g_wnd && h != g_badgeWnd) { RECT r; GetWindowRect(h, &r); if (r.right - r.left > 300) { *(HWND*)l = h; return FALSE; } }
    return TRUE;
}
HWND GameWindow() { HWND g = nullptr; EnumWindows(FindGameWindow, (LPARAM)&g); return g; }

// ---- the on-screen button ----
bool g_badgeClicked, g_badgeHover;
double g_bs = 1; HFONT g_bFont, g_bKey;
constexpr int BW = 290, BH = 50;
LRESULT CALLBACK BadgeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc0 = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(dc0); HBITMAP bmp = CreateCompatibleBitmap(dc0, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(dc, bmp);
        double pulse = 0.5 + 0.5 * std::sin(GetTickCount() / 260.0);
        VGrad(dc, 0, 0, rc.right, rc.bottom, Mix(0x0B2A66, 0x1E6FE0, g_badgeHover ? 0.9 : 0.35 * pulse), 0x061127);
        Frame(dc, 0, 0, rc.right, rc.bottom, Mix(GLOW, GLOW_HI, pulse), std::max(2, (int)lround(2 * g_bs)));
        int kh = (int)lround(26 * g_bs), kw = (int)lround(78 * g_bs), kx = (int)lround(10 * g_bs), ky = (rc.bottom - kh) / 2;
        Fill(dc, kx, ky, kw, kh, TAG); Frame(dc, kx, ky, kw, kh, 0xFFFFFF, 1);
        SelectObject(dc, g_bKey); SetTextColor(dc, 0xFFFFFF); SetBkMode(dc, TRANSPARENT);
        RECT kr{kx, ky, kx + kw, ky + kh}; DrawTextW(dc, L"SELECT", -1, &kr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, g_bFont); SetTextColor(dc, 0xFFFFFF);
        RECT tr{kx + kw + (int)lround(10 * g_bs), 0, rc.right - 6, rc.bottom}; DrawTextW(dc, L"PLAYBOOK", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc); EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: g_badgeClicked = true; return 0;
    case WM_MOUSEMOVE: if (!g_badgeHover) { g_badgeHover = true; TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, h, 0}; TrackMouseEvent(&te); } return 0;
    case WM_MOUSELEAVE: g_badgeHover = false; return 0;
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}
}   // namespace

bool PlayBadge(bool show) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (show && !g_badgeWnd) {
        static bool reg = false;
        if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = BadgeProc; wc.hInstance = inst; wc.lpszClassName = L"MaddenPlaybookButton"; RegisterClassW(&wc); reg = true; }
        HWND game = GameWindow();
        RECT gr; if (game) GetClientRect(game, &gr), MapWindowPoints(game, nullptr, (POINT*)&gr, 2); else SystemParametersInfoW(SPI_GETWORKAREA, 0, &gr, 0);
        g_bs = std::clamp((gr.bottom - gr.top) / 900.0, 0.7, 2.0);
        Faces();
        g_bFont = CreateFontW(-(int)lround(24 * g_bs), 0, 0, 0, FW_BOLD, TRUE, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, g_cond.c_str());
        g_bKey = CreateFontW(-(int)lround(14 * g_bs), 0, 0, 0, FW_BOLD, FALSE, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, g_face.c_str());
        int w = (int)lround(BW * g_bs), h = (int)lround(BH * g_bs);
        int x = gr.right - w - (int)lround(24 * g_bs), y = gr.top + (gr.bottom - gr.top) * 13 / 100;
        g_badgeWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"MaddenPlaybookButton", L"", WS_POPUP,
                                     x, y, w, h, nullptr, nullptr, inst, nullptr);
        SetLayeredWindowAttributes(g_badgeWnd, 0, 235, LWA_ALPHA);
        ShowWindow(g_badgeWnd, SW_SHOWNOACTIVATE);
        SetTimer(g_badgeWnd, 1, 40, nullptr);
        g_badgeClicked = g_badgeHover = false;
    } else if (!show && g_badgeWnd) {
        DestroyWindow(g_badgeWnd); g_badgeWnd = nullptr;
        DeleteObject(g_bFont); DeleteObject(g_bKey);
    }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    bool c = g_badgeClicked; g_badgeClicked = false;
    return c;
}

void ShowPlayBoxes(PcBoxReq* req, int n) {
    n = std::clamp(n, 1, 2);
    g_nbox = n; g_cur = -1;
    for (int i = 0; i < n; i++) {
        Box& bx = g_boxes[i];
        const std::vector<PcPlay>& list = *req[i].list;
        bx.list = &list; bx.side = req[i].side; bx.port = req[i].port; bx.result = -1; bx.done = false; bx.told = false; bx.onPick = req[i].onPick;
        bx.open = n == 1 || req[i].open;
        bx.forms.clear(); bx.fplays.clear();
        bx.bx0 = bx.by0 = 1e9; bx.bx1 = bx.by1 = -1e9;
        for (int k = 0; k < (int)list.size(); k++) {
            if (bx.forms.empty() || bx.forms.back() != list[k].form) { bx.forms.push_back(list[k].form); bx.fplays.emplace_back(); }
            bx.fplays.back().push_back(k);
            for (auto& pl : list[k].art) {
                auto add = [&](double x, double y) { bx.bx0 = std::min(bx.bx0, x); bx.bx1 = std::max(bx.bx1, x); bx.by0 = std::min(bx.by0, y); bx.by1 = std::max(bx.by1, y); };
                add(pl.x, pl.y);
                for (auto& q : pl.pts) add(pl.x + q.dx, pl.y + q.dy);
            }
        }
        if (bx.forms.empty()) { bx.done = true; bx.form = bx.sel = bx.first = 0; continue; }
        if (bx.bx0 > bx.bx1) { bx.bx0 = 0; bx.bx1 = 180; bx.by0 = 0; bx.by1 = 110; }
        bx.bx0 -= 6; bx.bx1 += 6; bx.by0 -= 6; bx.by1 += 6;
        bx.form = std::clamp(req[i].form, 0, (int)bx.forms.size() - 1); bx.sel = req[i].play; bx.first = 0;
        UseBox(i); Keep(); g_first = std::max(0, g_sel - 1); Keep(); SaveBox();
    }
    if (AllDone()) { for (int i = 0; i < n; i++) req[i].result = -1; return; }
    for (int i = 0; i < n; i++) if (g_boxes[i].open && n > 1 && g_boxes[i].port >= 0 && g_boxes[i].port < 4) g_padMuteMask |= 1u << g_boxes[i].port;
    for (int i = 0; i < 4; i++) { g_portPrev[i] = 0xFFFFFFFF; g_portRep[i] = 0; }
    LoadX(); Faces();
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"MaddenPlayBox"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc); reg = true; }
    g_wnd = nullptr;
    HWND game = GameWindow();
    RECT area; if (game) GetClientRect(game, &area), MapWindowPoints(game, nullptr, (POINT*)&area, 2); else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    int aw = area.right - area.left, ah = area.bottom - area.top;
    g_s = std::clamp(std::min(aw * 0.96 / W, ah * (g_nbox > 1 ? 0.94 : 0.62) / TH()), 0.5, 2.0);
    g_fTitle = MakeFont(g_cond, 34, FW_BOLD, true); g_fForm = MakeFont(g_cond, 28, FW_BOLD, true); g_fSub = MakeFont(g_face, 15, FW_SEMIBOLD);
    g_fName = MakeFont(g_cond, 22, FW_BOLD, true); g_fSmall = MakeFont(g_face, 12, FW_BOLD);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"MaddenPlayBox", L"Full playbook", WS_POPUP,
                            area.left + (aw - S(W)) / 2, area.bottom - S(TH()) - (g_nbox > 1 ? ah * 2 / 100 : ah * 4 / 100), S(W), S(TH()), nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_wnd, 0, 40, LWA_ALPHA);
    g_openTick = GetTickCount();
    ShowWindow(g_wnd, SW_SHOW); SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    SetTimer(g_wnd, 1, 16, nullptr);
    MSG msg;
    while ((n > 1 ? AnyOpen() : !AllDone()) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == g_badgeWnd && msg.message == WM_LBUTTONDOWN) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (n > 1) { InvalidateRect(g_wnd, nullptr, FALSE); UpdateWindow(g_wnd); Sleep(250); }   // show READY a moment
    KillTimer(g_wnd, 1); DestroyWindow(g_wnd); g_wnd = nullptr;
    for (HFONT f : {g_fTitle, g_fForm, g_fSub, g_fName, g_fSmall}) DeleteObject(f);
    if (game) SetForegroundWindow(game);
    SaveBox();
    for (int i = 0; i < n; i++) {
        if (n > 1 && g_boxes[i].open && g_boxes[i].port >= 0 && g_boxes[i].port < 4) { g_padMuteMask &= ~(1u << g_boxes[i].port); g_padMuteRelease |= 1u << g_boxes[i].port; }
        req[i].form = g_boxes[i].form; req[i].play = g_boxes[i].sel; req[i].result = g_boxes[i].result;
    }
    g_cur = -1;
}
int ShowPlayBox(const std::vector<PcPlay>& list, int side, int& form, int& play) {
    PcBoxReq r{&list, side, -1, form, play, -1, true, nullptr};
    ShowPlayBoxes(&r, 1);
    form = r.form; play = r.play;
    return r.result;
}
#else
bool PlayBadge(bool) { return false; }
int ShowPlayBox(const std::vector<PcPlay>&, int, int&, int&) { return -1; }
void ShowPlayBoxes(PcBoxReq*, int) {}
#endif
