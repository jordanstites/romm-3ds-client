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
// Status is filesystem work -- several stat() calls per ROM, plus a scan of
// every installed title. Computing it in the draw loop meant redoing all of it
// for every visible row at 60fps, which made the 3DS platform crawl. Cache it
// alongside the list and refresh only when the list changes.
static RomStatus *statusList = NULL;
static int statusCount = 0;
static char currentPlatform[128] = "";
static char currentPlatformSlug[CONFIG_MAX_SLUG_LEN] = "";
static const Config *statusConfig = NULL;
static ListNav nav;

void roms_set_status_context(const Config *config, const char *platformSlug) {
    statusConfig = config;
    snprintf(currentPlatformSlug, sizeof(currentPlatformSlug), "%s", platformSlug ? platformSlug : "");
}

// Recompute cached status for rows [from, count).
static void refresh_status(int from, int count) {
    if (!statusConfig || !currentPlatformSlug[0] || !romList) return;

    RomStatus *grown = realloc(statusList, (size_t)count * sizeof(RomStatus));
    if (!grown) return;
    statusList = grown;

    for (int i = from; i < count; i++) {
        statusList[i] =
            romstatus_for(statusConfig, romList[i].id, currentPlatformSlug, romList[i].name, romList[i].fsName);
    }
    statusCount = count;
}

void roms_refresh_status(void) {
    refresh_status(0, statusCount);
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
    if (statusList) {
        free(statusList);
        statusList = NULL;
    }
    statusCount = 0;
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
    statusCount = 0;
    refresh_status(0, count);
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
    int previous = nav.count;
    nav.count += count;
    free(roms);

    // Only the newly appended rows need computing.
    refresh_status(previous, nav.count);
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

            RomStatus status = (i < statusCount) ? statusList[i] : (RomStatus){0};

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

            // Presence. Installed takes visual priority over a staged file --
            // for a 3DS title the installed copy is the one you play, and a
            // leftover .3ds in the folder is only there to be converted.
            const char *presence = NULL;
            u32 presenceColour = UI_COLOR_INFO;
            if (status.installed && status.onDevice) {
                presence = "IN+"; // installed, and a staged file is still around
                presenceColour = UI_COLOR_SUCCESS;
            } else if (status.installed) {
                // A confirmed link is shown differently from a name guess: only
                // the confirmed one is safe to sync a save archive against.
                presence = status.linked ? "IN*" : "IN";
                presenceColour = UI_COLOR_SUCCESS;
            } else if (status.onDevice) {
                presence = "DL";
            }

            if (presence) {
                bx -= ui_get_text_width_scaled(presence, 0.45f);
                ui_draw_text_scaled(bx, y + 4, presence, presenceColour, 0.45f);
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
