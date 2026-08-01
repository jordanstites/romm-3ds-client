/*
 * Library index - which ROMs are actually on this SD card
 */

#include "library.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LIBRARY_PATH CONFIG_DIR "/library.tsv"

static LibraryEntry entries[LIBRARY_MAX_ENTRIES];
static int entryCount = 0;
static bool loaded = false;

static bool library_save(void) {
    mkdir(CONFIG_DIR, 0755);

    FILE *f = fopen(LIBRARY_PATH, "w");
    if (!f) {
        log_error("Could not write the library index: %s", LIBRARY_PATH);
        return false;
    }

    for (int i = 0; i < entryCount; i++) {
        fprintf(f, "%d\t%s\t%s\n", entries[i].romId, entries[i].platformSlug, entries[i].fsName);
    }

    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

void library_init(void) {
    if (loaded) return;
    loaded = true;
    entryCount = 0;

    FILE *f = fopen(LIBRARY_PATH, "r");
    if (!f) return;

    char line[512];
    while (entryCount < LIBRARY_MAX_ENTRIES && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        char *idText = line;
        char *slug = strchr(line, '\t');
        if (!slug) continue;
        *slug++ = '\0';

        char *fsName = strchr(slug, '\t');
        if (!fsName) continue;
        *fsName++ = '\0';

        if (idText[0] == '\0' || fsName[0] == '\0') continue;

        LibraryEntry *e = &entries[entryCount];
        e->romId = atoi(idText);
        snprintf(e->platformSlug, sizeof(e->platformSlug), "%s", slug);
        snprintf(e->fsName, sizeof(e->fsName), "%s", fsName);
        entryCount++;
    }

    fclose(f);
    log_info("Library index holds %d ROMs", entryCount);
}

bool library_record(int romId, const char *platformSlug, const char *fsName) {
    if (romId <= 0 || !platformSlug || !fsName) return false;
    library_init();

    // Replace in place if we already know this ROM -- redownloading should not
    // create a duplicate.
    for (int i = 0; i < entryCount; i++) {
        if (entries[i].romId == romId) {
            snprintf(entries[i].platformSlug, sizeof(entries[i].platformSlug), "%s", platformSlug);
            snprintf(entries[i].fsName, sizeof(entries[i].fsName), "%s", fsName);
            return library_save();
        }
    }

    if (entryCount >= LIBRARY_MAX_ENTRIES) {
        log_error("Library index is full (%d entries); not recording rom %d", LIBRARY_MAX_ENTRIES, romId);
        return false;
    }

    LibraryEntry *e = &entries[entryCount++];
    e->romId = romId;
    snprintf(e->platformSlug, sizeof(e->platformSlug), "%s", platformSlug);
    snprintf(e->fsName, sizeof(e->fsName), "%s", fsName);
    return library_save();
}

int library_count(void) {
    library_init();
    return entryCount;
}

const LibraryEntry *library_find(int romId) {
    library_init();
    for (int i = 0; i < entryCount; i++) {
        if (entries[i].romId == romId) return &entries[i];
    }
    return NULL;
}

const LibraryEntry *library_get(int index) {
    library_init();
    if (index < 0 || index >= entryCount) return NULL;
    return &entries[index];
}
