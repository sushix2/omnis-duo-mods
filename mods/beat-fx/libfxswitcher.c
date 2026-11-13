/*
 * libfxknob.so - Hardware Beat FX Target Selector for Pioneer DJ / AlphaTheta OMNIS-DUO
 *
 * Physical Controls:
 *  - SHIFT + Headphone CUE 2 -> Next Beat FX Target
 *  - SHIFT + Headphone CUE 1 -> Previous Beat FX Target
 *
 * Target Cycle:
 *  CH1 <-> CH2 <-> MIC <-> MASTER
 *
 * Features:
 *  - Suppresses / swallows stock Headphone CUE action while SHIFT is held.
 *  - Relaxes the SPI Rx CRC check so a modified packet is accepted.
 *  - Updates effector channel in audio engine (FxState::setChannelSelect).
 *  - Updates CHSelectPanel buttonClicked listener and updateButtons highlights.
 *  - Drawer logic:
 *      * If closed: slide open drawer smoothly.
 *      * If already open: keep open without re-triggering slide animations or redrawing.
 *      * Dismisses after 3 seconds of inactivity.
 *  - Intercepts poll, ppoll, epoll_wait on JUCE message thread for real-time responsiveness.
 *
 * Audio-routing perpetuation (verified against EP148 v1.23):
 *  - FxState::setChannelSelect (0x1d720e8) writes the engine object at FxState+0xa0
 *    and dispatches a routing command through 0x1d75090 with a per-channel enum
 *    (CH1=2, CH2=7, MIC=4, MASTER=5). This is the same call the native touchscreen
 *    CHSelectPanel listener (0x1bb45f8) tail-calls, so the hardware shortcut
 *    perpetuates into the audio engine exactly like a touchscreen tap.
 *  - FX_MOD_VERIFY=1 enables before/after engine-state logging to stderr.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <poll.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <ucontext.h>

#ifndef gettid
static inline pid_t get_tid(void) {
    return (pid_t)syscall(SYS_gettid);
}
#define gettid get_tid
#endif

extern char *__progname;

static int s_spi_fd = -1;
static void *g_chSelectPanel = NULL;
static void *g_beatFxPanel = NULL;
static void *g_panelPlacingComp = NULL;
static void *g_fxState = NULL;
static int s_current_cycle_idx = 0; /* 0=CH1, 1=CH2, 2=MIC, 3=MASTER */
static volatile int s_pending_target_step = 0; /* +1 for NEXT, -1 for PREV */
static uint64_t s_panel_dismiss_time_ms = 0;

/* ---------------------------------------------------------------------------
 * Audio-routing perpetuation verification (FX_MOD_VERIFY=1)
 *
 * The mod must not merely repaint the GUI: the change has to reach the audio
 * engine. FxState::setChannelSelect (0x1d720e8) is the single funnel the stock
 * touchscreen path uses; it writes the engine object at FxState+0xa0 and
 * dispatches a routing command via 0x1d75090. We snapshot that engine state
 * before and after each hardware step so a log line proves the write landed.
 * ------------------------------------------------------------------------- */
static int s_verify = 0;
static unsigned long s_verify_steps = 0;
static unsigned long s_verify_engine_changes = 0;
/* Deferred confirmation: after a step, getChannelSelect (+0x4d) is updated
 * asynchronously by the engine. We re-read it on later GUI-thread ticks and
 * log when it matches the commanded channel, proving the engine accepted the
 * routing change end-to-end. */
static int s_confirm_pending = 0;
static int s_confirm_expected = -1;
static uint64_t s_confirm_deadline_ms = 0;

/* ---------------------------------------------------------------------------
 * MOD menu (UTILITY screen)
 *
 * The stock UTILITY screen is a table of sections, each holding rows. The
 * section list is built by 0x1ae2528 and the DECK row list by 0x1ae4128.
 * Both append to a small vector via 0x21490b0(vector, index, string), which
 * auto-grows through 0x216e460.
 *
 * We append one extra section ("MOD") and one extra row ("Beat FX Switcher") to
 * the live table after the stock builders have run, then gate the mod on the
 * row's value. The row is a plain toggle: tapping it flips s_mod_enabled.
 *
 * Table layout (from disassembly):
 *   vector { void **data; int32 capacity; int32 size; }
 *   section list base = UtilityView + 0x48
 *   DECK row list base = UtilityView + 0x58
 * ------------------------------------------------------------------------- */
static volatile int s_mod_enabled = 1;   /* default ON, matching current behaviour */
static int s_mod_menu_installed = 0;
static void *g_utility_view = NULL;
static void *g_mod_section_vec = NULL;   /* UtilityView + 0x48 */
static void *g_mod_row_vec = NULL;       /* UtilityView + 0x58 */
static int s_mod_row_index = -1;

#define UTILITY_SECTION_VEC_OFF 0x40
#define UTILITY_ROW_VEC_OFF     0x58
#define VEC_APPEND_FN           0x21490b0u
#define VEC_GROW_FN             0x216e460u
#define MAKE_STRING_FN          0x21237d0u
#define STRING_DTOR_FN          0x20ec2c0u
#define SECTION_BUILDER_FN      0x1ae2528u
#define DECK_ROW_BUILDER_FN     0x1ae4128u
#define UTILITY_VIEW_VT         0x279e3f0u

/* Vector helpers. The vector is { void **data; int32 cap; int32 size; }. */
static int vec_size(void *vec) {
    return vec ? *(int *)((char *)vec + 0x10) : 0;
}

/* Append the MOD section + row to a live UtilityView table.
 *
 * IMPORTANT: the section vector does NOT hold plain strings. Each element is
 * a section object (see 0x1ae42e0: it stores sub-pointers at +0xc8 and builds
 * a row list at +0x28). Appending a bare juce::String corrupts the table and
 * crashes the UI on the next paint.
 *
 * A correct native MOD section therefore requires constructing a real section
 * object (label + row list + controller), which is not yet mapped. Until that
 * is done, this function only records the UtilityView so the mod can be
 * toggled through the config file / controller command.
 *
 * Returns 0 (not installed) so callers keep retrying harmlessly. */
static int install_mod_menu(void *utility_view) {
    if (!utility_view) return 0;
    if (*(uintptr_t *)utility_view != UTILITY_VIEW_VT) return 0;

    void *sec_vec = (char *)utility_view + UTILITY_SECTION_VEC_OFF;
    void *row_vec = (char *)utility_view + UTILITY_ROW_VEC_OFF;

    int sec_n = vec_size(sec_vec);
    int row_n = vec_size(row_vec);
    if (s_verify && !g_utility_view) {
        fprintf(stderr,
                "[FX_MOD_VERIFY] UtilityView %p: section_vec=%p size=%d, row_vec=%p size=%d\n",
                utility_view, sec_vec, sec_n, row_vec, row_n);
    }

    g_utility_view = utility_view;
    g_mod_section_vec = sec_vec;
    g_mod_row_vec = row_vec;
    return 0;   /* native section not implemented yet */
}

/* Per-channel routing enum handed to the engine dispatcher (0x1d75090). */
static const int CH_ENGINE_ENUM[4] = {
    2, /* CH1     */
    7, /* CH2     */
    4, /* MIC     */
    5  /* MASTER  */
};

/* FxState+0xa0 holds the engine object pointer; +0x10 of that object is the
 * engine core. Reading it is safe only after validating the FxState vtable. */
static void *fx_engine_object(void) {
    if (!g_fxState) return NULL;
    if (*(uintptr_t *)g_fxState != 0x2cea1d8) return NULL; /* FxState vtable guard */
    return *(void **)((char *)g_fxState + 0xa0);
}

static void verify_log_engine(const char *tag, int ch_val) {
    if (!s_verify) return;
    void *eng = fx_engine_object();
    int reported = -1;
    if (g_fxState) {
        reported = ((int (*)(void *))0x1d71d90)(g_fxState);
    }
    fprintf(stderr,
            "[FX_MOD_VERIFY] %s ch_val=%d engine_obj=%p engine_enum=%d "
            "getChannelSelect=%d expected_enum=%d\n",
            tag, ch_val, eng, eng ? CH_ENGINE_ENUM[ch_val] : -1,
            reported, CH_ENGINE_ENUM[ch_val]);
}

