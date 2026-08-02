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
#include <strings.h>
#include <time.h>
#include <unistd.h>
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
static char caBundlePath[128] = "";
static char authHeader[576] = "";

// Origin (scheme://host:port) of the configured server. The bearer token is
// only ever attached to requests going here.
static char trustedOrigin[256] = "";

#define MAX_REDIRECTS 5

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// A 3DS with a wrong clock rejects perfectly good certificates, because
// mbedTLS validates notBefore/notAfter while the console's own SSL module does
// not. Let's Encrypt certificates are short-lived, so this bites sooner there
// than with a long-dated certificate. Checking at startup turns a confusing
// TLS error into an obvious cause.
static void warn_if_clock_looks_wrong(void) {
    // The build date is a sound lower bound: this binary cannot legitimately be
    // running before it was compiled. __DATE__ is "Mmm dd yyyy"; only the year
    // is needed, and strptime is unavailable here.
    int builtYear = atoi(__DATE__ + 7);
    if (builtYear < 2000) return;

    time_t now = time(NULL);
    struct tm current;
    if (!gmtime_r(&now, &current)) return;

    int currentYear = current.tm_year + 1900;
    if (currentYear < builtYear) {
        log_error("The console's clock reads %d, before this build (%d).", currentYear, builtYear);
        log_error("HTTPS will fail until the date is fixed in System Settings.");
    }
}

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

    static const char *candidates[] = {HTTP_USER_CA_BUNDLE, HTTP_SYSTEM_CA_BUNDLE, HTTP_BUNDLED_CA};
    caBundlePath[0] = '\0';
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *ca = fopen(candidates[i], "r");
        if (ca) {
            fclose(ca);
            snprintf(caBundlePath, sizeof(caBundlePath), "%s", candidates[i]);
            break;
        }
    }

    if (caBundlePath[0] != '\0') {
        log_info("Verifying certificates against %s", caBundlePath);
    } else {
        // Not fatal: plain HTTP still works, which is how development on a LAN
        // happens. HTTPS will fail, loudly, rather than silently unverified.
        log_error("No CA bundle found -- HTTPS will fail. Is romfs missing from the build?");
    }

    warn_if_clock_looks_wrong();

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
    return caBundlePath[0] != '\0';
}

const char *http_ca_bundle_path(void) {
    return caBundlePath;
}

// Copies "scheme://host[:port]" out of a URL, stopping at the path.
static void extract_origin(const char *url, char *out, size_t outLen) {
    out[0] = '\0';
    if (!url) return;

    const char *schemeEnd = strstr(url, "://");
    if (!schemeEnd) return;

    const char *hostStart = schemeEnd + 3;
    const char *hostEnd = strchr(hostStart, '/');
    size_t len = hostEnd ? (size_t)(hostEnd - url) : strlen(url);
    if (len >= outLen) len = outLen - 1;

    memcpy(out, url, len);
    out[len] = '\0';
}

void http_set_trusted_origin(const char *url) {
    extract_origin(url, trustedOrigin, sizeof(trustedOrigin));

    if (trustedOrigin[0] == '\0') {
        // Without an origin nothing is ever trusted, so every request goes out
        // unauthenticated and the server answers 403 -- while pairing appears
        // to have succeeded. Loud, because the symptom points nowhere near the
        // cause.
        log_error("Cannot read a scheme and host from '%s'.", url ? url : "(none)");
        log_error("Requests will be unauthenticated. Set the URL as https://host in Settings.");
        return;
    }

    log_info("Credentials will only be sent to %s", trustedOrigin);
}

// Whether a URL points at the configured server. Compared including scheme and
// port, so an https -> http downgrade on the same host is also untrusted.
static bool url_is_trusted(const char *url) {
    if (trustedOrigin[0] == '\0') return false;

    char origin[256];
    extract_origin(url, origin, sizeof(origin));
    if (origin[0] == '\0') return false;

    return strcasecmp(origin, trustedOrigin) == 0;
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

    // Redirects are followed by hand rather than by curl. curl strips
    // CURLOPT_USERPWD on a cross-host redirect but does NOT strip custom
    // headers, so with CURLOPT_FOLLOWLOCATION the bearer token would be sent
    // to whatever host the redirect names -- and RomM does redirect, to S3 or
    // through a reverse proxy. Each hop is re-issued explicitly and the token
    // is attached only when the target is the configured server.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (caBundlePath[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_CAINFO, caBundlePath);
    }

    if (authHeader[0] != '\0' && url_is_trusted(url)) {
        *headers = curl_slist_append(*headers, authHeader);
    }
}

// Reads the redirect target for a 3xx response. Returns false when the response
// is not a redirect or names no destination.
static bool next_redirect(CURL *curl, long status, char *out, size_t outLen) {
    if (status < 300 || status >= 400) return false;

    char *location = NULL;
    if (curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location) != CURLE_OK || !location) {
        return false;
    }

    snprintf(out, outLen, "%s", location);
    return true;
}

// mbedTLS validates notBefore/notAfter, which the native ssl:C module does not.
// The 3DS RTC drifts and users set it wrong, so this misfires often enough to
// deserve its own message rather than a raw curl error.
static void log_transport_failure(CURLcode code, const char *url) {
    switch (code) {
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
        log_error("Certificate rejected for %s", url);
        // mbedTLS checks notBefore/notAfter, which the 3DS's own SSL module
        // does not -- so a console whose clock is wrong looks exactly like an
        // untrusted certificate, and that is by far the more common cause.
        log_error("Check the console's date and time first.");
        log_error("Self-signed? Put its CA at %s", HTTP_USER_CA_BUNDLE);
        break;
    case CURLE_SSL_CONNECT_ERROR:
        log_error("TLS handshake failed for %s", url);
        log_error("The server may require TLS 1.3; mbedTLS 2.28 speaks 1.2.");
        break;
    case CURLE_COULDNT_RESOLVE_HOST:
        log_error("Could not resolve the host in %s", url);
        break;
    case CURLE_COULDNT_CONNECT:
        log_error("Could not connect to %s", url);
        break;
    default:
        log_error("Request to %s failed: %s", url, curl_easy_strerror(code));
        break;
    }
}

