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
static HFONT g_fTitle, g_fNav, g_fPlay, g_fH1, g_fBody, g_fBodyB, g_fSmall, g_fCap, g_fCardV;
static RECT g_card;                          // the solid card of Settings / Mods / Log
static std::wstring g_statusText = L"Ready to play"; static COLORREF g_statusDot = RGB(46, 204, 113);
static std::wstring g_updateMsg;
std::wstring g_versionLabel;

static const COLORREF kText = RGB(236, 242, 255), kDim = RGB(148, 163, 194), kCardC = RGB(16, 22, 38), kEditBg = RGB(10, 15, 28);
static const uint32_t kSide = 0x0A0E18, kAccent = 0x2F8FFF, kAccentHi = 0x8EE3FF;

struct Hit { RECT r; int id; };
static std::vector<Hit> g_hits;              // nav 0..3, PLAY 10, info cards 20..22
enum { HIT_NAV = 0, HIT_PLAY = 10, HIT_CARD = 20 };

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
    Repaint(true);
}
static RECT PlayBtnRect() { int cx = kSideW + 56; return RECT{cx, g_fh - 286, cx + 340, g_fh - 220}; }

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
    }
    int cx = kSideW, cw = w - cx;
    ScaleBackground(cw, h);
    HDC dc = CreateCompatibleDC(sdc); HGDIOBJ old = SelectObject(dc, g_frame);
    // 1. picture (or a plain backdrop)
    if (g_scaled) { HDC s = CreateCompatibleDC(sdc); HGDIOBJ os = SelectObject(s, g_scaled); BitBlt(dc, cx, 0, cw, h, s, 0, 0, SRCCOPY); SelectObject(s, os); DeleteDC(s); }
    GdiFlush();
    if (!g_scaled) for (int y = 0; y < h; y++) for (int x = cx; x < w; x++) { uint32_t c = 0x0B1834; BlendPx(c, 0x03060E, (double)y / h); g_px[y * w + x] = c; }
    // 2. shading over the picture: fade in from the sidebar, darker bottom; pages other than Play dim it a lot
    for (int y = 0; y < h; y++) for (int x = cx; x < w; x++) {
        uint32_t& p = g_px[y * w + x];
        double fx = (double)(x - cx) / cw, fy = (double)y / h;
        if (g_page == PAGE_PLAY) {
            BlendPx(p, kSide, std::max(0.0, 0.75 - fx * 2.2));            // left fade
            BlendPx(p, 0x03050B, std::max(0.0, (fy - 0.55) * 1.6));         // bottom fade
        } else BlendPx(p, 0x070B16, 0.82);
    }
    // 3. sidebar
    for (int y = 0; y < h; y++) for (int x = 0; x < cx; x++) { uint32_t c = kSide; BlendPx(c, 0x0F1830, 0.35 * (1.0 - (double)y / h)); g_px[y * w + x] = c; }
    for (int y = 0; y < h; y++) BlendPx(g_px[y * w + cx - 1], 0x24324F, 1.0);
    g_hits.clear();
    // brand
    Txt(dc, L"MADDEN", RECT{26, 24, cx - 10, 56}, g_fTitle, RGB(255, 255, 255));
    Txt(dc, L"ARCADE", RECT{26, 52, cx - 10, 84}, g_fTitle, RGB(110, 190, 255));
    FillRound(RECT{26, 86, 124, 106}, 6, kAccent, 1.0);
    Txt(dc, L"UNLEASHED", RECT{26, 86, 124, 106}, g_fCap, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // nav
    for (int i = 0; i < PAGE_COUNT; i++) {
        RECT r{12, 128 + i * 52, cx - 12, 128 + i * 52 + 44};
        bool on = g_page == i, hov = g_hot == HIT_NAV + i;
        if (on) { FillRound(r, 10, 0x1B2A48, 1.0); FillRound(RECT{r.left, r.top + 10, r.left + 4, r.bottom - 10}, 2, kAccent, 1.0); }
        else if (hov) FillRound(r, 10, 0x141E34, 1.0);
        NavIcon(dc, i, r.left + 26, (r.top + r.bottom) / 2, on ? RGB(142, 227, 255) : RGB(150, 165, 195));
        Txt(dc, kPageNames[i], RECT{r.left + 50, r.top, r.right, r.bottom}, g_fNav, on ? kText : RGB(178, 190, 214));
        g_hits.push_back({r, HIT_NAV + i});
    }
    extern std::wstring g_versionLabel;
    Txt(dc, g_versionLabel.empty() ? std::wstring(L"Test build") : L"Version " + g_versionLabel, RECT{26, h - 64, cx - 10, h - 44}, g_fSmall, RGB(120, 134, 165));
    Txt(dc, L"Fan project - use your own game files", RECT{26, h - 44, cx - 10, h - 24}, g_fSmall, RGB(92, 105, 135));

    if (g_page == PAGE_PLAY) {
        int x0 = cx + 56;
        // mod update pill
        if (!g_updateMsg.empty()) {
            RECT pr{x0, g_fh - 340, std::min(w - 40, x0 + 760), g_fh - 306};
            FillRound(pr, 17, 0x3A2E08, 0.92); StrokeRound(pr, 17, 0xFFC93A, 0.9);
            Txt(dc, L"\x2605  " + g_updateMsg, RECT{pr.left + 16, pr.top, pr.right - 12, pr.bottom}, g_fBody, RGB(255, 222, 140));
        }
        // PLAY / STOP
        RECT pb = PlayBtnRect();
        bool run = g_proc != nullptr, hov = g_hot == HIT_PLAY, dn = hov && g_down;
        GlowRound(pb, 14, run ? 0xFF4D4D : kAccent, hov ? 14 : 9, 0.9);
        if (run) FillRoundV(pb, 14, dn ? 0xB01E2A : 0xF0444E, dn ? 0xF0444E : 0xA3121E, 1.0);
        else FillRoundV(pb, 14, dn ? 0x0B4FC0 : hov ? 0x6CC6FF : 0x48B0FF, dn ? 0x2F8FFF : 0x0B4FC0, 1.0);
        FillRound(RECT{pb.left + 2, pb.top + 2, pb.right - 2, pb.top + 4}, 2, 0xFFFFFF, 0.35);
        StrokeRound(pb, 14, run ? 0xFFB0B0 : kAccentHi, 0.85);
        {   // label with a play triangle / stop square
            std::wstring t = run ? L"STOP" : L"PLAY";
            SelectObject(dc, g_fPlay); SIZE sz; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
            int iw = 22, gap = 16, tot = iw + gap + sz.cx, lx = (pb.left + pb.right - tot) / 2, my = (pb.top + pb.bottom) / 2 + (dn ? 1 : 0);
            HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255)); HGDIOBJ ob = SelectObject(dc, wb), op = SelectObject(dc, GetStockObject(NULL_PEN));
            if (run) Rectangle(dc, lx + 2, my - 9, lx + 20, my + 9);
            else { POINT tri[3] = {{lx + 2, my - 12}, {lx + 22, my}, {lx + 2, my + 12}}; Polygon(dc, tri, 3); }
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(wb);
            RECT tr{lx + iw + gap, pb.top + (dn ? 1 : 0), pb.right, pb.bottom + (dn ? 1 : 0)};
            Txt(dc, t, RECT{tr.left + 1, tr.top + 2, tr.right + 1, tr.bottom + 2}, g_fPlay, RGB(0, 18, 50));
            Txt(dc, t, tr, g_fPlay, RGB(255, 255, 255));
        }
        g_hits.push_back({pb, HIT_PLAY});
        // status
        HBRUSH db = CreateSolidBrush(g_statusDot); HGDIOBJ ob = SelectObject(dc, db), op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, x0 + 4, pb.bottom + 24, x0 + 14, pb.bottom + 34); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(db);
        Txt(dc, g_statusText, RECT{x0 + 24, pb.bottom + 16, w - 40, pb.bottom + 42}, g_fBodyB, kText);
        // info cards
        wchar_t path[MAX_PATH * 2] = L""; GetWindowTextW(g_path, path, MAX_PATH * 2);
        std::wstring p = path; while (!p.empty() && (p.back() == L'\\' || p.back() == L' ')) p.pop_back();
        std::wstring files = p.empty() ? L"Not set" : p.substr(p.find_last_of(L"\\/") == std::wstring::npos ? 0 : p.find_last_of(L"\\/") + 1);
        std::wstring q = IniRead(L"\\settings.ini", L"settings", L"quality", L"medium"), fs = IniRead(L"\\settings.ini", L"settings", L"fullscreen", L"1");
        std::wstring roster = FileWord(g_exeDir + L"\\mods\\roster.txt", L"original"), lines = FileWord(g_exeDir + L"\\mods\\lines.txt", L"5");
        struct C { const wchar_t* cap; std::wstring val; } cards[3] = {
            {L"GAME FILES", files}, {L"GRAPHICS", Cap(q) + (fs == L"1" ? L"  \x2022  Fullscreen" : L"  \x2022  Windowed")},
            {L"ROSTER", (roster == L"original" ? L"Original" : roster) + L"  \x2022  " + (lines == L"full" ? L"Full lines" : lines + L" on " + lines)}};
        int top = h - 128, bot = h - 36, left = x0, right = w - 40, n = 3, gap = 0, cwid = (right - left) / n;
        RECT all{left, top, right, bot};
        FillRound(all, 14, 0x0B1222, 0.80); StrokeRound(all, 14, 0x2C3B5C, 0.9);
        for (int i = 0; i < n; i++) {
            RECT r{left + i * cwid + gap, top, (i == n - 1 ? right : left + (i + 1) * cwid), bot};
            if (g_hot == HIT_CARD + i) FillRound(RECT{r.left + 3, r.top + 3, r.right - 3, r.bottom - 3}, 12, 0x1B2A48, 0.8);
            if (i) for (int y = top + 16; y < bot - 16; y++) BlendPx(g_px[y * w + r.left], 0x2C3B5C, 1.0);
            Txt(dc, cards[i].cap, RECT{r.left + 24, r.top + 18, r.right - 40, r.top + 38}, g_fCap, RGB(140, 156, 190));
            Txt(dc, cards[i].val, RECT{r.left + 24, r.top + 42, r.right - 44, r.top + 72}, g_fCardV, kText);
            Txt(dc, L"\x203A", RECT{r.right - 40, r.top, r.right - 18, r.bottom}, g_fH1, RGB(150, 165, 195), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            g_hits.push_back({r, HIT_CARD + i});
        }
    } else {
        Txt(dc, kPageNames[g_page], RECT{cx + 40, 22, w - 40, 66}, g_fH1, kText);
        const wchar_t* sub = g_page == PAGE_SETTINGS ? L"Game files, renderer and picture quality" : g_page == PAGE_MODS ? L"Roster, players per side and the pre-game lineup window" : g_page == PAGE_ONLINE ? L"Your gamertag, and playing a friend in Xbox Live mode" : L"Everything the game prints while it runs (also saved to madden_log.txt)";
        Txt(dc, sub, RECT{cx + 42, 62, w - 40, 84}, g_fBody, kDim);
        g_card = RECT{cx + 32, 100, w - 32, h - 28};
        FillRound(g_card, 14, (uint32_t)GetRValue(kCardC) << 16 | GetGValue(kCardC) << 8 | GetBValue(kCardC), 1.0);
        StrokeRound(g_card, 14, 0x2C3B5C, 1.0);
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
    g_cardBrush = CreateSolidBrush(kCardC); g_editBrush = CreateSolidBrush(kEditBg);
}

