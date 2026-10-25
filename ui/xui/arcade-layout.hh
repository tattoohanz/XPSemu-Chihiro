//
// XPSemu Chihiro: the arcade front's look (after SEGA ALL.Net P-ras MULTI
// Ver.3's game select): a sky blue bar with the Chihiro logo (if there is
// one) and the clock, a pale banner naming the emulator, the games as
// flyer-shaped tiles in a row on white, the selected one in the middle with
// its name under it, and a sky blue bar at the foot with the test menu's
// buttons.
//
// Drawing only, from plain data (dashboard.cc fills it each frame), so the
// host tool ps5/tools/arcade-preview.cc can draw it too.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <string>
#include <vector>
#include "imgui.h"

struct ArcadeTileArt {
    ImTextureID tex = 0; // 0: none (the name on a white tile)
    float w = 0, h = 0;  // Its size (any shape, fitted into the tile)
    float fade = 1;      // 0..1 as it comes in
};

struct ArcadeGame {
    std::string title;  // Under the tile; on it too when it has no picture
    std::string info;   // Maker and year, or ""
    ArcadeTileArt art;
    bool running = false; // The game in the machine now
};

struct ArcadeFrame {
    std::vector<ArcadeGame> games;
    int selected = 0;
    float scroll = 0;   // The row's position, following selected (index units)
    float time = 0;     // Seconds, for what moves
    float alpha = 1;
    ImFont *font = nullptr; // The monospace (FontManager::m_arcade_font)
    std::string clock;      // "08:46", top right
    std::string top = "FREE PLAY";
    std::string banner = "PS5 Chihiro Emulator \xE7\xA8\xBC\xE5\x83\x8D\xE4\xB8\xAD"; // 稼働中
    std::string empty;      // Said in the middle when there are no games
    ArcadeTileArt logo;     // Top left, fitted into the bar; none: nothing
};

// The whole screen, size in pixels (laid out in 1080p units).
void ArcadeDraw(ImDrawList *dl, const ArcadeFrame &f, ImVec2 size);

// The middle tile's box in pixels (where a game's start animation begins).
void ArcadeMiddleTile(ImVec2 size, ImVec2 *p0, ImVec2 *p1);
