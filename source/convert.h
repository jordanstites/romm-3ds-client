/*
 * NCSD (.3ds/.cci) to CIA conversion
 *
 * A cartridge image and an installable title hold the same content in different
 * wrappers. An NCSD is a partition table pointing at up to eight NCCH
 * partitions; a CIA is a header, ticket and TMD followed by those same
 * partitions as content. Converting is repackaging, not re-encoding, so nothing
 * here rewrites a single byte of a partition.
 *
 * The awkward part is ordering. The TMD carries a SHA-256 of every content but
 * sits ahead of them in the file, so the hashes have to be known before any
 * output exists. That means two passes over the source: one to hash, one to
 * write. The source is read from the SD card rather than the network for that
 * reason -- a second pass over an HTTP body would mean downloading twice.
 *
 * Content is streamed straight into AM, so no CIA is ever written to the card
 * and the FAT32 4GB file limit does not apply to the output.
 */

#ifndef CONVERT_H
#define CONVERT_H

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CONVERT_OK,
    CONVERT_NOT_NCSD,      // not a cartridge image, or an unreadable header
    CONVERT_NO_PARTITIONS, // header parsed but nothing to install
    CONVERT_IO_ERROR,      // could not read the source
    CONVERT_INSTALL_FAILED,
    CONVERT_CANCELLED,
} ConvertResult;

// Reports bytes hashed and written against the total for both passes, so a
// progress bar advances over the whole operation rather than resetting at the
// halfway point. Returning false cancels.
typedef bool (*ConvertProgress)(uint64_t done, uint64_t total, void *userdata);

// Convert the NCSD at `path` and install it. Handles the whole sequence:
// reading the partition table, hashing, building the CIA metadata, and
// streaming into AM.
ConvertResult convert_install_ncsd(const char *path, ConvertProgress onProgress, void *userdata);

// Human-readable reason, for surfacing a failure rather than a bare code.
const char *convert_result_text(ConvertResult result);

#endif // CONVERT_H
