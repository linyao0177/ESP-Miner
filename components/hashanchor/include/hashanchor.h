#ifndef HASHANCHOR_H_
#define HASHANCHOR_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/* NVS config keys (defined in nvs_config.h) */
/* NVS_CONFIG_HASHANCHOR_URL      - STR  - HashAnchor API URL */
/* NVS_CONFIG_HASHANCHOR_API_KEY  - STR  - API Key (ha_xxx) */
/* NVS_CONFIG_HASHANCHOR_ENABLED  - BOOL - Enable switch */
/* NVS_CONFIG_HASHANCHOR_INTERVAL - U16  - Report interval (seconds) */
/* NVS_CONFIG_HASHANCHOR_DEVICE_ID - STR - Device ID */

/**
 * Main FreeRTOS task for HashAnchor integration.
 * Periodically collects telemetry, hashes, signs, and submits to HashAnchor API.
 * Should be started with xTaskCreateWithCaps on PSRAM at priority 3.
 */
void hashanchor_task(void *pvParameters);

/**
 * Initialize Ed25519 keypair from NVS, or generate a new one if not present.
 * Must be called once before hashanchor_sign().
 */
esp_err_t hashanchor_crypto_init(void);

/**
 * Get the public key as a hex string (64 chars + null terminator).
 * Returns pointer to static buffer - do not free.
 */
const char *hashanchor_get_public_key_hex(void);

/**
 * Sign data with the device Ed25519 private key.
 * @param data     Data to sign
 * @param data_len Length of data
 * @param sig_out  Output buffer for 64-byte signature
 */
void hashanchor_sign(const uint8_t *data, size_t data_len, uint8_t sig_out[64]);

/**
 * Collect telemetry from GlobalState into a cJSON object.
 * Caller must cJSON_Delete the result.
 */
cJSON *hashanchor_collect(void *global_state);

/**
 * Submit a signed hash to the HashAnchor API.
 * @param hash      32-byte SHA-256 hash
 * @param signature 64-byte Ed25519 signature
 * @param metadata  JSON metadata payload
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t hashanchor_submit(const uint8_t hash[32], const uint8_t signature[64], const cJSON *metadata);

/**
 * Register the device with the HashAnchor API.
 * Sends the device's public key so the server can verify future signatures.
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t hashanchor_register_device(void *global_state);

#endif /* HASHANCHOR_H_ */
