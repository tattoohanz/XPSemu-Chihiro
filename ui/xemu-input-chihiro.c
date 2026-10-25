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
 *     Triangle (Y)     view change     Square (X) intrude change (Maximum
 *     Circle (B)       card in (Maximum Tune)                  Tune)
 *     Crazy Taxi: R1 drive, L1 reverse, Square jump, no view change.
 *   Ollie King
 *     left stick       board swing (left/right), front/rear (up/down)
 *     L1 (White)       left grab       R1 (Black) right grab
 *   Every game
 *     Options (Start) or Cross (A)  START      D-pad up  COIN
 *     D-pad right      TEST            D-pad left SERVICE
 *   (Create also gives a coin, but on the PS5 it opens the capture menu.)
 *
 * Maximum Tune's shifter is an H gate; the paddles move its lever one gear
 * at a time and leave it there, as the desktop fork does (wmmt_lever).
 *
 * Gun games (Ghost Squad, Virtua Cop 3, The House of the Dead III): a
 * crosshair over the picture, moved by a mouse or the right stick
 * (gun_inputs, below).
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
#include "hw/xbox/chihiro/chihiro-driveboard-sega838.h"
#include "hw/xbox/chihiro/chihiro-cabinet.h"
#include "hw/xbox/chihiro/chihiro-cardreader-hw210.h"
#include "qemu/timer.h"

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

/*
 * XPSemu Chihiro: the drive board's force feedback as DualSense rumble, as
 * the desktop fork gives it a plain gamepad (ui/xemu-input.c there,
 * chihiro_ffb_update). A pad has no wheel to turn, so it feels the knocks
 * and textures, not the centring and the weight:
 *   OutRun 2 (Sega 838): the effect packages the game plays (impacts,
 *     sand, grass) and its explicit vibration, driveboard_get_rumble();
 *     driveboard_get_ffb() is the packages' clock, once per UI frame.
 *   Maximum Tune (V257): the board's push; a collision's push begins as a
 *     knock, full force for 80 ms.
 * Both motors get the same level. XEMU_CHIHIRO_FFB in env.txt is the
 * strength in percent (100 = as the desktop fork, 0 = off, up to 200).
 */
#define V257_KNOCK_MS  80
#define V257_KNOCK_MIN 96

static int ffb_strength(void)
{
    static int str = -1;
    if (str < 0) {
        const char *e = getenv("XEMU_CHIHIRO_FFB");
        str = e && *e ? atoi(e) : 100;
        str = str < 0 ? 0 : str > 200 ? 200 : str;
    }
    return str;
}

/* 0-0xFFFF, before the strength */
static uint32_t ffb_level(void)
{
    switch (chihiro_cabinet_drive_board()) {
    case CHIHIRO_DRIVE_SEGA838: {
        if (!chihiro_driveboard_global) {
            return 0;
        }
        DriveBoardFFB ffb;
        driveboard_get_ffb(chihiro_driveboard_global, &ffb);
        return driveboard_get_rumble(chihiro_driveboard_global);
    }
    case CHIHIRO_DRIVE_V257: {
        static int prev, knock_push;
        static int64_t knock_until;
        int target, spring, damping, torque;
        if (!chihiro_v257_global) {
            return 0;
        }
        v257_drive(chihiro_v257_global, &target, &spring, &damping, &torque);
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (abs(torque) >= V257_KNOCK_MIN &&
            (!prev || (torque > 0) != (prev > 0))) {
            knock_push = torque > 0 ? 0x7F : -0x7F;
            knock_until = now + V257_KNOCK_MS;
        }
        prev = torque;
        if (now < knock_until) {
            torque = knock_push;
        }
        return (uint32_t)abs(torque) * 0xFFFF / 0x7F;
    }
    default:
        return 0;
    }
}

static void chihiro_ffb_rumble(void)
{
    int str = ffb_strength();
    uint32_t level = str ? ffb_level() * (uint32_t)str / 100 : 0;
    uint8_t motor = level >= 0xFFFF ? 255 : (uint8_t)(level >> 8);
#ifdef __PROSPERO__
    xemu_ps5_pad_vibrate(0, motor, motor);
#else
    (void)motor;
#endif
}

