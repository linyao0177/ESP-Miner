#include "hashanchor.h"
#include "nvs_config.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ha_http";

#define HTTP_TIMEOUT_MS 10000
#define MAX_RESPONSE_LEN 2048

typedef struct {
    char *buffer;
    int len;
    int max_len;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    if (!resp) return ESP_OK;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (resp->len + evt->data_len < resp->max_len) {
                memcpy(resp->buffer + resp->len, evt->data, evt->data_len);
                resp->len += evt->data_len;
                resp->buffer[resp->len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex_out)
{
    for (size_t i = 0; i < len; i++) {
        sprintf(hex_out + i * 2, "%02x", bytes[i]);
    }
    hex_out[len * 2] = '\0';
}

esp_err_t hashanchor_submit(const uint8_t hash[32], const uint8_t signature[64], const cJSON *metadata)
{
    char *url = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_URL);
    char *api_key = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_API_KEY);
    char *device_id = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_DEVICE_ID);

    if (!url || strlen(url) == 0 || !api_key || strlen(api_key) == 0) {
        ESP_LOGW(TAG, "HashAnchor URL or API key not configured");
        free(url);
        free(api_key);
        free(device_id);
        return ESP_FAIL;
    }

    /* Build endpoint URL */
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "%s/v1/device/submit-signed", url);

    /* Convert hash and signature to hex strings */
    char hash_hex[67]; /* "0x" + 64 hex chars + null */
    char sig_hex[129];  /* 128 hex chars + null */
    hash_hex[0] = '0';
    hash_hex[1] = 'x';
    bytes_to_hex(hash, 32, hash_hex + 2);
    bytes_to_hex(signature, 64, sig_hex);

    /* Build JSON body matching SubmitSignedHashBody schema */
    cJSON *body = cJSON_CreateObject();
    if (device_id && strlen(device_id) > 0) {
        cJSON_AddStringToObject(body, "deviceId", device_id);
    }
    cJSON_AddStringToObject(body, "hash", hash_hex);
    cJSON_AddStringToObject(body, "signature", sig_hex);
    cJSON_AddStringToObject(body, "algorithm", "ed25519");

    /* Metadata contains telemetry only */
    if (metadata) {
        cJSON *meta = cJSON_Duplicate(metadata, 1);
        cJSON_AddItemToObject(body, "metadata", meta);
    }

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        free(url);
        free(api_key);
        free(device_id);
        return ESP_FAIL;
    }

    /* HTTP POST */
    char resp_buf[MAX_RESPONSE_LEN];
    http_response_t resp = { .buffer = resp_buf, .len = 0, .max_len = MAX_RESPONSE_LEN };

    esp_http_client_config_t config = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(json_str);
        free(url);
        free(api_key);
        free(device_id);
        return ESP_FAIL;
    }

    /* Set headers */
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && (status == 200 || status == 201)) {
        ESP_LOGI(TAG, "Hash submitted successfully (status=%d)", status);
    } else {
        ESP_LOGW(TAG, "Hash submission failed: err=%s, status=%d, response=%s",
                 esp_err_to_name(err), status, resp.len > 0 ? resp.buffer : "(empty)");
        err = ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    free(json_str);
    free(url);
    free(api_key);
    free(device_id);

    return err;
}

esp_err_t hashanchor_register_device(void *global_state)
{
    char *url = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_URL);
    char *api_key = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_API_KEY);
    char *device_id = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_DEVICE_ID);

    if (!url || strlen(url) == 0 || !api_key || strlen(api_key) == 0 ||
        !device_id || strlen(device_id) == 0) {
        ESP_LOGW(TAG, "Cannot register device: URL, API key, or device ID not configured");
        free(url);
        free(api_key);
        free(device_id);
        return ESP_FAIL;
    }

    /* Build endpoint URL */
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "%s/v1/device/register", url);

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "deviceId", device_id);
    cJSON_AddStringToObject(body, "publicKey", hashanchor_get_public_key_hex());
    cJSON_AddStringToObject(body, "algorithm", "ed25519");

    /* Add device info */
    char *board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION);
    char *asic_model = nvs_config_get_string(NVS_CONFIG_ASIC_MODEL);
    char *device_model = nvs_config_get_string(NVS_CONFIG_DEVICE_MODEL);
    char *hostname = nvs_config_get_string(NVS_CONFIG_HOSTNAME);

    cJSON *device_info = cJSON_CreateObject();
    if (board_version) { cJSON_AddStringToObject(device_info, "boardVersion", board_version); free(board_version); }
    if (asic_model)    { cJSON_AddStringToObject(device_info, "asicModel", asic_model); free(asic_model); }
    if (device_model)  { cJSON_AddStringToObject(device_info, "deviceModel", device_model); free(device_model); }
    if (hostname)      { cJSON_AddStringToObject(device_info, "hostname", hostname); free(hostname); }
    cJSON_AddItemToObject(body, "deviceInfo", device_info);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        free(url);
        free(api_key);
        free(device_id);
        return ESP_FAIL;
    }

    /* HTTP POST */
    char resp_buf[MAX_RESPONSE_LEN];
    http_response_t resp = { .buffer = resp_buf, .len = 0, .max_len = MAX_RESPONSE_LEN };

    esp_http_client_config_t config = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(json_str);
        free(url);
        free(api_key);
        free(device_id);
        return ESP_FAIL;
    }

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && (status == 200 || status == 201)) {
        ESP_LOGI(TAG, "Device registered successfully: %s", device_id);
    } else if (err == ESP_OK && status == 409) {
        ESP_LOGI(TAG, "Device already registered: %s", device_id);
        err = ESP_OK; /* Not an error */
    } else {
        ESP_LOGW(TAG, "Device registration failed: err=%s, status=%d, response=%s",
                 esp_err_to_name(err), status, resp.len > 0 ? resp.buffer : "(empty)");
        err = ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    free(json_str);
    free(url);
    free(api_key);
    free(device_id);

    return err;
}
