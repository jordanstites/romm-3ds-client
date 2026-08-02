/*
 * Settings screen - Configure server connection
 */

#include "settings.h"
#include "../ui.h"
#include "../browser.h"
#include <stdio.h>
#include <string.h>

// Credentials are not settings. Pairing happens on its own screen and the
// resulting token is stored by the auth module, never in config.ini.
typedef enum {
    FIELD_SERVER_URL,
    FIELD_SERVER_SCHEME,
    FIELD_ROM_FOLDER,
    FIELD_SHOW_ALL_PLATFORMS,
    FIELD_COUNT
} SettingsField;

static Config *currentConfig = NULL;
static int selectedField = FIELD_SERVER_URL;
static bool browsingFolders = false;
static int scrollOffset = 0;

// Each field takes 2 lines (label + value), we can show 3 fields + footer
#define SETTINGS_VISIBLE_FIELDS 3

void settings_init(Config *config) {
    currentConfig = config;
    selectedField = FIELD_SERVER_URL;
    browsingFolders = false;
    scrollOffset = 0;
}

SettingsResult settings_update(u32 kDown) {
    if (!currentConfig) return SETTINGS_NONE;

    // Handle folder browser mode
    if (browsingFolders) {
        bool selected = browser_update(kDown);
        if (selected) {
            snprintf(currentConfig->romFolder, sizeof(currentConfig->romFolder), "%s", browser_get_selected_path());
            browsingFolders = false;
            browser_exit();
        } else if (browser_was_cancelled()) {
            browsingFolders = false;
            browser_exit();
        }
        return SETTINGS_NONE;
    }

    // Navigation
    if (kDown & KEY_DOWN) {
        selectedField = (selectedField + 1) % FIELD_COUNT;
        // Adjust scroll to keep selection visible
        if (selectedField >= scrollOffset + SETTINGS_VISIBLE_FIELDS) {
            scrollOffset = selectedField - SETTINGS_VISIBLE_FIELDS + 1;
        }
        if (selectedField < scrollOffset) {
            scrollOffset = selectedField;
        }
    }
    if (kDown & KEY_UP) {
        selectedField = (selectedField - 1 + FIELD_COUNT) % FIELD_COUNT;
        // Adjust scroll to keep selection visible
        if (selectedField < scrollOffset) {
            scrollOffset = selectedField;
        }
        if (selectedField >= scrollOffset + SETTINGS_VISIBLE_FIELDS) {
            scrollOffset = selectedField - SETTINGS_VISIBLE_FIELDS + 1;
        }
    }

    // Select/Edit field
    if (kDown & KEY_A) {
        switch (selectedField) {
        case FIELD_SERVER_URL:
            ui_show_keyboard("Server URL", currentConfig->serverUrl, CONFIG_MAX_URL_LEN, false);
            // Adds the scheme if it was left off, which otherwise breaks
            // authentication in a way that looks like a server problem.
            config_normalize_server_url(currentConfig);
            break;
        case FIELD_SERVER_SCHEME:
            config_set_server_scheme(currentConfig, !config_server_uses_https(currentConfig));
            break;
        case FIELD_ROM_FOLDER:
            browser_init(currentConfig->romFolder[0] ? currentConfig->romFolder : "sdmc:/");
            browsingFolders = true;
            break;
        case FIELD_SHOW_ALL_PLATFORMS:
            currentConfig->showAllPlatforms = !currentConfig->showAllPlatforms;
            break;
        default:
            break;
        }
    }

    // Cancel
    if (kDown & KEY_B) {
        return SETTINGS_CANCELLED;
    }

    return SETTINGS_NONE;
}

void settings_draw(void) {
    if (!currentConfig) return;

    // If browsing folders, draw browser instead
    if (browsingFolders) {
        browser_draw();
        return;
    }

    ui_draw_header("Settings");

    float y = UI_HEADER_HEIGHT + UI_PADDING;
    float fieldWidth = SCREEN_TOP_WIDTH - (UI_PADDING * 2) - 8; // Leave room for scrollbar

    // Draw visible fields
    int fieldsDrawn = 0;
    for (int field = scrollOffset; field < FIELD_COUNT && fieldsDrawn < SETTINGS_VISIBLE_FIELDS; field++) {
        switch (field) {
        case FIELD_SERVER_URL:
            ui_draw_text(UI_PADDING, y, "Server URL:", UI_COLOR_TEXT_DIM);
            y += UI_LINE_HEIGHT;
            ui_draw_list_item(UI_PADDING, y, fieldWidth,
                              currentConfig->serverUrl[0] ? currentConfig->serverUrl : "(not set)",
                              selectedField == FIELD_SERVER_URL);
            break;

        case FIELD_SERVER_SCHEME:
            ui_draw_text(UI_PADDING, y, "Protocol:", UI_COLOR_TEXT_DIM);
            y += UI_LINE_HEIGHT;
            ui_draw_list_item(UI_PADDING, y, fieldWidth,
                              config_server_uses_https(currentConfig) ? "https:// (secure)" : "http:// (plain)",
                              selectedField == FIELD_SERVER_SCHEME);
            break;

        case FIELD_ROM_FOLDER:
            ui_draw_text(UI_PADDING, y, "ROM Folder:", UI_COLOR_TEXT_DIM);
            y += UI_LINE_HEIGHT;
            ui_draw_list_item(UI_PADDING, y, fieldWidth,
                              currentConfig->romFolder[0] ? currentConfig->romFolder : "(not set)",
                              selectedField == FIELD_ROM_FOLDER);
            break;

        case FIELD_SHOW_ALL_PLATFORMS:
            ui_draw_text(UI_PADDING, y, "Platforms shown:", UI_COLOR_TEXT_DIM);
            y += UI_LINE_HEIGHT;
            ui_draw_list_item(UI_PADDING, y, fieldWidth,
                              currentConfig->showAllPlatforms ? "All in library" : "Only 3DS-playable",
                              selectedField == FIELD_SHOW_ALL_PLATFORMS);
            break;
        }

        y += UI_LINE_HEIGHT + UI_PADDING;
        fieldsDrawn++;
    }

    // Draw thin scrollbar if content exceeds visible area
    if (FIELD_COUNT > SETTINGS_VISIBLE_FIELDS) {
        float scrollbarX = SCREEN_TOP_WIDTH - 6;
        float scrollbarY = UI_HEADER_HEIGHT + UI_PADDING;
        float scrollbarHeight = SCREEN_TOP_HEIGHT - UI_HEADER_HEIGHT - UI_LINE_HEIGHT - (UI_PADDING * 3);

        // Track (thin line)
        ui_draw_rect(scrollbarX, scrollbarY, 4, scrollbarHeight, UI_COLOR_SCROLLBAR_TRACK);

        // Thumb
        int maxScroll = FIELD_COUNT - SETTINGS_VISIBLE_FIELDS;
        float thumbHeight = (float)SETTINGS_VISIBLE_FIELDS / FIELD_COUNT * scrollbarHeight;
        if (thumbHeight < 10) thumbHeight = 10;
        float thumbY = scrollbarY + ((float)scrollOffset / maxScroll) * (scrollbarHeight - thumbHeight);
        ui_draw_rect(scrollbarX, thumbY, 4, thumbHeight, UI_COLOR_SCROLLBAR_THUMB);
    }

    // Help text at bottom
    ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING, "A: Select \xC2\xB7 B: Cancel",
                 UI_COLOR_TEXT_DIM);
}
