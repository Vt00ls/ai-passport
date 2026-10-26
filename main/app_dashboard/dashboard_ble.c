// main/app_dashboard/dashboard_ble.c
// NimBLE peripheral for the node-monitor dashboard.
//
// Threading contract: every callback here runs on the NimBLE host task. Nothing
// in this file touches LVGL. A write only parses into the shared snapshot, which
// the UI task samples later.
#include "app_dashboard/dashboard_ble.h"

#include <string.h>

#include "app_dashboard/dashboard_protocol.h"
#include "app_dashboard/dashboard_state.h"
#include "demo_radio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "dash_ble";

#define DASHBOARD_DEVICE_NAME "PassportDash"

// Kept below the 31-byte legacy advertising limit: the name goes in the
// advertising payload and the 128-bit service UUID in the scan response.
#define DASHBOARD_NAME_MAX 16

// Preferred ATT MTU. The status frame is up to 200 bytes, so the negotiated MTU
// must be at least 203 to carry it in one write. NimBLE's default is 256; we set
// it explicitly so the assumption is visible rather than incidental.
#define DASHBOARD_PREFERRED_MTU 256

// 1f7a0001-6b3c-4a1e-9c2d-5e8f0a9b4c10, least-significant byte first.
static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x10, 0x4c, 0x9b, 0x0a, 0x8f, 0x5e, 0x2d, 0x9c,
    0x1e, 0x4a, 0x3c, 0x6b, 0x01, 0x00, 0x7a, 0x1f);

// 1f7a0002-6b3c-4a1e-9c2d-5e8f0a9b4c10, least-significant byte first.
static const ble_uuid128_t s_status_uuid = BLE_UUID128_INIT(
    0x10, 0x4c, 0x9b, 0x0a, 0x8f, 0x5e, 0x2d, 0x9c,
    0x1e, 0x4a, 0x3c, 0x6b, 0x02, 0x00, 0x7a, 0x1f);

static volatile dashboard_ble_state_t s_state = DASHBOARD_BLE_OFF;
static volatile int s_last_error;
static volatile uint16_t s_mtu;
static uint8_t s_addr_type;
static bool s_initialized;
static SemaphoreHandle_t s_host_stopped;
static TaskHandle_t s_host_task;
// True while we believe the controller is advertising. NimBLE stops advertising on
// connect and on a failed connection attempt, and the restart can fail silently, so
// a watchdog below re-arms it.
static volatile bool s_adv_active;
static esp_timer_handle_t s_adv_watchdog;

// Single connection (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1), so one scratch buffer is
// enough and it never races: only the NimBLE host task runs the write path.
static char s_write_scratch[DASHBOARD_STATUS_MAX_BYTES];

static int gap_event(struct ble_gap_event *event, void *arg);

static int begin_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DASHBOARD_DEVICE_NAME;
    fields.name_len = (uint8_t)strlen(DASHBOARD_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return rc;
    }

    // The 128-bit service UUID does not fit next to the name, so it rides in the
    // scan response; hosts that filter on the service UUID still find the badge.
    struct ble_hs_adv_fields response;
    memset(&response, 0, sizeof(response));
    response.uuids128 = &s_service_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response fields failed: %d", rc);
        return rc;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == BLE_HS_EALREADY) {
        // Already advertising: on real hardware this happened after a disconnect and
        // was treated as a failure, which left the badge invisible to every scanner.
        s_adv_active = true;
        return 0;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
        s_adv_active = false;
        return rc;
    }
    s_adv_active = true;
    s_state = DASHBOARD_BLE_ADVERTISING;
    ESP_LOGI(TAG, "advertising as %s", DASHBOARD_DEVICE_NAME);
    return 0;
}

