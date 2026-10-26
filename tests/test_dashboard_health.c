// tests/test_dashboard_health.c
// Host test for the verdict rules: thresholds, CRIT dwell, recovery debounce,
// link escalation, acknowledgement and reboot detection.
#include "app_dashboard/dashboard_health.h"

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

#define CHECK_EQ(actual, expected)                                               \
    do {                                                                         \
        const long a_ = (long)(actual);                                          \
        const long e_ = (long)(expected);                                        \
        if (a_ != e_) {                                                          \
            fprintf(stderr, "FAIL %s:%d: expected %ld, got %ld\n",                \
                    __FILE__, __LINE__, e_, a_);                                  \
            s_failures++;                                                        \
        }                                                                        \
    } while (0)

#define SECONDS 1000u

// A healthy frame with every field present, so tests only vary what they mean to.
static dashboard_snapshot_t healthy(uint32_t uptime_s)
{
    dashboard_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    dashboard_status_reset(&snapshot.status);
    snapshot.status.present = 0xFFFF & ~0;  // all bits, adjusted below
    snapshot.status.present = 0;
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_VERSION);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_TIME);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_HOST);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_CORES);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_UPTIME);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_LOAD);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_CPU);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_MEM);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_DISK);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_TEMP);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_NET);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_AGENTS);
    snapshot.status.present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_AGENT_IDLE);
    snapshot.status.version = 1;
    strcpy(snapshot.status.host, "Ubuntu-Box-Dev");
    snapshot.status.cores = 4;
    snapshot.status.uptime_s = uptime_s;
    snapshot.status.load_x100 = 42;
    snapshot.status.cpu_percent = 7;
    snapshot.status.mem_percent = 38;
    snapshot.status.disk_percent = 33;
    snapshot.status.temp_c = 51;
    snapshot.status.net_up = true;
    strcpy(snapshot.status.net_iface, "eno1");
    strcpy(snapshot.status.net_ip, "10.2.65.192");
    snapshot.status.agents_active = 1;
    snapshot.status.agent_idle_s = 3;
    snapshot.have_rx = true;
    snapshot.connected = true;
    snapshot.accepted_frames = 1;
    snapshot.last_rx_ms = 0;
    return snapshot;
}

static void test_healthy_is_quiet(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    const dashboard_verdict_t verdict = dashboard_health_update(&health, &snapshot, 1000);
    CHECK_EQ(verdict.alert, DASHBOARD_ALERT_NONE);
    CHECK_EQ(verdict.severity, DASHBOARD_SEVERITY_OK);
    CHECK(!verdict.requires_action);
}

static void test_no_data_is_grey_not_alarming(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(0);
    snapshot.have_rx = false;
    snapshot.connected = false;
    const dashboard_verdict_t verdict = dashboard_health_update(&health, &snapshot, 0);
    CHECK_EQ(verdict.alert, DASHBOARD_ALERT_NO_DATA);
    CHECK_EQ(verdict.severity, DASHBOARD_SEVERITY_OK);
}

static void test_disk_warn_immediate_crit_after_dwell(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;

    snapshot.status.disk_percent = 85;  // WARN band
    snapshot.last_rx_ms = 0;
    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_DISK);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);

    // CRIT band. The dwell starts at the first sample that is in the CRIT band.
    snapshot.status.disk_percent = 95;
    snapshot.last_rx_ms = 1000;
    v = dashboard_health_update(&health, &snapshot, 1000);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);
    snapshot.last_rx_ms = 10999;
    v = dashboard_health_update(&health, &snapshot, 10999);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);
    snapshot.last_rx_ms = 11000;
    v = dashboard_health_update(&health, &snapshot, 11000);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_DISK);
    CHECK(v.requires_action);

    // Recovery is debounced: still CRIT until the value has been low for 5 s.
    snapshot.status.disk_percent = 50;
    snapshot.last_rx_ms = 14000;
    v = dashboard_health_update(&health, &snapshot, 14000);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
    snapshot.last_rx_ms = 18999;
    v = dashboard_health_update(&health, &snapshot, 18999);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
    snapshot.last_rx_ms = 19000;
    v = dashboard_health_update(&health, &snapshot, 19000);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_OK);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);
}

static void test_heartbeat_warn_then_silent(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;

    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 5 * SECONDS);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);

    // 20 s old: missed pushes, amber.
    v = dashboard_health_update(&health, &snapshot, 20 * SECONDS);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_SILENT);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);

    // 61 s old: the host is no longer answering.
    v = dashboard_health_update(&health, &snapshot, 61 * SECONDS);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_SILENT);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
    CHECK(v.requires_action);
}

