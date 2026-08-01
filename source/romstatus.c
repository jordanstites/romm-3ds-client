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
        if (saves_find_existing(config, platformSlug, fsName, slot, path, sizeof(path))) found++;
    }
    return found;
}

// Reduce a title to comparable form: lowercase, letters and digits only.
// Punctuation, spacing, region tags and edition suffixes vary constantly
// between a RomM filename and a title's own SMDH name, and none of it carries
// meaning for identification.
static void normalize_title(const char *src, char *out, size_t outLen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < outLen - 1; i++) {
        unsigned char ch = (unsigned char)src[i];
        // Region and language tags are noise: "Pokemon Sun (USA) (En,Ja,Fr)".
        if (ch == '(' || ch == '[') break;
        if (isalnum(ch)) out[j++] = (char)tolower(ch);
    }
    out[j] = '\0';
}

// Whether two normalised titles denote the same game.
//
// A prefix test alone is not enough: a title's own SMDH name frequently drops
// the series prefix the library keeps, as in "Happy Home Designer" against
// "Animal Crossing: Happy Home Designer". So the shorter name matching
// anywhere inside the longer one counts, guarded by a length floor so short
// generic words cannot pull in unrelated entries.
#define MATCH_MIN_PREFIX 6
#define MATCH_MIN_CONTAINED 10

static bool titles_look_equivalent(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la == 0 || lb == 0) return false;

    const char *shortStr = la <= lb ? a : b;
    const char *longStr = la <= lb ? b : a;
    size_t shortLen = la <= lb ? la : lb;

    if (shortLen < MATCH_MIN_PREFIX) return la == lb && strcmp(a, b) == 0;

    if (strncmp(a, b, shortLen) == 0) return true;

    if (shortLen >= MATCH_MIN_CONTAINED && strstr(longStr, shortStr) != NULL) return true;

    return false;
}

static bool looks_installed(const char *romName, const char *fsName) {
    if (titles_count() == 0) return false;

    char candidates[2][160];
    normalize_title(romName ? romName : "", candidates[0], sizeof(candidates[0]));
    normalize_title(fsName ? fsName : "", candidates[1], sizeof(candidates[1]));

    char installed[160];
    for (int i = 0; i < titles_count(); i++) {
        normalize_title(titles_get(i)->name, installed, sizeof(installed));
        for (int c = 0; c < 2; c++) {
            if (titles_look_equivalent(installed, candidates[c])) return true;
        }
    }
    return false;
}

RomStatus romstatus_for(const Config *config, int romId, const char *platformSlug, const char *romName,
                        const char *fsName) {
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
        status.installed = looks_installed(romName, fsName);
    }

    return status;
}
