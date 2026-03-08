#ifndef BLE_X402_H_
#define BLE_X402_H_

#include "esp_err.h"
#include "global_state.h"

#ifdef CONFIG_BT_ENABLED

/**
 * Initialize BLE GATT x402 service.
 * Exposes hashrate offer, mining stats, payment write, and receipt notification.
 */
esp_err_t ble_x402_init(GlobalState *global_state);

/**
 * Update mining stats in the BLE stats characteristic.
 * Called periodically from heartbeat.
 */
void ble_x402_update_stats(float hashrate, float temp, float power, uint16_t freq);

/**
 * Send payment receipt notification to connected BLE client.
 */
void ble_x402_notify_receipt(const char *receipt_json);

#else

static inline esp_err_t ble_x402_init(GlobalState *g) { return ESP_OK; }
static inline void ble_x402_update_stats(float h, float t, float p, uint16_t f) {}
static inline void ble_x402_notify_receipt(const char *r) {}

#endif /* CONFIG_BT_ENABLED */

#endif /* BLE_X402_H_ */
