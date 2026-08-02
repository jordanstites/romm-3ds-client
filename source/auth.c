/*
 * Auth module - RomM client token storage
 */

#include "auth.h"
#include "cJSON/cJSON.h"
#include "config.h"
#include "http.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <3ds.h>

// Only what the client actually needs. Deliberately excludes users.*, tasks.run
// and roms.write -- a token on an SD card should not be able to administer the
// server or modify the library.
#define REQUESTED_SCOPES                                                                                               \
    "\"roms.read\",\"platforms.read\",\"assets.read\",\"assets.write\","                                               \
    "\"devices.read\",\"devices.write\",\"me.read\""

static void copy_string_field(const cJSON *root, const char *key, char *dst, size_t dstLen) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(dst, dstLen, "%s", item->valuestring);
    } else {
        dst[0] = '\0';
    }
}

void auth_init(AuthToken *token) {
    memset(token, 0, sizeof(AuthToken));
}

bool auth_load(AuthToken *token) {
    auth_init(token);

    FILE *f = fopen(AUTH_TOKEN_PATH, "r");
    if (!f) {
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // A token file this large is not something we wrote.
    if (size <= 0 || size > 8192) {
        fclose(f);
        log_error("Token file has implausible size (%ld bytes)", size);
        return false;
    }

    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(f);
        return false;
    }

    size_t read = fread(buffer, 1, (size_t)size, f);
    buffer[read] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);

    if (!root) {
        log_error("Token file is not valid JSON");
        return false;
    }

    copy_string_field(root, "access_token", token->accessToken, AUTH_MAX_TOKEN_LEN);
    copy_string_field(root, "device_id", token->deviceId, AUTH_MAX_DEVICE_ID_LEN);
    copy_string_field(root, "expires_at", token->expiresAt, AUTH_MAX_EXPIRES_LEN);

    cJSON_Delete(root);

    if (!auth_has_token(token)) {
        log_error("Token file has no access_token");
        auth_init(token);
        return false;
    }

    return true;
}

bool auth_save(const AuthToken *token) {
    mkdir(CONFIG_DIR, 0755);

    FILE *f = fopen(AUTH_TOKEN_PATH, "w");
    if (!f) {
        log_error("Failed to open token file for writing: %s", AUTH_TOKEN_PATH);
        return false;
    }

    // Written by hand rather than via cJSON_Print to avoid a heap allocation
    // whose lifetime would straddle the file handle.
    fprintf(f, "{\n");
    fprintf(f, "  \"access_token\": \"%s\",\n", token->accessToken);
    fprintf(f, "  \"device_id\": \"%s\",\n", token->deviceId);
    fprintf(f, "  \"expires_at\": \"%s\"\n", token->expiresAt);
    fprintf(f, "}\n");

    bool ok = !ferror(f);
    if (!ok) {
        log_error("Failed to write token file: %s", AUTH_TOKEN_PATH);
    }
    fclose(f);
    return ok;
}

void auth_clear(AuthToken *token) {
    if (remove(AUTH_TOKEN_PATH) != 0) {
        // Absent is the desired end state, so this is not worth an error.
        log_info("No token file to remove");
    }
    auth_init(token);
}

bool auth_has_token(const AuthToken *token) {
    return token->accessToken[0] != '\0';
}

bool auth_status_is_unauthenticated(int statusCode) {
    // RomM answers 403 for a missing or invalid bearer token; 401 is included
    // for servers and proxies that use it.
    return statusCode == 401 || statusCode == 403;
}

// ---------------------------------------------------------------------------
// Device authorization flow
// ---------------------------------------------------------------------------

// A stable per-console identifier so re-pairing updates the same device record
// on the server instead of accumulating duplicates.
//
// Generated once and persisted rather than derived from the console's real
// device ID: PS_GetDeviceId needs the ps:ps service, which Luma's 3dsx service
// allowlist does not grant, so it would fail on every launch and leave every
// console reporting the same string. This also avoids sending a hardware
// identifier to the server, which it has no use for.
static void build_device_identifier(char *out, size_t outLen) {
    FILE *f = fopen(DEVICE_ID_PATH, "r");
    if (f) {
        char stored[64] = "";
        if (fgets(stored, sizeof(stored), f)) {
            stored[strcspn(stored, "\r\n")] = '\0';
        }
        fclose(f);
        if (stored[0] != '\0') {
            snprintf(out, outLen, "%s", stored);
            return;
        }
    }

    // Not a secret, just needs to be unlikely to collide.
    snprintf(out, outLen, "3ds-%08lx%08lx", (unsigned long)osGetTime(), (unsigned long)svcGetSystemTick());

    mkdir(CONFIG_DIR, 0755);
    f = fopen(DEVICE_ID_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", out);
        fclose(f);
    } else {
        // Pairing still works; the server just sees a new device next time.
        log_error("Could not persist the device identifier to %s", DEVICE_ID_PATH);
    }
}

