/*
 * CIA container construction
 */

#include "ciabuild.h"
#include <mbedtls/sha256.h>
#include <string.h>

// Every section is padded to a 64-byte boundary.
#define CIA_ALIGN 64

#define CIA_HEADER_SIZE 0x2020
#define CIA_TYPE 0x0000
#define CIA_VERSION 0x0000

// A retail certificate chain is 0xA00 bytes: three certificates of 0x400,
// 0x300 and 0x300. Left zeroed, like the signatures.
#define CERT_CHAIN_SIZE 0xA00

// Ticket: RSA-2048 SHA-256 signature (0x140 including padding) then 0x210 of
// ticket data.
#define TICKET_SIG_TYPE 0x00010004
#define TICKET_SIG_SIZE 0x140
#define TICKET_DATA_SIZE 0x210
#define TICKET_SIZE (TICKET_SIG_SIZE + TICKET_DATA_SIZE)
#define TICKET_OFF_TITLE_ID 0x9C
#define TICKET_OFF_TITLE_VERSION 0xA6

// TMD: same signature form, then a 0xC4 header, then 64 content-info records of
// 0x24, then one 0x30 chunk record per content.
#define TMD_SIG_TYPE 0x00010004
#define TMD_SIG_SIZE 0x140
#define TMD_HEADER_SIZE 0xC4
#define TMD_INFO_RECORDS 64
#define TMD_INFO_RECORD_SIZE 0x24
#define TMD_INFO_SIZE (TMD_INFO_RECORDS * TMD_INFO_RECORD_SIZE)
#define TMD_CHUNK_SIZE 0x30

#define TMD_OFF_TITLE_ID 0x4C
#define TMD_OFF_TITLE_VERSION 0x9C
#define TMD_OFF_CONTENT_COUNT 0x9E
#define TMD_OFF_INFO_HASH 0xA4

static u32 align_up(u32 value) {
    return (value + (CIA_ALIGN - 1)) & ~(u32)(CIA_ALIGN - 1);
}

static void write_le16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)(v >> 8);
}

static void write_le32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

static void write_le64(u8 *p, u64 v) {
    write_le32(p, (u32)(v & 0xFFFFFFFF));
    write_le32(p + 4, (u32)(v >> 32));
}

// Title IDs and most TMD/ticket fields are big-endian, unlike the CIA header
// around them.
static void write_be16(u8 *p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xFF);
}

static void write_be32(u8 *p, u32 v) {
    p[0] = (u8)((v >> 24) & 0xFF);
    p[1] = (u8)((v >> 16) & 0xFF);
    p[2] = (u8)((v >> 8) & 0xFF);
    p[3] = (u8)(v & 0xFF);
}

static void write_be64(u8 *p, u64 v) {
    write_be32(p, (u32)(v >> 32));
    write_be32(p + 4, (u32)(v & 0xFFFFFFFF));
}

static u32 tmd_size_for(int contentCount) {
    return TMD_SIG_SIZE + TMD_HEADER_SIZE + TMD_INFO_SIZE + (u32)contentCount * TMD_CHUNK_SIZE;
}

size_t ciabuild_metadata_size(const CiaSpec *spec) {
    return align_up(CIA_HEADER_SIZE) + align_up(CERT_CHAIN_SIZE) + align_up(TICKET_SIZE) +
           align_up(tmd_size_for(spec->contentCount));
}