/*
 * XPSemu Chihiro: the gun games. The desktop fork aims with the system
 * cursor or a light gun (chihiro_gun_aim there); the PS5 has neither, so a
 * crosshair moves over the picture:
 *   a mouse (USB or Bluetooth): moves it. Left button the trigger, right
 *     the gun's body button (as the desktop's defaults), middle Change
 *     (Ghost Squad) or the pedal (Virtua Cop 3), a side button reload.
 *     Moved past the picture's edge, the gun is off the picture (a reload),
 *     as on the desktop.
 *   the DualSense: the right stick moves it (finer near the centre). R2 the
 *     trigger, L2 reload (the gun off the picture while held), R1 the body
 *     button, L1 Change or the pedal, Circle card in (Ghost Squad).
 * The aim goes into player 1's analog pair (0/1) as a fraction of the
 * picture, as the desktop sends it; while off the picture both are 0 and
 * the SCREEN switch (sw0 bit 0) is on. env.txt: XEMU_CHIHIRO_MOUSE_SPEED and
 * XEMU_CHIHIRO_AIM_SPEED (percent), XEMU_CHIHIRO_CROSSHAIR (0 = none).
 */
#define AIM_SAFE 0.01f          /* the desktop's: nearer an edge is off */
#define AIM_STICK_EDGE 0.02f    /* the stick keeps it on the picture */
#define AIM_MOUSE_PAST 0.03f    /* how far past the edge a mouse takes it */
#define AIM_MOUSE_ACROSS 1000.0f /* mouse counts across the picture (100%) */
#define AIM_STICK_ACROSS_S 0.75f /* seconds across it, stick full (100%) */
#define AIM_DEADZONE 0.12f

static float g_aim_x = 0.5f, g_aim_y = 0.5f;
static bool g_aim_off, g_gun_game;

static int env_percent(const char *name, int def, int max)
{
    const char *e = getenv(name);
    int v = e && *e ? atoi(e) : def;
    return v < 0 ? 0 : v > max ? max : v;
}

