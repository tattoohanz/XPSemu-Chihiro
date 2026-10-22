/*
 * OS-specific Helpers (PS5)
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

#include "xemu-os-utils.h"
#include "xemu-settings.h"
#include "ui/xemu-ps5-app.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/stat.h>
#include <pthread.h>
#include <ucontext.h>
#include <unistd.h>

#define XEMU_PS5_DIR "/data/xemu-chihiro"
#define XEMU_PS5_LOG XEMU_PS5_DIR "/xemu.log"

/* Where the eboot is loaded, and a bound on its code for backtraces. */
#define EBOOT_BASE 0x400000ULL
#define EBOOT_END 0x10000000ULL

int sceKernelUsleep(unsigned int microseconds);
int sceKernelSendNotificationRequest(int device, void *request, size_t size,
                                     int blocking);

/*
 * A system toast, which works even when nothing can be logged: the kernel's
 * 0xc30-byte request with the text at 0x2d (as PS5SX2's ProsperoNotify.cpp).
 */
/* The kernel's, not the wrapped one (ps5/compat/libc_stubs.c). */
uid_t __real_geteuid(void);

static void notify(const char *message)
{
    static uint8_t request[0xc30];
    memset(request, 0, sizeof(request));
    snprintf((char *)request + 0x2d, 1024, "%s", message);
    sceKernelSendNotificationRequest(0, request, sizeof(request), 0);
}

/*
 * A title runs sandboxed: no /data, and no executable memory for the JIT.
 * The HEN (etaHEN, OnionHEN or the PS5SX2 Helper, which is OnionHEN-based)
 * jailbreaks the process that publishes its PID here, if the title ID is in
 * its allowlist (/data/whitelist.txt for the PS5SX2 Helper). Protocol and
 * timing from PS5SX2 (ps5/coreorbis/orbis-shims/ProsperoHenJailbreak.cpp).
 */
#define HEN_REQUEST "/download0/etahen_jailbreak"
#define HEN_REQUEST_STAGED "/download0/etahen_jailbreak.tmp"
#define HEN_POLL_US 16667
#define HEN_MAX_POLLS 600          /* ~10 s for the HEN to take the request */
#define HEN_POST_CONSUME_POLLS 450 /* ~7.5 s for it to finish the jailbreak */

/* Writes what happened to msg; true once the process is root. */
static bool hen_jailbreak(char *msg, size_t msg_size)
{
    int pid = getpid();
    int uid_before = __real_geteuid();
    if (uid_before == 0) {
        snprintf(msg, msg_size, "already root (pid %d)", pid);
        return true;
    }

    char request[32];
    int len = snprintf(request, sizeof(request), "{\"PID\":%d}\n", pid);

    if ((unlink(HEN_REQUEST) != 0 && errno != ENOENT) ||
        (unlink(HEN_REQUEST_STAGED) != 0 && errno != ENOENT)) {
        snprintf(msg, msg_size, "stale request cleanup failed, errno %d", errno);
        return false;
    }
    int fd = open(HEN_REQUEST_STAGED, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                  0666);
    if (fd < 0) {
        snprintf(msg, msg_size, "request open failed, errno %d", errno);
        return false;
    }
    bool ok = fchmod(fd, 0666) == 0 && write(fd, request, len) == len &&
              fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    if (!ok || rename(HEN_REQUEST_STAGED, HEN_REQUEST) != 0) {
        snprintf(msg, msg_size, "request write failed, errno %d", errno);
        unlink(HEN_REQUEST_STAGED);
        return false;
    }

    /* The request disappearing only means it was taken: the jailbreak itself
     * finishes a little later, and acting before that sees stale creds. */
    int polls = 0;
    while (access(HEN_REQUEST, F_OK) == 0) {
        if (++polls >= HEN_MAX_POLLS) {
            unlink(HEN_REQUEST);
            snprintf(msg, msg_size,
                     "helper.elf didn't answer (pid %d): load helper.elf "
                     "with kstuff, then start XPSemu again",
                     pid);
            return false;
        }
        sceKernelUsleep(HEN_POLL_US);
    }
    int uid = __real_geteuid();
    for (int i = 0; i < HEN_POST_CONSUME_POLLS && uid != 0; i++) {
        sceKernelUsleep(HEN_POLL_US);
        uid = __real_geteuid();
    }
    /* __real_geteuid() may keep saying 1 with working creds (PS5SX2): the /data
     * and JIT checks that follow are what count. */
    snprintf(msg, msg_size, "request taken after %d polls, uid %d -> %d",
             polls, uid_before, uid);
    return uid == 0;
}

const char *xemu_get_os_info(void)
{
    // TODO: add the firmware version
    return "PlayStation 5";
}

