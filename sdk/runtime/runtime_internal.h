/* Shared between the runtime's own files; not part of the app API. */
#ifndef HB_RUNTIME_INTERNAL_H
#define HB_RUNTIME_INTERNAL_H

#include "hb/abi.h"
#include "hb/app.h"

struct hb_loader_api *hb__runtime_api(void);

/* Run when main() returns (on the app's stack, so it may wait): hb_gfx_open sets it to
 * hb_gfx_close, so an app that forgets to close still gives the screen back. */
extern void (*hb__cleanup)(void);

#endif
