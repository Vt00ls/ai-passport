// main/app_dashboard/dashboard_format.h
// Text formatting for the dashboard pages. No LVGL and no ESP-IDF dependency, so
// the truncation and boundary behaviour is covered by host tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// "NEVER" when nothing has been received yet, otherwise "5s", "3m 20s",
// "2h 11m", "1d 4h". Never writes past `capacity`.
void dashboard_format_age(char *buffer, size_t capacity, uint32_t age_s, bool never_received);

// "45s", "34m", "11h 34m", "1d 11h". Drops the smaller unit once it stops
// mattering, so a 40-day uptime still fits on one line.
void dashboard_format_uptime(char *buffer, size_t capacity, uint32_t seconds);

// "0.42" from load_x100 == 42.
void dashboard_format_load(char *buffer, size_t capacity, uint16_t load_x100);

// "--" when `present` is false, otherwise "38%". A missing field must never
// render as 0.
void dashboard_format_percent(char *buffer, size_t capacity, bool present, uint8_t percent);

// "--" when `present` is false, otherwise "-5C" / "51C". Kept ASCII-only so the
// default Montserrat fonts cover every glyph.
void dashboard_format_temperature(char *buffer, size_t capacity, bool present, int16_t celsius);

// "--" when `present` is false, otherwise the string as received.
void dashboard_format_text(char *buffer, size_t capacity, bool present, const char *text);