/* The game's own log gets the last of xemu.log (ui/xui/game-log.cc). */
void xemu_ps5_game_log_copy(void);

#include "xemu-dev-ps5.h"
#if XPSEMU_DEV
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
#endif

static void write_str(const char *s)
{
    (void)!write(STDERR_FILENO, s, strlen(s));
}

/*
 * Logs where a crash happened. The eboot is position independent: subtract
 * the load address logged at startup (the address of xemu_ps5_early_init)
 * and add that function's address in build-ps5/title/llvm-pie.elf to look
 * an address up there.
 */
static void crash_handler(int sig, siginfo_t *info, void *context)
{
    static volatile sig_atomic_t in_handler;
    char buf[160];

    if (in_handler) {
        _exit(128 + sig);
    }
    in_handler = 1;

    /*
     * The PS5's context is FreeBSD's with the machine context 0x30 bytes
     * further on, so the header's field names read the wrong words: rbp is
     * at +0x88, rip at +0xe0 and rsp at +0xf8 (PS5SX2's ProsperoCrash.cpp).
     */
    const uint64_t *ctx = (const uint64_t *)context;
    uint64_t rip = ctx[0xe0 / 8], rsp = ctx[0xf8 / 8], rbp = ctx[0x88 / 8];
    snprintf(buf, sizeof(buf),
             "\n*** xemu crashed: signal %d, fault address %p, thread %p\n"
             "*** rip %#lx rsp %#lx rbp %#lx (eboot+%#lx)\n",
             sig, info->si_addr, (void *)pthread_self(), (unsigned long)rip,
             (unsigned long)rsp, (unsigned long)rbp,
             (unsigned long)(rip - EBOOT_BASE));
    write_str(buf);

    char toast[128];
    snprintf(toast, sizeof(toast), "XPSemu crashed: signal %d at eboot+%#lx",
             sig, (unsigned long)(rip - EBOOT_BASE));
    notify(toast);

    /* Return addresses into the eboot on the crashed stack: a backtrace that
     * needs no frame pointers (with some stale entries). Look them up with
     * llvm-addr2line -e build-ps5/title/llvm-pie.elf. */
#if XPSEMU_DEV
    /* The journal: where, and the eboot's return addresses (offsets) */
    char line[512];
    int n = snprintf(line, sizeof(line), "C %lld %d %s %lx %lx",
                     (long long)time(NULL), sig,
                     rip >= EBOOT_BASE && rip < EBOOT_END ? "eboot" :
                     rip >= 0x900000000ULL && rip < 0x908000000ULL ? "jit" :
                                                             "system",
                     (unsigned long)(rip >= EBOOT_BASE && rip < EBOOT_END ?
                                     rip - EBOOT_BASE : rip),
                     (unsigned long)info->si_addr);
#endif
    if (rsp >= 0x100000 && (rsp & 7) == 0) {
        const uint64_t *sp = (const uint64_t *)rsp;
        for (int i = 0, found = 0; i < 256 && found < 24; i++) {
            if (sp[i] >= EBOOT_BASE && sp[i] < EBOOT_END) {
                snprintf(buf, sizeof(buf), "***   stack[%d] %#lx\n", i,
                         (unsigned long)sp[i]);
                write_str(buf);
#if XPSEMU_DEV
                if (found < 8 && n < (int)sizeof(line) - 24) {
                    n += snprintf(line + n, sizeof(line) - n, " %lx",
                                  (unsigned long)(sp[i] - EBOOT_BASE));
                }
#endif
                found++;
            }
        }
    }

    xemu_ps5_game_log_copy();
#if XPSEMU_DEV
    snprintf(line + n, sizeof(line) - n, "\n");
    xemu_dev_journal_raw(line);
    /* The stress test goes on: XPSemu again (it picks the next game) */
    if (xemu_dev_stress_on) {
        write_str("*** stress test: starting XPSemu again\n");
        sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
        sceSystemServiceLoadExec("/data/homebrew/" XEMU_PS5_TITLE_ID "/eboot.bin", NULL);
    }
#endif
    _exit(128 + sig);
}

/*
 * Logs the imports the console left unresolved: calling one jumps to 0. The
 * title's imports.txt (ps5/build-title.sh) lists each GOT slot's address in
 * the ELF and the symbol it holds.
 */
static char g_missing_imports[2048] = " "; /* " name name ... " */

int xemu_ps5_import_ok(const char *name)
{
    char token[160];
    snprintf(token, sizeof(token), " %s ", name);
    return strstr(g_missing_imports, token) == NULL;
}

