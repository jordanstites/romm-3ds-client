/*
 * Platform compatibility - which RomM platforms a 3DS can actually play
 */

#include "compat.h"
#include <string.h>

// Grouped by how they run on the console. Slugs come from RomM's Platform enum
// (backend/handler/metadata/base_handler.py), not from guesswork -- an
// unrecognised slug is silently hidden, so a wrong entry here is invisible.
static const char *const PLAYABLE_SLUGS[] = {
    // Native, installed as CIA
    "3ds",
    "new-nintendo-3ds",

    // DS family, via TWiLight Menu++ / nds-bootstrap
    "nds",
    "nintendo-dsi",

    // Game Boy family. GBA via GBARunner2, GB/GBC via GameYob.
    "gb",
    "gbc",
    "gba",
    "virtualboy",

    // 8/16-bit consoles, via TWiLight Menu++ emulators
    "nes",
    "famicom",
    "snes",
    "sfam",
    "genesis",
    "sms",
    "gamegear",
    "sg1000",
    "turbografx16--1",
    "supergrafx",

    // Other handhelds
    "neo-geo-pocket",
    "neo-geo-pocket-color",
    "wonderswan",
    "wonderswan-color",
    "lynx",
    "supervision",
    "mega-duck-slash-cougar-boy",

    // Early cartridge consoles and home computers
    "atari2600",
    "atari5200",
    "atari7800",
    "colecovision",
    "intellivision",
    "vectrex",
    "msx",
    "msx2",
    "acpc",
};

static const int PLAYABLE_COUNT = (int)(sizeof(PLAYABLE_SLUGS) / sizeof(PLAYABLE_SLUGS[0]));

bool compat_platform_is_playable(const char *slug) {
    if (!slug || slug[0] == '\0') return false;

    for (int i = 0; i < PLAYABLE_COUNT; i++) {
        if (strcmp(slug, PLAYABLE_SLUGS[i]) == 0) {
            return true;
        }
    }
    return false;
}
