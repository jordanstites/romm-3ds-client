/*
 * Installed titles screen - what is actually on this console
 */

#ifndef INSTALLED_H
#define INSTALLED_H

#include <3ds.h>

typedef enum { INSTALLED_NONE, INSTALLED_BACK } InstalledResult;

void installed_init(void);
InstalledResult installed_update(u32 kDown);
void installed_draw(void);

#endif // INSTALLED_H
