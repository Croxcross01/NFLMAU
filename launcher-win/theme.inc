// Launcher look. The whole window is drawn in one pass into an off-screen picture and copied to the screen (no see-through
// controls): a sidebar on the left, the background picture (launcher_bg.jpg next to the launcher - your own file, not
// shipped) filling the rest, and on the Play page a big PLAY button, a status line and info cards. Settings / Mods / Log
// put their standard controls on a solid dark card. Included by launcher.cpp.
#include <olectl.h>
#include <uxtheme.h>

enum { PAGE_PLAY, PAGE_SETTINGS, PAGE_MODS, PAGE_ONLINE, PAGE_LOG, PAGE_COUNT };
static const wchar_t* kPageNames[] = {L"Play", L"Settings", L"Mods", L"Online", L"Log"};
static const int kSideW = 232;
static int g_page = PAGE_PLAY;
static int g_hot = -1;                       // hovered hit id
static bool g_down = false;

static HBITMAP g_bgPic; static int g_picW, g_picH;
static HBITMAP g_scaled; static int g_scW, g_scH;              // the picture scaled to the content area
static HBITMAP g_frame; static uint32_t* g_px; static int g_fw, g_fh;   // the composed window picture
static HBRUSH g_cardBrush, g_editBrush;
static HFONT g_fTicker, g_fTitle, g_fNav, g_fPlay, g_fH1, g_fBody, g_fBodyB, g_fSmall, g_fCap, g_fCardV;
static RECT g_card;                          // the solid card of Settings / Mods / Log
static std::wstring g_statusText = L"Ready to play"; static COLORREF g_statusDot = RGB(46, 204, 113);
static std::wstring g_updateMsg;
std::wstring g_versionLabel;

static const COLORREF kText = RGB(236, 242, 255), kDim = RGB(148, 163, 194), kCardC = RGB(16, 22, 38), kEditBg = RGB(10, 15, 28);
static const uint32_t kSide = 0x0A0E18, kAccent = 0x2F8FFF, kAccentHi = 0x8EE3FF;

struct Hit { RECT r; int id; };
static std::vector<Hit> g_hits;              // nav 0..3, PLAY 10, info cards 20..22
enum { HIT_NAV = 0, HIT_PLAY = 10, HIT_CARD = 20, HIT_TOWER = 30 };
static bool g_animFrame = false;             // a light-show frame (Play page): only the moving parts change
static RECT g_animRect;                      // what a light-show frame repaints
static std::vector<uint32_t> g_base; static int g_baseW, g_baseH, g_basePage = -1, g_baseGen, g_bgGen;   // picture + shading + sidebar, kept between frames
static double AnimTime() { return (double)(GetTickCount64() % 3600000ull) / 1000.0; }

