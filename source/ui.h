/*
 * UI module - Graphics helpers using citro2d
 */

#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <citro2d.h>

// Screen dimensions
#define SCREEN_TOP_WIDTH 400
#define SCREEN_TOP_HEIGHT 240
#define SCREEN_BOTTOM_WIDTH 320
#define SCREEN_BOTTOM_HEIGHT 240

// Colors (RGBA8 format)
// RomM's own dark theme, from frontend/src/styles/themes.ts, so the console
// looks like the web UI rather than approximating it.
//   background #0D1117   surface #161B22   toplayer #1C2330
//   primary #8B74E8      secondary #9E8CD6  accent  #E1A38D
#define UI_COLOR_BG C2D_Color32(0x0D, 0x11, 0x17, 0xFF)
#define UI_COLOR_TEXT C2D_Color32(0xFE, 0xFD, 0xFE, 0xFF)     // romm-white
#define UI_COLOR_TEXT_DIM C2D_Color32(0x9E, 0x8C, 0xD6, 0xFF) // secondary
#define UI_COLOR_SELECTED C2D_Color32(0x60, 0x43, 0xC8, 0xFF) // primary-darken
#define UI_COLOR_ACCENT C2D_Color32(0x8B, 0x74, 0xE8, 0xFF)   // primary
#define UI_COLOR_HEADER C2D_Color32(0x16, 0x1B, 0x22, 0xFF)   // surface
#define UI_COLOR_SURFACE C2D_Color32(0x1C, 0x23, 0x30, 0xFF)  // toplayer
#define UI_COLOR_SCROLLBAR_TRACK C2D_Color32(0x1C, 0x23, 0x30, 0xFF)
#define UI_COLOR_SCROLLBAR_THUMB C2D_Color32(0x8B, 0x74, 0xE8, 0xFF)

// Status colours, also RomM's
#define UI_COLOR_SUCCESS C2D_Color32(0x3F, 0xB9, 0x50, 0xFF) // romm-green
#define UI_COLOR_DANGER C2D_Color32(0xDA, 0x36, 0x33, 0xFF)  // romm-red
#define UI_COLOR_INFO C2D_Color32(0x00, 0x70, 0xF3, 0xFF)    // romm-blue
#define UI_COLOR_GOLD C2D_Color32(0xFF, 0xD7, 0x00, 0xFF)    // romm-gold

// Layout constants
#define UI_PADDING 8
#define UI_LINE_HEIGHT 20
#define UI_HEADER_HEIGHT 30
#define UI_VISIBLE_ITEMS 8

// API/data constants
#define ROM_PAGE_SIZE 50

// Initialize UI module (load font, etc)
void ui_init(void);

// Cleanup UI module
void ui_exit(void);

// Draw text at position
void ui_draw_text(float x, float y, const char *text, u32 color);

// Draw text with scale
void ui_draw_text_scaled(float x, float y, const char *text, u32 color, float scale);

// Draw a filled rectangle
void ui_draw_rect(float x, float y, float w, float h, u32 color);

// Draw a list item (text with optional selection highlight)
void ui_draw_list_item(float x, float y, float w, const char *text, bool selected);

// Draw a header bar
void ui_draw_header(const char *title);

// Draw a header bar for bottom screen (narrower)
void ui_draw_header_bottom(const char *title);

// Get text width
float ui_get_text_width(const char *text);
float ui_get_text_width_scaled(const char *text, float scale);

// Draw a centered loading message on top screen
void ui_draw_loading(const char *message);

// Draw progress bar on top screen (progress 0.0 to 1.0, negative if unknown)
// label: action label (e.g., "Downloading...", "Extracting...")
// name: ROM name to display above progress bar
// queueText: optional queue context (e.g., "ROM 1 of 3 in your queue"), NULL if not queued
void ui_draw_progress(float progress, const char *label, const char *sizeText, const char *name, const char *queueText);

// Show software keyboard and get input
// Returns true if user confirmed, false if cancelled
bool ui_show_keyboard(const char *hint, char *buffer, size_t bufferSize, bool password);

// Touch utility - check if a point is inside a rectangle
bool ui_touch_in_rect(int tx, int ty, int x, int y, int w, int h);

// Button widget styles
typedef enum { UI_BUTTON_PRIMARY, UI_BUTTON_SECONDARY, UI_BUTTON_DANGER } UIButtonStyle;

// Draw a styled button with shadow and gradient
void ui_draw_button(float x, float y, float w, float h, const char *text, bool pressed, UIButtonStyle style);

// Icon drawing functions (designed for 20px, scales to any size)
void ui_draw_icon_bug(float x, float y, float size, u32 color);
void ui_draw_icon_gear(float x, float y, float size, u32 color);
void ui_draw_icon_queue(float x, float y, float size, u32 color);
void ui_draw_icon_search(float x, float y, float size, u32 color);
void ui_draw_icon_sync(float x, float y, float size, u32 color);
void ui_draw_icon_home(float x, float y, float size, u32 color);
void ui_draw_icon_info(float x, float y, float size, u32 color);

// Render `text` as a QR code inside a size x size box. Returns false if the
// text will not fit the encoder's version cap.
bool ui_draw_qr(float x, float y, float size, const char *text);

// Transient on-screen message.
//
// Actions triggered from a screen that does not visibly change -- linking a
// title, starting an upload, a refused install -- previously reported only to
// the log, which reads as the button doing nothing at all.
void ui_toast(const char *fmt, ...);
void ui_toast_error(const char *fmt, ...);

// Draws the current message if one is still current. Call last, so it sits on
// top of whatever screen is beneath it.
void ui_draw_toast(void);

// Draw word-wrapped text within maxWidth. Returns number of lines drawn.
// skipLines: skip this many lines before drawing (for scrolling)
// maxLines: maximum lines to draw
int ui_draw_wrapped_text(float x, float y, float maxWidth, const char *text, u32 color, int maxLines, int skipLines);

#endif // UI_H
