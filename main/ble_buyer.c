#include "ble_buyer.h"

#ifdef CONFIG_BT_NIMBLE_ROLE_CENTRAL

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "boat_crypto.h"
#include "boat_nano.h"
#include "boat_pay.h"
#include "boat_pal.h"
#include "hashanchor.h"
#include "display.h"
#include "nvs_config.h"

static const char *TAG = "ble_buyer";

/* ---- eCandle GATT UUIDs (0xEE00 service) ---- */
#define ECANDLE_SVC          0xEE00
#define CHR_STREAM_REQUEST   0xEE01  /* Write: initiate purchase */
#define CHR_STREAM_OFFER     0xEE02  /* Notify: seller price offer */
#define CHR_SLICE_REQUEST    0xEE03  /* Notify: per-slice payment request */
#define CHR_SLICE_PAYMENT    0xEE04  /* Write: signed EIP-3009 */
#define CHR_STREAM_STATUS    0xEE05  /* Notify: session status */
#define CHR_DEVICE_INFO      0xEE06  /* Read: seller info + wallet */

/* Arc Testnet USDC contract (for reference in paymentRequirements) */
static const uint8_t USDC_ARC[20] __attribute__((unused)) = {
    0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
/* Circle Gateway contract (verifyingContract for EIP-712 domain) */
static const uint8_t GATEWAY_ARC[20] = {
    0x00, 0x77, 0x77, 0x7d, 0x7E, 0xBA, 0x46, 0x88, 0xBD, 0xeF,
    0x3E, 0x31, 0x1b, 0x84, 0x6F, 0x25, 0x87, 0x0A, 0x19, 0xB9
};
#define CHAIN_ID        5042002
#define DOMAIN_NAME     "GatewayWalletBatched"
#define DOMAIN_VERSION  "1"

/* ---- Config ---- */
#define DEFAULT_THRESHOLD       60   /* $0.000060/slice = ~$0.06/kWh */
#define SCAN_DURATION_MS      5000   /* 5s per scan cycle */
#define RESCAN_DELAY_MS       5000   /* wait 5s between scans when waiting */

/* ---- State ---- */
static const boat_keypair_t *s_kp = NULL;
static ble_buyer_result_t s_result;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static char s_target_name[32] = {0};
static int s_target_found = 0;
static uint32_t s_max_slices = 24;
static uint32_t s_slice_seconds = 10;
static uint16_t s_adv_price = 0;    /* price from advertising data */
static uint8_t  s_adv_avail = 0;    /* available Wh from advertising */
static uint8_t  s_adv_state = 0xFF; /* seller state from advertising */

/* Discovered characteristic value handles */
static uint16_t h_stream_request = 0;
static uint16_t h_stream_offer = 0;
static uint16_t h_slice_request = 0;
static uint16_t h_slice_payment = 0;
static uint16_t h_stream_status = 0;
static uint16_t h_device_info = 0;

/* CCCD descriptor handles (discovered dynamically) */
static uint16_t h_cccd_offer = 0;
static uint16_t h_cccd_slice_req = 0;
static uint16_t h_cccd_status = 0;

/* Service handle range for descriptor discovery */
static uint16_t s_svc_end_handle = 0;

/* Seller address in binary */
static uint8_t s_seller_addr[20] = {0};

/* ---- OLED display helper ---- */
static void oled_update(void)
{
    char l1[48], l2[48], l3[48];

    switch (s_result.state) {
    case BLE_BUYER_SCANNING:
        snprintf(l1, 48, "Scanning...");
        snprintf(l2, 48, "Thr: $%.6f/sl", (double)s_result.threshold / 1000000.0);
        l3[0] = '\0';
        break;
    case BLE_BUYER_WAITING:
        snprintf(l1, 48, "%s $%.4f", s_result.seller_name, (double)s_result.price_per_slice / 1000000.0);
        snprintf(l2, 48, "Too expensive");
        l3[0] = '\0';
        break;
    case BLE_BUYER_CONNECTING:
    case BLE_BUYER_DISCOVERING:
    case BLE_BUYER_READING_INFO:
    case BLE_BUYER_NEGOTIATING:
        snprintf(l1, 48, "Connecting...");
        snprintf(l2, 48, "$%.6f/slice", (double)s_result.price_per_slice / 1000000.0);
        l3[0] = '\0';
        break;
    case BLE_BUYER_STREAMING:
        snprintf(l1, 48, "Mining");
        snprintf(l2, 48, "Slice %lu/%lu", (unsigned long)s_result.slices_paid,
                 (unsigned long)s_result.max_slices);
        snprintf(l3, 48, "$%.6f paid", (double)s_result.total_paid / 1000000.0);
        break;
    case BLE_BUYER_DECIDING:
        snprintf(l1, 48, "Session done");
        snprintf(l2, 48, "Sessions: %lu", (unsigned long)s_result.total_sessions);
        snprintf(l3, 48, "Checking price...");
        break;
    case BLE_BUYER_COMPLETE:
        snprintf(l1, 48, "Complete");
        snprintf(l2, 48, "S:%lu $%.6f", (unsigned long)s_result.total_sessions,
                 (double)s_result.total_paid / 1000000.0);
        l3[0] = '\0';
        break;
    default:
        snprintf(l1, 48, "BLE Buy");
        snprintf(l2, 48, "Idle");
        l3[0] = '\0';
    }

    screen_ble_buy_update(l1, l2, l3[0] ? l3 : NULL);
}

/* ---- JSON helpers ---- */

static int jget(const char *json, const char *key, char *out, size_t max)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (p) {
        p += strlen(pat);
        const char *e = strchr(p, '"');
        if (!e) return -1;
        size_t len = (size_t)(e - p) < max - 1 ? (size_t)(e - p) : max - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        return 0;
    }
    /* Try numeric */
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && i < max - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static uint64_t jget_u64(const char *json, const char *key)
{
    char buf[32] = {0};
    if (jget(json, key, buf, sizeof(buf)) < 0) return 0;
    uint64_t v = 0;
    /* Handle both "42" (string) and 42 (number), also "0.000042" */
    const char *p = buf;
    if (*p == '"') p++;
    /* Check for decimal format like "0.000042" */
    const char *dot = strchr(p, '.');
    if (dot) {
        /* Parse as float * 10^6 (USDC 6 decimals) */
        double fv = strtod(p, NULL);
        v = (uint64_t)(fv * 1000000.0 + 0.5);
    } else {
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    }
    return v;
}

/* ---- Build auth JSON for SlicePayment ---- */

static int auth_to_json(const boat_nano_auth_t *a, char *out, size_t max)
{
    char from_h[43], to_h[43], nonce_h[67], r_h[67], s_h[67];
    snprintf(from_h, 43, "0x"); boat_bytes_to_hex(a->from, 20, from_h + 2);
    snprintf(to_h, 43, "0x");   boat_bytes_to_hex(a->to, 20, to_h + 2);
    snprintf(nonce_h, 67, "0x"); boat_bytes_to_hex(a->nonce, 32, nonce_h + 2);
    snprintf(r_h, 67, "0x");    boat_bytes_to_hex(a->r, 32, r_h + 2);
    snprintf(s_h, 67, "0x");    boat_bytes_to_hex(a->s, 32, s_h + 2);

    /* Compact signature: 0x + r(64) + s(64) + v(2) = 132 hex chars */
    char sig_h[135];
    memcpy(sig_h, "0x", 2);
    boat_bytes_to_hex(a->r, 32, sig_h + 2);
    boat_bytes_to_hex(a->s, 32, sig_h + 66);
    snprintf(sig_h + 130, 5, "%02x", (unsigned)a->v);

    /* Decimal value */
    uint64_t val_dec = 0;
    for (int i = 24; i < 32; i++)
        val_dec = (val_dec << 8) | a->value[i];

    /* Minimal JSON (~220 bytes): omit from/to (eCandle knows both from session).
     * Must be < 253 bytes (MTU 256 - 3 ATT overhead). */
    return snprintf(out, max,
        "{\"va\":\"%" PRIu64 "\",\"vb\":%" PRIu32 ","
        "\"n\":\"%s\",\"sig\":\"%s\"}",
        val_dec, a->valid_before, nonce_h, sig_h);
}

/* ---- Forward declarations ---- */
static int gap_event(struct ble_gap_event *event, void *arg);
static void sign_and_send_slice(const char *slice_json);
static void start_mining(void);
static void stop_mining(void);
static void deferred_first_payment(void *arg1, uint32_t arg2);
static int on_info_read(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg);
static int on_verify_debug_read(uint16_t conn, const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg);
static void send_stream_request(void);

/* ---- BM1370 control via localhost HTTP ---- */

static void start_mining(void)
{
    if (s_result.mining_active) return;
    s_result.mining_active = 1;
    ESP_LOGI(TAG, "BM1370: START mining + fan ON");

    /* Set ASIC frequency + fan via NVS (immediate, no HTTP needed) */
    nvs_config_set_u16(NVS_CONFIG_ASIC_FREQUENCY, 490);
    nvs_config_set_bool(NVS_CONFIG_AUTO_FAN_SPEED, true);
    /* Also try HTTP for immediate effect (may fail during BLE, that's OK) */
    char body[64];
    snprintf(body, sizeof(body), "{\"frequency\":490,\"autofanspeed\":1,\"fanspeed\":100}");
    int status = 0;
    char resp[128] = {0};
    boat_pal_http_post("http://localhost/api/system", NULL,
                        "application/json", body,
                        &status, resp, sizeof(resp));
    ESP_LOGI(TAG, "AxeOS start: HTTP %d", status);
}

static void stop_mining(void)
{
    if (!s_result.mining_active) return;
    s_result.mining_active = 0;
    ESP_LOGI(TAG, "BM1370: STOP mining + fan OFF");

    /* Set frequency=0 + fan off via NVS */
    nvs_config_set_u16(NVS_CONFIG_ASIC_FREQUENCY, 0);
    nvs_config_set_bool(NVS_CONFIG_AUTO_FAN_SPEED, false);
    nvs_config_set_u16(NVS_CONFIG_MANUAL_FAN_SPEED, 0);
    /* Also try HTTP */
    char body[64];
    snprintf(body, sizeof(body), "{\"frequency\":0,\"autofanspeed\":0,\"fanspeed\":0}");
    int status = 0;
    char resp[128] = {0};
    boat_pal_http_post("http://localhost/api/system", NULL,
                        "application/json", body,
                        &status, resp, sizeof(resp));
    ESP_LOGI(TAG, "AxeOS stop: HTTP %d", status);
}

/* Resume hashanchor when BLE session ends (any reason) */
static void session_ended(void)
{
    hashanchor_set_paused(0);
    ESP_LOGI(TAG, "Hashanchor resumed");
    oled_update();
}

/* ---- Sign a slice payment and write to eCandle ---- */

static void sign_and_send_slice(const char *slice_json)
{
    if (!s_kp || s_conn == BLE_HS_CONN_HANDLE_NONE || !h_slice_payment) {
        ESP_LOGE(TAG, "Cannot send slice: not connected");
        return;
    }

    /* Parse slice request for nonce/timing */
    boat_nano_slice_t slice = {0};
    char nonce_str[67] = {0};

    if (slice_json) {
        char buf[20] = {0};
        jget(slice_json, "slice_id", buf, sizeof(buf));
        slice.slice_id = (uint32_t)atoi(buf);

        jget(slice_json, "nonce", nonce_str, sizeof(nonce_str));
        if (nonce_str[0]) boat_hex_to_bytes(nonce_str, slice.nonce, 32);
        else boat_pal_random(slice.nonce, 32);

        buf[0] = 0;
        jget(slice_json, "valid_after", buf, sizeof(buf));
        slice.valid_after = (uint32_t)atoi(buf);

        buf[0] = 0;
        jget(slice_json, "valid_before", buf, sizeof(buf));
        slice.valid_before = (uint32_t)atoi(buf);
    } else {
        /* First slice: no slice_request yet */
        boat_pal_random(slice.nonce, 32);
        slice.valid_after = 0;
        slice.valid_before = boat_pal_time() + 345600; /* 4 days — Circle Gateway minimum */
    }

    /* Set EIP-712 domain: GatewayWalletBatched with Gateway verifyingContract */
    boat_pay_set_domain_ext(CHAIN_ID, GATEWAY_ARC, DOMAIN_NAME, DOMAIN_VERSION);

    /* Build payment struct */
    boat_payment_t pay;
    memset(&pay, 0, sizeof(pay));
    memcpy(pay.to, s_seller_addr, 20);
    /* value: uint256 big-endian */
    {
        uint64_t v = s_result.price_per_slice;
        for (int i = 7; i >= 0; i--) pay.value[31-i] = (uint8_t)(v >> (i*8));
    }
    pay.valid_after  = slice.valid_after;
    pay.valid_before = slice.valid_before;
    memcpy(pay.nonce, slice.nonce, 32);

    /* Sign EIP-3009 with Gateway domain */
    uint8_t sig65[65];
    boat_err_t err = boat_pay_authorize(s_kp, &pay, sig65);
    if (err != BOAT_OK) {
        ESP_LOGE(TAG, "pay_authorize failed: %d", err);
        snprintf(s_result.error, sizeof(s_result.error), "sign: %d", err);
        s_result.state = BLE_BUYER_ERROR;
        return;
    }

    /* Build boat_nano_auth_t from pay + signature */
    boat_nano_auth_t auth;
    memset(&auth, 0, sizeof(auth));
    memcpy(auth.from, s_kp->eth_address, 20);
    memcpy(auth.to, pay.to, 20);
    memcpy(auth.value, pay.value, 32);
    auth.valid_after  = pay.valid_after;
    auth.valid_before = pay.valid_before;
    memcpy(auth.nonce, pay.nonce, 32);
    memcpy(auth.r, sig65, 32);
    memcpy(auth.s, sig65 + 32, 32);
    auth.v = sig65[64];

    /* Build JSON */
    char *pay_json = malloc(600);
    if (!pay_json) { s_result.state = BLE_BUYER_ERROR; return; }
    int len = auth_to_json(&auth, pay_json, 600);

    ESP_LOGI(TAG, "Slice #%" PRIu32 " signed (v=%d), writing %d bytes...",
             s_result.slices_paid + 1, auth.v, len);

    /* Write to SlicePayment characteristic */
    s_result.last_write_len = len;
    int rc = ble_gattc_write_flat(s_conn, h_slice_payment,
                                   pay_json, (uint16_t)len,
                                   NULL, NULL);
    s_result.last_write_rc = rc;
    free(pay_json);

    ESP_LOGI(TAG, "write_flat rc=%d len=%d handle=%d", rc, len, h_slice_payment);

    if (rc != 0) {
        ESP_LOGE(TAG, "BLE write failed: %d (len=%d, handle=%d)", rc, len, h_slice_payment);
        snprintf(s_result.error, sizeof(s_result.error), "write:%d len=%d h=%d", rc, len, h_slice_payment);
        s_result.state = BLE_BUYER_ERROR;
        return;
    }

    s_result.slices_paid++;
    s_result.total_paid += s_result.price_per_slice;
    oled_update();
    ESP_LOGI(TAG, "Slice #%" PRIu32 "/%" PRIu32 " paid, total=%" PRIu64,
             s_result.slices_paid, s_result.max_slices, s_result.total_paid);

    /* Energy management: stop BM1370 early to reserve power for ESP32.
     * BM1370 ~15W, ESP32 ~2W. Reserve last 2 slices for ESP32 standby
     * so it can complete BLE negotiation for next purchase. */
    if (s_result.slices_paid >= s_result.max_slices - 2 && s_result.mining_active) {
        ESP_LOGW(TAG, "Approaching max slices — stopping BM1370 to reserve ESP32 power");
        stop_mining();
    }

    /* Read DeviceInfo after payment to get verify debug (last_verify field) */
    if (h_device_info && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gattc_read(s_conn, h_device_info, on_verify_debug_read, NULL);
    }
}

/* Deferred first payment: runs from timer service task after 2s delay */
static void deferred_first_payment(void *arg1, uint32_t arg2)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (s_result.state == BLE_BUYER_STREAMING && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "Sending deferred first SlicePayment...");
        sign_and_send_slice(NULL);
    }
}

