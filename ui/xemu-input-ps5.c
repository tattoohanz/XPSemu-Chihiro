/*
 * xemu Input: the PS5's controller (DualSense), read with libScePad
 *
 * SDL has no PS5 joystick driver in this build, so the DualSense is its own
 * controller type, mapped to an Xbox controller. The calls and the layout of
 * the pad's state are the ones PS5SX2 uses (ps5/coreorbis/main-boot.cpp).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "xemu-input.h"
#include "xemu-notifications.h"
#include "xemu-os-utils.h"
#include "xemu-settings.h"

int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int sceUserServiceGetLoginUserIdList(void *list);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);
int scePadClose(int32_t handle);

/* The start of ScePadData; the rest (motion, touch, ...) isn't used. */
typedef struct PadData {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; /* 0-255, 128 centred, down and right larger */
    uint8_t l2, r2;         /* 0-255 */
    uint8_t pad[2];
    uint8_t rest[256];
} PadData;

enum {
    PAD_L3 = 0x00000002,
    PAD_R3 = 0x00000004,
    PAD_OPTIONS = 0x00000008,
    PAD_UP = 0x00000010,
    PAD_RIGHT = 0x00000020,
    PAD_DOWN = 0x00000040,
    PAD_LEFT = 0x00000080,
    PAD_L1 = 0x00000400,
    PAD_R1 = 0x00000800,
    PAD_TRIANGLE = 0x00001000,
    PAD_CIRCLE = 0x00002000,
    PAD_CROSS = 0x00004000,
    PAD_SQUARE = 0x00008000,
    PAD_TOUCHPAD = 0x00100000,
    PAD_CREATE = 0x00000001, /* PS5SX2 maps it with the touchpad, to Select */
};

/*
 * XPSemu: up to four players. On the PS5 each controller belongs to a
 * signed-in user (or a guest), so player 1 is the user who started
 * XPSemu and players 2-4 are the other signed-in users, in the order the
 * PS5 lists them. Every two seconds the list is checked: a new user's
 * controller is opened and plugged into the Xbox's next free port (its own
 * first), and a user who signed out is unplugged, as on a real Xbox.
 */
#define PS5_PLAYERS 4
typedef struct Ps5Player {
    ControllerState *state;
    int32_t user, handle;
    uint32_t ui; /* Its buttons in the standard layout (the dashboard) */
} Ps5Player;
static Ps5Player g_players[PS5_PLAYERS] = {
    { NULL, -1, -1, 0 }, { NULL, -1, -1, 0 },
    { NULL, -1, -1, 0 }, { NULL, -1, -1, 0 },
};
static int64_t g_next_scan_ms;

typedef struct LoginList {
    int32_t user[4]; /* -1: none */
} LoginList;

static void player_open(int i)
{
    Ps5Player *p = &g_players[i];
    p->handle = scePadOpen(p->user, 0, 0, NULL);
    if (p->handle < 0) {
        p->handle = scePadGetHandle(p->user, 0, 0);
    }
    fprintf(stderr, "xemu PS5 pad: player %d: user %d, handle %d\n", i + 1,
            p->user, p->handle);
}

/* Players 2-4 on the Xbox: plugged into a free port, or unplugged */
static void player_plug(int i, bool on)
{
    ControllerState *state = g_players[i].state;
    char msg[64];
    if (!state || i == 0) {
        return; /* Player 1 stays on port 1 (xemu_input_init) */
    }
    if (on && state->bound < 0) {
        for (int k = 0; k < 4; k++) {
            int port = (i + k) % 4;
            if (!bound_controllers[port]) {
                xemu_input_bind(port, state, 0);
                xemu_input_rebind_xmu(port);
                snprintf(msg, sizeof(msg), "Player %d's controller is on port %d",
                         i + 1, port + 1);
                xemu_queue_notification(msg);
                fprintf(stderr, "xemu PS5 pad: %s\n", msg);
                return;
            }
        }
    } else if (!on && state->bound >= 0) {
        int port = state->bound;
        xemu_input_bind(port, NULL, 0);
        snprintf(msg, sizeof(msg), "Player %d's controller is off (port %d)",
                 i + 1, port + 1);
        xemu_queue_notification(msg);
        fprintf(stderr, "xemu PS5 pad: %s\n", msg);
    }
}

static void mouse_scan(void); /* XPSemu Chihiro: the gun games' mouse */

