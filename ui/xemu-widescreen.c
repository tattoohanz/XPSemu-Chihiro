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

#include "xemu-widescreen.h"

#include <stdio.h>
#include <stdlib.h>

static bool g_widescreen = false;

void xemu_set_widescreen(bool widescreen)
{
    g_widescreen = widescreen;
}

bool xemu_get_widescreen(void)
{
    return g_widescreen;
}

static bool g_ws_checked;
static float g_ws_scale = 1.0f;
static bool g_ws_2d_only;

static void xemu_ws_check(void)
{
    const char *e = getenv("XEMU_WS_HACK");

    g_ws_checked = true;
    if (e && (e[0] == '1' || e[0] == '2')) {
        g_ws_scale = (4.0f / 3.0f) / (16.0f / 9.0f);   /* 0.75 */
        g_ws_2d_only = e[0] == '2';
        fprintf(stderr, "xemu: widescreen hack: clip x * %.3f, %s\n",
                g_ws_scale, g_ws_2d_only ?
                    "the 2D only (programmable shaders; the 3D as the game "
                    "draws it)" :
                    "every draw");
    }
}

float xemu_get_ws_scale(void)
{
    if (!g_ws_checked) {
        xemu_ws_check();
    }
    return g_ws_scale;
}

bool xemu_get_ws_2d_only(void)
{
    if (!g_ws_checked) {
        xemu_ws_check();
    }
    return g_ws_2d_only;
}
