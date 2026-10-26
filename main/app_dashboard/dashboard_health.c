// main/app_dashboard/dashboard_health.c
// Verdict evaluation with dwell, recovery debounce and acknowledgement.
//
// Rules implemented here, matching docs/development/dashboard-node-monitor.md:
//   - WARN applies at once; CRIT must persist for `crit_dwell_ms` first.
//   - A severity drops back only after `recover_ms` below the threshold.
//   - Heartbeat and link are duration based: their thresholds already are the
//     dwell, so no extra delay is layered on top.
//   - The host name, agent counts and similar informational fields never raise
//     an alert, and a missing field is "no data", never a zero-scoring breach.
#include "app_dashboard/dashboard_health.h"

#include <string.h>

dashboard_health_config_t dashboard_health_default_config(void)
{
    dashboard_health_config_t config;
    memset(&config, 0, sizeof(config));
    config.heartbeat_warn_s = 15;
    config.heartbeat_crit_s = 60;
    config.link_warn_s = 20;
    config.link_crit_s = 120;
    config.disk_warn_pct = 80;
    config.disk_crit_pct = 90;
    config.mem_warn_pct = 80;
    config.mem_crit_pct = 90;
    config.load_warn_pct = 100;
    config.load_crit_pct = 200;
    config.temp_warn_c = 75;
    config.temp_crit_c = 85;
    config.battery_warn_pct = 20;
    config.battery_crit_pct = 10;
    config.battery_recover_margin_pct = 5;
    config.battery_crit_dwell_ms = 30000;
    config.battery_charge_suppress_ms = 120000;
    config.crit_dwell_ms = 10000;
    config.recover_ms = 5000;
    return config;
}

void dashboard_health_init(dashboard_health_t *health, const dashboard_health_config_t *config)
{
    if (!health) return;
    memset(health, 0, sizeof(*health));
    health->config = config ? *config : dashboard_health_default_config();
    health->acknowledged = DASHBOARD_ALERT_NONE;
    health->last_reported = DASHBOARD_ALERT_NONE;
    health->initialised = true;
}

static dashboard_severity_t classify_percent(uint16_t value, uint8_t warn, uint8_t crit)
{
    if (value >= crit) return DASHBOARD_SEVERITY_CRIT;
    if (value >= warn) return DASHBOARD_SEVERITY_WARN;
    return DASHBOARD_SEVERITY_OK;
}

// Apply the dwell / debounce rules for one threshold-based item.
static void apply_timing(dashboard_item_state_t *state, dashboard_severity_t target,
                         const dashboard_health_config_t *config, uint64_t now_ms,
                         uint32_t crit_dwell_ms)
{
    // Recovery: hold the higher severity until the condition has been clear for a
    // while, so a value hovering on a threshold cannot flap the screen.
    if (target < state->severity) {
        if (!state->below_pending) {
            state->below_pending = true;
            state->below_since_ms = now_ms;
        }
        if (now_ms - state->below_since_ms >= config->recover_ms) {
            state->severity = target;
            state->below_pending = false;
        }
        state->crit_pending = false;
        return;
    }
    state->below_pending = false;

    // Escalation: WARN immediately, CRIT only after the dwell.
    if (target == DASHBOARD_SEVERITY_OK) {
        state->severity = DASHBOARD_SEVERITY_OK;
        state->crit_pending = false;
        return;
    }
    if (target == DASHBOARD_SEVERITY_WARN) {
        if (state->severity == DASHBOARD_SEVERITY_OK) state->severity = DASHBOARD_SEVERITY_WARN;
        state->crit_pending = false;
        return;
    }
    // target == CRIT
    if (state->severity == DASHBOARD_SEVERITY_CRIT) {
        state->crit_pending = false;
        return;
    }
    if (!state->crit_pending) {
        state->crit_pending = true;
        state->crit_since_ms = now_ms;
    }
    if (now_ms - state->crit_since_ms >= crit_dwell_ms) {
        state->severity = DASHBOARD_SEVERITY_CRIT;
        state->crit_pending = false;
    } else if (state->severity == DASHBOARD_SEVERITY_OK) {
        // During the dwell the condition is at least worth a WARN.
        state->severity = DASHBOARD_SEVERITY_WARN;
    }
}

