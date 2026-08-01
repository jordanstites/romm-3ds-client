/*
 * Local save discovery and hashing
 */

#include "saves.h"
#include "library.h"
#include "log.h"
#include <mbedtls/md5.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define HASH_CHUNK_SIZE 8192

// Extension a platform's saves use. TWiLight Menu++ and the emulators it ships
// are not consistent: most write .sav, the SNES and Genesis cores write .srm.
static const char *save_extension_for(const char *platformSlug) {
    if (strcmp(platformSlug, "snes") == 0 || strcmp(platformSlug, "sfam") == 0 ||
        strcmp(platformSlug, "genesis") == 0) {
        return "srm";
    }
    return "sav";
}

// DS and DSi saves live in a `saves/` subfolder; everything else sits beside
// the ROM. Introduced in TWiLight Menu++ v6.8.3 -- `roms/nds/saves/` and
// `roms/dsi/saves/` ship pre-created while `roms/gba/` deliberately does not.
static bool platform_uses_saves_subfolder(const char *platformSlug) {
    return strcmp(platformSlug, "nds") == 0 || strcmp(platformSlug, "nintendo-dsi") == 0;
}

// Strip the final extension: "Pokemon Sun.nds" -> "Pokemon Sun".
static void strip_extension(const char *fileName, char *out, size_t outLen) {
    snprintf(out, outLen, "%s", fileName);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

bool saves_hash_file(const char *path, char out[SAVES_HASH_LEN]) {
    out[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    if (mbedtls_md5_starts_ret(&ctx) != 0) {
        mbedtls_md5_free(&ctx);
        fclose(f);
        return false;
    }

    unsigned char buffer[HASH_CHUNK_SIZE];
    size_t read;
    bool ok = true;
    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (mbedtls_md5_update_ret(&ctx, buffer, read) != 0) {
            ok = false;
            break;
        }
    }
    if (ferror(f)) ok = false;
    fclose(f);

    unsigned char digest[16];
    if (ok && mbedtls_md5_finish_ret(&ctx, digest) != 0) ok = false;
    mbedtls_md5_free(&ctx);

    if (!ok) return false;

    for (int i = 0; i < 16; i++) {
        snprintf(&out[i * 2], 3, "%02x", digest[i]);
    }
    out[32] = '\0';
    return true;
}

// Directory holding a platform's ROMs, honouring the user's folder mapping.
static bool platform_rom_dir(const Config *config, const char *platformSlug, char *out, size_t outLen) {
    const char *folder = config_get_platform_folder(platformSlug);
    if (!folder || folder[0] == '\0') return false;
    snprintf(out, outLen, "%s/%s", config->romFolder, folder);
    return true;
}

bool saves_build_path(const Config *config, const char *platformSlug, const char *romFsName, const char *slot,
                      char *out, size_t outLen) {
    char romDir[SAVES_MAX_PATH];
    if (!platform_rom_dir(config, platformSlug, romDir, sizeof(romDir))) return false;

    char stem[SAVES_MAX_NAME];
    strip_extension(romFsName, stem, sizeof(stem));

    const char *ext = save_extension_for(platformSlug);

    // Slot 0 is the plain extension; higher slots append the number, matching
    // TWiLight Menu++'s getSavExtension().
    char suffix[8] = "";
    if (slot && slot[0] != '\0' && strcmp(slot, "0") != 0) {
        snprintf(suffix, sizeof(suffix), "%s", slot);
    }

    if (platform_uses_saves_subfolder(platformSlug)) {
        snprintf(out, outLen, "%s/saves/%s.%s%s", romDir, stem, ext, suffix);
    } else {
        snprintf(out, outLen, "%s/%s.%s%s", romDir, stem, ext, suffix);
    }
    return true;
}

int saves_scan(const Config *config, LocalSave *out, int maxSaves) {
    library_init();

    int found = 0;
    int romCount = library_count();

    for (int i = 0; i < romCount && found < maxSaves; i++) {
        const LibraryEntry *entry = library_get(i);
        if (!entry) continue;

        // Check slot 0 first, then numbered slots. Stopping at the first gap
        // would miss a save in slot 2 when slot 1 was deleted, so probe a fixed
        // small range instead.
        for (int slotNum = 0; slotNum <= 3 && found < maxSaves; slotNum++) {
            char slot[8];
            snprintf(slot, sizeof(slot), "%d", slotNum);

            char path[SAVES_MAX_PATH];
            if (!saves_build_path(config, entry->platformSlug, entry->fsName, slot, path, sizeof(path))) {
                break; // no folder mapping for this platform
            }

            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
                continue;
            }

            LocalSave *save = &out[found];
            memset(save, 0, sizeof(LocalSave));
            save->romId = entry->romId;
            snprintf(save->path, sizeof(save->path), "%s", path);
            snprintf(save->slot, sizeof(save->slot), "%s", slot);
            save->sizeBytes = (uint64_t)st.st_size;
            save->modifiedAt = (uint64_t)st.st_mtime;

            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            // Truncation is impossible in practice -- a basename cannot exceed
            // a FAT32 long name -- but the compiler cannot see that.
            snprintf(save->fileName, sizeof(save->fileName), "%.*s", (int)sizeof(save->fileName) - 1, base);

            if (!saves_hash_file(path, save->contentHash)) {
                log_error("Could not hash %s; skipping", path);
                continue;
            }

            log_debug("Save: rom %d slot %s %s (%llu bytes)", save->romId, save->slot, save->fileName,
                      (unsigned long long)save->sizeBytes);
            found++;
        }
    }

    log_info("Found %d local save(s) across %d indexed ROM(s)", found, romCount);
    return found;
}
