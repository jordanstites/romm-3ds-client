/*
 * Per-ROM status for the browser
 */

#include "romstatus.h"
#include "api.h"
#include "cJSON/cJSON.h"
#include "http.h"
#include "log.h"
#include "saves.h"
#include "titles.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_TRACKED_ROMS 1024

typedef struct {
    int romId;
    int saveCount;
} SaveTally;

static SaveTally tallies[MAX_TRACKED_ROMS];
static int tallyCount = 0;
static int loadedPlatformId = -1;

void romstatus_invalidate(void) {
    tallyCount = 0;
    loadedPlatformId = -1;
}

static void tally_add(int romId) {
    for (int i = 0; i < tallyCount; i++) {
        if (tallies[i].romId == romId) {
            tallies[i].saveCount++;
            return;
        }
    }
    if (tallyCount >= MAX_TRACKED_ROMS) return;
    tallies[tallyCount].romId = romId;
    tallies[tallyCount].saveCount = 1;
    tallyCount++;
}

static int tally_get(int romId) {
    for (int i = 0; i < tallyCount; i++) {
        if (tallies[i].romId == romId) return tallies[i].saveCount;
    }
    return 0;
}

bool romstatus_load_platform(int platformId) {
    if (loadedPlatformId == platformId) return true;

    tallyCount = 0;
    loadedPlatformId = platformId;

    char url[512];
    snprintf(url, sizeof(url), "%s/api/saves?platform_id=%d", api_get_base_url(), platformId);

    HttpResponse response;
    if (!http_get(url, &response)) {
        log_error("Could not fetch save counts for platform %d", platformId);
        return false;
    }

    if (response.statusCode != 200) {
        log_error("Save counts for platform %d: HTTP %ld", platformId, response.statusCode);
        http_response_free(&response);
        return false;
    }

    cJSON *root = cJSON_Parse(response.data);
    http_response_free(&response);
    if (!root) return false;

    if (cJSON_IsArray(root)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, root) {
            const cJSON *romId = cJSON_GetObjectItemCaseSensitive(item, "rom_id");
            if (cJSON_IsNumber(romId)) tally_add(romId->valueint);
        }
    }

    cJSON_Delete(root);
    log_debug("Platform %d: saves recorded for %d ROM(s)", platformId, tallyCount);
    return true;
}

// Count save files sitting next to the ROM. Cheap: a handful of stat() calls,
// no hashing and no network.
static int count_local_saves(const Config *config, const char *platformSlug, const char *fsName) {
    int found = 0;
    for (int slotNum = 0; slotNum <= 3; slotNum++) {
        char slot[8];
        snprintf(slot, sizeof(slot), "%d", slotNum);

        char path[SAVES_MAX_PATH];
        if (!saves_build_path(config, platformSlug, fsName, slot, path, sizeof(path))) break;

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) found++;
    }
    return found;
}

// Case-insensitive substring match, used to guess whether an installed title
// corresponds to a RomM entry. Deliberately only drives a display hint -- a
// wrong guess here shows a misleading icon, which is recoverable, whereas the
// same guess driving a save restore would not be.
static bool name_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle || needle[0] == '\0') return false;

    for (const char *h = haystack; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*b == '\0') return true;
    }
    return false;
}

// Strip the extension and anything in brackets, so "Pokemon Sun (USA).3ds"
// compares against an installed title's "Pokemon Sun".
static void clean_rom_name(const char *fsName, char *out, size_t outLen) {
    size_t j = 0;
    for (size_t i = 0; fsName[i] && j < outLen - 1; i++) {
        char c = fsName[i];
        if (c == '(' || c == '[') break;
        if (c == '.') {
            // Could be an extension or a real dot; assume extension near the end.
            if (!strchr(&fsName[i + 1], '.')) break;
        }
        out[j++] = c;
    }
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '_')) j--;
    out[j] = '\0';
}

static bool looks_installed(const char *fsName) {
    if (titles_count() == 0) return false;

    char cleaned[128];
    clean_rom_name(fsName, cleaned, sizeof(cleaned));
    if (cleaned[0] == '\0') return false;

    for (int i = 0; i < titles_count(); i++) {
        const InstalledTitle *t = titles_get(i);
        if (name_contains(t->name, cleaned) || name_contains(cleaned, t->name)) return true;
    }
    return false;
}

RomStatus romstatus_for(const Config *config, int romId, const char *platformSlug, const char *fsName) {
    RomStatus status = {0};

    status.serverSaves = tally_get(romId);
    status.localSaves = count_local_saves(config, platformSlug, fsName);

    const char *folder = config_get_platform_folder(platformSlug);
    if (folder && folder[0]) {
        char path[SAVES_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s/%s", config->romFolder, folder, fsName);
        struct stat st;
        status.onDevice = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    }

    // Only meaningful for native 3DS titles; other platforms are never
    // "installed" in the AM sense.
    if (!status.onDevice && (strcmp(platformSlug, "3ds") == 0 || strcmp(platformSlug, "new-nintendo-3ds") == 0)) {
        status.installed = looks_installed(fsName);
    }

    return status;
}
