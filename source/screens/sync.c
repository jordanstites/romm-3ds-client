/*
 * Sync screen - save synchronisation with RomM
 *
 * Uploads and downloads run unattended. Conflicts do not: when both sides
 * changed, the screen stops and asks, because picking wrong destroys save data
 * the user cannot recover.
 */

#include "sync.h"
#include "../log.h"
#include "../listnav.h"
#include "../savesync.h"
#include "../ui.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    STAGE_SCANNING,
    STAGE_SUMMARY,  // plan ready, reviewing which entries to run
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

static int cursor = 0;         // index into plan.operations while running
static int conflictChoice = 0; // 0 = keep local, 1 = keep server, 2 = skip
static ListNav reviewNav;      // selection over the actionable entries

// Indices of operations worth reviewing. no_op entries are excluded: there is
// nothing to decide about a save that already matches on both sides.
static int reviewable[SYNC_MAX_OPERATIONS];
static int reviewableCount = 0;

static void build_review_list(void) {
    reviewableCount = 0;
    for (int i = 0; i < plan.operationCount; i++) {
        if (plan.operations[i].action != SYNC_OP_NO_OP) {
            reviewable[reviewableCount++] = i;
        }
    }
    listnav_reset(&reviewNav);
    listnav_set(&reviewNav, reviewableCount, reviewableCount);
}

static int selected_count(void) {
    int n = 0;
    for (int i = 0; i < reviewableCount; i++) {
        if (plan.operations[reviewable[i]].selected) n++;
    }
    return n;
}
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

    savesync_cleanup(localSaves, localSaveCount);

    // Declares how this console syncs. Harmless to repeat, and the device
    // record created by pairing does not carry it.
    savesync_register_device(currentConfig, currentToken);

    localSaveCount = savesync_collect(currentConfig, localSaves, SAVES_MAX);

    if (!savesync_negotiate(currentConfig, currentToken, localSaves, localSaveCount, &plan)) {
        snprintf(failureDetail, sizeof(failureDetail),
                 "Could not reach the server, or it does not support save sync. RomM 4.9.0 or newer is required.");
        stage = STAGE_FAILED;
        return;
    }

    build_review_list();
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

        if (op->action == SYNC_OP_NO_OP || !op->selected) {
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
    savesync_cleanup(localSaves, localSaveCount);
    stage = STAGE_FINISHED;
}

SyncScreenResult sync_screen_update(u32 kDown) {
    switch (stage) {
    case STAGE_SCANNING:
        return SYNC_SCREEN_NONE;

    case STAGE_SUMMARY:
        listnav_update(&reviewNav, kDown);

        // A toggles the highlighted entry rather than starting, so a save can
        // be excluded without abandoning the whole sync.
        if ((kDown & KEY_A) && reviewNav.selectedIndex < reviewableCount) {
            SyncOperation *op = &plan.operations[reviewable[reviewNav.selectedIndex]];
            op->selected = !op->selected;
        }

        // Y selects or clears everything, so "just this one" is two presses
        // rather than many.
        if (kDown & KEY_Y) {
            bool anySelected = selected_count() > 0;
            for (int i = 0; i < reviewableCount; i++) {
                plan.operations[reviewable[i]].selected = !anySelected;
            }
        }

        // Not START: the main loop treats that as quit, so it would close the
        // app mid-review.
        if (kDown & KEY_X) {
            stage = (selected_count() == 0) ? STAGE_FINISHED : STAGE_RUNNING;
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

    case STAGE_SUMMARY: {
        if (reviewableCount == 0) {
            snprintf(line, sizeof(line), "%d save%s checked, all up to date", localSaveCount,
                     localSaveCount == 1 ? "" : "s");
            draw_centered(y + UI_LINE_HEIGHT * 2, "Nothing to sync", UI_COLOR_TEXT);
            draw_centered(y + UI_LINE_HEIGHT * 3, line, UI_COLOR_TEXT_DIM);
            draw_centered(SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT * 2 - UI_PADDING, "B: Back", UI_COLOR_TEXT_DIM);
            break;
        }

        float itemWidth = SCREEN_TOP_WIDTH - (UI_PADDING * 2);
        int start, end;
        listnav_visible_range(&reviewNav, &start, &end);

        for (int i = start; i < end && i < reviewableCount; i++) {
            const SyncOperation *op = &plan.operations[reviewable[i]];
            bool highlighted = (i == reviewNav.selectedIndex);

            if (highlighted) {
                ui_draw_rect(UI_PADDING, y, itemWidth, UI_LINE_HEIGHT, UI_COLOR_SELECTED);
            }

            // Direction is the thing to read at a glance; the checkbox says
            // whether it will actually happen.
            const char *mark = op->selected ? "[x]" : "[ ]";
            const char *verb = op->action == SYNC_OP_UPLOAD     ? "up"
                               : op->action == SYNC_OP_DOWNLOAD ? "down"
                                                                : "conflict";
            u32 verbColour = op->action == SYNC_OP_CONFLICT ? UI_COLOR_GOLD : UI_COLOR_TEXT_DIM;

            ui_draw_text(UI_PADDING + 4, y + 2, mark, op->selected ? UI_COLOR_SUCCESS : UI_COLOR_TEXT_DIM);
            ui_draw_text_scaled(UI_PADDING + 34, y + 4, verb, verbColour, 0.45f);

            char name[160];
            snprintf(name, sizeof(name), "%.140s", op->fileName);
            ui_draw_text_scaled(UI_PADDING + 76, y + 4, name, UI_COLOR_TEXT, 0.5f);

            y += UI_LINE_HEIGHT;
        }

        listnav_draw_scroll_indicator(&reviewNav);

        snprintf(line, sizeof(line), "%d of %d selected", selected_count(), reviewableCount);
        ui_draw_text_scaled(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT * 2 - 2, line, UI_COLOR_TEXT_DIM, 0.5f);

        ui_draw_text_scaled(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING,
                            "A: toggle \xC2\xB7 Y: all/none \xC2\xB7 X: sync \xC2\xB7 B: back", UI_COLOR_TEXT_DIM,
                            0.5f);
        break;
    }

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
