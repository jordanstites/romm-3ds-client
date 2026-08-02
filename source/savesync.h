/*
 * Save sync - client half of RomM's device sync protocol
 *
 * The server does the comparison. We send an inventory of local saves, it
 * returns a list of operations, we execute them and report back. Deliberately
 * not a locally-invented compare loop: matching the documented protocol is what
 * makes saves round-trip with Grout, Argosy and anything else pointed at the
 * same RomM.
 */

#ifndef SAVESYNC_H
#define SAVESYNC_H

#include "auth.h"
#include "config.h"
#include "saves.h"
#include <stdbool.h>

#define SYNC_MAX_OPERATIONS 256

typedef enum { SYNC_OP_NO_OP, SYNC_OP_UPLOAD, SYNC_OP_DOWNLOAD, SYNC_OP_CONFLICT } SyncAction;

typedef enum {
    SYNC_RESOLVE_UNRESOLVED,
    SYNC_RESOLVE_KEEP_LOCAL,  // upload ours, server copy is superseded
    SYNC_RESOLVE_KEEP_SERVER, // download theirs, ours is backed up first
    SYNC_RESOLVE_SKIP
} SyncResolution;

typedef struct {
    SyncAction action;
    int romId;
    int saveId; // server-side asset id; 0 when the server has no copy
    char fileName[SAVES_MAX_NAME];
    // Wide enough for a named channel, not just a digit: native archives use
    // "3ds" and "extdata".
    char slot[16];
    char reason[128];
    char serverUpdatedAt[40];
    char serverHash[SAVES_HASH_LEN];

    // Filled in from the local scan so operations can be executed without
    // re-deriving paths.
    char localPath[SAVES_MAX_PATH];
    char platformSlug[CONFIG_MAX_SLUG_LEN];
    bool hasLocal;

    // Copied from the matching LocalSave so execution knows whether to replace
    // a file or write back a save archive.
    bool nativeArchive;
    // Which of the title's two archives this is. Both can exist for one
    // title, so restoring has to write back to the one it came from.
    bool extdata;
    unsigned long long titleId;
    unsigned int uniqueId;
    int mediaType;

    // Cleared to skip this operation. Every entry starts selected, so the
    // default is still "sync everything", but nothing has to be all-or-nothing
    // -- a save you do not want touched can be dropped before anything runs.
    bool selected;

    SyncResolution resolution; // only meaningful for SYNC_OP_CONFLICT
    bool done;
    bool failed;
    // Nothing went wrong, there was just nothing to do here -- most often a
    // save for a game that lives on another device but not on this card.
    bool skipped;
} SyncOperation;

typedef struct {
    int sessionId;
    SyncOperation operations[SYNC_MAX_OPERATIONS];
    int operationCount;
    int uploadCount;
    int downloadCount;
    int conflictCount;
    int noOpCount;
} SyncPlan;

// Gather every save this console holds: files next to ROMs for the emulated
// platforms, plus the save archives of any native 3DS titles that have been
// linked to a ROM. Native archives are staged to zips under CONFIG_DIR, which
// savesync_cleanup() removes once the session is over.
// Declare this console's sync intent to the server: an API client that both
// pushes and pulls. The device record itself is created by pairing, but that
// leaves sync_mode unset, and the protocol expects a device to say how it
// syncs. Idempotent, and a failure is not fatal.
void savesync_register_device(const Config *config, const AuthToken *token);

int savesync_collect(const Config *config, LocalSave *out, int maxSaves);

// Delete staged archive zips left by savesync_collect().
void savesync_cleanup(const LocalSave *saves, int saveCount);

// Ask the server what needs doing. Returns false on transport or auth failure.
bool savesync_negotiate(const Config *config, const AuthToken *token, const LocalSave *saves, int saveCount,
                        SyncPlan *plan);

// Execute one operation. Conflicts must have a resolution set first; an
// unresolved conflict is skipped. Returns false on failure, with the operation
// marked failed.
bool savesync_execute(const Config *config, const AuthToken *token, SyncOperation *op);

// Tell the server the session is over. Best effort -- a failure here does not
// undo work already done.
void savesync_complete(const Config *config, const SyncPlan *plan);

#endif // SAVESYNC_H
