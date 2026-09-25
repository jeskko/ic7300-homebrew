/* Two popups in a row: each ui_message_box() blocks until its OK is tapped, then the next one
 * opens. */
#include "hb/app.h"

int main(void)
{
    ui_message_box("Homebrew SDK for the IC-7300");
    ui_message_box("Apps live in \\homebrew\non the SD card.\nPick one from SD Card >\nHomebrew Apps.");
    return 0;
}
