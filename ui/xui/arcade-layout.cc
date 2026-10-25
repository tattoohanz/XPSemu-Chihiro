//
// XPSemu Chihiro: the arcade front's look (see arcade-layout.hh).
//
// Measured off a P-ras MULTI Ver.3 screen and put in 1080p units: the top
// bar 0-105, the banner 105-193, the bottom bar from 925, the play button's
// centre at 1691, 931. Its tiles are square; here they are a flyer's shape
// (A4, 1 by 1.41), 500 high from 232, their centres 455 apart, the name at
// 785. Colours as on that screen: the bars
// #02B9FF, the banner #C6FCFC, the names #3C313F, the play button #FF6189,
// the bars' text white outlined in dark teal.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "arcade-layout.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct Ctx {
    ImDrawList *dl;
    ImFont *font;
    float s;      // Pixels per 1080p unit
    float w;      // Screen width in units
    float alpha;
};

ImU32 Col(const Ctx &c, int r, int g, int b, float a = 1)
{
    return IM_COL32(r, g, b, (int)(255 * std::clamp(a * c.alpha, 0.0f, 1.0f)));
}

ImVec2 P(const Ctx &c, float x, float y) { return ImVec2(x * c.s, y * c.s); }

ImVec2 Measure(const Ctx &c, float size, const char *text)
{
    ImVec2 m = c.font->CalcTextSizeA(size * c.s, FLT_MAX, 0, text);
    return ImVec2(m.x / c.s, m.y / c.s);
}

void Text(const Ctx &c, float size, float x, float y, ImU32 col,
          const char *text)
{
    c.dl->AddText(c.font, size * c.s, P(c, x, y), col, text);
}

// Text centred on x, its middle at y.
void TextCentred(const Ctx &c, float size, float x, float y, ImU32 col,
                 const char *text)
{
    ImVec2 m = Measure(c, size, text);
    Text(c, size, x - m.x / 2, y - m.y / 2, col, text);
}

// As the bars' text: white, outlined (drawn around it in eight steps).
void TextOutlined(const Ctx &c, float size, float x, float y, const char *text,
                  float a = 1)
{
    const float o = 2.2f;
    static const float dirs[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 },
                                      { -0.7f, -0.7f }, { 0.7f, -0.7f },
                                      { -0.7f, 0.7f }, { 0.7f, 0.7f } };
    for (const auto &d : dirs) {
        Text(c, size, x + d[0] * o, y + d[1] * o, Col(c, 16, 85, 101, a), text);
    }
    Text(c, size, x, y, Col(c, 255, 255, 255, a), text);
}

void TextOutlinedCentred(const Ctx &c, float size, float x, float y,
                         const char *text, float a = 1)
{
    ImVec2 m = Measure(c, size, text);
    TextOutlined(c, size, x - m.x / 2, y - m.y / 2, text, a);
}

// The name on a tile without a picture: wrapped, each line centred.
void TextWrappedCentred(const Ctx &c, float size, float cx, float cy,
                        float width, ImU32 col, const std::string &text)
{
    std::vector<std::string> lines;
    const char *p = text.c_str(), *end = p + text.size();
    float scale = size * c.s / c.font->FontSize;
    while (p < end) {
        const char *brk = c.font->CalcWordWrapPositionA(scale, p, end,
                                                         width * c.s);
        if (brk == p) {
            brk = p + 1; // A word longer than the line
        }
        std::string line(p, brk);
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        lines.push_back(line);
        p = brk;
        while (p < end && *p == ' ') {
            p++;
        }
    }
    float lh = size * 1.25f;
    float y = cy - lh * lines.size() / 2 + lh / 2;
    for (const auto &line : lines) {
        TextCentred(c, size, cx, y, col, line.c_str());
        y += lh;
    }
}

