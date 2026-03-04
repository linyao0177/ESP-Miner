#include "hashanchor.h"
#include "global_state.h"
#include "nvs_config.h"

#include "esp_log.h"
#include "mbedtls/sha256.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "ha_task";

cJSON *hashanchor_collect(void *global_state)
{
    GlobalState *state = (GlobalState *)global_state;

    cJSON *payload = cJSON_CreateObject();

    /* Timestamp */
    time_t now;
    time(&now);
    cJSON_AddNumberToObject(payload, "timestamp", (double)now);

    /* Mining stats from SYSTEM_MODULE */
    cJSON_AddNumberToObject(payload, "hashrate", state->SYSTEM_MODULE.current_hashrate);
    cJSON_AddNumberToObject(payload, "hashrate_1m", state->SYSTEM_MODULE.hashrate_1m);
    cJSON_AddNumberToObject(payload, "hashrate_10m", state->SYSTEM_MODULE.hashrate_10m);
    cJSON_AddNumberToObject(payload, "hashrate_1h", state->SYSTEM_MODULE.hashrate_1h);
    cJSON_AddNumberToObject(payload, "shares_accepted", (double)state->SYSTEM_MODULE.shares_accepted);
    cJSON_AddNumberToObject(payload, "shares_rejected", (double)state->SYSTEM_MODULE.shares_rejected);
    cJSON_AddNumberToObject(payload, "best_diff", (double)state->SYSTEM_MODULE.best_nonce_diff);
    cJSON_AddNumberToObject(payload, "best_session_diff", (double)state->SYSTEM_MODULE.best_session_nonce_diff);

    /* Power/thermal from POWER_MANAGEMENT_MODULE */
    cJSON_AddNumberToObject(payload, "power", state->POWER_MANAGEMENT_MODULE.power);
    cJSON_AddNumberToObject(payload, "voltage", state->POWER_MANAGEMENT_MODULE.voltage);
    cJSON_AddNumberToObject(payload, "current", state->POWER_MANAGEMENT_MODULE.current);
    cJSON_AddNumberToObject(payload, "chip_temp", state->POWER_MANAGEMENT_MODULE.chip_temp_avg);
    cJSON_AddNumberToObject(payload, "vr_temp", state->POWER_MANAGEMENT_MODULE.vr_temp);
    cJSON_AddNumberToObject(payload, "fan_rpm", state->POWER_MANAGEMENT_MODULE.fan_rpm);
    cJSON_AddNumberToObject(payload, "fan_percent", state->POWER_MANAGEMENT_MODULE.fan_perc);
    cJSON_AddNumberToObject(payload, "frequency", state->POWER_MANAGEMENT_MODULE.frequency_value);

    /* Pool info */
    cJSON_AddNumberToObject(payload, "pool_difficulty", state->pool_difficulty);
    cJSON_AddNumberToObject(payload, "block_height", state->block_height);

    /* Device info */
    char *board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION);
    char *asic_model = nvs_config_get_string(NVS_CONFIG_ASIC_MODEL);
    if (board_version) { cJSON_AddStringToObject(payload, "board_version", board_version); free(board_version); }
    if (asic_model)    { cJSON_AddStringToObject(payload, "asic_model", asic_model); free(asic_model); }

    return payload;
}

void hashanchor_task(void *pvParameters)
{
    GlobalState *state = (GlobalState *)pvParameters;

    ESP_LOGI(TAG, "HashAnchor task started, waiting for WiFi...");

    /* Wait for WiFi connection */
    while (!state->SYSTEM_MODULE.is_connected) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    ESP_LOGI(TAG, "WiFi connected, initializing crypto...");

    /* Initialize Ed25519 keypair (load from NVS or generate new) */
    if (hashanchor_crypto_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize crypto, task exiting");
        vTaskDelete(NULL);
        return;
    }

    /* Wait for initial config before attempting registration */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Attempt device registration (non-fatal if it fails) */
    if (nvs_config_get_bool(NVS_CONFIG_HASHANCHOR_ENABLED)) {
        esp_err_t reg_err = hashanchor_register_device(state);
        if (reg_err != ESP_OK) {
            ESP_LOGW(TAG, "Device registration failed, will retry on next cycle");
        }
    }

    bool registered = false;

    while (1) {
        if (!nvs_config_get_bool(NVS_CONFIG_HASHANCHOR_ENABLED)) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            registered = false;
            continue;
        }

        /* Retry registration if not done yet */
        if (!registered) {
            if (hashanchor_register_device(state) == ESP_OK) {
                registered = true;
            } else {
                ESP_LOGW(TAG, "Registration retry failed, will try again next cycle");
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
        }

        /* 1. Collect telemetry data from GlobalState */
        cJSON *payload = hashanchor_collect(state);
        char *json_str = cJSON_PrintUnformatted(payload);

        if (!json_str) {
            ESP_LOGE(TAG, "Failed to serialize telemetry");
            cJSON_Delete(payload);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        ESP_LOGD(TAG, "Telemetry: %s", json_str);

        /* 2. SHA-256 hash (mbedTLS hardware accelerated on ESP32-S3) */
        uint8_t hash[32];
        mbedtls_sha256((const unsigned char *)json_str, strlen(json_str), hash, 0);

        /* 3. Ed25519 sign the hash */
        uint8_t sig[64];
        hashanchor_sign(hash, 32, sig);

        /* 4. Submit to HashAnchor API */
        esp_err_t submit_err = hashanchor_submit(hash, sig, payload);
        if (submit_err == ESP_OK) {
            char hash_hex[9];
            for (int i = 0; i < 4; i++) sprintf(hash_hex + i * 2, "%02x", hash[i]);
            hash_hex[8] = '\0';
            ESP_LOGI(TAG, "Submitted hash 0x%s...", hash_hex);
        }

        cJSON_Delete(payload);
        free(json_str);

        /* Sleep for configured interval */
        uint16_t interval = nvs_config_get_u16(NVS_CONFIG_HASHANCHOR_INTERVAL);
        if (interval < 60) interval = 60; /* Minimum 60 seconds */
        ESP_LOGD(TAG, "Next submission in %d seconds", interval);
        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
    }
}
