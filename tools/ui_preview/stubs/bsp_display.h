// Preview stub: the real header needs esp_lcd_types.h, which is device-only.
#pragma once
#include "esp_err.h"
#define BSP_LVGL_SCREEN_RADIUS 30
void bsp_display_backlight(unsigned char percent);

/* Single-threaded preview: locking is a no-op that always succeeds. */
int bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