size_t ciabuild_write_metadata(const CiaSpec *spec, u8 *out, size_t outSize) {
    if (!spec || !out || spec->contentCount <= 0 || spec->contentCount > CIA_MAX_CONTENTS) return 0;

    size_t needed = ciabuild_metadata_size(spec);
    if (outSize < needed) return 0;

    memset(out, 0, needed);

    u32 tmdSize = tmd_size_for(spec->contentCount);
    u64 contentSize = 0;
    for (int i = 0; i < spec->contentCount; i++) {
        contentSize += spec->contents[i].size;
    }

    // ---- header ----------------------------------------------------------
    u8 *header = out;
    write_le32(header + 0x00, CIA_HEADER_SIZE);
    write_le16(header + 0x04, CIA_TYPE);
    write_le16(header + 0x06, CIA_VERSION);
    write_le32(header + 0x08, CERT_CHAIN_SIZE);
    write_le32(header + 0x0C, TICKET_SIZE);
    write_le32(header + 0x10, tmdSize);
    write_le32(header + 0x14, 0); // no meta region
    write_le64(header + 0x18, contentSize);

    // Content index: a bitmap where the high bit of the first byte is index 0.
    // Without the matching bit set, AM ignores the content entirely.
    u8 *contentIndex = header + 0x20;
    for (int i = 0; i < spec->contentCount; i++) {
        u16 index = spec->contents[i].index;
        contentIndex[index / 8] |= (u8)(0x80 >> (index % 8));
    }

    // ---- certificate chain ----------------------------------------------
    // Left zeroed. Signature checking is patched out by the CFW this runs on.
    u8 *cert = out + align_up(CIA_HEADER_SIZE);

    // ---- ticket ----------------------------------------------------------
    u8 *ticket = cert + align_up(CERT_CHAIN_SIZE);
    write_be32(ticket, TICKET_SIG_TYPE);

    u8 *ticketData = ticket + TICKET_SIG_SIZE;
    write_be64(ticketData + TICKET_OFF_TITLE_ID, spec->titleId);
    write_be16(ticketData + TICKET_OFF_TITLE_VERSION, spec->titleVersion);

    // ---- TMD -------------------------------------------------------------
    u8 *tmd = ticket + align_up(TICKET_SIZE);
    write_be32(tmd, TMD_SIG_TYPE);

    u8 *tmdHeader = tmd + TMD_SIG_SIZE;
    write_be64(tmdHeader + TMD_OFF_TITLE_ID, spec->titleId);
    write_be16(tmdHeader + TMD_OFF_TITLE_VERSION, spec->titleVersion);
    write_be16(tmdHeader + TMD_OFF_CONTENT_COUNT, (u16)spec->contentCount);

    // Content chunk records follow the 64 info records.
    u8 *infoRecords = tmdHeader + TMD_HEADER_SIZE;
    u8 *chunks = infoRecords + TMD_INFO_SIZE;

    for (int i = 0; i < spec->contentCount; i++) {
        u8 *chunk = chunks + (size_t)i * TMD_CHUNK_SIZE;
        write_be32(chunk + 0x00, (u32)spec->contents[i].index); // content id
        write_be16(chunk + 0x04, spec->contents[i].index);      // content index

        // Type flags, and 0 is the right answer. Bit 0 means the content is
        // encrypted with the ticket's title key; setting it would make AM
        // decrypt content that was never encrypted, installing garbage. Checked
        // against a CIA makerom built from unencrypted content, which also
        // writes 0. Any NCCH-level encryption is a layer below this and is left
        // exactly as it was found.
        write_be16(chunk + 0x06, 0x0000);
        write_be64(chunk + 0x08, spec->contents[i].size);
        memcpy(chunk + 0x10, spec->contents[i].hash, CIA_SHA256_SIZE);
    }

    // The first info record covers every chunk: index 0, count N, and the
    // SHA-256 of the chunk records it covers. The remaining 63 stay zero.
    write_be16(infoRecords + 0x00, 0);
    write_be16(infoRecords + 0x02, (u16)spec->contentCount);
    mbedtls_sha256_ret(chunks, (size_t)spec->contentCount * TMD_CHUNK_SIZE, infoRecords + 0x04, 0);

    // And the TMD header carries the SHA-256 of the whole info-record block.
    // Both hashes are checked on install, so an omission here fails late and
    // opaquely rather than at build time.
    mbedtls_sha256_ret(infoRecords, TMD_INFO_SIZE, tmdHeader + TMD_OFF_INFO_HASH, 0);

    return needed;
}
