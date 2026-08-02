/*
 * NCSD (.3ds/.cci) to CIA conversion
 */

#include "convert.h"
#include "ciabuild.h"
#include "install.h"
#include "log.h"
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Offsets are stored in media units throughout the NCSD header.
#define NCSD_MEDIA_UNIT 0x200

#define NCSD_MAGIC_OFFSET 0x100
#define NCSD_MEDIA_ID_OFFSET 0x108
#define NCSD_PARTITION_TABLE 0x120
#define NCSD_HEADER_BYTES 0x200

// Large enough that reading is not dominated by per-call overhead, small enough
// to sit comfortably on the heap alongside curl and AM.
#define CONVERT_CHUNK (64 * 1024)

const char *convert_result_text(ConvertResult result) {
    switch (result) {
    case CONVERT_OK:
        return "OK";
    case CONVERT_NOT_NCSD:
        return "This file is not a 3DS cartridge image.";
    case CONVERT_NO_PARTITIONS:
        return "This cartridge image has no installable partitions.";
    case CONVERT_IO_ERROR:
        return "Could not read the file from the SD card.";
    case CONVERT_INSTALL_FAILED:
        return "The install was refused. Check free space on the target.";
    case CONVERT_CANCELLED:
        return "Cancelled.";
    default:
        return "Conversion failed.";
    }
}

static u32 read_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 read_le64(const u8 *p) {
    return (u64)read_le32(p) | ((u64)read_le32(p + 4) << 32);
}

typedef struct {
    u64 offset; // bytes from the start of the file
    u64 size;   // bytes
    u16 index;
} Partition;

// Reads the partition table. Absent partitions have a zero size and are skipped
// rather than recorded as empty content, which AM would reject.
static ConvertResult read_header(FILE *f, u64 fileSize, CiaSpec *spec, Partition *parts, int *partCount) {
    u8 header[NCSD_HEADER_BYTES];

    if (fseek(f, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), f) != sizeof(header)) {
        return CONVERT_IO_ERROR;
    }

    if (memcmp(header + NCSD_MAGIC_OFFSET, "NCSD", 4) != 0) {
        return CONVERT_NOT_NCSD;
    }

    memset(spec, 0, sizeof(*spec));
    spec->titleId = read_le64(header + NCSD_MEDIA_ID_OFFSET);
    spec->titleVersion = 0;

    int count = 0;
    for (int i = 0; i < CIA_MAX_CONTENTS; i++) {
        const u8 *entry = header + NCSD_PARTITION_TABLE + (size_t)i * 8;
        u64 offset = (u64)read_le32(entry) * NCSD_MEDIA_UNIT;
        u64 size = (u64)read_le32(entry + 4) * NCSD_MEDIA_UNIT;

        if (size == 0) continue;

        // A truncated or padded dump would otherwise be hashed past the end of
        // the file, producing a CIA that installs and then fails to launch.
        if (offset >= fileSize || offset + size > fileSize) {
            log_error("Partition %d runs past the end of the file; the dump is incomplete", i);
            return CONVERT_IO_ERROR;
        }

        parts[count].offset = offset;
        parts[count].size = size;
        parts[count].index = (u16)i;

        spec->contents[count].index = (u16)i;
        spec->contents[count].size = (u32)size;
        count++;
    }

    if (count == 0) return CONVERT_NO_PARTITIONS;

    spec->contentCount = count;
    *partCount = count;
    return CONVERT_OK;
}

// Pass one: SHA-256 of each partition, into the spec the TMD is built from.
static ConvertResult hash_partitions(FILE *f, CiaSpec *spec, const Partition *parts, int partCount, u8 *buffer,
                                     u64 total, u64 *done, ConvertProgress onProgress, void *userdata) {
    for (int i = 0; i < partCount; i++) {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {
            mbedtls_sha256_free(&ctx);
            return CONVERT_IO_ERROR;
        }

        if (fseek(f, (long)parts[i].offset, SEEK_SET) != 0) {
            mbedtls_sha256_free(&ctx);
            return CONVERT_IO_ERROR;
        }

        u64 remaining = parts[i].size;
        while (remaining > 0) {
            size_t want = remaining < CONVERT_CHUNK ? (size_t)remaining : CONVERT_CHUNK;
            size_t got = fread(buffer, 1, want, f);
            if (got != want) {
                mbedtls_sha256_free(&ctx);
                return CONVERT_IO_ERROR;
            }

            mbedtls_sha256_update_ret(&ctx, buffer, got);
            remaining -= got;
            *done += got;

            if (onProgress && !onProgress(*done, total, userdata)) {
                mbedtls_sha256_free(&ctx);
                return CONVERT_CANCELLED;
            }
        }

        mbedtls_sha256_finish_ret(&ctx, spec->contents[i].hash);
        mbedtls_sha256_free(&ctx);
    }

    return CONVERT_OK;
}

