/*
 * Installed title enumeration
 *
 * Lists the 3DS games installed on this console, which is the starting point
 * for native save sync: a save archive is addressed by title ID, so we have to
 * know what is installed before we can read anything.
 *
 * Also answers "do I already have this game?" for the ROM browser, which file
 * presence on the SD card cannot -- an installed title has no ROM file.
 */

#ifndef TITLES_H
#define TITLES_H

#include <3ds.h>
#include <stdbool.h>

#define TITLES_MAX 512
#define TITLES_MAX_NAME 128
#define TITLES_PRODUCT_CODE_LEN 16

typedef struct {
    u64 titleId;
    u32 lowId;
    u32 highId;
    u32 uniqueId; // (lowId >> 8) & 0xFFFFF, needed for the save secure value
    FS_MediaType mediaType;
    char name[TITLES_MAX_NAME];                  // from SMDH, English short title
    char productCode[TITLES_PRODUCT_CODE_LEN];   // e.g. CTR-P-ABCD
} InstalledTitle;

// Bring up the AM service. Luma3DS grants am:net to .3dsx, so no CIA build is
// needed. Returns false if the service is unavailable.
bool titles_init(void);
void titles_exit(void);

// Enumerate installed applications, newest scan replaces the previous one.
// Filters out updates, DLC and system titles -- only real games. Returns the
// number found.
int titles_scan(void);

int titles_count(void);
const InstalledTitle *titles_get(int index);

// Look up by title ID, or NULL if it is not installed.
const InstalledTitle *titles_find(u64 titleId);

#endif // TITLES_H
