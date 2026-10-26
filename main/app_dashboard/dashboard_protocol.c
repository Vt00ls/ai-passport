// main/app_dashboard/dashboard_protocol.c
// Firmware-side decoder for protocol/protocol.md. No ESP-IDF, no LVGL, no heap:
// this file must stay compilable by a plain host `cc` so the parser is covered
// by tests/test_dashboard_protocol.c.
#include "app_dashboard/dashboard_protocol.h"

#include <string.h>

typedef struct {
    const char *key;
    uint8_t key_length;
    dashboard_field_t field;
} field_entry_t;

// Lookup table kept tiny on purpose (no-PSRAM target, ~7.7 KB largest free block).
static const field_entry_t s_fields[] = {
    { "v", 1, DASHBOARD_FIELD_VERSION },
    { "t", 1, DASHBOARD_FIELD_TIME },
    { "h", 1, DASHBOARD_FIELD_HOST },
    { "nc", 2, DASHBOARD_FIELD_CORES },
    { "up", 2, DASHBOARD_FIELD_UPTIME },
    { "ld", 2, DASHBOARD_FIELD_LOAD },
    { "cpu", 3, DASHBOARD_FIELD_CPU },
    { "mem", 3, DASHBOARD_FIELD_MEM },
    { "dsk", 3, DASHBOARD_FIELD_DISK },
    { "tmp", 3, DASHBOARD_FIELD_TEMP },
    { "net", 3, DASHBOARD_FIELD_NET },
    { "ag", 2, DASHBOARD_FIELD_AGENTS },
    { "as", 2, DASHBOARD_FIELD_AGENT_IDLE },
    { "ar", 2, DASHBOARD_FIELD_AGENTS_RUNNING },
    { "ws", 2, DASHBOARD_FIELD_WORKSPACE },
    { "pv", 2, DASHBOARD_FIELD_PROVIDER },
};

#define FIELD_COUNT ((size_t)(sizeof(s_fields) / sizeof(s_fields[0])))

_Static_assert(DASHBOARD_FIELD_COUNT == 16,
               "presence mask is 16 bits; keep the field list and the enum in sync");

// ---------------------------------------------------------------------------
// Scalar helpers
// ---------------------------------------------------------------------------

static bool is_value_char(char c, bool allow_comma)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
    case '.':
    case '_':
    case '/':
    case '-':
    case '~':  // truncation marker: a shortened label says so instead of looking wrong
        return true;
    case ',':
        return allow_comma;
    default:
        return false;
    }
}

static bool value_chars_ok(const char *text, size_t length, bool allow_comma)
{
    if (length == 0) return false;
    for (size_t i = 0; i < length; i++) {
        if (!is_value_char(text[i], allow_comma)) return false;
    }
    return true;
}

