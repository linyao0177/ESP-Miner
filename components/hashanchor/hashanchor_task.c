#include "hashanchor.h"
#include "global_state.h"
#include "nvs_config.h"
#include "coinbase_decoder.h"
#include "esp_timer.h"

#include "boat_crypto.h"
#include "boat_identity.h"
#include "boat_attest.h"
#include "boat_pay.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "ha_task";

/* Polygon USDC contract address */
static const uint8_t USDC_CONTRACT[20] = {
    0x3c, 0x49, 0x9c, 0x54, 0x2c, 0xEF, 0x5E, 0x38, 0x11, 0xe1,
    0x19, 0x2c, 0xe7, 0x0d, 0x8c, 0xC0, 0x3d, 0x5c, 0x33, 0x59
};

static cJSON *hashanchor_collect(GlobalState *state)
{
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

    /* Stratum / mining pool identity */
    char *stratum_user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER);
    char *stratum_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL);
    if (stratum_user) { cJSON_AddStringToObject(payload, "stratum_user", stratum_user); free(stratum_user); }
    if (stratum_url)  { cJSON_AddStringToObject(payload, "stratum_url", stratum_url); free(stratum_url); }

    /* Network difficulty */
    if (state->network_diff_string[0] != '\0') {
        cJSON_AddStringToObject(payload, "network_diff", state->network_diff_string);
    }

    /* Device uptime in seconds */
    uint32_t uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    cJSON_AddNumberToObject(payload, "uptime_seconds", (double)uptime_sec);

    /* Coinbase outputs (decoded from stratum) */
    if (state->coinbase_output_count > 0) {
        cJSON *outputs = cJSON_CreateArray();
        for (int i = 0; i < state->coinbase_output_count && i < MAX_COINBASE_TX_OUTPUTS; i++) {
            cJSON *out = cJSON_CreateObject();
            cJSON_AddStringToObject(out, "address", state->coinbase_outputs[i].address);
            cJSON_AddNumberToObject(out, "value_satoshis", (double)state->coinbase_outputs[i].value_satoshis);
            cJSON_AddBoolToObject(out, "is_user_output", state->coinbase_outputs[i].is_user_output);
            cJSON_AddItemToArray(outputs, out);
        }
        cJSON_AddItemToObject(payload, "coinbase_outputs", outputs);
    }

    /* Device info */
    char *board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION);
    char *asic_model = nvs_config_get_string(NVS_CONFIG_ASIC_MODEL);
    if (board_version) { cJSON_AddStringToObject(payload, "board_version", board_version); free(board_version); }
    if (asic_model)    { cJSON_AddStringToObject(payload, "asic_model", asic_model); free(asic_model); }

    return payload;
}

static char *build_device_info_json(void)
{
    cJSON *info = cJSON_CreateObject();
    char *board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION);
    char *asic_model = nvs_config_get_string(NVS_CONFIG_ASIC_MODEL);
    char *device_model = nvs_config_get_string(NVS_CONFIG_DEVICE_MODEL);
    char *hostname = nvs_config_get_string(NVS_CONFIG_HOSTNAME);

    if (board_version) { cJSON_AddStringToObject(info, "boardVersion", board_version); free(board_version); }
    if (asic_model)    { cJSON_AddStringToObject(info, "asicModel", asic_model); free(asic_model); }
    if (device_model)  { cJSON_AddStringToObject(info, "deviceModel", device_model); free(device_model); }
    if (hostname)      { cJSON_AddStringToObject(info, "hostname", hostname); free(hostname); }

    char *json = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    return json;
}

