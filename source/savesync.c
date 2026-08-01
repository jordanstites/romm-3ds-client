/*
 * Save sync - client half of RomM's device sync protocol
 */

#include "savesync.h"
#include "api.h"
#include "cJSON/cJSON.h"
#include "http.h"
#include "library.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COPY_CHUNK_SIZE 8192

static void iso8601_from_unix(uint64_t seconds, char *out, size_t outLen) {
    time_t t = (time_t)seconds;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, outLen, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static SyncAction action_from_string(const char *value) {
    if (!value) return SYNC_OP_NO_OP;
    if (strcmp(value, "upload") == 0) return SYNC_OP_UPLOAD;
    if (strcmp(value, "download") == 0) return SYNC_OP_DOWNLOAD;
    if (strcmp(value, "conflict") == 0) return SYNC_OP_CONFLICT;
    return SYNC_OP_NO_OP;
}

static void copy_string_field(const cJSON *root, const char *key, char *dst, size_t dstLen) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(dst, dstLen, "%s", item->valuestring);
    } else {
        dst[0] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Negotiate
// ---------------------------------------------------------------------------

bool savesync_negotiate(const Config *config, const AuthToken *token, const LocalSave *saves, int saveCount,
                        SyncPlan *plan) {
    memset(plan, 0, sizeof(SyncPlan));

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    if (token->deviceId[0] != '\0') {
        cJSON_AddStringToObject(root, "device_id", token->deviceId);
    }

    cJSON *array = cJSON_AddArrayToObject(root, "saves");
    for (int i = 0; i < saveCount; i++) {
        const LocalSave *s = &saves[i];
        cJSON *entry = cJSON_CreateObject();

        cJSON_AddNumberToObject(entry, "rom_id", s->romId);
        cJSON_AddStringToObject(entry, "file_name", s->fileName);
        // Always send a slot. A null slot makes the server skip pairing
        // entirely and treat every save as an unconditional upload -- archival
        // behaviour that never resolves conflicts.
        cJSON_AddStringToObject(entry, "slot", s->slot);
        cJSON_AddStringToObject(entry, "content_hash", s->contentHash);

        char updatedAt[40];
        iso8601_from_unix(s->modifiedAt, updatedAt, sizeof(updatedAt));
        cJSON_AddStringToObject(entry, "updated_at", updatedAt);
        cJSON_AddNumberToObject(entry, "file_size_bytes", (double)s->sizeBytes);

        cJSON_AddItemToArray(array, entry);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;

    char url[512];
    snprintf(url, sizeof(url), "%s/api/sync/negotiate", config->serverUrl);

    HttpResponse response;
    bool sent = http_post_json(url, body, &response);
    free(body);

    if (!sent) {
        log_error("Could not reach the server to negotiate sync");
        return false;
    }

    if (response.statusCode != 200 && response.statusCode != 201) {
        log_error("Sync negotiate failed: HTTP %ld", response.statusCode);
        http_response_free(&response);
        return false;
    }

    cJSON *reply = cJSON_Parse(response.data);
    http_response_free(&response);
    if (!reply) {
        log_error("Sync negotiate response was not valid JSON");
        return false;
    }

    const cJSON *sessionId = cJSON_GetObjectItemCaseSensitive(reply, "session_id");
    plan->sessionId = cJSON_IsNumber(sessionId) ? sessionId->valueint : 0;

    const cJSON *operations = cJSON_GetObjectItemCaseSensitive(reply, "operations");
    if (cJSON_IsArray(operations)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, operations) {
            if (plan->operationCount >= SYNC_MAX_OPERATIONS) {
                log_error("Server returned more than %d operations; the rest are ignored", SYNC_MAX_OPERATIONS);
                break;
            }

            SyncOperation *op = &plan->operations[plan->operationCount];
            memset(op, 0, sizeof(SyncOperation));

            char action[32];
            copy_string_field(item, "action", action, sizeof(action));
            op->action = action_from_string(action);

            const cJSON *romId = cJSON_GetObjectItemCaseSensitive(item, "rom_id");
            op->romId = cJSON_IsNumber(romId) ? romId->valueint : 0;

            const cJSON *saveId = cJSON_GetObjectItemCaseSensitive(item, "save_id");
            op->saveId = cJSON_IsNumber(saveId) ? saveId->valueint : 0;

            copy_string_field(item, "file_name", op->fileName, sizeof(op->fileName));
            copy_string_field(item, "slot", op->slot, sizeof(op->slot));
            copy_string_field(item, "reason", op->reason, sizeof(op->reason));
            copy_string_field(item, "server_updated_at", op->serverUpdatedAt, sizeof(op->serverUpdatedAt));
            copy_string_field(item, "server_content_hash", op->serverHash, sizeof(op->serverHash));

            // Attach the local file, matching on (rom_id, slot) -- the same key
            // the server pairs on.
            for (int i = 0; i < saveCount; i++) {
                if (saves[i].romId == op->romId && strcmp(saves[i].slot, op->slot) == 0) {
                    snprintf(op->localPath, sizeof(op->localPath), "%s", saves[i].path);
                    op->hasLocal = true;
                    break;
                }
            }

            const LibraryEntry *entry = library_find(op->romId);
            if (entry) {
                snprintf(op->platformSlug, sizeof(op->platformSlug), "%s", entry->platformSlug);
            }

            switch (op->action) {
            case SYNC_OP_UPLOAD:
                plan->uploadCount++;
                break;
            case SYNC_OP_DOWNLOAD:
                plan->downloadCount++;
                break;
            case SYNC_OP_CONFLICT:
                plan->conflictCount++;
                break;
            default:
                plan->noOpCount++;
                break;
            }

            plan->operationCount++;
        }
    }

    cJSON_Delete(reply);

    log_info("Sync plan: %d upload, %d download, %d conflict, %d unchanged", plan->uploadCount, plan->downloadCount,
             plan->conflictCount, plan->noOpCount);
    return true;
}

// ---------------------------------------------------------------------------
// Executing operations
// ---------------------------------------------------------------------------

// Copy to <path>.bak before anything overwrites it. Save data is
// irreplaceable, so a download never clobbers without leaving a copy behind.
static bool backup_local_file(const char *path) {
    // Room for the suffix, so a long path cannot silently produce a truncated
    // backup name that overwrites something else.
    char backupPath[SAVES_MAX_PATH + 8];
    snprintf(backupPath, sizeof(backupPath), "%s.bak", path);

    FILE *src = fopen(path, "rb");
    if (!src) return true; // nothing to preserve

    FILE *dst = fopen(backupPath, "wb");
    if (!dst) {
        fclose(src);
        log_error("Could not create a backup at %s", backupPath);
        return false;
    }

    char buffer[COPY_CHUNK_SIZE];
    size_t read;
    bool ok = true;
    while ((read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, read, dst) != read) {
            ok = false;
            break;
        }
    }
    if (ferror(src)) ok = false;

    fclose(src);
    fclose(dst);

    if (!ok) {
        remove(backupPath);
        log_error("Backup of %s failed", path);
        return false;
    }

    log_info("Backed up %s", backupPath);
    return true;
}

