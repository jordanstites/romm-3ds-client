/*
 * Background transfers
 */

#include "transfer.h"
#include "api.h"
#include "http.h"
#include "install.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Generous because curl and, for installs, the AM service both sit on this
// stack. The zip work needed 192KB for similar reasons.
#define TRANSFER_STACK_SIZE (128 * 1024)

typedef struct {
    // Everything here is read by the UI thread and written by the worker, so
    // all access goes through `lock`.
    LightLock lock;
    TransferStatus status;
    volatile bool cancelRequested;

    Thread thread;
    bool threadRunning;

    // Owned by the worker for the duration of a transfer.
    char url[1024];
    char destPath[512];
    CiaInstall install;
} Transfer;

static Transfer transfer;

void transfer_init(void) {
    memset(&transfer, 0, sizeof(transfer));
    LightLock_Init(&transfer.lock);
}

static void set_state(TransferState state, const char *detail) {
    LightLock_Lock(&transfer.lock);
    transfer.status.state = state;
    if (detail) {
        snprintf(transfer.status.detail, sizeof(transfer.status.detail), "%s", detail);
    }
    LightLock_Unlock(&transfer.lock);
}

// Called from the worker for every chunk. Returning false aborts the transfer,
// which is how cancellation takes effect.
static bool on_progress(uint64_t transferred, uint64_t total) {
    LightLock_Lock(&transfer.lock);
    transfer.status.transferred = transferred;
    transfer.status.total = total;
    bool keepGoing = !transfer.cancelRequested;
    LightLock_Unlock(&transfer.lock);
    return keepGoing;
}

static bool install_sink(const void *data, size_t length, void *userdata) {
    return install_write((CiaInstall *)userdata, data, length);
}

static void worker(void *arg) {
    (void)arg;

    TransferKind kind;
    LightLock_Lock(&transfer.lock);
    kind = transfer.status.kind;
    LightLock_Unlock(&transfer.lock);

    bool ok = false;

    if (kind == TRANSFER_KIND_DOWNLOAD) {
        ok = http_download_to_file(transfer.url, transfer.destPath, on_progress);
    } else {
        // The destination media depends on the title ID, which is in the CIA's
        // ticket -- read it before opening the install so a DSiWare or system
        // title is not sent to the SD card.
        FS_MediaType media = MEDIATYPE_SD;
        HttpResponse header;
        if (http_get_range(transfer.url, 0, INSTALL_HEADER_PROBE_BYTES, &header)) {
            if (header.statusCode >= 200 && header.statusCode < 300) {
                u64 titleId = install_title_id_from_header(header.data, header.size);
                if (titleId != 0) media = install_destination_for(titleId);
            }
            http_response_free(&header);
        }

        if (install_begin(&transfer.install, media)) {
            ok = http_download_to_sink(transfer.url, install_sink, &transfer.install, on_progress);
            if (ok) {
                ok = install_finish(&transfer.install);
            } else {
                // A partial import left open makes the next attempt fail.
                install_cancel(&transfer.install);
            }
        }
    }

    LightLock_Lock(&transfer.lock);
    if (transfer.cancelRequested) {
        transfer.status.state = TRANSFER_CANCELLED;
        snprintf(transfer.status.detail, sizeof(transfer.status.detail), "Cancelled");
    } else if (ok) {
        transfer.status.state = TRANSFER_SUCCEEDED;
        snprintf(transfer.status.detail, sizeof(transfer.status.detail), "Done");
    } else {
        transfer.status.state = TRANSFER_FAILED;
        snprintf(transfer.status.detail, sizeof(transfer.status.detail), "Failed - see the log");
    }
    LightLock_Unlock(&transfer.lock);
}

static bool start(TransferKind kind, int romId, const char *fsName, const char *destPath, const char *label) {
    if (transfer_is_active()) {
        log_error("A transfer is already running");
        return false;
    }

    // A finished thread still holds its handle until reaped.
    if (transfer.threadRunning) {
        threadJoin(transfer.thread, U64_MAX);
        threadFree(transfer.thread);
        transfer.threadRunning = false;
    }

    if (!api_build_content_url(romId, fsName, transfer.url, sizeof(transfer.url))) {
        return false;
    }
    snprintf(transfer.destPath, sizeof(transfer.destPath), "%s", destPath ? destPath : "");

    LightLock_Lock(&transfer.lock);
    memset(&transfer.status, 0, sizeof(transfer.status));
    transfer.status.state = TRANSFER_RUNNING;
    transfer.status.kind = kind;
    snprintf(transfer.status.label, sizeof(transfer.status.label), "%s", label ? label : "");
    snprintf(transfer.status.detail, sizeof(transfer.status.detail), "%s",
             kind == TRANSFER_KIND_INSTALL ? "Installing" : "Downloading");
    transfer.cancelRequested = false;
    LightLock_Unlock(&transfer.lock);

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);

    // One step lower than the UI so drawing stays responsive while the transfer
    // saturates whatever is left. Core -2 is the default processor; the
    // application core always permits thread creation, unlike the system core
    // which would need APT_SetAppCpuTimeLimit first.
    transfer.thread = threadCreate(worker, NULL, TRANSFER_STACK_SIZE, priority + 1, -2, false);
    if (!transfer.thread) {
        log_error("Could not start the transfer thread");
        set_state(TRANSFER_FAILED, "Could not start");
        return false;
    }

    transfer.threadRunning = true;
    return true;
}

bool transfer_start_download(int romId, const char *fsName, const char *destPath, const char *label) {
    return start(TRANSFER_KIND_DOWNLOAD, romId, fsName, destPath, label);
}

bool transfer_start_install(int romId, const char *fsName, const char *label) {
    return start(TRANSFER_KIND_INSTALL, romId, fsName, NULL, label);
}

void transfer_poll(TransferStatus *out) {
    LightLock_Lock(&transfer.lock);
    *out = transfer.status;
    LightLock_Unlock(&transfer.lock);
}

void transfer_cancel(void) {
    LightLock_Lock(&transfer.lock);
    if (transfer.status.state == TRANSFER_RUNNING) {
        transfer.cancelRequested = true;
        snprintf(transfer.status.detail, sizeof(transfer.status.detail), "Cancelling...");
    }
    LightLock_Unlock(&transfer.lock);
}

bool transfer_is_active(void) {
    LightLock_Lock(&transfer.lock);
    bool active = transfer.status.state == TRANSFER_RUNNING;
    LightLock_Unlock(&transfer.lock);
    return active;
}

void transfer_acknowledge(void) {
    if (transfer.threadRunning) {
        threadJoin(transfer.thread, U64_MAX);
        threadFree(transfer.thread);
        transfer.threadRunning = false;
    }

    LightLock_Lock(&transfer.lock);
    memset(&transfer.status, 0, sizeof(transfer.status));
    transfer.status.state = TRANSFER_IDLE;
    transfer.cancelRequested = false;
    LightLock_Unlock(&transfer.lock);
}

void transfer_exit(void) {
    transfer_cancel();
    if (transfer.threadRunning) {
        threadJoin(transfer.thread, U64_MAX);
        threadFree(transfer.thread);
        transfer.threadRunning = false;
    }
}