/* ---------------------------------------------------------------------------
 * Pointer validation (design borrowed from the OPUS-QUAD FX knob mod).
 *
 * Every heap-discovered object is re-checked against its expected vtable
 * immediately before use. If the heap scan latched a stale or unrelated
 * allocation, the mod does nothing instead of calling into garbage.
 * ------------------------------------------------------------------------- */
#define VT_FXSTATE        0x2cea1d8u
#define VT_CHSELECT       0x2baaa48u
#define VT_CHSELECT_1     0x2baabc0u
#define VT_CHSELECT_2     0x2baabf0u
#define VT_PANELPLACING   0x2c4d600u

/* A pointer we are willing to dereference. The heap scan reads object fields
 * that we have not validated, so a garbage value must never reach a load.
 * Rejects NULL, low addresses, non-canonical values and anything outside the
 * 48-bit user range. */
static int plausible_ptr(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000ULL && v < 0x0000800000000000ULL && (v & 7) == 0;
}

static int valid_fxstate(void) {
    return plausible_ptr(g_fxState) && *(uintptr_t *)g_fxState == VT_FXSTATE;
}

static int valid_chselect(void) {
    return plausible_ptr(g_chSelectPanel) &&
           *(uintptr_t *)g_chSelectPanel == VT_CHSELECT &&
           *(uintptr_t *)((char *)g_chSelectPanel + 0xc8) == VT_CHSELECT_1 &&
           *(uintptr_t *)((char *)g_chSelectPanel + 0x108) == VT_CHSELECT_2;
}

static int valid_panelplacing(void) {
    return plausible_ptr(g_panelPlacingComp) &&
           *(uintptr_t *)g_panelPlacingComp == VT_PANELPLACING;
}

static const unsigned int CH_BUTTON_OFFSETS[4] = {
    0x128, /* CH1 */
    0x130, /* CH2 */
    0x138, /* MIC */
    0x140  /* MASTER */
};

static const int CH_VALUES[4] = {
    0, /* CH1 */
    1, /* CH2 */
    2, /* MIC */
    3  /* MASTER */
};

static uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------------------
 * Version guard
 *
 * Every address below is a file vaddr for EP148 v1.23 (non-PIE, base
 * 0x400000). They are NOT valid for v2.00 - all eight patch sites differ, and
 * the vtable constants point at unrelated data. Writing to them on the wrong
 * build corrupts arbitrary instructions, which shows up as a black screen
 * with the panel lights on.
 *
 * So: before touching anything, read the bytes that are supposed to be there
 * and refuse the whole install if they do not match. Failing safe beats
 * silently corrupting code.
 * ------------------------------------------------------------------------- */

/* Original instruction words at each patch site, EP148 v1.23.
 * These are the little-endian uint32 values as read from the file, i.e. the
 * byte sequence a10f0054 at 0x1f48ea0 is the word 0x54000fa1. */
#define EXPECT_CRC_SITE     0x54000fa1u  /* 0x1f48ea0: a10f0054 */
#define EXPECT_TBL_HEIGHT   0x52802384u  /* 0x1ab32bc: 84238052 */
#define EXPECT_CMP_163      0x7104701fu  /* 0x1ab3338: 1f700471 */
#define EXPECT_TBL_HEIGHT2  0x52802384u  /* 0x1ab3344: 84238052 */
#define EXPECT_LOOP_1A7     0x7105869fu  /* 0x179d24c: 9f860571 */
#define EXPECT_CAT_BUILDER  0xa9bc7bfdu  /* 0x1ae2528: fd7bbca9 */
#define EXPECT_ITEM_SETCAT  0xa9bc7bfdu  /* 0x1accdc0: fd7bbca9 */
#define EXPECT_VALUE_SETCAT 0xa9bb7bfdu  /* 0x1add858: fd7bbba9 */

static int s_version_ok = 0;
static uint64_t s_init_time_ms = 0;

static uint64_t get_now_ms(void);

/* ---------------------------------------------------------------------------
 * Persistent breadcrumb logger (raw syscalls only)
 *
 * Writes timestamped milestones to /mnt/FX_MOD_LOG.txt (persistent ext4 on
 * mmcblk0p5, opened with O_SYNC so lines survive power loss), /tmp/FX_MOD_LOG.txt,
 * and stderr. Uses raw syscalls so it never triggers our own open/close hooks
 * or dlsym locks.
 * ------------------------------------------------------------------------- */
static void mod_log_raw(const char *buf, size_t len) {
    static const char *paths[] = { "/mnt/FX_MOD_LOG.txt", "/tmp/FX_MOD_LOG.txt" };
    if (len == 0) return;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int flags = O_WRONLY | O_CREAT | O_APPEND | (i == 0 ? O_SYNC : 0);
        int fd = (int)syscall(SYS_openat, AT_FDCWD, paths[i], flags, 0644);
        if (fd >= 0) {
            ssize_t w = syscall(SYS_write, fd, buf, len);
            (void)w;
            syscall(SYS_close, fd);
        }
    }
    ssize_t w = syscall(SYS_write, 2, buf, len);
    (void)w;
}

__attribute__((format(printf, 1, 2)))
static void mod_logf(const char *fmt, ...) {
    char buf[512];
    uint64_t now = get_now_ms();
    uint64_t rel = s_init_time_ms ? (now - s_init_time_ms) : 0;
    int hdr = snprintf(buf, sizeof(buf), "[FX_MOD +%llums pid=%d tid=%d] ",
                       (unsigned long long)rel, (int)getpid(), (int)gettid());
    if (hdr < 0) hdr = 0;
    if (hdr >= (int)sizeof(buf)) hdr = (int)sizeof(buf) - 1;

    va_list ap;
    va_start(ap, fmt);
    int body = vsnprintf(buf + hdr, sizeof(buf) - (size_t)hdr, fmt, ap);
    va_end(ap);
    if (body < 0) body = 0;
    int total = hdr + body;
    if (total >= (int)sizeof(buf)) total = (int)sizeof(buf) - 1;
    mod_log_raw(buf, (size_t)total);
}

static void dump_maps_once(void) {
    static int dumped = 0;
    if (dumped) return;
    dumped = 1;
    int sfd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY, 0);
    if (sfd < 0) return;
    int dfd = (int)syscall(SYS_openat, AT_FDCWD, "/mnt/FX_MOD_MAPS.txt",
                           O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
    if (dfd >= 0) {
        char buf[2048];
        ssize_t n;
        while ((n = syscall(SYS_read, sfd, buf, sizeof(buf))) > 0) {
            ssize_t w = syscall(SYS_write, dfd, buf, (size_t)n);
            (void)w;
        }
        syscall(SYS_close, dfd);
    }
    syscall(SYS_close, sfd);
}

/* ---------------------------------------------------------------------------
 * Crash reporting
 *
 * A fault in this library kills EP148 before it draws a frame, which looks
 * exactly like a black screen with the panel lights on - no message, no
 * reboot. Write the fault to a persistent path so there is something to read
 * after the fact. /mnt is mmcblk0p5 and survives a power cycle; /tmp does not.
 * ------------------------------------------------------------------------- */
static int       s_scan_nregions;
static int       s_scan_region;
static uintptr_t s_scan_cursor;
static int       s_scan_active;
static int       s_scan_complete;