/* From the UI thread with the BQL (xemu_input_update_controllers): plugging
 * a port adds USB devices, so never from inside the Xbox's USB polling. */
void xemu_input_ps5_pads_scan(void)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (now < g_next_scan_ms) {
        return;
    }
    g_next_scan_ms = now + 2000;

    static bool initialized;
    if (!initialized) {
        int rc_user = sceUserServiceInitialize(NULL);
        int rc_pad = scePadInit();
        fprintf(stderr, "xemu PS5 pad: user service %#x, pad %#x\n", rc_user,
                rc_pad);
        initialized = true;
    }
    Ps5Player *one = &g_players[0];
    if (one->user < 0 && sceUserServiceGetInitialUser(&one->user) != 0) {
        one->user = -1;
    }
    if (one->user >= 0 && one->handle < 0) {
        player_open(0);
    }
    mouse_scan();

    LoginList list;
    memset(&list, 0xFF, sizeof(list));
    if (sceUserServiceGetLoginUserIdList(&list) != 0) {
        return;
    }
    /* Signed out: unplugged */
    for (int i = 1; i < PS5_PLAYERS; i++) {
        Ps5Player *p = &g_players[i];
        bool in = false;
        for (int k = 0; k < 4; k++) {
            in |= p->user >= 0 && list.user[k] == p->user;
        }
        if (p->user >= 0 && !in) {
            if (p->handle >= 0) {
                scePadClose(p->handle);
            }
            fprintf(stderr, "xemu PS5 pad: player %d (user %d) signed out\n",
                    i + 1, p->user);
            p->user = p->handle = -1;
            p->ui = 0;
            player_plug(i, false);
        }
    }
    /* Signed in: the next free player */
    for (int k = 0; k < 4; k++) {
        int32_t user = list.user[k];
        bool known = user == -1;
        for (int i = 0; i < PS5_PLAYERS && !known; i++) {
            known = g_players[i].user == user;
        }
        for (int i = 1; i < PS5_PLAYERS && !known; i++) {
            if (g_players[i].user < 0) {
                g_players[i].user = user;
                known = true;
            }
        }
    }
    for (int i = 1; i < PS5_PLAYERS; i++) {
        Ps5Player *p = &g_players[i];
        if (p->user >= 0 && p->handle < 0) {
            player_open(i);
        }
        if (p->handle >= 0) {
            player_plug(i, true);
        }
    }
}

ControllerState *xemu_input_ps5_pad_create(int player)
{
    static const char *const names[PS5_PLAYERS] = {
        "DualSense", "DualSense (player 2)", "DualSense (player 3)",
        "DualSense (player 4)",
    };
    ControllerState *state = g_new0(ControllerState, 1);
    state->type = INPUT_DEVICE_PS5_PAD;
    state->name = names[player & 3];
    state->bound = -1;
    state->peripheral_types[0] = PERIPHERAL_NONE;
    state->peripheral_types[1] = PERIPHERAL_NONE;
    g_players[player & 3].state = state;
    return state;
}

static int16_t stick(uint8_t v)
{
    int value = ((int)v - 128) * 256;
    return value > 32767 ? 32767 : value;
}

static int16_t trigger(uint8_t v)
{
    return (int16_t)((int)v * 32767 / 255);
}

/*
 * XPSemu: the Xbox controller's buttons and triggers can each come from any
 * of the DualSense's (Settings > Controller), kept in
 * /data/xemu-chihiro/controller.txt. The touchpad stays the Guide button (it opens
 * the dashboard), the sticks stay the sticks. The dashboard follows the
 * layout too (xemu_ps5_pad_ui_buttons gives the standard one).
 */
