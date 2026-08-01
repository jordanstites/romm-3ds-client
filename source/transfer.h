/*
 * Background transfers
 *
 * Downloads used to run on the main thread, so the app was frozen for their
 * duration -- at roughly 2 MiB/s to the SD card, a multi-GB 3DS title meant
 * tens of minutes during which nothing could be browsed, queued or cancelled
 * cleanly.
 *
 * A worker thread now owns the transfer and publishes progress behind a lock.
 * The UI reads that snapshot once a frame and otherwise carries on as normal.
 */

#ifndef TRANSFER_H
#define TRANSFER_H

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#define TRANSFER_MAX_LABEL 256

typedef enum {
    TRANSFER_IDLE,
    TRANSFER_RUNNING,
    TRANSFER_SUCCEEDED,
    TRANSFER_FAILED,
    TRANSFER_CANCELLED,
} TransferState;

typedef enum {
    TRANSFER_KIND_DOWNLOAD, // to a file on the SD card
    TRANSFER_KIND_INSTALL,  // streamed into the CIA installer
} TransferKind;

typedef struct {
    TransferState state;
    TransferKind kind;
    uint64_t transferred;
    uint64_t total; // 0 while the size is still unknown
    char label[TRANSFER_MAX_LABEL];
    char detail[TRANSFER_MAX_LABEL];
} TransferStatus;

void transfer_init(void);
void transfer_exit(void);

// Begin a download to destPath. Returns false if a transfer is already running
// or the thread could not be started.
bool transfer_start_download(int romId, const char *fsName, const char *destPath, const char *label);

// Begin an install streamed straight into AM, never touching the SD card.
bool transfer_start_install(int romId, const char *fsName, const char *label);

// Snapshot of the worker's progress. Safe to call every frame.
void transfer_poll(TransferStatus *out);

// Ask the worker to stop. It finishes at the next chunk boundary, so the state
// does not change immediately.
void transfer_cancel(void);

bool transfer_is_active(void);

// Reap a finished transfer, returning it to idle. Call once the result has been
// shown, otherwise the next transfer cannot start.
void transfer_acknowledge(void);

#endif // TRANSFER_H