static int on_verify_debug_read(uint16_t conn, const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0 && attr) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        char *json = malloc(len + 1);
        if (json) {
            ble_hs_mbuf_to_flat(attr->om, json, len, NULL);
            json[len] = '\0';
            ESP_LOGW(TAG, "=== VERIFY DEBUG: %s ===", json);

            /* Store in error field so HTTP API can show it */
            char last_verify[128] = {0};
            jget(json, "last_verify", last_verify, sizeof(last_verify));
            if (last_verify[0]) {
                snprintf(s_result.error, sizeof(s_result.error), "verify_dbg: %.100s", last_verify);
            }
            free(json);
        }
    }
    return 0;
}

/* ---- Descriptor discovery (find CCCD handles) ---- */

static int on_dsc_disc(uint16_t conn, const struct ble_gatt_error *error,
                        uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0 && dsc) {
        uint16_t uuid = ble_uuid_u16(&dsc->uuid.u);
        if (uuid == 0x2902) { /* Client Characteristic Configuration Descriptor */
            ESP_LOGI(TAG, "  CCCD found: handle=%d for chr_val=%d", dsc->handle, chr_val_handle);
            if (chr_val_handle == h_stream_offer) h_cccd_offer = dsc->handle;
            else if (chr_val_handle == h_slice_request) h_cccd_slice_req = dsc->handle;
            else if (chr_val_handle == h_stream_status) h_cccd_status = dsc->handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        /* If descriptor discovery didn't find CCCDs, fallback to val_handle+1 */
        if (!h_cccd_offer && h_stream_offer) h_cccd_offer = h_stream_offer + 1;
        if (!h_cccd_slice_req && h_slice_request) h_cccd_slice_req = h_slice_request + 1;
        if (!h_cccd_status && h_stream_status) h_cccd_status = h_stream_status + 1;

        ESP_LOGI(TAG, "CCCD handles: offer=%d sliceReq=%d status=%d",
                 h_cccd_offer, h_cccd_slice_req, h_cccd_status);

        /* Now read DeviceInfo → subscribe → StreamRequest */
        s_result.state = BLE_BUYER_READING_INFO;
        if (h_device_info) {
            ble_gattc_read(s_conn, h_device_info, on_info_read, NULL);
        } else {
            send_stream_request();
        }
    }
    return 0;
}

