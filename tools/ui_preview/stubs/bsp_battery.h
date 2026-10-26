// Preview stub: the real header pulls in esp_err.h only, but we keep this small.
#pragma once
#include "esp_err.h"
int bsp_battery_soc(void);
int bsp_battery_mv(void);
esp_err_t bsp_battery_init(void);