static void report_unresolved_imports(void)
{
    /* The title's own folder: /app0 isn't mounted for ShadowMountPlus titles,
     * but jailbroken, xemu can read where it was installed. */
    static const char *const paths[] = {
        "/app0/imports.txt",
        "/data/homebrew/" XEMU_PS5_TITLE_ID "/imports.txt",
    };
    FILE *f = NULL;
    for (size_t i = 0; !f && i < sizeof(paths) / sizeof(paths[0]); i++) {
        f = fopen(paths[i], "r");
    }
    if (!f) {
        fprintf(stderr, "xemu PS5: can't read imports.txt, errno %d\n", errno);
        return;
    }

    char name[128], list[512] = "";
    unsigned long offset;
    int checked = 0, missing = 0;
    while (fscanf(f, "%lx %127s", &offset, name) == 2) {
        checked++;
        if (*(const uint64_t *)(EBOOT_BASE + offset) == 0) {
            fprintf(stderr, "xemu PS5: unresolved import: %s\n", name);
            size_t used = strlen(g_missing_imports);
            snprintf(g_missing_imports + used, sizeof(g_missing_imports) - used,
                     "%s ", name);
            if (missing++ < 12) {
                snprintf(list + strlen(list), sizeof(list) - strlen(list),
                         "%s%s", missing > 1 ? " " : "", name);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "xemu PS5: %d of %d imports unresolved\n", missing,
            checked);

    if (missing) {
        char toast[600];
        snprintf(toast, sizeof(toast), "XPSemu: %d unresolved imports: %s",
                 missing, list);
        notify(toast);
    }
}

void xemu_ps5_early_init(void)
{
    /* Before anything touches /data, which the sandbox hides. */
    char jailbreak_msg[192];
    bool jailbroken = hen_jailbreak(jailbreak_msg, sizeof(jailbreak_msg));

    mkdir(XEMU_PS5_DIR, 0777);

    /* RADV's own disk cache of the GPU code it compiles (it finds no
     * folder by itself on the PS5): shaders from earlier sessions load
     * instead of compiling again. */
    mkdir(XEMU_PS5_DIR "/cache", 0777);
    mkdir(XEMU_PS5_DIR "/cache/mesa", 0777);
    setenv("MESA_SHADER_CACHE_DIR", XEMU_PS5_DIR "/cache/mesa", 1);
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "1G", 1);

    /* The log of the run before, for when this one doesn't get far. */
    rename(XEMU_PS5_LOG, XEMU_PS5_LOG ".old");
    bool logging = freopen(XEMU_PS5_LOG, "w", stderr) != NULL;
    if (logging) {
        setvbuf(stderr, NULL, _IONBF, 0);
        /* A title may start without descriptors 1 and 2, so the log may not
         * be on 2: put it there for the crash handler's write(2) too. */
        if (fileno(stderr) != STDERR_FILENO) {
            dup2(fileno(stderr), STDERR_FILENO);
        }
        dup2(fileno(stderr), STDOUT_FILENO);
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    /* Quiet start: a toast only when something is wrong (the log has it all). */
    if (!jailbroken || !logging) {
        char toast[256];
        snprintf(toast, sizeof(toast), "XPSemu: jailbreak %s: %s%s",
                 jailbroken ? "ok" : "NOT CONFIRMED", jailbreak_msg,
                 logging ? "" : " (can't write " XEMU_PS5_LOG ")");
        notify(toast);
    }

    fprintf(stderr, "xemu PS5: load address marker xemu_ps5_early_init=%p\n",
            (void *)xemu_ps5_early_init);
    fprintf(stderr, "xemu PS5: jailbreak %s: %s\n",
            jailbroken ? "ok" : "NOT CONFIRMED", jailbreak_msg);

    report_unresolved_imports();
#if XPSEMU_DEV
    xemu_dev_start(); /* The journal's S line */
#endif

    /* The main thread's handler runs on its own stack, so a broken stack
     * can still be reported. */
    static uint8_t alt_stack[64 * 1024] __attribute__((aligned(16)));
    stack_t ss = { .ss_sp = alt_stack, .ss_size = sizeof(alt_stack) };
    if (sigaltstack(&ss, NULL) != 0) {
        fprintf(stderr, "xemu PS5: sigaltstack failed, errno %d\n", errno);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    /* SIGSYS: a system call the sandbox refuses */
    const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT,
                            SIGSYS,  SIGTRAP, SIGEMT };
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &sa, NULL) != 0) {
            fprintf(stderr, "xemu PS5: no handler for signal %d, errno %d\n",
                    signals[i], errno);
        }
    }
}

/*
 * CPU cores for the busiest threads, as PS5SX2 lays them out
 * (pcsx2/GS/Renderers/Common/GSRenderer.cpp, OrbisApplyLayout): the Xbox's
 * CPU (the TCG vCPU thread) and its GPU (nv2a's pfifo thread) each get a
 * core of their own, with its SMT sibling left idle, and every other thread
 * the CPUs left over. Hyperthread siblings are taken to be CPUs 2k and
 * 2k+1, as PS5SX2 does.
 *
 * The threads register when they start; the layout is applied later, from
 * the UI thread, once start-up (Vulkan's threads included) is over, so no
 * thread created meanwhile inherits a hot thread's single CPU.
 */
