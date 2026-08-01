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

static Result open_save_archive(u64 titleId, FS_MediaType mediaType, FS_Archive *archive) {
    u32 lowId = (u32)(titleId & 0xFFFFFFFF);
    u32 highId = (u32)(titleId >> 32);

    // ARCHIVE_SAVEDATA resolves the save from the calling process's own
    // exheader, so it can only ever open our own. USER_SAVEDATA takes an
    // explicit title, which is the whole point here.
    u32 path[3] = {(u32)mediaType, lowId, highId};
    FS_Path binaryPath = {PATH_BINARY, sizeof(path), path};

    return FSUSER_OpenArchive(archive, ARCHIVE_USER_SAVEDATA, binaryPath);
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

    zip_fileinfo info = {0};
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
        // an ordinary state rather than a failure worth alarming about.
        log_info("No save archive for %016llX (0x%08lX)", (unsigned long long)titleId, res);
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
    const char *path;
    char *hashOut;
    SaveArchiveResult result;
    bool ok;
} ZipJob;

static void export_entry(void *arg) {
    ZipJob *job = (ZipJob *)arg;
    job->result = export_impl(job->titleId, job->mediaType, job->path);
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

bool savearchive_zip_content_hash(const char *zipPath, char out[SAVES_HASH_LEN]) {
    out[0] = '\0';
    ZipJob job = {.path = zipPath, .hashOut = out, .ok = false};
    if (!run_with_big_stack(hash_entry, &job)) return false;
    return job.ok;
}
