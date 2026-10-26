// main/app_dashboard/dashboard_protocol.h
// Firmware-side decoder for the node-monitor status string defined in
// protocol/protocol.md. Deliberately free of ESP-IDF and LVGL dependencies so
// the parsing, heartbeat-age and link-state logic can be covered by host tests
// (tests/test_dashboard_protocol.c) instead of only on the device.
//
// RAM contract: no heap allocation. Callers own a single dashboard_status_t
// (about 80 bytes) and reuse it for every incoming BLE write.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wire limit from protocol/protocol.md: one ATT write after MTU negotiation.
#define DASHBOARD_STATUS_MAX_BYTES 200

#define DASHBOARD_HOST_MAX 24
#define DASHBOARD_IFACE_MAX 12
#define DASHBOARD_IPV4_MAX 15
#define DASHBOARD_WORKSPACE_MAX 20
#define DASHBOARD_PROVIDER_MAX 8

#define DASHBOARD_PROTOCOL_VERSION 1

// Age thresholds from protocol/protocol.md ("Push cadence and link thresholds").
#define DASHBOARD_LINK_LIVE_MAX_S 15u
#define DASHBOARD_LINK_STALE_MAX_S 120u

// Every field that may carry data. The order is the on-wire order.
typedef enum {
    DASHBOARD_FIELD_VERSION = 0,
    DASHBOARD_FIELD_TIME,
    DASHBOARD_FIELD_HOST,
    DASHBOARD_FIELD_CORES,
    DASHBOARD_FIELD_UPTIME,
    DASHBOARD_FIELD_LOAD,
    DASHBOARD_FIELD_CPU,
    DASHBOARD_FIELD_MEM,
    DASHBOARD_FIELD_DISK,
    DASHBOARD_FIELD_TEMP,
    DASHBOARD_FIELD_NET,
    DASHBOARD_FIELD_AGENTS,
    DASHBOARD_FIELD_AGENT_IDLE,
    DASHBOARD_FIELD_AGENTS_RUNNING,
    DASHBOARD_FIELD_WORKSPACE,
    DASHBOARD_FIELD_PROVIDER,
    DASHBOARD_FIELD_COUNT,
} dashboard_field_t;

// Presence bit for `field`. An absent field means "no data", never zero.
// 32 bits so the field list can grow without silently truncating the mask.
#define DASHBOARD_FIELD_BIT(field) ((uint32_t)1u << (unsigned)(field))

enum {
    DASHBOARD_RANGE_TEMP_MIN = -40,
    DASHBOARD_RANGE_TEMP_MAX = 125,
};

typedef enum {
    DASHBOARD_PARSE_OK = 0,
    DASHBOARD_PARSE_EMPTY,               // zero-length payload
    DASHBOARD_PARSE_TOO_LONG,            // longer than DASHBOARD_STATUS_MAX_BYTES
    DASHBOARD_PARSE_SYNTAX,              // bad segment framing ('=' count, empty segment)
    DASHBOARD_PARSE_UNKNOWN_FIELD,       // key not in the protocol
    DASHBOARD_PARSE_DUPLICATE_FIELD,     // key repeated
    DASHBOARD_PARSE_BAD_VALUE,           // wrong charset, type, or range
    DASHBOARD_PARSE_MISSING_FIELD,       // required key absent
    DASHBOARD_PARSE_UNSUPPORTED_VERSION, // v newer than DASHBOARD_PROTOCOL_VERSION
} dashboard_parse_t;

typedef struct {
    uint32_t present;    // dashboard_field_t bitmask of fields actually received
    uint16_t version;    // v
    uint32_t heartbeat;  // t: host wall-clock seconds at collection
    uint32_t uptime_s;   // up
    uint16_t cores;      // nc: online logical CPUs, the yardstick for load_x100
    uint16_t load_x100;  // ld, hundredths ("0.42" -> 42)
    uint16_t agents_active;
    int32_t agent_idle_s;  // as; -1 means "never seen"
    int16_t temp_c;        // tmp
    uint8_t cpu_percent;
    uint8_t mem_percent;
    uint8_t disk_percent;
    char host[DASHBOARD_HOST_MAX + 1];
    char net_iface[DASHBOARD_IFACE_MAX + 1];
    char net_ip[DASHBOARD_IPV4_MAX + 1];
    bool net_up;
    uint8_t agents_running;                        // ar: 0 means every agent is idle
    char workspace[DASHBOARD_WORKSPACE_MAX + 1];   // ws: the running agent's workspace
    char provider[DASHBOARD_PROVIDER_MAX + 1];     // pv: selects the logo
} dashboard_status_t;

// Reset to "nothing known": clears the presence mask and every value.
void dashboard_status_reset(dashboard_status_t *status);

// Parse one status string. `length` comes from the BLE write; the payload is not
// required to be NUL-terminated. On failure `*status` is left in the reset
// ("no data") state so a corrupt frame can never show stale values as current.
dashboard_parse_t dashboard_status_parse(const char *text, size_t length,
                                         dashboard_status_t *status);

// True when `field` arrived in the last successful parse.
bool dashboard_status_has(const dashboard_status_t *status, dashboard_field_t field);

typedef enum {
    DASHBOARD_LINK_NONE = 0,  // no status received yet
    DASHBOARD_LINK_LIVE,      // fresh
    DASHBOARD_LINK_STALE,     // missing several pushes
    DASHBOARD_LINK_LOST,      // effectively gone
} dashboard_link_t;

typedef struct {
    uint32_t age_s;         // whole seconds since the last received status
    bool never_received;    // no status since boot (age_s is then 0)
    bool clock_went_back;   // now < last_rx: monotonic input was not monotonic
} dashboard_age_t;

// Age from the badge's own monotonic clock, because the badge has no wall clock.
// `last_rx_ms` is the monotonic millisecond stamp captured when the last status
// was accepted; it is ignored when `have_rx` is false.
dashboard_age_t dashboard_heartbeat_age(uint64_t now_ms, uint64_t last_rx_ms, bool have_rx);

// Classify the link from that age.
dashboard_link_t dashboard_link_state(bool have_rx, uint32_t age_s);
