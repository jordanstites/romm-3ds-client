/*
 * Sync screen - save synchronisation with RomM
 *
 * Uploads and downloads run unattended. Conflicts do not: when both sides
 * changed, the screen stops and asks, because picking wrong destroys save data
 * the user cannot recover.
 */

#include "sync.h"
#include "../log.h"
#include "../savesync.h"
#include "../ui.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    STAGE_SCANNING,
    STAGE_SUMMARY,  // plan ready, waiting for the user to start
    STAGE_RUNNING,  // executing non-conflict operations
    STAGE_CONFLICT, // paused on a conflict
    STAGE_FINISHED,
    STAGE_FAILED
} SyncStage;

static const Config *currentConfig = NULL;
static AuthToken *currentToken = NULL;

static SyncStage stage = STAGE_SCANNING;
static SyncPlan plan;
static LocalSave localSaves[SAVES_MAX];
static int localSaveCount = 0;

static int cursor = 0;          // index into plan.operations while running
static int conflictChoice = 0;  // 0 = keep local, 1 = keep server, 2 = skip
static int completed = 0;
static int failed = 0;
static int skipped = 0;
static char failureDetail[160] = "";

static void begin(void) {
    stage = STAGE_SCANNING;
    cursor = 0;
    completed = 0;
    failed = 0;
    skipped = 0;
    failureDetail[0] = '\0';

    localSaveCount = saves_scan(currentConfig, localSaves, SAVES_MAX);

    if (!savesync_negotiate(currentConfig, currentToken, localSaves, localSaveCount, &plan)) {
        snprintf(failureDetail, sizeof(failureDetail),
                 "Could not reach the server, or it does not support save sync. RomM 4.9.0 or newer is required.");
        stage = STAGE_FAILED;
        return;
    }

    stage = STAGE_SUMMARY;
}

void sync_screen_init(const Config *config, AuthToken *token) {
    currentConfig = config;
    currentToken = token;
    memset(&plan, 0, sizeof(plan));
    begin();
}

// Advance past operations needing no work, stopping on the next conflict.
static void run_next(void) {
    while (cursor < plan.operationCount) {
        SyncOperation *op = &plan.operations[cursor];

        if (op->action == SYNC_OP_NO_OP) {
            cursor++;
            continue;
        }

        if (op->action == SYNC_OP_CONFLICT && op->resolution == SYNC_RESOLVE_UNRESOLVED) {
            conflictChoice = 0;
            stage = STAGE_CONFLICT;
            return;
        }

        if (savesync_execute(currentConfig, currentToken, op)) {
            if (op->done) {
                completed++;
            } else if (op->skipped) {
                skipped++;
            }
        } else {
            failed++;
        }
        cursor++;

        // One operation per frame keeps the UI responsive; transfers are
        // synchronous and a large plan would otherwise freeze the console.
        return;
    }

    savesync_complete(currentConfig, &plan);
    stage = STAGE_FINISHED;
}

SyncScreenResult sync_screen_update(u32 kDown) {
    switch (stage) {
    case STAGE_SCANNING:
        return SYNC_SCREEN_NONE;

    case STAGE_SUMMARY:
        if (kDown & KEY_A) {
            if (plan.uploadCount + plan.downloadCount + plan.conflictCount == 0) {
                stage = STAGE_FINISHED;
            } else {
                stage = STAGE_RUNNING;
            }
        }
        if (kDown & KEY_B) return SYNC_SCREEN_DONE;
        return SYNC_SCREEN_NONE;

    case STAGE_RUNNING:
        run_next();
        return SYNC_SCREEN_NONE;

    case STAGE_CONFLICT: {
        if (kDown & KEY_UP) conflictChoice = (conflictChoice + 2) % 3;
        if (kDown & KEY_DOWN) conflictChoice = (conflictChoice + 1) % 3;

        if (kDown & KEY_A) {
            SyncOperation *op = &plan.operations[cursor];
            op->resolution = (conflictChoice == 0)   ? SYNC_RESOLVE_KEEP_LOCAL
                             : (conflictChoice == 1) ? SYNC_RESOLVE_KEEP_SERVER
                                                     : SYNC_RESOLVE_SKIP;
            stage = STAGE_RUNNING;
        }
        return SYNC_SCREEN_NONE;
    }

    case STAGE_FINISHED:
    case STAGE_FAILED:
        if (kDown & KEY_A) {
            if (stage == STAGE_FAILED) {
                begin();
            } else {
                return SYNC_SCREEN_DONE;
            }
        }
        if (kDown & KEY_B) return SYNC_SCREEN_DONE;
        return SYNC_SCREEN_NONE;
    }

    return SYNC_SCREEN_NONE;
}

static void draw_centered(float y, const char *text, u32 color) {
    float w = ui_get_text_width(text);
    ui_draw_text((SCREEN_TOP_WIDTH - w) / 2, y, text, color);
}

