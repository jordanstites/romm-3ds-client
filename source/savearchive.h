/*
 * 3DS save archive access
 *
 * A native 3DS save is a small filesystem inside the title's save archive, not
 * a file on the SD card. RomM stores one file per save, so the tree is packed
 * to a zip on the way out and unpacked on the way back.
 *
 * Zip layout matters beyond this app: entries are stored relative to the save
 * root ("main", "sub/foo.dat"), with no wrapper directory and no title ID in
 * the path. That is what makes the same archive extract straight into an
 * emulator's save directory, so a save can round-trip console -> RomM ->
 * emulator -> RomM -> console.
 */

#ifndef SAVEARCHIVE_H
#define SAVEARCHIVE_H

#include "saves.h"
#include <3ds.h>
#include <stdbool.h>

typedef enum {
    SAVEARCHIVE_OK,
    SAVEARCHIVE_NOT_FOUND,   // title has no save archive
    SAVEARCHIVE_OPEN_FAILED, // archive exists but could not be opened
    SAVEARCHIVE_EMPTY,       // opened, but contains no files
    SAVEARCHIVE_IO_ERROR
} SaveArchiveResult;

// Pack a title's save archive into a zip at destPath.
SaveArchiveResult savearchive_export(u64 titleId, FS_MediaType mediaType, const char *destPath);

// Whether a title currently has save data. Opens the archive and looks for one
// entry rather than exporting it, so it is cheap enough to call while drawing a
// list. No zip work, so it needs no oversized stack.
bool savearchive_has_save(u64 titleId, FS_MediaType mediaType);

// Replace a title's save archive with the contents of a zip.
//
// Destructive and irreversible on the console side, so the caller must have
// confirmed the title. Writes are committed and the secure value cleared before
// returning; skipping either leaves the game with a save it will reject.
// `uniqueId` comes from the installed title and is needed for the secure value.
SaveArchiveResult savearchive_import(u64 titleId, FS_MediaType mediaType, u32 uniqueId, const char *zipPath);

// RomM hashes a zip as a composite: MD5 of "<name>:<md5>" lines for each entry,
// sorted by name and joined with newlines -- not MD5 of the archive bytes. A
// plain file hash would never match and every save would look changed.
bool savearchive_zip_content_hash(const char *zipPath, char out[SAVES_HASH_LEN]);

// Human-readable reason, for surfacing failures rather than a bare code.
const char *savearchive_result_text(SaveArchiveResult result);

#endif // SAVEARCHIVE_H
