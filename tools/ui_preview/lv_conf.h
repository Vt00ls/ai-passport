/* Host-side LVGL config for the dashboard layout preview.
 * Mirrors the firmware settings that affect layout: 16-bit colour, the same two
 * Montserrat fonts, and the same refresh period. Everything else uses LVGL's
 * defaults, which is enough for labels and plain objects. */
#pragma once

#define LV_COLOR_DEPTH 16
#define LV_DEF_REFR_PERIOD 20
#define LV_DPI_DEF 130

#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

/* Same fonts the firmware enables in sdkconfig.defaults. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_THEME_DEFAULT 1
#define LV_USE_LABEL 1
#define LV_USE_SNAPSHOT 0

#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_48 1