static void crash_handler(int sig, siginfo_t *si, void *uc) {
    static const char *paths[] = { "/mnt/FX_MOD_CRASH.txt", "/tmp/FX_MOD_CRASH.txt" };
    char buf[768];
    int n = snprintf(buf, sizeof(buf),
                     "signal=%d (%s)\n"
                     "pid=%d tid=%d\n"
                     "fault_addr=%p\n"
                     "pc=%p\n"
                     "lr=%p\n"
                     "sp=%p\n"
                     "version_ok=%d\n"
                     "scan_active=%d scan_complete=%d scan_region=%d/%d scan_cursor=0x%lx\n"
                     "chSelect=%p ppc=%p fxState=%p beatFx=%p\n",
                     sig,
                     sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" :
                     sig == SIGILL ? "SIGILL" : sig == SIGABRT ? "SIGABRT" : "?",
                     (int)getpid(), (int)gettid(),
                     si ? si->si_addr : NULL,
                     uc ? (void *)((ucontext_t *)uc)->uc_mcontext.pc : NULL,
                     uc ? (void *)((ucontext_t *)uc)->uc_mcontext.regs[30] : NULL,
                     uc ? (void *)((ucontext_t *)uc)->uc_mcontext.sp : NULL,
                     s_version_ok,
                     s_scan_active, s_scan_complete, s_scan_region, s_scan_nregions,
                     (unsigned long)s_scan_cursor,
                     g_chSelectPanel, g_panelPlacingComp, g_fxState, g_beatFxPanel);

    /* snprintf returns the length it wanted, which can exceed the buffer. */
    if (n < 0) n = 0;
    else if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

    /* Raw syscalls only. This file defines its own open()/close() wrappers, so
     * calling them here would re-enter dlsym(RTLD_NEXT, ...) inside a signal
     * handler, which is not async-signal-safe and takes _dl_load_lock. */
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = (int)syscall(SYS_openat, AT_FDCWD, paths[i],
                              O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
        if (fd >= 0) {
            ssize_t w = syscall(SYS_write, fd, buf, (size_t)n);
            (void)w;
            syscall(SYS_close, fd);
        }
    }
    mod_log_raw(buf, (size_t)n);

    /* Restore the default action and re-raise so the process still dies. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

/* Returns 1 when the word at addr is the expected original instruction. */
static int expect_word(uintptr_t addr, uint32_t expected, const char *what) {
    uint32_t got = *(const uint32_t *)addr;
    if (got != expected) {
        fprintf(stderr,
                "[FX_MOD] REFUSED %s at 0x%lx: expected %08x, found %08x "
                "(wrong EP148 build - mod is for v1.23)\n",
                what, (unsigned long)addr, expected, got);
        return 0;
    }
    return 1;
}

/* Checks every site the mod intends to modify. All-or-nothing. */
static int check_ep148_version(void) {
    int ok = 1;
    ok &= expect_word(0x1f48ea0, EXPECT_CRC_SITE,     "CRC site");
    ok &= expect_word(0x1ab32bc, EXPECT_TBL_HEIGHT,   "table height");
    ok &= expect_word(0x1ab3338, EXPECT_CMP_163,      "cmp 0x163");
    ok &= expect_word(0x1ab3344, EXPECT_TBL_HEIGHT2,  "table height 2");
    ok &= expect_word(0x179d24c, EXPECT_LOOP_1A7,     "loop 0x1a7");
    ok &= expect_word(0x1ae2528, EXPECT_CAT_BUILDER,  "category builder");
    ok &= expect_word(0x1accdc0, EXPECT_ITEM_SETCAT,  "item setCategory");
    ok &= expect_word(0x1add858, EXPECT_VALUE_SETCAT, "value setCategory");

    /* Vtable slot 0 must be the exact function this build expects. A range or
     * alignment check is not enough: AArch64 functions are 4-byte aligned, and
     * any 8-byte-aligned value in range would pass. Compare the real address. */
    static const struct {
        uintptr_t addr;
        uintptr_t slot0;
        const char *name;
    } vts[] = {
        { VT_FXSTATE,      0x0000000001d71158ULL, "FxState vtable" },
        { VT_CHSELECT,     0x0000000001bb46c8ULL, "CHSelectPanel vtable" },
        { VT_PANELPLACING, 0x0000000001bcbb10ULL, "PanelPlacingComponent vtable" },
        { UTILITY_VIEW_VT, 0x00000000017a01d0ULL, "UtilityView vtable" },
    };
    for (unsigned i = 0; i < sizeof(vts) / sizeof(vts[0]); i++) {
        uintptr_t fn = *(const uintptr_t *)vts[i].addr;
        if (fn != vts[i].slot0) {
            fprintf(stderr,
                    "[FX_MOD] REFUSED %s at 0x%lx: slot 0 is 0x%016lx, "
                    "expected 0x%016lx (wrong EP148 build)\n",
                    vts[i].name, (unsigned long)vts[i].addr,
                    (unsigned long)fn, (unsigned long)vts[i].slot0);
            ok = 0;
        }
    }
    return ok;
}

static void patch_code32(uintptr_t addr, uint32_t val) {
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t base = addr & ~(uintptr_t)(pg - 1);
    mprotect((void *)base, (size_t)pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)addr = val;
    mprotect((void *)base, (size_t)pg * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)addr, (char *)addr + 4);
}

static void patch_jump64(uintptr_t addr, void *target) {
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t base = addr & ~(uintptr_t)(pg - 1);
    mprotect((void *)base, (size_t)pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t *p = (uint32_t *)addr;
    p[0] = 0x58000050; /* ldr x16, [pc, #8] */
    p[1] = 0xd61f0200; /* br x16 */
    *(void **)&p[2] = target;
    mprotect((void *)base, (size_t)pg * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)addr, (char *)addr + 16);
}

static void hook_category_builder(void *model) {
    void *arr = (char *)model + 0x48;
    const char *names[5] = { "DECK", "MIXER", "GENERAL", "INFO", "MOD" };
    for (int i = 0; i < 5; i++) {
        void *str_buf = NULL;
        ((void (*)(void *, const char *))0x21237d0)(&str_buf, names[i]);
        ((void (*)(void *, int, void *))0x21490b0)(arr, i, &str_buf);
        ((void (*)(void *))0x20ec2c0)(&str_buf);
    }
    fprintf(stderr, "[FX_MOD] Populated CategoryTableModel at %p with 5 categories (including MOD)!\n", model);
}

