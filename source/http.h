/*
 * HTTP transport - libcurl + mbedTLS
 *
 * Replaces the 3DS's native httpc/sslc services. That stack tops out at
 * TLS 1.1 (libctru exposes exactly three SSL options: default, disable-verify,
 * and force-TLSv1.0) and ships an 11-entry root store with no ISRG/Let's
 * Encrypt anchor, so it cannot complete a handshake against a modern server
 * no matter which root CA is added. mbedTLS 2.28 gives us TLS 1.2 with ECDHE,
 * AES-GCM, SNI and ECDSA, and lets us keep certificate verification ON.
 *
 * It also gives us POST, multipart and Range, none of which httpc offered --
 * the device authorization flow and save sync both need them.
 */

#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Responses larger than this are refused rather than exhausting the heap.
// The 3DS gives homebrew ~64MB on an Old 3DS, less what libctru and the GPU
// already hold.
#define HTTP_MAX_RESPONSE_SIZE (2 * 1024 * 1024)

// curl's compiled-in default on devkitPro's 3ds-curl build. Luma3DS ships this
// bundle (Mozilla-derived, contains ISRG Root X1) as part of Homebrew Menu.
#define HTTP_SYSTEM_CA_BUNDLE "sdmc:/config/ssl/cacert.pem"

typedef struct {
    char *data; // NUL-terminated body; caller frees with http_response_free
    size_t size;
    long statusCode;
} HttpResponse;

// Return false to cancel the transfer in progress.
typedef bool (*HttpProgressCb)(uint64_t transferred, uint64_t total);

// Bring up the socket service and curl. Returns false if either fails, which
// on a 3DS usually means no network. Safe to call once at startup.
bool http_init(void);
void http_exit(void);

// True if a CA bundle was found at startup. When false, HTTPS will fail --
// surface that to the user rather than silently disabling verification.
bool http_has_ca_bundle(void);

// The one origin credentials may be sent to. Requests to anywhere else -- most
// importantly a redirect target -- are issued without the token.
void http_set_trusted_origin(const char *url);

// Sent as `Authorization: Bearer ...`, but only on requests to the trusted
// origin.
void http_set_bearer_token(const char *token);
void http_clear_auth(void);

void http_response_free(HttpResponse *response);

// Each of these returns false on transport failure. An HTTP error status is
// NOT a transport failure -- check response->statusCode.
bool http_get(const char *url, HttpResponse *response);
bool http_post_json(const char *url, const char *jsonBody, HttpResponse *response);

// Streams to destPath, never buffering the body in RAM. Deletes a partial file
// on failure or cancellation so a stale truncated ROM is not left behind.
bool http_download_to_file(const char *url, const char *destPath, HttpProgressCb progressCb);

// Multipart upload of a single file field, for POST /api/saves.
bool http_post_file(const char *url, const char *fieldName, const char *filePath, HttpResponse *response);

#endif // HTTP_H
