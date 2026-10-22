/*
 * xemu Input: a controller onto a Chihiro cabinet's JVS I/O board
 *
 * XPSemu Chihiro. The desktop fork binds keyboard keys, mouse buttons, pad
 * axes and raw wheels to each JVS input (its Settings > Input > Chihiro
 * pages). On the PS5 there is one DualSense and no such pages, so the
 * cabinet's controls come from player 1's controller in its Xbox layout
 * (XPSemu's Settings > Controller remaps that layout), fixed per game:
 *
 *   Driving games (Maximum Tune 1/2, OutRun 2, Crazy Taxi)
 *     left stick X     steering        R2 (RT)    gas      L2 (LT)  brake
 *     R1 (Black)       gear up         L1 (White) gear down
 *     Triangle (Y)     view change     Square (X) Maximum Tune's 2nd button
 *     Circle (B)       card in (Maximum Tune)
 *   Every game
 *     Options (Start) or Cross (A)  START      D-pad up  COIN
 *     D-pad right      TEST            D-pad left SERVICE
 *   (Create also gives a coin, but on the PS5 it opens the capture menu.)
 *
 * Maximum Tune's shifter is an H gate; the paddles move its lever one gear
 * at a time and leave it there, as the desktop fork does (wmmt_lever).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "xemu-input.h"
#include "xemu-settings.h"
#include "hw/xbox/chihiro/chihiro.h"
#include "hw/xbox/chihiro/chihiro-jvs.h"
#include "hw/xbox/chihiro/chihiro-driveboard-v257.h"

/* Stick travel below this is the stick resting (DualSense drift). */
#define STEER_DEADZONE 0.08f
#define TRIGGER_DEADZONE 0.04f

static bool pressed(const ControllerState *pad, uint16_t button)
{
    return pad && (pad->buttons & button);
}

/* True on the frame a button goes down, not while it is held. */
static bool edge(bool down, bool *was)
{
    bool now = down && !*was;
    *was = down;
    return now;
}