void Bars(const Ctx &c, const ArcadeFrame &f)
{
    ImDrawList *dl = c.dl;
    float w = c.w;
    // White behind everything
    dl->AddRectFilled(P(c, 0, 0), P(c, w, 1080), Col(c, 255, 255, 255));

    // The banner, fading into the white at its foot
    dl->AddRectFilled(P(c, 0, 105), P(c, w, 189), Col(c, 198, 252, 252));
    dl->AddRectFilledMultiColor(P(c, 0, 189), P(c, w, 199),
                                Col(c, 198, 252, 252), Col(c, 198, 252, 252),
                                Col(c, 255, 255, 255), Col(c, 255, 255, 255));
    TextCentred(c, 40, w / 2, 149, Col(c, 28, 39, 45), f.banner.c_str());

    // The top bar, a lighter line at its foot
    dl->AddRectFilled(P(c, 0, 0), P(c, w, 105), Col(c, 2, 185, 255));
    dl->AddRectFilled(P(c, 0, 103), P(c, w, 106), Col(c, 124, 230, 252));
    TextOutlinedCentred(c, 44, w / 2, 52, f.top.c_str());
    if (!f.clock.empty()) {
        ImVec2 m = Measure(c, 44, f.clock.c_str());
        TextOutlined(c, 44, w - 62 - m.x, 52 - m.y / 2, f.clock.c_str());
    }

    // Its corner badge: a green tile with "チ" (Chihiro) on white
    {
        ImVec2 p0 = P(c, 44, 20), p1 = P(c, 110, 86);
        dl->AddRectFilled(p0, p1, Col(c, 5, 165, 13), 12 * c.s);
        dl->AddRectFilled(P(c, 51, 27), P(c, 103, 79), Col(c, 255, 255, 255),
                          8 * c.s);
        TextCentred(c, 40, 77, 54, Col(c, 5, 165, 13), "\xE3\x83\x81");
    }

    // The bottom bar, a pale glow above it
    dl->AddRectFilledMultiColor(P(c, 0, 914), P(c, w, 925),
                                Col(c, 255, 255, 255), Col(c, 255, 255, 255),
                                Col(c, 201, 255, 255), Col(c, 201, 255, 255));
    dl->AddRectFilled(P(c, 0, 925), P(c, w, 1080), Col(c, 2, 185, 255));
}

// A D-pad with one arm lit (0 up, 1 right, 2 down, 3 left), as the
// buttons in P-ras's own help: grey, the one to press red.
void DPad(const Ctx &c, float cx, float cy, int lit)
{
    const float arm = 15, half = 9;
    ImU32 grey = Col(c, 213, 210, 219), red = Col(c, 230, 76, 60);
    ImU32 edge = Col(c, 16, 85, 101);
    // The cross, outlined
    auto cross = [&](float grow, ImU32 col) {
        c.dl->AddRectFilled(P(c, cx - half - grow, cy - half - arm - grow),
                            P(c, cx + half + grow, cy + half + arm + grow), col,
                            3 * c.s);
        c.dl->AddRectFilled(P(c, cx - half - arm - grow, cy - half - grow),
                            P(c, cx + half + arm + grow, cy + half + grow), col,
                            3 * c.s);
    };
    cross(2.5f, edge);
    cross(0, grey);
    float dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
    float ax = cx + dx[lit] * (arm + half) / 2 * 1.15f;
    float ay = cy + dy[lit] * (arm + half) / 2 * 1.15f;
    float hw = dx[lit] ? (arm + half) / 2 : half - 1;
    float hh = dy[lit] ? (arm + half) / 2 : half - 1;
    c.dl->AddRectFilled(P(c, ax - hw, ay - hh), P(c, ax + hw, ay + hh), red,
                        3 * c.s);
}

