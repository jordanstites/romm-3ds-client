/*
 * About screen - App info and credits
 */

#include "about.h"
#include "../ui.h"

AboutResult about_update(u32 kDown) {
    if (kDown & KEY_B) return ABOUT_BACK;
    if (kDown & KEY_A) return ABOUT_INSTALLED;
    return ABOUT_NONE;
}

void about_draw(void) {
    const char *appName = APP_TITLE;
    float nameW = ui_get_text_width_scaled(appName, 1.0f);
    ui_draw_text_scaled((SCREEN_TOP_WIDTH - nameW) / 2, 34, appName, UI_COLOR_TEXT, 1.0f);

    const char *tagline = "A RomM client for Nintendo 3DS";
    float tagW = ui_get_text_width(tagline);
    ui_draw_text((SCREEN_TOP_WIDTH - tagW) / 2, 64, tagline, UI_COLOR_TEXT_DIM);

    const char *version = "v" APP_VERSION;
    float verW = ui_get_text_width_scaled(version, 0.45f);
    ui_draw_text_scaled((SCREEN_TOP_WIDTH - verW) / 2, 84, version, UI_COLOR_TEXT_DIM, 0.45f);

    float contentWidth = SCREEN_TOP_WIDTH - (UI_PADDING * 2);
    ui_draw_wrapped_text(UI_PADDING, 116, contentWidth,
                         "Free and open source, under the MIT license.\n"
                         "\n"
                         "Built on rommlet by Derek Prior, which provided the "
                         "interface, download queue and build system.",
                         UI_COLOR_TEXT, 5, 0);

    ui_draw_text(UI_PADDING, SCREEN_TOP_HEIGHT - UI_LINE_HEIGHT - UI_PADDING, "A: Installed titles \xC2\xB7 B: Back",
                 UI_COLOR_TEXT_DIM);
}
