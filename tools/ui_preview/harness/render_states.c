// Host render harness for the real main/app_dashboard/dashboard_ui.c.
//
// Only device dependencies are stubbed: the snapshot source, the battery gauge, the
// backlight and the LVGL lock. The page code, the verdict state machine and the
// formatters are the firmware's own, so these images show what the badge draws.
//
// Time is advanced one second at a time and the UI's own 200 ms timer is pumped, so
// dwell, escalation and hysteresis run exactly as on the device. See README.md.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_dashboard/dashboard_state.h"
#include "app_dashboard/dashboard_ui.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

#define WIDTH 240
#define HEIGHT 320

static uint16_t s_fb[WIDTH * HEIGHT];
static uint64_t s_now_ms;
static dashboard_snapshot_t s_snapshot;
static int s_backlight = 100;

const char *esp_err_to_name(esp_err_t code) { (void)code; return "stub"; }

static int s_battery = 87;
int bsp_battery_soc(void) { return s_battery; }
int bsp_battery_mv(void) { return 3900; }
esp_err_t bsp_battery_init(void) { return ESP_OK; }
void bsp_display_backlight(unsigned char percent) { s_backlight = percent; }
int bsp_lvgl_lock(int timeout_ms) { (void)timeout_ms; return 1; }
void bsp_lvgl_unlock(void) {}

uint64_t dashboard_state_now_ms(void) { return s_now_ms; }
void dashboard_state_init(void) { memset(&s_snapshot, 0, sizeof(s_snapshot)); }
bool dashboard_state_read(dashboard_snapshot_t *out)
{
    if (!out) return false;
    *out = s_snapshot;
    return true;
}

static uint32_t tick_cb(void) { return (uint32_t)s_now_ms; }

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *src = (const uint16_t *)px_map;
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int dx = area->x1 + x, dy = area->y1 + y;
            if (dx >= 0 && dx < WIDTH && dy >= 0 && dy < HEIGHT) {
                s_fb[dy * WIDTH + dx] = src[y * w + x];
            }
        }
    }
    lv_display_flush_ready(display);
}

static void write_bmp(const char *path)
{
    const int row = WIDTH * 3;
    const int pad = (4 - (row % 4)) % 4;
    const int img = (row + pad) * HEIGHT;
    unsigned char h[54];
    memset(h, 0, sizeof(h));
    h[0] = 'B'; h[1] = 'M';
    h[2] = (unsigned char)(54 + img); h[3] = (unsigned char)((54 + img) >> 8);
    h[4] = (unsigned char)((54 + img) >> 16); h[5] = (unsigned char)((54 + img) >> 24);
    h[10] = 54; h[14] = 40;
    h[18] = (unsigned char)WIDTH; h[19] = (unsigned char)(WIDTH >> 8);
    h[22] = (unsigned char)HEIGHT; h[23] = (unsigned char)(HEIGHT >> 8);
    h[26] = 1; h[28] = 24;
    h[34] = (unsigned char)img; h[35] = (unsigned char)(img >> 8);
    h[36] = (unsigned char)(img >> 16); h[37] = (unsigned char)(img >> 24);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(h, 1, sizeof(h), f);
    unsigned char line[WIDTH * 3 + 4];
    for (int y = HEIGHT - 1; y >= 0; y--) {
        for (int x = 0; x < WIDTH; x++) {
            const uint16_t p = s_fb[y * WIDTH + x];
            line[x * 3 + 0] = (unsigned char)((p & 0x1F) * 255 / 31);
            line[x * 3 + 1] = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
            line[x * 3 + 2] = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
        }
        memset(line + row, 0, (size_t)pad);
        fwrite(line, 1, (size_t)(row + pad), f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

static void set_healthy_frame(uint32_t disk, uint32_t mem)
{
    dashboard_status_t *s = &s_snapshot.status;
    dashboard_status_reset(s);
    s->version = 1;
    s->present = 0;
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_VERSION);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_TIME);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_HOST);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_CORES);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_UPTIME);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_LOAD);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_CPU);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_MEM);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_DISK);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_TEMP);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_NET);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_AGENTS);
    s->present |= DASHBOARD_FIELD_BIT(DASHBOARD_FIELD_AGENT_IDLE);
    snprintf(s->host, sizeof(s->host), "Ubuntu-Box-Dev");
    s->cores = 4;
    s->uptime_s = 128099u;
    s->load_x100 = 42;
    s->cpu_percent = 7;
    s->mem_percent = (uint8_t)mem;
    s->disk_percent = (uint8_t)disk;
    s->temp_c = 51;
    s->net_up = true;
    snprintf(s->net_iface, sizeof(s->net_iface), "eno1");
    snprintf(s->net_ip, sizeof(s->net_ip), "10.2.65.61");
    s->agents_active = 2;
    s->agent_idle_s = 5;
    s_snapshot.have_rx = true;
    s_snapshot.connected = true;
    s_snapshot.accepted_frames++;
    s_snapshot.last_rx_ms = s_now_ms;
}

