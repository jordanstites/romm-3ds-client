/*
 * Title mapping - which installed 3DS title a RomM ROM corresponds to
 *
 * Native 3DS saves live in a save archive addressed by title ID, while RomM
 * knows only rom_ids, and stores no title ID to join on. Something has to
 * bridge the two.
 *
 * Name matching is good enough to *suggest* a link and to draw a badge, but not
 * to drive one: restoring a save to the wrong title overwrites data that cannot
 * be recovered. So a mapping only exists once the user has confirmed it, and it
 * is stored by title ID rather than by name so renaming anything cannot break
 * or silently repoint it.
 */

#ifndef TITLEMAP_H
#define TITLEMAP_H

#include <3ds.h>
#include <stdbool.h>

#define TITLEMAP_MAX 256

typedef struct {
    int romId;
    u64 titleId;
} TitleMapping;

void titlemap_init(void);

// Confirm a link. Replaces any existing mapping for either side, since a ROM
// maps to exactly one title and vice versa.
bool titlemap_set(int romId, u64 titleId);

// Remove the mapping for a ROM, if any.
bool titlemap_clear(int romId);

// Title mapped to this ROM, or 0 if unmapped.
u64 titlemap_get_title(int romId);

// ROM mapped to this title, or 0 if unmapped.
int titlemap_get_rom(u64 titleId);

int titlemap_count(void);

#endif // TITLEMAP_H
