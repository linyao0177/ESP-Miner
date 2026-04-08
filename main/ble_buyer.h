#ifndef BLE_BUYER_H_
#define BLE_BUYER_H_

#include "esp_err.h"
#include "boat_crypto.h"
#include "boat_nano.h"

#ifdef CONFIG_BT_NIMBLE_ROLE_CENTRAL

/* BLE buyer state machine */
typedef enum {
    BLE_BUYER_IDLE = 0,
    BLE_BUYER_SCANNING,
    BLE_BUYER_CONNECTING,
    BLE_BUYER_DISCOVERING,
    BLE_BUYER_READING_INFO,
    BLE_BUYER_NEGOTIATING,       /* sent StreamRequest, waiting StreamOffer */
    BLE_BUYER_STREAMING,         /* actively paying per slice */
    BLE_BUYER_COMPLETE,
    BLE_BUYER_ERROR,
} ble_buyer_state_t;

/* Result of a buy session */
typedef struct {
    ble_buyer_state_t state;
    char seller_addr[43];        /* seller ETH address (payTo) */
    char seller_name[32];        /* BLE device name */
    uint64_t price_per_slice;    /* USDC base units */
    uint32_t slice_seconds;
    uint32_t max_slices;
    uint32_t slices_paid;        /* slices signed and written */
    uint64_t total_paid;         /* total USDC paid */
    float total_wh;              /* total Wh received */
    int mining_active;           /* 1 if BM1370 is running */
    char error[128];
} ble_buyer_result_t;

/* Initialize the BLE buyer (central) module */
esp_err_t ble_buyer_init(const boat_keypair_t *kp);

/* Start a buy session: scan → connect → negotiate → stream */
esp_err_t ble_buyer_start(const char *device_name,
                           uint32_t max_slices,
                           uint32_t slice_seconds);

/* Get current session result */
const ble_buyer_result_t *ble_buyer_get_result(void);

/* Cancel ongoing session */
void ble_buyer_cancel(void);

#else

static inline esp_err_t ble_buyer_init(const boat_keypair_t *kp) { return ESP_OK; }
static inline esp_err_t ble_buyer_start(const char *n, uint32_t m, uint32_t s) { return ESP_OK; }
static inline const void *ble_buyer_get_result(void) { return NULL; }
static inline void ble_buyer_cancel(void) {}

#endif
#endif
