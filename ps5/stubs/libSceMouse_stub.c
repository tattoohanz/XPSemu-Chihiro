/*
 * XPSemu Chihiro: link stub for the console's mouse library (libSceMouse),
 * which the payload SDK has no stub for. ps5/build-title.sh builds it as
 * libSceMouse.sprx; ps5-native-tool takes the module and library name from
 * that soname and the NIDs from these names. Host-link only: never packaged
 * or run. The prototypes are the PS4 SDK's (shadPS4's
 * src/core/libraries/mouse/mouse.h); ui/xemu-input-ps5.c calls them.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdint.h>

int sceMouseInit(void)
{
    return -1;
}

int sceMouseOpen(int32_t user_id, int32_t type, int32_t index,
                 const void *param)
{
    (void)user_id;
    (void)type;
    (void)index;
    (void)param;
    return -1;
}

int sceMouseRead(int32_t handle, void *data, int32_t num)
{
    (void)handle;
    (void)data;
    (void)num;
    return -1;
}

int sceMouseClose(int32_t handle)
{
    (void)handle;
    return -1;
}
