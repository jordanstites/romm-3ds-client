/*
 * API module - RomM API wrapper with HTTP and JSON parsing
 */

#ifndef API_H
#define API_H

#include <stdbool.h>
#include <stdint.h>

// Platform data from /api/platforms
typedef struct {
    int id;
    char slug[64];
    char name[128];
    char displayName[128];
    int romCount;
} Platform;

// ROM data from /api/roms
typedef struct {
    int id;
    int platformId;
    char name[256];
    char fsName[256];
} Rom;

// Detailed ROM data from /api/roms/{id}
typedef struct {
    int id;
    int platformId;
    char name[256];
    char fsName[256];
    char summary[1024];
    char platformName[128];
    char firstReleaseDate[32];
    char md5Hash[64];
} RomDetail;

// Bring up the HTTP transport. Returns false if the network stack could not
// start, which on a 3DS usually means the console is offline.
bool api_init(void);

// Cleanup API module
void api_exit(void);

// Set base URL for API requests
void api_set_base_url(const char *url);

// Set the RomM client token sent as `Authorization: Bearer ...`.
// Passing NULL or "" sends requests unauthenticated.
void api_set_bearer_token(const char *token);

// Drop the in-memory auth header. Use on 401 alongside auth_clear().
void api_clear_auth(void);

// Fetch platforms from server
// Returns array of platforms, sets count. Caller must free with api_free_platforms
Platform *api_get_platforms(int *count);

// Free platforms array
void api_free_platforms(Platform *platforms, int count);

// Fetch ROMs for a platform
// Returns array of ROMs, sets count and total. Caller must free with api_free_roms
Rom *api_get_roms(int platformId, int offset, int limit, int *count, int *total);

// Search ROMs across platforms
// platformIds is an array of platform IDs to search (NULL or count 0 = all platforms)
// Returns array of ROMs, sets count and total. Caller must free with api_free_roms
Rom *api_search_roms(const char *searchTerm, const int *platformIds, int platformIdCount, int offset, int limit,
                     int *count, int *total);

// Free ROMs array
void api_free_roms(Rom *roms, int count);

// Fetch single ROM details
// Returns ROM detail, caller must free with api_free_rom_detail
RomDetail *api_get_rom_detail(int romId);

// Free ROM detail
void api_free_rom_detail(RomDetail *detail);

// Progress callback for downloads (bytesDownloaded, totalBytes; total may be 0 if unknown)
// Return true to continue, false to cancel
typedef bool (*DownloadProgressCb)(uint64_t bytesDownloaded, uint64_t totalBytes);

// Download a ROM file to the specified path
// fileName is the fs_name from the ROM detail (used in URL path)
// progressCb is called periodically with download progress (may be NULL)
// Returns true on success, false on failure
bool api_download_rom(int romId, const char *fileName, const char *destPath, DownloadProgressCb progressCb);

#endif // API_H
