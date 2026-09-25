/* Homebrew loader: the part of the SDK that is flashed once, appended to body.bin.
 *
 * Entry points, patched into the stock firmware by build.py:
 *
 *   hb_menu_action -- the "Homebrew Apps" row in SET > SD Card (a type-3 settings row; the
 *                     firmware calls it with no arguments on tap). Lists C:\homebrew\*.BIN and
 *                     opens the app picker: a borrowed stock list screen (see below).
 *   hb_idle_hook   -- replaces main_idle_loop's `bl civ_tx_pump`: runs civ_tx_pump as before,
 *                     puts the borrowed screen back once the picker is left, then runs the
 *                     resident app's idle_hook, if it set one (see hb/abi.h).
 *
 * The picker. The firmware has no free list screen, so we borrow PLAYER SET (screen 0x63,
 * category 0x40, one stock row, deep in the voice-recorder menus): while the picker is up, its
 * category's row list, its title and its saved cursor are ours; the idle hook restores all
 * three the first pass after the user leaves the screen. Each app is one type-3 row whose
 * catalog record lives in the confirmed-unused padding gap build.py reserves; all of them share
 * hb_app_row_action, which finds the tapped row in fw_list_cursor.
 */
#include <stdbool.h>

#include "hb/abi.h"
#include "hb/firmware.h"

#define APPS_DIR            "C:\\homebrew"
#define MAX_APPS            14          /* catalog slots build.py reserves */
#define APP_CATALOG_FIRST   0x882       /* 0x881 is the Homebrew Apps row itself */
#define PICKER_SCREEN       0x63
#define PICKER_CATEGORY     0x40
#define NAME_MAX            13          /* 8.3 + NUL */

static struct hb_loader_api g_api = {
    .abi_version = HB_ABI_VERSION,
    .fw_build = HB_FW_142,
    .idle_hook = 0,
};

static uint32_t g_scratch[9];           /* the open RPCs' 36-byte scratch argument */

/* ---- file loading ----------------------------------------------------------------------- */

static int g_handle;
static int32_t g_read_actual;

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

static int32_t load_file(const char *path)
{
    if (fw_rpc_wait(fw_file_open(path, 0, &g_handle, g_scratch), FW_RPC_ERR_SELECT) != 0)
        return -1;
    g_read_actual = 0;
    int rc = fw_rpc_wait(fw_file_read(g_handle, (void *)HB_APP_REGION, HB_APP_REGION_SIZE,
                                      &g_read_actual), FW_RPC_ERR_SELECT);
    fw_rpc_wait(fw_file_close(g_handle, 0), FW_RPC_ERR_SELECT);
    if (rc != 0)
        return -1;
    return g_read_actual;   /* negative errno on failure, per the wrapper */
}

