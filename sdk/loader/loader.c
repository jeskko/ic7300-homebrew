/* Homebrew loader: the part of the SDK that is flashed once, appended to body.bin.
 *
 * Two entry points, both patched into the stock firmware by build.py:
 *
 *   hb_menu_action -- the "Homebrew Apps" row in SET > SD Card (a type-3 settings row; the
 *                     firmware calls it with no arguments on tap). Loads C:\IC-7300\APP.BIN
 *                     into HB_APP_REGION, checks its header and calls its entry point.
 *   hb_idle_hook   -- replaces main_idle_loop's `bl civ_tx_pump`: runs civ_tx_pump as before,
 *                     then the resident app's idle_hook, if it set one (see hb/abi.h).
 *
 * Supersedes sdk/examples/homebrew-apps-menu/menu_hook.s (same menu row, same file I/O and
 * the same fail-closed checks) and adds the header check, the ABI and the idle tick.
 */
#include "hb/abi.h"
#include "hb/firmware.h"

static struct hb_loader_api g_api = {
    .abi_version = HB_ABI_VERSION,
    .fw_build = HB_FW_142,
    .idle_hook = 0,
};

static int g_handle;
static int32_t g_read_actual;
static uint32_t g_open_scratch[9];

void hb_idle_hook(void)
{
    fw_civ_tx_pump();
    void (*hook)(void) = g_api.idle_hook;
    if (hook)
        hook();
}

/* Make instruction fetch see freshly written code: clean D-cache and invalidate I-cache by
 * line over the range, then invalidate the branch predictor. Unverified on real hardware
 * (QEMU models no cache incoherency). */
static void cache_sync(uint32_t start, uint32_t len)
{
    for (uint32_t a = start & ~31u; a < start + len; a += 32) {
        __asm__ volatile("mcr p15, 0, %0, c7, c11, 1" :: "r"(a) : "memory");   /* DCCMVAU */
        __asm__ volatile("mcr p15, 0, %0, c7, c5, 1" :: "r"(a) : "memory");    /* ICIMVAU */
    }
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 6\n\tdsb\n\tisb" :: "r"(0) : "memory");
}

static int32_t load_app(void)
{
    if (fw_rpc_wait(fw_file_open("C:\\IC-7300\\APP.BIN", 0, &g_handle, g_open_scratch),
                    FW_RPC_ERR_SELECT) != 0)
        return -1;
    g_read_actual = 0;
    int rc = fw_rpc_wait(fw_file_read(g_handle, (void *)HB_APP_REGION, HB_APP_REGION_SIZE,
                                      &g_read_actual), FW_RPC_ERR_SELECT);
    fw_rpc_wait(fw_file_close(g_handle, 0), FW_RPC_ERR_SELECT);
    if (rc != 0)
        return -1;
    return g_read_actual;   /* negative errno on failure, per the wrapper */
}

void hb_menu_action(void)
{
    if (g_api.idle_hook)
        return;                         /* an app is resident; don't load over it */

    int32_t len = load_app();
    if (len < (int32_t)sizeof(struct hb_app_header) || len > (int32_t)HB_APP_REGION_SIZE)
        return;

    const struct hb_app_header *h = (const struct hb_app_header *)HB_APP_REGION;
    if (h->magic != HB_APP_MAGIC || h->abi_version != HB_ABI_VERSION)
        return;
    if (h->entry < HB_APP_REGION + sizeof *h || h->entry >= HB_APP_REGION + (uint32_t)len ||
        (h->entry & 3))
        return;
    if (h->image_end < HB_APP_REGION + (uint32_t)len ||
        h->image_end > HB_APP_REGION + HB_APP_REGION_SIZE)
        return;

    cache_sync(HB_APP_REGION, (uint32_t)len);
    ((hb_app_entry_fn)h->entry)(&g_api);
}