/* ---- Characteristic discovery ---- */

static int on_chr_disc(uint16_t conn, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr) {
        uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        ESP_LOGI(TAG, "  chr 0x%04X def=%d val=%d", uuid, chr->def_handle, chr->val_handle);
        switch (uuid) {
            case CHR_STREAM_REQUEST: h_stream_request = chr->val_handle; s_result.h_request = chr->val_handle; break;
            case CHR_STREAM_OFFER:   h_stream_offer = chr->val_handle; break;
            case CHR_SLICE_REQUEST:  h_slice_request = chr->val_handle; break;
            case CHR_SLICE_PAYMENT:  h_slice_payment = chr->val_handle; s_result.h_payment = chr->val_handle; break;
            case CHR_STREAM_STATUS:  h_stream_status = chr->val_handle; break;
            case CHR_DEVICE_INFO:    h_device_info = chr->val_handle; break;
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Chr discovery done: req=%d offer=%d sliceReq=%d pay=%d status=%d info=%d",
                 h_stream_request, h_stream_offer, h_slice_request,
                 h_slice_payment, h_stream_status, h_device_info);

        if (!h_slice_payment || !h_stream_request) {
            snprintf(s_result.error, sizeof(s_result.error), "missing chr");
            s_result.state = BLE_BUYER_ERROR;
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        /* Discover descriptors to find CCCD handles */
        uint16_t start = h_stream_offer ? h_stream_offer : 1;
        ble_gattc_disc_all_dscs(s_conn, start, s_svc_end_handle,
                                 on_dsc_disc, NULL);
    }
    return 0;
}

