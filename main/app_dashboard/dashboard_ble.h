// main/app_dashboard/dashboard_ble.h
// BLE peripheral: one GATT service with a single writable status characteristic.
// The central (dev-forge) connects and writes one status frame every few seconds.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DASHBOARD_BLE_OFF = 0,
    DASHBOARD_BLE_STARTING,
    DASHBOARD_BLE_ADVERTISING,
    DASHBOARD_BLE_CONNECTED,
    DASHBOARD_BLE_FAILED,
} dashboard_ble_state_t;

// Initialize NimBLE, register the GATT service and start advertising.
// Idempotent: a second call returns ESP_ERR_INVALID_STATE without leaking.
esp_err_t dashboard_ble_start(void);

dashboard_ble_state_t dashboard_ble_state(void);

// Last failure code (NimBLE rc or esp_err_t), 0 when nothing has failed.
int dashboard_ble_last_error(void);

// ATT MTU negotiated with the current/last central. 0 until a connection exists.
// The status frame needs >= 203 bytes to travel in a single write.
uint16_t dashboard_ble_mtu(void);
