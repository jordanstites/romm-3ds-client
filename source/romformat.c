/*
 * ROM format probing
 */

#include "romformat.h"
#include "api.h"
#include "http.h"
#include "install.h"
#include "log.h"
#include <string.h>

// NCSD (a .3ds / .cci cartridge image), per 3dbrew. The 0x100 bytes before the
// magic are the RSA signature.
#define NCSD_MAGIC_OFFSET 0x100
#define NCSD_MEDIA_ID_OFFSET 0x108
#define NCSD_PARTITION_TABLE_OFFSET 0x120
// Offsets and lengths in the partition table are counted in media units.
#define NCSD_MEDIA_UNIT 0x200

// NCCH, the container each partition holds.
#define NCCH_MAGIC_OFFSET 0x100
#define NCCH_FLAGS_OFFSET 0x188
#define NCCH_FLAG_CRYPTO_METHOD 3
#define NCCH_FLAG_MISC 7
#define NCCH_MISC_FIXED_KEY 0x01
#define NCCH_MISC_NO_CRYPTO 0x04

// Crypto method values that need more than boot9's standard keyslot.
#define NCCH_CRYPTO_STANDARD 0x00
#define NCCH_CRYPTO_7X 0x01
#define NCCH_CRYPTO_SECURE3 0x0A // 9.6+ seed crypto
#define NCCH_CRYPTO_SECURE4 0x0B

// Enough for the NCSD header and its partition table.
#define NCSD_PROBE_BYTES 0x200

static u32 read_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 read_le64(const u8 *p) {
    return (u64)read_le32(p) | ((u64)read_le32(p + 4) << 32);
}

static bool magic_at(const u8 *data, size_t length, size_t offset, const char *magic) {
    if (offset + 4 > length) return false;
    return memcmp(data + offset, magic, 4) == 0;
}

const char *romformat_describe(const RomFormatInfo *info) {
    if (!info->probed) return "Format unknown (server unreachable)";

    switch (info->kind) {
    case ROMFORMAT_CIA:
        return "CIA - installable";
    case ROMFORMAT_NCSD:
        switch (info->crypto) {
        case ROMCRYPTO_NONE:
            return "3DS image, decrypted - convertible";
        case ROMCRYPTO_FIXED:
            return "3DS image, fixed key - convertible";
        case ROMCRYPTO_STANDARD:
            return "3DS image, encrypted - needs boot9";
        case ROMCRYPTO_SEED:
            return "3DS image, seed crypto - needs boot9 + seed";
        default:
            return "3DS image, unknown encryption";
        }
    default:
        return "Not a 3DS title";
    }
}

bool romformat_is_installable(const RomFormatInfo *info) {
    return info->probed && info->kind == ROMFORMAT_CIA;
}

// Reads the NCCH header of the first partition to learn how it is encrypted.
static void probe_ncch(const char *url, u32 partitionOffset, RomFormatInfo *out) {
    HttpResponse response;
    u64 from = partitionOffset;
    u64 to = from + NCSD_PROBE_BYTES;

    if (!http_get_range(url, from, to, &response)) return;

    if (response.statusCode >= 200 && response.statusCode < 300 && response.size > NCCH_FLAGS_OFFSET + 8) {
        const u8 *data = (const u8 *)response.data;

        if (magic_at(data, response.size, NCCH_MAGIC_OFFSET, "NCCH")) {
            const u8 *flags = data + NCCH_FLAGS_OFFSET;
            out->cryptoMethod = flags[NCCH_FLAG_CRYPTO_METHOD];
            u8 misc = flags[NCCH_FLAG_MISC];

            if (misc & NCCH_MISC_NO_CRYPTO) {
                out->crypto = ROMCRYPTO_NONE;
            } else if (misc & NCCH_MISC_FIXED_KEY) {
                out->crypto = ROMCRYPTO_FIXED;
            } else if (out->cryptoMethod == NCCH_CRYPTO_SECURE3 || out->cryptoMethod == NCCH_CRYPTO_SECURE4) {
                out->crypto = ROMCRYPTO_SEED;
            } else if (out->cryptoMethod == NCCH_CRYPTO_STANDARD || out->cryptoMethod == NCCH_CRYPTO_7X) {
                out->crypto = ROMCRYPTO_STANDARD;
            }
        } else {
            log_debug("No NCCH magic at partition offset 0x%lX", (unsigned long)partitionOffset);
        }
    }

    http_response_free(&response);
}

