/*
 * Installed title enumeration
 */

#include "titles.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Title ID high word for a normal installed application. Updates (0x0004000E),
// DLC (0x0004008C), system titles and applets all share the enumeration but are
// not games, so they are filtered out.
#define TITLE_HIGH_APPLICATION 0x00040000

// SMDH layout, per 3dbrew. Only the parts we read are described precisely; the
// rest is skipped by offset so the struct stays small.
#define SMDH_MAGIC 0x48444D53 // 'SMDH' little-endian
#define SMDH_TITLE_ENTRY_SIZE 0x200
#define SMDH_TITLES_OFFSET 0x08
#define SMDH_SHORT_DESC_CHARS 0x40
// 16 language entries; index 1 is English. Index 0 is Japanese, which is a poor
// default for a mostly-English library.
#define SMDH_LANG_ENGLISH 1
#define SMDH_LANG_JAPANESE 0

static InstalledTitle titles[TITLES_MAX];
static int titleCount = 0;
static bool amReady = false;

bool titles_init(void) {
    if (amReady) return true;

    Result res = amInit();
    if (R_FAILED(res)) {
        log_error("amInit failed (0x%08lX); cannot list installed games", res);
        return false;
    }
    amReady = true;
    return true;
}

void titles_exit(void) {
    if (amReady) {
        amExit();
        amReady = false;
    }
}

// Fold a Latin-1 accented character onto its base letter. Without this,
// "Pokemon" on the server never matches "Pok\u00e9mon" in the SMDH -- which is
// precisely the title most likely to be looked up.
static char fold_latin1(u16 c) {
    if (c >= 0x00C0 && c <= 0x00C5) return 'A';
    if (c == 0x00C7) return 'C';
    if (c >= 0x00C8 && c <= 0x00CB) return 'E';
    if (c >= 0x00CC && c <= 0x00CF) return 'I';
    if (c == 0x00D1) return 'N';
    if ((c >= 0x00D2 && c <= 0x00D6) || c == 0x00D8) return 'O';
    if (c >= 0x00D9 && c <= 0x00DC) return 'U';
    if (c == 0x00DD) return 'Y';
    if (c >= 0x00E0 && c <= 0x00E5) return 'a';
    if (c == 0x00E7) return 'c';
    if (c >= 0x00E8 && c <= 0x00EB) return 'e';
    if (c >= 0x00EC && c <= 0x00EF) return 'i';
    if (c == 0x00F1) return 'n';
    if ((c >= 0x00F2 && c <= 0x00F6) || c == 0x00F8) return 'o';
    if (c >= 0x00F9 && c <= 0x00FC) return 'u';
    if (c == 0x00FD || c == 0x00FF) return 'y';

    // Punctuation that shows up in titles and would otherwise become '?'.
    if (c == 0x2019 || c == 0x2018) return '\'';
    if (c == 0x201C || c == 0x201D) return '"';
    if (c == 0x2013 || c == 0x2014) return '-';
    if (c == 0x00A0) return ' ';

    return 0;
}

// UTF-16LE -> ASCII, used for display and for matching against RomM names.
static void utf16_to_ascii(const u16 *src, int maxChars, char *out, size_t outLen) {
    size_t j = 0;
    for (int i = 0; i < maxChars && src[i] != 0 && j < outLen - 1; i++) {
        u16 c = src[i];
        if (c == '\n' || c == '\r') {
            // Short titles are frequently two lines; a space reads better.
            if (j > 0 && out[j - 1] != ' ') out[j++] = ' ';
            continue;
        }

        if (c < 0x80) {
            out[j++] = (char)c;
            continue;
        }

        char folded = fold_latin1(c);
        // Trademark and copyright signs are noise in a title; drop them rather
        // than leaving a '?' that would break matching.
        if (folded) {
            out[j++] = folded;
        } else if (c != 0x2122 && c != 0x00AE && c != 0x00A9) {
            out[j++] = '?';
        }
    }

    // Trim trailing space left by a line break at the end.
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
}