/* ---- Service discovery ---- */

static int on_svc_disc(uint16_t conn, const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0 && svc) {
        ESP_LOGI(TAG, "Found 0xEE00 service: %d-%d",
                 svc->start_handle, svc->end_handle);
        s_svc_end_handle = svc->end_handle;
        s_result.svc_start = svc->start_handle;
        s_result.svc_end = svc->end_handle;
        ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle,
                                 on_chr_disc, NULL);
    } else if (error->status == BLE_HS_EDONE) {
        if (!h_stream_request) {
            ESP_LOGW(TAG, "0xEE00 service not found");
            snprintf(s_result.error, sizeof(s_result.error), "no 0xEE00");
            s_result.state = BLE_BUYER_ERROR;
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

/* ---- Send StreamRequest ---- */

static int s_cccd_pending = 0;
static void do_write_stream_request(void); /* forward decl */

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "CCCD subscribed OK (handle=%d)", attr ? attr->handle : 0);
    } else {
        ESP_LOGW(TAG, "CCCD write failed: status=%d", error->status);
    }
    /* Chain next CCCD write, then finally StreamRequest */
    int tag = (int)(uintptr_t)arg;
    uint8_t next_cccd[2] = {0x01, 0x00};
    if (tag == 1 && h_slice_request) {
        uint16_t h = h_cccd_slice_req ? h_cccd_slice_req : h_slice_request + 1;
        ESP_LOGI(TAG, "Subscribe sliceReq CCCD at handle %d", h);
        ble_gattc_write_flat(s_conn, h, next_cccd, 2,
                              on_cccd_written, (void *)(uintptr_t)2);
    } else if (tag <= 2 && h_stream_status) {
        uint16_t h = h_cccd_status ? h_cccd_status : h_stream_status + 1;
        ESP_LOGI(TAG, "Subscribe status CCCD at handle %d", h);
        ble_gattc_write_flat(s_conn, h, next_cccd, 2,
                              on_cccd_written, (void *)(uintptr_t)3);
    } else {
        /* All CCCDs done — now safe to write StreamRequest */
        do_write_stream_request();
    }
    return 0;
}

