// tests/test_dashboard_protocol.c
// Host test for the firmware-side status decoder. Compiles with a plain C11
// toolchain (no ESP-IDF, no LVGL) and replays the shared conformance cases in
// protocol/fixtures/status_cases.txt, which the Python producer test also reads.
#include "app_dashboard/dashboard_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            s_failures++;                                                       \
        }                                                                       \
    } while (0)

static void check_parse_ok(const char *payload)
{
    dashboard_status_t status;
    const dashboard_parse_t result = dashboard_status_parse(payload, strlen(payload), &status);
    if (result != DASHBOARD_PARSE_OK) {
        fprintf(stderr, "FAIL accepted-case rejected (%d): %s\n", (int)result, payload);
        s_failures++;
        return;
    }
    // Every accepted frame must record the two required fields.
    CHECK(dashboard_status_has(&status, DASHBOARD_FIELD_VERSION));
    CHECK(dashboard_status_has(&status, DASHBOARD_FIELD_TIME));
}

static void check_parse_err(const char *reason, const char *payload)
{
    dashboard_status_t status;
    const dashboard_parse_t result = dashboard_status_parse(payload, strlen(payload), &status);
    if (result == DASHBOARD_PARSE_OK) {
        fprintf(stderr, "FAIL rejected-case accepted [%s]: %s\n", reason, payload);
        s_failures++;
        return;
    }
    // A rejected frame must not leak values into the caller's view.
    CHECK(status.present == 0);
    CHECK(!dashboard_status_has(&status, DASHBOARD_FIELD_TIME));
}

static int run_fixture_file(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "FAIL cannot open fixture file: %s\n", path);
        s_failures++;
        return 1;
    }

    char line[512];
    int cases = 0;
    while (fgets(line, (int)sizeof(line), file)) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0 || line[0] == '#') continue;

        if (strncmp(line, "ok|", 3) == 0) {
            check_parse_ok(line + 3);
            cases++;
        } else if (strncmp(line, "err|", 4) == 0) {
            const char *reason = line + 4;
            const char *separator = strchr(reason, '|');
            if (separator) {
                char reason_buffer[64];
                const size_t reason_length = (size_t)(separator - reason);
                if (reason_length < sizeof(reason_buffer)) {
                    memcpy(reason_buffer, reason, reason_length);
                    reason_buffer[reason_length] = '\0';
                    check_parse_err(reason_buffer, separator + 1);
                    cases++;
                    continue;
                }
            }
            fprintf(stderr, "FAIL malformed fixture line: %s\n", line);
            s_failures++;
        } else {
            fprintf(stderr, "FAIL unknown fixture directive: %s\n", line);
            s_failures++;
        }
    }
    fclose(file);

    if (cases < 30) {
        fprintf(stderr, "FAIL fixture file looks truncated: %d cases\n", cases);
        s_failures++;
    }
    printf("dashboard protocol fixtures: %d cases\n", cases);
    return 0;
}

static void test_full_frame_values(void)
{
    const char *payload =
        "v=1;t=1782530000;h=dev-forge;up=128099;ld=0.42;cpu=7;mem=38;dsk=33;tmp=51;"
        "net=eno1,up,10.2.65.61;ag=2;as=5";
    dashboard_status_t status;
    CHECK(dashboard_status_parse(payload, strlen(payload), &status) == DASHBOARD_PARSE_OK);

    CHECK(status.version == 1);
    CHECK(status.heartbeat == 1782530000u);
    CHECK(strcmp(status.host, "dev-forge") == 0);
    CHECK(status.uptime_s == 128099u);
    CHECK(status.load_x100 == 42);
    CHECK(status.cpu_percent == 7);
    CHECK(status.mem_percent == 38);
    CHECK(status.disk_percent == 33);
    CHECK(status.temp_c == 51);
    CHECK(strcmp(status.net_iface, "eno1") == 0);
    CHECK(strcmp(status.net_ip, "10.2.65.61") == 0);
    CHECK(status.net_up);
    CHECK(status.agents_active == 2);
    CHECK(status.agent_idle_s == 5);

    // All twelve fields present.
    uint16_t expected = 0;
    for (int field = 0; field < DASHBOARD_FIELD_COUNT; field++) {
        expected = (uint16_t)(expected | DASHBOARD_FIELD_BIT(field));
    }
    CHECK(status.present == expected);
}

static void test_missing_optional_fields_are_absent_not_zero(void)
{
    dashboard_status_t status;
    CHECK(dashboard_status_parse("v=1;t=7", 7, &status) == DASHBOARD_PARSE_OK);
    CHECK(dashboard_status_has(&status, DASHBOARD_FIELD_TIME));
    CHECK(!dashboard_status_has(&status, DASHBOARD_FIELD_CPU));
    CHECK(!dashboard_status_has(&status, DASHBOARD_FIELD_NET));
    CHECK(!dashboard_status_has(&status, DASHBOARD_FIELD_AGENTS));
    // -1 is the documented "never seen" sentinel, not 0 seconds.
    CHECK(status.agent_idle_s == -1);
}

static void test_length_boundaries(void)
{
    dashboard_status_t status;
    CHECK(dashboard_status_parse("", 0, &status) == DASHBOARD_PARSE_EMPTY);
    CHECK(dashboard_status_parse(NULL, 8, &status) == DASHBOARD_PARSE_EMPTY);

    // 200 bytes is accepted, 201 is not.
    char payload[DASHBOARD_STATUS_MAX_BYTES + 2];
    memset(payload, 'a', sizeof(payload));
    memcpy(payload, "v=1;t=5;h=", 10);
    payload[DASHBOARD_STATUS_MAX_BYTES] = '\0';
    CHECK(dashboard_status_parse(payload, DASHBOARD_STATUS_MAX_BYTES, &status)
          != DASHBOARD_PARSE_TOO_LONG);
    CHECK(dashboard_status_parse(payload, DASHBOARD_STATUS_MAX_BYTES + 1, &status)
          == DASHBOARD_PARSE_TOO_LONG);
}

