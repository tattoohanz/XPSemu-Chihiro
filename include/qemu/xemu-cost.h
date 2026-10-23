/*
 * XPSemu: the timer and bottom-half callbacks timed for the game log (log
 * only). hw/xbox/nv2a/pgraph/profile.c sets the hook; until then nothing is
 * timed.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QEMU_XEMU_COST_H
#define QEMU_XEMU_COST_H

enum { XEMU_COST_TIMER, XEMU_COST_BH };
extern void (*xemu_cb_cost_hook)(int kind, const void *cb, uint64_t tsc);

static inline uint64_t xemu_cost_tsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

#endif