void Help(const Ctx &c)
{
    // The test menu's buttons: TEST (D-pad right) opens it and chooses,
    // SERVICE (D-pad left) moves through it; COIN (D-pad up) for a credit.
    struct Item { int lit; const char *label; };
    static const Item items[] = { { 1, "TEST" }, { 3, "SERVICE" }, { 0, "COIN" } };
    const float size = 36, gap = 70, icon = 60;
    float total = 0;
    for (const auto &it : items) {
        total += icon + Measure(c, size, it.label).x;
    }
    total += gap * 2;
    float x = 840 - total / 2; // Left of the play button
    for (const auto &it : items) {
        DPad(c, x + 24, 1002, it.lit);
        TextOutlined(c, size, x + icon, 1002 - Measure(c, size, it.label).y / 2,
                     it.label);
        x += icon + Measure(c, size, it.label).x + gap;
    }
}

// Small boxes at the bottom left, as P-ras's: this game's settings, the
// emulator's settings.
void Corner(const Ctx &c)
{
    struct Box { const char *glyph, *label; };
    static const Box boxes[] = { { "tri", "GAME" }, { "opt", "SETTINGS" } };
    float x = 16;
    for (const auto &b : boxes) {
        float lw = Measure(c, 22, b.label).x;
        float bw = 44 + lw + 14;
        ImVec2 p0 = P(c, x, 1024), p1 = P(c, x + bw, 1066);
        c.dl->AddRectFilled(p0, p1, Col(c, 26, 122, 196), 7 * c.s);
        c.dl->AddRect(p0, p1, Col(c, 255, 255, 255, 0.9f), 7 * c.s, 0,
                      2 * c.s);
        float gx = x + 23, gy = 1045;
        if (!strcmp(b.glyph, "tri")) {
            c.dl->AddTriangle(P(c, gx, gy - 10), P(c, gx + 11, gy + 9),
                              P(c, gx - 11, gy + 9), Col(c, 255, 255, 255),
                              2.5f * c.s);
        } else {
            for (int i = -1; i <= 1; i++) {
                c.dl->AddLine(P(c, gx - 10, gy + i * 7), P(c, gx + 10, gy + i * 7),
                              Col(c, 255, 255, 255), 2.5f * c.s);
            }
        }
        Text(c, 22, x + 44, 1045 - Measure(c, 22, b.label).y / 2,
             Col(c, 255, 255, 255), b.label);
        x += bw + 12;
    }
}

void PlayButton(const Ctx &c)
{
    const float cx = 1691, cy = 931, r = 66;
    c.dl->AddCircleFilled(P(c, cx + 3, cy + 5), r * c.s, Col(c, 0, 60, 90, 0.25f),
                          64);
    c.dl->AddCircleFilled(P(c, cx, cy), r * c.s, Col(c, 255, 97, 137), 64);
    c.dl->AddTriangleFilled(P(c, cx - 18, cy - 27), P(c, cx + 30, cy),
                            P(c, cx - 18, cy + 27), Col(c, 255, 255, 255));
    // The DualSense's cross that works it, on a white badge
    float bx = cx - 52, by = cy - 50;
    c.dl->AddCircleFilled(P(c, bx, by), 21 * c.s, Col(c, 255, 255, 255), 32);
    c.dl->AddCircle(P(c, bx, by), 21 * c.s, Col(c, 2, 185, 255), 32, 2.5f * c.s);
    c.dl->AddLine(P(c, bx - 9, by - 9), P(c, bx + 9, by + 9), Col(c, 2, 120, 200),
                  3.5f * c.s);
    c.dl->AddLine(P(c, bx + 9, by - 9), P(c, bx - 9, by + 9), Col(c, 2, 120, 200),
                  3.5f * c.s);
}

const float kTileH = 500, kTileW = kTileH / 1.41f, kTileTop = 232, kStep = 455;

