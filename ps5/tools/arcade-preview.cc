//
// arcade-preview: draws XPSemu Chihiro's arcade front (ui/xui/arcade-layout.cc)
// on the host into a PPM, with a small software rasteriser standing in for
// the console's Vulkan backend, so the layout can be checked without a
// console.
//
//   arcade-preview <out.ppm> <width> <height> <selected> [scroll]
//                  [title|picture.png]...
//
// Each game is a name, then for each picture "=picture" (its tile's) or
// "=logo:picture" (its logo, under the row when it is selected);
// "logo=picture" is the logo at the top left. "page=Title" draws, instead
// of the front, a mock of a Settings page over its ground in the colours
// the dashboard's pages take there (DrawSettings' categories).
//
// Build (from the repository):
//   g++ -std=c++17 -O2 -Isubprojects/imgui -Iui/xui -Iui/thirdparty/stb_image
//     ps5/tools/arcade-preview.cc ui/xui/arcade-layout.cc \
//     subprojects/imgui/imgui*.cpp -o arcade-preview
//
#include "arcade-layout.hh"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct HostTexture {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};
static std::vector<HostTexture> s_textures(1); // 0 is none

static ImTextureID Upload(const uint8_t *rgba, int w, int h)
{
    HostTexture t;
    t.rgba.assign(rgba, rgba + (size_t)w * h * 4);
    t.w = w;
    t.h = h;
    s_textures.push_back(std::move(t));
    return (ImTextureID)(s_textures.size() - 1);
}

static void Sample(const HostTexture &t, float u, float v, float out[4])
{
    const float x = u * t.w - 0.5f, y = v * t.h - 0.5f;
    const int x0 = (int)floorf(x), y0 = (int)floorf(y);
    const float fx = x - x0, fy = y - y0;
    float acc[4] = { 0, 0, 0, 0 };
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int px = std::clamp(x0 + i, 0, t.w - 1), py = std::clamp(y0 + j, 0, t.h - 1);
            float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const uint8_t *p = &t.rgba[((size_t)py * t.w + px) * 4];
            for (int k = 0; k < 4; k++) {
                acc[k] += wgt * p[k] / 255.0f;
            }
        }
    }
    memcpy(out, acc, sizeof(acc));
}

static void Rasterise(ImDrawData *dd, std::vector<float> &fb, int W, int H)
{
    for (int l = 0; l < dd->CmdListsCount; l++) {
        const ImDrawList *list = dd->CmdLists[l];
        for (const ImDrawCmd &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                continue;
            }
            int cx0 = std::max(0, (int)cmd.ClipRect.x), cy0 = std::max(0, (int)cmd.ClipRect.y);
            int cx1 = std::min(W, (int)ceilf(cmd.ClipRect.z)), cy1 = std::min(H, (int)ceilf(cmd.ClipRect.w));
            size_t id = (size_t)cmd.GetTexID();
            const HostTexture *tex = id && id < s_textures.size() ? &s_textures[id] : nullptr;
            for (unsigned e = 0; e < cmd.ElemCount; e += 3) {
                const ImDrawVert *v[3];
                for (int k = 0; k < 3; k++) {
                    v[k] = &list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + e + k]];
                }
                float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) -
                             (v[2]->pos.x - v[0]->pos.x) * (v[1]->pos.y - v[0]->pos.y);
                if (fabsf(area) < 1e-6f) {
                    continue;
                }
                int bx0 = std::max(cx0, (int)floorf(std::min({ v[0]->pos.x, v[1]->pos.x, v[2]->pos.x })));
                int by0 = std::max(cy0, (int)floorf(std::min({ v[0]->pos.y, v[1]->pos.y, v[2]->pos.y })));
                int bx1 = std::min(cx1, (int)ceilf(std::max({ v[0]->pos.x, v[1]->pos.x, v[2]->pos.x })));
                int by1 = std::min(cy1, (int)ceilf(std::max({ v[0]->pos.y, v[1]->pos.y, v[2]->pos.y })));
                for (int y = by0; y < by1; y++) {
                    for (int x = bx0; x < bx1; x++) {
                        float px = x + 0.5f, py = y + 0.5f;
                        float w0 = ((v[1]->pos.x - px) * (v[2]->pos.y - py) - (v[2]->pos.x - px) * (v[1]->pos.y - py)) / area;
                        float w1 = ((v[2]->pos.x - px) * (v[0]->pos.y - py) - (v[0]->pos.x - px) * (v[2]->pos.y - py)) / area;
                        float w2 = 1 - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) {
                            continue;
                        }
                        float col[4];
                        for (int k = 0; k < 4; k++) {
                            int sh = k * 8;
                            col[k] = (w0 * ((v[0]->col >> sh) & 0xFF) + w1 * ((v[1]->col >> sh) & 0xFF) +
                                      w2 * ((v[2]->col >> sh) & 0xFF)) / 255.0f;
                        }
                        if (tex) {
                            float t[4];
                            Sample(*tex, w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x,
                                   w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y, t);
                            for (int k = 0; k < 4; k++) {
                                col[k] *= t[k];
                            }
                        }
                        float *d = &fb[((size_t)y * W + x) * 3];
                        for (int k = 0; k < 3; k++) {
                            d[k] = d[k] * (1 - col[3]) + col[k] * col[3];
                        }
                    }
                }
            }
        }
    }
}

