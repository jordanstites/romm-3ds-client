/*
 * 3DS save archive access
 */

#include "savearchive.h"
#include "log.h"
#include <mbedtls/md5.h>
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARCHIVE_PATH_MAX 512
#define COPY_CHUNK 8192
#define MAX_ENTRIES 256

// minizip's zipOpen3 allocates a ~64KB zip64_internal on the stack, and unzip
// is comparable. The main thread's stack is far smaller than that, so calling
// either from it faults on the first write past the end. Both run on a
// dedicated thread with room to spare instead.
#define ZIP_THREAD_STACK (192 * 1024)

const char *savearchive_result_text(SaveArchiveResult result) {
    switch (result) {
    case SAVEARCHIVE_OK:
        return "OK";
    case SAVEARCHIVE_NOT_FOUND:
        return "This title has no save data yet. Play it once and save.";
    case SAVEARCHIVE_OPEN_FAILED:
        return "Could not open the save archive.";
    case SAVEARCHIVE_EMPTY:
        return "The save archive is empty.";
    case SAVEARCHIVE_IO_ERROR:
    default:
        return "Failed while reading the save archive.";
    }
}

// ---------------------------------------------------------------------------
// Archive access
// ---------------------------------------------------------------------------

// Reports what the card slot is doing, when a cartridge save fails to open.
//
// A cartridge is not like an installed title: the card can be absent, powered
// down, or a DS card whose save is on an SPI chip rather than in a 3DS save
// archive. Those look identical from the archive call alone, which returns a
// generic FS error for all of them.
static void log_card_state(void) {
    bool inserted = false;
    if (R_FAILED(FSUSER_CardSlotIsInserted(&inserted))) {
        log_info("  card slot state unavailable");
        return;
    }
    if (!inserted) {
        log_info("  no card inserted");
        return;
    }

    bool powered = false;
    Result res = FSUSER_CardSlotPowerOn(&powered);
    log_info("  card inserted, powered=%d (%s)", (int)powered, R_SUCCEEDED(res) ? "ok" : "power query failed");

    FS_CardType type;
    if (R_SUCCEEDED(FSUSER_GetCardType(&type))) {
        // A DS card keeps its save on a cartridge SPI chip, which this archive
        // API cannot reach at all -- that is a different subsystem, not a
        // permissions problem.
        log_info("  card type: %s", type == CARD_TWL ? "DS (save is on cart SPI, not readable here)" : "3DS");
    }
}

static Result open_save_archive_once(u64 titleId, FS_MediaType mediaType, FS_Archive *archive) {
    u32 lowId = (u32)(titleId & 0xFFFFFFFF);
    u32 highId = (u32)(titleId >> 32);

    // A cartridge has its own archive. USER_SAVEDATA addresses a title by id on
    // a given media, which is how an installed title is reached, but a card's
    // save is not stored that way -- GAMECARD_SAVEDATA refers to whatever card
    // is currently inserted, so it takes no title and an empty path.
    if (mediaType == MEDIATYPE_GAME_CARD) {
        Result res = FSUSER_OpenArchive(archive, ARCHIVE_GAMECARD_SAVEDATA, fsMakePath(PATH_EMPTY, ""));
        if (R_SUCCEEDED(res)) return res;

        log_debug("Gamecard archive refused (%s); trying it as a titled save", log_result_text(res));
        // Fall through: a card-installed title may still answer to the normal
        // form, and failing both is more informative than failing one.
    }

    // ARCHIVE_SAVEDATA resolves the save from the calling process's own
    // exheader, so it can only ever open our own. USER_SAVEDATA takes an
    // explicit title, which is the whole point here.
    u32 path[3] = {(u32)mediaType, lowId, highId};
    FS_Path binaryPath = {PATH_BINARY, sizeof(path), path};

    return FSUSER_OpenArchive(archive, ARCHIVE_USER_SAVEDATA, binaryPath);
}

// Summary 9 is "canceled": the operation was abandoned rather than refused or
// found missing.
#define RESULT_SUMMARY_CANCELED 9

static u32 result_summary(Result res) {
    return ((u32)res >> 21) & 0x3F;
}