int scePthreadGetaffinity(pthread_t thread, unsigned long long *mask);
int scePthreadSetaffinity(pthread_t thread, unsigned long long mask);
int scePthreadSetprio(pthread_t thread, int prio);

static pthread_t pin_threads[XEMU_PS5_THREAD__COUNT];
static bool pin_registered[XEMU_PS5_THREAD__COUNT];
static char pin_summary[256] = "not applied";

static void prof_register(int role);

void xemu_ps5_register_thread(int role)
{
    if (role >= 0 && role < XEMU_PS5_THREAD__COUNT) {
        pin_threads[role] = pthread_self();
        pin_registered[role] = true;
    }
#ifdef XPS_PROFILER /* Removed from the build: it interrupted threads */
    if (role >= 0 && role < XEMU_PS5_THREAD__ALL) {
        prof_register(role);
    }
#endif
}

const char *xemu_ps5_pinning_summary(void)
{
    return pin_summary;
}

void xemu_ps5_apply_pinning(void)
{
    unsigned long long all = 0;
    if (scePthreadGetaffinity(pthread_self(), &all) != 0 || all == 0) {
        snprintf(pin_summary, sizeof(pin_summary), "no CPU mask");
        fprintf(stderr, "xemu PS5: pinning: %s\n", pin_summary);
        return;
    }
    if (!g_config.perf.cpu_pinning) {
        snprintf(pin_summary, sizeof(pin_summary), "off (CPUs %#llx)", all);
        fprintf(stderr, "xemu PS5: pinning: %s\n", pin_summary);
        return;
    }

    /* Whole cores from the top: the CPU thread's, then the GPU thread's. */
    int cpu[XEMU_PS5_THREAD__COUNT] = { -1, -1 };
    unsigned long long reserved = 0;
    int found = 0;
    for (int k = 31; k >= 0 && found < XEMU_PS5_THREAD__COUNT; k--) {
        unsigned long long core = 3ull << (2 * k);
        if ((all & core) == core) {
            cpu[found++] = 2 * k;
            reserved |= core;
        }
    }
    unsigned long long rest = all & ~reserved;
    if (found < XEMU_PS5_THREAD__COUNT || rest == 0) {
        snprintf(pin_summary, sizeof(pin_summary),
                 "skipped: CPUs %#llx have too few whole cores", all);
        fprintf(stderr, "xemu PS5: pinning: %s\n", pin_summary);
        return;
    }

    /* Every thread to the rest first (the console's kernel may want a
     * smaller set than cpuset_t: PS5SX2 found 8, 16 or 32 bytes). */
    cpuset_t set;
    CPU_ZERO(&set);
    for (int c = 0; c < 64; c++) {
        if ((rest >> c) & 1) {
            CPU_SET(c, &set);
        }
    }
    int rc_all = -1;
    const size_t sizes[] = { 8, 16, sizeof(set) };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        rc_all = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
                                    sizes[i], &set) == 0 ? 0 : errno;
        if (rc_all != ERANGE) {
            break;
        }
    }

    int rc[XEMU_PS5_THREAD__COUNT];
    for (int i = 0; i < XEMU_PS5_THREAD__COUNT; i++) {
        rc[i] = pin_registered[i] ?
                    scePthreadSetaffinity(pin_threads[i], 1ull << cpu[i]) :
                    -1;
    }

    /* Top priority for the two hot threads, each alone on its core
     * (PS5SX2 does the same: default-priority threads get delayed). */
    char prio[48] = "default priority";
    if (g_config.perf.thread_priority) {
        int rp[XEMU_PS5_THREAD__COUNT];
        for (int i = 0; i < XEMU_PS5_THREAD__COUNT; i++) {
            rp[i] = pin_registered[i] && rc[i] == 0 ?
                        scePthreadSetprio(pin_threads[i], 256) : -1;
        }
        snprintf(prio, sizeof(prio), "priority 256 (rc %#x, %#x)",
                 rp[0], rp[1]);
    }
    snprintf(pin_summary, sizeof(pin_summary),
             "Xbox CPU on CPU %d (rc %#x), Xbox GPU on CPU %d (rc %#x), "
             "others %#llx (rc %d), of %#llx, %s",
             cpu[0], rc[0], cpu[1], rc[1], rest, rc_all, all, prio);
    fprintf(stderr, "xemu PS5: pinning: %s\n", pin_summary);
}


