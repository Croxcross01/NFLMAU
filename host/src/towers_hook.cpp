// Towers: the menu's MenuItemChanged native (registered next to SetGameModeType by the menu code). It is told the
// highlighted item (its first ActionScript argument, a string); towers.cpp keeps the last one, so when team select
// opens it knows whether the Towers button (the menu's PLAYNOW_TEAMSELECT item) or Play Now (TEAMSELECT) was pressed.
#include "kernel.h"
#include <cstdio>
#include <string>

void TowersMenuItem(const std::string& item);
void TowersDifficultySet(int value);

extern "C" PPC_FUNC(__imp__sub_8898E810);
extern "C" PPC_FUNC(__imp__sub_8905AB70);   // ActionScript argument n (r3) -> value (0 if none)
extern "C" PPC_FUNC(__imp__sub_89056990);   // value -> string object
extern "C" PPC_FUNC(__imp__sub_89051030);   // string object text (+8) -> char*
extern "C" PPC_FUNC(__imp__sub_890569A8);   // value -> integer
extern "C" PPC_FUNC(__imp__sub_8898BF20);   // GameSetupAdapterClass.SetDifficulty
PPC_FUNC(sub_8898E810) {
    PPCContext saved = ctx;
    std::string item;
    ctx.r3.u64 = 0; __imp__sub_8905AB70(ctx, base);
    uint32_t v = ctx.r3.u32;
    if (v >= 0x10000 && v < 0xE0000000u && ((ReadU32(v + 4) >> 27) & 1)) {
        ctx.r3.u64 = v; __imp__sub_89056990(ctx, base);
        ctx.r3.u64 = ctx.r3.u32 + 8; __imp__sub_89051030(ctx, base);
        uint32_t s = ctx.r3.u32;
        if (s >= 0x10000 && s < 0xE0000000u) for (int i = 0; i < 80; i++) { char c = (char)base[s + i]; if (!c) break; item += (c >= 32 && c < 127) ? c : '?'; }
    }
    ctx = saved;
    TowersMenuItem(item);
    __imp__sub_8898E810(ctx, base);
}

// GameSetupAdapterClass.SetDifficulty: the settings screen's Skill Level row, told the new level (0 Rookie .. 3 All-Madden)
// each time it changes. towers.cpp follows it to step the row to All-Madden in a Towers game.
PPC_FUNC(sub_8898BF20) {
    PPCContext saved = ctx;
    int value = -1;
    ctx.r3.u64 = 0; __imp__sub_8905AB70(ctx, base);
    uint32_t v = ctx.r3.u32;
    if (v >= 0x10000 && v < 0xE0000000u && ((ReadU32(v + 4) >> 27) & 1)) { ctx.r3.u64 = v; __imp__sub_890569A8(ctx, base); value = (int)ctx.r3.u32; }
    ctx = saved;
    __imp__sub_8898BF20(ctx, base);
    TowersDifficultySet(value);
}
