// main/app_dashboard/dashboard_ui.c
// Three pages (host health / agents / network) plus shared chrome.
//
// Rendering rules that the acceptance criteria depend on:
//   - a field absent from the last frame renders "--", never 0;
//   - when the link is lost or the central disconnects, values render "--" while
//     the heartbeat age keeps counting, so a frozen host stays visible;
//   - a value is only rewritten when it actually changes, to keep SPI traffic and
//     CPU cost low on a board with no PSRAM.
#include "app_dashboard/dashboard_ui.h"

#include <stdio.h>
#include <string.h>

#include "app_dashboard/dashboard_ble.h"
#include "app_dashboard/dashboard_format.h"
#include "app_dashboard/dashboard_protocol.h"
#include "app_dashboard/dashboard_state.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

#define PAGE_COUNT 3
#define PAGE_HOST 0
#define PAGE_AGENTS 1
#define PAGE_NETWORK 2

// Dark flat panel: intentionally different from the demo's light pixel-art shell.
#define COL_BG lv_color_hex(0x0B0F14)
#define COL_TEXT lv_color_hex(0xE6EDF3)
#define COL_MUTED lv_color_hex(0x6E7B8B)
#define COL_DIM lv_color_hex(0x4A5563)
#define COL_OK lv_color_hex(0x3FB950)
#define COL_WARN lv_color_hex(0xD29922)
#define COL_ERROR lv_color_hex(0xF85149)
#define COL_ACCENT lv_color_hex(0x58A6FF)

#define REFRESH_PERIOD_MS 200
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 20

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *dot;
    lv_obj_t *age;
    lv_obj_t *battery;
} page_chrome_t;

static page_chrome_t s_chrome[PAGE_COUNT];
// Each page renders its own indicator row, so the dots must be tracked per page:
// a single flat array would leave pages 1 and 2 always highlighting page 0.
static lv_obj_t *s_dots[PAGE_COUNT][PAGE_COUNT];
static lv_timer_t *s_timer;

// Host page
static lv_obj_t *s_host_name;
static lv_obj_t *s_host_uptime;
static lv_obj_t *s_host_load;
static lv_obj_t *s_host_cpu;
static lv_obj_t *s_host_mem;
static lv_obj_t *s_host_dsk;
static lv_obj_t *s_host_temp;

// Agents page
static lv_obj_t *s_agent_active;
static lv_obj_t *s_agent_last;
static lv_obj_t *s_agent_hint;

// Network page
static lv_obj_t *s_net_iface;
static lv_obj_t *s_net_state;
static lv_obj_t *s_net_ip;

static uint8_t s_page;
static bool s_started;
static bool s_dimmed;

// Render cache so unchanged values are not rewritten every tick.
static uint32_t s_rendered_frames;
static bool s_rendered_connected;
static bool s_rendered_have_rx;
static uint32_t s_rendered_age_s;
static bool s_rendered_never;
static bool s_rendered_valid;

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