/*
 * A sampling profiler for the game log (XPSemu): where the busy threads
 * spend their time. A sampler thread signals each registered thread (the
 * Xbox's CPU and GPU, the UI) 500 times a second; the handler, running on
 * that thread, notes where it was (rip, from the PS5's ucontext, see the
 * crash handler) and the thread's own CPU time. Addresses in the eboot
 * are counted by 16-byte block and reported as eboot offsets, like the
 * crash handler's: llvm-addr2line -f -C -e build-ps5/title/llvm-pie.elf
 * <offset>.
 */
#define PROF_SIGNAL SIGPROF
#define PROF_HZ 500
#define PROF_SLOTS 4096
#define JIT_BASE 0x900000000ULL /* ps5/compat/mmap.c's JIT buffer */
#define JIT_END (JIT_BASE + (512ULL << 20))

typedef struct {
    uint64_t addr;
    uint32_t count;
} ProfSlot;

static pthread_t prof_threads[XEMU_PS5_THREAD__ALL];
static volatile bool prof_registered[XEMU_PS5_THREAD__ALL];
static ProfSlot prof_table[XEMU_PS5_THREAD__ALL][PROF_SLOTS];
static uint32_t prof_total[XEMU_PS5_THREAD__ALL], prof_jit[XEMU_PS5_THREAD__ALL],
    prof_eboot[XEMU_PS5_THREAD__ALL], prof_other[XEMU_PS5_THREAD__ALL];
static volatile int64_t prof_cpu_ns[XEMU_PS5_THREAD__ALL]; /* -1: unknown */
static volatile bool prof_on;
static int prof_started;              /* prof_init has run (atomic) */
static int prof_sigaction_rc = 1, prof_thread_rc = 1;
static volatile uint32_t prof_sent, prof_received, prof_kill_errors;
static volatile int prof_last_kill_error;
static volatile uint32_t prof_loops;

static void prof_handler(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    (void)info;
    int saved_errno = errno;
    prof_received++;
    pthread_t self = pthread_self();
    int role = -1;
    for (int i = 0; i < XEMU_PS5_THREAD__ALL; i++) {
        if (prof_registered[i] && pthread_equal(prof_threads[i], self)) {
            role = i;
            break;
        }
    }
    if (role < 0) {
        errno = saved_errno;
        return;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        prof_cpu_ns[role] = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    }
    if (prof_on) {
        const uint64_t *ctx = (const uint64_t *)context;
        uint64_t rip = ctx[0xe0 / 8]; /* See crash_handler */
        prof_total[role]++;
        if (rip >= JIT_BASE && rip < JIT_END) {
            prof_jit[role]++; /* The game's code, translated */
        } else if (rip < EBOOT_BASE || rip >= EBOOT_END) {
            prof_other[role]++; /* System libraries: waits, drivers... */
        } else {
            prof_eboot[role]++;
            uint64_t key = rip & ~0xfULL;
            uint32_t h = (uint32_t)((key >> 4) * 2654435761u) % PROF_SLOTS;
            for (int probe = 0; probe < 64; probe++) {
                ProfSlot *slot = &prof_table[role][(h + probe) % PROF_SLOTS];
                if (slot->addr == key) {
                    slot->count++;
                    break;
                }
                if (slot->addr == 0) {
                    slot->addr = key;
                    slot->count = 1;
                    break;
                }
            }
        }
    }
    errno = saved_errno;
}

static void *prof_sampler(void *arg)
{
    (void)arg;
    const struct timespec period = { 0, 1000000000 / PROF_HZ };
    const struct timespec idle = { 0, 200000000 };
    for (;;) {
        prof_loops++;
        if (!g_config.perf.profiler) { /* Advanced settings */
            nanosleep(&idle, NULL);
            continue;
        }
        for (int i = 0; i < XEMU_PS5_THREAD__ALL; i++) {
            if (prof_registered[i]) {
                int rc = pthread_kill(prof_threads[i], PROF_SIGNAL);
                if (rc == 0) {
                    prof_sent++;
                } else {
                    prof_kill_errors++;
                    prof_last_kill_error = rc;
                }
            }
        }
        nanosleep(&period, NULL);
    }
    return NULL;
}

static void prof_init(void)
{
    for (int i = 0; i < XEMU_PS5_THREAD__ALL; i++) {
        prof_cpu_ns[i] = -1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    prof_sigaction_rc = sigaction(PROF_SIGNAL, &sa, NULL) == 0 ? 0 : errno;

    pthread_t t;
    prof_thread_rc = pthread_create(&t, NULL, prof_sampler, NULL);
    if (prof_thread_rc == 0) {
        pthread_detach(t);
    }
    fprintf(stderr, "xemu PS5: profiler: sigaction %d, sampler thread %d\n",
            prof_sigaction_rc, prof_thread_rc);
}

/* A thread registers for the profiler (from itself: QEMU's threads start
 * with every signal blocked, so it lets the profiler's through). */
static void prof_register(int role)
{
    /* Once, by whichever thread registers first (pthread_once did nothing
     * on the console: prof_init never ran). */
    if (__atomic_exchange_n(&prof_started, 1, __ATOMIC_ACQ_REL) == 0) {
        prof_init();
    }
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, PROF_SIGNAL);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    prof_threads[role] = pthread_self();
    prof_registered[role] = true;
}

