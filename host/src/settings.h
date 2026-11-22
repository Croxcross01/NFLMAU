// User settings shared with the launcher (settings.ini next to the executables).
#pragma once
#include <string>
#include <cstdint>

struct Settings {
    std::string renderer = "vulkan";   // vulkan | d3d12 | none (headless)
    std::string quality = "medium";    // low | medium | high | custom
    uint32_t resolutionScale = 2;      // internal resolution: 1 = 1280x720, 2 = 2560x1440 (2K), 3 = 3840x2160 (4K)
    uint32_t windowWidth = 1600, windowHeight = 900;
    bool fullscreen = false;
    bool vsync = true;
    uint32_t anisotropy = 8;           // texture filtering: 1, 2, 4, 8, 16
    bool showFps = true;
    bool depthOfField = false;   // replay/cutscene depth-of-field blur
    bool mipmaps = true;
    bool audio = true;           // sound output
    int volume = 100;            // 0-100         // load the game's texture mip chains (less shimmer at a distance)
    std::string gpu;                   // preferred adapter name (empty = automatic)
};
extern Settings g_settings;
// Load settings.ini (missing keys keep their defaults); quality presets fill in scale/anisotropy unless "custom".
void LoadSettings(const std::string& path);
void ApplyQualityPreset(Settings& s);