// Safety net: a headless badge that stops advertising is unreachable until someone
// power-cycles it, so re-arm advertising whenever it should be up but is not.
static void adv_watchdog(void *arg)
{
    (void)arg;
    if (!s_initialized || s_state == DASHBOARD_BLE_CONNECTED || s_adv_active) return;
    ESP_LOGW(TAG, "advertising is down; re-arming");
    (void)begin_advertising();
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (length == 0) {
            ESP_LOGW(TAG, "empty write rejected");
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (length > DASHBOARD_STATUS_MAX_BYTES) {
            ESP_LOGW(TAG, "write of %u bytes exceeds %u; rejected", (unsigned)length,
                     (unsigned)DASHBOARD_STATUS_MAX_BYTES);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint16_t copied = 0;
        const int rc = ble_hs_mbuf_to_flat(ctxt->om, s_write_scratch, sizeof(s_write_scratch),
                                           &copied);
        if (rc != 0) {
            ESP_LOGW(TAG, "mbuf flatten failed: %d", rc);
            return BLE_ATT_ERR_UNLIKELY;
        }
        // A rejected frame is not an ATT error: the central did reach us, and
        // retrying the same malformed payload would not help. Report it in the
        // snapshot so the UI can show why the data stopped moving.
        if (!dashboard_state_publish(s_write_scratch, copied)) {
            ESP_LOGW(TAG, "status frame rejected");
        }
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        size_t length = dashboard_state_copy_raw(s_write_scratch, sizeof(s_write_scratch));
        if (length == 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
        return os_mbuf_append(ctxt->om, s_write_scratch, (uint16_t)length) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_mtu = ble_att_mtu(event->connect.conn_handle);
            s_adv_active = false;
            s_state = DASHBOARD_BLE_CONNECTED;
            dashboard_state_set_connected(true);
            // At connection time the MTU is always the BLE minimum (23); the exchange
            // that actually sizes it arrives as BLE_GAP_EVENT_MTU a few tens of
            // milliseconds later. Judging the MTU here produced a false alarm on real
            // hardware, so the check lives in the MTU event instead.
            ESP_LOGI(TAG, "connected; initial ATT MTU %u (exchange follows)",
                     (unsigned)s_mtu);
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            s_adv_active = false;
            (void)begin_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected; reason %d", event->disconnect.reason);
        dashboard_state_set_connected(false);
        // Keep advertising so a host reboot reconnects without user action.
        s_adv_active = false;
        (void)begin_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_active = false;
        (void)begin_advertising();
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU updated to %u (channel %d)", (unsigned)event->mtu.value,
                 event->mtu.channel_id);
        if (s_mtu < DASHBOARD_STATUS_MAX_BYTES + 3) {
            ESP_LOGW(TAG, "MTU %u is below %u: the status frame needs splitting",
                     (unsigned)s_mtu, (unsigned)(DASHBOARD_STATUS_MAX_BYTES + 3));
        }
        break;
    default:
        break;
    }
    return 0;
}

static void on_reset(int reason)
{
    s_last_error = reason;
    s_state = DASHBOARD_BLE_FAILED;
    ESP_LOGE(TAG, "host reset: %d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) rc = begin_advertising();
    if (rc != 0) {
        s_last_error = rc;
        s_state = DASHBOARD_BLE_FAILED;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    xSemaphoreGive(s_host_stopped);
    for (;;) vTaskSuspend(NULL);
}

esp_err_t dashboard_ble_start(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    dashboard_state_init();
    s_state = DASHBOARD_BLE_STARTING;

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) {
        s_last_error = err;
        s_state = DASHBOARD_BLE_FAILED;
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        s_last_error = err;
        s_state = DASHBOARD_BLE_FAILED;
        return err;
    }
    s_initialized = true;

    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) {
        s_last_error = ESP_ERR_NO_MEM;
        s_state = DASHBOARD_BLE_FAILED;
        (void)nimble_port_deinit();
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    const int rc = ble_svc_gap_device_name_set(DASHBOARD_DEVICE_NAME);
    if (rc != 0) {
        s_last_error = rc;
        s_state = DASHBOARD_BLE_FAILED;
        return ESP_FAIL;
    }

    if (ble_gatts_count_cfg(s_services) != 0 || ble_gatts_add_svcs(s_services) != 0) {
        s_last_error = ESP_FAIL;
        s_state = DASHBOARD_BLE_FAILED;
        return ESP_FAIL;
    }

    // Request the larger MTU before any central connects.
    ble_att_set_preferred_mtu(DASHBOARD_PREFERRED_MTU);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    const esp_timer_create_args_t timer_args = {
        .callback = adv_watchdog,
        .name = "adv_wd",
    };
    if (esp_timer_create(&timer_args, &s_adv_watchdog) == ESP_OK) {
        (void)esp_timer_start_periodic(s_adv_watchdog, 1000000);  // 1 s
    } else {
        ESP_LOGW(TAG, "advertising watchdog unavailable");
    }

    if (xTaskCreate(host_task, "dash_nimble", NIMBLE_HS_STACK_SIZE, NULL,
                    configMAX_PRIORITIES - 4, &s_host_task) != pdPASS) {
        s_last_error = ESP_ERR_NO_MEM;
        s_state = DASHBOARD_BLE_FAILED;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

dashboard_ble_state_t dashboard_ble_state(void)
{
    return s_state;
}

int dashboard_ble_last_error(void)
{
    return s_last_error;
}

uint16_t dashboard_ble_mtu(void)
{
    return s_mtu;
}