static void subscribe_notifications(void)
{
    uint8_t cccd[2] = {0x01, 0x00};
    /* Chain CCCD writes one at a time to avoid BLE_HS_EBUSY */
    uint16_t cccd_h = h_cccd_offer ? h_cccd_offer : (h_stream_offer ? h_stream_offer + 1 : 0);
    if (cccd_h) {
        ESP_LOGI(TAG, "Subscribe offer CCCD at handle %d", cccd_h);
        ble_gattc_write_flat(s_conn, cccd_h, cccd, 2, on_cccd_written,
                              (void *)(uintptr_t)1);
    } else {
        /* No offer to subscribe, try slice_request */
        uint16_t sr_h = h_cccd_slice_req ? h_cccd_slice_req : (h_slice_request ? h_slice_request + 1 : 0);
        if (sr_h) {
            ESP_LOGI(TAG, "Subscribe sliceReq CCCD at handle %d", sr_h);
            ble_gattc_write_flat(s_conn, sr_h, cccd, 2, on_cccd_written,
                                  (void *)(uintptr_t)2);
        } else {
            /* Nothing to subscribe */
            do_write_stream_request();
        }
    }
}

static void send_stream_request(void)
{
    if (!h_stream_request) return;

    /* Subscribe to notifications — StreamRequest will be sent from CCCD callback */
    s_cccd_pending = 0;
    subscribe_notifications();
    if (s_cccd_pending == 0) {
        /* No notifications to subscribe — send immediately */
        do_write_stream_request();
    }
}

static int on_fallback_info_read(uint16_t conn, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg);

static int on_stream_req_written(uint16_t conn, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "StreamRequest written OK, waiting for StreamOffer...");
        /*
         * Workaround: if CCCD subscription failed silently, we won't get the
         * StreamOffer notify. After 2 seconds, fallback to reading DeviceInfo
         * to check if eCandle entered NEGOTIATING and proceed with known price.
         */
        /* Use a timer or just read DeviceInfo after a short delay.
         * Since we can't do async timers easily here, we'll poll DeviceInfo
         * immediately — by the time the write response arrives, eCandle
         * should already be in NEGOTIATING state. */
        if (h_device_info) {
            ble_gattc_read(conn, h_device_info, on_fallback_info_read, NULL);
        }
    } else {
        ESP_LOGW(TAG, "StreamRequest write error: %d (trying fallback)", error->status);
        /* Write may have timed out but eCandle might still have processed it.
         * Try fallback: read DeviceInfo to check state */
        if (h_device_info && s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gattc_read(conn, h_device_info, on_fallback_info_read, NULL);
        } else {
            snprintf(s_result.error, sizeof(s_result.error), "stream_req_cb: %d", error->status);
            s_result.state = BLE_BUYER_ERROR;
        }
    }
    return 0;
}

/* Fallback: if StreamOffer notify not received, read DeviceInfo to check state */
static int on_fallback_info_read(uint16_t conn, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg)
{
    if (s_result.state != BLE_BUYER_NEGOTIATING) {
        /* StreamOffer already arrived via notify — no need for fallback */
        return 0;
    }

    if (error->status == 0 && attr) {
        char json[256] = {0};
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len < sizeof(json) - 1) {
            ble_hs_mbuf_to_flat(attr->om, json, len, NULL);
            ESP_LOGI(TAG, "Fallback DeviceInfo: %s", json);

            char state_str[20] = {0};
            jget(json, "state", state_str, sizeof(state_str));

            if (strcmp(state_str, "NEGOTIATING") == 0 ||
                strcmp(state_str, "IDLE") == 0) {
                /* eCandle processed StreamRequest — use known price as fallback.
                 * Defer SlicePayment by 2s to let eCandle finish processing.
                 * Can't vTaskDelay in BLE callback — use timer. */
                ESP_LOGW(TAG, "StreamOffer not received, deferring payment 2s...");

                if (s_result.price_per_slice == 0)
                    s_result.price_per_slice = 42;

                s_result.state = BLE_BUYER_STREAMING;
                /* Use xTimerPendFunctionCall for deferred execution */
                oled_update();
                xTimerPendFunctionCall(
                    (PendedFunction_t)deferred_first_payment,
                    NULL, 0, pdMS_TO_TICKS(100));
                start_mining();
            }
        }
    }
    return 0;
}

