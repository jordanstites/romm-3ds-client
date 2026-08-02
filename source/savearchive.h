/*
 * 3DS save archive access
 *
 * A native 3DS save is a small filesystem inside the title's save archive, not
 * a file on the SD card. RomM stores one file per save, so the tree is packed
 * to a zip on the way out and unpacked on the way back.
 *
 * Zip layout matters beyond this app, and differs by kind. Savedata is written
 * inside the container Azahar keeps it in, which is what lets the same archive
 * round-trip console -> RomM -> emulator -> RomM -> console. Extdata is written
 * relative to its own root; no other client reads it yet, so there is nothing
 * to match.
 *
 * The two are separate save units -- a game may keep one, the other, or both,
 * and Fantasy Life keeps only extdata -- so they are exported and restored
 * independently rather than one standing in for the other.
 */

#ifndef SAVEARCHIVE_H
#define SAVEARCHIVE_H

#include "saves.h"
#include <3ds.h>
#include <stdbool.h>

// Which of a title's two possible save archives to act on.
typedef enum {
    SAVEARCHIVE_KIND_SAVEDATA,
    SAVEARCHIVE_KIND_EXTDATA,
} SaveArchiveKind;

typedef enum {
    SAVEARCHIVE_OK,
    SAVEARCHIVE_NOT_FOUND,   // title has no save archive
    SAVEARCHIVE_OPEN_FAILED, // archive exists but could not be opened
    SAVEARCHIVE_EMPTY,       // opened, but contains no files
    SAVEARCHIVE_IO_ERROR
} SaveArchiveResult;

// Pack one of a title's save archives into a zip at destPath. Returns
// SAVEARCHIVE_NOT_FOUND if the title does not use that kind, which is ordinary
// -- most titles have savedata and no extdata.
SaveArchiveResult savearchive_export(u64 titleId, FS_MediaType mediaType, SaveArchiveKind kind, const char *destPath);

// Whether a title currently has save data. Opens the archive and looks for one
// entry rather than exporting it, so it is cheap enough to call while drawing a
// list. No zip work, so it needs no oversized stack.
bool savearchive_has_save(u64 titleId, FS_MediaType mediaType, SaveArchiveKind kind);

// Replace a title's save archive with the contents of a zip.
//
// Destructive and irreversible on the console side, so the caller must have
// confirmed the title. Writes are committed and the secure value cleared before
// returning; skipping either leaves the game with a save it will reject.
// `uniqueId` comes from the installed title and is needed for the secure value.
SaveArchiveResult savearchive_import(u64 titleId, FS_MediaType mediaType, u32 uniqueId, SaveArchiveKind kind,
                                     const char *zipPath);

// RomM hashes a zip as a composite: MD5 of "<name>:<md5>" lines for each entry,
// sorted by name and joined with newlines -- not MD5 of the archive bytes. A
// plain file hash would never match and every save would look changed.
bool savearchive_zip_content_hash(const char *zipPath, char out[SAVES_HASH_LEN]);

// Which kind a title actually keeps its save in: savedata when it has one,
// extdata otherwise. A title with both reports savedata, that being the main
// save -- the sync path handles the two separately, but a single manual backup
// needs one answer.
SaveArchiveKind savearchive_primary_kind(u64 titleId, FS_MediaType mediaType);

// Human-readable reason, for surfacing failures rather than a bare code.
const char *savearchive_result_text(SaveArchiveResult result);

#endif // SAVEARCHIVE_H