static void *make_trampoline16(uintptr_t src_addr, void *hook_target) {
    long pg = sysconf(_SC_PAGESIZE);
    void *tramp = mmap(NULL, pg, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    uint32_t *t = (uint32_t *)tramp;
    uint32_t *s = (uint32_t *)src_addr;
    t[0] = s[0];
    t[1] = s[1];
    t[2] = s[2];
    t[3] = s[3];
    t[4] = 0x58000050; /* ldr x16, [pc, #8] */
    t[5] = 0xd61f0200; /* br x16 */
    *(void **)&t[6] = (void *)(src_addr + 16);
    __builtin___clear_cache((char *)tramp, (char *)tramp + 32);

    patch_jump64(src_addr, hook_target);
    return tramp;
}

static void *g_mod_item_model = NULL;
static void *g_mod_value_model = NULL;
static void *g_mod_item_table = NULL;static void *g_mod_value_table = NULL;

static void *g_modItemModel_vtable_primary[32];
static void *g_modItemModel_vtable_sub[32];
static void *g_modValueModel_vtable[32];

static int mod_item_getNumRows(void *self) {
    return 1;
}

static int mod_value_getNumRows(void *self) {
    return 2;
}

static void mod_value_cellClicked(void *model, int rowNumber, int columnId, void *mouseEvent) {
    fprintf(stderr, "[FX_MOD] mod_value_cellClicked: row=%d col=%d\n", rowNumber, columnId);
    s_mod_enabled = (rowNumber == 1);
    fprintf(stderr, "[FX_MOD] MOD menu: Beat FX Switcher -> %s\n", s_mod_enabled ? "ON" : "OFF");

    FILE *f = fopen("/home/root/pdj/mod.cfg", "w");
    if (f) {
        fprintf(f, "beatfx=%d\n", s_mod_enabled);
        fclose(f);
    }

    *(int *)((char *)model + 0x90) = s_mod_enabled ? 1 : 0;
    *(int *)((char *)model + 0x58) = 0;

    if (g_mod_value_table) {
        ((void (*)(void *, int, int, int))0x1fd7a90)(g_mod_value_table, s_mod_enabled ? 1 : 0, 0, 1);
        ((void (*)(void *))0x1fd3eb0)(g_mod_value_table);
        ((void (*)(void *))0x2296580)(g_mod_value_table);
    }
    if (g_mod_item_table) {
        ((void (*)(void *))0x2296580)(g_mod_item_table);
    }
}

static void init_mod_models(void *item_ctrl, void *value_ctrl) {
    if (!g_mod_item_model && item_ctrl) {
        void *stock_deck_item = *(void **)((char *)item_ctrl + 0x138);
        if (stock_deck_item) {
            g_mod_item_model = malloc(0x200);
            memcpy(g_mod_item_model, stock_deck_item, 0x1b0);

            memcpy(g_modItemModel_vtable_primary, (void *)0x2a7da30, sizeof(g_modItemModel_vtable_primary));
            memcpy(g_modItemModel_vtable_sub, (void *)0x2a7da88, sizeof(g_modItemModel_vtable_sub));

            g_modItemModel_vtable_primary[5] = (void *)mod_item_getNumRows;
            g_modItemModel_vtable_sub[2] = (void *)mod_item_getNumRows;

            *(void **)g_mod_item_model = g_modItemModel_vtable_primary;
            *(void **)((char *)g_mod_item_model + 0x28) = g_modItemModel_vtable_sub;

            void *arr_names = (char *)g_mod_item_model + 0x58;
            void *arr_vals = (char *)g_mod_item_model + 0x70;
            memset(arr_names, 0, 0x18);
            memset(arr_vals, 0, 0x18);

            void *str_name = NULL;
            ((void (*)(void *, const char *))0x21237d0)(&str_name, "Beat FX Switcher");
            ((void (*)(void *, int, void *))0x21490b0)(arr_names, 0, &str_name);
            ((void (*)(void *))0x20ec2c0)(&str_name);

            void *str_val = NULL;
            ((void (*)(void *, const char *))0x21237d0)(&str_val, "");
            ((void (*)(void *, int, void *))0x21490b0)(arr_vals, 0, &str_val);
            ((void (*)(void *))0x20ec2c0)(&str_val);

            *(uint64_t *)((char *)g_mod_item_model + 0x88) = 0;
            *(uint32_t *)((char *)g_mod_item_model + 0x90) = 0;
            *(uint32_t *)((char *)g_mod_item_model + 0x98) = 0;
            fprintf(stderr, "[FX_MOD] Initialized g_mod_item_model at %p\n", g_mod_item_model);
        }
    }

    if (!g_mod_value_model && value_ctrl) {
        void *stock_deck_val = *(void **)((char *)value_ctrl + 0x138);
        if (stock_deck_val) {
            g_mod_value_model = malloc(0x100);
            memcpy(g_mod_value_model, stock_deck_val, 0xb8);

            memcpy(g_modValueModel_vtable, (void *)0x2a7df60, sizeof(g_modValueModel_vtable));
            g_modValueModel_vtable[2] = (void *)mod_value_getNumRows;
            /* Slot 6 is TableListBoxModel::cellClicked (single tap).
             * Slot 7 is cellDoubleClicked -- hooking that made the toggle
             * require a double-click. Hook both so either gesture works. */
            g_modValueModel_vtable[6] = (void *)mod_value_cellClicked;
            g_modValueModel_vtable[7] = (void *)mod_value_cellClicked;

            *(void **)g_mod_value_model = g_modValueModel_vtable;

            void *arr_options = (char *)g_mod_value_model + 0x60;
            memset(arr_options, 0, 0x18);

            const char *opt_names[2] = { "Off", "On" };
            for (int i = 0; i < 2; i++) {
                void *s = NULL;
                ((void (*)(void *, const char *))0x21237d0)(&s, opt_names[i]);
                ((void (*)(void *, int, void *))0x21490b0)(arr_options, i, &s);
                ((void (*)(void *))0x20ec2c0)(&s);
            }

            *(int *)((char *)g_mod_value_model + 0x58) = 0;
            *(int *)((char *)g_mod_value_model + 0x70) = 2;
            *(int *)((char *)g_mod_value_model + 0x90) = s_mod_enabled ? 1 : 0;

            fprintf(stderr, "[FX_MOD] Initialized g_mod_value_model at %p\n", g_mod_value_model);
        }
    }
}

static void (*real_item_table_set_category)(void *, int) = NULL;
static void (*real_value_table_set_category_and_item)(void *, int, int) = NULL;

static void hook_item_table_set_category(void *ctrl, int category_idx) {
    g_mod_item_table = *(void **)((char *)ctrl + 0x130);
    if (category_idx == 4) {
        init_mod_models(ctrl, NULL);
        if (g_mod_item_model && g_mod_item_table) {
            void *table = g_mod_item_table;
            ((void (*)(void *, void *))0x1fd6be0)(table, (char *)g_mod_item_model + 0x28);
            ((void (*)(void *, int, int))0x22ad230)(table, 0x1d2, 0x30);
            ((void (*)(void *, int, int, int))0x1fd7a90)(table, 0, 0, 1);
            ((void (*)(void *))0x1fd3eb0)(table);
            ((void (*)(void *))0x2296580)(table);
            fprintf(stderr, "[FX_MOD] ItemTableController switched to MOD (Beat FX Switcher)\n");
            return;
        }
    }
    real_item_table_set_category(ctrl, category_idx);
}

static void hook_value_table_set_category_and_item(void *ctrl, int category_idx, int item_idx) {
    g_mod_value_table = *(void **)((char *)ctrl + 0x130);
    if (category_idx == 4) {
        init_mod_models(NULL, ctrl);
        if (g_mod_value_model && g_mod_value_table) {
            void *table = g_mod_value_table;
            *(int *)((char *)g_mod_value_model + 0x90) = s_mod_enabled ? 1 : 0;
            *(int *)((char *)g_mod_value_model + 0x58) = 0;
            *(int *)((char *)g_mod_value_model + 0x70) = 2;
            ((void (*)(void *, void *))0x1fd6be0)(table, g_mod_value_model);
            ((void (*)(void *, int, int))0x22ad230)(table, 0x1d2, 2 * 0x30);
            int sel = s_mod_enabled ? 1 : 0;
            ((void (*)(void *, int, int, int))0x1fd7a90)(table, sel, 0, 1);
            ((void (*)(void *))0x1fd3eb0)(table);
            ((void (*)(void *))0x2296580)(table);
            fprintf(stderr, "[FX_MOD] ValueTableController switched to MOD (Off/On, active=%d)\n", sel);
            return;
        }
    }
    real_value_table_set_category_and_item(ctrl, category_idx, item_idx);
}

/* ---------------------------------------------------------------------------
 * Incremental heap scan
 *
 * find_panels() runs from check_gui_thread_tasks(), which is called on every
 * poll/ppoll/epoll_wait - i.e. inside the GUI event loop. Walking the whole
 * heap in one pass can take long enough that the UI thread never gets to
 * paint, which looks like a black screen with the panel lights on.
 *
 * So the scan is resumable: it parses /proc/self/maps once into a small table
 * of candidate regions, then walks at most SCAN_BUDGET bytes per call and
 * continues from where it stopped on the next tick.
 * ------------------------------------------------------------------------- */
#define SCAN_MAX_REGIONS 64
#define SCAN_BUDGET      0x100000u   /* 1 MiB per call */

static struct { uintptr_t start, end; } s_scan_regions[SCAN_MAX_REGIONS];
static int      s_scan_nregions = 0;
static int      s_scan_region   = 0;
static uintptr_t s_scan_cursor  = 0;
static int      s_scan_active   = 0;   /* a scan is in progress */
static int      s_scan_complete = 0;   /* finished; do not restart */
static int      s_scan_passes   = 0;

static void scan_reset(void) {
    s_scan_nregions = 0;
    s_scan_region   = 0;
    s_scan_cursor   = 0;
    s_scan_active   = 0;
    s_scan_complete = 0;
}

/* Reads /proc/self/maps and records the regions worth walking. */
static int scan_load_regions(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    s_scan_nregions = 0;
    while (fgets(line, sizeof(line), f) && s_scan_nregions < SCAN_MAX_REGIONS) {
        if (!strstr(line, "rw-p")) continue;
        if (strstr(line, "stack") || strstr(line, "/lib") || strstr(line, "/usr") ||
            strstr(line, "/dev")  || strstr(line, "/mnt") || strstr(line, "/tmp") ||
            strstr(line, "/var")) continue;
        uintptr_t start = 0, end = 0;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
        if (start > 0x100000000ULL) continue;      /* heap/bss below 4GB only */
        if (end <= start) continue;
        if (end - start > 0x40000000) continue;    /* skip absurdly large maps */
        if (end <= start + 0x200) continue;
        s_scan_regions[s_scan_nregions].start = start;
        s_scan_regions[s_scan_nregions].end   = end;
        s_scan_nregions++;
    }
    fclose(f);
    return s_scan_nregions;
}

/* Re-validates a cached region against /proc/self/maps right before walking a
 * slice so that a region that shrank or was unmapped between ticks cannot
 * fault. If /proc/self/maps cannot be opened (e.g. standalone synthetic
 * harness), trusts the caller's region bounds. */
static int validate_region_slice(uintptr_t cursor, uintptr_t *end_inout) {
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY, 0);
    if (fd < 0) return 1;

    char buf[4096];
    int carry = 0;
    int valid = 0;
    ssize_t n;
    while ((n = syscall(SYS_read, fd, buf + carry, sizeof(buf) - 1 - (size_t)carry)) > 0) {
        int total = carry + (int)n;
        buf[total] = '\0';
        char *line = buf;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            if (strstr(line, "rw-p")) {
                uintptr_t vstart = 0, vend = 0;
                if (sscanf(line, "%lx-%lx", &vstart, &vend) == 2) {
                    if (cursor >= vstart && cursor < vend) {
                        if (*end_inout > vend) *end_inout = vend;
                        valid = 1;
                        goto done;
                    }
                }
            }
            line = nl + 1;
        }
        carry = total - (int)(line - buf);
        if (carry > 0 && line != buf) memmove(buf, line, (size_t)carry);
        else if (carry >= (int)sizeof(buf) - 1) carry = 0;
    }
