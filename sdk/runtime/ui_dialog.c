/* ui_message_box(): the firmware's own popup dialog with our text.
 *
 * There's no spare message record to own (the table runs straight into string data past its
 * last entry), so we borrow the stock one-button dialog FW_DIALOG_ITEM_OK: swap its record's
 * text pointers for ours, show it, wait for it to close, and put the stock text back.
 */
#include "hb/app.h"
#include "hb/firmware.h"

#define MAX_LINES   FW_MSG_TEXT_LINES
#define TEXT_MAX    256

static char g_text[TEXT_MAX];
static bool g_ok_tapped;

static int on_ok(uint8_t *result)
{
    (void)result;
    g_ok_tapped = true;
    return 2;                           /* what the stock no-callback path returns: close */
}

static bool no_dialog_up(void *arg)
{
    (void)arg;
    return fw_dialog_state->active_item == 0;
}

static bool our_dialog_gone(void *arg)
{
    (void)arg;
    return fw_dialog_state->active_item != FW_DIALOG_ITEM_OK;
}

/* Split `text` into g_text, filling `lines`; returns the number of lines. */
static int split_lines(const char *text, const char **lines)
{
    int n = 0, i = 0;
    lines[n++] = g_text;
    for (; *text && i < TEXT_MAX - 1; text++) {
        if (*text == '\n') {
            if (n == MAX_LINES)
                break;
            g_text[i++] = 0;
            lines[n++] = &g_text[i];
        } else {
            g_text[i++] = *text;
        }
    }
    g_text[i] = 0;
    return n;
}

bool ui_message_box(const char *text)
{
    volatile struct fw_dialog_item *item = &fw_dialog_items[FW_DIALOG_ITEM_OK];
    if (item->type != 1 || item->message != FW_DIALOG_RECORD_OK)
        return false;                   /* not the table we were built against */

    /* Don't knock a firmware dialog off the screen; wait our turn. */
    hb_wait_until(no_dialog_up, 0);

    struct fw_message_record *rec = &fw_message_records[FW_DIALOG_RECORD_OK];
    struct fw_message_record saved = *rec;

    const char *lines[MAX_LINES];
    int n = split_lines(text, lines);
    for (int i = 0; i < 9; i++) {
        const char *s = i < n ? lines[i] : i == FW_MSG_BUTTON_RIGHT ? "OK" : "";
        rec->en[i] = s;
        rec->jp[i] = s;
    }
    rec->flags = 0;

    g_ok_tapped = false;
    fw_ui_show_message_dialog(FW_DIALOG_ITEM_OK, on_ok, 0, 0);
    hb_wait_until(our_dialog_gone, 0);

    *rec = saved;
    return g_ok_tapped;
}