static Result open_save_archive(u64 titleId, FS_MediaType mediaType, FS_Archive *archive) {
    Result res = open_save_archive_once(titleId, mediaType, archive);
    if (R_SUCCEEDED(res) || mediaType != MEDIATYPE_GAME_CARD) return res;

    // A card inserted after the console booted leaves this process holding a
    // stale view of the slot, and FS reports that as "canceled" rather than as
    // a missing archive or a refusal. Powering the slot on makes the card
    // current; retrying then usually succeeds. Only worth doing for that
    // specific summary -- a genuinely absent save should fail immediately
    // rather than be retried.
    if (result_summary(res) != RESULT_SUMMARY_CANCELED) return res;

    bool powered = false;
    if (R_FAILED(FSUSER_CardSlotPowerOn(&powered))) return res;

    log_info("Card slot re-mounted; retrying the save archive");
    return open_save_archive_once(titleId, mediaType, archive);
}

// UTF-16 -> ASCII for entry names. 3DS save trees use plain ASCII names in
// practice; anything else is replaced rather than truncating the name.
static void entry_name_to_ascii(const u16 *src, char *out, size_t outLen) {
    size_t j = 0;
    for (size_t i = 0; src[i] != 0 && j < outLen - 1; i++) {
        out[j++] = (src[i] < 0x80) ? (char)src[i] : '_';
    }
    out[j] = '\0';
}

// Copy one archive file into the open zip entry.
static bool add_file_to_zip(FS_Archive archive, const char *archivePath, const char *zipName, zipFile zf) {
    FS_Path filePath = fsMakePath(PATH_ASCII, archivePath);

    Handle file;
    if (R_FAILED(FSUSER_OpenFile(&file, archive, filePath, FS_OPEN_READ, 0))) {
        log_error("Could not open %s inside the save archive", archivePath);
        return false;
    }

    u64 size = 0;
    if (R_FAILED(FSFILE_GetSize(file, &size))) {
        FSFILE_Close(file);
        return false;
    }

    // A zeroed zip_fileinfo writes month 0, day 0, which is not a valid DOS
    // date -- some tools reject it outright. The archive carries no per-file
    // timestamp we can read, so stamp with the current time.
    zip_fileinfo info = {0};
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if (local) {
        info.tmz_date.tm_sec = local->tm_sec;
        info.tmz_date.tm_min = local->tm_min;
        info.tmz_date.tm_hour = local->tm_hour;
        info.tmz_date.tm_mday = local->tm_mday;
        info.tmz_date.tm_mon = local->tm_mon;
        info.tmz_date.tm_year = local->tm_year + 1900;
    }

    if (zipOpenNewFileInZip(zf, zipName, &info, NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK) {
        FSFILE_Close(file);
        return false;
    }

    u8 *buffer = malloc(COPY_CHUNK);
    if (!buffer) {
        zipCloseFileInZip(zf);
        FSFILE_Close(file);
        return false;
    }

    bool ok = true;
    u64 offset = 0;
    while (offset < size) {
        u32 want = (u32)((size - offset > COPY_CHUNK) ? COPY_CHUNK : (size - offset));
        u32 read = 0;
        if (R_FAILED(FSFILE_Read(file, &read, offset, buffer, want)) || read == 0) {
            ok = false;
            break;
        }
        if (zipWriteInFileInZip(zf, buffer, read) != ZIP_OK) {
            ok = false;
            break;
        }
        offset += read;
    }

    free(buffer);
    zipCloseFileInZip(zf);
    FSFILE_Close(file);
    return ok;
}

// Per-level working memory for the directory walk. Kept on the heap and reused
// rather than on the stack: four path buffers plus a directory entry per frame
// is roughly 2.6KB, and recursing that on a thread whose stack is shared with
// the graphics stack is how this crashed.
typedef struct {
    FS_Archive archive;
    zipFile zf;
    int fileCount;
    char path[ARCHIVE_PATH_MAX];   // path within the archive, built in place
    char prefix[ARCHIVE_PATH_MAX]; // matching path within the zip
    FS_DirectoryEntry entry;
} WalkState;

// Walks the directory currently described by state->path / state->prefix,
// restoring both before returning so the caller's view is unchanged.
static bool add_directory(WalkState *state, int depth) {
    // Save trees are shallow. A deeper one means something is wrong, and
    // recursing further risks the stack rather than reporting it.
    if (depth > 6) {
        log_error("Save archive nests deeper than expected; stopping at %s", state->path);
        return false;
    }

    FS_Path fsPath = fsMakePath(PATH_ASCII, state->path);
    Handle dir;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, state->archive, fsPath))) {
        log_error("Could not open directory '%s' in the save archive", state->path);
        return false;
    }

    size_t pathLen = strlen(state->path);
    size_t prefixLen = strlen(state->prefix);

    bool ok = true;
    u32 read = 1;

    while (ok && R_SUCCEEDED(FSDIR_Read(dir, &read, 1, &state->entry)) && read > 0) {
        char name[256];
        entry_name_to_ascii(state->entry.name, name, sizeof(name));
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        size_t nameLen = strlen(name);
        if (pathLen + nameLen + 2 >= ARCHIVE_PATH_MAX || prefixLen + nameLen + 2 >= ARCHIVE_PATH_MAX) {
            log_error("Path too long inside the save archive: %s%s", state->path, name);
            ok = false;
            break;
        }

        // Append in place, then truncate back for the next sibling.
        memcpy(state->path + pathLen, name, nameLen + 1);
        memcpy(state->prefix + prefixLen, name, nameLen + 1);

        bool isDir = (state->entry.attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir) {
            state->path[pathLen + nameLen] = '/';
            state->path[pathLen + nameLen + 1] = '\0';
            state->prefix[prefixLen + nameLen] = '/';
            state->prefix[prefixLen + nameLen + 1] = '\0';
            ok = add_directory(state, depth + 1);
        } else {
            if (state->fileCount >= MAX_ENTRIES) {
                log_error("Save archive holds more than %d files; refusing to continue", MAX_ENTRIES);
                ok = false;
            } else if (add_file_to_zip(state->archive, state->path, state->prefix, state->zf)) {
                state->fileCount++;
            } else {
                ok = false;
            }
        }

        state->path[pathLen] = '\0';
        state->prefix[prefixLen] = '\0';
    }

    FSDIR_Close(dir);
    return ok;
}