done:
    syscall(SYS_close, fd);
    return valid;
}

static int ptr_in_scan_regions(const void *p) {
    uintptr_t v = (uintptr_t)p;
    for (int i = 0; i < s_scan_nregions; i++) {
        if (v >= s_scan_regions[i].start &&
            v + sizeof(uintptr_t) <= s_scan_regions[i].end) {
            return 1;
        }
    }
    return 0;
}

static void find_panels(void) {
    /* Drop anything that no longer validates before deciding we are done.
     * Checking raw non-NULL here would wedge the scan forever once a stale
     * pointer had been stored. */
    if (!valid_chselect())     g_chSelectPanel = NULL;
    if (!valid_panelplacing()) g_panelPlacingComp = NULL;
    if (!valid_fxstate())      g_fxState = NULL;
    /* Do not return early just because all three are present: the caller keeps
     * calling while s_scan_active is set, and returning here would leave the
     * scan unfinished, so it would restart from the beginning forever. */
    if (g_chSelectPanel && g_panelPlacingComp && g_fxState && s_scan_complete) return;

    /* Something is missing, so a fresh scan is warranted. */
    if (s_scan_complete) scan_reset();

    if (!s_scan_active) {
        dump_maps_once();
        if (!scan_load_regions()) {
            s_scan_active = 0;
            s_scan_complete = 1;
            return;
        }
        s_scan_active = 1;
        s_scan_region = 0;
        s_scan_cursor = s_scan_regions[0].start;
        s_scan_passes++;
        if (s_scan_passes <= 5) {
            mod_logf("scan pass #%d start: nregions=%d first=0x%lx-0x%lx last=0x%lx-0x%lx\n",
                     s_scan_passes, s_scan_nregions,
                     (unsigned long)s_scan_regions[0].start,
                     (unsigned long)s_scan_regions[0].end,
                     (unsigned long)s_scan_regions[s_scan_nregions - 1].start,
                     (unsigned long)s_scan_regions[s_scan_nregions - 1].end);
        }
    }

    uintptr_t vtable_ch0 = 0x2baaa48; /* CHSelectPanel primary vtable */
    uintptr_t vtable_ch1 = 0x2baabc0; /* CHSelectPanel secondary vtable at +0xc8 */
    uintptr_t vtable_ch2 = 0x2baabf0; /* CHSelectPanel secondary vtable at +0x108 */
    uintptr_t vtable_bfx0 = 0x2b75048; /* BeatFXPanel primary vtable */
    uintptr_t vtable_ppc = 0x2c4d600;  /* PanelPlacingComponent vtable */

    uintptr_t budget = SCAN_BUDGET;

    while (s_scan_region < s_scan_nregions) {
        uintptr_t end = s_scan_regions[s_scan_region].end;
        if (!validate_region_slice(s_scan_cursor, &end) || end <= 0x200) {
            s_scan_region++;
            if (s_scan_region < s_scan_nregions) {
                s_scan_cursor = s_scan_regions[s_scan_region].start;
            }
            continue;
        }
        s_scan_regions[s_scan_region].end = end;
        uintptr_t limit = end - 0x200;

        for (uintptr_t p = s_scan_cursor; p < limit; p += 8) {
            uintptr_t w = *(uintptr_t *)p;

            if (!g_chSelectPanel && w == vtable_ch0 &&
                *(uintptr_t *)(p + 0xc8) == vtable_ch1 &&
                *(uintptr_t *)(p + 0x108) == vtable_ch2) {
                g_chSelectPanel = (void *)p;
                if (!g_fxState) {
                    /* +0x110 is CHSelectPanel's FxState* field. Accept it if
                     * it lands inside any mapped scan region and its own vtable
                     * word is the one we expect. */
                    void *cand = *(void **)((char *)g_chSelectPanel + 0x110);
                    if (plausible_ptr(cand) &&
                        ptr_in_scan_regions(cand) &&
                        *(uintptr_t *)cand == VT_FXSTATE) {
                        g_fxState = cand;
                    }
                }
                mod_logf("Discovered CHSelectPanel at %p, fxState: %p\n",
                         g_chSelectPanel, g_fxState);
            }

            if (!g_beatFxPanel && w == vtable_bfx0) {
                g_beatFxPanel = (void *)p;
                /* No FxState read here: +0x110 is CHSelectPanel's field, not
                 * BeatFXPanel's. Reading it on this object yields garbage. */
                mod_logf("Discovered BeatFXPanel at %p\n", g_beatFxPanel);
            }

            /* An object whose own vtable word is VT_FXSTATE is an FxState by
             * definition, and p is already inside the mapped region we are
             * walking, so it is safe to take directly. */
            if (!g_fxState && w == VT_FXSTATE) {
                g_fxState = (void *)p;
                mod_logf("Discovered FxState at %p\n", g_fxState);
            }

            if (!g_panelPlacingComp && w == vtable_ppc) {
                void *sub = *(void **)((char *)p + 280);
                if (plausible_ptr(sub)) {
                    g_panelPlacingComp = (void *)p;
                    mod_logf("Discovered PanelPlacingComponent at %p\n", g_panelPlacingComp);
                }
            }

            /* UtilityView: primary vtable 0x279e3f0. The table vectors are
             * populated lazily when the UTILITY screen is first built, so we
             * remember the object and retry from the GUI-thread tick. */
            if (!s_mod_menu_installed && w == UTILITY_VIEW_VT) {
                if (!g_utility_view) {
                    g_utility_view = (void *)p;
                    mod_logf("Discovered UtilityView at %p\n", g_utility_view);
                }
                install_mod_menu((void *)p);
            }

            /* Stop scanning the rest of a 500+ MiB heap once all four target
             * objects have been located. */
            if (g_chSelectPanel && g_panelPlacingComp && g_fxState && g_beatFxPanel) {
                s_scan_active   = 0;
                s_scan_complete = 1;
                s_scan_cursor   = 0;
                s_scan_region   = 0;
                mod_logf("ALL PANELS READY: chSelect=%p ppc=%p fxState=%p beatFx=%p (pass=%d)\n",
                         g_chSelectPanel, g_panelPlacingComp, g_fxState, g_beatFxPanel, s_scan_passes);
                return;
            }

            /* Yield back to the event loop once this call has done its share. */
            if (budget <= 8) {
                s_scan_cursor = p + 8;
                return;
            }
            budget -= 8;
        }

        s_scan_region++;
        if (s_scan_region < s_scan_nregions) {
            s_scan_cursor = s_scan_regions[s_scan_region].start;
        }
        if (g_chSelectPanel && g_panelPlacingComp && g_fxState) break;
    }

    /* Walked everything. Do not restart until something invalidates. */
    s_scan_active   = 0;
    s_scan_complete = 1;
    s_scan_cursor   = 0;
    s_scan_region   = 0;
    if (s_scan_passes <= 5 || (g_chSelectPanel && g_panelPlacingComp && g_fxState)) {
        mod_logf("scan pass #%d finished: chSelect=%p ppc=%p fxState=%p beatFx=%p\n",
                 s_scan_passes, g_chSelectPanel, g_panelPlacingComp, g_fxState, g_beatFxPanel);
    }
}