static const uint32_t kSourceBit[PS5_SRC__COUNT] = {
    [PS5_SRC_CROSS] = PAD_CROSS,     [PS5_SRC_CIRCLE] = PAD_CIRCLE,
    [PS5_SRC_SQUARE] = PAD_SQUARE,   [PS5_SRC_TRIANGLE] = PAD_TRIANGLE,
    [PS5_SRC_L1] = PAD_L1,           [PS5_SRC_R1] = PAD_R1,
    [PS5_SRC_L3] = PAD_L3,           [PS5_SRC_R3] = PAD_R3,
    [PS5_SRC_OPTIONS] = PAD_OPTIONS, [PS5_SRC_CREATE] = PAD_CREATE,
    [PS5_SRC_UP] = PAD_UP,           [PS5_SRC_DOWN] = PAD_DOWN,
    [PS5_SRC_LEFT] = PAD_LEFT,       [PS5_SRC_RIGHT] = PAD_RIGHT,
    /* L2 and R2 are read as triggers */
};
static const char *const kSourceName[PS5_SRC__COUNT] = {
    "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3",
    "R3", "Options", "Create", "D-pad up", "D-pad down", "D-pad left",
    "D-pad right",
};
static const char *const kSourceKey[PS5_SRC__COUNT] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3",
    "r3", "options", "create", "up", "down", "left", "right",
};
static const uint16_t kXboxBit[XB__COUNT] = {
    [XB_A] = CONTROLLER_BUTTON_A,           [XB_B] = CONTROLLER_BUTTON_B,
    [XB_X] = CONTROLLER_BUTTON_X,           [XB_Y] = CONTROLLER_BUTTON_Y,
    [XB_BLACK] = CONTROLLER_BUTTON_BLACK,   [XB_WHITE] = CONTROLLER_BUTTON_WHITE,
    [XB_BACK] = CONTROLLER_BUTTON_BACK,     [XB_START] = CONTROLLER_BUTTON_START,
    [XB_LSTICK] = CONTROLLER_BUTTON_LSTICK, [XB_RSTICK] = CONTROLLER_BUTTON_RSTICK,
    [XB_UP] = CONTROLLER_BUTTON_DPAD_UP,    [XB_DOWN] = CONTROLLER_BUTTON_DPAD_DOWN,
    [XB_LEFT] = CONTROLLER_BUTTON_DPAD_LEFT, [XB_RIGHT] = CONTROLLER_BUTTON_DPAD_RIGHT,
    /* The triggers are axes */
};
static const char *const kXboxKey[XB__COUNT] = {
    "a", "b", "x", "y", "black", "white", "back", "start", "lstick", "rstick",
    "up", "down", "left", "right", "lt", "rt",
};
static const int8_t kDefaultMap[XB__COUNT] = {
    [XB_A] = PS5_SRC_CROSS,       [XB_B] = PS5_SRC_CIRCLE,
    [XB_X] = PS5_SRC_SQUARE,      [XB_Y] = PS5_SRC_TRIANGLE,
    [XB_BLACK] = PS5_SRC_R1,      [XB_WHITE] = PS5_SRC_L1,
    [XB_BACK] = PS5_SRC_CREATE,   [XB_START] = PS5_SRC_OPTIONS,
    [XB_LSTICK] = PS5_SRC_L3,     [XB_RSTICK] = PS5_SRC_R3,
    [XB_UP] = PS5_SRC_UP,         [XB_DOWN] = PS5_SRC_DOWN,
    [XB_LEFT] = PS5_SRC_LEFT,     [XB_RIGHT] = PS5_SRC_RIGHT,
    [XB_LT] = PS5_SRC_L2,         [XB_RT] = PS5_SRC_R2,
};
static int8_t g_map[XB__COUNT];
static bool g_map_loaded;
static uint32_t g_raw_buttons;    /* The DualSense as last read */
static uint8_t g_raw_l2, g_raw_r2;
static uint32_t g_ui_buttons;     /* In the standard layout */
int g_xemu_ps5_pad_hold;          /* The dashboard is up: no rumble, triggers */

static char *map_file(void)
{
    return g_strdup_printf("%scontroller.txt", xemu_settings_get_base_path());
}

static void map_load(void)
{
    g_map_loaded = true;
    memcpy(g_map, kDefaultMap, sizeof(g_map));
    g_autofree char *path = map_file();
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char xbox[32], src[32];
    while (fscanf(f, " %31[^=]=%31s", xbox, src) == 2) {
        for (int x = 0; x < XB__COUNT; x++) {
            for (int s = 0; s < PS5_SRC__COUNT; s++) {
                if (!strcmp(xbox, kXboxKey[x]) && !strcmp(src, kSourceKey[s])) {
                    g_map[x] = s;
                }
            }
        }
    }
    fclose(f);
    fprintf(stderr, "xemu PS5 pad: layout from %s:", path);
    for (int x = 0; x < XB__COUNT; x++) {
        fprintf(stderr, " %s=%s", kXboxKey[x], kSourceKey[g_map[x]]);
    }
    fprintf(stderr, "\n");
}