void hashanchor_task(void *pvParameters)
{
    GlobalState *state = (GlobalState *)pvParameters;

    ESP_LOGI(TAG, "HashAnchor task started, waiting for WiFi...");

    /* Wait for WiFi connection */
    while (!state->SYSTEM_MODULE.is_connected) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    ESP_LOGI(TAG, "WiFi connected, initializing boat-mwr crypto...");

    /* Initialize dual keypair (Ed25519 + secp256k1) via boat-mwr */
    boat_keypair_t kp;
    if (boat_crypto_init(&kp) != BOAT_OK) {
        ESP_LOGE(TAG, "Failed to initialize boat-mwr crypto, task exiting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "boat-mwr initialized. Ed25519 PK: %s, ETH: %s",
             kp.ed25519_pk_hex, kp.eth_addr_hex);

    /* Persist ETH address to main NVS so HTTP server can expose it */
    nvs_config_set_string(NVS_CONFIG_HASHANCHOR_ETH_ADDRESS, kp.eth_addr_hex);

    /* Initialize EIP-712 domain for Polygon USDC (needed for payment mode) */
    boat_pay_set_domain(137, USDC_CONTRACT);

    /* Cached payment requirements (persists across loop iterations) */
    boat_pay_requirements_t pay_req;
    memset(&pay_req, 0, sizeof(pay_req));

    /* Auto-register device (only when enabled and configured) */
    if (nvs_config_get_bool(NVS_CONFIG_HASHANCHOR_ENABLED)) {
        char *url = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_URL);
        char *api_key = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_API_KEY);
        char *device_id = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_DEVICE_ID);

        if (url && strlen(url) > 0 && api_key && strlen(api_key) > 0 &&
            device_id && strlen(device_id) > 0) {
            boat_config_t cfg = {
                .relay_url = url,
                .api_key = api_key,
                .device_id = device_id,
            };
            char *dev_info = build_device_info_json();
            boat_err_t err = boat_identity_register_ext(&cfg, &kp, dev_info);
            if (err != BOAT_OK) {
                ESP_LOGW(TAG, "Device registration failed (non-fatal)");
            }
            free(dev_info);
        } else {
            ESP_LOGW(TAG, "Cannot register: URL, API key, or device ID not configured");
        }

        free(url);
        free(api_key);
        free(device_id);
    }

    while (1) {
        if (!nvs_config_get_bool(NVS_CONFIG_HASHANCHOR_ENABLED)) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        /* Read NVS config for this iteration */
        char *url = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_URL);
        char *api_key = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_API_KEY);
        char *device_id = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_DEVICE_ID);

        /* Determine payment mode: API key empty → use USDC payment */
        int payment_mode = (!api_key || strlen(api_key) == 0);

        if (!url || strlen(url) == 0) {
            ESP_LOGW(TAG, "HashAnchor URL not configured, skipping");
            free(url); free(api_key); free(device_id);
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        if (!payment_mode && (!api_key || strlen(api_key) == 0)) {
            ESP_LOGW(TAG, "HashAnchor API key not configured, skipping");
            free(url); free(api_key); free(device_id);
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        /* Collect telemetry */
        cJSON *payload = hashanchor_collect(state);
        char *json_str = cJSON_PrintUnformatted(payload);

        if (!json_str) {
            ESP_LOGE(TAG, "Failed to serialize telemetry");
            cJSON_Delete(payload);
            free(url); free(api_key); free(device_id);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Create attestation: SHA-256 hash + Ed25519 sign */
        boat_attestation_t att;
        boat_err_t err = boat_attest_create(&kp, json_str, &att);
        if (err != BOAT_OK) {
            ESP_LOGE(TAG, "Failed to create attestation");
            cJSON_Delete(payload);
            free(json_str);
            free(url); free(api_key); free(device_id);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Submit to HashAnchor with telemetry as metadata */
        char *metadata_json = cJSON_PrintUnformatted(payload);
        boat_config_t cfg = {
            .relay_url = url,
            .api_key = api_key,
            .device_id = device_id,
        };

        if (payment_mode) {
            /* Payment mode: submit with USDC payment (EIP-3009) */
            err = boat_attest_submit_paid(&cfg, &kp, &att, metadata_json, &pay_req);
            if (err == BOAT_OK) {
                ESP_LOGI(TAG, "Paid attestation submitted successfully");
            } else {
                ESP_LOGW(TAG, "Paid attestation submit failed: %d", err);
            }
        } else {
            /* API Key mode: existing flow */
            err = boat_attest_submit(&cfg, &kp, &att, metadata_json);
            if (err != BOAT_OK) {
                ESP_LOGW(TAG, "Attestation submit failed");
            }
        }

        cJSON_Delete(payload);
        free(json_str);
        free(metadata_json);
        free(url);
        free(api_key);
        free(device_id);

        /* Sleep for configured interval */
        uint16_t interval = nvs_config_get_u16(NVS_CONFIG_HASHANCHOR_INTERVAL);
        if (interval < 60) interval = 60;
        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
    }
}