static void build_chrome(int page, const char *title)
{
    page_chrome_t *chrome = &s_chrome[page];
    chrome->screen = make_screen();
    if (!chrome->screen) return;

    // Link dot: colour carries the link state, so it reads at a glance.
    chrome->dot = lv_obj_create(chrome->screen);
    lv_obj_set_size(chrome->dot, 10, 10);
    lv_obj_set_pos(chrome->dot, 14, 17);
    lv_obj_set_style_radius(chrome->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(chrome->dot, 0, 0);
    lv_obj_set_style_bg_color(chrome->dot, COL_DIM, 0);
    lv_obj_set_style_bg_opa(chrome->dot, LV_OPA_COVER, 0);

    chrome->age = make_label(chrome->screen, &lv_font_montserrat_14, COL_MUTED, 32, 12);
    chrome->battery = make_label(chrome->screen, &lv_font_montserrat_14, COL_MUTED, 0, 12);
    lv_obj_align(chrome->battery, LV_ALIGN_TOP_RIGHT, -14, 12);

    lv_obj_t *title_label = make_label(chrome->screen, &lv_font_montserrat_20, COL_ACCENT, 14, 38);
    lv_label_set_text(title_label, title);

    // Page indicator.
    const int dot_spacing = 16;
    const int total = (PAGE_COUNT - 1) * dot_spacing;
    for (int index = 0; index < PAGE_COUNT; index++) {
        lv_obj_t *indicator = lv_obj_create(chrome->screen);
        lv_obj_set_size(indicator, 7, 7);
        lv_obj_set_pos(indicator, 120 - total / 2 + index * dot_spacing, 300);
        lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(indicator, 0, 0);
        lv_obj_set_style_bg_color(indicator, page == index ? COL_ACCENT : COL_DIM, 0);
        lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
        s_dots[page][index] = indicator;
    }
}

static void build_section(lv_obj_t *screen, const char *caption, int x, int y,
                          lv_obj_t **value_label)
{
    lv_obj_t *label = make_label(screen, &lv_font_montserrat_14, COL_MUTED, x, y);
    lv_label_set_text(label, caption);
    *value_label = make_label(screen, &lv_font_montserrat_20, COL_TEXT, x, y + 18);
}

static void build_host_page(void)
{
    build_chrome(PAGE_HOST, "HOST");
    lv_obj_t *screen = s_chrome[PAGE_HOST].screen;
    if (!screen) return;

    s_host_name = make_label(screen, &lv_font_montserrat_14, COL_MUTED, 14, 70);

    build_section(screen, "UPTIME", 14, 96, &s_host_uptime);
    build_section(screen, "LOAD", 14, 146, &s_host_load);
    build_section(screen, "CPU", 122, 146, &s_host_cpu);
    build_section(screen, "MEM", 14, 196, &s_host_mem);
    build_section(screen, "DISK", 122, 196, &s_host_dsk);
    build_section(screen, "TEMP", 14, 246, &s_host_temp);
}

static void build_agents_page(void)
{
    build_chrome(PAGE_AGENTS, "AGENTS");
    lv_obj_t *screen = s_chrome[PAGE_AGENTS].screen;
    if (!screen) return;

    build_section(screen, "ACTIVE SESSIONS", 14, 84, &s_agent_active);
    build_section(screen, "LAST ACTIVITY", 14, 146, &s_agent_last);
    s_agent_hint = make_label(screen, &lv_font_montserrat_14, COL_DIM, 14, 250);
    lv_label_set_text(s_agent_hint, "");
}

static void build_network_page(void)
{
    build_chrome(PAGE_NETWORK, "NETWORK");
    lv_obj_t *screen = s_chrome[PAGE_NETWORK].screen;
    if (!screen) return;

    build_section(screen, "INTERFACE", 14, 84, &s_net_iface);
    build_section(screen, "STATE", 14, 146, &s_net_state);
    build_section(screen, "ADDRESS", 14, 196, &s_net_ip);
}

static void set_label(lv_obj_t *label, const char *text, lv_color_t color)
{
    if (!label) return;
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
}

static lv_color_t link_color(dashboard_link_t link)
{
    switch (link) {
    case DASHBOARD_LINK_LIVE:
        return COL_OK;
    case DASHBOARD_LINK_STALE:
        return COL_WARN;
    case DASHBOARD_LINK_LOST:
        return COL_ERROR;
    case DASHBOARD_LINK_NONE:
    default:
        return COL_DIM;
    }
}

static void render(const dashboard_snapshot_t *snapshot)
{
    const uint64_t now_ms = dashboard_state_now_ms();
    const dashboard_age_t age =
        dashboard_heartbeat_age(now_ms, snapshot->last_rx_ms, snapshot->have_rx);

    // A disconnect is treated as "lost" immediately: waiting for the 120 s
    // threshold would keep showing stale values as if they were current.
    dashboard_link_t link = dashboard_link_state(snapshot->have_rx, age.age_s);
    if (!snapshot->connected && link != DASHBOARD_LINK_NONE) {
        link = DASHBOARD_LINK_LOST;
    }
    const bool show_values =
        snapshot->have_rx && (link == DASHBOARD_LINK_LIVE || link == DASHBOARD_LINK_STALE);
    const lv_color_t value_color = (link == DASHBOARD_LINK_STALE) ? COL_MUTED : COL_TEXT;

    const bool values_changed = !s_rendered_valid ||
                                s_rendered_frames != snapshot->accepted_frames ||
                                s_rendered_connected != snapshot->connected ||
                                s_rendered_have_rx != snapshot->have_rx;

    if (values_changed) {
        char buffer[32];
        const dashboard_status_t *status = &snapshot->status;

        if (show_values) {
            dashboard_format_text(buffer, sizeof(buffer),
                                  dashboard_status_has(status, DASHBOARD_FIELD_HOST),
                                  status->host);
            set_label(s_host_name, buffer, COL_MUTED);
        } else {
            set_label(s_host_name, "", COL_MUTED);
        }

        // Host page.
        dashboard_format_uptime(buffer, sizeof(buffer), status->uptime_s);
        set_label(s_host_uptime,
                  show_values && dashboard_status_has(status, DASHBOARD_FIELD_UPTIME) ? buffer
                                                                                     : "--",
                  value_color);
        dashboard_format_load(buffer, sizeof(buffer), status->load_x100);
        set_label(s_host_load,
                  show_values && dashboard_status_has(status, DASHBOARD_FIELD_LOAD) ? buffer
                                                                                   : "--",
                  value_color);
        dashboard_format_percent(buffer, sizeof(buffer),
                                 show_values && dashboard_status_has(status, DASHBOARD_FIELD_CPU),
                                 status->cpu_percent);
        set_label(s_host_cpu, buffer, value_color);
        dashboard_format_percent(buffer, sizeof(buffer),
                                 show_values && dashboard_status_has(status, DASHBOARD_FIELD_MEM),
                                 status->mem_percent);
        set_label(s_host_mem, buffer, value_color);
        dashboard_format_percent(buffer, sizeof(buffer),
                                 show_values && dashboard_status_has(status, DASHBOARD_FIELD_DISK),
                                 status->disk_percent);
        set_label(s_host_dsk, buffer, value_color);
        dashboard_format_temperature(buffer, sizeof(buffer),
                                     show_values && dashboard_status_has(status, DASHBOARD_FIELD_TEMP),
                                     status->temp_c);
        set_label(s_host_temp, buffer, value_color);

        // Agents page. The count is clamped so the label cannot grow unbounded
        // on a wildly out-of-range value.
        if (show_values && dashboard_status_has(status, DASHBOARD_FIELD_AGENTS)) {
            snprintf(buffer, sizeof(buffer), "%u",
                     (unsigned)(status->agents_active > 999u ? 999u : status->agents_active));
        } else {
            snprintf(buffer, sizeof(buffer), "--");
        }
        set_label(s_agent_active, buffer, value_color);
        if (show_values && dashboard_status_has(status, DASHBOARD_FIELD_AGENT_IDLE)) {
            if (status->agent_idle_s < 0) {
                dashboard_format_text(buffer, sizeof(buffer), false, NULL);
            } else {
                dashboard_format_age(buffer, sizeof(buffer), (uint32_t)status->agent_idle_s, false);
            }
        } else {
            dashboard_format_text(buffer, sizeof(buffer), false, NULL);
        }
        set_label(s_agent_last, buffer, value_color);
        set_label(s_agent_hint, show_values ? "" : "waiting for host", COL_DIM);

        // Network page.
        dashboard_format_text(buffer, sizeof(buffer),
                             show_values && dashboard_status_has(status, DASHBOARD_FIELD_NET),
                             status->net_iface);
        set_label(s_net_iface, buffer, value_color);
        set_label(s_net_state,
                  !show_values || !dashboard_status_has(status, DASHBOARD_FIELD_NET)
                      ? "--"
                      : (status->net_up ? "ONLINE" : "OFFLINE"),
                  !show_values || !dashboard_status_has(status, DASHBOARD_FIELD_NET)
                      ? value_color
                      : (status->net_up ? COL_OK : COL_ERROR));
        dashboard_format_text(buffer, sizeof(buffer),
                             show_values && dashboard_status_has(status, DASHBOARD_FIELD_NET),
                             status->net_ip);
        set_label(s_net_ip, buffer, value_color);

        s_rendered_frames = snapshot->accepted_frames;
        s_rendered_connected = snapshot->connected;
        s_rendered_have_rx = snapshot->have_rx;
        s_rendered_valid = true;
        s_rendered_age_s = UINT32_MAX;  // force the age line to refresh below
    }

    // The age changes every second, so only rewrite it when the second changes.
    if (s_rendered_age_s != age.age_s || s_rendered_never != age.never_received) {
        char buffer[24];
        dashboard_format_age(buffer, sizeof(buffer), age.age_s, age.never_received);
        for (int page = 0; page < PAGE_COUNT; page++) {
            if (!s_chrome[page].age) continue;
            lv_label_set_text(s_chrome[page].age, buffer);
            lv_obj_set_style_text_color(s_chrome[page].age, link_color(link), 0);
        }
        s_rendered_age_s = age.age_s;
        s_rendered_never = age.never_received;
    }

    // Battery is read from the gauge, not from the host frame. Clamped to 0..100
    // so the formatted text always fits the buffer.
    char battery_text[8];
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        battery_text[0] = '\0';
    } else {
        const unsigned percent = (unsigned)(soc > 100 ? 100 : soc);
        snprintf(battery_text, sizeof(battery_text), "%u%%", percent);
    }
    for (int page = 0; page < PAGE_COUNT; page++) {
        if (!s_chrome[page].battery) continue;
        lv_label_set_text(s_chrome[page].battery, battery_text);
    }

    for (int page = 0; page < PAGE_COUNT; page++) {
        if (!s_chrome[page].dot) continue;
        lv_obj_set_style_bg_color(s_chrome[page].dot, link_color(link), 0);
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    dashboard_snapshot_t snapshot;
    if (!dashboard_state_read(&snapshot)) return;
    render(&snapshot);
}

static void show_page(uint8_t page)
{
    s_page = page;
    if (s_chrome[page].screen) {
        lv_screen_load(s_chrome[page].screen);
    }
    for (int index = 0; index < PAGE_COUNT; index++) {
        if (!s_dots[page][index]) continue;
        lv_obj_set_style_bg_color(s_dots[page][index], index == page ? COL_ACCENT : COL_DIM, 0);
    }
    s_rendered_valid = false;  // the newly shown page needs a full rewrite
}

esp_err_t dashboard_ui_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;

    dashboard_state_init();
    build_host_page();
    build_agents_page();
    build_network_page();
    if (!s_chrome[PAGE_HOST].screen || !s_chrome[PAGE_AGENTS].screen ||
        !s_chrome[PAGE_NETWORK].screen) {
        dashboard_ui_stop();
        return ESP_ERR_NO_MEM;
    }

    show_page(PAGE_HOST);
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
        if (s_chrome[page].screen) {
            lv_obj_delete(s_chrome[page].screen);
            s_chrome[page].screen = NULL;
        }
        s_chrome[page].dot = NULL;
        s_chrome[page].age = NULL;
        s_chrome[page].battery = NULL;
        for (int index = 0; index < PAGE_COUNT; index++) {
            s_dots[page][index] = NULL;
        }
    }
    s_host_name = s_host_uptime = s_host_load = s_host_cpu = NULL;
    s_host_mem = s_host_dsk = s_host_temp = NULL;
    s_agent_active = s_agent_last = s_agent_hint = NULL;
    s_net_iface = s_net_state = s_net_ip = NULL;
    s_rendered_valid = false;
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
        s_rendered_valid = false;  // force a redraw even if nothing changed
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        s_dimmed = !s_dimmed;
        bsp_display_backlight(s_dimmed ? BACKLIGHT_DIM : BACKLIGHT_FULL);
        return;
    }
}
