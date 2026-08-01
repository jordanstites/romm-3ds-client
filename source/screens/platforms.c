/*
 * Platforms screen - Display list of platforms from RomM
 */

#include "platforms.h"
#include "../config.h"
#include "../ui.h"
#include "../listnav.h"
#include <stdio.h>
#include <string.h>

static Platform *platformList = NULL;
static ListNav nav;

void platforms_init(void) {
    platformList = NULL;
    listnav_reset(&nav);
}

void platforms_set_data(Platform *platforms, int count) {
    platformList = platforms;
    listnav_set(&nav, count, count);
}

PlatformsResult platforms_update(u32 kDown, int *outSelectedIndex) {
    if (!platformList || nav.count == 0) {
        return PLATFORMS_NONE;
    }

    listnav_update(&nav, kDown);

    if (kDown & KEY_A) {
        if (outSelectedIndex) *outSelectedIndex = nav.selectedIndex;
        return PLATFORMS_SELECTED;
    }

    if (kDown & KEY_X) {
        if (outSelectedIndex) *outSelectedIndex = nav.selectedIndex;
        return PLATFORMS_SET_FOLDER;
    }

    return PLATFORMS_NONE;
}

void platforms_draw(void) {
    ui_draw_header("Platforms");

    if (!platformList || nav.count == 0) {
        ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT / 2, "No platforms found.", UI_COLOR_TEXT_DIM);
        return;
    }

    float y = UI_HEADER_HEIGHT + UI_PADDING;
    float itemWidth = SCREEN_TOP_WIDTH - (UI_PADDING * 2);

    int start, end;
    listnav_visible_range(&nav, &start, &end);

    for (int i = start; i < end; i++) {
        bool selected = (i == nav.selectedIndex);
        if (selected) {
            ui_draw_rect(UI_PADDING, y, itemWidth, UI_LINE_HEIGHT, UI_COLOR_SELECTED);
        }

        char itemText[224];
        snprintf(itemText, sizeof(itemText), "%.150s (%d)", platformList[i].displayName, platformList[i].romCount);
        ui_draw_text(UI_PADDING * 2, y + 2, itemText, UI_COLOR_TEXT);

        // Which SD folder this platform reads and writes. Without it the app
        // cannot tell what is already on the card, so make it visible rather
        // than something only discovered by a download failing.
        const char *folder = config_get_platform_folder(platformList[i].slug);
        const char *folderLabel = (folder && folder[0]) ? folder : "not set";
        u32 folderColour = (folder && folder[0]) ? UI_COLOR_TEXT_DIM : UI_COLOR_GOLD;

        float labelWidth = ui_get_text_width_scaled(folderLabel, 0.45f);
        ui_draw_text_scaled(UI_PADDING + itemWidth - labelWidth - UI_PADDING, y + 4, folderLabel, folderColour, 0.45f);

        y += UI_LINE_HEIGHT;
    }

    if (nav.count > UI_VISIBLE_ITEMS) {
        listnav_draw_scroll_indicator(&nav);
    }

    ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING, "A: Select \xC2\xB7 X: Set SD folder",
                 UI_COLOR_TEXT_DIM);
}