static void test_link_escalation_ladder(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;

    // Establish a healthy baseline first.
    dashboard_health_update(&health, &snapshot, 0);

    snapshot.connected = false;
    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NO_LINK);
    // 0-20 s: grey, so a reconnect blip does not cry wolf.
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_OK);

    v = dashboard_health_update(&health, &snapshot, 19 * SECONDS);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_OK);

    v = dashboard_health_update(&health, &snapshot, 20 * SECONDS);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);

    v = dashboard_health_update(&health, &snapshot, 119 * SECONDS);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);

    // Beyond two minutes it is no longer a blip: red.
    v = dashboard_health_update(&health, &snapshot, 120 * SECONDS);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
    CHECK(v.requires_action);

    // Reconnect clears it immediately.
    snapshot.connected = true;
    snapshot.last_rx_ms = 120 * SECONDS;
    v = dashboard_health_update(&health, &snapshot, 120 * SECONDS);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);
}

static void test_load_is_judged_against_cores(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;

    // 4.00 on 4 cores is exactly 100 percent: still OK (threshold is > 100).
    snapshot.status.load_x100 = 400;
    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);

    // 4.20 on 4 cores is 105 percent: WARN at once.
    snapshot.status.load_x100 = 420;
    v = dashboard_health_update(&health, &snapshot, 1000);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_LOAD);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);

    // The same load on 16 cores is only 26 percent: no alert at all.
    dashboard_health_t wide;
    dashboard_health_init(&wide, NULL);
    dashboard_snapshot_t wide_snapshot = healthy(1000);
    wide_snapshot.last_rx_ms = 0;
    wide_snapshot.status.cores = 16;
    wide_snapshot.status.load_x100 = 420;
    v = dashboard_health_update(&wide, &wide_snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);

    // Without a core count the load cannot be judged, so it must stay silent
    // rather than guess.
    dashboard_health_t blind;
    dashboard_health_init(&blind, NULL);
    dashboard_snapshot_t blind_snapshot = healthy(1000);
    blind_snapshot.last_rx_ms = 0;
    blind_snapshot.status.load_x100 = 3000;
    blind_snapshot.status.present &= (uint16_t)~DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_CORES);
    v = dashboard_health_update(&blind, &blind_snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);
}

static void test_network_down_is_crit_after_dwell(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;
    snapshot.status.net_up = false;

    snapshot.last_rx_ms = 0;
    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NET);
    // A carrier flap should not turn the screen red instantly.
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);
    snapshot.last_rx_ms = 10000;
    v = dashboard_health_update(&health, &snapshot, 10000);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
}

static void test_missing_field_never_alerts(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;
    snapshot.status.present &= (uint16_t)~DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_DISK);
    snapshot.status.disk_percent = 99;  // value present but field marked absent
    const dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);
}

static void test_silent_outranks_a_metric_breach(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;
    snapshot.status.disk_percent = 95;

    // Hold the disk breach long enough to become CRIT, heartbeat still arriving.
    dashboard_health_update(&health, &snapshot, 0);
    snapshot.last_rx_ms = 10000;
    dashboard_health_update(&health, &snapshot, 10000);

    // Now the host stops answering entirely: the stale disk number is history.
    const dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 200000);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_SILENT);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
}

static void test_acknowledge_silences_but_keeps_severity(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;
    snapshot.status.disk_percent = 95;
    dashboard_health_update(&health, &snapshot, 0);
    snapshot.last_rx_ms = 10000;
    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 10000);
    CHECK(v.requires_action);

    dashboard_health_acknowledge(&health);
    snapshot.last_rx_ms = 11000;
    v = dashboard_health_update(&health, &snapshot, 11000);
    CHECK(v.acknowledged);
    CHECK(!v.requires_action);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);  // colour stays

    // A different problem re-arms the pulse.
    snapshot.status.mem_percent = 95;
    snapshot.status.disk_percent = 10;
    snapshot.last_rx_ms = 20000;
    dashboard_health_update(&health, &snapshot, 20000);
    snapshot.last_rx_ms = 31000;
    v = dashboard_health_update(&health, &snapshot, 31000);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_MEM);
    CHECK(v.requires_action);
}

static void test_reboot_detected_from_uptime_regression(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(100000);
    snapshot.last_rx_ms = 0;
    uint32_t age_s = 999;
    dashboard_health_update(&health, &snapshot, 0);
    // No reboot yet: false, and the out parameter is cleared.
    CHECK(!dashboard_health_reboot_age_s(&health, 0, &age_s));
    CHECK_EQ(age_s, 0);

    // Uptime jumping backwards means the host restarted.
    snapshot.status.uptime_s = 5;
    dashboard_health_update(&health, &snapshot, 5000);
    // Detected at t=5 s: age is 0 there, 90 s later it is 90.
    CHECK(dashboard_health_reboot_age_s(&health, 5000, &age_s));
    CHECK_EQ(age_s, 0);
    CHECK(dashboard_health_reboot_age_s(&health, 95000, &age_s));
    CHECK_EQ(age_s, 90);
}

