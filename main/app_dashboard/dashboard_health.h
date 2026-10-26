// main/app_dashboard/dashboard_health.h
// Turns a status snapshot into one verdict: which condition is worst, how
// severe it is, and whether it has persisted long enough to deserve attention.
//
// No ESP-IDF and no LVGL dependency, so the dwell, debounce and escalation rules
// are covered by host tests (tests/test_dashboard_health.c) instead of only being
// observable on a desk.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_dashboard/dashboard_protocol.h"
#include "app_dashboard/dashboard_state.h"

// What is wrong. The UI maps this 1:1 to the status card text.
typedef enum {
    DASHBOARD_ALERT_NONE = 0,  // everything within thresholds
    DASHBOARD_ALERT_NO_DATA,   // nothing received since boot
    DASHBOARD_ALERT_NO_LINK,   // BLE link down (ambiguous: badge, pusher or host)
    DASHBOARD_ALERT_SILENT,    // link up, frames stopped (host is the likely cause)
    DASHBOARD_ALERT_DISK,
    DASHBOARD_ALERT_MEM,
    DASHBOARD_ALERT_LOAD,
    DASHBOARD_ALERT_TEMP,
    DASHBOARD_ALERT_NET,
    DASHBOARD_ALERT_BATTERY,  // the badge's own battery, not the host's
    DASHBOARD_ALERT_COUNT,
} dashboard_alert_t;

typedef enum {
    DASHBOARD_SEVERITY_OK = 0,
    DASHBOARD_SEVERITY_WARN,
    DASHBOARD_SEVERITY_CRIT,
} dashboard_severity_t;

typedef struct {
    // Heartbeat age thresholds, in seconds since the last accepted frame.
    uint32_t heartbeat_warn_s;
    uint32_t heartbeat_crit_s;
    // BLE link-down escalation: the durations are the dwell, so a normal
    // reconnect blip never reaches WARN.
    uint32_t link_warn_s;
    uint32_t link_crit_s;
    uint8_t disk_warn_pct;
    uint8_t disk_crit_pct;
    uint8_t mem_warn_pct;
    uint8_t mem_crit_pct;
    // Load expressed as percent of the core count: 100 means load == cores.
    uint16_t load_warn_pct;
    uint16_t load_crit_pct;
    int16_t temp_warn_c;
    int16_t temp_crit_c;
    // Badge battery. A panel that dies silently is worse than one that nags.
    uint8_t battery_warn_pct;
    uint8_t battery_crit_pct;
    // Leave the CRIT band only after climbing this far above the threshold, because
    // a battery voltage reading rebounds under load.
    uint8_t battery_recover_margin_pct;
    uint32_t battery_crit_dwell_ms;
    // Rising charge means USB power is attached; stay quiet for this long.
    uint32_t battery_charge_suppress_ms;

    // A threshold breach must persist this long before it becomes CRIT.
    uint32_t crit_dwell_ms;
    // ... and must clear for this long before the severity drops back.
    uint32_t recover_ms;
} dashboard_health_config_t;

typedef struct {
    dashboard_severity_t severity;  // current confirmed severity
    uint64_t crit_since_ms;         // first moment CRIT was a candidate
    bool crit_pending;
    uint64_t below_since_ms;        // first moment it dropped below `severity`
    bool below_pending;
    // Link only: how long the BLE connection has been down. The link is duration
    // based, so its own timer is the dwell.
    bool have_down;
    uint64_t down_since_ms;
} dashboard_item_state_t;

typedef struct {
    dashboard_health_config_t config;
    // Sized from the enum, so adding an alert kind can never silently overflow it.
    dashboard_item_state_t items[DASHBOARD_ALERT_COUNT];
    bool initialised;

    // Acknowledged alert: suppresses the pulse, never the colour.
    dashboard_alert_t acknowledged;
    // Last kind reported, used to clear the acknowledgement when things change.
    dashboard_alert_t last_reported;

    // Badge battery, fed by dashboard_health_set_battery().
    bool battery_known;
    int last_battery_soc;
    uint64_t charge_suppress_until_ms;

    // Reboot detection: `up` going backwards means the host restarted.
    bool have_uptime;
    uint32_t last_uptime_s;
    bool have_reboot;
    uint64_t reboot_ms;
} dashboard_health_t;

typedef struct {
    dashboard_alert_t alert;
    dashboard_severity_t severity;
    bool acknowledged;  // true when the user pressed OK for this alert
    bool requires_action;  // CRIT and not acknowledged: pulse, repeat reminders
} dashboard_verdict_t;

dashboard_health_config_t dashboard_health_default_config(void);

void dashboard_health_init(dashboard_health_t *health, const dashboard_health_config_t *config);

// Evaluate one update. `now_ms` is the badge monotonic clock; `snapshot` may be
// re-evaluated as often as the caller likes (the UI calls it every refresh).
dashboard_verdict_t dashboard_health_update(dashboard_health_t *health,
                                            const dashboard_snapshot_t *snapshot,
                                            uint64_t now_ms);

// Feed the badge's own battery reading. -1 means "unknown" and disables the alert.
// A rising reading is treated as charging and suppresses the alert for a while.
void dashboard_health_set_battery(dashboard_health_t *health, int percent, uint64_t now_ms);

// Silence the pulse for the current alert. The colour stays until it clears.
void dashboard_health_acknowledge(dashboard_health_t *health);

// How long the BLE link itself has been down, or 0 while connected. The dashboard
// shows this for NO LINK: "DOWN FOR" must not quote the frame age, which starts
// earlier (the pusher can stop before the link drops).
uint32_t dashboard_health_link_down_s(const dashboard_health_t *health, uint64_t now_ms);

// How long ago a host reboot was detected. Returns false when no reboot has been
// seen; the age is written to `out_s` only when it returns true, so "just now" and
// "never" cannot be confused.
bool dashboard_health_reboot_age_s(const dashboard_health_t *health, uint64_t now_ms,
                                   uint32_t *out_s);