static void map_save(void)
{
    g_autofree char *path = map_file();
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    for (int x = 0; x < XB__COUNT; x++) {
        fprintf(f, "%s=%s\n", kXboxKey[x], kSourceKey[g_map[x]]);
    }
    fclose(f);
}

int xemu_ps5_map_get(int xbox)
{
    if (!g_map_loaded) {
        map_load();
    }
    return xbox >= 0 && xbox < XB__COUNT ? g_map[xbox] : -1;
}

/* Gives xbox the source; whatever had it gets xbox's old one (a swap). */
void xemu_ps5_map_set(int xbox, int source)
{
    if (!g_map_loaded) {
        map_load();
    }
    if (xbox < 0 || xbox >= XB__COUNT || source < 0 || source >= PS5_SRC__COUNT) {
        return;
    }
    for (int x = 0; x < XB__COUNT; x++) {
        if (g_map[x] == source) {
            g_map[x] = g_map[xbox];
        }
    }
    g_map[xbox] = source;
    map_save();
    fprintf(stderr, "xemu PS5 pad: Xbox %s now from %s\n", kXboxKey[xbox],
            kSourceName[source]);
}

void xemu_ps5_map_reset(void)
{
    memcpy(g_map, kDefaultMap, sizeof(g_map));
    g_map_loaded = true;
    map_save();
}

const char *xemu_ps5_source_name(int source)
{
    return source >= 0 && source < PS5_SRC__COUNT ? kSourceName[source] : "-";
}

/* How far a source is pressed, 0..1. */
static float source_level(int source, uint32_t buttons, uint8_t l2, uint8_t r2)
{
    if (source == PS5_SRC_L2) {
        return l2 / 255.0f;
    }
    if (source == PS5_SRC_R2) {
        return r2 / 255.0f;
    }
    return source >= 0 && source < PS5_SRC__COUNT &&
           (buttons & kSourceBit[source]) ? 1.0f : 0.0f;
}

/* The first source pressed now (for "press a button"), or -1. */
int xemu_ps5_pad_pressed_source(void)
{
    for (int s = 0; s < PS5_SRC__COUNT; s++) {
        if (source_level(s, g_raw_buttons, g_raw_l2, g_raw_r2) > 0.6f) {
            return s;
        }
    }
    return -1;
}

uint32_t xemu_ps5_pad_ui_buttons(void)
{
    return g_ui_buttons;
}

/* No rumble on the DualSense (a plain controller, by choice). */
void xemu_input_update_ps5_pad_rumble(ControllerState *state)
{
    (void)state;
}

/*
 * XPSemu Chihiro: the DualSense's two motors for a Chihiro cabinet's drive
 * board (xemu-input-chihiro.c). The pad is put in compatible vibration mode
 * (2) once per handle, as PS5_RetroArch does; a level is sent when it
 * changes, and again every second while it is on. While the dashboard is
 * up the motors stop.
 */
typedef struct PadVibration {
    uint8_t large_motor, small_motor;
} PadVibration;
int scePadSetVibration(int32_t handle, const PadVibration *param);
int scePadSetVibrationMode(int32_t handle, int32_t mode);

static struct {
    int32_t mode_handle;      /* the handle its mode was set on, -1 none */
    PadVibration last;
    int64_t last_ms;
    int sends, failures, last_rc;
    uint8_t peak;
} g_vib = { -1, { 0, 0 }, 0, 0, 0, 0, 0 };

void xemu_ps5_pad_vibrate(int player, uint8_t large, uint8_t small)
{
    if (player < 0 || player >= PS5_PLAYERS || g_players[player].handle < 0 ||
        !xemu_ps5_import_ok("scePadSetVibration") ||
        !xemu_ps5_import_ok("scePadSetVibrationMode")) {
        return;
    }
    int32_t handle = g_players[player].handle;
    if (g_xemu_ps5_pad_hold) {
        large = small = 0;
    }
    if (g_vib.mode_handle != handle) {
        int rc = scePadSetVibrationMode(handle, 2);
        g_vib.mode_handle = handle;
        g_vib.last.large_motor = g_vib.last.small_motor = 0;
        g_vib.last_ms = 0;
        fprintf(stderr, "xemu PS5 pad: vibration mode 2 on handle %d: %#x\n",
                handle, rc);
    }
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    bool changed = large != g_vib.last.large_motor ||
                   small != g_vib.last.small_motor;
    bool refresh = (large || small) && now - g_vib.last_ms >= 1000;
    if (!changed && !refresh) {
        return;
    }
    PadVibration v = { large, small };
    int rc = scePadSetVibration(handle, &v);
    g_vib.last = v;
    g_vib.last_ms = now;
    g_vib.sends++;
    g_vib.peak = MAX(g_vib.peak, MAX(large, small));
    if (rc != 0) {
        g_vib.failures++;
        g_vib.last_rc = rc;
    }
}