static void do_write_stream_request(void)
{
    char req[256];
    int len = snprintf(req, sizeof(req),
        "{\"action\":\"stream_request\","
        "\"slice_seconds\":%" PRIu32 ","
        "\"max_slices\":%" PRIu32 ","
        "\"payer\":\"%s\"}",
        s_slice_seconds, s_max_slices, s_kp->eth_addr_hex);

    s_result.state = BLE_BUYER_NEGOTIATING;
    ESP_LOGI(TAG, "StreamRequest (%d bytes): %s", len, req);

    int rc = ble_gattc_write_flat(s_conn, h_stream_request,
                                   req, (uint16_t)len,
                                   on_stream_req_written, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "StreamRequest write failed: %d", rc);
        snprintf(s_result.error, sizeof(s_result.error), "stream_req: %d", rc);
        s_result.state = BLE_BUYER_ERROR;
    }
}

/* ---- Handle notifications from eCandle ---- */

static void handle_notify(uint16_t attr_handle, const uint8_t *data, uint16_t len)
{
    char json[512] = {0};
    if (len >= sizeof(json)) len = sizeof(json) - 1;
    memcpy(json, data, len);

    if (attr_handle == h_stream_offer) {
        /* StreamOffer: parse price and confirm */
        ESP_LOGI(TAG, "StreamOffer: %s", json);

        s_result.price_per_slice = jget_u64(json, "price_per_slice");
        if (s_result.price_per_slice == 0) {
            /* Fallback: try "amount" field */
            s_result.price_per_slice = jget_u64(json, "amount");
        }

        char pay_to[43] = {0};
        jget(json, "payTo", pay_to, sizeof(pay_to));
        if (pay_to[0]) {
            strncpy(s_result.seller_addr, pay_to, sizeof(s_result.seller_addr) - 1);
            boat_hex_to_bytes(pay_to, s_seller_addr, 20);
        }

        ESP_LOGI(TAG, "Price: %" PRIu64 " USDC/slice, payTo: %s",
                 s_result.price_per_slice, s_result.seller_addr);

        /* Send first SlicePayment immediately */
        s_result.state = BLE_BUYER_STREAMING;
        sign_and_send_slice(NULL);
                oled_update();
        start_mining();

    } else if (attr_handle == h_slice_request) {
        /* SliceRequest: sign and pay within 3 seconds */
        ESP_LOGI(TAG, "SliceRequest: %s", json);

        if (s_result.state != BLE_BUYER_STREAMING) {
            ESP_LOGW(TAG, "SliceRequest while not streaming, ignoring");
            return;
        }

        if (s_result.slices_paid >= s_result.max_slices) {
            ESP_LOGI(TAG, "Max slices reached, not paying");
            return;
        }

        sign_and_send_slice(json);

    } else if (attr_handle == h_stream_status) {
        /* StreamStatus: sync mining state */
        ESP_LOGI(TAG, "StreamStatus: %s", json);

        char status[20] = {0};
        jget(json, "status", status, sizeof(status));

        char wh_str[16] = {0};
        jget(json, "total_wh", wh_str, sizeof(wh_str));
        if (wh_str[0]) s_result.total_wh = strtof(wh_str, NULL);

        if (strcmp(status, "STREAMING") == 0) {
            start_mining();
        } else if (strcmp(status, "COMPLETE") == 0) {
            ESP_LOGI(TAG, "Session COMPLETE: %" PRIu32 " slices, %.4f Wh, %" PRIu64 " USDC",
                     s_result.slices_paid, s_result.total_wh, s_result.total_paid);
            stop_mining();
            s_result.total_sessions++;
            s_result.slices_paid = 0; /* reset for next session */

            if (s_result.auto_mode) {
                /* DECIDING: disconnect, rescan to check price */
                ESP_LOGI(TAG, "Auto mode: disconnecting, will rescan for price");
                s_result.state = BLE_BUYER_DECIDING;
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                /* Rescan will be triggered from disconnect handler */
            } else {
                s_result.state = BLE_BUYER_COMPLETE;
                session_ended();
            }
        } else if (strcmp(status, "ERROR") == 0) {
            ESP_LOGW(TAG, "Session ERROR from seller");
            stop_mining();
            snprintf(s_result.error, sizeof(s_result.error), "seller error");
            s_result.state = BLE_BUYER_ERROR;
        }
    }
}