static bool upload_save(const Config *config, const AuthToken *token, SyncOperation *op) {
    if (!op->hasLocal || op->localPath[0] == '\0') {
        log_error("Upload requested for rom %d but no local file was found", op->romId);
        return false;
    }

    // rom_id, slot and device_id are query parameters, not form fields; only
    // the file itself is multipart.
    char url[1024];
    snprintf(url, sizeof(url), "%s/api/saves?rom_id=%d&slot=%s&device_id=%s&overwrite=true", config->serverUrl,
             op->romId, op->slot, token->deviceId);

    HttpResponse response;
    if (!http_post_file(url, "saveFile", op->localPath, &response)) {
        log_error("Upload of %s failed at the transport", op->fileName);
        return false;
    }

    bool ok = response.statusCode >= 200 && response.statusCode < 300;
    if (!ok) {
        log_error("Upload of %s rejected: HTTP %ld", op->fileName, response.statusCode);
    } else {
        log_info("Uploaded %s", op->fileName);
    }

    http_response_free(&response);
    return ok;
}

static bool download_save(const Config *config, const AuthToken *token, SyncOperation *op) {
    if (op->saveId <= 0) {
        log_error("Download requested for rom %d but the server sent no save id", op->romId);
        return false;
    }

    // Work out where it belongs. If we already have a local copy use that exact
    // path; otherwise derive it from the platform layout.
    char destPath[SAVES_MAX_PATH];
    if (op->hasLocal && op->localPath[0] != '\0') {
        snprintf(destPath, sizeof(destPath), "%s", op->localPath);
    } else {
        const LibraryEntry *entry = library_find(op->romId);
        if (!entry) {
            // The server offers every save the account owns, including ones
            // written by other devices for games this console does not have.
            // There is nowhere sensible to put those, and it is not an error.
            log_info("Skipping save for rom %d: that game is not on this card", op->romId);
            op->skipped = true;
            return true;
        }
        if (!saves_build_path(config, entry->platformSlug, entry->fsName, op->slot, destPath, sizeof(destPath))) {
            log_error("No folder is configured for platform '%s'; set one to sync its saves", entry->platformSlug);
            return false;
        }
    }

    if (!backup_local_file(destPath)) {
        return false;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/api/saves/%d/content?device_id=%s", config->serverUrl, op->saveId, token->deviceId);

    if (!http_download_to_file(url, destPath, NULL)) {
        log_error("Download of %s failed", op->fileName);
        return false;
    }

    // Tell the server it landed, so its per-device sync watermark advances.
    char ackUrl[512];
    snprintf(ackUrl, sizeof(ackUrl), "%s/api/saves/%d/downloaded", config->serverUrl, op->saveId);
    HttpResponse ack;
    if (http_post_json(ackUrl, "{}", &ack)) {
        http_response_free(&ack);
    }

    log_info("Downloaded %s", op->fileName);
    return true;
}

bool savesync_execute(const Config *config, const AuthToken *token, SyncOperation *op) {
    op->failed = false;

    SyncAction effective = op->action;

    if (op->action == SYNC_OP_CONFLICT) {
        switch (op->resolution) {
        case SYNC_RESOLVE_KEEP_LOCAL:
            effective = SYNC_OP_UPLOAD;
            break;
        case SYNC_RESOLVE_KEEP_SERVER:
            effective = SYNC_OP_DOWNLOAD;
            break;
        case SYNC_RESOLVE_SKIP:
        case SYNC_RESOLVE_UNRESOLVED:
        default:
            // Never guess on a conflict. Both sides changed, and picking wrong
            // destroys data the user cannot get back.
            log_info("Skipping unresolved conflict for %s", op->fileName);
            op->done = false;
            return true;
        }
    }

    bool ok = true;
    switch (effective) {
    case SYNC_OP_UPLOAD:
        ok = upload_save(config, token, op);
        break;
    case SYNC_OP_DOWNLOAD:
        ok = download_save(config, token, op);
        break;
    case SYNC_OP_NO_OP:
    default:
        break;
    }

    if (op->skipped) {
        op->done = false;
        op->failed = false;
        return true;
    }

    op->done = ok;
    op->failed = !ok;
    return ok;
}

void savesync_complete(const Config *config, const SyncPlan *plan) {
    if (plan->sessionId <= 0) return;

    int completed = 0;
    int failed = 0;
    for (int i = 0; i < plan->operationCount; i++) {
        if (plan->operations[i].failed) {
            failed++;
        } else if (plan->operations[i].done) {
            completed++;
        }
        // Skips are deliberately reported as neither, so the server's session
        // record matches what actually happened.
    }

    char body[128];
    snprintf(body, sizeof(body), "{\"operations_completed\":%d,\"operations_failed\":%d}", completed, failed);

    char url[512];
    snprintf(url, sizeof(url), "%s/api/sync/sessions/%d/complete", config->serverUrl, plan->sessionId);

    HttpResponse response;
    if (http_post_json(url, body, &response)) {
        http_response_free(&response);
        log_info("Sync session %d closed: %d done, %d failed", plan->sessionId, completed, failed);
    } else {
        log_error("Could not close sync session %d", plan->sessionId);
    }
}