/* The game log's line: sends since the last call, the strongest level, the
 * failures and the last error; "" when nothing was sent. */
void xemu_ps5_pad_vibration_format(char *buf, size_t size)
{
    buf[0] = '\0';
    if (!g_vib.sends) {
        return;
    }
    int n = snprintf(buf, size, "%d sends, strongest %d%%", g_vib.sends,
                     g_vib.peak * 100 / 255);
    if (g_vib.failures && n > 0 && (size_t)n < size) {
        snprintf(buf + n, size - n, ", %d failed (last %#x)", g_vib.failures,
                 g_vib.last_rc);
    }
    g_vib.sends = g_vib.failures = 0;
    g_vib.peak = 0;
}

/*
 * XPSemu Chihiro: a USB or Bluetooth mouse (libSceMouse) for the gun games'
 * aim (xemu-input-chihiro.c). Linked as an import, the console leaves the
 * library unbound for this title (its three imports stayed 0, 02c77dc), so
 * the first gun game loads it at run time (sceKernelLoadStartModule) and
 * looks its functions up by name, then by NID (sceKernelDlsym); each step is
 * logged. Opened for player 1's user in merged mode (every mouse as one);
 * each read returns the samples since the last one, each with its own
 * movement and the buttons as they were. The prototypes, the layout of a
 * sample and the NIDs are the PS4 SDK's, as shadPS4 has them
 * (src/core/libraries/mouse/mouse.h, mouse.cpp).
 */
typedef struct MouseOpenParam {
    uint8_t behavior; /* 1: every mouse merged into one */
    uint8_t reserved[7];
} MouseOpenParam;
typedef struct MouseData {
    uint64_t timestamp;
    bool connected;
    uint32_t buttons; /* left 1, right 2, middle 4, side 8 and 16 */
    int32_t x, y;     /* movement since the last sample, down positive */
    int32_t wheel, tilt;
    uint8_t reserved[8];
} MouseData;
QEMU_BUILD_BUG_ON(sizeof(MouseData) != 40);
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp,
                             uint32_t flags, const void *option, int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);

static int (*mouse_init)(void);
static int (*mouse_open)(int32_t user_id, int32_t type, int32_t index,
                         const MouseOpenParam *param);
static int (*mouse_read)(int32_t handle, MouseData *data, int32_t num);

static struct {
    bool wanted, tried, loaded, inited, connected;
    int module, load_rc, sym_rc;
    int init_rc, open_rc, logged_rc;
    int32_t handle;
    uint32_t buttons;
    /* For the game log, since its last line */
    int samples, failures, last_rc;
    int64_t moved;
    uint32_t pressed;
} g_mouse = { .module = -1, .handle = -1, .logged_rc = 1 };

static void *mouse_sym(const char *name, const char *nid)
{
    void *by_name = NULL, *by_nid = NULL;
    int rc_name = sceKernelDlsym(g_mouse.module, name, &by_name);
    int rc_nid = by_name ? 0 : sceKernelDlsym(g_mouse.module, nid, &by_nid);
    void *p = by_name ? by_name : by_nid;
    fprintf(stderr, "xemu PS5 mouse: %s by name %#x%s, by NID %s %#x%s\n",
            name, rc_name, by_name ? " found" : "", nid, rc_nid,
            by_nid ? " found" : "");
    if (!p) {
        g_mouse.sym_rc = rc_nid ? rc_nid : rc_name;
    }
    return p;
}