/* ---- GAP event handler ---- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                     event->disc.length_data) != 0)
            break;
        if (!fields.name || fields.name_len == 0) break;

        char name[32] = {0};
        size_t nlen = fields.name_len < 31 ? fields.name_len : 31;
        memcpy(name, fields.name, nlen);

        int match = 0;
        if (s_target_name[0])
            match = (strstr(name, s_target_name) != NULL);
        else
            match = (strstr(name, "ECandle") || strstr(name, "eCandle") ||
                     strstr(name, "ecandle"));

        if (match && !s_target_found) {
            strncpy(s_result.seller_name, name, sizeof(s_result.seller_name) - 1);

            /* Parse manufacturer specific data for price */
            if (fields.mfg_data && fields.mfg_data_len >= 6) {
                const uint8_t *mfg = fields.mfg_data;
                /* [0-1]=company, [2-3]=price_per_slice LE, [4]=avail_wh, [5]=state */
                s_adv_price = (uint16_t)mfg[2] | ((uint16_t)mfg[3] << 8);
                s_adv_avail = mfg[4];
                s_adv_state = mfg[5];
                s_result.price_per_slice = s_adv_price;
                s_result.seller_available_wh = s_adv_avail;
                s_result.seller_state = s_adv_state;
                ESP_LOGI(TAG, "Found: %s price=%" PRIu16 " avail=%d state=%d",
                         name, s_adv_price, s_adv_avail, s_adv_state);
            } else {
                ESP_LOGI(TAG, "Found: %s (no mfg data, will read DeviceInfo)", name);
                s_adv_price = 0;
                s_adv_state = 0; /* assume IDLE */
            }

            /* Auto mode: check profitability before connecting */
            if (s_result.auto_mode && s_adv_price > 0) {
                if (s_adv_price > s_result.threshold) {
                    ESP_LOGI(TAG, "Price %" PRIu16 " > threshold %" PRIu64 ", waiting",
                             s_adv_price, s_result.threshold);
                    s_result.state = BLE_BUYER_WAITING;
                    /* Don't connect, let scan complete, will rescan */
                    oled_update();
                    break;
                }
                if (s_adv_state != 0) { /* 0 = IDLE */
                    ESP_LOGI(TAG, "Seller state %d != IDLE, waiting", s_adv_state);
                    s_result.state = BLE_BUYER_WAITING;
                    oled_update();
                    break;
                }
            }

            /* Profitable or single-shot mode → connect */
            s_target_found = 1;
            ble_gap_disc_cancel();
            s_result.state = BLE_BUYER_CONNECTING;
            ESP_LOGI(TAG, "Connecting to %s (price=%" PRIu64 ")", name, s_result.price_per_slice);
            int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                                      10000, NULL, gap_event, NULL);
            if (rc != 0) {
                snprintf(s_result.error, sizeof(s_result.error), "connect: %d", rc);
                s_result.state = BLE_BUYER_ERROR;
            }
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!s_target_found) {
            if (s_result.auto_mode) {
                /* Auto mode: rescan after delay */
                if (s_result.state == BLE_BUYER_WAITING) {
                    ESP_LOGI(TAG, "Price too high, rescan in %dms", RESCAN_DELAY_MS);
                } else {
                    ESP_LOGI(TAG, "No ECandle found, rescan in %dms", RESCAN_DELAY_MS);
                    s_result.state = BLE_BUYER_SCANNING;
                }
    oled_update();
                vTaskDelay(pdMS_TO_TICKS(RESCAN_DELAY_MS));
                s_target_found = 0;
                struct ble_gap_disc_params params = { .filter_duplicates = 1, .passive = 1 };
                ble_gap_adv_stop();
                uint8_t addr_type;
                ble_hs_id_infer_auto(0, &addr_type);
                ble_gap_disc(addr_type, SCAN_DURATION_MS, &params, gap_event, NULL);
            } else {
                ESP_LOGW(TAG, "Scan done, no ECandle found");
                snprintf(s_result.error, sizeof(s_result.error), "no ECandle found");
                s_result.state = BLE_BUYER_ERROR;
                session_ended();
            }
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected (handle=%d)", s_conn);

            /* MTU 512: SlicePayment JSON is ~480 bytes, needs MTU > 480 */
            ble_att_set_preferred_mtu(512);
            ble_gattc_exchange_mtu(s_conn, NULL, NULL);

            s_result.state = BLE_BUYER_DISCOVERING;
            h_stream_request = h_stream_offer = h_slice_request = 0;
            h_slice_payment = h_stream_status = h_device_info = 0;

            ble_uuid16_t svc = BLE_UUID16_INIT(ECANDLE_SVC);
            ble_gattc_disc_svc_by_uuid(s_conn, &svc.u, on_svc_disc, NULL);
        } else {
            snprintf(s_result.error, sizeof(s_result.error),
                     "conn: %d", event->connect.status);
            s_result.state = BLE_BUYER_ERROR;
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason=%d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        stop_mining();

        if (s_result.state == BLE_BUYER_DECIDING) {
            /* Session complete, rescan to check price for next buy */
            ESP_LOGI(TAG, "Session done (total=%d), rescanning for price...",
                     (int)s_result.total_sessions);
            s_target_found = 0;
            s_result.state = BLE_BUYER_SCANNING;
            vTaskDelay(pdMS_TO_TICKS(2000)); /* brief delay before rescan */
    oled_update();
            struct ble_gap_disc_params params = { .filter_duplicates = 0, .passive = 1 };
            ble_gap_adv_stop();
            uint8_t addr_type;
            ble_hs_id_infer_auto(0, &addr_type);
            ble_gap_disc(addr_type, SCAN_DURATION_MS, &params, gap_event, NULL);
        } else if (s_result.state == BLE_BUYER_STREAMING ||
                   s_result.state == BLE_BUYER_NEGOTIATING) {
            /* Unexpected disconnect during session — auto reconnect */
            ESP_LOGW(TAG, "Session interrupted, reconnecting...");
            s_target_found = 0;
            s_result.state = BLE_BUYER_SCANNING;
            struct ble_gap_disc_params params = { .filter_duplicates = 1, .passive = 1 };
    oled_update();
            ble_gap_disc(BLE_OWN_ADDR_PUBLIC, SCAN_DURATION_MS, &params, gap_event, NULL);
        } else if (s_result.state != BLE_BUYER_COMPLETE &&
                   s_result.state != BLE_BUYER_ERROR) {
            s_result.state = BLE_BUYER_COMPLETE;
            session_ended();
        }
        break;

    case BLE_GAP_EVENT_MTU:
        s_result.last_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU: %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t *buf = malloc(len + 1);
        if (buf) {
            ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, NULL);
            buf[len] = '\0';
            handle_notify(event->notify_rx.attr_handle, buf, len);
            free(buf);
        }
        break;
    }

    default:
        break;
    }
    return 0;
}

