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

typedef enum {
    SYNC_OP_NO_OP,
    SYNC_OP_UPLOAD,
    SYNC_OP_DOWNLOAD,
    SYNC_OP_CONFLICT
} SyncAction;

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
    char slot[8];
    char reason[128];
    char serverUpdatedAt[40];
    char serverHash[SAVES_HASH_LEN];

    // Filled in from the local scan so operations can be executed without
    // re-deriving paths.
    char localPath[SAVES_MAX_PATH];
    char platformSlug[CONFIG_MAX_SLUG_LEN];
    bool hasLocal;

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