void xemu_ps5_profile_start(void)
{
    prof_on = false;
    memset(prof_table, 0, sizeof(prof_table));
    memset(prof_total, 0, sizeof(prof_total));
    memset(prof_jit, 0, sizeof(prof_jit));
    memset(prof_eboot, 0, sizeof(prof_eboot));
    memset(prof_other, 0, sizeof(prof_other));
    prof_on = true;
}

void xemu_ps5_profile_stop(void)
{
    prof_on = false;
}

bool xemu_ps5_thread_cpu_seconds(int role, double *seconds)
{
    if (role < 0 || role >= XEMU_PS5_THREAD__ALL || prof_cpu_ns[role] < 0) {
        return false;
    }
    *seconds = prof_cpu_ns[role] / 1e9;
    return true;
}

static int prof_slot_cmp(const void *a, const void *b)
{
    const ProfSlot *x = a, *y = b;
    return (int)y->count - (int)x->count;
}

char *xemu_ps5_profile_report(void)
{
#ifndef XPS_PROFILER
    return NULL; /* No profiler in the build */
#endif
    static const char *const names[XEMU_PS5_THREAD__ALL] = {
        "Xbox CPU thread", "Xbox GPU thread", "UI thread"
    };
    size_t cap = 16384, len = 0;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }
    out[0] = 0;
#define PUT(...) \
    len += snprintf(out + len, len < cap ? cap - len : 0, __VA_ARGS__)

    PUT("Profile (%d samples/s per thread; eboot offsets: llvm-addr2line "
        "-f -C -e llvm-pie.elf <offset>)\n", PROF_HZ);
    PUT("  [profiler: sigaction %d, sampler %d, signals sent %u, received %u, "
        "failed %u (last error %d), sampler loops %u, setting %s, threads",
        prof_sigaction_rc, prof_thread_rc, prof_sent, prof_received,
        prof_kill_errors, prof_last_kill_error, prof_loops,
        g_config.perf.profiler ? "on" : "OFF");
    for (int r = 0; r < XEMU_PS5_THREAD__ALL; r++) {
        PUT(" %c", prof_registered[r] ? '+' : '-');
    }
    PUT("]\n");
    static ProfSlot top[PROF_SLOTS];
    for (int r = 0; r < XEMU_PS5_THREAD__ALL; r++) {
        uint32_t total = prof_total[r];
        if (!total) {
            PUT("  %s: no samples\n", names[r]);
            continue;
        }
        PUT("  %s: %u samples - game code (JIT) %.1f%%, XPSemu/xemu %.1f%%, "
            "system (waits, driver) %.1f%%\n",
            names[r], total, 100.0 * prof_jit[r] / total,
            100.0 * prof_eboot[r] / total, 100.0 * prof_other[r] / total);
        memcpy(top, prof_table[r], sizeof(top));
        qsort(top, PROF_SLOTS, sizeof(ProfSlot), prof_slot_cmp);
        int places = r == XEMU_PS5_THREAD_CPU ? 80 : 15;
        for (int i = 0; i < places && top[i].count; i++) {
            PUT("    eboot+%#lx  %5.1f%%\n",
                (unsigned long)(top[i].addr - EBOOT_BASE),
                100.0 * top[i].count / total);
        }
    }
#undef PUT
    return out;
}

/*
 * No hard disk image: XPSemu makes one, xemu-dashboard's blank Xbox disk
 * (ui/xui/blank-hdd.h: formatted C, E and the X/Y/Z caches, a small free
 * dashboard, no Microsoft files). Written whole beside it, then renamed in.
 */
#include <zlib.h>
#include "xui/blank-hdd.h"

int xemu_ps5_hdd_created;