// Performs the request, following redirects by re-issuing each hop with the
// headers appropriate to its destination. `headers` covers the first hop only;
// subsequent hops get a fresh list, which is the point -- the token is dropped
// when a redirect leaves the configured server.
static bool perform_with_body(CURL *curl, struct curl_slist *headers, const char *url, HttpResponse *response) {
    MemoryBuffer buf = {0};

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode code = curl_easy_perform(curl);

    // Follow redirects by hand.
    char nextUrl[1024];
    struct curl_slist *hopHeaders = NULL;
    for (int hop = 0; code == CURLE_OK && hop < MAX_REDIRECTS; hop++) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (!next_redirect(curl, status, nextUrl, sizeof(nextUrl))) break;

        if (!url_is_trusted(nextUrl)) {
            log_debug("Redirect leaves the server; continuing without credentials");
        }

        // Discard the redirect body before collecting the next response.
        free(buf.data);
        memset(&buf, 0, sizeof(buf));

        if (hopHeaders) curl_slist_free_all(hopHeaders);
        hopHeaders = NULL;
        apply_common_options(curl, nextUrl, &hopHeaders);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hopHeaders);

        code = curl_easy_perform(curl);
    }
    if (hopHeaders) curl_slist_free_all(hopHeaders);

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

    // Same manual redirect handling as above. RomM redirects downloads to S3
    // or through a proxy, and those hops must not carry the token.
    char nextUrl[1024];
    struct curl_slist *hopHeaders = NULL;
    for (int hop = 0; code == CURLE_OK && hop < MAX_REDIRECTS; hop++) {
        if (!next_redirect(curl, status, nextUrl, sizeof(nextUrl))) break;

        if (!url_is_trusted(nextUrl)) {
            log_debug("Download redirect leaves the server; continuing without credentials");
        }

        // Restart the file: the redirect response body, if any, was written to
        // it, and the real content begins at the next hop.
        rewind(file);
        if (ftruncate(fileno(file), 0) != 0) {
            log_error("Could not reset %s before following a redirect", destPath);
            code = CURLE_WRITE_ERROR;
            break;
        }
        state.cancelled = false;

        if (hopHeaders) curl_slist_free_all(hopHeaders);
        hopHeaders = NULL;
        apply_common_options(curl, nextUrl, &hopHeaders);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hopHeaders);

        code = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    if (hopHeaders) curl_slist_free_all(hopHeaders);

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
// Streaming to an arbitrary sink
// ---------------------------------------------------------------------------

typedef struct {
    HttpSinkFn sink;
    void *userdata;
    HttpProgressCb progressCb;
    bool cancelled;
    bool sinkFailed;
} SinkState;

static size_t write_to_sink(void *contents, size_t size, size_t nmemb, void *userdata) {
    SinkState *state = (SinkState *)userdata;
    size_t chunk = size * nmemb;

    if (!state->sink(contents, chunk, state->userdata)) {
        state->sinkFailed = true;
        return 0; // aborts the transfer
    }
    return chunk;
}

static int report_sink_progress(void *userdata, curl_off_t total, curl_off_t received, curl_off_t ultotal,
                                curl_off_t uploaded) {
    (void)ultotal;
    (void)uploaded;

    SinkState *state = (SinkState *)userdata;
    if (!state->progressCb) return 0;

    if (!state->progressCb((uint64_t)received, (uint64_t)total)) {
        state->cancelled = true;
        return 1;
    }
    return 0;
}

bool http_download_to_sink(const char *url, HttpSinkFn sink, void *userdata, HttpProgressCb progressCb) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    SinkState state = {.sink = sink, .userdata = userdata, .progressCb = progressCb};

    struct curl_slist *headers = NULL;
    apply_common_options(curl, url, &headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, (long)DOWNLOAD_READ_BUFFER);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, report_sink_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode code = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    // A redirect body must not reach the sink, so hops are followed only after
    // the sink has been told to discard what it has.
    char nextUrl[1024];
    struct curl_slist *hopHeaders = NULL;
    for (int hop = 0; code == CURLE_OK && hop < MAX_REDIRECTS; hop++) {
        if (!next_redirect(curl, status, nextUrl, sizeof(nextUrl))) break;

        if (hopHeaders) curl_slist_free_all(hopHeaders);
        hopHeaders = NULL;
        apply_common_options(curl, nextUrl, &hopHeaders);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hopHeaders);

        code = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    if (hopHeaders) curl_slist_free_all(hopHeaders);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        if (state.cancelled) {
            log_info("Transfer cancelled");
        } else if (state.sinkFailed) {
            log_error("Aborted: the destination rejected data from %s", url);
        } else {
            log_transport_failure(code, url);
        }
        return false;
    }

    if (status < 200 || status >= 300) {
        log_error("%s returned HTTP %ld", url, status);
        return false;
    }

    return true;
}

bool http_get_range(const char *url, uint64_t from, uint64_t to, HttpResponse *response) {
    memset(response, 0, sizeof(HttpResponse));

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist *headers = NULL;
    apply_common_options(curl, url, &headers);

    char range[64];
    snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)from, (unsigned long long)to);
    curl_easy_setopt(curl, CURLOPT_RANGE, range);

    bool ok = perform_with_body(curl, headers, url, response);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
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
