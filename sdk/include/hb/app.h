/* Homebrew app API.
 *
 * An app is a plain C program: write `int main(void)`, build it with sdk/tools/build_app.py,
 * drop the resulting NAME.BIN in C:\homebrew\ on the SD card, and start it from
 * MENU > SET > SD Card > Homebrew Apps. It may be up to 1 MB including bss and stack, and
 * hb/heap.h gives it another 448 KB of dynamic memory.
 *
 * main() runs on the radio's UI thread, but on its own stack (a coroutine), so calls that
 * wait -- ui_message_box(), hb_wait_until() -- really do block *your* code while the radio keeps
 * running: the runtime hands control back to the firmware and resumes you once per UI loop pass
 * until whatever you're waiting for has happened. Never busy-wait in a loop instead: nothing on
 * screen updates until you yield.
 */
#ifndef HB_APP_H
#define HB_APP_H

#include <stdbool.h>
#include <stdint.h>

int main(void);

/* Give the firmware one UI loop pass, then continue. */
void hb_yield(void);

/* Yield until done(arg) returns true (checked once per UI loop pass). */
void hb_wait_until(bool (*done)(void *arg), void *arg);

/* Show a popup with `text` and an OK button; returns once it has been dismissed. `text` may
 * hold up to 6 lines separated by '\n'. Returns true if OK was tapped, false if the dialog went
 * away some other way (e.g. the firmware replaced it with one of its own). */
bool ui_message_box(const char *text);

#endif