// Reads the title's icon (SMDH) straight out of its installed content. The
// archive path identifies the title; the file path is the fixed "icon" romfs
// entry. Both are binary paths, not text.
static bool read_title_name(u32 lowId, u32 highId, u8 media, char *out, size_t outLen) {
    out[0] = '\0';

    u32 archivePath[] = {lowId, highId, media, 0x00000000};
    static const u32 filePath[] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369 /* "icon" */, 0x00000000};

    FS_Path binArchivePath = {PATH_BINARY, sizeof(archivePath), archivePath};
    FS_Path binFilePath = {PATH_BINARY, sizeof(filePath), filePath};

    Handle file;
    Result res =
        FSUSER_OpenFileDirectly(&file, ARCHIVE_SAVEDATA_AND_CONTENT, binArchivePath, binFilePath, FS_OPEN_READ, 0);
    if (R_FAILED(res)) {
        log_debug("SMDH open failed for %08lX (0x%08lX)", (unsigned long)lowId, res);
        return false;
    }

    // Only the header and the English entry are needed, not the icon bitmaps,
    // which are several KB per title.
    size_t wanted = SMDH_TITLES_OFFSET + SMDH_TITLE_ENTRY_SIZE * (SMDH_LANG_ENGLISH + 1);
    u8 *buffer = malloc(wanted);
    if (!buffer) {
        FSFILE_Close(file);
        return false;
    }

    u32 read = 0;
    res = FSFILE_Read(file, &read, 0, buffer, (u32)wanted);
    FSFILE_Close(file);

    if (R_FAILED(res) || read < wanted) {
        log_debug("SMDH read short for %08lX: %lu of %u bytes (0x%08lX)", (unsigned long)lowId, (unsigned long)read,
                  (unsigned)wanted, res);
        free(buffer);
        return false;
    }

    u32 magic;
    memcpy(&magic, buffer, sizeof(magic));
    if (magic != SMDH_MAGIC) {
        log_debug("SMDH magic wrong for %08lX: %08lX", (unsigned long)lowId, (unsigned long)magic);
        free(buffer);
        return false;
    }

    const u16 *english = (const u16 *)(buffer + SMDH_TITLES_OFFSET + SMDH_TITLE_ENTRY_SIZE * SMDH_LANG_ENGLISH);
    utf16_to_ascii(english, SMDH_SHORT_DESC_CHARS, out, outLen);

    // Japan-only titles leave the English entry blank.
    if (out[0] == '\0') {
        const u16 *japanese = (const u16 *)(buffer + SMDH_TITLES_OFFSET + SMDH_TITLE_ENTRY_SIZE * SMDH_LANG_JAPANESE);
        utf16_to_ascii(japanese, SMDH_SHORT_DESC_CHARS, out, outLen);
    }

    free(buffer);
    return out[0] != '\0';
}

static void add_title(u64 id, FS_MediaType media) {
    if (titleCount >= TITLES_MAX) return;

    u32 highId = (u32)(id >> 32);
    u32 lowId = (u32)(id & 0xFFFFFFFF);

    if (highId != TITLE_HIGH_APPLICATION) return;

    InstalledTitle *t = &titles[titleCount];
    memset(t, 0, sizeof(InstalledTitle));
    t->titleId = id;
    t->highId = highId;
    t->lowId = lowId;
    t->uniqueId = (lowId >> 8) & 0xFFFFF;
    t->mediaType = media;

    if (R_FAILED(AM_GetTitleProductCode(media, id, t->productCode))) {
        t->productCode[0] = '\0';
    }
    t->productCode[TITLES_PRODUCT_CODE_LEN - 1] = '\0';

    if (!read_title_name(lowId, highId, (u8)media, t->name, sizeof(t->name))) {
        // Without a name there is nothing to match against a RomM entry, but
        // the title ID is still useful, so keep it and show the code.
        t->nameFromSmdh = false;
        snprintf(t->name, sizeof(t->name), "%s", t->productCode[0] ? t->productCode : "(unknown title)");
    } else {
        t->nameFromSmdh = true;
    }

    titleCount++;
}

static void scan_media(FS_MediaType media) {
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(media, &count)) || count == 0) return;

    u64 *ids = malloc(count * sizeof(u64));
    if (!ids) return;

    u32 read = 0;
    if (R_SUCCEEDED(AM_GetTitleList(&read, media, count, ids))) {
        for (u32 i = 0; i < read; i++) {
            add_title(ids[i], media);
        }
    }

    free(ids);
}

int titles_scan(void) {
    titleCount = 0;
    if (!titles_init()) return 0;

    scan_media(MEDIATYPE_SD);
    // A cartridge, if one is inserted, reports at most one title.
    scan_media(MEDIATYPE_GAME_CARD);

    int named = 0;
    for (int i = 0; i < titleCount; i++) {
        if (titles[i].nameFromSmdh) named++;
    }
    log_info("Found %d installed title(s), %d named; VC games included", titleCount, named);
    if (named < titleCount) {
        log_error("%d title(s) have no readable name; matching will fail for those", titleCount - named);
    }
    return titleCount;
}

int titles_count(void) {
    return titleCount;
}

const InstalledTitle *titles_get(int index) {
    if (index < 0 || index >= titleCount) return NULL;
    return &titles[index];
}

const InstalledTitle *titles_find(u64 titleId) {
    for (int i = 0; i < titleCount; i++) {
        if (titles[i].titleId == titleId) return &titles[i];
    }
    return NULL;
}
