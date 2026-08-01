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

// Draws the QR code on the bottom screen. Returns false when there is nothing
// to show (no active pairing attempt), so the caller can fall back.
bool pairing_draw_qr(void);

#endif // PAIRING_H