// Strict unsigned decimal with overflow rejection. No sign, no whitespace.
static bool parse_u32(const char *text, size_t length, uint32_t *out)
{
    if (length == 0) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < length; i++) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        const uint32_t digit = (uint32_t)(c - '0');
        if (value > (UINT32_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static bool parse_i32(const char *text, size_t length, int32_t *out)
{
    if (length == 0) return false;
    bool negative = false;
    size_t index = 0;
    if (text[0] == '-') {
        negative = true;
        index = 1;
        if (length == 1) return false;
    }
    uint32_t magnitude = 0;
    if (!parse_u32(text + index, length - index, &magnitude)) return false;
    if (magnitude > (uint32_t)INT32_MAX) return false;
    *out = negative ? -(int32_t)magnitude : (int32_t)magnitude;
    return true;
}

// "d.dd" exactly: the wire format fixes two decimals, so "0.5" is a producer bug.
static bool parse_load_x100(const char *text, size_t length, uint16_t *out)
{
    size_t dot = length;
    for (size_t i = 0; i < length; i++) {
        if (text[i] != '.') continue;
        if (dot != length) return false;  // second dot
        dot = i;
    }
    if (dot == length || dot == 0) return false;
    if (length - dot - 1 != 2) return false;

    uint32_t whole = 0;
    uint32_t fraction = 0;
    if (!parse_u32(text, dot, &whole)) return false;
    if (!parse_u32(text + dot + 1, 2, &fraction)) return false;
    if (whole > UINT16_MAX / 100u) return false;  // bounds whole * 100
    const uint32_t combined = whole * 100u + fraction;
    if (combined > UINT16_MAX) return false;
    *out = (uint16_t)combined;
    return true;
}

static bool copy_bounded(char *destination, size_t capacity, const char *source,
                         size_t length)
{
    if (length == 0 || length >= capacity) return false;
    memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Field encoders
// ---------------------------------------------------------------------------

static bool parse_net(dashboard_status_t *status, const char *value, size_t length)
{
    if (!value_chars_ok(value, length, true)) return false;

    size_t first = length;
    size_t second = length;
    for (size_t i = 0; i < length; i++) {
        if (value[i] != ',') continue;
        if (first == length) {
            first = i;
        } else if (second == length) {
            second = i;
        } else {
            return false;  // a third separator is malformed
        }
    }
    if (first == length || second == length) return false;

    const size_t iface_length = first;
    const size_t state_length = second - first - 1;
    const char *state = value + first + 1;
    const char *ip = value + second + 1;
    const size_t ip_length = length - second - 1;

    if (!value_chars_ok(value, iface_length, false)) return false;
    if (!copy_bounded(status->net_iface, sizeof(status->net_iface), value, iface_length)) {
        return false;
    }

    if (state_length == 2 && memcmp(state, "up", 2) == 0) {
        status->net_up = true;
    } else if (state_length == 4 && memcmp(state, "down", 4) == 0) {
        status->net_up = false;
    } else {
        return false;
    }

    if (!value_chars_ok(ip, ip_length, false)) return false;
    if (!copy_bounded(status->net_ip, sizeof(status->net_ip), ip, ip_length)) return false;
    return true;
}

static bool parse_field(dashboard_status_t *status, dashboard_field_t field,
                        const char *value, size_t length)
{
    // net is the only field whose value carries an internal separator.
    if (!value_chars_ok(value, length, field == DASHBOARD_FIELD_NET)) return false;

    uint32_t unsigned_value = 0;
    int32_t signed_value = 0;

    switch (field) {
    case DASHBOARD_FIELD_VERSION:
        if (!parse_u32(value, length, &unsigned_value)) return false;
        if (unsigned_value == 0) return false;  // versions start at 1
        if (unsigned_value > DASHBOARD_PROTOCOL_VERSION) return false;
        status->version = (uint16_t)unsigned_value;
        return true;
    case DASHBOARD_FIELD_TIME:
        if (!parse_u32(value, length, &unsigned_value)) return false;
        status->heartbeat = unsigned_value;
        return true;
    case DASHBOARD_FIELD_HOST:
        return copy_bounded(status->host, sizeof(status->host), value, length);
    case DASHBOARD_FIELD_CORES:
        // 0 would make the load judgement meaningless, so it is rejected.
        if (!parse_u32(value, length, &unsigned_value)) return false;
        if (unsigned_value < 1u || unsigned_value > 1024u) return false;
        status->cores = (uint16_t)unsigned_value;
        return true;
    case DASHBOARD_FIELD_UPTIME:
        if (!parse_u32(value, length, &unsigned_value)) return false;
        status->uptime_s = unsigned_value;
        return true;
    case DASHBOARD_FIELD_LOAD:
        return parse_load_x100(value, length, &status->load_x100);
    case DASHBOARD_FIELD_CPU:
        if (!parse_u32(value, length, &unsigned_value) || unsigned_value > 100u) return false;
        status->cpu_percent = (uint8_t)unsigned_value;
        return true;
    case DASHBOARD_FIELD_MEM:
        if (!parse_u32(value, length, &unsigned_value) || unsigned_value > 100u) return false;
        status->mem_percent = (uint8_t)unsigned_value;
        return true;
    case DASHBOARD_FIELD_DISK:
        if (!parse_u32(value, length, &unsigned_value) || unsigned_value > 100u) return false;
        status->disk_percent = (uint8_t)unsigned_value;
        return true;
    case DASHBOARD_FIELD_TEMP:
        if (!parse_i32(value, length, &signed_value)) return false;
        if (signed_value < DASHBOARD_RANGE_TEMP_MIN || signed_value > DASHBOARD_RANGE_TEMP_MAX) {
            return false;
        }
        status->temp_c = (int16_t)signed_value;
        return true;
    case DASHBOARD_FIELD_NET:
        return parse_net(status, value, length);
    case DASHBOARD_FIELD_AGENTS:
        if (!parse_u32(value, length, &unsigned_value) || unsigned_value > UINT16_MAX) return false;
        status->agents_active = (uint16_t)unsigned_value;
        return true;
    case DASHBOARD_FIELD_AGENT_IDLE:
        if (!parse_i32(value, length, &signed_value)) return false;
        if (signed_value < -1) return false;
        status->agent_idle_s = signed_value;
        return true;
    case DASHBOARD_FIELD_AGENTS_RUNNING:
        if (!parse_u32(value, length, &unsigned_value) || unsigned_value > 99u) return false;
        status->agents_running = (uint8_t)unsigned_value;
        return true;
    case DASHBOARD_FIELD_WORKSPACE:
        return copy_bounded(status->workspace, sizeof(status->workspace), value, length);
    case DASHBOARD_FIELD_PROVIDER:
        return copy_bounded(status->provider, sizeof(status->provider), value, length);
    case DASHBOARD_FIELD_COUNT:
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void dashboard_status_reset(dashboard_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->agent_idle_s = -1;
}

dashboard_parse_t dashboard_status_parse(const char *text, size_t length,
                                         dashboard_status_t *status)
{
    if (!status) return DASHBOARD_PARSE_EMPTY;
    dashboard_status_reset(status);

    if (!text || length == 0) return DASHBOARD_PARSE_EMPTY;
    if (length > DASHBOARD_STATUS_MAX_BYTES) return DASHBOARD_PARSE_TOO_LONG;
    if (memchr(text, '\n', length) || memchr(text, '\r', length)) {
        return DASHBOARD_PARSE_SYNTAX;
    }

    dashboard_status_t parsed;
    dashboard_status_reset(&parsed);

    size_t cursor = 0;
    while (cursor < length) {
        size_t segment_end = cursor;
        while (segment_end < length && text[segment_end] != ';') segment_end++;
        const size_t segment_length = segment_end - cursor;

        size_t equals = segment_length;
        for (size_t i = 0; i < segment_length; i++) {
            if (text[cursor + i] != '=') continue;
            if (equals != segment_length) return DASHBOARD_PARSE_SYNTAX;
            equals = i;
        }
        if (equals == segment_length) return DASHBOARD_PARSE_SYNTAX;

        const char *key = text + cursor;
        const size_t key_length = equals;
        const char *value = text + cursor + equals + 1;
        const size_t value_length = segment_length - equals - 1;

        const field_entry_t *entry = NULL;
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (s_fields[i].key_length != key_length) continue;
            if (memcmp(s_fields[i].key, key, key_length) != 0) continue;
            entry = &s_fields[i];
            break;
        }
        if (!entry) return DASHBOARD_PARSE_UNKNOWN_FIELD;

        const uint32_t bit = DASHBOARD_FIELD_BIT(entry->field);
        if (parsed.present & bit) return DASHBOARD_PARSE_DUPLICATE_FIELD;
        if (!parse_field(&parsed, entry->field, value, value_length)) {
            return DASHBOARD_PARSE_BAD_VALUE;
        }
        parsed.present |= bit;

        if (segment_end == length) {
            cursor = length;
        } else {
            cursor = segment_end + 1;
            if (cursor == length) return DASHBOARD_PARSE_SYNTAX;  // trailing ';'
        }
    }

    if (!(parsed.present & DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_VERSION)) ||
        !(parsed.present & DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_TIME))) {
        return DASHBOARD_PARSE_MISSING_FIELD;
    }

    *status = parsed;
    return DASHBOARD_PARSE_OK;
}

bool dashboard_status_has(const dashboard_status_t *status, dashboard_field_t field)
{
    if (!status || field < 0 || field >= DASHBOARD_FIELD_COUNT) return false;
    return (status->present & DASHBOARD_FIELD_BIT(field)) != 0;
}

dashboard_age_t dashboard_heartbeat_age(uint64_t now_ms, uint64_t last_rx_ms, bool have_rx)
{
    dashboard_age_t age = { 0, false, false };
    if (!have_rx) {
        age.never_received = true;
        return age;
    }
    if (now_ms < last_rx_ms) {
        // A monotonic source that moved backwards is a bug, not 18 quintillion
        // seconds of silence: report zero age and let the caller log it.
        age.clock_went_back = true;
        return age;
    }
    age.age_s = (uint32_t)((now_ms - last_rx_ms) / 1000u);
    return age;
}

dashboard_link_t dashboard_link_state(bool have_rx, uint32_t age_s)
{
    if (!have_rx) return DASHBOARD_LINK_NONE;
    if (age_s <= DASHBOARD_LINK_LIVE_MAX_S) return DASHBOARD_LINK_LIVE;
    if (age_s <= DASHBOARD_LINK_STALE_MAX_S) return DASHBOARD_LINK_STALE;
    return DASHBOARD_LINK_LOST;
}
