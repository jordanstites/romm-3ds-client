/*
 * ROM detail screen - Display ROM information
 */

#include "romdetail.h"
#include "../ui.h"
#include <stdio.h>
#include <string.h>

static RomDetail *currentDetail = NULL;
static int scrollOffset = 0;

void romdetail_init(void) {
    currentDetail = NULL;
    scrollOffset = 0;
}

void romdetail_set_data(RomDetail *detail) {
    currentDetail = detail;
    scrollOffset = 0;
}

RomDetailResult romdetail_update(u32 kDown) {
    // Back to ROM list
    if (kDown & KEY_B) {
        return ROMDETAIL_BACK;
    }

    // Link this ROM to an installed 3DS title, so its save archive can be
    // synced. Only meaningful for native 3DS titles; main.c gates on platform.
    if (kDown & KEY_X) {
        return ROMDETAIL_LINK_TITLE;
    }

    // Upload this title's save archive to RomM. Read-only on the console side,
    // so it cannot damage a save.
    if (kDown & KEY_Y) {
        return ROMDETAIL_UPLOAD_SAVE;
    }

    // Scroll description if needed
    if (kDown & KEY_DOWN) {
        scrollOffset++;
    }
    if (kDown & KEY_UP) {
        if (scrollOffset > 0) scrollOffset--;
    }

    return ROMDETAIL_NONE;
}

void romdetail_draw(void) {
    if (!currentDetail) {
        ui_draw_header("ROM Details");
        ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT / 2, "No ROM selected.", UI_COLOR_TEXT_DIM);
        return;
    }

    ui_draw_header("ROM Details");

    float y = UI_HEADER_HEIGHT + UI_PADDING;
    float contentWidth = SCREEN_TOP_WIDTH - (UI_PADDING * 2);

    // Title (prominent)
    ui_draw_text(UI_PADDING, y, currentDetail->name, UI_COLOR_TEXT);
    y += UI_LINE_HEIGHT;

    // Platform (no label)
    if (currentDetail->platformName[0]) {
        ui_draw_text(UI_PADDING, y, currentDetail->platformName, UI_COLOR_TEXT_DIM);
        y += UI_LINE_HEIGHT;
    }

    // Release date
    if (currentDetail->firstReleaseDate[0]) {
        char dateText[64];
        snprintf(dateText, sizeof(dateText), "Released: %s", currentDetail->firstReleaseDate);
        ui_draw_text(UI_PADDING, y, dateText, UI_COLOR_TEXT_DIM);
        y += UI_LINE_HEIGHT;
    }

    y += UI_PADDING;

    // Description/Summary with wrapping
    if (currentDetail->summary[0]) {
        ui_draw_text(UI_PADDING, y, "Description:", UI_COLOR_TEXT_DIM);
        y += UI_LINE_HEIGHT;

        int maxDescLines = (SCREEN_TOP_HEIGHT - y - UI_LINE_HEIGHT - UI_PADDING * 2) / UI_LINE_HEIGHT;
        ui_draw_wrapped_text(UI_PADDING, y, contentWidth, currentDetail->summary, UI_COLOR_TEXT, maxDescLines,
                             scrollOffset);
    }

    // Help text
    ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING,
                 "B: Back \xC2\xB7 X: Link \xC2\xB7 Y: Upload save", UI_COLOR_TEXT_DIM);
}
