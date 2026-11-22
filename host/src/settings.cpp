#include "settings.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

Settings g_settings;

static std::string Trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}
static bool ToBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

void ApplyQualityPreset(Settings& s) {
    if (s.quality == "low")         { s.resolutionScale = 1; s.anisotropy = 1; }
    else if (s.quality == "medium") { s.resolutionScale = 2; s.anisotropy = 8; }
    else if (s.quality == "high")   { s.resolutionScale = 3; s.anisotropy = 16; }
}

void LoadSettings(const std::string& path) {
    Settings s;
    bool scaleSet = false, anisoSet = false;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string l = Trim(line);
            if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[') continue;
            size_t eq = l.find('='); if (eq == std::string::npos) continue;
            std::string k = Trim(l.substr(0, eq)), v = Trim(l.substr(eq + 1));
            for (auto& c : k) c = (char)tolower((unsigned char)c);
            if (k == "renderer") s.renderer = v;
            else if (k == "quality") s.quality = v;
            else if (k == "resolution_scale") { s.resolutionScale = (uint32_t)atoi(v.c_str()); scaleSet = true; }
            else if (k == "window_width") s.windowWidth = (uint32_t)atoi(v.c_str());
            else if (k == "window_height") s.windowHeight = (uint32_t)atoi(v.c_str());
            else if (k == "fullscreen") s.fullscreen = ToBool(v);
            else if (k == "vsync") s.vsync = ToBool(v);
            else if (k == "anisotropy") { s.anisotropy = (uint32_t)atoi(v.c_str()); anisoSet = true; }
            else if (k == "show_fps") s.showFps = ToBool(v);
            else if (k == "depth_of_field") s.depthOfField = ToBool(v);
            else if (k == "mipmaps") s.mipmaps = ToBool(v);
            else if (k == "audio") s.audio = ToBool(v);
            else if (k == "volume") s.volume = atoi(v.c_str());
            else if (k == "gpu") s.gpu = v;
        }
        fclose(f);
    }
    for (auto& c : s.renderer) c = (char)tolower((unsigned char)c);
    for (auto& c : s.quality) c = (char)tolower((unsigned char)c);
    if (s.quality != "custom") {
        Settings p = s; ApplyQualityPreset(p);
        if (!scaleSet) s.resolutionScale = p.resolutionScale;
        if (!anisoSet) s.anisotropy = p.anisotropy;
    }
    if (s.resolutionScale < 1) s.resolutionScale = 1;
    if (s.resolutionScale > 3) s.resolutionScale = 3;
    if (s.windowWidth < 640) s.windowWidth = 640;
    if (s.windowHeight < 360) s.windowHeight = 360;
    g_settings = s;
    fprintf(stderr, "[settings] renderer=%s quality=%s internal=%ux%u (%ux) window=%ux%u%s vsync=%d aniso=%ux\n",
            s.renderer.c_str(), s.quality.c_str(), 1280 * s.resolutionScale, 720 * s.resolutionScale, s.resolutionScale,
            s.windowWidth, s.windowHeight, s.fullscreen ? " fullscreen" : "", s.vsync, s.anisotropy);
}
