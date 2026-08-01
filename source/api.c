/*
 * API module - RomM API wrapper with HTTP and JSON parsing
 */

#include "api.h"
#include "http.h"
#include "log.h"
#include "cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <3ds.h>

#define MAX_URL_LEN 1024
#define TRACE_BODY_PREVIEW_LEN 500 // Max chars to show for response body

static char baseUrl[256] = "";

// Status of the most recent request, so callers can tell "unauthorized" from
// "unreachable" without every function growing an out-param.
static int lastStatus = 0;

static void url_encode(const char *src, char *dst, size_t dstLen) {
    static const char *unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstLen - 1; i++) {
        if (strchr(unreserved, src[i])) {
            dst[j++] = src[i];
        } else if (j + 3 < dstLen) {
            snprintf(&dst[j], 4, "%%%02X", (unsigned char)src[i]);
            j += 3;
        } else {
            break;
        }
    }
    dst[j] = '\0';
}

bool api_init(void) {
    return http_init();
}

void api_exit(void) {
    http_exit();
}

void api_set_base_url(const char *url) {
    snprintf(baseUrl, sizeof(baseUrl), "%s", url);
    // Remove trailing slash if present
    size_t len = strlen(baseUrl);
    if (len > 0 && baseUrl[len - 1] == '/') {
        baseUrl[len - 1] = '\0';
    }
}

const char *api_get_base_url(void) {
    return baseUrl;
}

void api_set_bearer_token(const char *token) {
    http_set_bearer_token(token);
}

void api_clear_auth(void) {
    http_clear_auth();
}

int api_last_status(void) {
    return lastStatus;
}

// Fetch a JSON body. Returns a heap string the caller frees, or NULL on any
// failure. statusCode is set whenever the server answered at all, so callers
// can distinguish "unauthorized" from "unreachable".
static char *fetch_json(const char *url, int *statusCode) {
    *statusCode = 0;
    lastStatus = 0;

    log_debug("GET %s", url);

    HttpResponse response;
    if (!http_get(url, &response)) {
        return NULL;
    }

    *statusCode = (int)response.statusCode;
    lastStatus = (int)response.statusCode;
    log_debug("Status: %ld", response.statusCode);

    if (response.statusCode != 200) {
        log_error("HTTP error: %ld", response.statusCode);
        http_response_free(&response);
        return NULL;
    }

    log_debug("Size: %u bytes", (unsigned)response.size);
    if (response.size <= TRACE_BODY_PREVIEW_LEN) {
        log_trace("Body:\n%s", response.data);
    } else {
        log_trace("Body (truncated):\n%.*s...\n[%u more bytes]", TRACE_BODY_PREVIEW_LEN, response.data,
                  (unsigned)(response.size - TRACE_BODY_PREVIEW_LEN));
    }

    // Ownership transfers to the caller; do not free via http_response_free.
    return response.data;
}

