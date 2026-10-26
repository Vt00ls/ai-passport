// main/app_dashboard/dashboard_app.c
// Wires the BLE peripheral and the UI together and owns key routing.
#include "app_dashboard/dashboard_app.h"

#include "app_dashboard/dashboard_ble.h"
#include "app_dashboard/dashboard_state.h"
#include "app_dashboard/dashboard_ui.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "dash_app";

#define LVGL_LOCK_TIMEOUT_MS 500

static bool s_started;

// Log the memory numbers the design document requires: the RAM budget table is an
// estimate, and this is what actually decides whether the UI fits.
static void log_memory(const char *stage)
{
    ESP_LOGI(TAG, "[%s] free heap %u B, largest block %u B", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

esp_err_t dashboard_app_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;

    dashboard_state_init();
    log_memory("boot");

    // BLE first: the UI is useful without it (it shows "no data"), but a UI that
    // never starts would hide the BLE failure, so start the link and report it.
    const esp_err_t ble_error = dashboard_ble_start();
    if (ble_error != ESP_OK) {
        ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(ble_error));
    } else {
        ESP_LOGI(TAG, "BLE advertising; waiting for the host to connect");
    }

    if (!bsp_lvgl_lock(LVGL_LOCK_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "LVGL lock timeout during UI start");
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ui_error = dashboard_ui_start();
    bsp_lvgl_unlock();
    if (ui_error != ESP_OK) {
        ESP_LOGE(TAG, "UI start failed: %s", esp_err_to_name(ui_error));
        return ui_error;
    }

    log_memory("ui started");
    s_started = true;
    ESP_LOGI(TAG, "dashboard ready; UP/DOWN switch pages, OK long-press dims the backlight");
    return ble_error;
}

void dashboard_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_started) return;
    if (!bsp_lvgl_lock(LVGL_LOCK_TIMEOUT_MS)) return;
    dashboard_ui_key(btn, ev);
    bsp_lvgl_unlock();
}