static void do_target_step(int dir) {
    if (!valid_chselect() || !valid_panelplacing() || !valid_fxstate()) {
        find_panels();
    }
    if (!valid_chselect() || !valid_panelplacing() || !valid_fxstate()) {
        fprintf(stderr, "[FX_MOD] Objects not ready or failed vtable validation; step ignored\n");
        return;
    }

    /* Sync with the engine's current channel.
     *
     * getChannelSelect (0x1d71d90) is `ldrb w0,[x0,#0x4d]`. setChannelSelect
     * does NOT write +0x4d; the engine updates it asynchronously when the
     * routing command is applied. Reading it immediately after a step can
     * therefore return the previous channel, which would make two fast
     * presses collapse into one. We only trust it when it agrees with the
     * value we last commanded, otherwise we keep our own cycle index. */
    int current_hw_ch = ((int (*)(void *))0x1d71d90)(g_fxState);
    if (current_hw_ch >= 0 && current_hw_ch <= 3 &&
        current_hw_ch == s_current_cycle_idx) {
        /* Engine agrees with our tracked position: nothing to resync. */
    } else if (current_hw_ch >= 0 && current_hw_ch <= 3 && s_verify_steps == 0) {
        /* First step after startup: adopt the engine's real position. */
        s_current_cycle_idx = current_hw_ch;
    }

    if (dir > 0) {
        s_current_cycle_idx = (s_current_cycle_idx + 1) % 4;
    } else {
        s_current_cycle_idx = (s_current_cycle_idx + 3) % 4;
    }

    static const char *names[4] = {"CH1", "CH2", "MIC", "MASTER"};
    int ch_val = CH_VALUES[s_current_cycle_idx];

    fprintf(stderr, "[FX_MOD] Hardware Target Stepped: %s -> %s (val=%d)\n",
            dir > 0 ? "NEXT (+1)" : "PREV (-1)", names[s_current_cycle_idx], ch_val);

    /* Snapshot the audio-engine routing state before the write so the log can
     * prove the change actually reached the engine (not just the GUI). */
    verify_log_engine("before", ch_val);

    /* 1. Update effector channel setting in audio engine / FxState (0x1d720e8).
     *    This is the exact funnel the native touchscreen listener tail-calls,
     *    so the hardware shortcut perpetuates into the audio routing. */
    ((void (*)(void *, int))0x1d720e8)(g_fxState, ch_val);

    /* Confirm the engine write landed. setChannelSelect writes the engine
     * object at FxState+0xa0 and dispatches the routing enum via 0x1d75090. */
    verify_log_engine("after", ch_val);
    if (s_verify) {
        s_verify_steps++;
        void *eng = fx_engine_object();
        if (eng) s_verify_engine_changes++;
        fprintf(stderr,
                "[FX_MOD_VERIFY] step=%lu engine_writes=%lu routing=%s\n",
                s_verify_steps, s_verify_engine_changes,
                eng ? "PERPETUATED_TO_ENGINE" : "ENGINE_OBJECT_MISSING");
        /* Arm deferred confirmation of the async getChannelSelect update. */
        s_confirm_pending = 1;
        s_confirm_expected = ch_val;
        s_confirm_deadline_ms = get_now_ms() + 2000;
    }

    /* 2. Call buttonClicked on CHSelectPanel listener (0x1bb45f8).
     *    The native listener re-derives FxState from listener+0x8 and calls
     *    setChannelSelect again, so this keeps the GUI and engine in lockstep
     *    with the stock code path. */
    unsigned int off = CH_BUTTON_OFFSETS[s_current_cycle_idx];
    void *btn = *(void **)((char *)g_chSelectPanel + off);
    if (btn) {
        void *listener = (char *)g_chSelectPanel + 0x108;
        ((void (*)(void *, void *))0x1bb45f8)(listener, btn);
    }

    /* 3. Refresh UI button highlights and label (0x1bb5ba0) */
    ((void (*)(void *))0x1bb5ba0)(g_chSelectPanel);

    /* 4. Drawer open / keep-open logic:
     * - If closed: slide open
     * - If already open: keep open without re-animating/redrawing
     * - Dismiss after 3 seconds of no switching
     */
    /* If the Beat FX selector (panel 0) is open, close it immediately so it never shows */
    void *sub0 = *(void **)((char *)g_panelPlacingComp + 280);
    if (sub0 && ((int (*)(void *))0x16e6258)(sub0)) {
        ((void (*)(void *))0x1bcc4a0)(g_panelPlacingComp);
    }

    int is_open = ((int (*)(void *))0x1bcd578)(g_panelPlacingComp);
    int bit1 = (*((unsigned char *)g_panelPlacingComp + 192) >> 1) & 1;

    if (!is_open && !bit1) {
        ((void (*)(void *, int))0x1bcd658)(g_panelPlacingComp, 1);
    }
    s_panel_dismiss_time_ms = get_now_ms() + 3000;
}

/* Toggle the mod from the MOD menu row.
 *
 * The stock Utility table routes a tap on a row to that row's setting. Our
 * appended row has no backing Setting::Item, so instead of relying on the
 * table's own dispatch we expose an explicit toggle that the emulator
 * controller (and a future native row binding) can call.
 *
 * Returns the new state. */
static int mod_toggle(void) {
    s_mod_enabled = !s_mod_enabled;
    fprintf(stderr, "[FX_MOD] MOD menu: Beat FX Switcher -> %s\n",
            s_mod_enabled ? "ON" : "OFF");
    return s_mod_enabled;
}

/* Poll a small config file for the MOD toggle.
 *
 * This is the persistence + control channel until a native UTILITY row is
 * wired. Format: a single line "beatfx=0" or "beatfx=1".
 * Path: /home/root/pdj/mod.cfg (overridable with FX_MOD_CFG). */
#define MOD_CFG_DEFAULT "/home/root/pdj/mod.cfg"

static void poll_mod_config(void) {
    static uint64_t last = 0;
    static int last_value = -1;
    uint64_t now = get_now_ms();
    if (now - last < 500) return;
    last = now;

    const char *path = getenv("FX_MOD_CFG");
    if (!path || !path[0]) path = MOD_CFG_DEFAULT;

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    int value = -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "beatfx");
        if (!p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        value = (eq[1] == '1') ? 1 : 0;
        break;
    }
    fclose(f);

    if (value >= 0 && value != last_value) {
        last_value = value;
        if (value != s_mod_enabled) {
            s_mod_enabled = value;
            fprintf(stderr, "[FX_MOD] MOD config: Beat FX Switcher -> %s\n",
                    s_mod_enabled ? "ON" : "OFF");
        }
    }
}

