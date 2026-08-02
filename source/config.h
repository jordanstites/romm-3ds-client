/*
 * Config module - Load/save settings to SD card
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define CONFIG_MAX_URL_LEN 256
#define CONFIG_MAX_PATH_LEN 256
#define CONFIG_MAX_SLUG_LEN 64
#define CONFIG_PATH "sdmc:/3ds/romm-3ds-client/config.ini"
#define CONFIG_DIR "sdmc:/3ds/romm-3ds-client"

// No credentials live here. Authentication uses a scoped client token stored
// separately by the auth module -- see auth.h.
typedef struct {
    char serverUrl[CONFIG_MAX_URL_LEN];
    char romFolder[CONFIG_MAX_PATH_LEN];
    // When false (the default) the platform list is filtered to systems the
    // 3DS can actually run. A RomM library often spans consoles this hardware
    // has no way to play.
    bool showAllPlatforms;
} Config;

// Initialize config with defaults
void config_init(Config *config);

// Load config from SD card, returns true if successful
bool config_load(Config *config);

// Save config to SD card, returns true if successful
bool config_save(const Config *config);

// Check if config has required fields
bool config_is_valid(const Config *config);

// Ensure the server URL carries a scheme and no trailing slash.
//
// A URL without one breaks more than it looks like it should: curl guesses
// http, but the origin check that decides whether to send credentials cannot
// parse it, so every request goes out unauthenticated and the server answers
// 403. Defaults to https when the scheme is missing.
void config_normalize_server_url(Config *config);

// Swap the scheme, keeping the rest of the URL.
void config_set_server_scheme(Config *config, bool https);

// True when the server URL uses https.
bool config_server_uses_https(const Config *config);

// Platform folder mappings (slug -> subfolder name)
// Get subfolder for a platform slug, returns NULL if not mapped
const char *config_get_platform_folder(const char *platformSlug);

// Set subfolder for a platform slug and persist to config file
bool config_set_platform_folder(const Config *config, const char *platformSlug, const char *folderName);

#endif // CONFIG_H