// push buttons become custom-drawn; check boxes lose the theme so their text can be light; dark combo boxes / scroll bars
static void ThemeChildren(HWND parent) {
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[32]; GetClassNameW(c, cls, 32);
        if (!_wcsicmp(cls, L"ComboBox")) { SetWindowTheme(c, L"DarkMode_CFD", nullptr); continue; }
        if (!_wcsicmp(cls, L"Edit")) { SetWindowTheme(c, L"DarkMode_Explorer", nullptr); continue; }
        if (_wcsicmp(cls, L"Button")) continue;
        LONG_PTR st = GetWindowLongPtrW(c, GWL_STYLE); int type = (int)(st & BS_TYPEMASK);
        if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) SetWindowLongPtrW(c, GWL_STYLE, (st & ~BS_TYPEMASK) | BS_OWNERDRAW);
        else SetWindowTheme(c, L"", L"");
    }
}

// buttons on the solid card
static void DrawButton(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC; RECT r = di->rcItem;
    FillRect(dc, &r, g_cardBrush);
    bool down = di->itemState & ODS_SELECTED, dis = di->itemState & ODS_DISABLED;
    wchar_t t[128]; GetWindowTextW(di->hwndItem, t, 128);
    bool primary = !wcscmp(t, L"Save");
    TRIVERTEX v[2] = {}; GRADIENT_RECT gr{0, 1};
    auto col = [](TRIVERTEX& tv, uint32_t c) { tv.Red = (COLOR16)(((c >> 16) & 255) << 8); tv.Green = (COLOR16)(((c >> 8) & 255) << 8); tv.Blue = (COLOR16)((c & 255) << 8); };
    RECT b = r; InflateRect(&b, -1, -1);
    HRGN rg = CreateRoundRectRgn(b.left, b.top, b.right + 1, b.bottom + 1, 10, 10); SelectClipRgn(dc, rg);
    v[0].x = b.left; v[0].y = b.top; v[1].x = b.right; v[1].y = b.bottom;
    if (dis) { col(v[0], 0x252C3B); col(v[1], 0x1A202C); }
    else if (primary) { col(v[0], down ? 0x0B4FC0 : 0x48B0FF); col(v[1], down ? 0x2F8FFF : 0x0B4FC0); }
    else { col(v[0], down ? 0x18233A : 0x26324D); col(v[1], down ? 0x26324D : 0x1A2338); }
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    SelectClipRgn(dc, nullptr); DeleteObject(rg);
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? RGB(142, 227, 255) : RGB(58, 74, 108));
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, b.left, b.top, b.right, b.bottom, 10, 10); SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
    RECT tr = b; if (down) OffsetRect(&tr, 0, 1);
    SetBkMode(dc, TRANSPARENT); SelectObject(dc, g_fBodyB); SetTextColor(dc, dis ? RGB(110, 120, 140) : RGB(255, 255, 255));
    DrawTextW(dc, t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static int HitAt(int x, int y) { for (auto& hh : g_hits) if (x >= hh.r.left && x < hh.r.right && y >= hh.r.top && y < hh.r.bottom) return hh.id; return -1; }

static void DarkTitleBar(HWND hw) {
    if (HMODULE d = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT(WINAPI * F)(HWND, DWORD, LPCVOID, DWORD);
        if (F f = (F)(void*)GetProcAddress(d, "DwmSetWindowAttribute")) { BOOL on = TRUE; f(hw, 20, &on, sizeof on); }
    }
}