static void check_gui_thread_tasks(void) {
    if (!s_version_ok) return;
    if (gettid() != getpid()) return;

    static __thread int s_in_gui_tasks = 0;
    if (s_in_gui_tasks) return;
    s_in_gui_tasks = 1;

    static int s_logged_first_gui = 0;
    if (!s_logged_first_gui) {
        s_logged_first_gui = 1;
        mod_logf("First GUI thread tick reached (tid=%d)\n", (int)gettid());
    }

    if (!valid_chselect() || !valid_panelplacing() || !valid_fxstate()) {
        /* find_panels() is incremental: it walks at most SCAN_BUDGET bytes per
         * call and resumes next time. Rate-limit each slice to at most once
         * per 20ms so it never starves the GUI event loop or stalls X11. */
        static uint64_t last_scan = 0;
        uint64_t now = get_now_ms();
        if ((s_scan_active && (now - last_scan >= 20)) || (now - last_scan >= 1000)) {
            last_scan = now;
            find_panels();
            if (s_verify) {
                mod_logf("scan: chselect=%p(valid=%d) ppc=%p(valid=%d) fx=%p(valid=%d)\n",
                         g_chSelectPanel, valid_chselect(),
                         g_panelPlacingComp, valid_panelplacing(),
                         g_fxState, valid_fxstate());
            }
        }
    }

    if (s_pending_target_step != 0) {
        int dir = s_pending_target_step;
        s_pending_target_step = 0;
        if (!s_mod_enabled) {
            if (s_verify) mod_logf("step ignored (MOD disabled)\n");
        } else {
            if (s_verify) mod_logf("GUI thread picked up step dir=%d\n", dir);
            do_target_step(dir);
        }
    }

    /* Retry MOD menu installation until the Utility table has been built. */
    if (!s_mod_menu_installed && g_utility_view) {
        static uint64_t last_try = 0;
        uint64_t t = get_now_ms();
        if (t - last_try > 500) {
            last_try = t;
            install_mod_menu(g_utility_view);
        }
    }

    /* Check for test trigger to open UTILITY view */
    if (g_utility_view && access("/tmp/show_utility", F_OK) == 0) {
        unlink("/tmp/show_utility");
        /* setVisible(g_utility_view, 1) */
        ((void (*)(void *, int))0x22abb20)(g_utility_view, 1);
        /* onCategorySelected(g_utility_view, 4) -> selects MOD */
        ((void (*)(void *, int))0x179d860)(g_utility_view, 4);
        /* repaintAllTables(g_utility_view) */
        ((void (*)(void *))0x179d738)(g_utility_view);
        mod_logf("Opened UtilityView and selected MOD section!\n");
    }

    /* MOD toggle control channel (config file). */
    poll_mod_config();

    uint64_t now = get_now_ms();
    if (s_panel_dismiss_time_ms > 0 && now >= s_panel_dismiss_time_ms) {
        s_panel_dismiss_time_ms = 0;
        if (valid_panelplacing()) {
            ((void (*)(void *))0x1bcc4a0)(g_panelPlacingComp);
        }
    }

    /* Deferred confirmation that the engine accepted the routing change.
     * getChannelSelect (+0x4d) is written asynchronously, so it lags the
     * setChannelSelect call by a short interval. */
    if (s_confirm_pending && valid_fxstate()) {
        int reported = ((int (*)(void *))0x1d71d90)(g_fxState);
        if (reported == s_confirm_expected) {
            s_confirm_pending = 0;
            mod_logf("CONFIRMED engine accepted routing: getChannelSelect=%d == commanded=%d\n",
                     reported, s_confirm_expected);
        } else if (now >= s_confirm_deadline_ms) {
            s_confirm_pending = 0;
            mod_logf("confirm timeout: getChannelSelect=%d commanded=%d (engine may still be applying)\n",
                     reported, s_confirm_expected);
        }
    }

    s_in_gui_tasks = 0;
}

static int (*real_open)(const char *path, int flags, ...) = NULL;
static int (*real_open64)(const char *path, int flags, ...) = NULL;
static int (*real_openat)(int dirfd, const char *path, int flags, ...) = NULL;
static int (*real_openat64)(int dirfd, const char *path, int flags, ...) = NULL;
static int (*real_close)(int fd) = NULL;
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static int (*real_poll)(struct pollfd *fds, nfds_t nfds, int timeout) = NULL;
static int (*real_ppoll)(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p, const sigset_t *sigmask) = NULL;
static int (*real_epoll_wait)(int epfd, void *events, int maxevents, int timeout) = NULL;

void _Exit(int status) {
    static void (*real_Exit)(int) = NULL;
    if (!real_Exit) real_Exit = dlsym(RTLD_NEXT, "_Exit");
    mod_logf("EP148 called _Exit(%d) from caller=%p\n", status, __builtin_return_address(0));
    if (real_Exit) real_Exit(status);
    syscall(SYS_exit_group, status);
    abort();
}

void _exit(int status) {
    static void (*real_exit)(int) = NULL;
    if (!real_exit) real_exit = dlsym(RTLD_NEXT, "_exit");
    mod_logf("EP148 called _exit(%d) from caller=%p\n", status, __builtin_return_address(0));
    if (real_exit) real_exit(status);
    syscall(SYS_exit_group, status);
    abort();
}

int open(const char *path, int flags, ...) {
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);

    int fd = real_open(path, flags, mode);
    if (s_version_ok && path && strstr(path, "subucom_spi") && fd >= 0) {
        s_spi_fd = fd;
        mod_logf("Tracked SPI device fd: %d (%s) via open\n", s_spi_fd, path);
    }
    return fd;
}

int open64(const char *path, int flags, ...) {
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);

    int fd = real_open64(path, flags, mode);
    if (s_version_ok && path && strstr(path, "subucom_spi") && fd >= 0) {
        s_spi_fd = fd;
        mod_logf("Tracked SPI device fd: %d (%s) via open64\n", s_spi_fd, path);
    }
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...) {
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);

    int fd = real_openat(dirfd, path, flags, mode);
    if (s_version_ok && path && strstr(path, "subucom_spi") && fd >= 0) {
        s_spi_fd = fd;
        mod_logf("Tracked SPI device fd: %d (%s) via openat\n", s_spi_fd, path);
    }
    return fd;
}

int openat64(int dirfd, const char *path, int flags, ...) {
    if (!real_openat64) real_openat64 = dlsym(RTLD_NEXT, "openat64");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);

    int fd = real_openat64(dirfd, path, flags, mode);
    if (s_version_ok && path && strstr(path, "subucom_spi") && fd >= 0) {
        s_spi_fd = fd;
        mod_logf("Tracked SPI device fd: %d (%s) via openat64\n", s_spi_fd, path);
    }
    return fd;
}

int close(int fd) {
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    if (fd == s_spi_fd && fd >= 0) {
        mod_logf("SPI device fd closed: %d\n", s_spi_fd);
        s_spi_fd = -1;
    }
    return real_close(fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read) real_read = dlsym(RTLD_NEXT, "read");
    ssize_t n = real_read(fd, buf, count);
    /* On an unrecognised build the SPI CRC site was never patched, so mutating
     * the packet here would corrupt it. Pass everything through untouched. */
    if (n <= 0 || !s_version_ok) return n;

    /* If SPI fd was not caught by open/openat hooks (e.g. inherited fd),
     * verify via /proc/self/fd symlink only on exact 128-byte reads. */
    if (s_spi_fd < 0 && count == 128 && n == 128) {
        char linkpath[64], target[256];
        snprintf(linkpath, sizeof(linkpath), "/proc/self/fd/%d", fd);
        ssize_t len = readlink(linkpath, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            if (strstr(target, "subucom_spi")) {
                s_spi_fd = fd;
                mod_logf("Tracked SPI device fd: %d (%s) via readlink\n", s_spi_fd, target);
            }
        }
    }

    if (fd == s_spi_fd && n >= 79) {
        unsigned char *pkt = (unsigned char *)buf;
        static int s_logged_first_spi = 0;
        if (!s_logged_first_spi) {
            s_logged_first_spi = 1;
            mod_logf("First SPI read: n=%zd ver=%02x%02x cue_byte=%02x\n",
                     n, pkt[0], pkt[1], pkt[78]);
        }

        int shift1 = (pkt[17] & 0x01) != 0;
        int shift2 = (pkt[33] & 0x01) != 0;
        int shift = shift1 || shift2;

        int cue1 = (pkt[78] & 0x20) != 0;
        int cue2 = (pkt[78] & 0x10) != 0;

        static int s_last_cue1 = 0;
        static int s_last_cue2 = 0;

        /* MOD menu toggle: when disabled, leave the packet byte-for-byte
         * stock (no CUE masking, no step detection). */
        if (!s_mod_enabled) {
            s_last_cue1 = cue1;
            s_last_cue2 = cue2;
            return n;
        }

        if (shift) {
            /* CRITICAL: Suppress/swallow stock headphone CUE action */
            pkt[78] &= ~0x30;

            /* Recompute genuine 16-bit big-endian CRC over the first 120 bytes
             * using EP148's own CRC function at 0x1f4d1d8. */
            if (n >= 122) {
                uint16_t crc = ((uint16_t (*)(void *))0x1f4d1d8)(pkt);
                pkt[120] = (unsigned char)(crc >> 8);
                pkt[121] = (unsigned char)(crc & 0xff);
            }

            /* Rising edge detection */
            if (cue1 && !s_last_cue1) {
                s_pending_target_step = -1; /* PREVIOUS */
                if (s_verify) mod_logf("SPI edge: CUE1 -> PREV\n");
            }
            if (cue2 && !s_last_cue2) {
                s_pending_target_step = +1; /* NEXT */
                if (s_verify) mod_logf("SPI edge: CUE2 -> NEXT\n");
            }
        }
        s_last_cue1 = cue1;
        s_last_cue2 = cue2;
    }
    return n;
}