// Step time forward one second at a time, pumping the UI's own 200 ms timer, so
// dwell and escalation are exercised exactly as on the device.
static void pump(int seconds, bool keep_frames_arriving)
{
    for (int i = 0; i < seconds; i++) {
        s_now_ms += 1000;
        if (keep_frames_arriving && s_snapshot.connected) {
            s_snapshot.accepted_frames++;
            s_snapshot.last_rx_ms = s_now_ms;
        }
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}

static void press(bsp_btn_t button)
{
    dashboard_ui_key(button, BSP_BTN_CLICK);
    lv_refr_now(NULL);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    dashboard_state_init();
    s_now_ms = 1000000u;
    if (dashboard_ui_start() != ESP_OK) {
        fprintf(stderr, "ui start failed\n");
        return 1;
    }

    // 1) Healthy: the panel is quiet.
    set_healthy_frame(33, 38);
    pump(10, true);
    write_bmp("live-1-status-ok.bmp");

    press(BSP_BTN_DOWN);
    write_bmp("live-2-detail.bmp");
    press(BSP_BTN_DOWN);
    write_bmp("live-3-agents.bmp");

    // 2) Disk reaches the CRIT band; hold past the 10 s dwell.
    press(BSP_BTN_DOWN);  // wraps back to STATUS
    set_healthy_frame(95, 38);
    pump(12, true);
    write_bmp("live-4-status-crit-disk.bmp");

    // 2b) Host reboots: uptime goes backwards, watchdog notice appears.
    set_healthy_frame(33, 38);
    pump(5, true);
    s_snapshot.status.uptime_s = 4;  // fresh boot
    s_snapshot.accepted_frames++;
    s_snapshot.last_rx_ms = s_now_ms;
    pump(2, true);
    write_bmp("live-4b-status-reboot.bmp");

    // 3) Host goes quiet while the link stays up: SILENT after 60 s.
    s_snapshot.status.disk_percent = 33;
    pump(70, false);
    write_bmp("live-5-status-silent.bmp");

    // 4) Link drops: grey for 20 s, amber to 2 min, red after.
    s_snapshot.connected = false;
    pump(21, false);
    write_bmp("live-6-status-nolink-warn.bmp");
    pump(100, false);
    write_bmp("live-7-status-nolink-crit.bmp");

    // Dropping into the WARN band (a fall, so it is not mistaken for charging):
    // the card must stay calm and only the battery text goes amber.
    set_healthy_frame(33, 38);
    s_battery = 25;
    pump(4, true);
    s_battery = 18;
    pump(4, true);
    write_bmp("live-8b-battery-warn.bmp");

    // Below the CRIT threshold, held past the 30 s dwell: LOW BATT takes over.
    s_battery = 8;
    pump(35, true);
    write_bmp("live-8-battery-crit.bmp");

    // A rising reading is USB power: the alert must go quiet again.
    s_battery = 12;
    pump(3, true);
    s_battery = 18;
    pump(3, true);
    write_bmp("live-9-battery-charging.bmp");

    printf("backlight at exit: %d\n", s_backlight);
    dashboard_ui_stop();
    return 0;
}
