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
#define DEVICE_ID_PATH "sdmc:/3ds/romm-3ds-client/device.id"

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

// ---------------------------------------------------------------------------
// Device authorization flow (RFC 8628 style, RomM 4.9.0+)
//
// The console displays a short code and polls; the user approves in RomM's web
// UI. This is deliberately not the /api/client-tokens pair-code flow, whose
// codes expire in 60 seconds -- far too tight for a soft keyboard. Device auth
// codes last 10 minutes and require no typing on the console.
// ---------------------------------------------------------------------------

#define AUTH_MAX_USER_CODE_LEN 32
#define AUTH_MAX_DEVICE_CODE_LEN 128
#define AUTH_MAX_VERIFY_URL_LEN 320

typedef struct {
    char userCode[AUTH_MAX_USER_CODE_LEN];      // shown to the user
    char deviceCode[AUTH_MAX_DEVICE_CODE_LEN];  // secret, sent when polling
    char verifyUrl[AUTH_MAX_VERIFY_URL_LEN];    // absolute URL for the user
    int expiresInSeconds;
    int pollIntervalSeconds;
} AuthPairing;

typedef enum {
    AUTH_POLL_PENDING,  // not approved yet, keep polling
    AUTH_POLL_APPROVED, // token written to the AuthToken out-param
    AUTH_POLL_DENIED,   // user rejected it
    AUTH_POLL_EXPIRED,  // code timed out, start over
    AUTH_POLL_ERROR     // transport or server failure
} AuthPollResult;

// Ask the server for a pairing code. serverUrl is the configured base URL,
// needed because RomM returns a *relative* verification path.
bool auth_begin_pairing(const char *serverUrl, AuthPairing *pairing);

// Poll once. Call no more often than pairing->pollIntervalSeconds.
AuthPollResult auth_poll_pairing(const char *serverUrl, const AuthPairing *pairing, AuthToken *token);

// True when a response status means "not authenticated". RomM answers 403 for
// a missing or invalid token, not 401, so checking only 401 would miss the
// revoked-token case entirely.
bool auth_status_is_unauthenticated(int statusCode);

#endif // AUTH_H