static SaveArchiveResult export_impl(u64 titleId, FS_MediaType mediaType, const char *destPath) {
    FS_Archive archive;
    Result res = open_save_archive(titleId, mediaType, &archive);
    if (R_FAILED(res)) {
        // A title that has never been played has no archive to open, which is
        // an ordinary state rather than a failure worth alarming about. The
        // media type is logged because a cartridge title uses a different one
        // and getting it wrong looks identical to having no save.
        log_info("No save for %016llX media %u", (unsigned long long)titleId, (unsigned)mediaType);
        log_info("  %s", log_result_text(res));
        if (mediaType == MEDIATYPE_GAME_CARD) {
            log_card_state();
        }
        return SAVEARCHIVE_NOT_FOUND;
    }

    zipFile zf = zipOpen(destPath, APPEND_STATUS_CREATE);
    if (!zf) {
        FSUSER_CloseArchive(archive);
        log_error("Could not create %s", destPath);
        return SAVEARCHIVE_IO_ERROR;
    }

    WalkState *state = calloc(1, sizeof(WalkState));
    if (!state) {
        zipClose(zf, NULL);
        FSUSER_CloseArchive(archive);
        remove(destPath);
        return SAVEARCHIVE_IO_ERROR;
    }

    state->archive = archive;
    state->zf = zf;
    snprintf(state->path, sizeof(state->path), "/");
    state->prefix[0] = '\0';

    bool ok = add_directory(state, 0);
    int fileCount = state->fileCount;
    free(state);

    zipClose(zf, NULL);
    FSUSER_CloseArchive(archive);

    if (!ok) {
        remove(destPath);
        return SAVEARCHIVE_IO_ERROR;
    }

    if (fileCount == 0) {
        remove(destPath);
        return SAVEARCHIVE_EMPTY;
    }

    log_info("Exported %d file(s) from %016llX", fileCount, (unsigned long long)titleId);
    return SAVEARCHIVE_OK;
}

