/*
 * HTTP transport - libcurl + mbedTLS
 */

#include "http.h"
#include "log.h"
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

#define USER_AGENT "romm-3ds-client/0.1"
#define CONNECT_TIMEOUT_SECONDS 15L

// socInit needs a 0x1000-aligned block. 1MB matches what FBI uses and is
// comfortable for the concurrent transfers we do (one).
#define SOC_BUFFER_SIZE (1024 * 1024)
#define SOC_ALIGNMENT 0x1000

// curl defaults to 16KB reads. The 3DS pays a real cost per syscall and per SD
// write, so larger chunks measurably raise throughput.
#define DOWNLOAD_READ_BUFFER (128 * 1024)
#define DOWNLOAD_WRITE_BUFFER (256 * 1024)

static u32 *socBuffer = NULL;
static bool socReady = false;
static bool curlReady = false;
static bool caBundleFound = false;
static char authHeader[576] = "";

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool http_init(void) {
    socBuffer = (u32 *)memalign(SOC_ALIGNMENT, SOC_BUFFER_SIZE);
    if (!socBuffer) {
        log_error("Could not allocate %d bytes for the socket service", SOC_BUFFER_SIZE);
        return false;
    }

    Result res = socInit(socBuffer, SOC_BUFFER_SIZE);
    if (R_FAILED(res)) {
        log_error("socInit failed (0x%08lX) -- is the console online?", res);
        free(socBuffer);
        socBuffer = NULL;
        return false;
    }
    socReady = true;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_error("curl_global_init failed");
        http_exit();
        return false;
    }
    curlReady = true;

    FILE *ca = fopen(HTTP_SYSTEM_CA_BUNDLE, "r");
    if (ca) {
        caBundleFound = true;
        fclose(ca);
    } else {
        // Not fatal: plain HTTP still works, which is how we develop on the LAN.
        log_error("No CA bundle at %s -- HTTPS will fail until one exists", HTTP_SYSTEM_CA_BUNDLE);
    }

    log_info("HTTP transport ready (%s)", curl_version());
    return true;
}

void http_exit(void) {
    if (curlReady) {
        curl_global_cleanup();
        curlReady = false;
    }
    if (socReady) {
        socExit();
        socReady = false;
    }
    if (socBuffer) {
        free(socBuffer);
        socBuffer = NULL;
    }
}

bool http_has_ca_bundle(void) {
    return caBundleFound;
}

void http_set_bearer_token(const char *token) {
    if (!token || token[0] == '\0') {
        http_clear_auth();
        return;
    }
    snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s", token);
}

void http_clear_auth(void) {
    memset(authHeader, 0, sizeof(authHeader));
}

void http_response_free(HttpResponse *response) {
    if (!response) return;
    free(response->data);
    response->data = NULL;
    response->size = 0;
    response->statusCode = 0;
}

// ---------------------------------------------------------------------------
// Shared request plumbing
// ---------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t size;
    bool overflowed;
} MemoryBuffer;

static size_t write_to_memory(void *contents, size_t size, size_t nmemb, void *userdata) {
    MemoryBuffer *buf = (MemoryBuffer *)userdata;
    size_t chunk = size * nmemb;

    if (buf->size + chunk > HTTP_MAX_RESPONSE_SIZE) {
        buf->overflowed = true;
        return 0; // aborts the transfer
    }

    char *grown = realloc(buf->data, buf->size + chunk + 1);
    if (!grown) {
        buf->overflowed = true;
        return 0;
    }

    buf->data = grown;
    memcpy(&buf->data[buf->size], contents, chunk);
    buf->size += chunk;
    buf->data[buf->size] = '\0';
    return chunk;
}

// Applies the settings every request shares. Verification stays ON: unlike a
// general-purpose installer talking to arbitrary hosts, we target one known
// server, so there is no reason to weaken it.
static void apply_common_options(CURL *curl, const char *url, struct curl_slist **headers) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);

    // RomM behind a reverse proxy or with S3-backed storage does redirect, so
    // we have to follow. Note the sharp edge: curl strips CURLOPT_USERPWD
    // credentials on a cross-host redirect, but it does NOT strip custom
    // headers set via CURLOPT_HTTPHEADER -- so our bearer token would follow
    // the redirect to whatever host it points at.
    //
    // Not exploitable against a self-hosted RomM serving its own files, but it
    // must be fixed before this client is pointed at anything on the public
    // internet: follow redirects manually and re-attach the token only when
    // the target host matches the configured server.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (caBundleFound) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, HTTP_SYSTEM_CA_BUNDLE);
    }

    if (authHeader[0] != '\0') {
        *headers = curl_slist_append(*headers, authHeader);
    }
}

