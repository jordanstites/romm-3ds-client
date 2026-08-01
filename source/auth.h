/*
 * Auth module - RomM client token storage
 *
 * Tokens live in their own file, not config.ini, so the human-editable config
 * never contains a credential. The token is still plaintext on an SD card --
 * the 3DS has no keystore -- but it is scoped, revocable from RomM's web UI,
 * and never the user's account password.
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>

#define AUTH_MAX_TOKEN_LEN 512
#define AUTH_MAX_DEVICE_ID_LEN 128
#define AUTH_MAX_EXPIRES_LEN 40

#define AUTH_TOKEN_PATH "sdmc:/3ds/romm-3ds-client/token.json"

typedef struct {
    char accessToken[AUTH_MAX_TOKEN_LEN];
    char deviceId[AUTH_MAX_DEVICE_ID_LEN];
    char expiresAt[AUTH_MAX_EXPIRES_LEN]; // ISO 8601; empty means no expiry
} AuthToken;

// Zero the struct.
void auth_init(AuthToken *token);

// Load the token from SD. Returns false if absent or malformed.
bool auth_load(AuthToken *token);

// Persist the token to SD, creating the directory if needed.
bool auth_save(const AuthToken *token);

// Delete the stored token and zero the struct. Call this on a 401 so a revoked
// token sends the user back to pairing instead of into a retry loop.
void auth_clear(AuthToken *token);

// True if a token is present.
bool auth_has_token(const AuthToken *token);

#endif // AUTH_H