bool savearchive_has_save(u64 titleId, FS_MediaType mediaType) {
    FS_Archive archive;
    if (R_FAILED(open_save_archive(titleId, mediaType, &archive))) return false;

    Handle dir;
    bool hasEntry = false;
    if (R_SUCCEEDED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, "/")))) {
        FS_DirectoryEntry entry;
        u32 read = 0;
        // An archive can exist while holding nothing, which is not a save.
        while (R_SUCCEEDED(FSDIR_Read(dir, &read, 1, &entry)) && read > 0) {
            if (!(entry.attributes & FS_ATTRIBUTE_DIRECTORY)) {
                hasEntry = true;
                break;
            }
        }
        FSDIR_Close(dir);
    }

    FSUSER_CloseArchive(archive);
    return hasEntry;
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

// Create any parent directories the entry needs. The archive will not create
// them implicitly, and a save tree can legitimately contain subdirectories.
static void ensure_parent_dirs(FS_Archive archive, const char *entryName) {
    char path[ARCHIVE_PATH_MAX];
    snprintf(path, sizeof(path), "/%.*s", (int)sizeof(path) - 2, entryName);

    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, path), 0);
        *p = '/';
    }
}

static bool write_entry(FS_Archive archive, const char *entryName, const u8 *data, u32 size) {
    char path[ARCHIVE_PATH_MAX];
    snprintf(path, sizeof(path), "/%.*s", (int)sizeof(path) - 2, entryName);

    ensure_parent_dirs(archive, entryName);

    FS_Path fsPath = fsMakePath(PATH_ASCII, path);

    // Delete first: writing over a longer existing file would leave the tail of
    // the old save behind, which for a fixed-size save is silent corruption.
    FSUSER_DeleteFile(archive, fsPath);

    Result res = FSUSER_CreateFile(archive, fsPath, 0, (u64)size);
    if (R_FAILED(res)) {
        log_error("Could not create %s in the save archive (0x%08lX)", path, res);
        return false;
    }

    Handle file;
    res = FSUSER_OpenFile(&file, archive, fsPath, FS_OPEN_WRITE, 0);
    if (R_FAILED(res)) {
        log_error("Could not open %s for writing (0x%08lX)", path, res);
        return false;
    }

    u32 written = 0;
    res = FSFILE_Write(file, &written, 0, data, size, FS_WRITE_FLUSH);
    FSFILE_Close(file);

    if (R_FAILED(res) || written != size) {
        log_error("Short write to %s: %lu of %lu bytes", path, (unsigned long)written, (unsigned long)size);
        return false;
    }
    return true;
}