/* Once: the module, then its three functions. */
static bool mouse_load(void)
{
    if (g_mouse.tried) {
        return g_mouse.loaded;
    }
    g_mouse.tried = true;
    if (!xemu_ps5_import_ok("sceKernelLoadStartModule") ||
        !xemu_ps5_import_ok("sceKernelDlsym")) {
        fprintf(stderr, "xemu PS5 mouse: no module loader\n");
        return false;
    }
    static const char *const paths[] = {
        "libSceMouse.sprx",
        "/system/common/lib/libSceMouse.sprx",
    };
    for (size_t i = 0; i < ARRAY_SIZE(paths) && g_mouse.module < 0; i++) {
        int status = 0;
        int h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &status);
        fprintf(stderr, "xemu PS5 mouse: load %s: %#x, status %#x\n",
                paths[i], h, status);
        g_mouse.load_rc = h;
        if (h >= 0) {
            g_mouse.module = h;
        }
    }
    if (g_mouse.module < 0) {
        return false;
    }
    mouse_init = (int (*)(void))mouse_sym("sceMouseInit", "Qs0wWulgl7U");
    mouse_open = (int (*)(int32_t, int32_t, int32_t, const MouseOpenParam *))
        mouse_sym("sceMouseOpen", "RaqxZIf6DvE");
    mouse_read = (int (*)(int32_t, MouseData *, int32_t))
        mouse_sym("sceMouseRead", "x8qnXqh-tiM");
    g_mouse.loaded = mouse_init && mouse_open && mouse_read;
    return g_mouse.loaded;
}

/* From xemu_input_ps5_pads_scan (every 2 s, the UI thread). */
static void mouse_scan(void)
{
    if (!g_mouse.wanted || g_mouse.handle >= 0 || g_players[0].user < 0 ||
        !mouse_load()) {
        return;
    }
    if (!g_mouse.inited) {
        g_mouse.init_rc = mouse_init();
        g_mouse.inited = true;
        fprintf(stderr, "xemu PS5 mouse: init %#x\n", g_mouse.init_rc);
    }
    MouseOpenParam merged = { .behavior = 1 };
    int rc = mouse_open(g_players[0].user, 0, 0, &merged);
    if (rc < 0) {
        rc = mouse_open(g_players[0].user, 0, 0, NULL);
    }
    g_mouse.open_rc = rc;
    if (rc >= 0) {
        g_mouse.handle = rc;
        fprintf(stderr, "xemu PS5 mouse: open for user %d: handle %d\n",
                g_players[0].user, rc);
    } else if (rc != g_mouse.logged_rc) {
        g_mouse.logged_rc = rc;
        fprintf(stderr, "xemu PS5 mouse: open for user %d: %#x\n",
                g_players[0].user, rc);
    }
}

bool xemu_ps5_mouse_poll(int *dx, int *dy, uint32_t *buttons)
{
    *dx = *dy = 0;
    *buttons = 0;
    g_mouse.wanted = true;
    if (g_mouse.handle < 0) {
        return false;
    }
    MouseData d[32];
    int n = mouse_read(g_mouse.handle, d, ARRAY_SIZE(d));
    if (n < 0) {
        g_mouse.failures++;
        g_mouse.last_rc = n;
        return false;
    }
    for (int i = 0; i < n; i++) {
        *dx += d[i].x;
        *dy += d[i].y;
        g_mouse.buttons = d[i].buttons;
        g_mouse.pressed |= d[i].buttons;
        if (d[i].connected != g_mouse.connected) {
            g_mouse.connected = d[i].connected;
            fprintf(stderr, "xemu PS5 mouse: %s\n",
                    g_mouse.connected ? "connected" : "disconnected");
        }
    }
    g_mouse.samples += n;
    g_mouse.moved += abs(*dx) + abs(*dy);
    /* The samples count whatever their connected flag says (merged mode may
     * leave it unset); with no mouse they don't move or press anything. */
    if (g_xemu_ps5_pad_hold) {
        /* The dashboard is up: the game gets nothing */
        *dx = *dy = 0;
        return true;
    }
    *buttons = g_mouse.buttons;
    return true;
}