Platform *api_get_platforms(int *count) {
    *count = 0;

    char url[MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s/api/platforms", baseUrl);

    int statusCode;
    char *response = fetch_json(url, &statusCode);
    if (!response) {
        return NULL;
    }

    // Parse JSON
    cJSON *json = cJSON_Parse(response);
    free(response);

    if (!json) {
        log_error("JSON parse error");
        return NULL;
    }

    if (!cJSON_IsArray(json)) {
        log_error("Expected array response");
        cJSON_Delete(json);
        return NULL;
    }

    int arraySize = cJSON_GetArraySize(json);
    if (arraySize == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    Platform *platforms = calloc(arraySize, sizeof(Platform));
    if (!platforms) {
        cJSON_Delete(json);
        return NULL;
    }

    int i = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, json) {
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *slug = cJSON_GetObjectItem(item, "slug");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *displayName = cJSON_GetObjectItem(item, "display_name");
        cJSON *romCount = cJSON_GetObjectItem(item, "rom_count");

        if (cJSON_IsNumber(id)) platforms[i].id = id->valueint;
        if (cJSON_IsString(slug)) snprintf(platforms[i].slug, sizeof(platforms[i].slug), "%s", slug->valuestring);
        if (cJSON_IsString(name)) snprintf(platforms[i].name, sizeof(platforms[i].name), "%s", name->valuestring);
        if (cJSON_IsString(displayName))
            snprintf(platforms[i].displayName, sizeof(platforms[i].displayName), "%s", displayName->valuestring);
        if (cJSON_IsNumber(romCount)) platforms[i].romCount = romCount->valueint;

        // Fallback to name if displayName is empty
        if (platforms[i].displayName[0] == '\0' && platforms[i].name[0] != '\0') {
            snprintf(platforms[i].displayName, sizeof(platforms[i].displayName), "%s", platforms[i].name);
        }

        i++;
    }

    *count = i;
    cJSON_Delete(json);
    return platforms;
}

void api_free_platforms(Platform *platforms, int count) {
    (void)count;
    if (platforms) free(platforms);
}

static Rom *parse_paginated_roms(const char *response, int *count, int *total) {
    *count = 0;
    *total = 0;

    cJSON *json = cJSON_Parse(response);
    if (!json) {
        log_error("JSON parse error");
        return NULL;
    }

    cJSON *totalJson = cJSON_GetObjectItem(json, "total");
    if (cJSON_IsNumber(totalJson)) {
        *total = totalJson->valueint;
    }

    cJSON *items = cJSON_GetObjectItem(json, "items");
    if (!items || !cJSON_IsArray(items)) {
        log_error("Expected items array");
        cJSON_Delete(json);
        return NULL;
    }

    int arraySize = cJSON_GetArraySize(items);
    if (arraySize == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    Rom *roms = calloc(arraySize, sizeof(Rom));
    if (!roms) {
        cJSON_Delete(json);
        return NULL;
    }

    int i = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *platformIdJson = cJSON_GetObjectItem(item, "platform_id");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *fsName = cJSON_GetObjectItem(item, "fs_name");

        if (cJSON_IsNumber(id)) roms[i].id = id->valueint;
        if (cJSON_IsNumber(platformIdJson)) roms[i].platformId = platformIdJson->valueint;
        if (cJSON_IsString(name)) snprintf(roms[i].name, sizeof(roms[i].name), "%s", name->valuestring);
        if (cJSON_IsString(fsName)) snprintf(roms[i].fsName, sizeof(roms[i].fsName), "%s", fsName->valuestring);

        i++;
    }

    *count = i;
    cJSON_Delete(json);
    return roms;
}

Rom *api_get_roms(int platformId, int offset, int limit, int *count, int *total) {
    char url[MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s/api/roms?platform_ids=%d&offset=%d&limit=%d&order_by=name", baseUrl, platformId,
             offset, limit);

    int statusCode;
    char *response = fetch_json(url, &statusCode);
    if (!response) {
        *count = 0;
        *total = 0;
        return NULL;
    }

    Rom *roms = parse_paginated_roms(response, count, total);
    free(response);
    return roms;
}

Rom *api_search_roms(const char *searchTerm, const int *platformIds, int platformIdCount, int offset, int limit,
                     int *count, int *total) {
    char encodedTerm[512];
    url_encode(searchTerm, encodedTerm, sizeof(encodedTerm));

    char url[MAX_URL_LEN];
    int pos = snprintf(url, sizeof(url), "%s/api/roms?search_term=%s&offset=%d&limit=%d&order_by=name", baseUrl,
                       encodedTerm, offset, limit);

    for (int i = 0; i < platformIdCount && pos < (int)sizeof(url) - 32; i++) {
        pos += snprintf(url + pos, sizeof(url) - pos, "&platform_ids=%d", platformIds[i]);
    }

    int statusCode;
    char *response = fetch_json(url, &statusCode);
    if (!response) {
        *count = 0;
        *total = 0;
        return NULL;
    }

    Rom *roms = parse_paginated_roms(response, count, total);
    free(response);
    return roms;
}

void api_free_roms(Rom *roms, int count) {
    (void)count;
    if (roms) free(roms);
}

RomDetail *api_get_rom_detail(int romId) {
    char url[MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s/api/roms/%d", baseUrl, romId);

    int statusCode;
    char *response = fetch_json(url, &statusCode);
    if (!response) {
        return NULL;
    }

    cJSON *json = cJSON_Parse(response);
    free(response);

    if (!json) {
        log_error("JSON parse error");
        return NULL;
    }

    RomDetail *detail = calloc(1, sizeof(RomDetail));
    if (!detail) {
        cJSON_Delete(json);
        return NULL;
    }

    // Basic fields
    cJSON *id = cJSON_GetObjectItem(json, "id");
    cJSON *platformId = cJSON_GetObjectItem(json, "platform_id");
    cJSON *name = cJSON_GetObjectItem(json, "name");
    cJSON *fsName = cJSON_GetObjectItem(json, "fs_name");
    cJSON *summary = cJSON_GetObjectItem(json, "summary");
    cJSON *md5Hash = cJSON_GetObjectItem(json, "md5_hash");

    // Platform name is a flat field, not nested
    cJSON *platformName = cJSON_GetObjectItem(json, "platform_display_name");
    if (!platformName || !cJSON_IsString(platformName) || !platformName->valuestring[0]) {
        platformName = cJSON_GetObjectItem(json, "platform_slug");
    }

    // Release date is inside the metadatum object (epoch seconds)
    cJSON *metadatum = cJSON_GetObjectItem(json, "metadatum");
    cJSON *firstReleaseDate = metadatum ? cJSON_GetObjectItem(metadatum, "first_release_date") : NULL;

    if (cJSON_IsNumber(id)) detail->id = id->valueint;
    if (cJSON_IsNumber(platformId)) detail->platformId = platformId->valueint;
    if (cJSON_IsString(name)) snprintf(detail->name, sizeof(detail->name), "%s", name->valuestring);
    if (cJSON_IsString(fsName)) snprintf(detail->fsName, sizeof(detail->fsName), "%s", fsName->valuestring);
    if (cJSON_IsString(summary)) snprintf(detail->summary, sizeof(detail->summary), "%s", summary->valuestring);
    if (cJSON_IsString(md5Hash)) snprintf(detail->md5Hash, sizeof(detail->md5Hash), "%s", md5Hash->valuestring);
    if (cJSON_IsString(platformName))
        snprintf(detail->platformName, sizeof(detail->platformName), "%s", platformName->valuestring);
    if (cJSON_IsNumber(firstReleaseDate)) {
        time_t epoch = (time_t)(firstReleaseDate->valuedouble / 1000.0);
        struct tm *tm = gmtime(&epoch);
        if (tm) strftime(detail->firstReleaseDate, sizeof(detail->firstReleaseDate), "%B %d, %Y", tm);
    }

    cJSON_Delete(json);
    return detail;
}

void api_free_rom_detail(RomDetail *detail) {
    if (detail) free(detail);
}

bool api_download_rom(int romId, const char *fileName, const char *destPath, DownloadProgressCb progressCb) {
    char encodedName[256];
    url_encode(fileName, encodedName, sizeof(encodedName));
    char url[MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s/api/roms/%d/content/%s", baseUrl, romId, encodedName);

    log_debug("GET %s", url);
    log_debug("Saving to: %s", destPath);

    // Redirects, chunked streaming to disk, partial-file cleanup and the
    // cancel path all live in the transport now. RomM behind a reverse proxy
    // or S3-backed storage does redirect, so following them still matters.
    return http_download_to_file(url, destPath, progressCb);
}
