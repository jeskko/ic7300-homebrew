/* App runtime: runs main() as a coroutine on the UI thread (see hb/app.h).
 *
 * The loader calls hb_runtime_start() from the "Homebrew Apps" menu action. We switch onto the
 * app stack and run main() until it first waits; then we switch back, set the loader's
 * idle_hook, and return, so the menu action (and the firmware's UI loop) carry on. Every loop
 * pass the loader calls hb_idle(), which switches back into the app once its wait condition
 * holds. When main() returns we clear idle_hook, and the loader is free to load an app again.
 */
#include "hb/abi.h"
#include "hb/app.h"

struct hb_coro { void *sp; };

void hb_coro_switch(struct hb_coro *from, struct hb_coro *to);

extern char __hb_stack_top[];

static struct hb_loader_api *g_api;
static struct hb_coro g_host, g_app;
static bool g_app_done;
static bool (*g_wait_done)(void *arg);
static void *g_wait_arg;

static void app_trampoline(void)
{
    main();
    g_app_done = true;
    hb_coro_switch(&g_app, &g_host);    /* never resumed */
    for (;;) {}
}

static void hb_idle(void);

/* Run the app until it next waits (or finishes), then decide whether we still need ticks. */
static void run_slice(void)
{
    hb_coro_switch(&g_host, &g_app);
    g_api->idle_hook = g_app_done ? 0 : hb_idle;
}

static void hb_idle(void)
{
    if (g_wait_done && !g_wait_done(g_wait_arg))
        return;
    run_slice();
}

void hb_wait_until(bool (*done)(void *arg), void *arg)
{
    if (done && done(arg))
        return;
    g_wait_done = done;
    g_wait_arg = arg;
    hb_coro_switch(&g_app, &g_host);
    g_wait_done = 0;
    g_wait_arg = 0;
}

void hb_yield(void)
{
    hb_wait_until(0, 0);
}

void hb_runtime_start(struct hb_loader_api *api)
{
    if (api->abi_version != HB_ABI_VERSION || api->fw_build != HB_FW_142)
        return;
    g_api = api;

    /* Initial frame for hb_coro_switch's pop {r4-r12, lr}: zeros, then lr = trampoline. */
    uint32_t *sp = (uint32_t *)((uintptr_t)__hb_stack_top - 10 * sizeof(uint32_t));
    for (int i = 0; i < 9; i++)
        sp[i] = 0;
    sp[9] = (uint32_t)app_trampoline;
    g_app.sp = sp;

    run_slice();
}
