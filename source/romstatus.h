/*
 * Per-ROM status for the browser
 *
 * Answers, for each ROM in a list: is it on this console, and does it have
 * saves here or on the server?
 *
 * Save counts come from one GET /api/saves?platform_id=N per platform rather
 * than a detail request per ROM -- SimpleRomSchema (what the list endpoint
 * returns) carries no save information, and DetailedRomSchema does, so the
 * naive approach would be one round trip per row.
 */

#ifndef ROMSTATUS_H
#define ROMSTATUS_H

#include "config.h"
#include <stdbool.h>

typedef struct {
    bool onDevice;      // ROM file present on the SD card
    bool installed;     // installed as a 3DS title (no ROM file involved)
    int serverSaves;    // saves RomM holds for this ROM
    int localSaves;     // save files found next to the ROM on this card
} RomStatus;

// Fetch save counts for a platform. One request; safe to call on every entry
// into a platform's ROM list. Returns false if the request failed, in which
// case counts read as zero rather than blocking the browser.
bool romstatus_load_platform(int platformId);

// Forget cached counts, e.g. after a sync changes them.
void romstatus_invalidate(void);

// Status for one ROM. platformSlug and fsName are needed for the on-device and
// local-save checks, which are filesystem lookups rather than API calls.
RomStatus romstatus_for(const Config *config, int romId, const char *platformSlug, const char *fsName);

#endif // ROMSTATUS_H
