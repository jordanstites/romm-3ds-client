/*
 * Library index - which ROMs are actually on this SD card
 *
 * Save sync needs to map a file on the SD card back to a rom_id on the server.
 * Scanning the library over the API to work that out would mean paging through
 * every ROM on every sync; instead we record the mapping when a download
 * succeeds, which is both exact and O(1) to look up.
 *
 * The index is a plain tab-separated file so it can be inspected or repaired by
 * hand, matching how the download queue is already stored.
 */

#ifndef LIBRARY_H
#define LIBRARY_H

#include "config.h"
#include <stdbool.h>

#define LIBRARY_MAX_ENTRIES 512
#define LIBRARY_MAX_NAME_LEN 256

typedef struct {
    int romId;
    char platformSlug[CONFIG_MAX_SLUG_LEN];
    char fsName[LIBRARY_MAX_NAME_LEN]; // file name as stored on disk
} LibraryEntry;

// Load the index from SD. Safe to call repeatedly.
void library_init(void);

// Record a downloaded ROM, replacing any existing entry for the same id.
// Persists immediately -- a crash between download and sync should not lose
// the mapping.
bool library_record(int romId, const char *platformSlug, const char *fsName);

// Number of indexed ROMs.
int library_count(void);

// Entry by position, or NULL if out of range.
const LibraryEntry *library_get(int index);

// Entry for a server-side ROM id, or NULL if that ROM is not on this card.
const LibraryEntry *library_find(int romId);

#endif // LIBRARY_H
