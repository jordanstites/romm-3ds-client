/*
 * Platform compatibility - which RomM platforms a 3DS can actually play
 *
 * A RomM library is usually much broader than one console can run. Filtering
 * the platform list keeps the browser to things you can actually put on the SD
 * card and launch.
 *
 * Slugs are RomM's own, taken from the Platform enum in
 * backend/handler/metadata/base_handler.py rather than guessed.
 */

#ifndef COMPAT_H
#define COMPAT_H

#include <stdbool.h>

// True if the platform is playable on a 3DS, either natively or through
// TWiLight Menu++ / an emulator that runs on the console.
bool compat_platform_is_playable(const char *slug);

#endif // COMPAT_H
