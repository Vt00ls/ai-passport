// tests/test_dashboard_format.c
// Host test for the dashboard text formatting (no LVGL, no ESP-IDF).
#include "app_dashboard/dashboard_format.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            s_failures++;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_STR(actual, expected)                                              \
    do {                                                                         \
        const char *a_ = (actual);                                               \
        const char *e_ = (expected);                                             \
        if (strcmp(a_, e_) != 0) {                                               \
            fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n",          \
                    __FILE__, __LINE__, e_, a_);                                 \
            s_failures++;                                                        \
        }                                                                        \
    } while (0)

static void test_age(void)
{
    char buffer[32];
    dashboard_format_age(buffer, sizeof(buffer), 0, true);
    CHECK_STR(buffer, "NEVER");
    dashboard_format_age(buffer, sizeof(buffer), 0, false);
    CHECK_STR(buffer, "0s");
    dashboard_format_age(buffer, sizeof(buffer), 5, false);
    CHECK_STR(buffer, "5s");
    dashboard_format_age(buffer, sizeof(buffer), 59, false);
    CHECK_STR(buffer, "59s");
    dashboard_format_age(buffer, sizeof(buffer), 60, false);
    CHECK_STR(buffer, "1m 0s");
    dashboard_format_age(buffer, sizeof(buffer), 200, false);
    CHECK_STR(buffer, "3m 20s");
    dashboard_format_age(buffer, sizeof(buffer), 3600, false);
    CHECK_STR(buffer, "1h 0m");
    dashboard_format_age(buffer, sizeof(buffer), 7900, false);
    CHECK_STR(buffer, "2h 11m");
    dashboard_format_age(buffer, sizeof(buffer), 86400u, false);
    CHECK_STR(buffer, "1d 0h");
    dashboard_format_age(buffer, sizeof(buffer), 0xFFFFFFFFu, false);
    CHECK_STR(buffer, "49710d 6h");
}

static void test_uptime(void)
{
    char buffer[32];
    dashboard_format_uptime(buffer, sizeof(buffer), 0);
    CHECK_STR(buffer, "0s");
    dashboard_format_uptime(buffer, sizeof(buffer), 45);
    CHECK_STR(buffer, "45s");
    dashboard_format_uptime(buffer, sizeof(buffer), 900);
    CHECK_STR(buffer, "15m");
    dashboard_format_uptime(buffer, sizeof(buffer), 128099u);
    CHECK_STR(buffer, "1d 11h");
    dashboard_format_uptime(buffer, sizeof(buffer), 3600);
    CHECK_STR(buffer, "1h 0m");
    dashboard_format_uptime(buffer, sizeof(buffer), 0xFFFFFFFFu);
    CHECK_STR(buffer, "49710d 6h");
}

static void test_load(void)
{
    char buffer[16];
    dashboard_format_load(buffer, sizeof(buffer), 42);
    CHECK_STR(buffer, "0.42");
    dashboard_format_load(buffer, sizeof(buffer), 0);
    CHECK_STR(buffer, "0.00");
    dashboard_format_load(buffer, sizeof(buffer), 5);
    CHECK_STR(buffer, "0.05");
    dashboard_format_load(buffer, sizeof(buffer), 100);
    CHECK_STR(buffer, "1.00");
    dashboard_format_load(buffer, sizeof(buffer), 65535);
    CHECK_STR(buffer, "655.35");
}

static void test_percent_and_temperature(void)
{
    char buffer[16];
    dashboard_format_percent(buffer, sizeof(buffer), false, 99);
    CHECK_STR(buffer, "--");
    dashboard_format_percent(buffer, sizeof(buffer), true, 0);
    CHECK_STR(buffer, "0%");
    dashboard_format_percent(buffer, sizeof(buffer), true, 38);
    CHECK_STR(buffer, "38%");
    dashboard_format_percent(buffer, sizeof(buffer), true, 100);
    CHECK_STR(buffer, "100%");

    dashboard_format_temperature(buffer, sizeof(buffer), false, 51);
    CHECK_STR(buffer, "--");
    dashboard_format_temperature(buffer, sizeof(buffer), true, 51);
    CHECK_STR(buffer, "51C");
    // Negative temperatures are valid inside the protocol range.
    dashboard_format_temperature(buffer, sizeof(buffer), true, -5);
    CHECK_STR(buffer, "-5C");
    dashboard_format_temperature(buffer, sizeof(buffer), true, -40);
    CHECK_STR(buffer, "-40C");
}

static void test_text_and_truncation(void)
{
    char buffer[8];
    dashboard_format_text(buffer, sizeof(buffer), false, "eno1");
    CHECK_STR(buffer, "--");
    dashboard_format_text(buffer, sizeof(buffer), true, "eno1");
    CHECK_STR(buffer, "eno1");
    dashboard_format_text(buffer, sizeof(buffer), true, NULL);
    CHECK_STR(buffer, "--");

    // A value longer than the buffer must be truncated, never overflowed.
    dashboard_format_text(buffer, sizeof(buffer), true, "abcdefghijklmnop");
    CHECK_STR(buffer, "abcdefg");
    CHECK(buffer[7] == '\0');

    // Capacity 0 and empty buffers must not crash or write.
    dashboard_format_text(NULL, 8, true, "x");
    dashboard_format_age(NULL, 0, 5, false);
    char tiny[1] = { 'X' };
    dashboard_format_text(tiny, sizeof(tiny), true, "abc");
    CHECK(tiny[0] == '\0');
}

int main(void)
{
    test_age();
    test_uptime();
    test_load();
    test_percent_and_temperature();
    test_text_and_truncation();

    if (s_failures != 0) {
        fprintf(stderr, "dashboard format tests: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("dashboard format tests: PASS\n");
    return 0;
}