/* ---- DeviceInfo read callback ---- */

static int on_info_read(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0 && attr) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        char json[256] = {0};
        if (len < sizeof(json) - 1) {
            ble_hs_mbuf_to_flat(attr->om, json, len, NULL);
            ESP_LOGI(TAG, "DeviceInfo: %s", json);

            char pay_to[43] = {0};
            jget(json, "payTo", pay_to, sizeof(pay_to));
            if (pay_to[0]) {
                strncpy(s_result.seller_addr, pay_to, sizeof(s_result.seller_addr) - 1);
                boat_hex_to_bytes(pay_to, s_seller_addr, 20);
                ESP_LOGI(TAG, "Seller wallet: %s", pay_to);
            }
        }

        /* Now send StreamRequest */
        send_stream_request();
    } else {
        ESP_LOGW(TAG, "DeviceInfo read err: %d, sending StreamRequest anyway", error->status);
        send_stream_request();
    }
    return 0;
}

/* ---- Public API ---- */

esp_err_t ble_buyer_init(const boat_keypair_t *kp)
{
    if (!kp) return ESP_ERR_INVALID_ARG;
    s_kp = kp;
    memset(&s_result, 0, sizeof(s_result));
    ESP_LOGI(TAG, "BLE buyer init (ETH: %s)", kp->eth_addr_hex);
    return ESP_OK;
}

esp_err_t ble_buyer_start_auto(const char *device_name,
                                uint64_t threshold,
                                uint32_t max_slices,
                                uint32_t slice_seconds)
{
    if (!s_kp) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ble_buyer_start(device_name, max_slices, slice_seconds);
    if (err == ESP_OK) {
        s_result.auto_mode = 1;
        s_result.threshold = threshold > 0 ? threshold : DEFAULT_THRESHOLD;
        ESP_LOGI(TAG, "Auto mode: threshold=%" PRIu64 " USDC/slice", s_result.threshold);
    }
    return err;
}

esp_err_t ble_buyer_start(const char *device_name,
                           uint32_t max_slices,
                           uint32_t slice_seconds)
{
    if (!s_kp) return ESP_ERR_INVALID_STATE;
    if (s_result.state >= BLE_BUYER_SCANNING &&
        s_result.state <= BLE_BUYER_STREAMING)
        return ESP_ERR_INVALID_STATE;

    memset(&s_result, 0, sizeof(s_result));
    s_target_found = 0;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_max_slices = max_slices > 0 ? max_slices : 24;
    s_slice_seconds = slice_seconds > 0 ? slice_seconds : 10;
    s_result.max_slices = s_max_slices;
    s_result.slice_seconds = s_slice_seconds;

    if (device_name)
        strncpy(s_target_name, device_name, sizeof(s_target_name) - 1);
    else
        s_target_name[0] = '\0';

    s_result.state = BLE_BUYER_SCANNING;
    ESP_LOGI(TAG, "Scanning for: %s (%" PRIu32 " slices × %" PRIu32 "s)",
             s_target_name[0] ? s_target_name : "ECandle",
             s_max_slices, s_slice_seconds);
    oled_update();

    /* Pause hashanchor HTTPS to free internal SRAM for BLE */
    hashanchor_set_paused(1);
    ESP_LOGI(TAG, "Hashanchor paused for BLE session");

    /* Use passive scan for WiFi coexistence (per ESP-IDF coex example) */
    struct ble_gap_disc_params params = {
        .filter_duplicates = 1,
        .passive = 1,    /* passive scan: no SCAN_REQ, better WiFi coexistence */
        .itvl = 0,       /* default */
        .window = 0,     /* default */
    };

    /* Stop advertising to free radio + memory for central scan */
    ble_gap_adv_stop();

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    rc = ble_gap_disc(own_addr_type, 15000, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        snprintf(s_result.error, sizeof(s_result.error), "scan: %d", rc);
        s_result.state = BLE_BUYER_ERROR;
        return ESP_FAIL;
    }
    return ESP_OK;
}

const ble_buyer_result_t *ble_buyer_get_result(void)
{
    return &s_result;
}

void ble_buyer_cancel(void)
{
    if (s_result.state == BLE_BUYER_SCANNING) ble_gap_disc_cancel();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    stop_mining();
    s_result.state = BLE_BUYER_IDLE;
    session_ended();
}

#endif /* CONFIG_BT_NIMBLE_ROLE_CENTRAL */
