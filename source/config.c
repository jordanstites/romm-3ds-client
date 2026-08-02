/*
 * Config module - Load/save settings to SD card
 */

#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <3ds.h>

// Platform folder mappings cache
#define MAX_MAPPINGS 64
typedef struct {
    char slug[CONFIG_MAX_SLUG_LEN];
    char folder[CONFIG_MAX_SLUG_LEN];
} PlatformMapping;

static PlatformMapping mappings[MAX_MAPPINGS];
static int mappingCount = 0;
static bool mappingsLoaded = false;

void config_init(Config *config) {
    memset(config, 0, sizeof(Config));
    config->serverUrl[0] = '\0';
    snprintf(config->romFolder, CONFIG_MAX_PATH_LEN, "sdmc:/roms");
}

bool config_load(Config *config) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        return false;
    }

    // Reset mappings when loading config
    mappingCount = 0;
    mappingsLoaded = true;

    char line[512];
    bool inMappingsSection = false;

    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';
        newline = strchr(line, '\r');
        if (newline) *newline = '\0';

        // Skip empty lines
        if (line[0] == '\0') continue;

        // Check for section headers
        if (line[0] == '[') {
            inMappingsSection = (strcmp(line, "[platform_mappings]") == 0);
            continue;
        }

        // Parse key=value
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        if (inMappingsSection) {
            // Platform mapping entry
            if (mappingCount < MAX_MAPPINGS) {
                snprintf(mappings[mappingCount].slug, CONFIG_MAX_SLUG_LEN, "%.63s", key);
                snprintf(mappings[mappingCount].folder, CONFIG_MAX_SLUG_LEN, "%.63s", value);
                mappingCount++;
            }
        } else {
            // Main config entries
            if (strcmp(key, "serverUrl") == 0) {
                snprintf(config->serverUrl, CONFIG_MAX_URL_LEN, "%s", value);
            } else if (strcmp(key, "romFolder") == 0) {
                snprintf(config->romFolder, CONFIG_MAX_PATH_LEN, "%s", value);
            } else if (strcmp(key, "showAllPlatforms") == 0) {
                config->showAllPlatforms = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            }
        }
    }

    fclose(f);
    config_normalize_server_url(config);
    return config_is_valid(config);
}

static bool save_config_file(const Config *config) {
    // Ensure directory exists
    mkdir(CONFIG_DIR, 0755);

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        log_error("Failed to open config file for writing: %s", CONFIG_PATH);
        return false;
    }

    // Write main config. Credentials are deliberately absent -- see auth.h.
    fprintf(f, "serverUrl=%s\n", config->serverUrl);
    fprintf(f, "romFolder=%s\n", config->romFolder);
    fprintf(f, "showAllPlatforms=%s\n", config->showAllPlatforms ? "true" : "false");

    // Write platform mappings section
    if (mappingCount > 0) {
        fprintf(f, "\n[platform_mappings]\n");
        for (int i = 0; i < mappingCount; i++) {
            fprintf(f, "%s=%s\n", mappings[i].slug, mappings[i].folder);
        }
    }

    bool ok = !ferror(f);
    if (!ok) {
        log_error("Failed to write config file: %s", CONFIG_PATH);
    }
    fclose(f);
    return ok;
}

bool config_save(const Config *config) {
    return save_config_file(config);
}

// Portion of the URL after the scheme, or the whole string if there is none.
static const char *url_after_scheme(const char *url) {
    const char *sep = strstr(url, "://");
    return sep ? sep + 3 : url;
}

void config_normalize_server_url(Config *config) {
    if (config->serverUrl[0] == '\0') return;

    // Trailing slash first, so it cannot survive the rebuild below.
    size_t len = strlen(config->serverUrl);
    while (len > 0 && config->serverUrl[len - 1] == '/') {
        config->serverUrl[--len] = '\0';
    }
    if (len == 0) return;

    if (strstr(config->serverUrl, "://") != NULL) return;

    // https by default: a server reachable from outside the LAN almost
    // certainly has TLS, and choosing http silently would be the less safe
    // guess of the two.
    char rebuilt[CONFIG_MAX_URL_LEN];
    snprintf(rebuilt, sizeof(rebuilt), "https://%.*s", (int)(CONFIG_MAX_URL_LEN - 9), config->serverUrl);
    snprintf(config->serverUrl, sizeof(config->serverUrl), "%s", rebuilt);
}

void config_set_server_scheme(Config *config, bool https) {
    if (config->serverUrl[0] == '\0') return;

    char rest[CONFIG_MAX_URL_LEN];
    snprintf(rest, sizeof(rest), "%s", url_after_scheme(config->serverUrl));
    snprintf(config->serverUrl, sizeof(config->serverUrl), "%s%.*s", https ? "https://" : "http://",
             (int)(CONFIG_MAX_URL_LEN - 9), rest);
}

bool config_server_uses_https(const Config *config) {
    return strncmp(config->serverUrl, "https://", 8) == 0;
}

bool config_is_valid(const Config *config) {
    // Only server reachability settings are checked here. Whether the user is
    // authenticated is a separate question -- ask auth_has_token().
    return config->serverUrl[0] != '\0' && config->romFolder[0] != '\0';
}

const char *config_get_platform_folder(const char *platformSlug) {
    for (int i = 0; i < mappingCount; i++) {
        if (strcmp(mappings[i].slug, platformSlug) == 0) {
            return mappings[i].folder;
        }
    }
    return NULL;
}

bool config_set_platform_folder(const Config *config, const char *platformSlug, const char *folderName) {
    // Check if mapping already exists
    for (int i = 0; i < mappingCount; i++) {
        if (strcmp(mappings[i].slug, platformSlug) == 0) {
            snprintf(mappings[i].folder, CONFIG_MAX_SLUG_LEN, "%.63s", folderName);
            log_info("Platform '%s' folder set to '%s'", platformSlug, folderName);
            return save_config_file(config);
        }
    }

    // Add new mapping
    if (mappingCount >= MAX_MAPPINGS) return false;

    snprintf(mappings[mappingCount].slug, CONFIG_MAX_SLUG_LEN, "%.63s", platformSlug);
    snprintf(mappings[mappingCount].folder, CONFIG_MAX_SLUG_LEN, "%.63s", folderName);
    mappingCount++;

    log_info("Platform '%s' folder set to '%s'", platformSlug, folderName);
    return save_config_file(config);
}