void xemu_ps5_blank_hdd(const char *path)
{
    struct stat st;
    if (!path || !path[0] || stat(path, &st) == 0 || errno != ENOENT) {
        return;
    }
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        mkdir(dir, 0777);
    }
    uLongf size = XPS_BLANK_HDD_SIZE;
    unsigned char *data = malloc(size);
    if (!data || uncompress(data, &size, kBlankHddZ, sizeof(kBlankHddZ)) != Z_OK ||
        size != XPS_BLANK_HDD_SIZE) {
        fprintf(stderr, "XPSemu: blank hard disk: can't unpack it\n");
        free(data);
        return;
    }
    char part[600];
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *f = fopen(part, "wb");
    int ok = f && fwrite(data, 1, size, f) == size;
    ok = f && fclose(f) == 0 && ok;
    free(data);
    if (!ok || rename(part, path) != 0) {
        fprintf(stderr, "XPSemu: blank hard disk: can't write %s (%d)\n", path,
                errno);
        unlink(part);
        return;
    }
    chmod(path, 0666);
    xemu_ps5_hdd_created = 1;
    fprintf(stderr, "XPSemu: no hard disk: made a blank one, %s\n", path);
}

/*
 * XPSemu Chihiro
 */

typedef struct XpsAppInfo {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} XpsAppInfo;
int sceKernelGetAppInfo(pid_t pid, XpsAppInfo *info);

const char *xemu_ps5_own_title_id(void)
{
    static char id[16];
    static bool asked;
    if (!asked) {
        asked = true;
        XpsAppInfo info;
        memset(&info, 0, sizeof(info));
        if (xemu_ps5_import_ok("sceKernelGetAppInfo") &&
            sceKernelGetAppInfo(getpid(), &info) == 0) {
            snprintf(id, sizeof(id), "%.*s", (int)strnlen(info.title_id,
                     sizeof(info.title_id)), info.title_id);
        }
        fprintf(stderr, "XPSemu Chihiro: running as %s\n", id[0] ? id : "(unknown)");
    }
    return id;
}

/* The first line of a small text file (malloc'd), or NULL. */
static char *first_line(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    char buf[1024];
    char *line = fgets(buf, sizeof(buf), f);
    fclose(f);
    if (!line) {
        return NULL;
    }
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                 line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = 0;
    }
    return n ? strdup(line) : NULL;
}

static const char kEnvTemplate[] =
    "# XPSemu Chihiro: switches read when the app starts, one NAME=VALUE\n"
    "# per line ('#' starts a comment). Change, save, restart the app.\n"
    "#\n"
    "# Maximum Tune 1/2 export: Gemballa maker and Blackbird's Gemballa\n"
    "# (in memory, only on the export V307.xbe / V322.xbe; 0 = off)\n"
    "XEMU_WM1_GEMBALLA=1\n"
    "XEMU_WM1_BLACKBIRD=1\n"
    "XEMU_WM1_BLACKBIRD_RIVAL=1\n"
    "XEMU_WM1_BLACKBIRD_SPECIAL=1\n"
    "XEMU_WM2_GEMBALLA=1\n"
    "XEMU_WM2_BLACKBIRD=1\n"
    "XEMU_WM2_BLACKBIRD_RIVAL=1\n"
    "XEMU_WM2_KIJIMA=1\n"
    "#\n"
    "# 16:9 widescreen for the Chihiro games (1 = on); set the screen shape\n"
    "# to 16:9 too. With it, as the desktop build's .bat: the copy-pass fix,\n"
    "# the HUD out to the 16:9 edges (within EDGE percent of an edge), the\n"
    "# menus kept 4:3, the menu background not stretched.\n"
    "XEMU_WS_HACK=0\n"
    "XEMU_WS_COPY=1\n"
    "XEMU_WS_HUD=1\n"
    "XEMU_WS_HUD_EDGE=26\n"
    "XEMU_WS_MENU43=1\n"
    "XEMU_WS_MENUBG=0\n"
    "#\n"
    "# Xbox CPU reads of a depth buffer take the last readback (about a frame\n"
    "# old) instead of waiting for the PS5 GPU (Maximum Tune 2's light checks;\n"
    "# 0 = wait as before)\n"
    "XEMU_DEPTH_READ_LAG=1\n"
    "#\n"
    "# A depth buffer larger than the color target serves it as it is\n"
    "# (Ollie King's smaller passes; 0 = drop and recreate it as before)\n"
    "XEMU_ZETA_REUSE_LARGER=1\n"
    "#\n"
    "# Chihiro log categories into xemu.log: boot,jvs,card,ffb,mbcom,usb,net\n"
    "#XEMU_CHIHIRO_LOG=boot\n";

/*
 * env.txt's switches. The link wraps getenv (ps5/build-title.sh): a name set
 * here answers from this table, anything else from the system's getenv, so
 * the Chihiro code's getenv("XEMU_WM2_GEMBALLA") works as on the desktop
 * without relying on the console's setenv.
 */
#define ENV_MAX 64
static struct {
    char name[64];
    char value[256];
} env_table[ENV_MAX];
static int env_count;