// mbedTLS validates notBefore/notAfter, which the native ssl:C module does not.
// The 3DS RTC drifts and users set it wrong, so this misfires often enough to
// deserve its own message rather than a raw curl error.
static void log_transport_failure(CURLcode code, const char *url) {
    if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE) {
        log_error("TLS verification failed for %s: %s", url, curl_easy_strerror(code));
        log_error("If the certificate looks fine, check the console's clock and date.");
    } else {
        log_error("Request to %s failed: %s", url, curl_easy_strerror(code));
    }
}

static bool perform_with_body(CURL *curl, struct curl_slist *headers, const char *url, HttpResponse *response) {
    MemoryBuffer buf = {0};

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode code = curl_easy_perform(curl);

    if (code != CURLE_OK) {
        if (buf.overflowed) {
            log_error("Response from %s exceeded %d bytes", url, HTTP_MAX_RESPONSE_SIZE);
        } else {
            log_transport_failure(code, url);
        }
        free(buf.data);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->statusCode);

    // A 204, or a 200 with no body, leaves buf.data NULL. Hand back an empty
    // string instead so callers can treat NULL as "transport failed" only.
    if (!buf.data) {
        buf.data = calloc(1, 1);
        if (!buf.data) return false;
    }

    response->data = buf.data;
    response->size = buf.size;
    return true;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

bool http_get(const char *url, HttpResponse *response) {
    memset(response, 0, sizeof(HttpResponse));

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist *headers = NULL;
    apply_common_options(curl, url, &headers);
    headers = curl_slist_append(headers, "Accept: application/json");

    bool ok = perform_with_body(curl, headers, url, response);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

bool http_post_json(const char *url, const char *jsonBody, HttpResponse *response) {
    memset(response, 0, sizeof(HttpResponse));

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist *headers = NULL;
    apply_common_options(curl, url, &headers);
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody ? jsonBody : "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(jsonBody ? jsonBody : ""));

    bool ok = perform_with_body(curl, headers, url, response);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

// ---------------------------------------------------------------------------
// Streaming download
// ---------------------------------------------------------------------------

typedef struct {
    FILE *file;
    HttpProgressCb progressCb;
    bool cancelled;
} DownloadState;

static size_t write_to_file(void *contents, size_t size, size_t nmemb, void *userdata) {
    DownloadState *state = (DownloadState *)userdata;
    // fwrite returns whole items written; curl wants bytes handled. These are
    // the same only because curl always passes size == 1, which is not
    // guaranteed by the interface.
    return fwrite(contents, size, nmemb, state->file) * size;
}

static int report_progress(void *userdata, curl_off_t total, curl_off_t received, curl_off_t ultotal,
                           curl_off_t uploaded) {
    (void)ultotal;
    (void)uploaded;

    DownloadState *state = (DownloadState *)userdata;
    if (!state->progressCb) return 0;

    if (!state->progressCb((uint64_t)received, (uint64_t)total)) {
        state->cancelled = true;
        return 1; // non-zero aborts the transfer
    }
    return 0;
}

bool http_download_to_file(const char *url, const char *destPath, HttpProgressCb progressCb) {
    FILE *file = fopen(destPath, "wb");
    if (!file) {
        log_error("Could not open %s for writing", destPath);
        return false;
    }

    // Batch stdio into large SD writes. The card is slow per-operation, so
    // fewer, bigger writes beat many small ones.
    char *writeBuffer = malloc(DOWNLOAD_WRITE_BUFFER);
    if (writeBuffer) {
        setvbuf(file, writeBuffer, _IOFBF, DOWNLOAD_WRITE_BUFFER);
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        free(writeBuffer);
        remove(destPath);
        return false;
    }

    DownloadState state = {.file = file, .progressCb = progressCb, .cancelled = false};

    struct curl_slist *headers = NULL;
    apply_common_options(curl, url, &headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, (long)DOWNLOAD_READ_BUFFER);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, report_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode code = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(file);
    free(writeBuffer);

    if (code != CURLE_OK) {
        if (state.cancelled) {
            log_info("Download cancelled: %s", destPath);
        } else {
            log_transport_failure(code, url);
        }
        remove(destPath);
        return false;
    }

    if (status < 200 || status >= 300) {
        log_error("Download of %s returned HTTP %ld", url, status);
        remove(destPath);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Multipart upload
// ---------------------------------------------------------------------------

bool http_post_file(const char *url, const char *fieldName, const char *filePath, HttpResponse *response) {
    memset(response, 0, sizeof(HttpResponse));

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, fieldName);
    if (curl_mime_filedata(part, filePath) != CURLE_OK) {
        log_error("Could not attach %s to the upload", filePath);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return false;
    }

    struct curl_slist *headers = NULL;
    apply_common_options(curl, url, &headers);
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    bool ok = perform_with_body(curl, headers, url, response);

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}