bool romformat_probe(int romId, const char *fsName, RomFormatInfo *out) {
    memset(out, 0, sizeof(RomFormatInfo));

    char url[1024];
    if (!api_build_content_url(romId, fsName, url, sizeof(url))) return false;

    // One request covers a CIA's ticket and an NCSD's header and partition
    // table, so the format can be identified without a second round trip.
    HttpResponse response;
    if (!http_get_range(url, 0, ROMFORMAT_PROBE_BYTES, &response)) {
        log_debug("Could not read the header of rom %d", romId);
        return false;
    }

    if (response.statusCode < 200 || response.statusCode >= 300) {
        log_debug("Header request for rom %d returned HTTP %ld", romId, response.statusCode);
        http_response_free(&response);
        return false;
    }

    out->probed = true;
    const u8 *data = (const u8 *)response.data;
    size_t size = response.size;

    if (magic_at(data, size, NCSD_MAGIC_OFFSET, "NCSD")) {
        out->kind = ROMFORMAT_NCSD;
        if (size >= NCSD_MEDIA_ID_OFFSET + 8) {
            out->titleId = read_le64(data + NCSD_MEDIA_ID_OFFSET);
        }

        // First partition holds the executable content; its offset is in media
        // units rather than bytes.
        u32 partitionOffset = 0;
        if (size >= NCSD_PARTITION_TABLE_OFFSET + 8) {
            partitionOffset = read_le32(data + NCSD_PARTITION_TABLE_OFFSET) * NCSD_MEDIA_UNIT;
        }

        if (partitionOffset > 0) {
            // Often within what we already fetched, in which case no second
            // request is needed.
            if ((size_t)partitionOffset + NCCH_FLAGS_OFFSET + 8 <= size &&
                magic_at(data, size, partitionOffset + NCCH_MAGIC_OFFSET, "NCCH")) {
                const u8 *flags = data + partitionOffset + NCCH_FLAGS_OFFSET;
                out->cryptoMethod = flags[NCCH_FLAG_CRYPTO_METHOD];
                u8 misc = flags[NCCH_FLAG_MISC];
                if (misc & NCCH_MISC_NO_CRYPTO) {
                    out->crypto = ROMCRYPTO_NONE;
                } else if (misc & NCCH_MISC_FIXED_KEY) {
                    out->crypto = ROMCRYPTO_FIXED;
                } else if (out->cryptoMethod == NCCH_CRYPTO_SECURE3 || out->cryptoMethod == NCCH_CRYPTO_SECURE4) {
                    out->crypto = ROMCRYPTO_SEED;
                } else {
                    out->crypto = ROMCRYPTO_STANDARD;
                }
            } else {
                http_response_free(&response);
                probe_ncch(url, partitionOffset, out);

                log_info("rom %d: %s (title %016llX, crypto flag 0x%02X)", romId, romformat_describe(out),
                         (unsigned long long)out->titleId, out->cryptoMethod);
                return true;
            }
        }
    } else {
        // Not an NCSD, so try the CIA layout.
        u64 titleId = install_title_id_from_header(data, size);
        if (titleId != 0) {
            out->kind = ROMFORMAT_CIA;
            out->titleId = titleId;
        }
    }

    http_response_free(&response);

    log_info("rom %d: %s (title %016llX, crypto flag 0x%02X)", romId, romformat_describe(out),
             (unsigned long long)out->titleId, out->cryptoMethod);
    return true;
}