static void LoadBackgroundPicture() {
    for (const wchar_t* name : {L"\\launcher_bg.jpg", L"\\launcher_bg.jpeg", L"\\mods\\launcher_bg.jpg"}) {
        std::wstring p = g_exeDir + name;
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD size = GetFileSize(f, nullptr);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size); DWORD got = 0;
        if (mem) { ReadFile(f, GlobalLock(mem), size, &got, nullptr); GlobalUnlock(mem); }
        CloseHandle(f);
        if (!mem || got != size) { if (mem) GlobalFree(mem); continue; }
        IStream* st = nullptr; IPicture* pic = nullptr;
        if (CreateStreamOnHGlobal(mem, TRUE, &st) == S_OK && OleLoadPicture(st, (LONG)size, FALSE, IID_IPicture, (void**)&pic) == S_OK) {
            OLE_HANDLE h = 0; pic->get_Handle(&h);
            g_bgPic = (HBITMAP)CopyImage((HBITMAP)(UINT_PTR)h, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
            BITMAP bm{}; GetObjectW(g_bgPic, sizeof bm, &bm); g_picW = bm.bmWidth; g_picH = bm.bmHeight;
            pic->Release();
        }
        if (st) st->Release(); else GlobalFree(mem);
        if (g_bgPic) return;
    }
    // no picture of your own: the Madden Arcade Unleashed backdrop built into the launcher
    if (HRSRC r = FindResourceW(nullptr, L"DEFAULTBG", RT_RCDATA)) {
        DWORD size = SizeofResource(nullptr, r); const void* src = LockResource(LoadResource(nullptr, r));
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (mem && src) { memcpy(GlobalLock(mem), src, size); GlobalUnlock(mem); }
        IStream* st = nullptr; IPicture* pic = nullptr;
        if (mem && CreateStreamOnHGlobal(mem, TRUE, &st) == S_OK && OleLoadPicture(st, (LONG)size, FALSE, IID_IPicture, (void**)&pic) == S_OK) {
            OLE_HANDLE h = 0; pic->get_Handle(&h);
            g_bgPic = (HBITMAP)CopyImage((HBITMAP)(UINT_PTR)h, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
            BITMAP bm{}; GetObjectW(g_bgPic, sizeof bm, &bm); g_picW = bm.bmWidth; g_picH = bm.bmHeight;
            pic->Release();
        }
        if (st) st->Release(); else if (mem) GlobalFree(mem);
    }
}

// ---- pixel helpers (on g_px) ----
static inline void BlendPx(uint32_t& d, uint32_t c, double a) {
    if (a <= 0) return; if (a > 1) a = 1;
    int r = (int)((d >> 16) & 255), g = (int)((d >> 8) & 255), b = (int)(d & 255);
    int cr = (int)((c >> 16) & 255), cg = (int)((c >> 8) & 255), cb = (int)(c & 255);   // signed: the difference can be negative
    r = std::clamp(r + (int)lround((cr - r) * a), 0, 255); g = std::clamp(g + (int)lround((cg - g) * a), 0, 255); b = std::clamp(b + (int)lround((cb - b) * a), 0, 255);
    d = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}
static double RoundCover(int x, int y, const RECT& r, int rad) {   // 0..1 coverage of a rounded rect (soft edge)
    double dx = std::max({(double)r.left + rad - x - 0.5, 0.0, x + 0.5 - ((double)r.right - rad)});
    double dy = std::max({(double)r.top + rad - y - 0.5, 0.0, y + 0.5 - ((double)r.bottom - rad)});
    double d = sqrt(dx * dx + dy * dy) - rad;
    return std::clamp(0.5 - d, 0.0, 1.0);
}
static void FillRound(RECT r, int rad, uint32_t c, double a) {
    for (int y = std::max<int>(0, r.top); y < std::min<int>(g_fh, r.bottom); y++)
        for (int x = std::max<int>(0, r.left); x < std::min<int>(g_fw, r.right); x++) {
            double cv = RoundCover(x, y, r, rad); if (cv > 0) BlendPx(g_px[y * g_fw + x], c, a * cv);
        }
}
static void FillRoundV(RECT r, int rad, uint32_t top, uint32_t bot, double a) {   // vertical gradient
    for (int y = std::max<int>(0, r.top); y < std::min<int>(g_fh, r.bottom); y++) {
        double t = (double)(y - r.top) / std::max<LONG>(1, r.bottom - r.top - 1);
        uint32_t c = top; BlendPx(c, bot, t);
        for (int x = std::max<int>(0, r.left); x < std::min<int>(g_fw, r.right); x++) {
            double cv = RoundCover(x, y, r, rad); if (cv > 0) BlendPx(g_px[y * g_fw + x], c, a * cv);
        }
    }
}
static void StrokeRound(RECT r, int rad, uint32_t c, double a) {   // 1px outline
    for (int y = std::max<int>(0, r.top); y < std::min<int>(g_fh, r.bottom); y++)
        for (int x = std::max<int>(0, r.left); x < std::min<int>(g_fw, r.right); x++) {
            RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
            double cv = RoundCover(x, y, r, rad) - RoundCover(x, y, in, std::max(0, rad - 1));
            if (cv > 0) BlendPx(g_px[y * g_fw + x], c, a * cv);
        }
}
static void GlowRound(RECT r, int rad, uint32_t c, int spread, double a) {
    for (int i = spread; i >= 1; i--) { RECT o{r.left - i, r.top - i, r.right + i, r.bottom + i}; StrokeRound(o, rad + i, c, a * (1.0 - (double)i / (spread + 1)) * 0.5); }
}

// slanted ("speed") shapes: the top edge sits k * height to the right of the bottom edge
static double RoundCoverD(double px, double py, double l, double t, double r, double b, double rad) {
    double dx = std::max({l + rad - px, 0.0, px - (r - rad)}), dy = std::max({t + rad - py, 0.0, py - (b - rad)});
    return std::clamp(0.5 - (sqrt(dx * dx + dy * dy) - rad), 0.0, 1.0);
}
static double SkewCover(int x, int y, RECT r, double rad, double k, double inset = 0) {
    double px = x + 0.5, py = y + 0.5, sh = (r.bottom - py) * k;
    return RoundCoverD(px - sh, py, r.left + inset, r.top + inset, r.right - inset, r.bottom - inset, std::max(0.0, rad - inset));
}
static int SkewRight(RECT r, double k) { return r.right + (int)ceil((r.bottom - r.top) * k) + 1; }
static void FillSkew(RECT r, int rad, double k, uint32_t top, uint32_t bot, double a) {
    for (int y = std::max<int>(0, r.top); y < std::min<int>(g_fh, r.bottom); y++) {
        uint32_t c = top; BlendPx(c, bot, (double)(y - r.top) / std::max<LONG>(1, r.bottom - r.top - 1));
        for (int x = std::max<int>(0, r.left); x < std::min<int>(g_fw, SkewRight(r, k)); x++) { double cv = SkewCover(x, y, r, rad, k); if (cv > 0) BlendPx(g_px[y * g_fw + x], c, a * cv); }
    }
}
static void StrokeSkew(RECT r, int rad, double k, uint32_t c, double a, double width = 1.2) {
    for (int y = std::max<int>(0, r.top); y < std::min<int>(g_fh, r.bottom); y++)
        for (int x = std::max<int>(0, r.left); x < std::min<int>(g_fw, SkewRight(r, k)); x++) {
            double cv = SkewCover(x, y, r, rad, k) - SkewCover(x, y, r, rad, k, width); if (cv > 0) BlendPx(g_px[y * g_fw + x], c, a * cv);
        }
}
static void GlowSkew(RECT r, int rad, double k, uint32_t c, int spread, double a) {   // one pass: fades out with distance from the edge
    int x1 = std::min<int>(g_fw, SkewRight(r, k) + spread);
    for (int y = std::max<int>(0, r.top - spread); y < std::min<int>(g_fh, r.bottom + spread); y++)
        for (int x = std::max<int>(0, r.left - spread); x < x1; x++) {
            double px = x + 0.5, py = y + 0.5, sh = (r.bottom - std::clamp(py, (double)r.top, (double)r.bottom)) * k + (py > r.bottom ? 0 : 0);
            double qx = px - sh, dx = std::max({r.left + (double)rad - qx, 0.0, qx - (r.right - (double)rad)}), dy = std::max({r.top + (double)rad - py, 0.0, py - (r.bottom - (double)rad)});
            double d = sqrt(dx * dx + dy * dy) - rad; if (d <= 0 || d >= spread) continue;
            double f = 1.0 - d / spread; BlendPx(g_px[y * g_fw + x], c, a * f * f * 0.55);
        }
}
// a soft white band sweeping across a slanted shape (the shine on the PLAY button)
static void ShineSkew(RECT r, int rad, double k, double pos, double a) {
    int span = SkewRight(r, k) - r.left;
    double cx = r.left - 60 + pos * (span + 120);
    for (int y = std::max<int>(0, r.top); y < std::min<int>(g_fh, r.bottom); y++)
        for (int x = std::max<int>(0, r.left); x < std::min<int>(g_fw, SkewRight(r, k)); x++) {
            double d = fabs((x + (r.bottom - y) * 0.9) - cx - (r.bottom - r.top) * 0.45) / 26.0; if (d >= 1) continue;
            double cv = SkewCover(x, y, r, rad, k); if (cv > 0) BlendPx(g_px[y * g_fw + x], 0xFFFFFF, a * cv * (1 - d) * (1 - d));
        }
}
// stadium light beams falling from the top of the Play page, swaying slowly
static void LightBeams(int x0, int x1, int h, double t) {
    struct B { double ox, ang, sway, speed, ph, w; uint32_t c; } beams[] = {
        {0.18, 0.30, 0.07, 0.23, 0.0, 26, 0x9CCBFF}, {0.47, -0.10, 0.09, 0.17, 2.1, 20, 0xBFE6FF}, {0.80, -0.34, 0.06, 0.29, 4.0, 30, 0x8AB8FF}};
    int cw = x1 - x0, yMax = (int)(h * 0.78);
    for (auto& b : beams) {
        double ang = b.ang + b.sway * sin(t * b.speed + b.ph), tn = tan(ang), ox = x0 + cw * b.ox;
        double pulse = 0.85 + 0.15 * sin(t * 0.9 + b.ph * 1.7);
        for (int y = 0; y < yMax; y++) {
            double c = ox + tn * y, hw = b.w + y * 0.28, fall = 1.0 - (double)y / yMax;
            int a0 = std::max(x0, (int)(c - hw)), a1 = std::min(x1, (int)(c + hw) + 1);
            for (int x = a0; x < a1; x++) {
                double d = fabs(x - c) / hw; if (d >= 1) continue;
                BlendPx(g_px[y * g_fw + x], b.c, (1 - d) * (1 - d) * 0.085 * fall * pulse);
            }
        }
        // the lamp at the top
        for (int y = 0; y < 26; y++) for (int x = std::max(x0, (int)ox - 40); x < std::min(x1, (int)ox + 40); x++) {
            double d = sqrt((x - ox) * (x - ox) + y * y * 2.2) / 40.0; if (d < 1) BlendPx(g_px[y * g_fw + x], 0xE8F6FF, (1 - d) * (1 - d) * 0.35 * pulse);
        }
    }
}

// ---- GDI text helpers (on the frame DC) ----
static void Txt(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void NavIcon(HDC dc, int i, int cx, int cy, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 2, c); HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (i) {
    case PAGE_PLAY: { POINT p[3] = {{cx - 5, cy - 7}, {cx + 7, cy}, {cx - 5, cy + 7}}; Polygon(dc, p, 3); break; }
    case PAGE_SETTINGS:
        for (int k = 0; k < 3; k++) { int y = cy - 6 + k * 6; MoveToEx(dc, cx - 8, y, nullptr); LineTo(dc, cx + 8, y); int kx = cx + (k == 1 ? 3 : -3); Ellipse(dc, kx - 2, y - 2, kx + 3, y + 3); }
        break;
    case PAGE_MODS: {
        POINT a[4] = {{cx, cy - 8}, {cx + 8, cy - 3}, {cx, cy + 2}, {cx - 8, cy - 3}}; Polygon(dc, a, 4);
        MoveToEx(dc, cx - 8, cy + 2, nullptr); LineTo(dc, cx, cy + 7); LineTo(dc, cx + 9, cy + 2); break; }
    case PAGE_ONLINE:   // a globe
        Ellipse(dc, cx - 8, cy - 8, cx + 9, cy + 9); Ellipse(dc, cx - 3, cy - 8, cx + 4, cy + 9);
        MoveToEx(dc, cx - 8, cy, nullptr); LineTo(dc, cx + 9, cy); break;
    case PAGE_LOG:
        RoundRect(dc, cx - 7, cy - 8, cx + 8, cy + 9, 4, 4);
        for (int k = 0; k < 3; k++) { MoveToEx(dc, cx - 3, cy - 3 + k * 4, nullptr); LineTo(dc, cx + 4, cy - 3 + k * 4); }
        break;
    }
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}

static std::wstring IniRead(const wchar_t* file, const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t b[512]; GetPrivateProfileStringW(sec, key, def, b, 512, (g_exeDir + file).c_str()); return b;
}
static std::wstring FileWord(const std::wstring& p, const wchar_t* def) {
    FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return def;
    char b[128] = {}; fgets(b, sizeof b, f); fclose(f);
    std::string s = b; while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s.empty() ? def : Widen(s);
}
static std::wstring Cap(std::wstring s) { if (!s.empty()) s[0] = towupper(s[0]); return s; }

static void ScaleBackground(int cw, int ch) {
    if (g_scaled && g_scW == cw && g_scH == ch) return;
    if (g_scaled) DeleteObject(g_scaled), g_scaled = nullptr;
    g_scW = cw; g_scH = ch;
    if (!g_bgPic || cw <= 0 || ch <= 0) return;
    HDC sdc = GetDC(nullptr);
    g_scaled = CreateCompatibleBitmap(sdc, cw, ch);
    HDC d = CreateCompatibleDC(sdc), s = CreateCompatibleDC(sdc);
    HGDIOBJ od = SelectObject(d, g_scaled), os = SelectObject(s, g_bgPic);
    double k = std::max((double)cw / g_picW, (double)ch / g_picH);
    int dw = (int)(g_picW * k + 0.5), dh = (int)(g_picH * k + 0.5);
    SetStretchBltMode(d, HALFTONE); SetBrushOrgEx(d, 0, 0, nullptr);
    StretchBlt(d, (cw - dw) / 2, (ch - dh) / 3, dw, dh, s, 0, 0, g_picW, g_picH, SRCCOPY);
    SelectObject(d, od); SelectObject(s, os); DeleteDC(d); DeleteDC(s); ReleaseDC(nullptr, sdc);
}

static void Repaint(bool children = false);
// after the Mods page picks or removes the background picture
static void ReloadBackground() {
    if (g_bgPic) DeleteObject(g_bgPic), g_bgPic = nullptr;
    if (g_scaled) DeleteObject(g_scaled), g_scaled = nullptr;
    g_scW = g_scH = 0;
    LoadBackgroundPicture();
    g_bgGen++;
    Repaint(true);
}
static const double kSlant = 0.22;
static RECT PlayBtnRect() { int cx = kSideW + 56; return RECT{cx, g_fh - 304, cx + 360, g_fh - 226}; }
static RECT TowerBtnRect() { RECT p = PlayBtnRect(); int l = SkewRight(p, kSlant) + 10; return RECT{l, p.top, l + 300, p.bottom}; }

// what the Play page shows (read at most once a second - the light show repaints many times a second)
struct PlayInfo { std::wstring files, gfx, roster, tCap, tMain, tSub; int tState = 0; uint32_t tColor = 0xE8B33A; };   // tState 0 none, 1 climbing, 2 done, -1 off
static PlayInfo g_info; static ULONGLONG g_infoAt;
static void RefreshPlayInfo() {
    ULONGLONG now = GetTickCount64(); if (g_animFrame && now - g_infoAt < 1000) return; g_infoAt = now;
    PlayInfo in;
    wchar_t path[MAX_PATH * 2] = L""; GetWindowTextW(g_path, path, MAX_PATH * 2);
    std::wstring p = path; while (!p.empty() && (p.back() == L'\\' || p.back() == L' ')) p.pop_back();
    in.files = p.empty() ? L"Not set" : p.substr(p.find_last_of(L"\\/") == std::wstring::npos ? 0 : p.find_last_of(L"\\/") + 1);
    std::wstring q = IniRead(L"\\settings.ini", L"settings", L"quality", L"medium"), fs = IniRead(L"\\settings.ini", L"settings", L"fullscreen", L"1");
    in.gfx = Cap(q) + (fs == L"1" ? L"  \x2022  Fullscreen" : L"  \x2022  Windowed");
    std::wstring roster = FileWord(g_exeDir + L"\\mods\\roster.txt", L"original"), lines = FileWord(g_exeDir + L"\\mods\\lines.txt", L"5");
    in.roster = (roster == L"original" ? L"Original" : roster) + L"  \x2022  " + (lines == L"full" ? L"Full lines" : lines + L" on " + lines);
    // Towers: the climb in the active save slot (mods\towers\slotN.txt, written by the game)
    in.tCap = L"TOWERS";
    if (FileExists(g_exeDir + L"\\mods\\towers.off")) { in.tState = -1; in.tMain = L"Turned off"; in.tSub = L"Remove mods\\towers.off to play it"; }
    else {
        int slot = _wtoi(FileWord(g_exeDir + L"\\mods\\towers\\active.txt", L"1").c_str()); if (slot < 1 || slot > 3) slot = 1;
        std::wstring team, next; int rung = 0, wins = 0, losses = 0, total = 0, id = 0;
        if (FILE* f = _wfopen((g_exeDir + L"\\mods\\towers\\slot" + std::to_wstring(slot) + L".txt").c_str(), L"rb")) {
            char b[1024];
            while (fgets(b, sizeof b, f)) {
                std::string l = b; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                size_t e = l.find('='); if (e == std::string::npos) continue;
                std::string k = l.substr(0, e), v = l.substr(e + 1);
                if (k == "team") id = atoi(v.c_str()); else if (k == "rung") rung = atoi(v.c_str());
                else if (k == "wins") wins = atoi(v.c_str()); else if (k == "losses") losses = atoi(v.c_str());
                else if (k == "order") total = v.empty() ? 0 : 1 + (int)std::count(v.begin(), v.end(), ',');
                else if (k == "name") team = Widen(v); else if (k == "next") next = Widen(v);
                else if (k == "color") { uint32_t c = (uint32_t)strtoul(v.c_str(), nullptr, 16); int r = c >> 16 & 255, g = c >> 8 & 255, bl = c & 255;
                    if (std::max({r, g, bl}) < 90) { r = r * 2 + 40; g = g * 2 + 40; bl = bl * 2 + 40; }   // too dark to see on navy: lift it
                    in.tColor = (uint32_t)std::min(r, 255) << 16 | (uint32_t)std::min(g, 255) << 8 | (uint32_t)std::min(bl, 255); }
            }
            fclose(f);
        }
        in.tCap = L"TOWERS  \x2022  SLOT " + std::to_wstring(slot);
        if (!id) { in.tMain = L"Start a climb"; in.tSub = L"Pick Towers in the game's main menu"; }
        else {
            if (team.empty()) team = L"Your team";
            if (total && rung >= total) { in.tState = 2; in.tMain = team + L" \x2014 champions"; in.tSub = L"All " + std::to_wstring(total) + L" teams beaten"; }
            else {
                in.tState = 1; in.tMain = team;
                in.tSub = L"Game " + std::to_wstring(rung + 1) + (total ? L" of " + std::to_wstring(total) : L"") + L"  \x2022  " + std::to_wstring(wins) + L"-" + std::to_wstring(losses);
                if (!next.empty()) in.tSub += L"  \x2022  vs " + next;
            }
        }
    }
    g_info = in;
}

// ---- the night field: a perspective football field under the PLAY button, with chalk plays drawn on it ----
struct Field { double yH, vx, fy, fx, zNear; };
static Field FieldOf(int x0, int x1, int h) { Field f; f.yH = h * 0.45; f.vx = (x0 + x1) / 2.0 + (x1 - x0) * 0.08; f.zNear = 6; f.fy = (h - f.yH) * f.zNear; f.fx = (x1 - x0) * 0.72 * f.zNear / 26.6; return f; }
static bool Project(const Field& f, double fx, double fz, double& sx, double& sy) { double z = fz; if (z < 1) return false; sx = f.vx + fx * f.fx / z; sy = f.yH + f.fy / z; return true; }
static void SoftDot(double x, double y, double r, uint32_t c, double a) {
    for (int yy = (int)(y - r - 1); yy <= (int)(y + r + 1); yy++) for (int xx = (int)(x - r - 1); xx <= (int)(x + r + 1); xx++) {
        if (xx < 0 || yy < 0 || xx >= g_fw || yy >= g_fh) continue;
        double d = sqrt((xx + 0.5 - x) * (xx + 0.5 - x) + (yy + 0.5 - y) * (yy + 0.5 - y)); double cv = std::clamp(r - d + 0.5, 0.0, 1.0);
        if (cv > 0) BlendPx(g_px[yy * g_fw + xx], c, a * cv);
    }
}
static void GlowLine(double x0, double y0, double x1, double y1, double r, uint32_t c, double a) {
    double len = std::max(1.0, hypot(x1 - x0, y1 - y0)); int n = (int)(len * 1.5) + 1;
    for (int i = 0; i <= n; i++) { double u = (double)i / n; SoftDot(x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, r, c, a / 1.8); }
}
static void FieldLine(const Field& f, double ax, double az, double bx, double bz, double r, uint32_t c, double a) {   // a straight line in field yards
    int n = 24; double px = 0, py = 0; bool have = false;
    for (int i = 0; i <= n; i++) { double u = (double)i / n, sx, sy; if (!Project(f, ax + (bx - ax) * u, az + (bz - az) * u, sx, sy)) { have = false; continue; }
        if (have) GlowLine(px, py, sx, sy, r, c, a * std::clamp((sy - f.yH) / 60.0, 0.15, 1.0)); px = sx; py = sy; have = true; }
}
static void DrawFieldBase(int x0, int x1, int h) {
    Field f = FieldOf(x0, x1, h);
    for (int y = (int)f.yH; y < h; y++) {   // turf: a dark green-teal glow that fades into the night at the horizon
        double k = (y - f.yH) / (h - f.yH), a = std::min(1.0, k * 3.0) * 0.30;
        double z = f.fy / std::max(0.5, y - f.yH), half = 26.6 * f.fx / z;
        int l = std::max(x0, (int)(f.vx - half * 1.25)), r = std::min(x1, (int)(f.vx + half * 1.25));
        for (int x = l; x < r; x++) { double e = fabs(x - f.vx) / (half * 1.25); BlendPx(g_px[y * g_fw + x], 0x0D4A3A, a * (1 - e * e)); }
    }
    for (int yd = 0; yd <= 120; yd += 5) {   // yard lines, brighter every 10
        double z = yd + 2.0; FieldLine(f, -26.6, z, 26.6, z, yd % 10 ? 0.6 : 0.9, 0x9FE8D0, yd % 10 ? 0.22 : 0.38);
        for (double hx : {-6.2, 6.2}) for (int k = 1; k < 5; k++) FieldLine(f, hx - 0.5, z + k, hx + 0.5, z + k, 0.5, 0x9FE8D0, 0.25);   // hash marks
    }
    FieldLine(f, -26.6, 2, -26.6, 122, 1.0, 0xBFF5E4, 0.45); FieldLine(f, 26.6, 2, 26.6, 122, 1.0, 0xBFF5E4, 0.45);   // sidelines
}
struct ChalkPlay { const wchar_t* name; std::vector<std::vector<std::pair<double, double>>> routes; };
static const std::pair<double, double> kO[] = {{-20, -1}, {-12, -1.5}, {-2, -1}, {0, -1}, {2, -1}, {7, -1}, {0, -5}, {0, -8}};   // offence (x yards, yards past the line)
static const std::pair<double, double> kX[] = {{-19, 6}, {19, 6}, {-11, 5}, {0, 7}, {9, 4}, {-8, 15}, {8, 15}};                  // defence
static const ChalkPlay kPlays[] = {
    {L"FOUR VERTICALS", {{{-20, -1}, {-20, 20}}, {{-12, -1.5}, {-12, 4}, {-10, 20}}, {{7, -1}, {7, 4}, {9, 19}}, {{0, -8}, {8, -6}, {17, -3}}}},
    {L"SLANT  /  FLAT", {{{-20, -1}, {-20, 3}, {-11, 9}}, {{-12, -1.5}, {-12, 3}, {-23, 3}}, {{7, -1}, {7, 2}, {-8, 4}}, {{0, -8}, {12, -6}, {18, 6}}}},
    {L"POST  /  CORNER", {{{-20, -1}, {-20, 9}, {-8, 20}}, {{-12, -1.5}, {-12, 8}, {-11, 6}}, {{7, -1}, {7, 9}, {17, 18}}, {{0, -8}, {-6, -5}, {-6, 2}}}},
};
static const wchar_t* g_playName = nullptr; static double g_playAlpha = 0;
static void ChalkPlays(int x0, int x1, int h, double t) {
    Field f = FieldOf(x0, x1, h);
    const double cycle = 7.5, los = 30; int idx = (int)(t / cycle) % 3; double ph = fmod(t, cycle);
    double show = ph < 0.6 ? ph / 0.6 : ph > 6.6 ? std::max(0.0, (7.4 - ph) / 0.8) : 1.0;
    double draw = std::clamp((ph - 0.7) / 2.4, 0.0, 1.0);
    g_playName = kPlays[idx].name; g_playAlpha = show * (ph > 1.0 ? 1 : 0);
    double sx, sy;
    FieldLine(f, -26.6, los, 26.6, los, 0.9, 0x6CC6FF, 0.35 * show);   // line of scrimmage
    for (auto& o : kO) if (Project(f, o.first, los + o.second, sx, sy)) { double r = 2.6 * f.fx / (los + o.second) / 3.0 + 2.0;
        for (int k = 0; k < 28; k++) { double an = k * 6.2832 / 28; SoftDot(sx + cos(an) * r, sy + sin(an) * r * 0.55, 0.8, 0xD8F2FF, 0.8 * show); } }
    for (auto& x : kX) if (Project(f, x.first, los + x.second, sx, sy)) { double r = 2.2 * f.fx / (los + x.second) / 3.0 + 1.6;
        GlowLine(sx - r, sy - r * 0.55, sx + r, sy + r * 0.55, 0.8, 0xFFA060, 0.9 * show); GlowLine(sx - r, sy + r * 0.55, sx + r, sy - r * 0.55, 0.8, 0xFFA060, 0.9 * show); }
    for (auto& rt : kPlays[idx].routes) {   // routes draw on like chalk, ending in an arrow
        double total = 0; std::vector<std::pair<double, double>> pts;
        for (auto& p : rt) { if (!Project(f, p.first, los + p.second, sx, sy)) continue; if (!pts.empty()) total += hypot(sx - pts.back().first, sy - pts.back().second); pts.push_back({sx, sy}); }
        double left = total * draw; double ex = 0, ey = 0, dx = 0, dy = 0;
        for (size_t i = 1; i < pts.size() && left > 0; i++) {
            double L = hypot(pts[i].first - pts[i - 1].first, pts[i].second - pts[i - 1].second), u = std::min(1.0, left / std::max(0.01, L));
            ex = pts[i - 1].first + (pts[i].first - pts[i - 1].first) * u; ey = pts[i - 1].second + (pts[i].second - pts[i - 1].second) * u;
            dx = pts[i].first - pts[i - 1].first; dy = pts[i].second - pts[i - 1].second;
            GlowLine(pts[i - 1].first, pts[i - 1].second, ex, ey, 2.2, kAccent, 0.35 * show); GlowLine(pts[i - 1].first, pts[i - 1].second, ex, ey, 0.9, 0xBFEAFF, 0.95 * show);
            left -= L;
        }
        if (draw > 0.05) { double d = std::max(0.01, hypot(dx, dy)); dx /= d; dy /= d;
            for (int sgn : {-1, 1}) GlowLine(ex, ey, ex - dx * 8 + sgn * dy * 5, ey - dy * 8 - sgn * dx * 5, 0.9, 0xBFEAFF, 0.95 * show); }
    }
}
// the stadium scoreboard at the top of the Play page: amber dot-matrix text scrolling past
static void Scoreboard(HDC dc, RECT r, const std::wstring& text, uint32_t team, double t) {
    GlowRound(r, 8, 0x5A4310, 6, 0.5);
    FillRound(r, 8, 0x05070D, 0.96); StrokeRound(r, 8, 0x6B5218, 0.9);
    RECT tag{r.left + 6, r.top + 6, r.left + 76, r.bottom - 6}; FillRoundV(tag, 5, 0x2F8FFF, 0x0B4FC0, 1.0);
    Txt(dc, L"MAU", tag, g_fTicker, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    FillRound(RECT{tag.right + 6, r.top + 9, tag.right + 10, r.bottom - 9}, 2, team, 0.9);
    RECT win{tag.right + 18, r.top + 2, r.right - 10, r.bottom - 2};
    SelectObject(dc, g_fTicker); SIZE sz; GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    int span = sz.cx + 80, off = (int)fmod(t * 55.0, (double)span);
    HRGN clip = CreateRectRgn(win.left, win.top, win.right, win.bottom); SelectClipRgn(dc, clip);
    for (int k = 0; k < 3; k++) { RECT tr{win.left - off + k * span, win.top, win.left - off + k * span + sz.cx + 10, win.bottom}; if (tr.left < win.right) Txt(dc, text, tr, g_fTicker, RGB(255, 184, 64)); }
    SelectClipRgn(dc, nullptr); DeleteObject(clip);
    GdiFlush();
    for (int y = win.top; y < win.bottom; y++) for (int x = win.left; x < win.right; x++)   // LED grid
        if (x % 3 == 2 || y % 3 == 2) BlendPx(g_px[y * g_fw + x], 0x05070D, 0.7);
    for (int x : {win.left, win.right - 30}) for (int y = win.top; y < win.bottom; y++) for (int k = 0; k < 30; k++) {   // fade the ends
        int xx = x + k; double a = x == win.left ? 1.0 - k / 30.0 : k / 30.0; BlendPx(g_px[y * g_fw + xx], 0x05070D, a); }
}

// picture, shading and the sidebar's backdrop - the same every frame, so kept
static void BuildBase(HDC sdc, HDC dc, int w, int h) {
    int cx = kSideW, cw = w - cx;
    bool play = g_page == PAGE_PLAY;
    if (g_baseW == w && g_baseH == h && g_basePage == (play ? 1 : 0) && g_baseGen == g_bgGen && (int)g_base.size() == w * h) { memcpy(g_px, g_base.data(), (size_t)w * h * 4); return; }
    ScaleBackground(cw, h);
    if (g_scaled) { HDC s = CreateCompatibleDC(sdc); HGDIOBJ os = SelectObject(s, g_scaled); BitBlt(dc, cx, 0, cw, h, s, 0, 0, SRCCOPY); SelectObject(s, os); DeleteDC(s); }
    GdiFlush();
    if (!g_scaled) for (int y = 0; y < h; y++) for (int x = cx; x < w; x++) { uint32_t c = 0x0B1834; BlendPx(c, 0x03060E, (double)y / h); g_px[y * w + x] = c; }
    for (int y = 0; y < h; y++) for (int x = cx; x < w; x++) {
        uint32_t& p = g_px[y * w + x];
        double fx = (double)(x - cx) / cw, fy = (double)y / h;
        if (play) {
            BlendPx(p, kSide, std::max(0.0, 0.75 - fx * 2.2));            // left fade
            BlendPx(p, 0x03050B, std::max(0.0, (fy - 0.50) * 1.7));         // bottom fade
            BlendPx(p, 0x03050B, std::max(0.0, (0.06 - fy) * 6));           // a little at the top
        } else {
            BlendPx(p, 0x070B16, 0.84);
            BlendPx(p, 0x12305E, std::max(0.0, 0.22 - fy * 0.9) * (1 - fx * 0.6));   // cool glow behind the page title
        }
    }
    if (play) DrawFieldBase(cx, w, h);
    // sidebar: dark glass with a faint diagonal sheen and a lit right edge
    for (int y = 0; y < h; y++) for (int x = 0; x < cx; x++) {
        uint32_t c = kSide; BlendPx(c, 0x0F1830, 0.40 * (1.0 - (double)y / h));
        double d = fabs((x + y * 0.6) - h * 0.35) / (h * 0.25); if (d < 1) BlendPx(c, 0x1A2A50, 0.18 * (1 - d));
        g_px[y * w + x] = c;
    }
    for (int y = 0; y < h; y++) { BlendPx(g_px[y * w + cx - 1], 0x2F5FA8, 0.55 + 0.35 * sin(3.14159 * y / h)); BlendPx(g_px[y * w + cx - 2], 0x1A2E58, 0.5); }
    g_base.assign(g_px, g_px + (size_t)w * h); g_baseW = w; g_baseH = h; g_basePage = play ? 1 : 0; g_baseGen = g_bgGen;
}

// Compose the whole window picture.
static void BuildFrame() {
    RECT rc; GetClientRect(g_wnd, &rc); int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    HDC sdc = GetDC(nullptr);
    if (!g_frame || g_fw != w || g_fh != h) {
        if (g_frame) DeleteObject(g_frame);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        g_frame = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&g_px, nullptr, 0); g_fw = w; g_fh = h;
        g_baseW = 0;
    }
    int cx = kSideW;
    double t = AnimTime();
    HDC dc = CreateCompatibleDC(sdc); HGDIOBJ old = SelectObject(dc, g_frame);
    BuildBase(sdc, dc, w, h);
    g_animRect = RECT{cx, 0, w, h};
    if (g_page == PAGE_PLAY) { LightBeams(cx, w, h, t); ChalkPlays(cx, w, h, t); }
    g_hits.clear();
    // brand
    Txt(dc, L"MADDEN", RECT{26, 22, cx - 10, 54}, g_fTitle, RGB(255, 255, 255));
    Txt(dc, L"ARCADE", RECT{27, 51, cx - 9, 83}, g_fTitle, RGB(4, 10, 24));
    Txt(dc, L"ARCADE", RECT{26, 50, cx - 10, 82}, g_fTitle, RGB(110, 190, 255));
    {   RECT u{26, 86, 128, 106}; GlowRound(u, 6, kAccent, 5, 0.6); FillRoundV(u, 6, 0x48B0FF, 0x1C6FE0, 1.0); FillRound(RECT{u.left + 2, u.top + 1, u.right - 2, u.top + 3}, 1, 0xFFFFFF, 0.35);
        Txt(dc, L"UNLEASHED", u, g_fCap, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
    for (int x = 20; x < cx - 20; x++) BlendPx(g_px[118 * w + x], 0x2F5FA8, 0.5 * sin(3.14159 * (x - 20) / (cx - 40)));   // divider
    // nav
    for (int i = 0; i < PAGE_COUNT; i++) {
        RECT r{12, 132 + i * 52, cx - 12, 132 + i * 52 + 44};
        bool on = g_page == i, hov = g_hot == HIT_NAV + i;
        if (on) {
            FillRoundV(r, 10, 0x1F3360, 0x15223F, 1.0); StrokeRound(r, 10, 0x34528A, 0.7);
            RECT bar{r.left, r.top + 9, r.left + 4, r.bottom - 9}; GlowRound(bar, 2, kAccentHi, 6, 0.8); FillRound(bar, 2, kAccentHi, 1.0);
        } else if (hov) { FillRound(r, 10, 0x141E34, 1.0); StrokeRound(r, 10, 0x22314F, 0.6); }
        NavIcon(dc, i, r.left + 26 + (hov && !on ? 2 : 0), (r.top + r.bottom) / 2, on ? RGB(142, 227, 255) : hov ? RGB(200, 215, 240) : RGB(150, 165, 195));
        Txt(dc, kPageNames[i], RECT{r.left + 50 + (hov && !on ? 2 : 0), r.top, r.right, r.bottom}, g_fNav, on ? kText : hov ? RGB(220, 230, 248) : RGB(178, 190, 214));
        g_hits.push_back({r, HIT_NAV + i});
    }
    extern std::wstring g_versionLabel;
    {   std::wstring v = g_versionLabel.empty() ? std::wstring(L"Test build") : L"v" + g_versionLabel;
        SelectObject(dc, g_fCap); SIZE sz; GetTextExtentPoint32W(dc, v.c_str(), (int)v.size(), &sz);
        RECT chip{24, h - 70, 24 + sz.cx + 22, h - 48}; FillRound(chip, 11, 0x13213F, 1.0); StrokeRound(chip, 11, 0x2F5FA8, 0.8);
        Txt(dc, v, chip, g_fCap, RGB(142, 200, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
    Txt(dc, L"Fan project - use your own game files", RECT{26, h - 42, cx - 10, h - 22}, g_fSmall, RGB(92, 105, 135));

    if (g_page == PAGE_PLAY) {
        RefreshPlayInfo();
        int x0 = cx + 56;
        {   std::wstring tk = g_statusText;
            for (auto& c : tk) c = towupper(c);
            tk += L"   %C6   ROSTER " + g_info.roster + L"   %C6   " + g_info.tCap + L": " + g_info.tMain;
            if (g_info.tState == 1) tk += L"  " + g_info.tSub;
            if (g_playName && g_playAlpha > 0.5) tk += std::wstring(L"   %C6   ON THE BOARD: ") + g_playName;
            tk += L"   %C6   GRAPHICS " + g_info.gfx + L"   %C6   " + (g_versionLabel.empty() ? std::wstring(L"TEST BUILD") : L"VERSION " + g_versionLabel);
            for (auto& c : tk) c = towupper(c);
            Scoreboard(dc, RECT{x0, 20, w - 40, 58}, tk, g_info.tState == 1 || g_info.tState == 2 ? g_info.tColor : kAccent, t); }
        // mod update pill
        if (!g_updateMsg.empty()) {
            RECT pr{x0, g_fh - 362, std::min(w - 40, x0 + 820), g_fh - 328};
            FillRound(pr, 17, 0x2A2206, 0.92); StrokeRound(pr, 17, 0xFFC93A, 0.85);
            HBRUSH yb = CreateSolidBrush(RGB(255, 201, 58)); HGDIOBJ ob = SelectObject(dc, yb), op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, pr.left + 14, pr.top + 12, pr.left + 24, pr.top + 22); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(yb);
            Txt(dc, g_updateMsg, RECT{pr.left + 34, pr.top, pr.right - 14, pr.bottom}, g_fBody, RGB(255, 226, 150));
        }
        // PLAY / STOP - a slanted button with a glow and a shine that sweeps across it every few seconds
        RECT pb = PlayBtnRect();
        bool run = g_proc != nullptr, hov = g_hot == HIT_PLAY, dn = hov && g_down;
        if (hov && !dn) OffsetRect(&pb, 0, -2);
        double breathe = 0.75 + 0.25 * sin(t * 2.0);
        GlowSkew(pb, 12, kSlant, run ? 0xFF4D4D : kAccent, hov ? 16 : 11, hov ? 1.0 : breathe);
        if (run) FillSkew(pb, 12, kSlant, dn ? 0xB01E2A : 0xF0444E, dn ? 0xF0444E : 0xA3121E, 1.0);
        else FillSkew(pb, 12, kSlant, dn ? 0x0B4FC0 : hov ? 0x6CC6FF : 0x48B0FF, dn ? 0x2F8FFF : 0x0B4FC0, 1.0);
        FillSkew(RECT{pb.left + 6, pb.top + 3, pb.right - 6, pb.top + (pb.bottom - pb.top) / 2}, 9, kSlant, 0xFFFFFF, 0xFFFFFF, 0.10);   // glassy top half
        if (!run) { double ph = fmod(t, 4.0) / 1.1; if (hov) ph = fmod(t, 1.6) / 1.1; if (ph < 1) ShineSkew(pb, 12, kSlant, ph, 0.35); }
        StrokeSkew(pb, 12, kSlant, run ? 0xFFB0B0 : kAccentHi, 0.9, 1.4);
        {   // label with a play triangle / stop square, centred on the slanted face
            std::wstring lab = run ? L"STOP" : L"PLAY";
            SelectObject(dc, g_fPlay); SIZE sz; GetTextExtentPoint32W(dc, lab.c_str(), (int)lab.size(), &sz);
            int face = (pb.left + SkewRight(pb, kSlant)) / 2;
            int iw = 24, gap = 16, tot = iw + gap + sz.cx, lx = face - tot / 2, my = (pb.top + pb.bottom) / 2 + (dn ? 1 : 0);
            HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255)); HGDIOBJ ob = SelectObject(dc, wb), op = SelectObject(dc, GetStockObject(NULL_PEN));
            if (run) Rectangle(dc, lx + 3, my - 10, lx + 23, my + 10);
            else { POINT tri[3] = {{lx + 4, my - 13}, {lx + 26, my}, {lx + 1, my + 13}}; Polygon(dc, tri, 3); }
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(wb);
            RECT tr{lx + iw + gap, pb.top + (dn ? 1 : 0), pb.right + 200, pb.bottom + (dn ? 1 : 0)};
            Txt(dc, lab, RECT{tr.left + 2, tr.top + 3, tr.right + 2, tr.bottom + 3}, g_fPlay, RGB(0, 18, 50));
            Txt(dc, lab, tr, g_fPlay, RGB(255, 255, 255));
        }
        { RECT hp = PlayBtnRect(); g_hits.push_back({RECT{hp.left, hp.top, SkewRight(hp, kSlant), hp.bottom}, HIT_PLAY}); }
        // TOWERS: your climb, in gold
        {
            RECT tb = TowerBtnRect(); bool th = g_hot == HIT_TOWER && !run;
            if (th && !g_down) OffsetRect(&tb, 0, -2);
            uint32_t gold = g_info.tState == -1 ? 0x56627C : 0xE8B33A;
            if (th) GlowSkew(tb, 12, kSlant, gold, 10, 0.8);
            FillSkew(tb, 12, kSlant, th ? 0x1E2A48 : 0x162038, 0x0C1222, 0.94);
            FillSkew(RECT{tb.left + 4, tb.top + 2, tb.right - 4, tb.top + 5}, 2, kSlant, gold, gold, 0.55);
            if (g_info.tState >= 1) FillSkew(RECT{tb.left + 8, tb.bottom - 7, tb.left + 140, tb.bottom - 4}, 1, kSlant, g_info.tColor, g_info.tColor, 0.95);   // your team's colour
            StrokeSkew(tb, 12, kSlant, gold, th ? 1.0 : 0.75, 1.2);
            // a small tower of plates on the left
            int ix = tb.left + 30, iy = tb.bottom - 16;
            for (int k = 0; k < 4; k++) { int ww = 30 - k * 6; RECT pl{ix - ww / 2 + k * 2, iy - (k + 1) * 11, ix + ww / 2 + k * 2, iy - (k + 1) * 11 + 8};
                bool lit = g_info.tState == 2 || (g_info.tState == 1 && k <= (int)(t * 1.5) % 4); FillRound(pl, 2, lit ? gold : 0x2C3A5C, lit ? 0.95 : 0.9); }
            int tx = tb.left + 64;
            Txt(dc, g_info.tCap, RECT{tx, tb.top + 8, SkewRight(tb, kSlant) - 10, tb.top + 26}, g_fCap, g_info.tState == -1 ? RGB(130, 140, 160) : RGB(255, 200, 80));
            Txt(dc, g_info.tMain, RECT{tx - 3, tb.top + 25, SkewRight(tb, kSlant) - 14, tb.top + 50}, g_fCardV, kText);
            Txt(dc, g_info.tSub, RECT{tx - 6, tb.top + 50, SkewRight(tb, kSlant) - 18, tb.top + 70}, g_fSmall, kDim);
            { RECT ht = TowerBtnRect(); g_hits.push_back({RECT{ht.left, ht.top, SkewRight(ht, kSlant), ht.bottom}, HIT_TOWER}); }
        }
        // status, with a pulsing dot
        {   RECT pbs = PlayBtnRect(); int dy = pbs.bottom + 30, dx = x0 + 9;
            uint32_t dc32 = (uint32_t)GetRValue(g_statusDot) << 16 | GetGValue(g_statusDot) << 8 | GetBValue(g_statusDot);
            double pr = 6 + 4 * (0.5 + 0.5 * sin(t * 3.0));
            for (int y = dy - 14; y <= dy + 14; y++) for (int x = dx - 14; x <= dx + 14; x++) {
                if (x < 0 || y < 0 || x >= w || y >= h) continue; double d = sqrt((double)(x - dx) * (x - dx) + (y - dy) * (y - dy));
                if (d < 5) BlendPx(g_px[y * w + x], dc32, std::clamp(5.0 - d, 0.0, 1.0)); else if (d < 5 + pr) BlendPx(g_px[y * w + x], dc32, 0.35 * (1 - (d - 5) / pr));
            }
            Txt(dc, g_statusText, RECT{x0 + 26, dy - 13, w - 40, dy + 13}, g_fBodyB, kText); }
        // info cards: separate glass cards that lift when you point at them
        struct C { const wchar_t* cap; const std::wstring* val; } cards[3] = {{L"GAME FILES", &g_info.files}, {L"GRAPHICS", &g_info.gfx}, {L"ROSTER", &g_info.roster}};
        int top = h - 134, bot = h - 40, left = x0, right = w - 40, n = 3, gap = 14, cwid = (right - left - gap * (n - 1)) / n;
        for (int i = 0; i < n; i++) {
            bool ch = g_hot == HIT_CARD + i;
            RECT r{left + i * (cwid + gap), top - (ch ? 4 : 0), (i == n - 1 ? right : left + i * (cwid + gap) + cwid), bot - (ch ? 4 : 0)};
            if (ch) GlowRound(r, 14, kAccent, 8, 0.6);
            FillRoundV(r, 14, ch ? 0x182644 : 0x111B32, 0x0A1020, 0.86);
            StrokeRound(r, 14, ch ? 0x4F8FE8 : 0x2C3B5C, 0.9);
            for (int x = r.left + 14; x < r.right - 14; x++) BlendPx(g_px[(r.top + 1) * w + x], 0xFFFFFF, 0.08);   // glass edge
            FillRound(RECT{r.left + 18, r.bottom - 5, r.left + 18 + (ch ? r.right - r.left - 36 : 36), r.bottom - 2}, 1, kAccent, ch ? 1.0 : 0.7);
            Txt(dc, cards[i].cap, RECT{r.left + 20, r.top + 16, r.right - 40, r.top + 34}, g_fCap, RGB(140, 156, 190));
            Txt(dc, *cards[i].val, RECT{r.left + 20, r.top + 38, r.right - 44, r.top + 68}, g_fCardV, kText);
            Txt(dc, L"\x203A", RECT{r.right - 40, r.top, r.right - 16, r.bottom}, g_fH1, ch ? RGB(142, 227, 255) : RGB(150, 165, 195), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            g_hits.push_back({RECT{r.left, top - 4, r.right, bot}, HIT_CARD + i});
        }
    } else {
        Txt(dc, kPageNames[g_page], RECT{cx + 40, 20, w - 40, 64}, g_fH1, kText);
        RECT ul{cx + 42, 62, cx + 98, 65}; GlowRound(ul, 2, kAccent, 5, 0.7); FillRound(ul, 2, kAccent, 1.0);
        const wchar_t* sub = g_page == PAGE_SETTINGS ? L"Game files, renderer and picture quality" : g_page == PAGE_MODS ? L"Roster, players per side and the pre-game lineup window" : g_page == PAGE_ONLINE ? L"Your gamertag, and playing a friend in Xbox Live mode" : L"Everything the game prints while it runs (also saved to madden_log.txt)";
        Txt(dc, sub, RECT{cx + 42, 70, w - 40, 92}, g_fBody, kDim);
        g_card = RECT{cx + 32, 100, w - 32, h - 28};
        GlowRound(g_card, 14, 0x1C3A6E, 10, 0.5);
        FillRound(g_card, 14, (uint32_t)GetRValue(kCardC) << 16 | GetGValue(kCardC) << 8 | GetBValue(kCardC), 1.0);
        StrokeRound(g_card, 14, 0x2C3B5C, 1.0);
        for (int x = g_card.left + 16; x < g_card.right - 16; x++) BlendPx(g_px[(g_card.top + 1) * w + x], 0x4F8FE8, 0.35 * sin(3.14159 * (x - g_card.left - 16) / (g_card.right - g_card.left - 32)));
    }
    GdiFlush();
    SelectObject(dc, old); DeleteDC(dc); ReleaseDC(nullptr, sdc);
}

static void Repaint(bool children) {
    BuildFrame();
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | (children ? RDW_ALLCHILDREN : 0));
}

static void ThemeFonts() {
    auto mk = [](int px, int weight, bool italic, const wchar_t* face) {
        return CreateFontW(-px, 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g_fTitle = mk(30, FW_BOLD, true, L"Bahnschrift SemiBold Condensed");
    g_fNav = mk(17, FW_SEMIBOLD, false, L"Segoe UI");
    g_fPlay = mk(36, FW_BOLD, true, L"Bahnschrift SemiBold Condensed");
    g_fH1 = mk(30, FW_BOLD, false, L"Segoe UI");
    g_fBody = mk(15, FW_NORMAL, false, L"Segoe UI");
    g_fBodyB = mk(16, FW_SEMIBOLD, false, L"Segoe UI");
    g_fSmall = mk(12, FW_NORMAL, false, L"Segoe UI");
    g_fCap = mk(12, FW_BOLD, false, L"Segoe UI");
    g_fCardV = mk(19, FW_BOLD, false, L"Segoe UI");
    g_fTicker = mk(17, FW_BOLD, false, L"Consolas");
    g_cardBrush = CreateSolidBrush(kCardC); g_editBrush = CreateSolidBrush(kEditBg);
}

// push buttons become custom-drawn; check boxes lose the theme so their text can be light; dark combo boxes / scroll bars
static LRESULT CALLBACK BtnHover(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static void ThemeChildren(HWND parent) {
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[32]; GetClassNameW(c, cls, 32);
        if (!_wcsicmp(cls, L"ComboBox")) { SetWindowTheme(c, L"DarkMode_CFD", nullptr); continue; }
        if (!_wcsicmp(cls, L"Edit")) { SetWindowTheme(c, L"DarkMode_Explorer", nullptr); continue; }
        if (_wcsicmp(cls, L"Button")) continue;
        LONG_PTR st = GetWindowLongPtrW(c, GWL_STYLE); int type = (int)(st & BS_TYPEMASK);
        if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) { SetWindowLongPtrW(c, GWL_STYLE, (st & ~BS_TYPEMASK) | BS_OWNERDRAW); SetWindowSubclass(c, BtnHover, 1, 0); }
        else SetWindowTheme(c, L"", L"");
    }
}

// buttons on the solid card: slanted like PLAY, drawn with the same smooth helpers into a small picture of their own.
// They light up when you point at them (hover is tracked by a subclass) and carry a small icon by what they do.
static HWND g_btnHot;
static LRESULT CALLBACK BtnHover(HWND hw, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEMOVE && g_btnHot != hw) { HWND old = g_btnHot; g_btnHot = hw; if (old) InvalidateRect(old, nullptr, FALSE); InvalidateRect(hw, nullptr, FALSE);
        TRACKMOUSEEVENT tm{sizeof tm, TME_LEAVE, hw, 0}; TrackMouseEvent(&tm); }
    if (m == WM_MOUSELEAVE && g_btnHot == hw) { g_btnHot = nullptr; InvalidateRect(hw, nullptr, FALSE); }
    if (m == WM_SETCURSOR) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
    return DefSubclassProc(hw, m, wp, lp);
}
enum { ICO_NONE, ICO_FOLDER, ICO_BOX, ICO_CHECK, ICO_X, ICO_IMAGE, ICO_DOC, ICO_STAR, ICO_REFRESH, ICO_SHEET, ICO_ARROW };
static int ButtonIcon(const std::wstring& t) {
    auto has = [&](const wchar_t* k) { std::wstring a = t, b = k; for (auto& c : a) c = towlower(c); return a.find(b) != std::wstring::npos; };
    if (has(L"folder")) return ICO_FOLDER; if (has(L"package")) return ICO_BOX; if (has(L"save")) return ICO_CHECK; if (has(L"remove") || has(L"leave") || has(L"cancel")) return ICO_X;
    if (has(L"picture")) return ICO_IMAGE; if (has(L"log")) return ICO_DOC; if (has(L"recommend")) return ICO_STAR; if (has(L"rebuild") || has(L"refresh")) return ICO_REFRESH;
    if (has(L"ratings")) return ICO_SHEET; return ICO_ARROW;
}
static void DrawIcon(HDC dc, int ico, int cx, int cy, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 2, c); HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (ico) {
    case ICO_FOLDER: { POINT p[6] = {{cx - 8, cy - 5}, {cx - 3, cy - 5}, {cx - 1, cy - 3}, {cx + 8, cy - 3}, {cx + 8, cy + 6}, {cx - 8, cy + 6}}; Polygon(dc, p, 6); break; }
    case ICO_BOX: Rectangle(dc, cx - 7, cy - 4, cx + 8, cy + 7); MoveToEx(dc, cx - 8, cy - 4, nullptr); LineTo(dc, cx - 4, cy - 8); LineTo(dc, cx + 5, cy - 8); LineTo(dc, cx + 9, cy - 4); MoveToEx(dc, cx, cy - 4, nullptr); LineTo(dc, cx, cy + 1); break;
    case ICO_CHECK: MoveToEx(dc, cx - 7, cy, nullptr); LineTo(dc, cx - 2, cy + 5); LineTo(dc, cx + 8, cy - 6); break;
    case ICO_X: MoveToEx(dc, cx - 6, cy - 6, nullptr); LineTo(dc, cx + 7, cy + 7); MoveToEx(dc, cx + 6, cy - 6, nullptr); LineTo(dc, cx - 7, cy + 7); break;
    case ICO_IMAGE: Rectangle(dc, cx - 8, cy - 6, cx + 9, cy + 7); MoveToEx(dc, cx - 6, cy + 4, nullptr); LineTo(dc, cx - 1, cy - 1); LineTo(dc, cx + 2, cy + 2); LineTo(dc, cx + 4, cy); LineTo(dc, cx + 7, cy + 4); Ellipse(dc, cx + 2, cy - 4, cx + 5, cy - 1); break;
    case ICO_DOC: Rectangle(dc, cx - 6, cy - 8, cx + 7, cy + 8); for (int k = 0; k < 3; k++) { MoveToEx(dc, cx - 3, cy - 3 + k * 4, nullptr); LineTo(dc, cx + 4, cy - 3 + k * 4); } break;
    case ICO_STAR: { POINT p[10]; for (int k = 0; k < 10; k++) { double an = -1.5708 + k * 3.14159 / 5, r = k % 2 ? 3.5 : 8.5; p[k] = {cx + (int)lround(cos(an) * r), cy + 1 + (int)lround(sin(an) * r)}; } Polygon(dc, p, 10); break; }
    case ICO_REFRESH: Arc(dc, cx - 7, cy - 7, cx + 8, cy + 8, cx + 7, cy - 3, cx + 7, cy + 1); MoveToEx(dc, cx + 7, cy - 8, nullptr); LineTo(dc, cx + 7, cy - 3); LineTo(dc, cx + 2, cy - 3); break;
    case ICO_SHEET: Rectangle(dc, cx - 8, cy - 7, cx + 9, cy + 8); MoveToEx(dc, cx - 8, cy - 2, nullptr); LineTo(dc, cx + 9, cy - 2); MoveToEx(dc, cx - 8, cy + 3, nullptr); LineTo(dc, cx + 9, cy + 3); MoveToEx(dc, cx - 2, cy - 7, nullptr); LineTo(dc, cx - 2, cy + 8); break;
    case ICO_ARROW: MoveToEx(dc, cx - 7, cy, nullptr); LineTo(dc, cx + 7, cy); MoveToEx(dc, cx + 2, cy - 5, nullptr); LineTo(dc, cx + 7, cy); LineTo(dc, cx + 2, cy + 5); break;
    }
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}
static void DrawButton(const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem; int bw = r.right - r.left, bh = r.bottom - r.top; if (bw <= 0 || bh <= 0) return;
    bool down = di->itemState & ODS_SELECTED, dis = di->itemState & ODS_DISABLED, hot = g_btnHot == di->hwndItem && !dis, focus = di->itemState & ODS_FOCUS;
    wchar_t t[128]; GetWindowTextW(di->hwndItem, t, 128);
    bool primary = !wcscmp(t, L"Save") || !wcsncmp(t, L"Use recommended", 15), danger = ButtonIcon(t) == ICO_X;
    // draw with the frame helpers into a picture the size of the button
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = bw; bi.bmiHeader.biHeight = -bh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    uint32_t* px = nullptr; HBITMAP bmp = CreateDIBSection(di->hDC, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0); if (!bmp) return;
    HDC m = CreateCompatibleDC(di->hDC); HGDIOBJ om = SelectObject(m, bmp);
    uint32_t* spx = g_px; int sw = g_fw, sh = g_fh; g_px = px; g_fw = bw; g_fh = bh;
    uint32_t card = (uint32_t)GetRValue(kCardC) << 16 | GetGValue(kCardC) << 8 | GetBValue(kCardC);
    for (int i = 0; i < bw * bh; i++) px[i] = card;
    const double k = 0.18; int slant = (int)ceil((bh - 6) * k);
    RECT f{3, 3 + (down ? 1 : 0), bw - 3 - slant, bh - 3 + (down ? 1 : 0)};
    uint32_t edge = dis ? 0x2A3346 : primary ? kAccentHi : danger ? 0xFF7A7A : hot ? 0x5FA8FF : 0x3A4A6C;
    if (hot || (primary && !dis)) GlowSkew(RECT{f.left + 1, f.top + 1, f.right - 1, f.bottom - 1}, 7, k, primary ? kAccent : danger ? 0xFF4D4D : kAccent, 3, hot ? 0.9 : 0.5);
    if (dis) FillSkew(f, 7, k, 0x222938, 0x191E2A, 1.0);
    else if (primary) FillSkew(f, 7, k, down ? 0x0B4FC0 : hot ? 0x6CC6FF : 0x48B0FF, down ? 0x2F8FFF : 0x0B4FC0, 1.0);
    else if (danger) FillSkew(f, 7, k, down ? 0x3A1218 : hot ? 0x5A1E26 : 0x3A1A22, 0x24121A, 1.0);
    else FillSkew(f, 7, k, down ? 0x16213A : hot ? 0x2C3C60 : 0x24314F, down ? 0x24314F : 0x161F36, 1.0);
    if (!dis) FillSkew(RECT{f.left + 3, f.top + 2, f.right - 3, (f.top + f.bottom) / 2}, 5, k, 0xFFFFFF, 0xFFFFFF, primary ? 0.14 : 0.06);   // glass
    if (!dis && !primary) FillSkew(RECT{f.left + 4, f.bottom - 4, f.left + 4 + (hot ? f.right - f.left - 8 : 18), f.bottom - 2}, 1, k, danger ? 0xFF7A7A : kAccent, danger ? 0xFF7A7A : kAccent, hot ? 0.95 : 0.7);   // accent line
    StrokeSkew(f, 7, k, edge, focus ? 1.0 : 0.85, focus ? 1.6 : 1.1);
    g_px = spx; g_fw = sw; g_fh = sh;
    // icon and label
    int ico = ButtonIcon(t);
    SelectObject(m, g_fBodyB); SIZE sz; GetTextExtentPoint32W(m, t, (int)wcslen(t), &sz);
    int iw = 18, gap = 8, tot = iw + gap + sz.cx, face = (f.left + f.right + slant) / 2, lx = std::max<int>(f.left + 10, face - tot / 2), my = (f.top + f.bottom) / 2;
    COLORREF tc = dis ? RGB(110, 120, 140) : primary ? RGB(255, 255, 255) : danger ? RGB(255, 200, 200) : hot ? RGB(255, 255, 255) : RGB(222, 232, 250);
    COLORREF icC = dis ? RGB(90, 100, 120) : primary ? RGB(255, 255, 255) : danger ? RGB(255, 140, 140) : RGB(142, 227, 255);
    DrawIcon(m, ico, lx + iw / 2 + (hot ? 1 : 0), my, icC);
    RECT tr{lx + iw + gap, f.top, f.right + slant, f.bottom};
    SetBkMode(m, TRANSPARENT);
    if (primary && !dis) { SetTextColor(m, RGB(0, 24, 64)); RECT sh2 = tr; OffsetRect(&sh2, 1, 1); DrawTextW(m, t, -1, &sh2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); }
    SetTextColor(m, tc); DrawTextW(m, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    GdiFlush();
    BitBlt(di->hDC, r.left, r.top, bw, bh, m, 0, 0, SRCCOPY);
    SelectObject(m, om); DeleteDC(m); DeleteObject(bmp);
}

static int HitAt(int x, int y) { for (auto& hh : g_hits) if (x >= hh.r.left && x < hh.r.right && y >= hh.r.top && y < hh.r.bottom) return hh.id; return -1; }

static void DarkTitleBar(HWND hw) {
    if (HMODULE d = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT(WINAPI * F)(HWND, DWORD, LPCVOID, DWORD);
        if (F f = (F)(void*)GetProcAddress(d, "DwmSetWindowAttribute")) { BOOL on = TRUE; f(hw, 20, &on, sizeof on); }
    }
}
