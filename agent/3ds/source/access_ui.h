/* On-console editor for the folders the owner opens to the Bridge (spec §11.2). Nothing on the network can
 * reach this: it is driven only by the console's own buttons. */
#ifndef ACCESS_UI_H
#define ACCESS_UI_H

#include <3ds.h>
#include <stdbool.h>

#include "ndp/ndp_access.h"

/* Called after every change (the caller saves the list and applies the new policy). Returns 0 on success. */
typedef int (*access_changed_fn)(const ndp_access *list);

void access_ui_open(ndp_access *list, access_changed_fn changed);
bool access_ui_active(void);
/* Feeds one frame of button presses (hidKeysDown). Returns true when the screen needs redrawing. */
bool access_ui_handle(u32 keys_down);
/* Touch input (graphical UI only): taps and drags. Returns true when the screen needs redrawing. */
bool access_ui_touch(u32 keys_down, u32 keys_held, const touchPosition *touch);
/* Draws the browser on the top screen and the help on the bottom one (the caller flushes/swaps). */
void access_ui_draw(PrintConsole *top, PrintConsole *bottom);

#endif