static ImU32 Rgb(ArcadeRgb c, float a = 1)
{
    return IM_COL32(c.r, c.g, c.b, (int)(255 * a));
}

// A mock of a Settings page over the front's ground: the dashboard's Bar
// (corners cut), its rows and focus ring, and the hints in the bottom bar,
// in the colours the pages take there (dashboard.cc, g_arcade_look).
static void DrawPageMock(ImDrawList *dl, const ArcadeFrame &f, ImFont *menu,
                         ImVec2 size, const char *title)
{
    float s = size.y / 1080.0f;
    ArcadeDrawGround(dl, f, size);
    ArcadeDrawBanner(dl, f, size, title);
    auto bar = [&](ImVec2 p0, ImVec2 p1, bool sel) {
        float cut = (p1.y - p0.y) * 0.3f;
        auto path = [&] {
            dl->PathLineTo(ImVec2(p0.x + cut, p0.y));
            dl->PathLineTo(ImVec2(p1.x, p0.y));
            dl->PathLineTo(ImVec2(p1.x, p1.y - cut));
            dl->PathLineTo(ImVec2(p1.x - cut, p1.y));
            dl->PathLineTo(ImVec2(p0.x, p1.y));
            dl->PathLineTo(ImVec2(p0.x, p0.y + cut));
        };
        path();
        dl->PathFillConvex(sel ? Rgb(kArcadeSky, 0.95f) : Rgb(kArcadePale));
        path();
        dl->PathStroke(sel ? Rgb(kArcadeSky) : Rgb(kArcadeEdge, 0.9f),
                       ImDrawFlags_Closed, 2 * s);
    };
    static const char *const cats[][2] = {
        { "Video", "4x   16:9   Fit   Smooth" }, { "Sound", "100%   On   On" },
        { "Interface", "Off   On   On   Off" },   { "Controller", "Your button layout" },
        { "Patches", "All your games' patches" },
        { "Advanced", "Memory, video output, audio, performance" },
    };
    float x = 280 * s, w = 1400 * s, h = 86 * s, gap = 102 * s, y0 = 236 * s;
    int sel = 1;
    for (int i = 0; i < 6; i++) {
        float y = y0 + i * gap;
        bool on = i == sel;
        bar(ImVec2(x, y), ImVec2(x + w, y + h), on);
        dl->AddCircle(ImVec2(x + 66 * s, y + h / 2), 20 * s,
                      on ? IM_COL32_WHITE : Rgb(kArcadeDeep), 24, 3.2f * s);
        dl->AddText(menu, 46 * s, ImVec2(x + 118 * s, y + 17 * s),
                    on ? IM_COL32_WHITE : Rgb(kArcadeName), cats[i][0]);
        ImVec2 m = menu->CalcTextSizeA(30 * s, FLT_MAX, 0, cats[i][1]);
        dl->AddText(menu, 30 * s, ImVec2(x + w - m.x - 60 * s, y + 27 * s),
                    on ? Rgb({ 255, 255, 255 }, 0.8f) : Rgb(kArcadeSoft, 0.75f),
                    cats[i][1]);
    }
    float fy = y0 + sel * gap, pad = 6 * s;
    for (int i = 3; i >= 1; i--) {
        float g = pad + i * 4 * s;
        dl->AddRect(ImVec2(x - g, fy - g), ImVec2(x + w + g, fy + h + g),
                    Rgb(kArcadeDeep, 0.07f), 14 * s, 0, 4 * s);
    }
    dl->AddRect(ImVec2(x - pad, fy - pad), ImVec2(x + w + pad, fy + h + pad),
                Rgb(kArcadeDeep), 12 * s, 0, 3.5f * s);
    dl->AddText(menu, 32 * s, ImVec2(x + 10 * s, y0 + 6 * gap + 10 * s),
                Rgb(kArcadeName), "How loud the games are, the menu sounds and "
                                  "the music.");
    // The hints, in the bottom bar
    float hx = 90 * s, hy = 1000 * s;
    for (const char *hint : { "Open", "Back" }) {
        dl->AddCircleFilled(ImVec2(hx + 22 * s, hy), 24 * s, Rgb(kArcadeTeal, 0.9f), 32);
        dl->AddCircle(ImVec2(hx + 22 * s, hy), 22 * s, IM_COL32(255, 255, 255, 153),
                      32, 2.2f * s);
        ImVec2 at(hx + 56 * s, hy - 18 * s);
        for (float dx : { -2.f, 2.f, 0.f })
            for (float dy : { -2.f, 2.f, 0.f })
                if (dx || dy)
                    dl->AddText(menu, 32 * s, ImVec2(at.x + dx * s, at.y + dy * s),
                                Rgb(kArcadeTeal), hint);
        dl->AddText(menu, 32 * s, at, IM_COL32_WHITE, hint);
        hx += 100 * s + menu->CalcTextSizeA(32 * s, FLT_MAX, 0, hint).x;
    }
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <out.ppm> <width> <height> <selected> [scroll] [game[=picture]]...\n", argv[0]);
        return 2;
    }
    const int W = atoi(argv[2]), H = atoi(argv[3]);
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)W, (float)H);
    io.DeltaTime = 1 / 60.0f;
    float scale = H / 1080.0f;
    ImFontConfig config;
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x30C1, 0x30C1, 0x30FB, 0x30FB,
                                      0x4E2D, 0x4E2D, 0x50CD, 0x50CD, 0x7A3C, 0x7A3C, 0 };
    ImFont *font = io.Fonts->AddFontFromFileTTF("data/MPLUS1Code-Medium.ttf", 46.0f * scale,
                                                &config, ranges);
    if (!font) {
        fprintf(stderr, "no data/MPLUS1Code-Medium.ttf (run from the repository)\n");
        return 1;
    }
    unsigned char *atlas;
    int aw, ah;

    ImFont *menu = io.Fonts->AddFontFromFileTTF("data/RobotoCondensed-Regular.ttf",
                                                46.0f * scale);
    ArcadeFrame f;
    f.font = font;
    f.selected = atoi(argv[4]);
    f.scroll = argc > 5 ? (float)atof(argv[5]) : (float)f.selected;
    f.clock = "08:46";
    io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    io.Fonts->SetTexID(Upload(atlas, aw, ah));
    std::string page;
    for (int i = 6; i < argc; i++) {
        ArcadeGame g;
        std::string arg = argv[i];
        size_t eq = arg.find('=');
        g.title = arg.substr(0, eq);
        if (g.title == "page") {
            page = arg.substr(eq + 1);
            continue;
        }
        // Its pictures, each after an "=": the tile's, or "logo:" its logo
        bool top_logo = g.title == "logo";
        while (eq != std::string::npos) {
            size_t next = arg.find('=', eq + 1);
            std::string file = arg.substr(eq + 1, next == std::string::npos ?
                                                      std::string::npos :
                                                      next - eq - 1);
            eq = next;
            bool is_logo = file.rfind("logo:", 0) == 0;
            if (is_logo) {
                file = file.substr(5);
            }
            int w, h, n;
            unsigned char *px = stbi_load(file.c_str(), &w, &h, &n, 4);
            ArcadeTileArt &art = top_logo ? f.logo : is_logo ? g.logo : g.art;
            if (px) {
                art.tex = Upload(px, w, h);
                art.w = (float)w;
                art.h = (float)h;
                stbi_image_free(px);
            } else {
                fprintf(stderr, "can't read %s\n", file.c_str());
            }
        }
        if (top_logo) { // logo=<picture>: the top left logo
            continue;
        }
        if (g.title == "Wangan Midnight Maximum Tune 2") {
            g.info = "NAMCO  2005";
            g.running = true;
        }
        f.games.push_back(g);
    }
    if (f.games.empty()) {
        f.empty = "Put your Chihiro netboot images (.bin) in /data/xemu-chihiro/games";
    }

    ImGui::NewFrame();
    if (page.empty()) {
        ArcadeDraw(ImGui::GetBackgroundDrawList(), f, io.DisplaySize);
    } else {
        DrawPageMock(ImGui::GetBackgroundDrawList(), f, menu, io.DisplaySize,
                     page.c_str());
    }
    ImGui::Render();

    std::vector<float> fb((size_t)W * H * 3, 0.0f);
    Rasterise(ImGui::GetDrawData(), fb, W, H);
    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", W, H);
    for (float v : fb) {
        fputc((int)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f), out);
    }
    fclose(out);
    printf("%s: %dx%d, %d games\n", argv[1], W, H, (int)f.games.size());
    return 0;
}
