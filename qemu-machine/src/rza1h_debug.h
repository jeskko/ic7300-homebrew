/*
 * Shared, permanent debug-logging helper for this project's own real peripheral devices.
 *
 * Why this exists: a genuine QEMU GDB-remote-stub reliability artifact under `-icount
 * shift=auto` (found 2026-09-09, see README-history.md's "GDB-remote-stub reliability
 * artifact" section) made GDB breakpoints an unreliable way to observe a real busy-wait's
 * completion -- host-side-only logging directly inside the device model, completely
 * independent of GDB, is what actually resolved it (a one-off, temporary `fprintf` added to
 * `dmac.c` and reverted after use). This header makes that technique a permanent, reusable
 * tool instead of something reinvented ad hoc per investigation.
 *
 * Deliberately NOT built on QEMU's own `-d`/`qemu_log_mask()` mechanism: `mmc.c`'s own real
 * command-dispatch logging already (mis)uses `LOG_UNIMP` for this, which works but pollutes
 * that category's otherwise-clean "genuinely unimplemented access" signal (relied on directly
 * this same session to answer "does anything touch the display hardware at all" -- a routine
 * peripheral trace mixed into the same category would have made that check much less
 * trustworthy). A dedicated env-var-gated mechanism keeps `-d unimp`/`-d guest_errors` clean
 * and needs no QEMU source patching -- `setup.sh` re-clones and re-patches a fresh QEMU
 * checkout on every run, so avoiding a second patch to rebase against future QEMU version
 * bumps is a real, concrete win, not just aesthetic.
 *
 * Usage: RZA1H_DEBUG=<comma-separated device names>|all ./qemu-system-arm ...
 *   e.g. RZA1H_DEBUG=dmac,mtu2   -- only those two devices
 *        RZA1H_DEBUG=all        -- every device that calls rza1h_debug()
 *   unset/empty                 -- fully silent (one getenv()+scan per call site, negligible
 *                                  since this is only ever called at low-frequency "boundary"
 *                                  events, never from a hot per-instruction/per-tick path)
 *
 * Log only "boundary" events -- command/register dispatch that changes real behavior, IRQ
 * raise/lower, mode transitions, completion -- not routine plain-passthrough register
 * reads/writes (most registers are inert; logging every touch would just be noise, the same
 * lesson `dmac.c`'s own one-off version already proved out). Each call site passes the
 * device's own short name (matching this project's own peripheral-table naming, e.g. "dmac",
 * "mtu2") so a single env var can select exactly the devices of interest for a given
 * investigation, and multiple instances of the same device type (SCIF0-7, RIIC0-2) share one
 * name -- no per-instance state here, deliberately, so this stays a plain header with no
 * accompanying .c file or meson.build change needed.
 */

#ifndef RZA1H_DEBUG_H
#define RZA1H_DEBUG_H

#include "qemu/osdep.h"

static inline bool rza1h_debug_enabled(const char *dev_name)
{
    const char *env = getenv("RZA1H_DEBUG");
    size_t dev_len;
    const char *p;

    if (!env || !*env) {
        return false;
    }
    if (strcmp(env, "all") == 0) {
        return true;
    }

    dev_len = strlen(dev_name);
    for (p = env; *p; ) {
        const char *comma = strchr(p, ',');
        size_t tok_len = comma ? (size_t)(comma - p) : strlen(p);

        if (tok_len == dev_len && strncmp(p, dev_name, tok_len) == 0) {
            return true;
        }
        p += tok_len;
        if (*p == ',') {
            p++;
        }
    }
    return false;
}

static inline void rza1h_debug(const char *dev_name, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static inline void rza1h_debug(const char *dev_name, const char *fmt, ...)
{
    va_list ap;

    if (!rza1h_debug_enabled(dev_name)) {
        return;
    }
    fprintf(stderr, "[rza1h:%s t=%.3f] ", dev_name, g_get_monotonic_time() / 1e6);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

#endif /* RZA1H_DEBUG_H */
