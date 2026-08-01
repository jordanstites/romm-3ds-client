/*
 * Auth module - RomM client token storage
 */

#include "auth.h"
#include "cJSON/cJSON.h"
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
