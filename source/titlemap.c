/*
 * Title mapping - which installed 3DS title a RomM ROM corresponds to
 */

#include "titlemap.h"
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TITLEMAP_PATH CONFIG_DIR "/titlemap.tsv"

static TitleMapping mappings[TITLEMAP_MAX];
static int mappingCount = 0;
static bool loaded = false;

static bool titlemap_save(void) {
    mkdir(CONFIG_DIR, 0755);

    FILE *f = fopen(TITLEMAP_PATH, "w");
    if (!f) {
        log_error("Could not write the title map: %s", TITLEMAP_PATH);
        return false;
    }

    for (int i = 0; i < mappingCount; i++) {
        fprintf(f, "%d\t%016llX\n", mappings[i].romId, (unsigned long long)mappings[i].titleId);
    }

    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

void titlemap_init(void) {
    if (loaded) return;
    loaded = true;
    mappingCount = 0;

    FILE *f = fopen(TITLEMAP_PATH, "r");
    if (!f) return;

    char line[128];
    while (mappingCount < TITLEMAP_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';

        int romId = atoi(line);
        u64 titleId = strtoull(tab, NULL, 16);
        if (romId <= 0 || titleId == 0) continue;

        mappings[mappingCount].romId = romId;
        mappings[mappingCount].titleId = titleId;
        mappingCount++;
    }

    fclose(f);
    log_info("Title map holds %d link(s)", mappingCount);
}

static void remove_at(int index) {
    for (int i = index; i < mappingCount - 1; i++) {
        mappings[i] = mappings[i + 1];
    }
    mappingCount--;
}

bool titlemap_set(int romId, u64 titleId) {
    if (romId <= 0 || titleId == 0) return false;
    titlemap_init();

    // A ROM maps to one title and a title to one ROM. Drop any existing link on
    // either side first, otherwise a correction would leave the old pairing
    // behind and two ROMs could claim the same save archive.
    for (int i = mappingCount - 1; i >= 0; i--) {
        if (mappings[i].romId == romId || mappings[i].titleId == titleId) {
            remove_at(i);
        }
    }

    if (mappingCount >= TITLEMAP_MAX) {
        log_error("Title map is full (%d entries)", TITLEMAP_MAX);
        return false;
    }

    mappings[mappingCount].romId = romId;
    mappings[mappingCount].titleId = titleId;
    mappingCount++;

    log_info("Linked rom %d to title %016llX", romId, (unsigned long long)titleId);
    return titlemap_save();
}

bool titlemap_clear(int romId) {
    titlemap_init();
    for (int i = 0; i < mappingCount; i++) {
        if (mappings[i].romId == romId) {
            remove_at(i);
            return titlemap_save();
        }
    }
    return true;
}

u64 titlemap_get_title(int romId) {
    titlemap_init();
    for (int i = 0; i < mappingCount; i++) {
        if (mappings[i].romId == romId) return mappings[i].titleId;
    }
    return 0;
}

int titlemap_get_rom(u64 titleId) {
    titlemap_init();
    for (int i = 0; i < mappingCount; i++) {
        if (mappings[i].titleId == titleId) return mappings[i].romId;
    }
    return 0;
}

int titlemap_count(void) {
    titlemap_init();
    return mappingCount;
}
