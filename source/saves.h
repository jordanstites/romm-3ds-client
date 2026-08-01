/*
 * Local save discovery and hashing (Tier 1: GB / GBC / GBA / NDS and friends)
 *
 * Finds save files sitting on the SD card next to downloaded ROMs, so they can
 * be offered to RomM's sync protocol.
 *
 * Layouts are not uniform. TWiLight Menu++ keeps DS saves in a `saves/`
 * subfolder but GB/GBA saves beside the ROM, a split introduced in v6.8.3, and
 * a user setting can move either. Multi-slot saves also mangle the extension
 * (.sav, .sav1, .sav2 ...), which is where the sync `slot` field comes from.
 */

#ifndef SAVES_H
#define SAVES_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAVES_MAX 256
#define SAVES_MAX_PATH 512
#define SAVES_MAX_NAME 256
#define SAVES_HASH_LEN 33 // 32 hex chars + NUL
#define SAVES_MAX_CANDIDATES 2

typedef struct {
    int romId;
    char path[SAVES_MAX_PATH];     // absolute sdmc: path
    char fileName[SAVES_MAX_NAME]; // basename, sent to the server
    char slot[8];                  // "0", "1", ... never empty; see note below
    char contentHash[SAVES_HASH_LEN];
    uint64_t sizeBytes;
    uint64_t modifiedAt; // unix seconds

    // Native 3DS titles have no save file on the card: the save lives in the
    // title's archive, and `path` points at a zip staged from it. Restoring one
    // means writing the archive back rather than replacing a file, so the
    // difference has to survive into the execute step.
    bool nativeArchive;
    unsigned long long titleId;
    unsigned int uniqueId;
    int mediaType;
} LocalSave;

// Scan the SD card for saves belonging to indexed ROMs. Returns how many were
// written to `out`. Hashing is the expensive part, so this is not free.
int saves_scan(const Config *config, LocalSave *out, int maxSaves);

// MD5 of a file's bytes, lowercase hex. This is what RomM's content_hash is:
// backend/handler/filesystem/assets_handler.py hashes plain files with
// hashlib.md5, and the column is String(length=32). Returns false if the file
// cannot be read.
bool saves_hash_file(const char *path, char out[SAVES_HASH_LEN]);

// Every place a save for this ROM could legitimately live, most likely first.
// Emulator configuration decides which is real, so both are probed.
int saves_candidate_paths(const Config *config, const char *platformSlug, const char *romFsName, const char *slot,
                          char out[][SAVES_MAX_PATH], int maxPaths);

// First candidate that actually exists on the card.
bool saves_find_existing(const Config *config, const char *platformSlug, const char *romFsName, const char *slot,
                         char *out, size_t outLen);

// Where to write a save for this ROM: the existing file if there is one,
// otherwise the platform's default layout.
bool saves_build_path(const Config *config, const char *platformSlug, const char *romFsName, const char *slot,
                      char *out, size_t outLen);

#endif // SAVES_H