static void test_battery_warn_then_crit_after_dwell(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;

    // 15 %: low, but not yet dying.
    dashboard_health_set_battery(&health, 15, 0);
    snapshot.last_rx_ms = 0;
    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_BATTERY);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);

    // 8 %: CRIT band, but only after the 30 s battery dwell.
    dashboard_health_set_battery(&health, 8, 0);
    snapshot.last_rx_ms = 0;
    dashboard_health_update(&health, &snapshot, 0);   // dwell starts here
    snapshot.last_rx_ms = 29999;
    v = dashboard_health_update(&health, &snapshot, 29999);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_WARN);
    snapshot.last_rx_ms = 30000;
    v = dashboard_health_update(&health, &snapshot, 30000);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_BATTERY);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
    CHECK(v.requires_action);
}

static void test_battery_crit_outranks_a_silent_host(void)
{
    // Decision: a dying badge outranks host problems, because the panel is about to
    // go dark and plugging in is a two-second fix.
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;
    dashboard_health_set_battery(&health, 5, 0);
    dashboard_health_update(&health, &snapshot, 0);

    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 30000);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_BATTERY);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
}

static void test_battery_warn_does_not_outrank_a_crashing_host(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;
    dashboard_health_set_battery(&health, 15, 0);
    dashboard_health_update(&health, &snapshot, 0);

    // Host goes silent; that is the more severe finding.
    const dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 61000);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_SILENT);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);
}

static void test_battery_hysteresis_and_charging(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;
    dashboard_health_set_battery(&health, 8, 0);
    dashboard_health_update(&health, &snapshot, 0);
    dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 30000);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);

    // A flat reading keeps CRIT: the recovery debounce alone must not clear it.
    dashboard_health_set_battery(&health, 8, 40000);
    snapshot.last_rx_ms = 40000;
    v = dashboard_health_update(&health, &snapshot, 50000);
    CHECK_EQ(v.severity, DASHBOARD_SEVERITY_CRIT);

    // A rising reading means USB power: stay quiet while it refills.
    dashboard_health_set_battery(&health, 14, 60000);
    dashboard_health_set_battery(&health, 18, 70000);
    dashboard_health_set_battery(&health, 15, 80000);  // even if it dips again
    snapshot.last_rx_ms = 80000;
    v = dashboard_health_update(&health, &snapshot, 80000);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);
}

static void test_unknown_battery_disables_the_alert(void)
{
    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    dashboard_snapshot_t snapshot = healthy(1000);
    snapshot.last_rx_ms = 0;
    dashboard_health_set_battery(&health, -1, 0);
    const dashboard_verdict_t v = dashboard_health_update(&health, &snapshot, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NONE);
}

static void test_null_arguments_are_safe(void)
{
    dashboard_health_init(NULL, NULL);
    dashboard_verdict_t v = dashboard_health_update(NULL, NULL, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NO_DATA);
    dashboard_health_acknowledge(NULL);
    CHECK(!dashboard_health_reboot_age_s(NULL, 0, NULL));

    dashboard_health_t health;
    dashboard_health_init(&health, NULL);
    v = dashboard_health_update(&health, NULL, 0);
    CHECK_EQ(v.alert, DASHBOARD_ALERT_NO_DATA);
}

int main(void)
{
    test_healthy_is_quiet();
    test_no_data_is_grey_not_alarming();
    test_disk_warn_immediate_crit_after_dwell();
    test_heartbeat_warn_then_silent();
    test_link_escalation_ladder();
    test_load_is_judged_against_cores();
    test_network_down_is_crit_after_dwell();
    test_missing_field_never_alerts();
    test_silent_outranks_a_metric_breach();
    test_acknowledge_silences_but_keeps_severity();
    test_reboot_detected_from_uptime_regression();
    test_battery_warn_then_crit_after_dwell();
    test_battery_crit_outranks_a_silent_host();
    test_battery_warn_does_not_outrank_a_crashing_host();
    test_battery_hysteresis_and_charging();
    test_unknown_battery_disables_the_alert();
    test_null_arguments_are_safe();

    if (s_failures != 0) {
        fprintf(stderr, "dashboard health tests: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("dashboard health tests: PASS\n");
    return 0;
}