void Tile(const Ctx &c, const ArcadeGame &g, float cx, float cy)
{
    ImDrawList *dl = c.dl;
    float hw = kTileW / 2, h = kTileH / 2;
    ImVec2 p0 = P(c, cx - hw, cy - h), p1 = P(c, cx + hw, cy + h);
    // A soft shadow to the right and below
    for (int i = 1; i <= 4; i++) {
        dl->AddRectFilled(P(c, cx - hw + i, cy - h + i), P(c, cx + hw + i, cy + h + i),
                          Col(c, 40, 40, 50, 0.07f));
    }
    dl->AddRectFilled(p0, p1, Col(c, 255, 255, 255));
    if (g.art.tex && g.art.w > 0 && g.art.h > 0) {
        // The picture fitted into the tile, white around it if its shape
        // is not a flyer's
        float k = std::min(kTileW / g.art.w, kTileH / g.art.h);
        float iw = g.art.w * k, ih = g.art.h * k;
        dl->AddImage(g.art.tex, P(c, cx - iw / 2, cy - ih / 2),
                     P(c, cx + iw / 2, cy + ih / 2), ImVec2(0, 0), ImVec2(1, 1),
                     Col(c, 255, 255, 255, g.art.fade));
    }
    if (!g.art.tex || g.art.fade < 1) {
        // Its name, as a logo on white
        float a = g.art.tex ? 1 - g.art.fade : 1;
        dl->AddRect(p0, p1, Col(c, 216, 216, 222, a), 0, 0, 2 * c.s);
        TextWrappedCentred(c, 34, cx, cy, kTileW - 50, Col(c, 60, 49, 63, a),
                           g.title);
    }
    if (g.running) {
        // Its badge: the game in the machine now
        const char *label = "PLAYING";
        ImVec2 m = Measure(c, 22, label);
        ImVec2 b0 = P(c, cx - hw + 10, cy - h + 10);
        ImVec2 b1 = P(c, cx - hw + 10 + m.x + 24, cy - h + 10 + m.y + 10);
        dl->AddRectFilled(b0, b1, Col(c, 255, 97, 137), 14 * c.s);
        Text(c, 22, cx - hw + 22, cy - h + 15, Col(c, 255, 255, 255), label);
    }
}

} // namespace

void ArcadeDraw(ImDrawList *dl, const ArcadeFrame &f, ImVec2 size)
{
    if (!f.font || size.y <= 0) {
        return;
    }
    Ctx c{ dl, f.font, size.y / 1080.0f, size.x / (size.y / 1080.0f), f.alpha };
    Bars(c, f);

    const float cy = kTileTop + kTileH / 2;
    int n = (int)f.games.size();
    if (!n) {
        TextWrappedCentred(c, 32, c.w / 2, cy, 1300, Col(c, 60, 49, 63),
                           f.empty);
    }
    // The row, clipped to the white between the bars
    dl->PushClipRect(P(c, 0, 199), P(c, c.w, 914), true);
    for (int i = 0; i < n; i++) {
        float x = c.w / 2 + (i - f.scroll) * kStep;
        if (x < -kTileW || x > c.w + kTileW) {
            continue;
        }
        Tile(c, f.games[i], x, cy);
    }
    if (n) {
        // The selected game's name, cross-fading as the row moves
        int sel = std::clamp(f.selected, 0, n - 1);
        float a = std::clamp(1 - fabsf(f.scroll - sel) * 2.5f, 0.0f, 1.0f);
        TextCentred(c, 34, c.w / 2, 785, Col(c, 60, 49, 63, a),
                    f.games[sel].title.c_str());
        if (!f.games[sel].info.empty()) {
            TextCentred(c, 24, c.w / 2, 830, Col(c, 150, 142, 160, a),
                        f.games[sel].info.c_str());
        }
    }
    dl->PopClipRect();

    Help(c);
    Corner(c);
    PlayButton(c);
}

void ArcadeMiddleTile(ImVec2 size, ImVec2 *p0, ImVec2 *p1)
{
    float s = size.y / 1080.0f;
    *p0 = ImVec2(size.x / 2 - kTileW / 2 * s, kTileTop * s);
    *p1 = ImVec2(size.x / 2 + kTileW / 2 * s, (kTileTop + kTileH) * s);
}
