// main/app_dashboard/dashboard_ui.c
// Three pages: STATUS (verdict first), DETAIL (the numbers), AGENTS.
//
// Design rules this file implements, agreed in the design review:
//   - STATUS answers "is the machine OK?" before showing any number.
//   - A missing measurement renders "--", never 0.
//   - Losing the link and losing the heartbeat are different states, because they
//     mean different things to the person walking past the panel.
//   - Only CRIT takes the screen over; it never claims more than the badge knows
//     (SILENT / NO LINK, not "dead").
//   - Values are rewritten only when they change, to keep SPI traffic low on a
//     board with no PSRAM.
#include "app_dashboard/dashboard_ui.h"

#include <stdio.h>
#include <string.h>

#include "app_dashboard/dashboard_ble.h"
#include "app_dashboard/dashboard_format.h"
#include "app_dashboard/dashboard_health.h"
#include "app_dashboard/dashboard_protocol.h"
#include "app_dashboard/dashboard_state.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

#define PAGE_COUNT 3
#define PAGE_STATUS 0
#define PAGE_DETAIL 1
#define PAGE_AGENTS 2

#define BEAT_SLOTS 60
#define BEAT_SLOT_MS 5000u

// Dark flat panel: deliberately unlike the demo's light pixel-art shell.
#define COL_BG lv_color_hex(0x0B0F14)
#define COL_CARD lv_color_hex(0x121A24)
#define COL_TEXT lv_color_hex(0xE6EDF3)
#define COL_MUTED lv_color_hex(0x6E7B8B)
#define COL_DIM lv_color_hex(0x39424E)
#define COL_OK lv_color_hex(0x3FB950)
#define COL_WARN lv_color_hex(0xD29922)
#define COL_CRIT lv_color_hex(0xF85149)
#define COL_IDLE lv_color_hex(0x4A5563)

#define REFRESH_PERIOD_MS 200
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 20
// 3 pulses of 400 ms on/off, then hold, and repeat every 30 s until acknowledged.
#define PULSE_ON_MS 400u
#define PULSE_PHASE_MS 2400u
#define PULSE_REPEAT_MS 30000u
// How long a detected host reboot stays worth mentioning on the status line.
#define REBOOT_NOTICE_S 600u

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *host;
    lv_obj_t *battery;
} chrome_t;

static chrome_t s_chrome[PAGE_COUNT];
static lv_obj_t *s_dots[PAGE_COUNT][PAGE_COUNT];
static lv_timer_t *s_timer;

// STATUS page
static lv_obj_t *s_card;
static lv_obj_t *s_verdict;
static lv_obj_t *s_card_value;
static lv_obj_t *s_card_caption;
static lv_obj_t *s_strip;
static lv_chart_series_t *s_strip_series;
static lv_obj_t *s_caption;
static lv_obj_t *s_ind_label[4];
static lv_obj_t *s_ind_bar[4];
static lv_obj_t *s_statusline;

// DETAIL page
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_value[6];
static lv_obj_t *s_detail_net;

// AGENTS page
static lv_obj_t *s_agents_title;
static lv_obj_t *s_agents_active;
static lv_obj_t *s_agents_last;
static lv_obj_t *s_agents_hint;

static dashboard_health_t s_health;
static dashboard_verdict_t s_verdict_now;

static uint8_t s_page;
static bool s_started;
static bool s_dimmed;
static uint8_t s_backlight_now;

// Heartbeat history: one slot per 5 s, newest at the right edge.
static uint8_t s_beats[BEAT_SLOTS];
static uint32_t s_beat_slot;
static bool s_beat_started;
static uint32_t s_last_frame_count;

// Render cache.
static bool s_rendered_valid;
static uint32_t s_rendered_frames;
static bool s_rendered_connected;
static bool s_rendered_have_rx;
static int s_rendered_alert;
static int s_rendered_severity;
static uint32_t s_rendered_age_s;
static bool s_rendered_never;
static uint32_t s_rendered_reboot_min;
static bool s_rendered_reboot_valid;
static uint64_t s_attention_since_ms;