void sync_screen_draw(void) {
    ui_draw_header("Save Sync");

    char line[256];
    float y = UI_HEADER_HEIGHT + UI_PADDING;

    switch (stage) {
    case STAGE_SCANNING:
        ui_draw_loading("Scanning and hashing saves...");
        break;

    case STAGE_SUMMARY:
        snprintf(line, sizeof(line), "%d local save%s found", localSaveCount, localSaveCount == 1 ? "" : "s");
        ui_draw_text(UI_PADDING, y, line, UI_COLOR_TEXT_DIM);
        y += UI_LINE_HEIGHT * 2;

        snprintf(line, sizeof(line), "Upload to server:    %d", plan.uploadCount);
        ui_draw_text(UI_PADDING, y, line, UI_COLOR_TEXT);
        y += UI_LINE_HEIGHT;
        snprintf(line, sizeof(line), "Download to console: %d", plan.downloadCount);
        ui_draw_text(UI_PADDING, y, line, UI_COLOR_TEXT);
        y += UI_LINE_HEIGHT;
        snprintf(line, sizeof(line), "Needs a decision:    %d", plan.conflictCount);
        ui_draw_text(UI_PADDING, y, line, plan.conflictCount > 0 ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM);
        y += UI_LINE_HEIGHT;
        snprintf(line, sizeof(line), "Already in sync:     %d", plan.noOpCount);
        ui_draw_text(UI_PADDING, y, line, UI_COLOR_TEXT_DIM);

        draw_centered(SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT * 2 - UI_PADDING,
                      plan.uploadCount + plan.downloadCount + plan.conflictCount > 0 ? "A: Start  -  B: Cancel"
                                                                                    : "Nothing to do.  B: Back",
                      UI_COLOR_TEXT_DIM);
        break;

    case STAGE_RUNNING: {
        const SyncOperation *op = (cursor < plan.operationCount) ? &plan.operations[cursor] : NULL;
        snprintf(line, sizeof(line), "%d of %d", cursor + 1, plan.operationCount);
        draw_centered(y + UI_LINE_HEIGHT, line, UI_COLOR_TEXT_DIM);
        if (op) {
            const char *verb = op->action == SYNC_OP_UPLOAD ? "Uploading" : "Downloading";
            draw_centered(y + UI_LINE_HEIGHT * 3, verb, UI_COLOR_TEXT_DIM);
            draw_centered(y + UI_LINE_HEIGHT * 4, op->fileName, UI_COLOR_TEXT);
        }
        break;
    }

    case STAGE_CONFLICT: {
        const SyncOperation *op = &plan.operations[cursor];

        ui_draw_text(UI_PADDING, y, "Both copies changed:", UI_COLOR_TEXT_DIM);
        y += UI_LINE_HEIGHT;
        ui_draw_text(UI_PADDING, y, op->fileName, UI_COLOR_TEXT);
        y += UI_LINE_HEIGHT;

        if (op->serverUpdatedAt[0]) {
            snprintf(line, sizeof(line), "Server copy: %s", op->serverUpdatedAt);
            ui_draw_text(UI_PADDING, y, line, UI_COLOR_TEXT_DIM);
            y += UI_LINE_HEIGHT;
        }
        y += UI_PADDING;

        static const char *choices[] = {"Keep the console's copy (upload it)", "Keep the server's copy (download it)",
                                        "Skip this one for now"};
        float width = SCREEN_TOP_WIDTH - UI_PADDING * 2;
        for (int i = 0; i < 3; i++) {
            ui_draw_list_item(UI_PADDING, y, width, choices[i], conflictChoice == i);
            y += UI_LINE_HEIGHT + 4;
        }

        draw_centered(SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING, "The other copy is kept as a .bak file",
                      UI_COLOR_TEXT_DIM);
        break;
    }

    case STAGE_FINISHED:
        draw_centered(y + UI_LINE_HEIGHT, "Sync complete", UI_COLOR_TEXT);

        snprintf(line, sizeof(line), "%d synced%s", completed, failed > 0 ? "," : "");
        draw_centered(y + UI_LINE_HEIGHT * 2, line, UI_COLOR_TEXT_DIM);

        if (failed > 0) {
            snprintf(line, sizeof(line), "%d failed - see the log", failed);
            draw_centered(y + UI_LINE_HEIGHT * 3, line, UI_COLOR_TEXT);
        }

        // Not a failure: the server offers every save on the account, including
        // ones for games that live on other devices.
        if (skipped > 0) {
            snprintf(line, sizeof(line), "%d save%s for games not on this console", skipped, skipped == 1 ? "" : "s");
            ui_draw_wrapped_text(UI_PADDING, y + UI_LINE_HEIGHT * (failed > 0 ? 4 : 3),
                                 SCREEN_TOP_WIDTH - UI_PADDING * 2, line, UI_COLOR_TEXT_DIM, 2, 0);
        }

        draw_centered(SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT * 2 - UI_PADDING, "A: Done", UI_COLOR_TEXT_DIM);
        break;

    case STAGE_FAILED:
        draw_centered(y + UI_LINE_HEIGHT, "Sync failed", UI_COLOR_TEXT);
        ui_draw_wrapped_text(UI_PADDING, y + UI_LINE_HEIGHT * 3, SCREEN_TOP_WIDTH - UI_PADDING * 2, failureDetail,
                             UI_COLOR_TEXT_DIM, 4, 0);
        draw_centered(SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT * 2 - UI_PADDING, "A: Retry  -  B: Back", UI_COLOR_TEXT_DIM);
        break;
    }
}
