/*
 * XPSemu: stopwatches on the emulator's costly steps, for the game log
 * (ui/xui/game-log.cc): total nanoseconds and how many times each, since
 * start. Atomic adds, so any thread can time.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef HW_XBOX_XEMU_TIMING_H
#define HW_XBOX_XEMU_TIMING_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XT_FINISH_SUBMIT, /* GPU thread: pgraph_vk_finish's vkQueueSubmit */
    XT_FINISH_WAIT,   /* ...and its wait for the PS5 GPU */
    XT_AUX_WAIT,      /* One-off GPU jobs (downloads, uploads): wait idle */
    XT_DOWNLOAD,      /* A surface copied back to the Xbox's memory */
    XT_GPU_IDLE,      /* GPU thread with nothing to do */
    XT_CPU_EXEC,      /* Xbox CPU thread running the game */
    XT_CPU_IDLE,      /* ...waiting (halted, or for the machine lock) */
    XT_UI_FRAME,      /* UI: getting the game's frame from the GPU thread */
    XT_UI_PRESENT,    /* UI: drawing and presenting to the TV */
    XT_TB_INVAL,      /* (count only) translated code blocks thrown away */
    XT_JC_FLUSH,      /* (count only) whole jump cache wiped */
    XT_FLUSH_CR3,     /* (count only) whole TLB flushed: the game wrote CR3 */
    XT_FLUSH_CR4,     /* (count only) ...: CR4 paging bits changed (PGE) */
    XT_FLUSH_CR0,     /* (count only) ...: CR0 paging bits changed */
    XT_UI_STALL,      /* (count only) UI waited over 8 ms for the frame */
    XT_SHADER,        /* GPU thread: a shader module made (GLSL + SPIR-V) */
    XT_PIPELINE,      /* GPU thread: vkCreateGraphicsPipelines */
    XT_TB_GEN,        /* CPU thread: code translated (or revived) */
    XT_TB_REVIVE,     /* (count only) ...a thrown-away block reused as is */
    XT_SMC_WRITE,     /* (count only) CPU store into a page holding code */
    XT_SMC_EXIT,      /* (count only) the running block rewrote itself */
    XT_TB_INVAL_DMA,  /* (count only) blocks thrown away by a device write */
    XT_SUBMIT_LOCK,   /* GPU thread: XT_FINISH_SUBMIT's wait for the queue */
    XT_UI_QUEUE,      /* UI: holding the queue (its submit and present) */
    XT_PCACHE,        /* The pipeline cache save (driver read and write) */
    XT__COUNT
};

extern uint64_t xemu_timing_ns[XT__COUNT];
extern uint32_t xemu_timing_count[XT__COUNT];
extern uint64_t xemu_timing_max_ns[XT__COUNT]; /* Slowest one (game log resets) */

static inline uint64_t xemu_timing_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

static inline void xemu_timing_add(int what, uint64_t since)
{
    __atomic_fetch_add(&xemu_timing_ns[what], xemu_timing_now() - since,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&xemu_timing_count[what], 1, __ATOMIC_RELAXED);
}

/*
 * XPSemu: where the Xbox CPU thread's run time goes outside the game's own
 * code, and who else holds the machine lock (BQL) meanwhile (log only, the
 * game log's "CPU thread in devices", "machine lock held" and "main loop"
 * lines). Small tables of TSC sums keyed by a pointer, claimed on first
 * use; the game log reads and clears them.
 */
typedef struct XemuCostSlot {
    const void *key;
    const char *name;   /* a region's name, a lock site's file */
    int line;           /* a lock site's line */
    uint64_t tsc, max;
    uint32_t n;
} XemuCostSlot;

#define XEMU_MMIO_SLOTS 256 /* the CPU thread's device accesses, by region */
#define XEMU_HOLD_SLOTS 64  /* the BQL held by other threads, by site */
#define XEMU_CB_SLOTS 64    /* main loop timers and bottom halves, by cb */
extern XemuCostSlot xemu_mmio_slot[XEMU_MMIO_SLOTS];
extern XemuCostSlot xemu_hold_slot[XEMU_HOLD_SLOTS];
extern XemuCostSlot xemu_timer_slot[XEMU_CB_SLOTS];
extern XemuCostSlot xemu_bh_slot[XEMU_CB_SLOTS];
extern uint64_t xemu_bql_wait_tsc;
extern uint32_t xemu_bql_wait_n;

static inline uint64_t xemu_tsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

static inline void xemu_cost_note(XemuCostSlot *t, unsigned nslots,
                                  const void *key, const char *name,
                                  int line, uint64_t tsc)
{
    unsigned h = (unsigned)((((uintptr_t)key >> 4) ^ (unsigned)line) %
                            nslots);
    for (unsigned k = 0; k < 16; k++) {
        XemuCostSlot *s = &t[(h + k) % nslots];
        const void *cur = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
        if (!cur) {
            const void *expected = NULL;
            if (__atomic_compare_exchange_n(&s->key, &expected, key, false,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                s->name = name;
                s->line = line;
                cur = key;
            } else {
                cur = expected;
            }
        }
        if (cur == key && (line < 0 || s->line == line)) {
            __atomic_fetch_add(&s->tsc, tsc, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
            if (tsc > __atomic_load_n(&s->max, __ATOMIC_RELAXED)) {
                __atomic_store_n(&s->max, tsc, __ATOMIC_RELAXED);
            }
            return;
        }
    }
    /* Table full around here: dropped (the lines say so if it matters). */
}

static inline void xemu_mmio_note(const void *mr, const char *name,
                                  uint64_t tsc)
{
    xemu_cost_note(xemu_mmio_slot, XEMU_MMIO_SLOTS, mr, name, -1, tsc);
}

/* The game log's lines since the last call ("" until a second call). */
void xemu_cost_format(char *mmio, size_t mmio_size, char *hold,
                      size_t hold_size, char *loop, size_t loop_size);

/*
 * Where translated code gets thrown away: a small table of guest physical
 * pages and how many blocks each lost (collisions just replace; the game
 * log reads and clears it).
 */
#define XEMU_INVAL_PAGES 256
extern uint32_t xemu_inval_page[XEMU_INVAL_PAGES];
extern uint32_t xemu_inval_hits[XEMU_INVAL_PAGES];

static inline void xemu_note_inval_page(uint64_t phys)
{
    uint32_t page = (uint32_t)(phys >> 12);
    unsigned i = (page ^ (page >> 8)) % XEMU_INVAL_PAGES;
    if (__atomic_load_n(&xemu_inval_page[i], __ATOMIC_RELAXED) != page) {
        __atomic_store_n(&xemu_inval_page[i], page, __ATOMIC_RELAXED);
        __atomic_store_n(&xemu_inval_hits[i], 0, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&xemu_inval_hits[i], 1, __ATOMIC_RELAXED);
}

/* Like xemu_timing_add, also keeping the slowest; returns the time taken */
static inline uint64_t xemu_timing_add_max(int what, uint64_t since)
{
    uint64_t ns = xemu_timing_now() - since;
    uint64_t max = __atomic_load_n(&xemu_timing_max_ns[what], __ATOMIC_RELAXED);
    __atomic_fetch_add(&xemu_timing_ns[what], ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&xemu_timing_count[what], 1, __ATOMIC_RELAXED);
    while (ns > max &&
           !__atomic_compare_exchange_n(&xemu_timing_max_ns[what], &max, ns,
                                        true, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    return ns;
}

#ifdef __cplusplus
}
#endif

#endif
