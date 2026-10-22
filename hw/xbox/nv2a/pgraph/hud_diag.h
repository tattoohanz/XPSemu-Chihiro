/*
 * nv2a HUD layout diagnostic (log only)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * XEMU_HUD_DIAG=1 enables it. One frame is captured every
 * XEMU_HUD_DIAG_EVERY flips (default 120), up to XEMU_HUD_DIAG_MAX frames
 * (default 300), into XEMU_HUD_DIAG_LOG (default: xemu-hud-diag.log next to
 * xemu.exe). Every draw of a captured frame is re-evaluated on the CPU from
 * the state the renderer is about to use, classified 2D/3D and measured.
 * The diagnostic never reaches the GPU or changes guest-visible state.
 *
 * With XEMU_WS_HACK=1 the same hook also runs the widescreen passes (see
 * hud_diag.c), both working on v0.x in the PGRAPH batch:
 *   copy-pass fix (XEMU_WS_COPY=0 disables): screen-space passes that copy
 *   a render target stay 1:1 under the hack's clip x scale;
 *   HUD anchoring (XEMU_WS_HUD=0 disables): 2D over a 3D scene, left/right
 *   third, moves out to the 16:9 edges at unchanged size;
 *   16:9 menu background (XEMU_WS_MENUBG=0 disables): after an untextured
 *   backdrop, full-width 2D layers are stretched to 16:9;
 *   4:3 clip (XEMU_WS_MENU43=0 disables): 2D screens after a textured
 *   backdrop are clipped to the 4:3 area through the window clip regions.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_HUD_DIAG_H
#define HW_XBOX_NV2A_PGRAPH_HUD_DIAG_H

struct NV2AState;
struct PGRAPHState;

extern bool pgraph_hud_diag_capturing;
extern bool pgraph_hud_ws_active;

void pgraph_hud_diag_draw_impl(struct NV2AState *d);
void pgraph_hud_ws_draw_impl(struct NV2AState *d);

/* From the pixel shader uniform setup: narrows the window clip regions
 * (host pixels, width_px x height_px surface) to the 4:3 area while the
 * current frame on the bound surface is a 2D screen. */
void pgraph_hud_ws_clip_regions(struct PGRAPHState *pg, int (*region)[4],
                                unsigned int width_px,
                                unsigned int height_px);

/* From NV097_CLEAR_SURFACE: on a 2D screen with the 4:3 clip, clears the
 * sides black and the 4:3 part in the game's colour. Returns true if it
 * issued the clear itself (the caller then skips its own). */
bool pgraph_hud_ws_clear(struct NV2AState *d, uint32_t parameter);
void pgraph_hud_diag_flip(struct NV2AState *d);

/* Call right before a draw is submitted (renderer draw_end / flush_draw),
 * while the batch buffers still hold it. The fix runs first, so a captured
 * frame logs the batch as it is submitted. */
static inline void pgraph_hud_diag_draw(struct NV2AState *d)
{
    if (pgraph_hud_ws_active) {
        pgraph_hud_ws_draw_impl(d);
    }
    if (pgraph_hud_diag_capturing) {
        pgraph_hud_diag_draw_impl(d);
    }
}

#endif