// Pass two: the partitions again, this time straight into AM.
static ConvertResult write_partitions(FILE *f, CiaInstall *install, const Partition *parts, int partCount, u8 *buffer,
                                      u64 total, u64 *done, ConvertProgress onProgress, void *userdata) {
    for (int i = 0; i < partCount; i++) {
        if (fseek(f, (long)parts[i].offset, SEEK_SET) != 0) return CONVERT_IO_ERROR;

        u64 remaining = parts[i].size;
        while (remaining > 0) {
            size_t want = remaining < CONVERT_CHUNK ? (size_t)remaining : CONVERT_CHUNK;
            size_t got = fread(buffer, 1, want, f);
            if (got != want) return CONVERT_IO_ERROR;

            if (!install_write(install, buffer, got)) return CONVERT_INSTALL_FAILED;

            remaining -= got;
            *done += got;

            if (onProgress && !onProgress(*done, total, userdata)) return CONVERT_CANCELLED;
        }
    }

    return CONVERT_OK;
}

ConvertResult convert_install_ncsd(const char *path, ConvertProgress onProgress, void *userdata) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_error("Could not open %s", path);
        return CONVERT_IO_ERROR;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return CONVERT_IO_ERROR;
    }
    long fileSize = ftell(f);
    if (fileSize <= 0) {
        fclose(f);
        return CONVERT_IO_ERROR;
    }

    CiaSpec spec;
    Partition parts[CIA_MAX_CONTENTS];
    int partCount = 0;

    ConvertResult res = read_header(f, (u64)fileSize, &spec, parts, &partCount);
    if (res != CONVERT_OK) {
        fclose(f);
        return res;
    }

    u64 contentTotal = 0;
    for (int i = 0; i < partCount; i++) {
        contentTotal += parts[i].size;
    }

    log_info("Converting %016llX: %d partition(s), %llu MB", (unsigned long long)spec.titleId, partCount,
             (unsigned long long)(contentTotal / (1024 * 1024)));

    // Both passes read every byte, so the total is twice the content.
    u64 total = contentTotal * 2;
    u64 done = 0;

    u8 *buffer = malloc(CONVERT_CHUNK);
    if (!buffer) {
        fclose(f);
        return CONVERT_IO_ERROR;
    }

    res = hash_partitions(f, &spec, parts, partCount, buffer, total, &done, onProgress, userdata);
    if (res != CONVERT_OK) {
        free(buffer);
        fclose(f);
        return res;
    }

    size_t metadataSize = ciabuild_metadata_size(&spec);
    u8 *metadata = malloc(metadataSize);
    if (!metadata) {
        free(buffer);
        fclose(f);
        return CONVERT_IO_ERROR;
    }

    if (ciabuild_write_metadata(&spec, metadata, metadataSize) == 0) {
        free(metadata);
        free(buffer);
        fclose(f);
        log_error("Could not build the CIA metadata");
        return CONVERT_IO_ERROR;
    }

    // Only now is anything opened for writing: a failure before this point
    // leaves no half-installed title behind.
    CiaInstall install;
    FS_MediaType media = install_destination_for(spec.titleId);
    if (!install_begin(&install, media)) {
        free(metadata);
        free(buffer);
        fclose(f);
        return CONVERT_INSTALL_FAILED;
    }

    bool ok = install_write(&install, metadata, metadataSize);
    free(metadata);

    if (ok) {
        res = write_partitions(f, &install, parts, partCount, buffer, total, &done, onProgress, userdata);
    } else {
        res = CONVERT_INSTALL_FAILED;
    }

    free(buffer);
    fclose(f);

    if (res != CONVERT_OK) {
        // An import left open makes the next attempt fail, so it is closed even
        // on the cancel path.
        install_cancel(&install);
        return res;
    }

    if (!install_finish(&install)) {
        return CONVERT_INSTALL_FAILED;
    }

    log_info("Installed %016llX", (unsigned long long)spec.titleId);
    return CONVERT_OK;
}