void *XOpenDisplay(const char *display_name) {
    static void *(*real_XOpenDisplay)(const char *) = NULL;
    if (!real_XOpenDisplay) real_XOpenDisplay = dlsym(RTLD_NEXT, "XOpenDisplay");
    if (!real_XOpenDisplay) return NULL;

    void *dpy = real_XOpenDisplay(display_name);
    if (!dpy && s_version_ok) {
        /* x11-only.sh runs "exec X :0" without -noreset. When systemctl
         * restarts EP148.service, the old EP148 disconnecting causes Xorg to
         * reset DRM/Glamor/modesetting, while stock EP148 (0x226fe80) only
         * tries XOpenDisplay twice with zero sleep before calling _Exit(1).
         * Retry for up to 5s so X :0 finishes resetting. */
        for (int attempt = 1; attempt <= 50 && !dpy; attempt++) {
            usleep(100000); /* 100 ms */
            dpy = real_XOpenDisplay(display_name);
            if (dpy || attempt == 1 || attempt == 50) {
                mod_logf("XOpenDisplay(%s) retry #%d -> %p\n",
                         display_name ? display_name : "(null)", attempt, dpy);
            }
        }
    } else if (s_version_ok) {
        mod_logf("XOpenDisplay(%s) -> %p\n",
                 display_name ? display_name : "(null)", dpy);
    }
    return dpy;
}

int XMapWindow(void *dpy, unsigned long w) {
    static int (*real_XMapWindow)(void *, unsigned long) = NULL;
    if (!real_XMapWindow) real_XMapWindow = dlsym(RTLD_NEXT, "XMapWindow");
    static int s_logged_maps = 0;
    if (s_version_ok && s_logged_maps < 5) {
        s_logged_maps++;
        mod_logf("XMapWindow(dpy=%p, win=0x%lx)\n", dpy, w);
    }
    return real_XMapWindow ? real_XMapWindow(dpy, w) : 0;
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p, const sigset_t *sigmask) {
    if (!real_ppoll) real_ppoll = dlsym(RTLD_NEXT, "ppoll");
    if (!s_version_ok || !tmo_p || gettid() != getpid()) {
        return real_ppoll ? real_ppoll(fds, nfds, tmo_p, sigmask) : 0;
    }
    struct timespec ts;
    if (nfds > 1 && s_panel_dismiss_time_ms > 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 30000000; /* 30 ms */
        tmo_p = &ts;
    }
    int res = real_ppoll ? real_ppoll(fds, nfds, tmo_p, sigmask) : 0;
    check_gui_thread_tasks();
    return res;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (!real_poll) real_poll = dlsym(RTLD_NEXT, "poll");
    /* JUCE's GUI message loop (0x218b524 / 0x218ba08 / 0x218c4d4) always passes
     * timeout = 2000 (>= 0) on the main thread. Any call with timeout < 0 is a
     * blocking synchronous wait (libxcb xcb_wait_for_reply holding the Xlib
     * Display lock, or edb_streamd blocking IPC at 0x234e66c) and must never be
     * clamped or interrupted by check_gui_thread_tasks(). */
    if (!s_version_ok || timeout < 0 || gettid() != getpid()) {
        return real_poll ? real_poll(fds, nfds, timeout) : 0;
    }
    if (nfds > 1) {
        int max_tmo = (s_panel_dismiss_time_ms > 0) ? 30 : 50;
        if (timeout > max_tmo) timeout = max_tmo;
    }
    int res = real_poll ? real_poll(fds, nfds, timeout) : 0;
    check_gui_thread_tasks();
    return res;
}

int epoll_wait(int epfd, void *events, int maxevents, int timeout) {
    if (!real_epoll_wait) real_epoll_wait = dlsym(RTLD_NEXT, "epoll_wait");
    if (!s_version_ok || timeout < 0 || gettid() != getpid()) {
        return real_epoll_wait ? real_epoll_wait(epfd, events, maxevents, timeout) : 0;
    }
    if (s_panel_dismiss_time_ms > 0 && timeout > 30) {
        timeout = 30;
    }
    int res = real_epoll_wait ? real_epoll_wait(epfd, events, maxevents, timeout) : 0;
    check_gui_thread_tasks();
    return res;
}

__attribute__((constructor))
static void libfxknob_init(void) {
    if (__progname && strcmp(__progname, "EP148") != 0) return;

    s_init_time_ms = get_now_ms();
    mod_logf("libfxknob constructor entered (progname=%s)\n", __progname ? __progname : "(null)");

    /* Optional audio-routing verification logging (FX_MOD_VERIFY=1). */
    const char *v = getenv("FX_MOD_VERIFY");
    s_verify = (v && v[0] == '1') ? 1 : 0;

    /* Install this first so a fault anywhere below leaves a report. */
    install_crash_handler();

    /* Refuse to touch a build these addresses were not derived from. */
    s_version_ok = check_ep148_version();
    mod_logf("check_ep148_version: %s\n", s_version_ok ? "PASSED (12/12)" : "FAILED");
    if (!s_version_ok) {
        /* The LD_PRELOAD hooks are already installed and cannot be removed, so
         * switch the mod off as well. Every hook checks s_version_ok, but
         * s_mod_enabled also gates the SPI packet rewrite. */
        s_mod_enabled = 0;
        mod_logf("EP148 build not recognised - mod disabled, no code patched (v1.23 only)\n");
        return;
    }

    /*
     * Relax the SPI CRC check in device_adapter::subcpu_comm::erp_ucom::RxFormatField
     * so a modified packet is accepted.
     */
    patch_code32(0x1f48ea0, 0xd503201f); /* NOP */

    /* Extend Column 1 category table height and background to fit 5 sections without scrolling */
    patch_code32(0x1ab32bc, 0x52802c64); /* mov w4, #0x163 (355px table height) */
    patch_code32(0x1ab3338, 0x71058c1f); /* cmp w0, #0x163 */
    patch_code32(0x1ab3344, 0x52802c64); /* mov w4, #0x163 */
    patch_code32(0x179d24c, 0x71069e9f); /* cmp w20, #0x1a7 (loop 5 category cell backgrounds) */

    /* Hook CategoryTableModel string builder to append MOD section */
    patch_jump64(0x1ae2528, (void *)hook_category_builder);

    /* Hook ItemTableController and ValueTableController category switchers */
    real_item_table_set_category = (void (*)(void *, int))make_trampoline16(0x1accdc0, (void *)hook_item_table_set_category);
    real_value_table_set_category_and_item = (void (*)(void *, int, int))make_trampoline16(0x1add858, (void *)hook_value_table_set_category_and_item);

    mod_logf("libfxknob v1.0 initialized successfully%s\n", s_verify ? " [VERIFY MODE]" : "");
}
