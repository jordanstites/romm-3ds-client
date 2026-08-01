/*
 * Pairing screen - RomM device authorization
 */

#ifndef PAIRING_H
#define PAIRING_H

#include "../auth.h"
#include "../config.h"
#include <3ds.h>

typedef enum {
    PAIRING_NONE,
    PAIRING_CANCELLED, // user backed out
    PAIRING_SUCCESS    // token obtained and saved
} PairingResult;

// Start a pairing attempt. Safe to call again to restart after failure.
void pairing_init(const Config *config, AuthToken *token);

// Drives polling as well as input, so it must be called every frame.
PairingResult pairing_update(u32 kDown);

void pairing_draw(void);

#endif // PAIRING_H
