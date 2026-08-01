/*
 * Installed titles screen - what is actually on this console
 */

#ifndef INSTALLED_H
#define INSTALLED_H

#include <3ds.h>

#include <stdbool.h>

typedef enum {
    INSTALLED_NONE,
    INSTALLED_BACK,
    INSTALLED_PICKED // a title was chosen while in picker mode
} InstalledResult;

// Browse mode: just shows what is installed.
void installed_init(void);

// Picker mode: choosing a title returns INSTALLED_PICKED. `suggestName` is the
// ROM being linked, used to preselect the most likely title.
void installed_init_picker(const char *romName, const char *suggestName);

// Title chosen in picker mode, or 0.
u64 installed_get_picked(void);

InstalledResult installed_update(u32 kDown);
void installed_draw(void);

#endif // INSTALLED_H
