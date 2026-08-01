/*
 * CIA installation via the AM service
 *
 * AM_StartCiaInstall hands back an ordinary filesystem handle to write into,
 * which means a CIA can be piped straight from the network into the installer.
 * Nothing needs to land on the SD card, so the FAT32 4GB per-file limit does
 * not apply to installs and the card is spared writing a copy it would only
 * delete afterwards.
 *
 * amInit() acquires am:net, which Luma3DS grants to .3dsx homebrew, so this
 * needs no CIA build of our own.
 */

#ifndef INSTALL_H
#define INSTALL_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    Handle handle;
    bool active;
    u64 written;
    FS_MediaType media;
} CiaInstall;

// Open an install. Fails if the AM service is unavailable.
bool install_begin(CiaInstall *install, FS_MediaType media);

// Append bytes. Must be called with the CIA's bytes in order.
bool install_write(CiaInstall *install, const void *data, size_t length);

// Commit. After this the title is installed and the handle is closed.
bool install_finish(CiaInstall *install);

// Abandon a partial install and release the handle. Safe to call on an install
// that already finished or never started.
void install_cancel(CiaInstall *install);

// How much of a CIA must be read to reach the ticket. A retail CIA puts it at
// 0x2A40 -- the 0x2020 header aligned up to 0x2040, plus a 0xA00 certificate
// chain -- and the title ID is a further 0x1DC bytes into it. Anything less
// silently fails to find the ID.
#define INSTALL_HEADER_PROBE_BYTES 0x4000

// Reads the title ID from the front of a CIA so the caller can send the title
// to the right media. `header` must hold at least INSTALL_HEADER_PROBE_BYTES.
// Returns 0 if the layout is not recognised.
u64 install_title_id_from_header(const void *header, size_t length);

// Whether a title ID belongs on the SD card or in NAND.
FS_MediaType install_destination_for(u64 titleId);

#endif // INSTALL_H
