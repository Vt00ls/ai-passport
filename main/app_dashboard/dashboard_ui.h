// main/app_dashboard/dashboard_ui.h
// The dashboard's own three-page UI. This deliberately does not reuse the
// baseline demo menu, ui_pixel shell, or its visual style: a derivative
// application must ship its own screens and navigation.
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdbool.h>

// Create the three pages and the refresh timer. Requires display + LVGL to be up.
esp_err_t dashboard_ui_start(void);

// Delete every object this module created. Safe to call when not started.
void dashboard_ui_stop(void);

// Handle one key event. The caller must already hold bsp_lvgl_lock(); this runs
// on the input task, not the LVGL task.
void dashboard_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);