static void test_embedded_nul_and_newline(void)
{
    dashboard_status_t status;
    // An embedded NUL means the producer sent something that is not plain ASCII
    // text; reject the frame instead of silently truncating a value.
    const char payload[] = "v=1;t=5;h=ab\0cd";
    CHECK(dashboard_status_parse(payload, sizeof(payload) - 1, &status)
          == DASHBOARD_PARSE_BAD_VALUE);
    CHECK(status.present == 0);

    const char newline[] = "v=1;t=5\n";
    CHECK(dashboard_status_parse(newline, sizeof(newline) - 1, &status)
          == DASHBOARD_PARSE_SYNTAX);
}

static void test_long_values_are_rejected_not_truncated(void)
{
    dashboard_status_t status;
    // Host name longer than DASHBOARD_HOST_MAX must be rejected, not sliced.
    const char payload[] = "v=1;t=5;h=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    CHECK(dashboard_status_parse(payload, strlen(payload), &status)
          == DASHBOARD_PARSE_BAD_VALUE);
    CHECK(status.present == 0);
}

static void test_repeat_parse_resets_previous_values(void)
{
    dashboard_status_t status;
    const char first[] = "v=1;t=5;cpu=90;net=eno1,up,1.2.3.4";
    CHECK(dashboard_status_parse(first, strlen(first), &status) == DASHBOARD_PARSE_OK);
    CHECK(status.cpu_percent == 90);
    // A failed parse must clear the struct so stale values cannot look current.
    CHECK(dashboard_status_parse("garbage", 7, &status) == DASHBOARD_PARSE_SYNTAX);
    CHECK(status.present == 0);
    CHECK(status.cpu_percent == 0);
    CHECK(status.net_iface[0] == '\0');
}

static void test_heartbeat_age(void)
{
    dashboard_age_t age = dashboard_heartbeat_age(10000u, 0u, false);
    CHECK(age.never_received);
    CHECK(age.age_s == 0);

    age = dashboard_heartbeat_age(10000u, 5000u, true);
    CHECK(!age.never_received && !age.clock_went_back);
    CHECK(age.age_s == 5);

    age = dashboard_heartbeat_age(5000u, 10000u, true);
    CHECK(age.clock_went_back);
    CHECK(age.age_s == 0);

    // Sub-second remainders floor; 59999 ms is 59 s, not 60.
    CHECK(dashboard_heartbeat_age(59999u, 0u, true).age_s == 59);
    // 32-bit day counter does not wrap the conversion.
    CHECK(dashboard_heartbeat_age(0xFFFFFFFFull, 0ull, true).age_s == 4294967u);
}

static void test_link_state_thresholds(void)
{
    CHECK(dashboard_link_state(false, 0) == DASHBOARD_LINK_NONE);
    CHECK(dashboard_link_state(true, 0) == DASHBOARD_LINK_LIVE);
    CHECK(dashboard_link_state(true, DASHBOARD_LINK_LIVE_MAX_S) == DASHBOARD_LINK_LIVE);
    CHECK(dashboard_link_state(true, DASHBOARD_LINK_LIVE_MAX_S + 1) == DASHBOARD_LINK_STALE);
    CHECK(dashboard_link_state(true, DASHBOARD_LINK_STALE_MAX_S) == DASHBOARD_LINK_STALE);
    CHECK(dashboard_link_state(true, DASHBOARD_LINK_STALE_MAX_S + 1) == DASHBOARD_LINK_LOST);
}

static void test_status_helpers_tolerate_bad_arguments(void)
{
    CHECK(dashboard_status_parse("v=1;t=5", 7, NULL) == DASHBOARD_PARSE_EMPTY);
    CHECK(!dashboard_status_has(NULL, DASHBOARD_FIELD_TIME));
    CHECK(!dashboard_status_has(NULL, DASHBOARD_FIELD_COUNT));

    dashboard_status_t status;
    const char payload[] = "v=1;t=5;cpu=1;as=-1";
    CHECK(dashboard_status_parse(payload, strlen(payload), &status) == DASHBOARD_PARSE_OK);
    CHECK(dashboard_status_has(&status, DASHBOARD_FIELD_AGENT_IDLE));
    CHECK(status.agent_idle_s == -1);
    CHECK(!dashboard_status_has(&status, DASHBOARD_FIELD_AGENTS));
    dashboard_status_reset(&status);
    CHECK(status.present == 0);
    CHECK(status.agent_idle_s == -1);
}

int main(int argc, char **argv)
{
    const char *fixture = argc > 1 ? argv[1] : "protocol/fixtures/status_cases.txt";

    run_fixture_file(fixture);
    test_full_frame_values();
    test_missing_optional_fields_are_absent_not_zero();
    test_length_boundaries();
    test_embedded_nul_and_newline();
    test_long_values_are_rejected_not_truncated();
    test_repeat_parse_resets_previous_values();
    test_heartbeat_age();
    test_link_state_thresholds();
    test_status_helpers_tolerate_bad_arguments();

    if (s_failures != 0) {
        fprintf(stderr, "dashboard protocol tests: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("dashboard protocol tests: PASS\n");
    return 0;
}
