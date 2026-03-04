#include "hashanchor.h"
#include "tweetnacl.h"
#include "nvs_config.h"

#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ha_crypto";

#define HA_NVS_NAMESPACE "hashanchor"
#define HA_NVS_KEY_SK    "ed25519_sk"
#define HA_NVS_KEY_PK    "ed25519_pk"

/* Ed25519 key sizes */
#define PK_LEN 32
#define SK_LEN 64

static uint8_t s_pk[PK_LEN];
static uint8_t s_sk[SK_LEN];
static char s_pk_hex[PK_LEN * 2 + 1];
static bool s_initialized = false;

/* TweetNaCl requires this function to be provided externally */
void randombytes(unsigned char *buf, unsigned long long len)
{
    esp_fill_random(buf, (size_t)len);
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex_out)
{
    for (size_t i = 0; i < len; i++) {
        sprintf(hex_out + i * 2, "%02x", bytes[i]);
    }
    hex_out[len * 2] = '\0';
}

esp_err_t hashanchor_crypto_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(HA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", HA_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    /* Try to load existing keypair from NVS */
    size_t sk_len = SK_LEN;
    size_t pk_len = PK_LEN;
    esp_err_t sk_err = nvs_get_blob(nvs, HA_NVS_KEY_SK, s_sk, &sk_len);
    esp_err_t pk_err = nvs_get_blob(nvs, HA_NVS_KEY_PK, s_pk, &pk_len);

    if (sk_err == ESP_OK && pk_err == ESP_OK && sk_len == SK_LEN && pk_len == PK_LEN) {
        ESP_LOGI(TAG, "Loaded Ed25519 keypair from NVS");
    } else {
        /* Generate new keypair */
        ESP_LOGI(TAG, "Generating new Ed25519 keypair...");
        crypto_sign_keypair(s_pk, s_sk);

        /* Store to NVS */
        err = nvs_set_blob(nvs, HA_NVS_KEY_SK, s_sk, SK_LEN);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store secret key: %s", esp_err_to_name(err));
            nvs_close(nvs);
            return err;
        }
        err = nvs_set_blob(nvs, HA_NVS_KEY_PK, s_pk, PK_LEN);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store public key: %s", esp_err_to_name(err));
            nvs_close(nvs);
            return err;
        }
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
            nvs_close(nvs);
            return err;
        }
        ESP_LOGI(TAG, "New Ed25519 keypair generated and stored");
    }

    nvs_close(nvs);

    /* Build hex string for public key */
    bytes_to_hex(s_pk, PK_LEN, s_pk_hex);
    ESP_LOGI(TAG, "Device public key: %s", s_pk_hex);

    s_initialized = true;
    return ESP_OK;
}

const char *hashanchor_get_public_key_hex(void)
{
    return s_pk_hex;
}

void hashanchor_sign(const uint8_t *data, size_t data_len, uint8_t sig_out[64])
{
    /* crypto_sign produces signature || message, we only want the 64-byte signature */
    size_t sm_len = data_len + crypto_sign_BYTES;
    uint8_t *sm = malloc(sm_len);
    if (!sm) {
        ESP_LOGE(TAG, "Failed to allocate memory for signing");
        memset(sig_out, 0, 64);
        return;
    }

    unsigned long long smlen_out;
    crypto_sign(sm, &smlen_out, data, data_len, s_sk);

    /* First 64 bytes of sm are the signature */
    memcpy(sig_out, sm, 64);
    free(sm);
}
