/*
 * CIA container construction
 *
 * Builds the metadata that wraps NCCH content into an installable CIA: header,
 * certificate chain, ticket, and TMD. Turning a .3ds into something AM will
 * accept is mostly this plus copying the partitions across.
 *
 * Signatures are left zeroed. Custom firmware does not verify them, which is
 * the same assumption every offline converter makes -- a correctly signed CIA
 * is not something that can be produced without Nintendo's keys.
 *
 * The awkward constraint is ordering: the TMD carries a SHA-256 of each
 * content, but sits before the content in the file. So the content has to be
 * hashed before any of this can be written, which means two passes over the
 * source rather than a single stream.
 */

#ifndef CIABUILD_H
#define CIABUILD_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// A CIA can hold up to 8 contents; a .3ds has at most 8 partitions.
#define CIA_MAX_CONTENTS 8
#define CIA_SHA256_SIZE 32

typedef struct {
    u16 index; // partition index it came from
    u32 size;  // bytes
    u8 hash[CIA_SHA256_SIZE];
} CiaContent;

typedef struct {
    u64 titleId;
    u16 titleVersion;
    CiaContent contents[CIA_MAX_CONTENTS];
    int contentCount;
} CiaSpec;

// Total size of the metadata that precedes the content.
size_t ciabuild_metadata_size(const CiaSpec *spec);

// Serialise header, certificate chain, ticket and TMD into `out`, which must be
// at least ciabuild_metadata_size() bytes. Returns bytes written, or 0.
size_t ciabuild_write_metadata(const CiaSpec *spec, u8 *out, size_t outSize);

#endif // CIABUILD_H
