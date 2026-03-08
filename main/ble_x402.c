#include "ble_x402.h"

#ifdef CONFIG_BT_ENABLED

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_x402";

/* x402 Service UUID: 00004020-0000-1000-8000-00805f9b34fb */
static const ble_uuid16_t x402_svc_uuid = BLE_UUID16_INIT(0x4020);
static const ble_uuid16_t offer_chr_uuid = BLE_UUID16_INIT(0x4021);
static const ble_uuid16_t stats_chr_uuid = BLE_UUID16_INIT(0x4022);
static const ble_uuid16_t payment_chr_uuid = BLE_UUID16_INIT(0x4023);
static const ble_uuid16_t receipt_chr_uuid = BLE_UUID16_INIT(0x4024);

static GlobalState *g_state = NULL;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t receipt_val_handle;

/* Buffers for characteristic values */
static char offer_json[256];
static char stats_json[256];
static char payment_buf[1024];
static int payment_len = 0;

static void ble_x402_build_offer(void) {
    if (!g_state) return;
    snprintf(offer_json, sizeof(offer_json),
        "{\"amount\":100000,\"network\":\"eip155:84532\","
        "\"asset\":\"0x036CbD53842c5426634e7929541eC2318f3dCF7e\","
        "\"payTo\":\"%s\",\"duration\":3600}",
        g_state->SYSTEM_MODULE.ip_addr_str);  /* placeholder payTo */
}

void ble_x402_update_stats(float hashrate, float temp, float power, uint16_t freq) {
    snprintf(stats_json, sizeof(stats_json),
        "{\"hashrate\":%.0f,\"temp\":%.1f,\"power\":%.1f,\"freq\":%u}",
        hashrate, temp, power, freq);
}

static int offer_access_cb(uint16_t conn, uint16_t attr,
    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ble_x402_build_offer();
        os_mbuf_append(ctxt->om, offer_json, strlen(offer_json));
        ESP_LOGI(TAG, "Offer read by client");
    }
    return 0;
}

static int stats_access_cb(uint16_t conn, uint16_t attr,
    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, stats_json, strlen(stats_json));
    }
    return 0;
}

static int payment_access_cb(uint16_t conn, uint16_t attr,
    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (payment_len + len < (int)sizeof(payment_buf) - 1) {
            ble_hs_mbuf_to_flat(ctxt->om, payment_buf + payment_len, len, NULL);
            payment_len += len;
            payment_buf[payment_len] = '\0';
            ESP_LOGI(TAG, "Payment data received (%d bytes total)", payment_len);

            /* Check if complete JSON (ends with '}') */
            if (payment_len > 0 && payment_buf[payment_len - 1] == '}') {
                ESP_LOGI(TAG, "Payment complete: %s", payment_buf);
                /* TODO: verify payment via x402 facilitator, then notify receipt */
                const char *receipt = "{\"status\":\"received\",\"amount\":100000}";
                ble_x402_notify_receipt(receipt);
                payment_len = 0;
            }
        } else {
            ESP_LOGW(TAG, "Payment buffer overflow");
            payment_len = 0;
        }
    }
    return 0;
}

static int receipt_access_cb(uint16_t conn, uint16_t attr,
    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

/* GATT service definition */
static const struct ble_gatt_svc_def x402_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &x402_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &offer_chr_uuid.u,
                .access_cb = offer_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &stats_chr_uuid.u,
                .access_cb = stats_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &payment_chr_uuid.u,
                .access_cb = payment_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &receipt_chr_uuid.u,
                .access_cb = receipt_access_cb,
                .val_handle = &receipt_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }, /* sentinel */
        },
    },
    { 0 }, /* sentinel */
};

void ble_x402_notify_receipt(const char *receipt_json) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(receipt_json, strlen(receipt_json));
    if (om) {
        ble_gatts_notify_custom(conn_handle, receipt_val_handle, om);
        ESP_LOGI(TAG, "Receipt notification sent");
    }
}

/* GAP event handler */
static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected (handle=%d)", conn_handle);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected");
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* Restart advertising */
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
            &(struct ble_gap_adv_params){
                .conn_mode = BLE_GAP_CONN_MODE_UND,
                .disc_mode = BLE_GAP_DISC_MODE_GEN,
            }, gap_event_cb, NULL);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void ble_on_sync(void) {
    /* Set device name */
    ble_svc_gap_device_name_set("Bitaxe-x402");

    /* Configure advertising data */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)"Bitaxe-x402";
    fields.name_len = strlen("Bitaxe-x402");
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0x4020) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    ble_gap_adv_set_fields(&fields);

    /* Start advertising */
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
        &adv_params, gap_event_cb, NULL);

    ESP_LOGI(TAG, "BLE advertising started as 'Bitaxe-x402'");
}

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_x402_init(GlobalState *global_state) {
    g_state = global_state;

    /* Initialize stats with current values */
    snprintf(stats_json, sizeof(stats_json),
        "{\"hashrate\":0,\"temp\":0,\"power\":0,\"freq\":0}");

    /* Init NimBLE */
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure NimBLE host */
    ble_hs_cfg.sync_cb = ble_on_sync;

    /* Register GATT services */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(x402_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(x402_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* Start NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE x402 service initialized");
    return ESP_OK;
}

#endif /* CONFIG_BT_ENABLED */
