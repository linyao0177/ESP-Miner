#ifndef BLE_BUYER_H_
#define BLE_BUYER_H_

#include "esp_err.h"
#include "boat_crypto.h"
#include "boat_nano.h"

#ifdef CONFIG_BT_NIMBLE_ROLE_CENTRAL

/* BLE buyer state machine (Demo: auto price + buy loop) */
typedef enum {
    BLE_BUYER_IDLE = 0,
    BLE_BUYER_SCANNING,          /* scanning for ECandle, reading price */
    BLE_BUYER_WAITING,           /* price too high, waiting for drop */
    BLE_BUYER_CONNECTING,
    BLE_BUYER_DISCOVERING,
    BLE_BUYER_READING_INFO,
    BLE_BUYER_NEGOTIATING,
    BLE_BUYER_STREAMING,         /* actively paying per slice + mining */
    BLE_BUYER_DECIDING,          /* session done, checking price */
    BLE_BUYER_COMPLETE,
    BLE_BUYER_ERROR,
} ble_buyer_state_t;

/* Session result + pricing info */
typedef struct {
    ble_buyer_state_t state;
    char seller_addr[43];        /* seller ETH address (payTo) */
    char seller_name[32];        /* BLE device name */
    uint64_t price_per_slice;    /* USDC base units from advertising/offer */
    uint64_t threshold;          /* profitability threshold (USDC base units/slice) */
    uint32_t slice_seconds;
    uint32_t max_slices;
    uint32_t slices_paid;        /* slices in current session */
    uint32_t total_sessions;     /* completed buy sessions */
    uint64_t total_paid;         /* total USDC paid (all sessions) */
    float total_wh;              /* total Wh received (all sessions) */
    int mining_active;           /* 1 if BM1370 is running */
    int auto_mode;               /* 1 if autonomous loop is running */
    int last_write_rc;           /* rc from last ble_gattc_write_flat */
    int last_write_len;          /* bytes written in last SlicePayment */
    int last_mtu;                /* negotiated MTU */
    uint16_t h_payment;          /* discovered SlicePayment handle */
    uint16_t h_request;          /* discovered StreamRequest handle */
    uint16_t svc_start;          /* 0xEE00 service start handle */
    uint16_t svc_end;            /* 0xEE00 service end handle */
    uint8_t seller_available_wh; /* from advertising */
    uint8_t seller_state;        /* from advertising (0=IDLE) */
    char error[128];
} ble_buyer_result_t;

/* Initialize the BLE buyer module */
esp_err_t ble_buyer_init(const boat_keypair_t *kp);

/*
 * Start autonomous buy loop (Demo mode):
 * Continuously scan → check price → buy if profitable → repeat.
 * Stops only when cancelled or error.
 */
esp_err_t ble_buyer_start_auto(const char *device_name,
                                uint64_t threshold,
                                uint32_t max_slices,
                                uint32_t slice_seconds);

/* Start a single buy session (original behavior) */
esp_err_t ble_buyer_start(const char *device_name,
                           uint32_t max_slices,
                           uint32_t slice_seconds);

/* Get current session result */
const ble_buyer_result_t *ble_buyer_get_result(void);

/* Cancel ongoing session / auto loop */
void ble_buyer_cancel(void);

#else

static inline esp_err_t ble_buyer_init(const boat_keypair_t *kp) { return ESP_OK; }
static inline esp_err_t ble_buyer_start_auto(const char *n, uint64_t t, uint32_t m, uint32_t s) { return ESP_OK; }
static inline esp_err_t ble_buyer_start(const char *n, uint32_t m, uint32_t s) { return ESP_OK; }
static inline const void *ble_buyer_get_result(void) { return NULL; }
static inline void ble_buyer_cancel(void) {}

#endif
#endif