/* The game log's line, once a gun game has asked for the mouse. */
void xemu_ps5_mouse_format(char *buf, size_t size)
{
    buf[0] = '\0';
    if (!g_mouse.wanted) {
        return;
    }
    if (!g_mouse.tried) {
        snprintf(buf, size, "not tried yet");
    } else if (g_mouse.module < 0) {
        snprintf(buf, size, "libSceMouse not loaded (%#x)", g_mouse.load_rc);
    } else if (!g_mouse.loaded) {
        snprintf(buf, size, "libSceMouse loaded (module %d), its functions "
                 "not found (%#x)", g_mouse.module, g_mouse.sym_rc);
    } else if (g_mouse.handle < 0) {
        snprintf(buf, size, "not open (init %#x, open %#x)", g_mouse.init_rc,
                 g_mouse.open_rc);
    } else {
        int n = snprintf(buf, size,
                         "handle %d, %s, %d samples, moved %" PRId64
                         " counts, buttons seen %#x",
                         g_mouse.handle,
                         g_mouse.connected ? "connected" : "not connected",
                         g_mouse.samples, g_mouse.moved, g_mouse.pressed);
        if (g_mouse.failures && n > 0 && (size_t)n < size) {
            snprintf(buf + n, size - n, ", %d reads failed (last %#x)",
                     g_mouse.failures, g_mouse.last_rc);
        }
    }
    g_mouse.samples = g_mouse.failures = 0;
    g_mouse.moved = 0;
    g_mouse.pressed = 0;
}

void xemu_input_update_ps5_pad_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    int player = 0;
    while (player < PS5_PLAYERS && g_players[player].state != state) {
        player++;
    }
    PadData d;
    if (player == PS5_PLAYERS || g_players[player].handle < 0 ||
        scePadReadState(g_players[player].handle, &d) != 0) {
        return;
    }
    if (!g_map_loaded) {
        map_load();
    }
    if (player == 0) { /* The button tester follows player 1 */
        g_raw_buttons = d.buttons;
        g_raw_l2 = d.l2;
        g_raw_r2 = d.r2;
    }

    /* The games: the buttons as mapped. */
    static uint32_t told; /* Remapped controls already logged */
    for (int x = 0; x < XB_LT; x++) {
        if (source_level(g_map[x], d.buttons, d.l2, d.r2) > 0.5f) {
            state->buttons |= kXboxBit[x];
            if (g_map[x] != kDefaultMap[x] && !(told & (1u << x))) {
                told |= 1u << x;
                fprintf(stderr, "xemu PS5 pad: %s pressed -> Xbox %s\n",
                        kSourceName[g_map[x]], kXboxKey[x]);
            }
        }
    }
    /* The PS button belongs to the system: the touchpad opens xemu's menu */
    if (d.buttons & PAD_TOUCHPAD) {
        state->buttons |= CONTROLLER_BUTTON_GUIDE;
    }
    state->axis[CONTROLLER_AXIS_LTRIG] =
        (int16_t)(source_level(g_map[XB_LT], d.buttons, d.l2, d.r2) * 32767);
    state->axis[CONTROLLER_AXIS_RTRIG] =
        (int16_t)(source_level(g_map[XB_RT], d.buttons, d.l2, d.r2) * 32767);
    /* xemu's Y axes are positive up (xemu_input_update_sdl_controller_state) */
    state->axis[CONTROLLER_AXIS_LSTICK_X] = stick(d.lx);
    state->axis[CONTROLLER_AXIS_LSTICK_Y] = -1 - stick(d.ly);
    state->axis[CONTROLLER_AXIS_RSTICK_X] = stick(d.rx);
    state->axis[CONTROLLER_AXIS_RSTICK_Y] = -1 - stick(d.ry);

    /* The dashboard: the standard layout. */
    uint32_t ui = 0;
    for (int x = 0; x < XB_LT; x++) {
        if (source_level(kDefaultMap[x], d.buttons, d.l2, d.r2) > 0.5f) {
            ui |= kXboxBit[x];
        }
    }
    if (d.buttons & PAD_TOUCHPAD) {
        ui |= CONTROLLER_BUTTON_GUIDE;
    }
    g_players[player].ui = ui;
    g_ui_buttons = g_players[0].ui; /* Only player 1 works the dashboard */
    (void)trigger;
}

/* Which player a DualSense is (0-3), or -1 if it isn't one */
int xemu_ps5_pad_player(const ControllerState *state)
{
    for (int i = 0; i < PS5_PLAYERS; i++) {
        if (g_players[i].state == state) {
            return i;
        }
    }
    return -1;
}

/* XPSemu: the dashboard's controller icon (top right): the pad is open. */
int xemu_ps5_pad_connected(void);
int xemu_ps5_pad_connected(void)
{
    return g_players[0].handle >= 0;
}