static lv_obj_t *make_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    if (!screen) return NULL;
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                            int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return NULL;
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *make_plain(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void build_chrome(int page)
{
    chrome_t *chrome = &s_chrome[page];
    chrome->screen = make_screen();
    if (!chrome->screen) return;

    chrome->host = make_label(chrome->screen, &lv_font_montserrat_14, COL_MUTED, 14, 12);
    chrome->battery = make_label(chrome->screen, &lv_font_montserrat_14, COL_MUTED, 0, 12);
    lv_obj_align(chrome->battery, LV_ALIGN_TOP_RIGHT, -14, 12);

    for (int index = 0; index < PAGE_COUNT; index++) {
        lv_obj_t *indicator = make_plain(chrome->screen, 104 + index * 14, 304, 6, 6);
        if (!indicator) continue;
        lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
        // Neutral, never the status colour: a healthy page must not look like an alert.
        lv_obj_set_style_bg_color(indicator, page == index ? COL_TEXT : COL_DIM, 0);
        s_dots[page][index] = indicator;
    }
}

static void build_status_page(void)
{
    build_chrome(PAGE_STATUS);
    lv_obj_t *screen = s_chrome[PAGE_STATUS].screen;
    if (!screen) return;

    s_card = make_plain(screen, 14, 50, 212, 104);
    if (!s_card) return;
    lv_obj_set_style_radius(s_card, 12, 0);
    lv_obj_set_style_bg_color(s_card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_border_color(s_card, COL_OK, 0);

    s_verdict = make_label(s_card, &lv_font_montserrat_36, COL_OK, 0, 8);
    lv_obj_set_width(s_verdict, 212);
    lv_obj_set_style_text_align(s_verdict, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_verdict, "--");

    // The caption disambiguates what the big number means: uptime while healthy,
    // time since the last frame once the host stops answering.
    s_card_caption = make_label(s_card, &lv_font_montserrat_14, COL_MUTED, 0, 46);
    lv_obj_set_width(s_card_caption, 212);
    lv_obj_set_style_text_align(s_card_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_card_caption, "");

    s_card_value = make_label(s_card, &lv_font_montserrat_36, COL_TEXT, 0, 64);
    lv_obj_set_width(s_card_value, 212);
    lv_obj_set_style_text_align(s_card_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_card_value, "--");

    s_strip = lv_chart_create(screen);
    if (s_strip) {
        lv_obj_set_size(s_strip, 212, 24);
        lv_obj_set_pos(s_strip, 14, 166);
        lv_chart_set_type(s_strip, LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(s_strip, BEAT_SLOTS);
        lv_chart_set_div_line_count(s_strip, 0, 0);
        lv_chart_set_axis_range(s_strip, LV_CHART_AXIS_PRIMARY_Y, 0, 1);
        lv_obj_set_style_bg_opa(s_strip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_strip, 0, 0);
        lv_obj_set_style_pad_all(s_strip, 0, 0);
        lv_obj_set_style_pad_column(s_strip, 1, LV_PART_MAIN);
        lv_obj_set_style_line_width(s_strip, 0, LV_PART_ITEMS);
        s_strip_series = lv_chart_add_series(s_strip, COL_OK, LV_CHART_AXIS_PRIMARY_Y);
        if (s_strip_series) {
            for (int i = 0; i < BEAT_SLOTS; i++) {
                lv_chart_set_series_value_by_id(s_strip, s_strip_series, (uint32_t)i, 0);
            }
        }
    }

    s_caption = make_label(screen, &lv_font_montserrat_14, COL_MUTED, 14, 194);
    lv_label_set_text(s_caption, "LAST 5 MIN");

    static const char *names[4] = { "LOAD", "MEM", "DISK", "TEMP" };
    for (int i = 0; i < 4; i++) {
        const int x = 14 + (i % 2) * 108;
        const int y = 216 + (i / 2) * 32;
        s_ind_label[i] = make_label(screen, &lv_font_montserrat_14, COL_MUTED, x, y);
        lv_label_set_text(s_ind_label[i], names[i]);
        s_ind_bar[i] = lv_bar_create(screen);
        if (!s_ind_bar[i]) continue;
        lv_obj_set_size(s_ind_bar[i], 88, 6);
        lv_obj_set_pos(s_ind_bar[i], x, y + 17);
        lv_obj_set_style_radius(s_ind_bar[i], 3, 0);
        lv_obj_set_style_bg_color(s_ind_bar[i], COL_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_ind_bar[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_ind_bar[i], 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_ind_bar[i], COL_OK, LV_PART_INDICATOR);
        lv_bar_set_range(s_ind_bar[i], 0, 100);
        lv_bar_set_value(s_ind_bar[i], 0, LV_ANIM_OFF);
    }

    s_statusline = make_label(screen, &lv_font_montserrat_14, COL_MUTED, 14, 282);
    lv_label_set_text(s_statusline, "");
}

static void build_detail_page(void)
{
    build_chrome(PAGE_DETAIL);
    lv_obj_t *screen = s_chrome[PAGE_DETAIL].screen;
    if (!screen) return;

    s_detail_title = make_label(screen, &lv_font_montserrat_20, COL_TEXT, 14, 40);
    lv_label_set_text(s_detail_title, "DETAIL");

    static const char *captions[6] = { "UPTIME", "LOAD", "CPU", "MEM", "DISK", "TEMP" };
    for (int i = 0; i < 6; i++) {
        const int x = 14 + (i % 2) * 108;
        const int y = 80 + (i / 2) * 46;
        lv_obj_t *caption = make_label(screen, &lv_font_montserrat_14, COL_MUTED, x, y);
        lv_label_set_text(caption, captions[i]);
        s_detail_value[i] = make_label(screen, &lv_font_montserrat_20, COL_TEXT, x, y + 18);
        lv_label_set_text(s_detail_value[i], "--");
    }

    s_detail_net = make_label(screen, &lv_font_montserrat_14, COL_MUTED, 14, 268);
    lv_label_set_text(s_detail_net, "");
    lv_obj_t *caption = make_label(screen, &lv_font_montserrat_14, COL_DIM, 14, 250);
    lv_label_set_text(caption, "NETWORK");
}

static void build_agents_page(void)
{
    build_chrome(PAGE_AGENTS);
    lv_obj_t *screen = s_chrome[PAGE_AGENTS].screen;
    if (!screen) return;

    s_agents_title = make_label(screen, &lv_font_montserrat_20, COL_TEXT, 14, 40);
    lv_label_set_text(s_agents_title, "AGENTS");

    lv_obj_t *caption = make_label(screen, &lv_font_montserrat_14, COL_MUTED, 14, 88);
    lv_label_set_text(caption, "ACTIVE SESSIONS");
    s_agents_active = make_label(screen, &lv_font_montserrat_48, COL_TEXT, 14, 106);
    lv_label_set_text(s_agents_active, "--");

    caption = make_label(screen, &lv_font_montserrat_14, COL_MUTED, 14, 176);
    lv_label_set_text(caption, "LAST ACTIVITY");
    s_agents_last = make_label(screen, &lv_font_montserrat_36, COL_TEXT, 14, 194);
    lv_label_set_text(s_agents_last, "--");

    s_agents_hint = make_label(screen, &lv_font_montserrat_14, COL_DIM, 14, 268);
    lv_label_set_text(s_agents_hint, "");
}

static lv_color_t severity_colour(dashboard_severity_t severity)
{
    switch (severity) {
    case DASHBOARD_SEVERITY_CRIT:
        return COL_CRIT;
    case DASHBOARD_SEVERITY_WARN:
        return COL_WARN;
    default:
        return COL_OK;
    }
}

// Vocabulary is deliberately modest: the badge cannot tell "host dead" from
// "pusher broken", so it reports what it observes, never a cause it cannot prove.
static const char *verdict_word(dashboard_alert_t alert, bool *critical_panel)
{
    *critical_panel = false;
    switch (alert) {
    case DASHBOARD_ALERT_NO_DATA:
        return "NO DATA";
    case DASHBOARD_ALERT_NO_LINK:
        return "NO LINK";
    case DASHBOARD_ALERT_SILENT:
        return "SILENT";
    case DASHBOARD_ALERT_DISK:
        *critical_panel = true;
        return "DISK";
    case DASHBOARD_ALERT_MEM:
        *critical_panel = true;
        return "MEMORY";
    case DASHBOARD_ALERT_LOAD:
        *critical_panel = true;
        return "LOAD";
    case DASHBOARD_ALERT_TEMP:
        *critical_panel = true;
        return "TEMP";
    case DASHBOARD_ALERT_NET:
        *critical_panel = true;
        return "NETWORK";
    case DASHBOARD_ALERT_BATTERY:
        *critical_panel = true;
        return "LOW BATT";
    case DASHBOARD_ALERT_NONE:
    default:
        return "OK";
    }
}

static const char *alert_action(dashboard_alert_t alert)
{
    switch (alert) {
    case DASHBOARD_ALERT_DISK:
        return "FREE DISK SPACE";
    case DASHBOARD_ALERT_MEM:
        return "CHECK MEMORY";
    case DASHBOARD_ALERT_LOAD:
        return "CHECK RUNNING JOBS";
    case DASHBOARD_ALERT_TEMP:
        return "CHECK COOLING";
    case DASHBOARD_ALERT_NET:
        return "CHECK NETWORK";
    case DASHBOARD_ALERT_BATTERY:
        return "PLUG IN THE CHARGER";
    default:
        return "";
    }
}

static void advance_beats(const dashboard_snapshot_t *snapshot, uint64_t now_ms)
{
    const uint32_t slot = (uint32_t)(now_ms / BEAT_SLOT_MS);
    if (!s_beat_started) {
        s_beat_started = true;
        s_beat_slot = slot;
        memset(s_beats, 0, sizeof(s_beats));
    } else if (slot != s_beat_slot) {
        // Clear every slot we skipped so a quiet period leaves visible gaps.
        uint32_t gap = slot - s_beat_slot;
        if (gap > BEAT_SLOTS) gap = BEAT_SLOTS;
        for (uint32_t i = 0; i < gap; i++) {
            s_beats[(s_beat_slot + 1 + i) % BEAT_SLOTS] = 0;
        }
        s_beat_slot = slot;
    }
    if (snapshot->accepted_frames != s_last_frame_count) {
        s_beats[slot % BEAT_SLOTS] = 1;
        s_last_frame_count = snapshot->accepted_frames;
    }
}

static void paint_beats(lv_color_t colour)
{
    if (!s_strip || !s_strip_series) return;
    for (int i = 0; i < BEAT_SLOTS; i++) {
        // Oldest sample on the left, so the strip reads like a timeline.
        const uint32_t slot_index = (s_beat_slot + 1 + (uint32_t)i) % BEAT_SLOTS;
        lv_chart_set_series_value_by_id(s_strip, s_strip_series, (uint32_t)i,
                                        s_beats[slot_index] ? 1 : 0);
    }
    lv_chart_set_series_color(s_strip, s_strip_series, colour);
    lv_chart_refresh(s_strip);
}

static void set_text(lv_obj_t *label, const char *text, lv_color_t colour)
{
    if (!label) return;
    lv_obj_set_style_text_color(label, colour, 0);
    lv_label_set_text(label, text);
}

static void render_status_page(const dashboard_snapshot_t *snapshot, const dashboard_verdict_t *verdict,
                               uint32_t age_s, bool never, lv_color_t state_colour,
                               bool show_values, bool values_changed, bool reboot_recent,
                               uint32_t reboot_age_s, uint32_t link_down_s,
                               int battery_percent)
{
    bool critical_panel = false;
    const char *word = verdict_word(verdict->alert, &critical_panel);

    if (!values_changed) return;

    const dashboard_status_t *status = &snapshot->status;
    char buffer[32];

    if (critical_panel) {
        // A metric breach: red fill, name, value, and what to do about it.
        lv_obj_set_style_bg_color(s_card, COL_CRIT, 0);
        lv_obj_set_style_border_color(s_card, COL_CRIT, 0);
        set_text(s_verdict, word, COL_BG);
        switch (verdict->alert) {
        case DASHBOARD_ALERT_DISK:
            snprintf(buffer, sizeof(buffer), "%u%%", (unsigned)status->disk_percent);
            break;
        case DASHBOARD_ALERT_MEM:
            snprintf(buffer, sizeof(buffer), "%u%%", (unsigned)status->mem_percent);
            break;
        case DASHBOARD_ALERT_LOAD:
            dashboard_format_load(buffer, sizeof(buffer), status->load_x100);
            break;
        case DASHBOARD_ALERT_TEMP:
            dashboard_format_temperature(buffer, sizeof(buffer), true, status->temp_c);
            break;
        case DASHBOARD_ALERT_NET:
            snprintf(buffer, sizeof(buffer), "DOWN");
            break;
        case DASHBOARD_ALERT_BATTERY:
            if (battery_percent >= 0) {
                snprintf(buffer, sizeof(buffer), "%d%%", battery_percent > 100 ? 100 : battery_percent);
            } else {
                snprintf(buffer, sizeof(buffer), "--");
            }
            break;
        default:
            snprintf(buffer, sizeof(buffer), "--");
            break;
        }
        // A metric breach is the one case that keeps the number as the hero.
        lv_obj_set_style_text_font(s_card_value, &lv_font_montserrat_48, 0);
        lv_obj_set_pos(s_card_value, 0, 44);
        set_text(s_card_caption, "", COL_BG);
        set_text(s_card_value, buffer, COL_BG);
        snprintf(buffer, sizeof(buffer), "%s", alert_action(verdict->alert));
        set_text(s_statusline, buffer, COL_CRIT);
    } else {
        lv_obj_set_style_bg_color(s_card, COL_CARD, 0);
        lv_obj_set_style_border_color(s_card, state_colour, 0);
        set_text(s_verdict, word, state_colour);
        lv_obj_set_style_text_font(s_card_value, &lv_font_montserrat_36, 0);
        lv_obj_set_pos(s_card_value, 0, 64);

        // Uptime while the host is answering; time since the last frame once it is not.
        // Uptime is monotonic, so the number no longer resets every push interval.
        char value_text[24];
        const char *caption = "UPTIME";
        if (verdict->alert == DASHBOARD_ALERT_SILENT) {
            caption = "NO FRAMES FOR";
            dashboard_format_age(value_text, sizeof(value_text), age_s, never);
        } else if (verdict->alert == DASHBOARD_ALERT_NO_LINK) {
            // The link-down duration, not the frame age: they start at different times.
            caption = "DOWN FOR";
            dashboard_format_age(value_text, sizeof(value_text), link_down_s, false);
        } else if (verdict->alert == DASHBOARD_ALERT_NO_DATA) {
            caption = "LAST PUSH";
            dashboard_format_text(value_text, sizeof(value_text), false, NULL);
        } else if (dashboard_status_has(status, DASHBOARD_FIELD_UPTIME)) {
            dashboard_format_uptime(value_text, sizeof(value_text), status->uptime_s);
        } else {
            dashboard_format_text(value_text, sizeof(value_text), false, NULL);
        }
        set_text(s_card_caption, caption,
                 (verdict->severity == DASHBOARD_SEVERITY_OK) ? COL_MUTED : state_colour);
        set_text(s_card_value, value_text,
                 (verdict->severity == DASHBOARD_SEVERITY_OK) ? COL_TEXT : state_colour);

        if (reboot_recent) {
            // The host restarted. Worth knowing, but only while the machine is
            // otherwise healthy: during an alert the alert text matters more.
            char notice[40];
            char ago[24];
            dashboard_format_age(ago, sizeof(ago), reboot_age_s, false);
            snprintf(notice, sizeof(notice), "rebooted %s ago", ago);
            set_text(s_statusline, notice, COL_WARN);
        } else if (verdict->alert == DASHBOARD_ALERT_SILENT) {
            set_text(s_statusline, "host stopped responding", COL_CRIT);
        } else if (verdict->alert == DASHBOARD_ALERT_NO_LINK) {
            set_text(s_statusline, "check bluetooth and pusher", state_colour);
        } else if (verdict->alert == DASHBOARD_ALERT_NO_DATA) {
            set_text(s_statusline, "waiting for host", COL_MUTED);
        } else {
            char line[40];
            const bool net_ok = dashboard_status_has(status, DASHBOARD_FIELD_NET) && status->net_up;
            if (dashboard_status_has(status, DASHBOARD_FIELD_AGENTS)) {
                snprintf(line, sizeof(line), "%s   %u agents", net_ok ? status->net_iface : "net down",
                         (unsigned)status->agents_active);
            } else {
                snprintf(line, sizeof(line), "%s", net_ok ? status->net_iface : "net down");
            }
            set_text(s_statusline, line, COL_MUTED);
        }
    }

    // Four pillars. Each shows its own colour so the culprit is findable at a glance.
    static const char *names[4] = { "LOAD", "MEM", "DISK", "TEMP" };
    const bool has[4] = {
        dashboard_status_has(status, DASHBOARD_FIELD_LOAD) &&
            dashboard_status_has(status, DASHBOARD_FIELD_CORES),
        dashboard_status_has(status, DASHBOARD_FIELD_MEM),
        dashboard_status_has(status, DASHBOARD_FIELD_DISK),
        dashboard_status_has(status, DASHBOARD_FIELD_TEMP),
    };
    for (int i = 0; i < 4; i++) {
        char text[32];
        if (!show_values || !has[i]) {
            snprintf(text, sizeof(text), "%s --", names[i]);
            set_text(s_ind_label[i], text, COL_DIM);
            if (s_ind_bar[i]) {
                lv_bar_set_value(s_ind_bar[i], 0, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(s_ind_bar[i], COL_DIM, LV_PART_INDICATOR);
            }
            continue;
        }
        // Each row has its own unit: percent for memory and disk, the raw load
        // average against the core count, and degrees for temperature. A shared
        // format string put a "%" on the temperature and confused the load value.
        int percent = 0;
        switch (i) {
        case 0: {  // LOAD: show "0.42/6" so it matches what top reports, while the
                   // bar still fills by capacity so the colour reads at a glance.
            char load_text[16];
            dashboard_format_load(load_text, sizeof(load_text), status->load_x100);
            snprintf(text, sizeof(text), "%s %s/%u", names[0], load_text,
                     (unsigned)status->cores);
            percent = (int)(status->load_x100 / status->cores);
            break;
        }
        case 1:
            snprintf(text, sizeof(text), "%s %u%%", names[1], (unsigned)status->mem_percent);
            percent = status->mem_percent;
            break;
        case 2:
            snprintf(text, sizeof(text), "%s %u%%", names[2], (unsigned)status->disk_percent);
            percent = status->disk_percent;
            break;
        default:
            snprintf(text, sizeof(text), "%s %dC", names[3], (int)status->temp_c);
            percent = status->temp_c;
            break;
        }
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        set_text(s_ind_label[i], text, COL_MUTED);
        if (s_ind_bar[i]) {
            lv_bar_set_value(s_ind_bar[i], percent, LV_ANIM_OFF);
            const dashboard_alert_t alert = (i == 0)   ? DASHBOARD_ALERT_LOAD
                                            : (i == 1) ? DASHBOARD_ALERT_MEM
                                            : (i == 2) ? DASHBOARD_ALERT_DISK
                                                       : DASHBOARD_ALERT_TEMP;
            const bool breached = (verdict->alert == alert);
            lv_obj_set_style_bg_color(
                s_ind_bar[i],
                breached ? severity_colour(verdict->severity) : COL_OK,
                LV_PART_INDICATOR);
        }
    }
}

static void render_detail_page(const dashboard_snapshot_t *snapshot, bool show_values)
{
    const dashboard_status_t *status = &snapshot->status;
    char buffer[32];

    dashboard_format_uptime(buffer, sizeof(buffer), status->uptime_s);
    set_text(s_detail_value[0],
             show_values && dashboard_status_has(status, DASHBOARD_FIELD_UPTIME) ? buffer : "--",
             COL_TEXT);

    dashboard_format_load(buffer, sizeof(buffer), status->load_x100);
    if (show_values && dashboard_status_has(status, DASHBOARD_FIELD_LOAD) &&
        dashboard_status_has(status, DASHBOARD_FIELD_CORES)) {
        char with_cores[48];
        snprintf(with_cores, sizeof(with_cores), "%s / %u", buffer, (unsigned)status->cores);
        set_text(s_detail_value[1], with_cores, COL_TEXT);
    } else {
        set_text(s_detail_value[1], "--", COL_TEXT);
    }

    dashboard_format_percent(buffer, sizeof(buffer),
                             show_values && dashboard_status_has(status, DASHBOARD_FIELD_CPU),
                             status->cpu_percent);
    set_text(s_detail_value[2], buffer, COL_TEXT);
    dashboard_format_percent(buffer, sizeof(buffer),
                             show_values && dashboard_status_has(status, DASHBOARD_FIELD_MEM),
                             status->mem_percent);
    set_text(s_detail_value[3], buffer, COL_TEXT);
    dashboard_format_percent(buffer, sizeof(buffer),
                             show_values && dashboard_status_has(status, DASHBOARD_FIELD_DISK),
                             status->disk_percent);
    set_text(s_detail_value[4], buffer, COL_TEXT);
    dashboard_format_temperature(buffer, sizeof(buffer),
                                 show_values && dashboard_status_has(status, DASHBOARD_FIELD_TEMP),
                                 status->temp_c);
    set_text(s_detail_value[5], buffer, COL_TEXT);

    if (show_values && dashboard_status_has(status, DASHBOARD_FIELD_NET)) {
        char line[48];
        snprintf(line, sizeof(line), "%s  %s  %s", status->net_iface,
                 status->net_up ? "up" : "down", status->net_ip);
        set_text(s_detail_net, line, status->net_up ? COL_MUTED : COL_CRIT);
    } else {
        set_text(s_detail_net, "--", COL_DIM);
    }
}

static void render_agents_page(const dashboard_snapshot_t *snapshot, bool show_values)
{
    const dashboard_status_t *status = &snapshot->status;
    char buffer[24];

    if (show_values && dashboard_status_has(status, DASHBOARD_FIELD_AGENTS)) {
        snprintf(buffer, sizeof(buffer), "%u", (unsigned)status->agents_active);
        set_text(s_agents_active, buffer, COL_TEXT);
    } else {
        set_text(s_agents_active, "--", COL_DIM);
    }

    if (show_values && dashboard_status_has(status, DASHBOARD_FIELD_AGENT_IDLE) &&
        status->agent_idle_s >= 0) {
        dashboard_format_age(buffer, sizeof(buffer), (uint32_t)status->agent_idle_s, false);
        set_text(s_agents_last, buffer, COL_TEXT);
    } else {
        set_text(s_agents_last, "--", COL_DIM);
    }

    set_text(s_agents_hint, show_values ? "" : "waiting for host", COL_DIM);
}

static void apply_backlight(const dashboard_verdict_t *verdict, uint64_t now_ms)
{
    if (verdict->requires_action) {
        if (s_attention_since_ms == 0) s_attention_since_ms = now_ms;
        const uint32_t phase = (uint32_t)((now_ms - s_attention_since_ms) % PULSE_REPEAT_MS);
        // Three pulses at the start of each cycle, then a steady hold.
        const uint8_t target =
            (phase >= PULSE_PHASE_MS || ((phase / PULSE_ON_MS) % 2u) == 0u) ? BACKLIGHT_FULL
                                                                            : BACKLIGHT_DIM;
        if (target != s_backlight_now) {
            bsp_display_backlight(target);
            s_backlight_now = target;
        }
        return;
    }
    s_attention_since_ms = 0;
    const uint8_t target = s_dimmed ? BACKLIGHT_DIM : BACKLIGHT_FULL;
    if (target != s_backlight_now) {
        bsp_display_backlight(target);
        s_backlight_now = target;
    }
}

static int battery_soc(void)
{
    return bsp_battery_soc();
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    dashboard_snapshot_t snapshot;
    if (!dashboard_state_read(&snapshot)) return;

    const uint64_t now_ms = dashboard_state_now_ms();
    const int battery_percent = bsp_battery_soc();
    dashboard_health_set_battery(&s_health, battery_percent, now_ms);
    s_verdict_now = dashboard_health_update(&s_health, &snapshot, now_ms);
    const dashboard_age_t age =
        dashboard_heartbeat_age(now_ms, snapshot.last_rx_ms, snapshot.have_rx);
    const lv_color_t state_colour = severity_colour(s_verdict_now.severity);

    // Values are only shown when we actually have current data. Losing the link or
    // the heartbeat turns the numbers into placeholders rather than history.
    const bool show_values =
        snapshot.have_rx && s_verdict_now.alert != DASHBOARD_ALERT_NO_DATA &&
        s_verdict_now.alert != DASHBOARD_ALERT_NO_LINK &&
        s_verdict_now.alert != DASHBOARD_ALERT_SILENT;

    uint32_t reboot_age_s = 0;
    const bool have_reboot = dashboard_health_reboot_age_s(&s_health, now_ms, &reboot_age_s);
    const bool reboot_recent = have_reboot && reboot_age_s < REBOOT_NOTICE_S;
    const uint32_t reboot_min = reboot_recent ? (reboot_age_s / 60u) : UINT32_MAX;

    const bool values_changed = !s_rendered_valid ||
                                s_rendered_frames != snapshot.accepted_frames ||
                                s_rendered_connected != snapshot.connected ||
                                s_rendered_have_rx != snapshot.have_rx ||
                                s_rendered_alert != (int)s_verdict_now.alert ||
                                s_rendered_severity != (int)s_verdict_now.severity ||
                                !s_rendered_reboot_valid ||
                                s_rendered_reboot_min != reboot_min;

    advance_beats(&snapshot, now_ms);

    // Chrome on every page: host name and battery.
    char battery_text[8];
    const int soc = battery_soc();
    if (soc < 0) {
        battery_text[0] = '\0';
    } else {
        snprintf(battery_text, sizeof(battery_text), "%u%%",
                 (unsigned)(soc > 100 ? 100 : soc));
    }
    const lv_color_t battery_colour = (soc < 0)                        ? COL_MUTED
                                      : (soc <= 10)                    ? COL_CRIT
                                      : (soc <= 20)                    ? COL_WARN
                                                                       : COL_MUTED;
    for (int page = 0; page < PAGE_COUNT; page++) {
        if (!s_chrome[page].battery) continue;
        lv_label_set_text(s_chrome[page].battery, battery_text);
        lv_obj_set_style_text_color(s_chrome[page].battery, battery_colour, 0);
        if (!s_chrome[page].host) continue;
        // The host name is identity, not a measurement: keep showing it.
        lv_label_set_text(s_chrome[page].host,
                          dashboard_status_has(&snapshot.status, DASHBOARD_FIELD_HOST)
                              ? snapshot.status.host
                              : "");
    }

    if (values_changed) {
        render_status_page(&snapshot, &s_verdict_now, age.age_s, age.never_received,
                           state_colour, show_values, true, reboot_recent, reboot_age_s,
                           dashboard_health_link_down_s(&s_health, now_ms), battery_percent);
        render_detail_page(&snapshot, show_values);
        render_agents_page(&snapshot, show_values);
        paint_beats(show_values ? state_colour : COL_IDLE);
        s_rendered_frames = snapshot.accepted_frames;
        s_rendered_connected = snapshot.connected;
        s_rendered_have_rx = snapshot.have_rx;
        s_rendered_alert = (int)s_verdict_now.alert;
        s_rendered_severity = (int)s_verdict_now.severity;
        s_rendered_reboot_min = reboot_min;
        s_rendered_reboot_valid = true;
        s_rendered_valid = true;
        s_rendered_age_s = UINT32_MAX;
    }

    // Only the silence states carry a per-second number; healthy states show uptime,
    // which changes at most once a minute, so they are refreshed by values_changed.
    const uint32_t link_down_s = dashboard_health_link_down_s(&s_health, now_ms);
    const uint32_t ticking_s =
        (s_verdict_now.alert == DASHBOARD_ALERT_NO_LINK) ? link_down_s : age.age_s;
    if ((s_verdict_now.alert == DASHBOARD_ALERT_SILENT ||
         s_verdict_now.alert == DASHBOARD_ALERT_NO_LINK) &&
        (s_rendered_age_s != ticking_s || s_rendered_never != age.never_received)) {
        char age_text[24];
        dashboard_format_age(age_text, sizeof(age_text), ticking_s, age.never_received);
        set_text(s_card_value, age_text, state_colour);
        paint_beats(show_values ? state_colour : COL_IDLE);
        s_rendered_age_s = ticking_s;
        s_rendered_never = age.never_received;
    }

    apply_backlight(&s_verdict_now, now_ms);
}

static void show_page(uint8_t page)
{
    s_page = page;
    if (s_chrome[page].screen) lv_screen_load(s_chrome[page].screen);
    for (int index = 0; index < PAGE_COUNT; index++) {
        if (!s_dots[page][index]) continue;
        lv_obj_set_style_bg_color(s_dots[page][index], index == page ? COL_TEXT : COL_DIM, 0);
    }
    s_rendered_valid = false;
}

esp_err_t dashboard_ui_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;

    dashboard_state_init();
    dashboard_health_init(&s_health, NULL);

    build_status_page();
    build_detail_page();
    build_agents_page();
    if (!s_chrome[PAGE_STATUS].screen || !s_chrome[PAGE_DETAIL].screen ||
        !s_chrome[PAGE_AGENTS].screen || !s_card) {
        dashboard_ui_stop();
        return ESP_ERR_NO_MEM;
    }

    show_page(PAGE_STATUS);
    s_backlight_now = BACKLIGHT_FULL;
    s_timer = lv_timer_create(refresh_timer_cb, REFRESH_PERIOD_MS, NULL);
    if (!s_timer) {
        dashboard_ui_stop();
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void dashboard_ui_stop(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    for (int page = 0; page < PAGE_COUNT; page++) {
        if (!s_chrome[page].screen) continue;
        lv_obj_delete(s_chrome[page].screen);
        s_chrome[page].screen = NULL;
        s_chrome[page].host = NULL;
        s_chrome[page].battery = NULL;
        for (int index = 0; index < PAGE_COUNT; index++) s_dots[page][index] = NULL;
    }
    s_card = s_verdict = s_card_value = s_card_caption = s_strip = s_caption = s_statusline = NULL;
    s_strip_series = NULL;
    for (int i = 0; i < 4; i++) {
        s_ind_label[i] = NULL;
        s_ind_bar[i] = NULL;
    }
    for (int i = 0; i < 6; i++) s_detail_value[i] = NULL;
    s_detail_title = s_detail_net = NULL;
    s_agents_title = s_agents_active = s_agents_last = s_agents_hint = NULL;
    s_rendered_valid = false;
    s_beat_started = false;
    s_started = false;
}

void dashboard_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_started) return;

    if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        show_page((uint8_t)((s_page + PAGE_COUNT - 1) % PAGE_COUNT));
        return;
    }
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        show_page((uint8_t)((s_page + 1) % PAGE_COUNT));
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        // Acknowledge: stops the pulse and the repeats, keeps the colour.
        dashboard_health_acknowledge(&s_health);
        s_attention_since_ms = 0;
        s_rendered_valid = false;
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        s_dimmed = !s_dimmed;
        const uint8_t target = s_dimmed ? BACKLIGHT_DIM : BACKLIGHT_FULL;
        bsp_display_backlight(target);
        s_backlight_now = target;
        return;
    }
}