static bool gun_profile(int profile)
{
    return profile == CONFIG_CHIHIRO_JVS_PROFILE_HOTD3 ||
           profile == CONFIG_CHIHIRO_JVS_PROFILE_VC3 ||
           profile == CONFIG_CHIHIRO_JVS_PROFILE_GS;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* -1..1, the deadzone taken out and squared: fine aim near the centre */
static float aim_axis(int16_t raw)
{
    float v = raw / 32767.0f;
    float m = fabsf(v);
    if (m < AIM_DEADZONE) {
        return 0.0f;
    }
    m = clampf((m - AIM_DEADZONE) / (1.0f - AIM_DEADZONE), 0.0f, 1.0f);
    return copysignf(m * m, v);
}

static void gun_inputs(ChihiroJVSState *jvs, const ControllerState *pad,
                       int profile, uint8_t *sw0, uint8_t *sw1)
{
    static int stick_pct = -1, mouse_pct = -1;
    static int64_t last_ns;
    if (stick_pct < 0) {
        stick_pct = env_percent("XEMU_CHIHIRO_AIM_SPEED", 100, 400);
        mouse_pct = env_percent("XEMU_CHIHIRO_MOUSE_SPEED", 100, 400);
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    float dt = last_ns ? (now - last_ns) / 1e9f : 0.0f;
    last_ns = now;
    dt = clampf(dt, 0.0f, 0.05f);

    /* The picture is 4:3: the same speed on screen up and down is 4/3 of
     * the fraction. Screen down is positive; xemu's Y axes are up. */
    float sx = pad ? aim_axis(pad->axis[CONTROLLER_AXIS_RSTICK_X]) : 0.0f;
    float sy = pad ? -aim_axis(pad->axis[CONTROLLER_AXIS_RSTICK_Y]) : 0.0f;
    if (sx != 0.0f || sy != 0.0f) {
        float v = stick_pct / 100.0f / AIM_STICK_ACROSS_S * dt;
        g_aim_x = clampf(g_aim_x + sx * v, AIM_STICK_EDGE,
                         1.0f - AIM_STICK_EDGE);
        g_aim_y = clampf(g_aim_y + sy * v * 4.0f / 3.0f, AIM_STICK_EDGE,
                         1.0f - AIM_STICK_EDGE);
    }

    uint32_t mb = 0;
#ifdef __PROSPERO__
    int mdx, mdy;
    xemu_ps5_mouse_poll(&mdx, &mdy, &mb);
    if (mdx || mdy) {
        float k = mouse_pct / 100.0f / AIM_MOUSE_ACROSS;
        g_aim_x = clampf(g_aim_x + mdx * k, -AIM_MOUSE_PAST,
                         1.0f + AIM_MOUSE_PAST);
        g_aim_y = clampf(g_aim_y + mdy * k * 4.0f / 3.0f, -AIM_MOUSE_PAST,
                         1.0f + AIM_MOUSE_PAST);
    }
#else
    (void)mouse_pct;
#endif

    bool fire = trigger(pad, CONTROLLER_AXIS_RTRIG) > 0.5f || (mb & 0x01);
    bool body = pressed(pad, CONTROLLER_BUTTON_BLACK) || (mb & 0x02);
    bool change = pressed(pad, CONTROLLER_BUTTON_WHITE) || (mb & 0x04);
    bool reload = trigger(pad, CONTROLLER_AXIS_LTRIG) > 0.5f || (mb & 0x18);
    bool off = reload || g_aim_x < AIM_SAFE || g_aim_x > 1.0f - AIM_SAFE ||
               g_aim_y < AIM_SAFE || g_aim_y > 1.0f - AIM_SAFE;
    g_aim_off = off;
    if (off) {
        jvs->analog[0] = 0;
        jvs->analog[1] = 0;
        *sw0 |= 0x01;
    } else {
        jvs->analog[0] = (uint16_t)(g_aim_x * 0xFFFF);
        jvs->analog[1] = (uint16_t)(g_aim_y * 0xFFFF);
    }
    if (fire) {
        *sw0 |= 0x02;
    }
    if (body) {
        *sw1 |= 0x80;
    }
    if (change && profile != CONFIG_CHIHIRO_JVS_PROFILE_HOTD3) {
        *sw1 |= 0x40;
    }
    if (profile == CONFIG_CHIHIRO_JVS_PROFILE_GS) {
        /* Card In pushes the card in hand; the switch follows the slot. */
        static bool card_was;
        if (edge(pressed(pad, CONTROLLER_BUTTON_B), &card_was)) {
            chihiro_hw210_card_key(0);
        }
        if (chihiro_hw210_enabled && chihiro_card_reader_present(0)) {
            *sw1 |= 0x20;
        }
    }
}

bool xemu_chihiro_crosshair(float *x, float *y)
{
    static int on = -1;
    if (on < 0) {
        on = env_percent("XEMU_CHIHIRO_CROSSHAIR", 1, 1);
    }
    if (!on || !g_gun_game || g_aim_off || !xemu_chihiro_mode() ||
        !chihiro_jvs_global) {
        return false;
    }
    *x = chihiro_jvs_global->analog[0] / 65535.0f;
    *y = chihiro_jvs_global->analog[1] / 65535.0f;
    return true;
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
    g_gun_game = gun_profile(profile);

    switch (profile) {
    case CONFIG_CHIHIRO_JVS_PROFILE_HOTD3:
    case CONFIG_CHIHIRO_JVS_PROFILE_VC3:
    case CONFIG_CHIHIRO_JVS_PROFILE_GS:
        gun_inputs(jvs, pad, profile, &sw0, &sw1);
        break;
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
        /* Cockpit games (Gundam) have no controller layout here yet: the
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

    chihiro_ffb_rumble();
}

/*
 * XPSemu Chihiro: what the controls above do in each game, for the arcade
 * front's Controls page (ui/xui/dashboard.cc), in the Xbox layout the
 * switches read; the page shows the DualSense button each comes from
 * (Settings > Controller). A change to a game's layout above changes its
 * rows here. The names are the cabinets' (and the desktop fork's keys).
 */
#define SYSTEM_CONTROLS                                                     \
    { CONTROLLER_BUTTON_START, "Start", true },                             \
    { CONTROLLER_BUTTON_A, "Start", true },                                 \
    { CONTROLLER_BUTTON_DPAD_UP, "Coin", true },                            \
    { CONTROLLER_BUTTON_DPAD_RIGHT, "Test", true },                         \
    { CONTROLLER_BUTTON_DPAD_LEFT, "Service", true }

static const ChihiroControlInfo controls_wmmt2[] = {
    { CHIHIRO_CTL_LSTICK_X, "Steering", false },
    { CHIHIRO_CTL_RT, "Gas", false },
    { CHIHIRO_CTL_LT, "Brake", false },
    { CONTROLLER_BUTTON_BLACK, "Gear up", false },
    { CONTROLLER_BUTTON_WHITE, "Gear down", false },
    { CONTROLLER_BUTTON_Y, "View change", false },
    { CONTROLLER_BUTTON_X, "Intrude change", false },
    { CONTROLLER_BUTTON_B, "Card in", false },
    SYSTEM_CONTROLS,
};
static const ChihiroControlInfo controls_or2[] = {
    { CHIHIRO_CTL_LSTICK_X, "Steering", false },
    { CHIHIRO_CTL_RT, "Gas", false },
    { CHIHIRO_CTL_LT, "Brake", false },
    { CONTROLLER_BUTTON_BLACK, "Gear up", false },
    { CONTROLLER_BUTTON_WHITE, "Gear down", false },
    { CONTROLLER_BUTTON_Y, "View change", false },
    SYSTEM_CONTROLS,
};
static const ChihiroControlInfo controls_ctx[] = {
    { CHIHIRO_CTL_LSTICK_X, "Steering", false },
    { CHIHIRO_CTL_RT, "Gas", false },
    { CHIHIRO_CTL_LT, "Brake", false },
    { CONTROLLER_BUTTON_BLACK, "Drive", false },
    { CONTROLLER_BUTTON_WHITE, "Reverse", false },
    { CONTROLLER_BUTTON_X, "Jump", false },
    SYSTEM_CONTROLS,
};
static const ChihiroControlInfo controls_ok[] = {
    { CHIHIRO_CTL_LSTICK_X, "Board swing", false },
    { CHIHIRO_CTL_LSTICK_Y, "Board front/rear", false },
    { CONTROLLER_BUTTON_WHITE, "Left grab", false },
    { CONTROLLER_BUTTON_BLACK, "Right grab", false },
    SYSTEM_CONTROLS,
};
/* gun_inputs: R1 is the gun's body button, L1 the switch Ghost Squad calls
 * Change and Virtua Cop 3 is a pedal (none on The House of the Dead III). */
static const ChihiroControlInfo controls_hotd3[] = {
    { CHIHIRO_CTL_RSTICK, "Move cursor", false },
    { CHIHIRO_CTL_RT, "Trigger", false },
    { CHIHIRO_CTL_LT, "Screen in/out", false },
    { CONTROLLER_BUTTON_BLACK, "Gun button", false },
    SYSTEM_CONTROLS,
};
static const ChihiroControlInfo controls_vc3[] = {
    { CHIHIRO_CTL_RSTICK, "Move cursor", false },
    { CHIHIRO_CTL_RT, "Trigger", false },
    { CHIHIRO_CTL_LT, "Screen in/out", false },
    { CONTROLLER_BUTTON_BLACK, "Change", false },
    { CONTROLLER_BUTTON_WHITE, "Pedal", false },
    SYSTEM_CONTROLS,
};
static const ChihiroControlInfo controls_gs[] = {
    { CHIHIRO_CTL_RSTICK, "Move cursor", false },
    { CHIHIRO_CTL_RT, "Trigger", false },
    { CHIHIRO_CTL_LT, "Screen in/out", false },
    { CONTROLLER_BUTTON_BLACK, "Gun button", false },
    { CONTROLLER_BUTTON_WHITE, "Change", false },
    { CONTROLLER_BUTTON_B, "Card in", false },
    SYSTEM_CONTROLS,
};
/* Gundam and the games the cabinet table has no profile for */
static const ChihiroControlInfo controls_none[] = {
    SYSTEM_CONTROLS,
};

static const struct {
    int profile;
    const ChihiroControlInfo *rows;
    int count;
    const char *mouse; /* gun_inputs' mouse buttons */
} chihiro_layouts[] = {
#define LAYOUT(p, t, m) { CONFIG_CHIHIRO_JVS_PROFILE_##p, t, ARRAY_SIZE(t), m }
    LAYOUT(WMMT2, controls_wmmt2, NULL),
    LAYOUT(OR2, controls_or2, NULL),
    LAYOUT(CTX, controls_ctx, NULL),
    LAYOUT(OK, controls_ok, NULL),
    LAYOUT(HOTD3, controls_hotd3,
           "Mouse: move the cursor. Left button trigger, right gun button, "
           "side button screen in/out."),
    LAYOUT(VC3, controls_vc3,
           "Mouse: move the cursor. Left button trigger, right change, "
           "middle pedal, side button screen in/out."),
    LAYOUT(GS, controls_gs,
           "Mouse: move the cursor. Left button trigger, right gun button, "
           "middle change, side button screen in/out."),
#undef LAYOUT
};

int xemu_chihiro_controls(int profile, const ChihiroControlInfo **rows)
{
    for (size_t i = 0; i < ARRAY_SIZE(chihiro_layouts); i++) {
        if (chihiro_layouts[i].profile == profile) {
            *rows = chihiro_layouts[i].rows;
            return chihiro_layouts[i].count;
        }
    }
    *rows = controls_none;
    return ARRAY_SIZE(controls_none);
}

const char *xemu_chihiro_mouse_controls(int profile)
{
    for (size_t i = 0; i < ARRAY_SIZE(chihiro_layouts); i++) {
        if (chihiro_layouts[i].profile == profile) {
            return chihiro_layouts[i].mouse;
        }
    }
    return NULL;
}

/* The games chihiro.c's cabinet table knows by their XBE, by MAME set (the
 * arcade front's kChihiroTitles): a game not running hasn't named itself. */
static const struct {
    const char *set;
    int profile;
} chihiro_set_profiles[] = {
    { "hotd3", CONFIG_CHIHIRO_JVS_PROFILE_HOTD3 },
    { "vcop3", CONFIG_CHIHIRO_JVS_PROFILE_VC3 },
    { "ghostsqu", CONFIG_CHIHIRO_JVS_PROFILE_GS },
    { "crtaxihr", CONFIG_CHIHIRO_JVS_PROFILE_CTX },
    { "outr2", CONFIG_CHIHIRO_JVS_PROFILE_OR2 },
    { "outr2st", CONFIG_CHIHIRO_JVS_PROFILE_OR2 },
    { "ollie", CONFIG_CHIHIRO_JVS_PROFILE_OK },
    { "wangmid", CONFIG_CHIHIRO_JVS_PROFILE_WMMT2 },
    { "wangmid2", CONFIG_CHIHIRO_JVS_PROFILE_WMMT2 },
    { "gundamos", CONFIG_CHIHIRO_JVS_PROFILE_GUNDAM },
};

int xemu_chihiro_game_profile(const char *set, bool running)
{
    int profile = running ? chihiro_detected_game_profile() : -1;
    if (profile >= 0 || !set) {
        return profile;
    }
    for (size_t i = 0; i < ARRAY_SIZE(chihiro_set_profiles); i++) {
        if (!strcmp(set, chihiro_set_profiles[i].set)) {
            return chihiro_set_profiles[i].profile;
        }
    }
    return -1;
}
