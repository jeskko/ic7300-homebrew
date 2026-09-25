/* The first homebrew GUI app: a "Hello, world!" popup with an OK button. */
#include "hb/app.h"

int main(void)
{
    ui_message_box("Hello, world!");
    return 0;
}
