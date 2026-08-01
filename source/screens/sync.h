/*
 * Sync screen - save synchronisation with RomM
 */

#ifndef SYNC_SCREEN_H
#define SYNC_SCREEN_H

#include "../auth.h"
#include "../config.h"
#include <3ds.h>

typedef enum {
    SYNC_SCREEN_NONE,
    SYNC_SCREEN_DONE // user dismissed the screen
} SyncScreenResult;

// Begin a sync run: scan, hash, negotiate. Blocking, so callers should render a
// loading frame first.
void sync_screen_init(const Config *config, AuthToken *token);

SyncScreenResult sync_screen_update(u32 kDown);
void sync_screen_draw(void);

#endif // SYNC_SCREEN_H