// Link and heartbeat are duration based, so they map straight to a severity.
static dashboard_severity_t classify_duration(uint32_t seconds, uint32_t warn, uint32_t crit)
{
    if (seconds >= crit) return DASHBOARD_SEVERITY_CRIT;
    if (seconds >= warn) return DASHBOARD_SEVERITY_WARN;
    return DASHBOARD_SEVERITY_OK;
}

static void detect_reboot(dashboard_health_t *health, const dashboard_snapshot_t *snapshot,
                          uint64_t now_ms)
{
    if (!dashboard_status_has(&snapshot->status, DASHBOARD_FIELD_UPTIME)) return;
    const uint32_t uptime = snapshot->status.uptime_s;
    if (health->have_uptime && uptime + 2u < health->last_uptime_s) {
        health->have_reboot = true;
        health->reboot_ms = now_ms;
    }
    health->have_uptime = true;
    health->last_uptime_s = uptime;
}

dashboard_verdict_t dashboard_health_update(dashboard_health_t *health,
                                            const dashboard_snapshot_t *snapshot,
                                            uint64_t now_ms)
{
    dashboard_verdict_t verdict;
    memset(&verdict, 0, sizeof(verdict));
    verdict.alert = DASHBOARD_ALERT_NO_DATA;
    verdict.severity = DASHBOARD_SEVERITY_OK;
    if (!health || !snapshot || !health->initialised) return verdict;

    const dashboard_health_config_t *config = &health->config;
    const dashboard_status_t *status = &snapshot->status;
    const dashboard_age_t age =
        dashboard_heartbeat_age(now_ms, snapshot->last_rx_ms, snapshot->have_rx);

    detect_reboot(health, snapshot, now_ms);

    // --- link: how long has the BLE connection been down? -------------------
    // Without a connection there are no frames, so the link tracks its own timer.
    if (!snapshot->connected) {
        if (!health->items[DASHBOARD_ALERT_NO_LINK].have_down) {
            health->items[DASHBOARD_ALERT_NO_LINK].have_down = true;
            health->items[DASHBOARD_ALERT_NO_LINK].down_since_ms = now_ms;
        }
    } else {
        health->items[DASHBOARD_ALERT_NO_LINK].have_down = false;
        health->items[DASHBOARD_ALERT_NO_LINK].severity = DASHBOARD_SEVERITY_OK;
    }

    // --- pick the worst condition ------------------------------------------
    dashboard_alert_t worst = DASHBOARD_ALERT_NONE;
    dashboard_severity_t worst_severity = DASHBOARD_SEVERITY_OK;

    const bool have_frames = snapshot->have_rx;

    if (!have_frames) {
        worst = DASHBOARD_ALERT_NO_DATA;
        worst_severity = DASHBOARD_SEVERITY_OK;  // grey, not alarming
    }

    if (!snapshot->connected) {
        const uint32_t down_s =
            (uint32_t)((now_ms - health->items[DASHBOARD_ALERT_NO_LINK].down_since_ms) / 1000u);
        const dashboard_severity_t severity =
            classify_duration(down_s, config->link_warn_s, config->link_crit_s);
        health->items[DASHBOARD_ALERT_NO_LINK].severity = severity;
        if (severity >= worst_severity) {
            worst = DASHBOARD_ALERT_NO_LINK;
            worst_severity = severity;
        }
    } else if (have_frames) {
        const dashboard_severity_t severity = classify_duration(
            age.age_s, config->heartbeat_warn_s, config->heartbeat_crit_s);
        health->items[DASHBOARD_ALERT_SILENT].severity = severity;
        if (severity > DASHBOARD_SEVERITY_OK && severity >= worst_severity) {
            worst = DASHBOARD_ALERT_SILENT;
            worst_severity = severity;
        }
    }

    // Threshold items only speak when their field actually arrived.
    struct {
        dashboard_alert_t alert;
        dashboard_field_t field;
        uint16_t value;
        uint8_t warn;
        uint8_t crit;
    } percent_items[] = {
        { DASHBOARD_ALERT_DISK, DASHBOARD_FIELD_DISK, status->disk_percent,
          config->disk_warn_pct, config->disk_crit_pct },
        { DASHBOARD_ALERT_MEM, DASHBOARD_FIELD_MEM, status->mem_percent,
          config->mem_warn_pct, config->mem_crit_pct },
    };
    for (size_t i = 0; i < sizeof(percent_items) / sizeof(percent_items[0]); i++) {
        dashboard_item_state_t *state = &health->items[percent_items[i].alert];
        if (!dashboard_status_has(status, percent_items[i].field)) {
            state->severity = DASHBOARD_SEVERITY_OK;
            continue;
        }
        apply_timing(state, classify_percent(percent_items[i].value, percent_items[i].warn,
                                             percent_items[i].crit),
                     config, now_ms, config->crit_dwell_ms);
        if (state->severity > worst_severity) {
            worst = percent_items[i].alert;
            worst_severity = state->severity;
        }
    }

    // Load is only judgeable against the core count.
    {
        dashboard_item_state_t *state = &health->items[DASHBOARD_ALERT_LOAD];
        if (!dashboard_status_has(status, DASHBOARD_FIELD_LOAD) ||
            !dashboard_status_has(status, DASHBOARD_FIELD_CORES) || status->cores == 0) {
            state->severity = DASHBOARD_SEVERITY_OK;
        } else {
            const uint16_t load_pct = (uint16_t)(status->load_x100 / status->cores);
            // Strictly greater: load == cores is saturation, not a breach.
            dashboard_severity_t target = DASHBOARD_SEVERITY_OK;
            if (load_pct > config->load_crit_pct) {
                target = DASHBOARD_SEVERITY_CRIT;
            } else if (load_pct > config->load_warn_pct) {
                target = DASHBOARD_SEVERITY_WARN;
            }
            apply_timing(state, target, config, now_ms, config->crit_dwell_ms);
            if (state->severity > worst_severity) {
                worst = DASHBOARD_ALERT_LOAD;
                worst_severity = state->severity;
            }
        }
    }

    {
        dashboard_item_state_t *state = &health->items[DASHBOARD_ALERT_TEMP];
        if (!dashboard_status_has(status, DASHBOARD_FIELD_TEMP)) {
            state->severity = DASHBOARD_SEVERITY_OK;
        } else {
            const int16_t celsius = status->temp_c;
            dashboard_severity_t target = DASHBOARD_SEVERITY_OK;
            if (celsius >= config->temp_crit_c) {
                target = DASHBOARD_SEVERITY_CRIT;
            } else if (celsius >= config->temp_warn_c) {
                target = DASHBOARD_SEVERITY_WARN;
            }
            apply_timing(state, target, config, now_ms, config->crit_dwell_ms);
            if (state->severity > worst_severity) {
                worst = DASHBOARD_ALERT_TEMP;
                worst_severity = state->severity;
            }
        }
    }

    {
        dashboard_item_state_t *state = &health->items[DASHBOARD_ALERT_NET];
        if (!dashboard_status_has(status, DASHBOARD_FIELD_NET)) {
            state->severity = DASHBOARD_SEVERITY_OK;
        } else {
            const dashboard_severity_t target =
                status->net_up ? DASHBOARD_SEVERITY_OK : DASHBOARD_SEVERITY_CRIT;
            apply_timing(state, target, config, now_ms, config->crit_dwell_ms);
            if (state->severity > worst_severity) {
                worst = DASHBOARD_ALERT_NET;
                worst_severity = state->severity;
            }
        }
    }

    // Heartbeat/link outrank metric breaches: if the host is silent, its last
    // reported disk percentage is history, not news.
    if (worst == DASHBOARD_ALERT_NO_LINK || worst == DASHBOARD_ALERT_SILENT) {
        worst_severity = health->items[worst].severity;
    }

    // --- the badge's own battery -------------------------------------------
    // It outranks host problems once it reaches CRIT: the panel is about to die, and
    // this is the one alert the user can fix in two seconds. A WARN does not
    // outrank a host CRIT.
    if (health->battery_known) {
        dashboard_item_state_t *state = &health->items[DASHBOARD_ALERT_BATTERY];
        if (now_ms < health->charge_suppress_until_ms) {
            state->severity = DASHBOARD_SEVERITY_OK;
            state->crit_pending = false;
            state->below_pending = false;
        } else {
            const int soc = health->last_battery_soc;
            dashboard_severity_t target;
            if (soc <= (int)config->battery_crit_pct) {
                target = DASHBOARD_SEVERITY_CRIT;
            } else if (soc <= (int)config->battery_warn_pct) {
                target = DASHBOARD_SEVERITY_WARN;
            } else {
                target = DASHBOARD_SEVERITY_OK;
            }
            // Hysteresis: hold CRIT until the charge has climbed clear of it.
            const int clear_at =
                (int)config->battery_crit_pct + (int)config->battery_recover_margin_pct;
            if (state->severity == DASHBOARD_SEVERITY_CRIT && soc < clear_at) {
                target = DASHBOARD_SEVERITY_CRIT;
            }
            apply_timing(state, target, config, now_ms, config->battery_crit_dwell_ms);
            if (state->severity == DASHBOARD_SEVERITY_CRIT) {
                worst = DASHBOARD_ALERT_BATTERY;
                worst_severity = DASHBOARD_SEVERITY_CRIT;
            } else if (state->severity > worst_severity) {
                worst = DASHBOARD_ALERT_BATTERY;
                worst_severity = state->severity;
            }
        }
    }

    // Nothing has ever arrived: report that, not the link state that follows from it.
    if (!have_frames) {
        worst = DASHBOARD_ALERT_NO_DATA;
        worst_severity = DASHBOARD_SEVERITY_OK;
    }

    // A new kind of problem re-arms the acknowledgement.
    if (worst != health->last_reported) {
        health->acknowledged = DASHBOARD_ALERT_NONE;
        health->last_reported = worst;
    }

    verdict.alert = worst;
    verdict.severity = worst_severity;
    verdict.acknowledged =
        health->acknowledged != DASHBOARD_ALERT_NONE && health->acknowledged == worst;
    verdict.requires_action =
        worst_severity == DASHBOARD_SEVERITY_CRIT && !verdict.acknowledged;
    return verdict;
}