char *__real_getenv(const char *name);
/* Hidden: the title must not export it (ps5-native-tool refuses exports). */
__attribute__((visibility("hidden"))) char *__wrap_getenv(const char *name);
__attribute__((visibility("hidden"))) char *__wrap_getenv(const char *name)
{
    for (int i = 0; name && i < env_count; i++) {
        if (!strcmp(env_table[i].name, name)) {
            return env_table[i].value;
        }
    }
    return __real_getenv(name);
}

void xemu_ps5_chihiro_prepare(void)
{
    mkdir(XEMU_PS5_DATA_DIR, 0777);
    mkdir(XEMU_PS5_CHIHIRO_DIR, 0777);
    mkdir(XEMU_PS5_DATA_DIR "/games", 0777);

    struct stat st;
    if (stat(XEMU_PS5_ENV_FILE, &st) != 0) {
        FILE *f = fopen(XEMU_PS5_ENV_FILE, "w");
        if (f) {
            fputs(kEnvTemplate, f);
            fclose(f);
            chmod(XEMU_PS5_ENV_FILE, 0666);
            fprintf(stderr, "XPSemu Chihiro: wrote %s\n", XEMU_PS5_ENV_FILE);
        }
    }

    FILE *f = fopen(XEMU_PS5_ENV_FILE, "r");
    if (!f) {
        fprintf(stderr, "XPSemu Chihiro: no %s (%d)\n", XEMU_PS5_ENV_FILE, errno);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' ||
                     p[n - 1] == ' ' || p[n - 1] == '\t')) {
            p[--n] = 0;
        }
        char *eq = strchr(p, '=');
        if (!n || p[0] == '#' || !eq || eq == p) {
            continue;
        }
        *eq = 0;
        char *name = p, *value = eq + 1;
        size_t k = strlen(name);
        while (k && (name[k - 1] == ' ' || name[k - 1] == '\t')) {
            name[--k] = 0;
        }
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        int slot = 0;
        while (slot < env_count && strcmp(env_table[slot].name, name) != 0) {
            slot++;
        }
        if (slot == ENV_MAX) {
            fprintf(stderr, "XPSemu Chihiro: env %s ignored (more than %d)\n",
                    name, ENV_MAX);
            continue;
        }
        snprintf(env_table[slot].name, sizeof(env_table[slot].name), "%s", name);
        snprintf(env_table[slot].value, sizeof(env_table[slot].value), "%s",
                 value);
        if (slot == env_count) {
            env_count++;
        }
        fprintf(stderr, "XPSemu Chihiro: env %s=%s\n", name, value);
    }
    fclose(f);
}

static char xbox_autolaunch[1024];
static bool boot_requested;

char *xemu_ps5_chihiro_boot_image(void)
{
    struct stat st;

    /* The dashboard's request comes first: it restarted the app for it
     * (another game, another machine, or none: back to the dashboard). */
    if (stat(XEMU_PS5_CHIHIRO_BOOT_FILE, &st) == 0) {
        boot_requested = true;
        char *image = first_line(XEMU_PS5_CHIHIRO_BOOT_FILE);
        unlink(XEMU_PS5_CHIHIRO_BOOT_FILE); /* Once: a crash restarts at the dashboard */
        if (!image) {
            fprintf(stderr, "XPSemu Chihiro: the dashboard asked for itself\n");
            return NULL;
        }
        if (stat(image, &st) != 0) {
            fprintf(stderr, "XPSemu Chihiro: can't boot %s\n", image);
            free(image);
            return NULL;
        }
        if (!xemu_media_is_xbox_disc(image)) {
            fprintf(stderr, "XPSemu Chihiro: the dashboard asked for %s\n", image);
            return image;
        }
        /* An Xbox game picked while a Chihiro ran: the Xbox starts, and its
         * dashboard starts the game (xemu_ps5_xbox_autolaunch). */
        snprintf(xbox_autolaunch, sizeof(xbox_autolaunch), "%s", image);
        fprintf(stderr, "XPSemu Chihiro: the dashboard asked for the Xbox "
                "game %s\n", image);
        free(image);
        return NULL;
    }

    const char *id = xemu_ps5_own_title_id();
    if (id[0] && strcmp(id, XEMU_PS5_TITLE_ID) != 0) {
        /* A home-screen shortcut: its game, every time it is opened. An
         * Xbox disc is left to the dashboard (it starts it itself). */
        char path[128];
        snprintf(path, sizeof(path), "/data/homebrew/%s/xpsemu-game.txt", id);
        char *image = first_line(path);
        if (image && stat(image, &st) == 0 && !xemu_media_is_xbox_disc(image)) {
            fprintf(stderr, "XPSemu Chihiro: shortcut %s boots %s\n", id, image);
            return image;
        }
        free(image);
    }
    return NULL;
}

bool xemu_ps5_boot_requested(void)
{
    return boot_requested;
}

const char *xemu_ps5_xbox_autolaunch(void)
{
    return xbox_autolaunch;
}