static float stick_x(const ControllerState *pad)
{
    if (!pad) {
        return 0.0f;
    }
    float v = pad->axis[CONTROLLER_AXIS_LSTICK_X] / 32767.0f;
    float m = fabsf(v);
    if (m < STEER_DEADZONE) {
        return 0.0f;
    }
    /* The deadzone taken out, so the wheel still starts from the centre. */
    v = copysignf((m - STEER_DEADZONE) / (1.0f - STEER_DEADZONE), v);
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

static float trigger(const ControllerState *pad, int axis)
{
    if (!pad) {
        return 0.0f;
    }
    float v = pad->axis[axis] / 32767.0f;
    if (v < TRIGGER_DEADZONE) {
        return 0.0f;
    }
    return v > 1.0f ? 1.0f : v;
}

/* Maximum Tune's H shifter (V322.xbe 0x00055AA0, V307.xbe 0x0006F150):
 * switches up 0x80, down 0x40, left 0x20, right 0x10, each gear where the
 * game reads it, neutral none. */
static const uint8_t wmmt_gate[7] = { 0x00, 0xA0, 0x60, 0x80, 0x40, 0x90, 0x50 };

static uint8_t wmmt_lever(const ControllerState *pad)
{
    static int gear;
    static bool up_was, down_was;

    if (edge(pressed(pad, CONTROLLER_BUTTON_BLACK), &up_was) && gear < 6) {
        gear++;
    }
    if (edge(pressed(pad, CONTROLLER_BUTTON_WHITE), &down_was) && gear > 0) {
        gear--;
    }
    return wmmt_gate[gear];
}

void xemu_input_update_chihiro_jvs(void)
{
    if (!xemu_chihiro_mode() || !chihiro_jvs_global) {
        return;
    }
    chihiro_card_ui_sync();
    ChihiroJVSState *jvs = chihiro_jvs_global;
    const ControllerState *pad = bound_controllers[0];
    uint8_t sw0 = 0, sw1 = 0;

    int profile = chihiro_detected_game_profile();
    if (profile < 0) {
        profile = g_config.chihiro.jvs.profile;
    }

    switch (profile) {
    case CONFIG_CHIHIRO_JVS_PROFILE_CTX:
    case CONFIG_CHIHIRO_JVS_PROFILE_OR2:
    case CONFIG_CHIHIRO_JVS_PROFILE_WMMT2: {
        float steer = stick_x(pad);
        uint16_t steer_val = (uint16_t)(0x8000 + (int)(steer * 32767.0f));
        jvs->analog[0] = steer_val;
        /* The wheel's own encoder, for a cabinet with a drive board that
         * reads it (Maximum Tune's V257), in the units the board uses. */
        if (chihiro_v257_global) {
            v257_set_wheel(chihiro_v257_global, steer_val >> 6);
        }
        jvs->analog[1] = (uint16_t)(trigger(pad, CONTROLLER_AXIS_RTRIG) *
                                    65535.0f);
        jvs->analog[2] = (uint16_t)(trigger(pad, CONTROLLER_AXIS_LTRIG) *
                                    65535.0f);

        if (profile == CONFIG_CHIHIRO_JVS_PROFILE_WMMT2) {
            static bool card_was;
            /* Player 1's second switch byte: the shifter, and the two wheel
             * buttons on bits 1 and 0 (V322.xbe 0x00055850, 0x00055880). */
            sw1 |= wmmt_lever(pad);
            if (pressed(pad, CONTROLLER_BUTTON_Y)) {
                sw1 |= 0x02;
            }
            if (pressed(pad, CONTROLLER_BUTTON_X)) {
                sw1 |= 0x01;
            }
            /* The card slot is the reader's own mouth, not a switch. */
            if (edge(pressed(pad, CONTROLLER_BUTTON_B), &card_was)) {
                chihiro_crp1231_card_key();
            }
        } else if (profile == CONFIG_CHIHIRO_JVS_PROFILE_CTX) {
            if (pressed(pad, CONTROLLER_BUTTON_BLACK)) {
                sw0 |= 0x20; /* drive gear */
            }
            if (pressed(pad, CONTROLLER_BUTTON_WHITE)) {
                sw0 |= 0x10; /* reverse */
            }
            if (pressed(pad, CONTROLLER_BUTTON_X)) {
                sw0 |= 0x02; /* jump */
            }
        } else {
            /* OutRun 2: view change is player 1's sw0 bit 4; its shifter is
             * on player 2's up/down pins (below). */
            if (pressed(pad, CONTROLLER_BUTTON_Y)) {
                sw0 |= 0x10;
            }
        }
        break;
    }
    case CONFIG_CHIHIRO_JVS_PROFILE_OK: {
        /* Ollie King: the board swings with the left stick (left reads
         * above the centre), leans front/back with it too, grabs on the
         * shoulder buttons. */
        float swing = -stick_x(pad);
        jvs->analog[1] = (uint16_t)(0x8000 + (int)(swing * 32767.0f));
        int16_t y = pad ? pad->axis[CONTROLLER_AXIS_LSTICK_Y] : 0;
        if (y > 16000) {
            sw0 |= 0x20; /* board front */
        } else if (y < -16000) {
            sw0 |= 0x10; /* board rear */
        }
        if (pressed(pad, CONTROLLER_BUTTON_WHITE)) {
            sw0 |= 0x02;
        }
        if (pressed(pad, CONTROLLER_BUTTON_BLACK)) {
            sw0 |= 0x01;
        }
        break;
    }
    default:
        /* Gun and cockpit games have no controller layout here yet: the
         * system buttons below still work their menus. */
        break;
    }

    if (pressed(pad, CONTROLLER_BUTTON_START) ||
        pressed(pad, CONTROLLER_BUTTON_A)) {
        sw0 |= 0x80;
    }
    if (pressed(pad, CONTROLLER_BUTTON_DPAD_LEFT)) {
        sw0 |= 0x40;
    }

    jvs->player_switches[0][0] = sw0;
    jvs->player_switches[0][1] = sw1;
    jvs->player_switches[1][0] = 0;
    jvs->player_switches[1][1] = 0;

    /* OutRun 2's sequential shifter is wired to the SECOND player's up/down
     * pins (sw0 bits 5/4 of player 2), as the desktop fork found. */
    if (profile == CONFIG_CHIHIRO_JVS_PROFILE_OR2) {
        if (pressed(pad, CONTROLLER_BUTTON_BLACK)) {
            jvs->player_switches[1][0] |= 0x20;
        }
        if (pressed(pad, CONTROLLER_BUTTON_WHITE)) {
            jvs->player_switches[1][0] |= 0x10;
        }
    }

    jvs->system_switches =
        pressed(pad, CONTROLLER_BUTTON_DPAD_RIGHT) ? 0x80 : 0x00;

    static bool coin_was;
    if (edge(pressed(pad, CONTROLLER_BUTTON_DPAD_UP) ||
                 pressed(pad, CONTROLLER_BUTTON_BACK),
             &coin_was)) {
        jvs->coin_count[0]++;
    }
}
