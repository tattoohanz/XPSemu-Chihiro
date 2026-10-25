/*
 * xemu wide screen handler
 *
 * Copyright (c) 2023 Matt Borgerson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef XEMU_WIDESCREEN
#define XEMU_WIDESCREEN

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void xemu_set_widescreen(bool widescreen);
bool xemu_get_widescreen(void);

/* Horizontal clip-space scale applied to every vertex, for titles that
 * render for a fixed 4:3 output and never read the Xbox video flags - the
 * Chihiro arcade games in particular, which the EEPROM-driven widescreen
 * above does nothing for. Narrowing x by 0.75 means stretching the result
 * to a 16:9 display reveals more of the world instead of distorting it.
 * Returns 1.0 when disabled. Read once per process: the value is baked
 * into generated shader source, so changing it needs a restart. */
float xemu_get_ws_scale(void);

/* XEMU_WS_HACK=2: the same scale for the programmable vertex shaders only.
 * The Chihiro titles draw their 3D with the fixed-function pipeline and
 * their HUD, menus and screen copies with programmable shaders, so this
 * narrows the 2D (and keeps the HUD anchoring, copy-pass fix and 4:3
 * menus, hw/xbox/nv2a/pgraph/hud_diag.c) while a game patch makes the 3D
 * 16:9 itself (Maximum Tune 1/2: XEMU_WM1_/XEMU_WM2_WIDESCREEN=1). */
bool xemu_get_ws_2d_only(void);

/* XEMU_WS_HACK=3: no shader is scaled; only the HUD is narrowed, on the CPU,
 * by hw/xbox/nv2a/pgraph/hud_diag.c: the 2D drawn on a surface after the 3D
 * scene was copied onto it (Ollie King: its scene, 3D characters drawn with
 * programmable shaders among it, is copied to the screen and the HUD drawn
 * over it), while a game patch makes the 3D 16:9 itself
 * (XEMU_OLLIE_WIDESCREEN=1). XEMU_WS_HACK=4: the same, the HUD being the 2D
 * drawn after the game's last full-screen pass over the 3D on the screen
 * surface itself (OutRun 2 / SP: their bloom composite; with
 * XEMU_OUTRUN2_WIDESCREEN=1). */
bool xemu_get_ws_cpu_only(void);
/* 3 or 4 as above, else 0 */
int xemu_get_ws_cpu_mode(void);

#ifdef __cplusplus
}
#endif

#endif
