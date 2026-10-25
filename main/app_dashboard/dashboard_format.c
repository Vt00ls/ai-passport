// main/app_dashboard/dashboard_format.c
// Pure formatting helpers. Compilable by a plain host cc (no ESP-IDF, no LVGL).
#include "app_dashboard/dashboard_format.h"

#include <stdio.h>

#define PLACEHOLDER "--"

static void write_text(char *buffer, size_t capacity, const char *text)
{
    if (!buffer || capacity == 0) return;
    if (!text) {
        text = PLACEHOLDER;
    }
    size_t index = 0;
    for (; text[index] != '\0' && index + 1 < capacity; index++) {
        buffer[index] = text[index];
    }
    buffer[index] = '\0';
}

void dashboard_format_age(char *buffer, size_t capacity, uint32_t age_s, bool never_received)
{
    char text[24];
    if (never_received) {
        write_text(buffer, capacity, "NEVER");
        return;
    }
    if (age_s < 60u) {
        snprintf(text, sizeof(text), "%us", (unsigned)age_s);
    } else if (age_s < 3600u) {
        snprintf(text, sizeof(text), "%um %us", (unsigned)(age_s / 60u), (unsigned)(age_s % 60u));
    } else if (age_s < 86400u) {
        snprintf(text, sizeof(text), "%uh %um", (unsigned)(age_s / 3600u),
                 (unsigned)((age_s % 3600u) / 60u));
    } else {
        snprintf(text, sizeof(text), "%ud %uh", (unsigned)(age_s / 86400u),
                 (unsigned)((age_s % 86400u) / 3600u));
    }
    write_text(buffer, capacity, text);
}

void dashboard_format_uptime(char *buffer, size_t capacity, uint32_t seconds)
{
    char text[24];
    if (seconds < 60u) {
        snprintf(text, sizeof(text), "%us", (unsigned)seconds);
    } else if (seconds < 3600u) {
        snprintf(text, sizeof(text), "%um", (unsigned)(seconds / 60u));
    } else if (seconds < 86400u) {
        snprintf(text, sizeof(text), "%uh %um", (unsigned)(seconds / 3600u),
                 (unsigned)((seconds % 3600u) / 60u));
    } else {
        snprintf(text, sizeof(text), "%ud %uh", (unsigned)(seconds / 86400u),
                 (unsigned)((seconds % 86400u) / 3600u));
    }
    write_text(buffer, capacity, text);
}

void dashboard_format_load(char *buffer, size_t capacity, uint16_t load_x100)
{
    char text[16];
    snprintf(text, sizeof(text), "%u.%02u", (unsigned)(load_x100 / 100u),
             (unsigned)(load_x100 % 100u));
    write_text(buffer, capacity, text);
}

void dashboard_format_percent(char *buffer, size_t capacity, bool present, uint8_t percent)
{
    if (!present) {
        write_text(buffer, capacity, PLACEHOLDER);
        return;
    }
    char text[8];
    snprintf(text, sizeof(text), "%u%%", (unsigned)percent);
    write_text(buffer, capacity, text);
}

void dashboard_format_temperature(char *buffer, size_t capacity, bool present, int16_t celsius)
{
    if (!present) {
        write_text(buffer, capacity, PLACEHOLDER);
        return;
    }
    char text[12];
    snprintf(text, sizeof(text), "%dC", (int)celsius);
    write_text(buffer, capacity, text);
}

void dashboard_format_text(char *buffer, size_t capacity, bool present, const char *text)
{
    write_text(buffer, capacity, present ? text : PLACEHOLDER);
}