void dashboard_health_set_battery(dashboard_health_t *health, int percent, uint64_t now_ms)
{
    if (!health || !health->initialised) return;
    if (percent < 0) {
        health->battery_known = false;
        return;
    }
    if (health->battery_known && percent > health->last_battery_soc) {
        // Charge rising means USB power is attached; do not nag while it refills.
        health->charge_suppress_until_ms = now_ms + health->config.battery_charge_suppress_ms;
    }
    health->last_battery_soc = percent;
    health->battery_known = true;
}

void dashboard_health_acknowledge(dashboard_health_t *health)
{
    if (!health || !health->initialised) return;
    health->acknowledged = health->last_reported;
}

uint32_t dashboard_health_link_down_s(const dashboard_health_t *health, uint64_t now_ms)
{
    if (!health || !health->initialised) return 0;
    const dashboard_item_state_t *link = &health->items[DASHBOARD_ALERT_NO_LINK];
    if (!link->have_down || now_ms <= link->down_since_ms) return 0;
    return (uint32_t)((now_ms - link->down_since_ms) / 1000u);
}

bool dashboard_health_reboot_age_s(const dashboard_health_t *health, uint64_t now_ms,
                                   uint32_t *out_s)
{
    if (out_s) *out_s = 0;
    if (!health || !health->have_reboot) return false;
    if (out_s) {
        *out_s = (now_ms > health->reboot_ms)
                     ? (uint32_t)((now_ms - health->reboot_ms) / 1000u)
                     : 0u;
    }
    return true;
}
