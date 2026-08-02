/*
 * Logging module - Leveled logging with subscriber pattern
 */

#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_BUFFER_SIZE 1024

static LogLevel currentLevel = LOG_INFO;
static LogSubscriber subscribers[LOG_MAX_SUBSCRIBERS] = {NULL};
static int subscriberCount = 0;

static const char *levelNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

void log_init(void) {
    currentLevel = LOG_INFO;
    subscriberCount = 0;
    for (int i = 0; i < LOG_MAX_SUBSCRIBERS; i++) {
        subscribers[i] = NULL;
    }
}

void log_exit(void) {
    subscriberCount = 0;
    for (int i = 0; i < LOG_MAX_SUBSCRIBERS; i++) {
        subscribers[i] = NULL;
    }
}

void log_set_level(LogLevel level) {
    currentLevel = level;
}

LogLevel log_get_level(void) {
    return currentLevel;
}

bool log_subscribe(LogSubscriber subscriber) {
    if (!subscriber) return false;

    // Check if already subscribed
    for (int i = 0; i < subscriberCount; i++) {
        if (subscribers[i] == subscriber) {
            return true;
        }
    }

    if (subscriberCount >= LOG_MAX_SUBSCRIBERS) {
        return false;
    }

    subscribers[subscriberCount++] = subscriber;
    return true;
}

void log_unsubscribe(LogSubscriber subscriber) {
    for (int i = 0; i < subscriberCount; i++) {
        if (subscribers[i] == subscriber) {
            // Shift remaining subscribers down
            for (int j = i; j < subscriberCount - 1; j++) {
                subscribers[j] = subscribers[j + 1];
            }
            subscribers[--subscriberCount] = NULL;
            return;
        }
    }
}

const char *log_level_name(LogLevel level) {
    if (level >= 0 && level <= LOG_FATAL) {
        return levelNames[level];
    }
    return "UNKNOWN";
}

static void log_message(LogLevel level, const char *fmt, va_list args) {
    // Skip if below current log level
    if (level < currentLevel) {
        return;
    }

    // Format the message
    char buffer[LOG_BUFFER_SIZE];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    // Notify all subscribers
    for (int i = 0; i < subscriberCount; i++) {
        if (subscribers[i]) {
            subscribers[i](level, buffer);
        }
    }
}

void log_trace(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_TRACE, fmt, args);
    va_end(args);
}

void log_debug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_DEBUG, fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_INFO, fmt, args);
    va_end(args);
}

void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_WARN, fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_ERROR, fmt, args);
    va_end(args);
}

void log_fatal(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_FATAL, fmt, args);
    va_end(args);
}

// Result codes pack a level, summary, module and description into 32 bits; see
// 3dbrew. Only the module and summary are named here, which is what narrows a
// failure -- the description is title- and call-specific.
const char *log_result_text(long result) {
    static char text[96];

    unsigned code = (unsigned)result;
    unsigned description = code & 0x3FF;
    unsigned module = (code >> 10) & 0xFF;
    unsigned summary = (code >> 21) & 0x3F;
    unsigned level = (code >> 27) & 0x1F;

    // Module numbering per 3dbrew's RM_* list. Order matters and is easy to get
    // wrong by omitting an entry: an off-by-one here reports a confidently
    // incorrect subsystem, which is worse than printing the raw value.
    static const char *const modules[] = {
        "common",  "kernel",    "util",     "file server", "loader server",
        "tcb",     "os",        "dbg",      "dmnt",        "pdn",
        "gsp",     "i2c",       "gpio",     "dd",          "codec",
        "spi",     "pxi",       "fs",       "di",          "hid",
        "cam",     "pi",        "pm",       "pm low",      "fsi",
        "srv",     "ndm",       "nwm",      "soc",         "ldr",
        "acc",     "romfs",     "am",       "hio",         "updater",
        "mic",     "fnd",       "mp",       "mpwl",        "ac",
        "http",    "dsp",       "snd",      "dlp",         "hio low",
        "csnd",    "ssl",       "am low",   "nex",         "friends",
        "rdt",     "applet",    "nim",      "ptm",         "midi",
        "mc",      "swc",       "fatfs",    "ngc",         "card",
        "cardnor", "sdmc",      "boss",     "dbm",         "config",
        "ps",      "cec",       "ir",       "uds",         "pl",
        "cup",     "gyroscope", "mcu",      "ns",          "news",
        "ro",      "gd",        "card spi", "ec",          "web browser",
        "test",    "enc",       "pia",      "act",         "vctl",
        "rpl",     "chrono",    "nfc",      "mvd",         "qtm",
        "npns",    "cpp",       "nfp",
    };

    static const char *const summaries[] = {"success",        "nothing happened", "would block",    "out of resource",
                                            "not found",      "invalid state",    "not supported",  "invalid argument",
                                            "wrong argument", "canceled",         "status changed", "internal"};
    static const char *const levels[] = {"success", "info"};

    const char *moduleName = module < sizeof(modules) / sizeof(modules[0]) ? modules[module] : "?";
    const char *summaryName = summary < sizeof(summaries) / sizeof(summaries[0]) ? summaries[summary] : "?";
    const char *levelName = level < sizeof(levels) / sizeof(levels[0]) ? levels[level]
                            : level == 25                              ? "status"
                            : level == 26                              ? "temporary"
                            : level == 27                              ? "permanent"
                            : level == 28                              ? "usage"
                                                                       : "?";

    snprintf(text, sizeof(text), "0x%08X %s/%s/%s d%u", code, moduleName, summaryName, levelName, description);
    return text;
}