static SaveArchiveResult import_impl(u64 titleId, FS_MediaType mediaType, u32 uniqueId, const char *zipPath) {
    unzFile uf = unzOpen(zipPath);
    if (!uf) {
        log_error("Could not open %s", zipPath);
        return SAVEARCHIVE_IO_ERROR;
    }

    FS_Archive archive;
    Result res = open_save_archive(titleId, mediaType, &archive);
    if (R_FAILED(res)) {
        unzClose(uf);
        log_error("Could not open the save archive for %016llX (0x%08lX)", (unsigned long long)titleId, res);
        return SAVEARCHIVE_OPEN_FAILED;
    }

    bool ok = true;
    int written = 0;

    if (unzGoToFirstFile(uf) == UNZ_OK) {
        do {
            unz_file_info info;
            char name[256];
            if (unzGetCurrentFileInfo(uf, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK) {
                ok = false;
                break;
            }

            size_t len = strlen(name);
            if (len == 0 || name[len - 1] == '/') continue;

            // A zip entry escaping the save root would write outside the
            // archive; refuse rather than sanitising into something surprising.
            if (name[0] == '/' || strstr(name, "..") != NULL) {
                log_error("Refusing suspicious entry '%s'", name);
                ok = false;
                break;
            }

            if (unzOpenCurrentFile(uf) != UNZ_OK) {
                ok = false;
                break;
            }

            u32 size = (u32)info.uncompressed_size;
            u8 *data = malloc(size ? size : 1);
            if (!data) {
                unzCloseCurrentFile(uf);
                ok = false;
                break;
            }

            int read = unzReadCurrentFile(uf, data, size);
            unzCloseCurrentFile(uf);

            if (read < 0 || (u32)read != size) {
                free(data);
                ok = false;
                break;
            }

            ok = write_entry(archive, name, data, size);
            free(data);
            if (!ok) break;
            written++;
        } while (unzGoToNextFile(uf) == UNZ_OK);
    }

    unzClose(uf);

    if (ok && written > 0) {
        // Mandatory, and in this order. The archive is journalled: without the
        // commit every write above is silently discarded, with no error.
        res = FSUSER_ControlArchive(archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
        if (R_FAILED(res)) {
            log_error("Commit failed (0x%08lX) -- the save was NOT written", res);
            ok = false;
        }
    }

    FSUSER_CloseArchive(archive);

    if (ok && written > 0) {
        // The secure value is anti-rollback: the game compares its stored value
        // against the system's and treats a mismatch as a restored backup,
        // which Pokemon titles report as a corrupted save. Deleting it makes
        // the system forget the expected value so the game regenerates one.
        u8 existed = 0;
        u64 secureValue = ((u64)SECUREVALUE_SLOT_SD << 32) | ((u64)uniqueId << 8);
        res = FSUSER_ControlSecureSave(SECURESAVE_ACTION_DELETE, &secureValue, sizeof(secureValue), &existed,
                                       sizeof(existed));
        if (R_FAILED(res)) {
            // The data is already committed, so this is a warning rather than a
            // failure -- but the game may refuse the save until it is cleared.
            log_error("Could not clear the secure value (0x%08lX); the game may report a corrupted save", res);
        } else {
            log_info("Secure value cleared (existed: %u)", existed);
        }
    }

    if (!ok) return SAVEARCHIVE_IO_ERROR;
    if (written == 0) return SAVEARCHIVE_EMPTY;

    log_info("Restored %d file(s) to %016llX", written, (unsigned long long)titleId);
    return SAVEARCHIVE_OK;
}

// ---------------------------------------------------------------------------
// Composite hash
// ---------------------------------------------------------------------------

typedef struct {
    char name[256];
    char hash[SAVES_HASH_LEN];
} ZipEntryHash;

static int compare_entry_names(const void *a, const void *b) {
    return strcmp(((const ZipEntryHash *)a)->name, ((const ZipEntryHash *)b)->name);
}

static void md5_hex(const unsigned char *data, size_t len, char out[SAVES_HASH_LEN]) {
    unsigned char digest[16];
    mbedtls_md5_ret(data, len, digest);
    for (int i = 0; i < 16; i++) {
        snprintf(&out[i * 2], 3, "%02x", digest[i]);
    }
    out[32] = '\0';
}

static bool zip_content_hash_impl(const char *zipPath, char out[SAVES_HASH_LEN]) {
    out[0] = '\0';

    unzFile uf = unzOpen(zipPath);
    if (!uf) return false;

    ZipEntryHash *entries = calloc(MAX_ENTRIES, sizeof(ZipEntryHash));
    if (!entries) {
        unzClose(uf);
        return false;
    }

    int count = 0;
    bool ok = true;

    if (unzGoToFirstFile(uf) == UNZ_OK) {
        do {
            if (count >= MAX_ENTRIES) break;

            unz_file_info info;
            char name[256];
            if (unzGetCurrentFileInfo(uf, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK) {
                ok = false;
                break;
            }

            // Directory entries are excluded, matching the server, which hashes
            // only names that do not end in a slash.
            size_t len = strlen(name);
            if (len == 0 || name[len - 1] == '/') continue;

            if (unzOpenCurrentFile(uf) != UNZ_OK) {
                ok = false;
                break;
            }

            unsigned char *content = malloc(info.uncompressed_size ? info.uncompressed_size : 1);
            if (!content) {
                unzCloseCurrentFile(uf);
                ok = false;
                break;
            }

            int read = unzReadCurrentFile(uf, content, (unsigned)info.uncompressed_size);
            unzCloseCurrentFile(uf);

            if (read < 0) {
                free(content);
                ok = false;
                break;
            }

            snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
            md5_hex(content, (size_t)read, entries[count].hash);
            free(content);
            count++;
        } while (unzGoToNextFile(uf) == UNZ_OK);
    }

    unzClose(uf);

    if (!ok || count == 0) {
        free(entries);
        return false;
    }

    // Sorted by name, then joined as "<name>:<md5>" lines with newline
    // separators, and the whole string hashed again.
    qsort(entries, count, sizeof(ZipEntryHash), compare_entry_names);

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts_ret(&ctx);

    for (int i = 0; i < count; i++) {
        char line[320];
        int len = snprintf(line, sizeof(line), "%s%s:%s", i == 0 ? "" : "\n", entries[i].name, entries[i].hash);
        mbedtls_md5_update_ret(&ctx, (const unsigned char *)line, (size_t)len);
    }

    unsigned char digest[16];
    mbedtls_md5_finish_ret(&ctx, digest);
    mbedtls_md5_free(&ctx);
    free(entries);

    for (int i = 0; i < 16; i++) {
        snprintf(&out[i * 2], 3, "%02x", digest[i]);
    }
    out[32] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Threaded entry points
//
// Everything above touches minizip, which needs far more stack than the main
// thread has. Running it on a dedicated thread is cheaper and less fragile than
// replacing the zip implementation, and joins immediately so callers keep their
// straight-line control flow.
// ---------------------------------------------------------------------------

typedef struct {
    u64 titleId;
    FS_MediaType mediaType;
    u32 uniqueId;
    const char *path;
    char *hashOut;
    SaveArchiveResult result;
    bool ok;
} ZipJob;

static void export_entry(void *arg) {
    ZipJob *job = (ZipJob *)arg;
    job->result = export_impl(job->titleId, job->mediaType, job->path);
}

static void import_entry(void *arg) {
    ZipJob *job = (ZipJob *)arg;
    job->result = import_impl(job->titleId, job->mediaType, job->uniqueId, job->path);
}

static void hash_entry(void *arg) {
    ZipJob *job = (ZipJob *)arg;
    job->ok = zip_content_hash_impl(job->path, job->hashOut);
}

// Runs `entry` on a thread with a large stack and waits for it.
static bool run_with_big_stack(ThreadFunc entry, ZipJob *job) {
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);

    // Core -2 is the default processor from the exheader; the application core
    // is always available without an APT time limit.
    Thread thread = threadCreate(entry, job, ZIP_THREAD_STACK, priority, -2, false);
    if (!thread) {
        log_error("Could not start the archive thread");
        return false;
    }

    threadJoin(thread, U64_MAX);
    threadFree(thread);
    return true;
}

SaveArchiveResult savearchive_export(u64 titleId, FS_MediaType mediaType, const char *destPath) {
    ZipJob job = {.titleId = titleId, .mediaType = mediaType, .path = destPath, .result = SAVEARCHIVE_IO_ERROR};
    if (!run_with_big_stack(export_entry, &job)) return SAVEARCHIVE_IO_ERROR;
    return job.result;
}

SaveArchiveResult savearchive_import(u64 titleId, FS_MediaType mediaType, u32 uniqueId, const char *zipPath) {
    ZipJob job = {.titleId = titleId,
                  .mediaType = mediaType,
                  .uniqueId = uniqueId,
                  .path = zipPath,
                  .result = SAVEARCHIVE_IO_ERROR};
    if (!run_with_big_stack(import_entry, &job)) return SAVEARCHIVE_IO_ERROR;
    return job.result;
}

bool savearchive_zip_content_hash(const char *zipPath, char out[SAVES_HASH_LEN]) {
    out[0] = '\0';
    ZipJob job = {.path = zipPath, .hashOut = out, .ok = false};
    if (!run_with_big_stack(hash_entry, &job)) return false;
    return job.ok;
}
