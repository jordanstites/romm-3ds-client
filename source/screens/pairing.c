/*
 * Pairing screen - RomM device authorization
 *
 * The console asks the server for a short code, shows it, and polls until the
 * user approves it in RomM's web UI. Nothing is typed on the 3DS and the
 * account password never reaches the console.
 */

#include "pairing.h"
#include "../log.h"
#include "../ui.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    STAGE_REQUESTING, // asking the server for a code
    STAGE_WAITING,    // code displayed, polling for approval
    STAGE_DENIED,
    STAGE_EXPIRED,
    STAGE_FAILED
} PairingStage;

static const Config *currentConfig = NULL;
static AuthToken *outToken = NULL;

static PairingStage stage = STAGE_REQUESTING;
static AuthPairing pairing;
static char failureDetail[128] = "";

// Wall-clock milliseconds, so the countdown stays honest even if we drop frames
// during a blocking poll.
static u64 pairingStartedAt = 0;
static u64 lastPollAt = 0;

static void begin_request(void) {
    stage = STAGE_REQUESTING;
    failureDetail[0] = '\0';

    if (!currentConfig || currentConfig->serverUrl[0] == '\0') {
        snprintf(failureDetail, sizeof(failureDetail), "Set a server URL in Settings first.");
        stage = STAGE_FAILED;
        return;
    }

    if (!auth_begin_pairing(currentConfig->serverUrl, &pairing)) {
        snprintf(failureDetail, sizeof(failureDetail), "Could not reach the server. Check Settings and the network.");
        stage = STAGE_FAILED;
        return;
    }

    pairingStartedAt = osGetTime();
    lastPollAt = pairingStartedAt;
    stage = STAGE_WAITING;
}

void pairing_init(const Config *config, AuthToken *token) {
    currentConfig = config;
    outToken = token;
    memset(&pairing, 0, sizeof(pairing));
    begin_request();
}

static int seconds_remaining(void) {
    if (pairing.expiresInSeconds <= 0) return 0;
    u64 elapsed = (osGetTime() - pairingStartedAt) / 1000;
    int remaining = pairing.expiresInSeconds - (int)elapsed;
    return remaining > 0 ? remaining : 0;
}

PairingResult pairing_update(u32 kDown) {
    if (kDown & KEY_B) return PAIRING_CANCELLED;

    // A retry from any terminal stage starts a fresh code.
    if ((kDown & KEY_A) && (stage == STAGE_FAILED || stage == STAGE_EXPIRED || stage == STAGE_DENIED)) {
        begin_request();
        return PAIRING_NONE;
    }

    if (stage == STAGE_REQUESTING) {
        // begin_request() already ran synchronously; this stage is only visible
        // for the frame before it completes.
        return PAIRING_NONE;
    }

    if (stage != STAGE_WAITING) {
        return PAIRING_NONE;
    }

    if (seconds_remaining() == 0) {
        stage = STAGE_EXPIRED;
        return PAIRING_NONE;
    }

    // Respect the server's advertised interval rather than hammering it.
    u64 now = osGetTime();
    u64 intervalMs = (u64)(pairing.pollIntervalSeconds > 0 ? pairing.pollIntervalSeconds : 5) * 1000;
    if (now - lastPollAt < intervalMs) {
        return PAIRING_NONE;
    }
    lastPollAt = now;

    switch (auth_poll_pairing(currentConfig->serverUrl, &pairing, outToken)) {
    case AUTH_POLL_APPROVED:
        if (!auth_save(outToken)) {
            // The token is valid in memory, so this session works, but it will
            // not survive a restart. Say so rather than pretending it is fine.
            snprintf(failureDetail, sizeof(failureDetail), "Paired, but the token could not be saved to the SD card.");
            stage = STAGE_FAILED;
            return PAIRING_NONE;
        }
        log_info("Paired successfully, device %s", outToken->deviceId);
        return PAIRING_SUCCESS;

    case AUTH_POLL_DENIED:
        stage = STAGE_DENIED;
        return PAIRING_NONE;

    case AUTH_POLL_EXPIRED:
        stage = STAGE_EXPIRED;
        return PAIRING_NONE;

    case AUTH_POLL_ERROR:
        // Transient failures are common on wifi; keep waiting rather than
        // throwing away a code that is still valid.
        log_error("Poll failed, will retry");
        return PAIRING_NONE;

    case AUTH_POLL_PENDING:
    default:
        return PAIRING_NONE;
    }
}

static void draw_centered(float y, const char *text, u32 color) {
    float w = ui_get_text_width(text);
    ui_draw_text((SCREEN_TOP_WIDTH - w) / 2, y, text, color);
}

static void draw_centered_scaled(float y, const char *text, u32 color, float scale) {
    float w = ui_get_text_width_scaled(text, scale);
    ui_draw_text_scaled((SCREEN_TOP_WIDTH - w) / 2, y, text, color, scale);
}

void pairing_draw(void) {
    ui_draw_header("Pair with RomM");

    switch (stage) {
    case STAGE_REQUESTING:
        ui_draw_loading("Requesting a pairing code...");
        break;

    case STAGE_WAITING: {
        draw_centered(52, "Enter this code in RomM:", UI_COLOR_TEXT_DIM);

        // The code is the whole point of this screen, so it gets the space.
        draw_centered_scaled(72, pairing.userCode, UI_COLOR_TEXT, 1.6f);

        draw_centered(126, "Open this address on your computer or phone:", UI_COLOR_TEXT_DIM);
        draw_centered_scaled(146, pairing.verifyUrl, UI_COLOR_TEXT, 0.5f);

        char countdown[64];
        int remaining = seconds_remaining();
        snprintf(countdown, sizeof(countdown), "Waiting for approval - expires in %d:%02d", remaining / 60,
                 remaining % 60);
        draw_centered(176, countdown, UI_COLOR_TEXT_DIM);
        break;
    }

    case STAGE_DENIED:
        draw_centered(80, "Pairing was declined.", UI_COLOR_TEXT);
        draw_centered(104, "A: Try again", UI_COLOR_TEXT_DIM);
        break;

    case STAGE_EXPIRED:
        draw_centered(80, "The pairing code expired.", UI_COLOR_TEXT);
        draw_centered(104, "A: Get a new code", UI_COLOR_TEXT_DIM);
        break;

    case STAGE_FAILED:
        draw_centered(72, "Pairing failed.", UI_COLOR_TEXT);
        ui_draw_wrapped_text(UI_PADDING, 96, SCREEN_TOP_WIDTH - (UI_PADDING * 2), failureDetail, UI_COLOR_TEXT_DIM, 3,
                             0);
        draw_centered(150, "A: Try again", UI_COLOR_TEXT_DIM);
        break;
    }

    ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING, "B: Back", UI_COLOR_TEXT_DIM);
}
