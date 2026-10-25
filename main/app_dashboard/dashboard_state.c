// main/app_dashboard/dashboard_state.c
// Mutex-protected snapshot shared between the NimBLE host task and the LVGL task.
#include "app_dashboard/dashboard_state.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Static allocation only: the target has no PSRAM and a small largest free block.
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static dashboard_snapshot_t s_snapshot;

void dashboard_state_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    dashboard_status_reset(&s_snapshot.status);
    s_snapshot.have_rx = false;
    s_snapshot.connected = false;
    s_snapshot.last_error = DASHBOARD_PARSE_OK;
}

uint64_t dashboard_state_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static bool lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

bool dashboard_state_publish(const char *text, size_t length)
{
    if (!s_lock) return false;

    // Parse outside the lock: the NimBLE task must not hold the snapshot while
    // doing string work, and the parse is bounded (<= 200 bytes).
    dashboard_status_t parsed;
    const dashboard_parse_t result = dashboard_status_parse(text, length, &parsed);
    const uint64_t now = dashboard_state_now_ms();
    if (!lock()) return false;

    bool accepted = false;
    if (result == DASHBOARD_PARSE_OK) {
        s_snapshot.status = parsed;
        s_snapshot.last_rx_ms = now;
        s_snapshot.have_rx = true;
        s_snapshot.accepted_frames++;
        s_snapshot.raw_length =
            (uint16_t)((length > DASHBOARD_STATUS_MAX_BYTES) ? DASHBOARD_STATUS_MAX_BYTES : length);
        memcpy(s_snapshot.raw, text, s_snapshot.raw_length);
        s_snapshot.raw[s_snapshot.raw_length] = '\0';
        accepted = true;
    } else {
        s_snapshot.rejected_frames++;
        s_snapshot.last_error = result;
    }
    unlock();
    return accepted;
}

bool dashboard_state_read(dashboard_snapshot_t *out)
{
    if (!out || !s_lock) return false;
    if (!lock()) return false;
    *out = s_snapshot;
    unlock();
    return true;
}

void dashboard_state_set_connected(bool connected)
{
    if (!s_lock) return;
    if (!lock()) return;
    s_snapshot.connected = connected;
    unlock();
}

size_t dashboard_state_copy_raw(char *destination, size_t capacity)
{
    if (!destination || capacity == 0 || !s_lock) return 0;
    if (!lock()) return 0;
    size_t length = s_snapshot.raw_length;
    if (length > capacity) length = capacity;
    memcpy(destination, s_snapshot.raw, length);
    unlock();
    return length;
}
