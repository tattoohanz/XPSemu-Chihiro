/*
 * QEMU Geforce NV2A profiling helpers
 *
 * Copyright (c) 2020-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/xemu-timing.h"

NV2AStats g_nv2a_stats;

/* XPSemu's stopwatches (hw/xbox/xemu-timing.h). */
uint64_t xemu_timing_ns[XT__COUNT];
uint64_t xemu_timing_max_ns[XT__COUNT];
uint32_t xemu_inval_page[XEMU_INVAL_PAGES];
uint32_t xemu_inval_hits[XEMU_INVAL_PAGES];
uint32_t xemu_timing_count[XT__COUNT];

void nv2a_profile_increment(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    const int64_t fps_update_interval = 250000;
    g_nv2a_stats.last_flip_time = now;

    static int64_t frame_count = 0;
    frame_count++;

    static int64_t ts = 0;
    int64_t delta = now - ts;
    if (delta >= fps_update_interval) {
        g_nv2a_stats.increment_fps = frame_count * 1000000 / delta;
        ts = now;
        frame_count = 0;
    }
}

/* Displayed FPS: distinct guest frames that reached the screen, counted at
 * the UI present. Rolling span of the last 60 distinct presents, rounded
 * to nearest: a stretched present interval or a single dropped frame does
 * not flip the reading, a sustained drop reads exactly. A fixed window with
 * truncating division reads 56-59 at a true 60. */
void nv2a_profile_present(void)
{
    if (nv2a_stock_active()) {
        stock_nv2a_profile_present();
        return;
    }
    static unsigned int last_id = (unsigned int)-1;
    static int64_t ring[128];
    static unsigned ring_n, ring_w;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    unsigned int id = g_nv2a_stats.presented_frame_id;
    if (id != last_id) {
        last_id = id;
        ring[ring_w] = now;
        ring_w = (ring_w + 1) % ARRAY_SIZE(ring);
        if (ring_n < ARRAY_SIZE(ring)) {
            ring_n++;
        }
    }
    if (ring_n >= 2) {
        unsigned frames = MIN(ring_n - 1, 60u);
        int64_t newest = ring[(ring_w + ARRAY_SIZE(ring) - 1) % ARRAY_SIZE(ring)];
        int64_t oldest =
            ring[(ring_w + ARRAY_SIZE(ring) - 1 - frames) % ARRAY_SIZE(ring)];
        /* A stall (no distinct frame for a while) lowers the reading as it
         * lasts instead of freezing the last good value. */
        int64_t span = MAX(newest - oldest, now - oldest - 100000);
        if (span > 0) {
            g_nv2a_stats.display_fps =
                (unsigned int)((frames * 1000000LL + span / 2) / span);
        }
    }
}

/* XPSemu Chihiro: where each frame's time went (the stopwatches of
 * hw/xbox/xemu-timing.h since the last flip, in microseconds), for the game
 * log's hitch lines. A stopwatch is added when its step ends, so a step that
 * spans a flip counts in the frame it ends in. */
static void profile_frame_times(int *c)
{
    static const struct {
        int xt, prof;
    } map[] = {
        { XT_FINISH_WAIT, NV2A_PROF_WAIT_US },
        { XT_AUX_WAIT, NV2A_PROF_AUX_US },
        { XT_DOWNLOAD, NV2A_PROF_DOWN_US },
        { XT_FINISH_SUBMIT, NV2A_PROF_SUBMIT_US },
        { XT_GPU_IDLE, NV2A_PROF_GPU_IDLE_US },
        { XT_CPU_EXEC, NV2A_PROF_CPU_RUN_US },
        { XT_CPU_IDLE, NV2A_PROF_CPU_IDLE_US },
        { XT_TB_GEN, NV2A_PROF_TB_GEN_US },
        { XT_UI_FRAME, NV2A_PROF_UI_FRAME_US },
        { XT_SUBMIT_LOCK, NV2A_PROF_SUBMIT_LOCK_US },
        { XT_UI_QUEUE, NV2A_PROF_UI_QUEUE_US },
        { XT_PCACHE, NV2A_PROF_PCACHE_US },
    };
    static uint64_t last[ARRAY_SIZE(map)];
    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        uint64_t ns = __atomic_load_n(&xemu_timing_ns[map[i].xt],
                                      __ATOMIC_RELAXED);
        uint64_t d = ns - last[i]; /* The totals only grow */
        last[i] = ns;
        c[map[i].prof] = (int)MIN(d / 1000, (uint64_t)INT32_MAX);
    }
}

void nv2a_profile_flip_stall(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t render_time = (now-g_nv2a_stats.last_flip_time)/1000;

    profile_frame_times(g_nv2a_stats.frame_working.counters);
    g_nv2a_stats.frame_working.mspf = render_time;
    g_nv2a_stats.frame_history[g_nv2a_stats.frame_ptr] =
        g_nv2a_stats.frame_working;
    g_nv2a_stats.frame_ptr =
        (g_nv2a_stats.frame_ptr + 1) % NV2A_PROF_NUM_FRAMES;
    g_nv2a_stats.frame_count++;
    memset(&g_nv2a_stats.frame_working, 0, sizeof(g_nv2a_stats.frame_working));
}

const char *nv2a_profile_get_counter_name(unsigned int cnt)
{
    if (nv2a_stock_active()) {
        return stock_nv2a_profile_get_counter_name(cnt);
    }
    const char *default_names[NV2A_PROF__COUNT] = {
        #define _X(x) stringify(x),
        NV2A_PROF_COUNTERS_XMAC
        #undef _X
    };

    assert(cnt < NV2A_PROF__COUNT);
    return default_names[cnt] + 10; /* 'NV2A_PROF_' */
}

int nv2a_profile_get_counter_value(unsigned int cnt)
{
    if (nv2a_stock_active()) {
        return stock_nv2a_profile_get_counter_value(cnt);
    }
    assert(cnt < NV2A_PROF__COUNT);
    unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) %
                       NV2A_PROF_NUM_FRAMES;
    return g_nv2a_stats.frame_history[idx].counters[cnt];
}