bool auth_begin_pairing(const char *serverUrl, AuthPairing *pairing) {
    memset(pairing, 0, sizeof(AuthPairing));

    char deviceIdent[64];
    build_device_identifier(deviceIdent, sizeof(deviceIdent));

    char body[512];
    snprintf(body, sizeof(body),
             "{\"client_device_identifier\":\"%s\",\"name\":\"Nintendo 3DS\","
             "\"client\":\"romm-3ds-client\",\"platform\":\"3ds\","
             "\"client_version\":\"" APP_VERSION "\",\"requested_scopes\":[" REQUESTED_SCOPES "]}",
             deviceIdent);

    char url[512];
    snprintf(url, sizeof(url), "%s/api/auth/device/init", serverUrl);

    HttpResponse response;
    if (!http_post_json(url, body, &response)) {
        log_error("Could not reach %s to start pairing", url);
        return false;
    }

    if (response.statusCode != 200 && response.statusCode != 201) {
        log_error("Pairing request rejected: HTTP %ld", response.statusCode);
        if (response.statusCode == 404) {
            log_error("This server may predate RomM 4.9.0, which added device auth.");
        }
        http_response_free(&response);
        return false;
    }

    cJSON *root = cJSON_Parse(response.data);
    http_response_free(&response);
    if (!root) {
        log_error("Pairing response was not valid JSON");
        return false;
    }

    copy_string_field(root, "user_code", pairing->userCode, AUTH_MAX_USER_CODE_LEN);
    copy_string_field(root, "device_code", pairing->deviceCode, AUTH_MAX_DEVICE_CODE_LEN);

    // Must be the _complete form. RomM's /pair/device page requires the code in
    // the query string -- without it the page renders "No pairing code
    // provided." (The bare path returns HTTP 200, but that is only the SPA
    // shell, so status alone does not tell you the page works.)
    //
    // The resulting URL is long, which is what the QR code on the bottom screen
    // is for. verifyUrl keeps the scheme so it stays scannable.
    char relative[AUTH_MAX_VERIFY_URL_LEN];
    copy_string_field(root, "verification_path_complete", relative, sizeof(relative));
    if (relative[0] == '\0') {
        copy_string_field(root, "verification_path", relative, sizeof(relative));
    }
    if (relative[0] != '\0') {
        // A QR without a scheme is not a link -- phone cameras will not offer
        // to open it, and the code has to be typed by hand instead. The
        // configured URL is normalised to carry one, but default here too
        // rather than silently emitting something unscannable.
        if (strstr(serverUrl, "://") != NULL) {
            snprintf(pairing->verifyUrl, AUTH_MAX_VERIFY_URL_LEN, "%s%s", serverUrl, relative);
        } else {
            log_error("Server URL '%s' has no scheme; assuming https for the QR code", serverUrl);
            snprintf(pairing->verifyUrl, AUTH_MAX_VERIFY_URL_LEN, "https://%s%s", serverUrl, relative);
        }

        // Scheme-stripped copy for the human-readable line: browsers infer it,
        // and it buys ~8 characters of screen width.
        const char *shortForm = pairing->verifyUrl;
        if (strncmp(shortForm, "https://", 8) == 0) {
            shortForm += 8;
        } else if (strncmp(shortForm, "http://", 7) == 0) {
            shortForm += 7;
        }
        snprintf(pairing->verifyUrlDisplay, AUTH_MAX_VERIFY_URL_LEN, "%s", shortForm);
    }

    const cJSON *expiresIn = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    pairing->expiresInSeconds = cJSON_IsNumber(expiresIn) ? expiresIn->valueint : 600;

    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "interval");
    pairing->pollIntervalSeconds = cJSON_IsNumber(interval) ? interval->valueint : 5;

    cJSON_Delete(root);

    if (pairing->userCode[0] == '\0' || pairing->deviceCode[0] == '\0') {
        log_error("Pairing response was missing a code");
        return false;
    }

    log_info("Pairing code %s, expires in %ds", pairing->userCode, pairing->expiresInSeconds);
    return true;
}

AuthPollResult auth_poll_pairing(const char *serverUrl, const AuthPairing *pairing, AuthToken *token) {
    char body[256];
    snprintf(body, sizeof(body), "{\"device_code\":\"%s\"}", pairing->deviceCode);

    char url[512];
    snprintf(url, sizeof(url), "%s/api/auth/device/token", serverUrl);

    HttpResponse response;
    if (!http_post_json(url, body, &response)) {
        return AUTH_POLL_ERROR;
    }

    long status = response.statusCode;

    if (status == 200 || status == 201) {
        cJSON *root = cJSON_Parse(response.data);
        http_response_free(&response);
        if (!root) return AUTH_POLL_ERROR;

        auth_init(token);
        copy_string_field(root, "access_token", token->accessToken, AUTH_MAX_TOKEN_LEN);
        copy_string_field(root, "device_id", token->deviceId, AUTH_MAX_DEVICE_ID_LEN);
        copy_string_field(root, "expires_at", token->expiresAt, AUTH_MAX_EXPIRES_LEN);
        cJSON_Delete(root);

        if (!auth_has_token(token)) {
            log_error("Approval succeeded but no token was returned");
            return AUTH_POLL_ERROR;
        }
        return AUTH_POLL_APPROVED;
    }

    // Still waiting. RFC 8628 uses 400 + an "error" discriminator, so read it
    // rather than assuming any 4xx means denial.
    AuthPollResult result = AUTH_POLL_PENDING;
    if (status == 400 || status == 401 || status == 403 || status == 404 || status == 428) {
        cJSON *root = cJSON_Parse(response.data);
        if (root) {
            char detail[128] = "";
            copy_string_field(root, "error", detail, sizeof(detail));
            if (detail[0] == '\0') {
                copy_string_field(root, "detail", detail, sizeof(detail));
            }
            if (strstr(detail, "expired")) {
                result = AUTH_POLL_EXPIRED;
            } else if (strstr(detail, "denied") || strstr(detail, "declined")) {
                result = AUTH_POLL_DENIED;
            }
            cJSON_Delete(root);
        }
        // A 404 with no useful body means the code is gone, i.e. expired.
        if (result == AUTH_POLL_PENDING && status == 404) {
            result = AUTH_POLL_EXPIRED;
        }
    } else if (status >= 500) {
        log_error("Server error while polling: HTTP %ld", status);
        result = AUTH_POLL_ERROR;
    }

    http_response_free(&response);
    return result;
}
