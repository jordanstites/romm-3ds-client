/*
 * CIA installation via the AM service
 */

#include "install.h"
#include "log.h"
#include <string.h>

// CIA header layout, per 3dbrew. All sizes are little-endian and each section
// is padded to a 64-byte boundary.
#define CIA_HEADER_SIZE 0x2020
#define CIA_OFF_CERT_SIZE 0x08
#define CIA_OFF_TICKET_SIZE 0x0C
#define CIA_OFF_TMD_SIZE 0x10

// Within a ticket, after its signature. Ticket signatures vary in length; the
// RSA-2048 SHA-256 form (type 0x00010004) is what retail uses, and its
// signature block including padding is 0x140 bytes.
#define TICKET_SIG_RSA2048_SHA256 0x00010004
#define TICKET_SIG_RSA2048_SIZE 0x140
#define TICKET_TITLE_ID_OFFSET 0x9C

// Title ID high words that live in NAND rather than on the SD card.
#define TITLE_HIGH_DSIWARE 0x00048004
#define TITLE_HIGH_SYSTEM 0x00040010

static u32 read_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 read_be64(const u8 *p) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

// Sections are aligned to 64 bytes, so each offset is the running total rounded
// up rather than the raw sum.
static u32 align64(u32 value) {
    return (value + 63) & ~63u;
}

u64 install_title_id_from_header(const void *header, size_t length) {
    if (!header || length < CIA_HEADER_SIZE) return 0;

    const u8 *bytes = (const u8 *)header;

    u32 headerSize = read_le32(bytes);
    if (headerSize != CIA_HEADER_SIZE) return 0;

    u32 certSize = read_le32(bytes + CIA_OFF_CERT_SIZE);
    u32 ticketSize = read_le32(bytes + CIA_OFF_TICKET_SIZE);
    if (ticketSize == 0) return 0;

    u32 ticketOffset = align64(headerSize) + align64(certSize);
    if ((size_t)ticketOffset + TICKET_SIG_RSA2048_SIZE + TICKET_TITLE_ID_OFFSET + 8 > length) return 0;

    const u8 *ticket = bytes + ticketOffset;
    if (read_be64(ticket) >> 32 != TICKET_SIG_RSA2048_SHA256) {
        // Some other signature type; the title ID offset would differ and
        // guessing at it risks installing to the wrong media.
        return 0;
    }

    // Title IDs are stored big-endian inside the ticket.
    return read_be64(ticket + TICKET_SIG_RSA2048_SIZE + TICKET_TITLE_ID_OFFSET);
}

FS_MediaType install_destination_for(u64 titleId) {
    u32 high = (u32)(titleId >> 32);
    if (high == TITLE_HIGH_DSIWARE || high == TITLE_HIGH_SYSTEM) {
        return MEDIATYPE_NAND;
    }
    return MEDIATYPE_SD;
}

bool install_begin(CiaInstall *install, FS_MediaType media) {
    memset(install, 0, sizeof(CiaInstall));
    install->media = media;

    Result res = AM_StartCiaInstall(media, &install->handle);
    if (R_FAILED(res)) {
        log_error("Could not start the install (0x%08lX)", res);
        return false;
    }

    install->active = true;
    return true;
}

bool install_write(CiaInstall *install, const void *data, size_t length) {
    if (!install->active || length == 0) return install->active;

    u32 written = 0;
    Result res = FSFILE_Write(install->handle, &written, install->written, data, (u32)length, 0);
    if (R_FAILED(res)) {
        log_error("Install write failed at %llu bytes (0x%08lX)", (unsigned long long)install->written, res);
        return false;
    }
    if (written != length) {
        log_error("Install write was short: %lu of %u bytes", (unsigned long)written, (unsigned)length);
        return false;
    }

    install->written += written;
    return true;
}

bool install_finish(CiaInstall *install) {
    if (!install->active) return false;

    Result res = AM_FinishCiaInstall(install->handle);
    install->active = false;

    if (R_FAILED(res)) {
        log_error("Install failed to commit (0x%08lX)", res);
        return false;
    }

    log_info("Installed %llu bytes", (unsigned long long)install->written);
    return true;
}

void install_cancel(CiaInstall *install) {
    if (!install->active) return;

    // Cancelling releases the handle and discards the partial title; without it
    // the import stays open and the next attempt fails.
    Result res = AM_CancelCIAInstall(install->handle);
    if (R_FAILED(res)) {
        log_error("Could not cancel the install (0x%08lX)", res);
    }
    install->active = false;
}
