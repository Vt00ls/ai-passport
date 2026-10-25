// main/app_dashboard/dashboard_app.h
// Application lifecycle for the node-monitor dashboard: start BLE + UI and route
// keys. Replaces the baseline demo menu as the product boot path.
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// Start the dashboard. Call after display, LVGL and the input task are ready.
// The caller must NOT hold bsp_lvgl_lock(): this function takes it itself.
esp_err_t dashboard_app_start(void);

// Route one key event from the input task. Takes the LVGL lock internally, so the
// caller must not already hold it.
void dashboard_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
