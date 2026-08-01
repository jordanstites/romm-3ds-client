/*
 * ROMs screen - Display list of ROMs for a platform
 */

#include "roms.h"
#include "../romstatus.h"
#include "../ui.h"
#include "../listnav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Rom *romList = NULL;
static char currentPlatform[128] = "";
static char currentPlatformSlug[CONFIG_MAX_SLUG_LEN] = "";
static const Config *statusConfig = NULL;
static ListNav nav;

void roms_set_status_context(const Config *config, const char *platformSlug) {
    statusConfig = config;
    snprintf(currentPlatformSlug, sizeof(currentPlatformSlug), "%s", platformSlug ? platformSlug : "");
}

void roms_init(void) {
    romList = NULL;
    currentPlatform[0] = '\0';
    listnav_reset(&nav);
}

void roms_clear(void) {
    if (romList) {
        free(romList);
        romList = NULL;
    }
    currentPlatform[0] = '\0';
    listnav_reset(&nav);
}

void roms_set_data(Rom *roms, int count, int total, const char *platformName) {
    if (romList) {
        free(romList);
    }
    romList = roms;
    listnav_set(&nav, count, total);
    snprintf(currentPlatform, sizeof(currentPlatform), "%s", platformName);
}

void roms_append_data(Rom *roms, int count) {
    if (!roms || count == 0) return;

    Rom *newList = realloc(romList, (nav.count + count) * sizeof(Rom));
    if (!newList) {
        free(roms);
        return;
    }

    romList = newList;
    memcpy(&romList[nav.count], roms, count * sizeof(Rom));
    nav.count += count;
    free(roms);
}

bool roms_needs_more_data(void) {
    return listnav_on_load_more(&nav);
}

int roms_get_count(void) {
    return nav.count;
}

int roms_get_id_at(int index) {
    if (!romList || index < 0 || index >= nav.count) {
        return -1;
    }
    return romList[index].id;
}

const Rom *roms_get_at(int index) {
    if (!romList || index < 0 || index >= nav.count) {
        return NULL;
    }
    return &romList[index];
}

int roms_get_selected_index(void) {
    return nav.selectedIndex;
}

RomsResult roms_update(u32 kDown) {
    if (kDown & KEY_B) {
        return ROMS_BACK;
    }

    if (!romList || nav.count == 0) {
        return ROMS_NONE;
    }

    listnav_update(&nav, kDown);

    if (kDown & KEY_A) {
        if (nav.selectedIndex < nav.count) {
            return ROMS_SELECTED;
        }
    }

    if (listnav_on_load_more(&nav)) {
        return ROMS_LOAD_MORE;
    }

    return ROMS_NONE;
}

void roms_draw(void) {
    char headerText[192];
    snprintf(headerText, sizeof(headerText), "ROMs - %s", currentPlatform);
    ui_draw_header(headerText);

    if (!romList || nav.count == 0) {
        ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT / 2, "No ROMs found for this platform.", UI_COLOR_TEXT_DIM);
        ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING, "B: Back to Platforms",
                     UI_COLOR_TEXT_DIM);
        return;
    }

    float y = UI_HEADER_HEIGHT + UI_PADDING;
    float itemWidth = SCREEN_TOP_WIDTH - (UI_PADDING * 2);

    int start, end;
    listnav_visible_range(&nav, &start, &end);

    // Badges are drawn right-aligned, so the name gets the remaining width.
    float badgeZone = 54.0f;

    for (int i = start; i < end; i++) {
        if (i < nav.count) {
            bool selected = (i == nav.selectedIndex);
            if (selected) {
                ui_draw_rect(UI_PADDING, y, itemWidth, UI_LINE_HEIGHT, UI_COLOR_SELECTED);
            }

            RomStatus status = {0};
            if (statusConfig && currentPlatformSlug[0]) {
                status = romstatus_for(statusConfig, romList[i].id, currentPlatformSlug, romList[i].name,
                                       romList[i].fsName);
            }

            // Truncate the name rather than letting it run under the badges.
            char label[288];
            snprintf(label, sizeof(label), "%.255s", romList[i].name);

            // Shorten until it clears the badge zone, then mark the cut.
            float maxNameWidth = itemWidth - badgeZone - UI_PADDING * 2;
            size_t len = strlen(label);
            while (len > 3 && ui_get_text_width(label) > maxNameWidth) {
                len--;
                label[len] = '\0';
            }
            if (len > 3 && len < strlen(romList[i].name)) {
                label[len - 3] = '\0';
                strcat(label, "...");
            }
            ui_draw_text(UI_PADDING + UI_PADDING, y + 2, label, UI_COLOR_TEXT);

            float bx = UI_PADDING + itemWidth - UI_PADDING;

            // Save count, right-most. Green when both sides have one, gold when
            // only one side does -- i.e. a sync would do something.
            if (status.serverSaves > 0 || status.localSaves > 0) {
                char saveText[24];
                snprintf(saveText, sizeof(saveText), "S%d/%d", status.localSaves, status.serverSaves);
                bx -= ui_get_text_width_scaled(saveText, 0.45f);
                u32 colour = (status.serverSaves > 0 && status.localSaves > 0) ? UI_COLOR_SUCCESS : UI_COLOR_GOLD;
                ui_draw_text_scaled(bx, y + 4, saveText, colour, 0.45f);
                bx -= 6;
            }

            // Presence on the console.
            const char *presence = status.onDevice ? "DL" : (status.installed ? "IN" : NULL);
            if (presence) {
                bx -= ui_get_text_width_scaled(presence, 0.45f);
                ui_draw_text_scaled(bx, y + 4, presence, UI_COLOR_INFO, 0.45f);
            }
        } else {
            bool selected = (i == nav.selectedIndex);
            if (selected) {
                ui_draw_rect(UI_PADDING, y, itemWidth, UI_LINE_HEIGHT, UI_COLOR_SELECTED);
            }
            ui_draw_text(UI_PADDING + UI_PADDING, y + 2, "Load more...", selected ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM);
        }
        y += UI_LINE_HEIGHT;
    }

    listnav_draw_scroll_indicator(&nav);

    ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING,
                 "A: Details \xC2\xB7 B: Back \xC2\xB7 L/R: Page", UI_COLOR_TEXT_DIM);
}
