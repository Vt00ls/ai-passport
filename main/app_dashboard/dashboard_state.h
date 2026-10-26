// main/app_dashboard/dashboard_state.h
// Single shared snapshot between the NimBLE host task (writer) and the LVGL task
// (reader). The BLE write callback must never touch LVGL, so it only publishes
// here; the UI timer samples with dashboard_state_read().
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_dashboard/dashboard_protocol.h"

typedef struct {
    dashboard_status_t status;      // last accepted frame ("no data" when !have_rx)
    char raw[DASHBOARD_STATUS_MAX_BYTES + 1];  // last accepted payload, for GATT READ
    uint16_t raw_length;
    uint64_t last_rx_ms;            // badge monotonic ms at the last accepted frame
    bool have_rx;                   // a frame has been accepted since boot
    bool connected;                 // a central is currently connected
    uint32_t accepted_frames;
    uint32_t rejected_frames;
    dashboard_parse_t last_error;   // why the most recent rejected frame failed
} dashboard_snapshot_t;

// Allocate the static lock. Safe to call more than once.
void dashboard_state_init(void);

// Badge monotonic clock in milliseconds (no wall clock on this board).
uint64_t dashboard_state_now_ms(void);

// Parse and store one frame from the BLE write callback.
// Returns false when the frame is rejected; the snapshot then keeps the previous
// values and only increments rejected_frames, so a corrupt frame can never show
// stale data as if it were fresh.
bool dashboard_state_publish(const char *text, size_t length);

// Copy the snapshot for rendering. Returns false before dashboard_state_init().
bool dashboard_state_read(dashboard_snapshot_t *out);

// Mark the link as up/down. A disconnect keeps the heartbeat timestamp so the
// age keeps growing across a host reboot.
void dashboard_state_set_connected(bool connected);

// Copy the last accepted payload for the GATT READ characteristic.
// Returns the copied length, or 0 when nothing has been accepted yet.
size_t dashboard_state_copy_raw(char *destination, size_t capacity);
