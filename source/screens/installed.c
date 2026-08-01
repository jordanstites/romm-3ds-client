/*
 * Installed titles screen - what is actually on this console
 *
 * Exists because the log is a poor place to inspect several dozen titles on a
 * 400px screen, and because native save sync needs these names matched to RomM
 * entries -- so being able to read them is a prerequisite, not a debug aid.
 */

#include "installed.h"
#include "../listnav.h"
#include "../titles.h"
#include "../ui.h"
#include <stdio.h>
#include <string.h>

static ListNav nav;
static bool pickerMode = false;
static u64 pickedTitleId = 0;
static char linkingRomName[128] = "";

static void load_list(void) {
    int count = titles_count();
    if (count == 0) count = titles_scan();
    listnav_reset(&nav);
    listnav_set(&nav, count, count);
}

void installed_init_picker(const char *romName, const char *suggestName) {
    pickerMode = true;
    pickedTitleId = 0;
    snprintf(linkingRomName, sizeof(linkingRomName), "%s", romName ? romName : "");
    load_list();

    // Preselect the name-matched title so the common case is one button press,
    // while still requiring the user to confirm rather than assuming.
    if (suggestName && suggestName[0]) {
        for (int i = 0; i < titles_count(); i++) {
            if (strcmp(titles_get(i)->name, suggestName) == 0) {
                nav.selectedIndex = i;
                break;
            }
        }
    }
}

u64 installed_get_picked(void) {
    return pickedTitleId;
}

void installed_init(void) {
    pickerMode = false;
    pickedTitleId = 0;
    linkingRomName[0] = '\0';
    load_list();
}

InstalledResult installed_update(u32 kDown) {
    if (kDown & KEY_B) return INSTALLED_BACK;

    if (pickerMode && (kDown & KEY_A)) {
        const InstalledTitle *t = titles_get(nav.selectedIndex);
        if (t) {
            pickedTitleId = t->titleId;
            return INSTALLED_PICKED;
        }
    }

    // Rescan on demand: a title installed while the app was open would
    // otherwise never appear.
    if (kDown & KEY_X) {
        int count = titles_scan();
        listnav_reset(&nav);
        listnav_set(&nav, count, count);
    }

    listnav_update(&nav, kDown);
    return INSTALLED_NONE;
}

void installed_draw(void) {
    char header[160];
    if (pickerMode) {
        snprintf(header, sizeof(header), "Link: %.120s", linkingRomName);
    } else {
        snprintf(header, sizeof(header), "Installed - %d title%s", titles_count(), titles_count() == 1 ? "" : "s");
    }
    ui_draw_header(header);

    if (titles_count() == 0) {
        ui_draw_wrapped_text(UI_PADDING, SCREEN_TOP_HEIGHT / 2 - UI_LINE_HEIGHT, SCREEN_TOP_WIDTH - UI_PADDING * 2,
                             "No installed titles found. This needs the AM service, which a Luma3DS setup grants to "
                             "homebrew.",
                             UI_COLOR_TEXT_DIM, 3, 0);
        ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING, "B: Back", UI_COLOR_TEXT_DIM);
        return;
    }

    float y = UI_HEADER_HEIGHT + UI_PADDING;
    float itemWidth = SCREEN_TOP_WIDTH - (UI_PADDING * 2);

    int start, end;
    listnav_visible_range(&nav, &start, &end);

    for (int i = start; i < end && i < titles_count(); i++) {
        const InstalledTitle *t = titles_get(i);
        bool selected = (i == nav.selectedIndex);

        if (selected) {
            ui_draw_rect(UI_PADDING, y, itemWidth, UI_LINE_HEIGHT, UI_COLOR_SELECTED);
        }

        // A title whose SMDH would not read shows its product code instead, and
        // cannot be name-matched -- flag it rather than letting it look normal.
        ui_draw_text(UI_PADDING * 2, y + 2, t->name, t->nameFromSmdh ? UI_COLOR_TEXT : UI_COLOR_GOLD);

        const char *code = t->productCode[0] ? t->productCode : "";
        float codeWidth = ui_get_text_width_scaled(code, 0.45f);
        ui_draw_text_scaled(UI_PADDING + itemWidth - codeWidth - UI_PADDING, y + 4, code, UI_COLOR_TEXT_DIM, 0.45f);

        y += UI_LINE_HEIGHT;
    }

    listnav_draw_scroll_indicator(&nav);

    // Full title ID for the highlighted entry: too wide to show per row, but
    // the thing you need when checking one against the server.
    const InstalledTitle *sel = titles_get(nav.selectedIndex);
    if (sel) {
        char detail[96];
        snprintf(detail, sizeof(detail), "%016llX", (unsigned long long)sel->titleId);
        ui_draw_text_scaled(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT * 2 - 2, detail, UI_COLOR_TEXT_DIM, 0.5f);
    }

    ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING,
                 pickerMode ? "A: Link this title \xC2\xB7 B: Cancel" : "B: Back \xC2\xB7 X: Rescan",
                 UI_COLOR_TEXT_DIM);
}