static void run_app(const char *path)
{
    if (g_api.idle_hook)
        return;                         /* an app is resident; don't load over it */

    int32_t len = load_file(path);
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

/* ---- listing C:\homebrew ---------------------------------------------------------------- */

static char g_names[MAX_APPS][NAME_MAX];        /* file names, e.g. "HELLO.BIN" */
static char g_labels[MAX_APPS][NAME_MAX];       /* row labels, e.g. "HELLO" */
static int g_app_count;
static char g_dirent_name[0x14];
static struct fw_dirent g_dirent;

static char upper(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

/* Is `name` "<1-8 chars>.BIN" (any case)? Returns the stem length, or 0. */
static int bin_stem_len(const char *name)
{
    int n = 0;
    while (name[n] && n < NAME_MAX)
        n++;
    if (n < 5 || n >= NAME_MAX || name[n - 4] != '.' || upper(name[n - 3]) != 'B' ||
        upper(name[n - 2]) != 'I' || upper(name[n - 1]) != 'N')
        return 0;
    return n - 4;
}

static int name_cmp(const char *a, const char *b)
{
    while (*a && upper(*a) == upper(*b))
        a++, b++;
    return upper(*a) - upper(*b);
}

static void add_app(const char *name)
{
    int stem = bin_stem_len(name);
    if (!stem)
        return;
    /* Insertion sort, so the picker lists apps alphabetically. When full, keep the first
     * MAX_APPS names in that order, whatever order the directory returns them in. */
    if (g_app_count == MAX_APPS) {
        if (name_cmp(name, g_names[MAX_APPS - 1]) >= 0)
            return;
        g_app_count--;
    }
    int i = g_app_count++;
    for (; i > 0 && name_cmp(name, g_names[i - 1]) < 0; i--) {
        for (int k = 0; k < NAME_MAX; k++) {
            g_names[i][k] = g_names[i - 1][k];
            g_labels[i][k] = g_labels[i - 1][k];
        }
    }
    int k = 0;
    for (; name[k]; k++)
        g_names[i][k] = name[k];
    g_names[i][k] = 0;
    for (k = 0; k < stem; k++)
        g_labels[i][k] = name[k];
    g_labels[i][k] = 0;
}

static void scan_apps(void)
{
    int dir;
    g_app_count = 0;
    if (fw_rpc_wait(fw_dir_open(APPS_DIR, &dir, g_scratch), FW_RPC_ERR_SELECT) != 0)
        return;
    g_dirent.name = g_dirent_name;
    g_dirent.next_pos = 0;
    for (int guard = 0; guard < 512; guard++) {     /* never trust the end marker alone */
        g_dirent.pos = g_dirent.next_pos;
        g_dirent.name_len = sizeof g_dirent_name;
        g_dirent_name[0] = 0;
        if (fw_rpc_wait(fw_dir_read(dir, &g_dirent), FW_RPC_ERR_SELECT) != 0)
            break;                                  /* end of directory (or an error) */
        g_dirent_name[sizeof g_dirent_name - 1] = 0;
        if (g_dirent.kind == FW_DIRENT_FILE && g_dirent.size >= sizeof(struct hb_app_header) &&
            g_dirent.size <= HB_APP_REGION_SIZE)
            add_app(g_dirent_name);
    }
    fw_rpc_wait(fw_dir_close(dir), FW_RPC_ERR_SELECT);
}

/* ---- the picker screen ------------------------------------------------------------------ */

static uint32_t g_rows[MAX_APPS];
static bool g_picker_active;
static struct {
    uint32_t count;
    const uint32_t *list;
    const char *title_en, *title_jp;
    uint32_t saved_cursor;
} g_stock;

static const char g_title[] = "HOMEBREW APPS";
static const char g_no_apps[] = "No apps in \\homebrew";

static void hb_app_row_action(void)
{
    unsigned i = fw_list_cursor;
    if (i >= (unsigned)g_app_count)
        return;
    char path[sizeof APPS_DIR + NAME_MAX];
    int k = 0;
    for (const char *s = APPS_DIR "\\"; *s; s++)
        path[k++] = *s;
    for (const char *s = g_names[i]; *s; s++)
        path[k++] = *s;
    path[k] = 0;
    run_app(path);
}

static void set_row(int i, void (*action)(void), int (*query)(uint8_t *), const char *label)
{
    volatile struct fw_settings_item *it = &fw_settings_catalog[APP_CATALOG_FIRST + i];
    it->action = action;
    it->query = query;
    it->flags = FW_ITEM_FLAGS_PLAIN;
    it->en = label;
    it->jp = label;
    g_rows[i] = 3u | ((uint32_t)(APP_CATALOG_FIRST + i) << 16);
}

static void picker_restore(void)
{
    volatile struct fw_settings_category *cat = &fw_settings_registry[PICKER_CATEGORY];
    volatile struct fw_screen_descriptor *d = &fw_screen_descriptors[PICKER_SCREEN - 0x13];
    cat->count = g_stock.count;
    cat->list = g_stock.list;
    d->title_en = g_stock.title_en;
    d->title_jp = g_stock.title_jp;
    fw_saved_cursor(PICKER_CATEGORY) = g_stock.saved_cursor;
    g_picker_active = false;
}

void hb_menu_action(void)
{
    if (g_api.idle_hook || g_picker_active)
        return;

    scan_apps();
    int rows = g_app_count;
    for (int i = 0; i < g_app_count; i++)
        set_row(i, hb_app_row_action, 0, g_labels[i]);
    if (rows == 0) {
        set_row(0, FW_PLACEHOLDER_ACTION, FW_PLACEHOLDER_QUERY, g_no_apps);
        rows = 1;
    }

    volatile struct fw_settings_category *cat = &fw_settings_registry[PICKER_CATEGORY];
    volatile struct fw_screen_descriptor *d = &fw_screen_descriptors[PICKER_SCREEN - 0x13];
    g_stock.count = cat->count;
    g_stock.list = cat->list;
    g_stock.title_en = d->title_en;
    g_stock.title_jp = d->title_jp;
    g_stock.saved_cursor = fw_saved_cursor(PICKER_CATEGORY);

    cat->list = g_rows;
    cat->count = (uint32_t)rows;
    d->title_en = g_title;
    d->title_jp = g_title;
    fw_saved_cursor(PICKER_CATEGORY) = 0;
    g_picker_active = true;

    fw_operating_mode_change_dispatch(PICKER_SCREEN);
}

void hb_idle_hook(void)
{
    fw_civ_tx_pump();
    if (g_picker_active && fw_current_screen != PICKER_SCREEN)
        picker_restore();
    void (*hook)(void) = g_api.idle_hook;
    if (hook)
        hook();
}
