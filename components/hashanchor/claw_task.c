#include "claw_task.h"
#include "hashanchor.h"
#include "global_state.h"
#include "nvs_config.h"
#include "esp_timer.h"

#include "boat_crypto.h"
#include "boat_identity.h"
#include "boat_attest.h"
#include "boat_pay.h"
#include "boat_x402.h"
#include "boat_claw.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

static const char *TAG = "claw";

/* ---- Shared state for x402 offer handler ---- */
static boat_keypair_t *g_claw_kp = NULL;
static GlobalState *g_claw_state = NULL;

/* ================================================================
 * Direct GlobalState → boat_claw_axeos_stats_t
 * No HTTP round-trip — reads in-memory struct directly.
 * ================================================================ */

static void collect_stats_from_global(GlobalState *state,
                                       boat_claw_axeos_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));

    /* Hashrate: GlobalState stores in GH/s, convert to TH/s */
    stats->hashrate_ths = state->SYSTEM_MODULE.hashrate_10m / 1000.0f;
    stats->temperature  = state->POWER_MANAGEMENT_MODULE.chip_temp_avg;
    stats->power_w      = state->POWER_MANAGEMENT_MODULE.power;
    stats->voltage      = state->POWER_MANAGEMENT_MODULE.voltage;
    stats->current      = state->POWER_MANAGEMENT_MODULE.current;
    stats->frequency_mhz = (uint32_t)state->POWER_MANAGEMENT_MODULE.frequency_value;

    stats->shares_accepted = (uint32_t)state->SYSTEM_MODULE.shares_accepted;
    stats->shares_rejected = (uint32_t)state->SYSTEM_MODULE.shares_rejected;

    uint32_t uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    stats->uptime_seconds = uptime_sec;

    stats->best_difficulty = (uint32_t)state->SYSTEM_MODULE.best_session_nonce_diff;
    stats->found_block = state->SYSTEM_MODULE.block_found;

    /* Pool info from NVS (current stratum) */
    char *url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL);
    char *user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER);
    if (url) {
        strncpy(stats->pool_url, url, sizeof(stats->pool_url) - 1);
        free(url);
    }
    if (user) {
        strncpy(stats->pool_user, user, sizeof(stats->pool_user) - 1);
        free(user);
    }
}

/* ================================================================
 * Heartbeat — runs every N seconds
 * Combines Cases A (optimizer), C (green), D (credit)
 * ================================================================ */

static void claw_heartbeat_cycle(GlobalState *state,
                                  boat_keypair_t *kp,
                                  boat_config_t *cfg,
                                  boat_pay_requirements_t *pay_req,
                                  uint32_t interval_sec)
{
    boat_claw_axeos_stats_t stats;
    collect_stats_from_global(state, &stats);

    ESP_LOGI(TAG, "Heartbeat: %.2f TH/s | %.1f°C | %.1fW | %" PRIu32 " MHz",
             stats.hashrate_ths, stats.temperature,
             stats.power_w, stats.frequency_mhz);

    /* ---- Anomaly: temperature >75°C → reduce frequency ---- */
    if (stats.temperature > 75.0f) {
        ESP_LOGW(TAG, "OVERHEAT %.1f°C — reducing frequency", stats.temperature);
        uint32_t safe_freq = stats.frequency_mhz > 50 ?
                             stats.frequency_mhz - 50 : 400;
        if (safe_freq >= 400 && safe_freq <= 700) {
            boat_claw_axeos_set_freq(safe_freq);
        }

        /* Telegram alert */
        char *tg_token = nvs_config_get_string(NVS_CONFIG_CLAW_TELEGRAM_TOKEN);
        char *tg_chat  = nvs_config_get_string(NVS_CONFIG_CLAW_TELEGRAM_CHAT);
        if (tg_token && strlen(tg_token) > 0 && tg_chat && strlen(tg_chat) > 0) {
            char alert[256];
            snprintf(alert, sizeof(alert),
                "*ALERT* Temp %.1f°C > 75°C, freq reduced to %" PRIu32 " MHz",
                stats.temperature, safe_freq);
            boat_claw_telegram_send(tg_token, tg_chat, alert);
        }
        free(tg_token);
        free(tg_chat);
    }

    /* ---- kWh calculation ---- */
    float hours = (float)interval_sec / 3600.0f;
    float kwh = (stats.power_w * hours) / 1000.0f;

    /* ---- Case C: Green energy report ---- */
    if (nvs_config_get_bool(NVS_CONFIG_CLAW_ENABLE_GREEN) && kwh > 0) {
        char green_json[512];
        snprintf(green_json, sizeof(green_json),
            "{\"type\":\"green_energy_report\","
            "\"kwh\":%.6f,"
            "\"power_w\":%.1f,"
            "\"duration_s\":%" PRIu32 ","
            "\"hashrate_ths\":%.2f,"
            "\"device\":\"%s\","
            "\"timestamp\":%" PRIu32 "}",
            kwh, stats.power_w, interval_sec,
            stats.hashrate_ths, kp->eth_addr_hex,
            (uint32_t)time(NULL));

        boat_attestation_t att;
        if (boat_attest_create(kp, green_json, &att) == BOAT_OK && cfg) {
            boat_attest_submit_paid(cfg, kp, &att, green_json, pay_req);
            ESP_LOGI(TAG, "Green report: %.4f kWh submitted", kwh);
        }
    }

    /* ---- Case D: Credit archive heartbeat ---- */
    if (nvs_config_get_bool(NVS_CONFIG_CLAW_ENABLE_CREDIT)) {
        char credit_json[512];
        snprintf(credit_json, sizeof(credit_json),
            "{\"type\":\"device_heartbeat\","
            "\"hashrate\":%.2f,"
            "\"temp\":%.1f,"
            "\"power\":%.1f,"
            "\"freq\":%" PRIu32 ","
            "\"uptime\":%" PRIu32 ","
            "\"shares_ok\":%" PRIu32 ","
            "\"shares_bad\":%" PRIu32 ","
            "\"kwh\":%.6f,"
            "\"device\":\"%s\","
            "\"timestamp\":%" PRIu32 "}",
            stats.hashrate_ths, stats.temperature,
            stats.power_w, stats.frequency_mhz,
            stats.uptime_seconds,
            stats.shares_accepted, stats.shares_rejected,
            kwh, kp->eth_addr_hex,
            (uint32_t)time(NULL));

        boat_attestation_t att;
        if (boat_attest_create(kp, credit_json, &att) == BOAT_OK && cfg) {
            boat_attest_submit_paid(cfg, kp, &att, credit_json, pay_req);
            ESP_LOGI(TAG, "Credit heartbeat submitted");
        }
    }

    /* ---- Telegram periodic report ---- */
    char *tg_token = nvs_config_get_string(NVS_CONFIG_CLAW_TELEGRAM_TOKEN);
    char *tg_chat  = nvs_config_get_string(NVS_CONFIG_CLAW_TELEGRAM_CHAT);
    if (tg_token && strlen(tg_token) > 0 && tg_chat && strlen(tg_chat) > 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
            "*Bitaxe Heartbeat*\n"
            "Hashrate: %.2f TH/s\n"
            "Temp: %.1f°C\n"
            "Power: %.1fW\n"
            "Freq: %" PRIu32 " MHz\n"
            "kWh: %.4f\n"
            "Uptime: %" PRIu32 "h%" PRIu32 "m\n"
            "Shares: %" PRIu32 " ok / %" PRIu32 " bad",
            stats.hashrate_ths, stats.temperature,
            stats.power_w, stats.frequency_mhz,
            kwh,
            stats.uptime_seconds / 3600,
            (stats.uptime_seconds % 3600) / 60,
            stats.shares_accepted, stats.shares_rejected);
        boat_claw_telegram_send(tg_token, tg_chat, msg);
    }
    free(tg_token);
    free(tg_chat);
}

/* ================================================================
 * x402 Offer: Hashrate Rental (Case B)
 * ================================================================ */

static boat_err_t hashrate_rental_callback(const char *buyer,
                                            const char *tx_hash,
                                            uint64_t amount,
                                            void *user_data)
{
    GlobalState *state = (GlobalState *)user_data;
    (void)state;

    ESP_LOGI(TAG, "Hashrate rental paid by %s (tx: %s, amount: %" PRIu64 ")",
             buyer, tx_hash, amount);

    /* In production: switch pool to buyer's pool for rental duration.
     * For now, log the event. The AI agent (zclaw) would handle
     * the actual pool switch via boat_claw_axeos_set_pool(). */

    /* Telegram notification */
    char *tg_token = nvs_config_get_string(NVS_CONFIG_CLAW_TELEGRAM_TOKEN);
    char *tg_chat  = nvs_config_get_string(NVS_CONFIG_CLAW_TELEGRAM_CHAT);
    if (tg_token && strlen(tg_token) > 0 && tg_chat && strlen(tg_chat) > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "*Hashrate Rental*\n"
            "Buyer: %.10s...\n"
            "Amount: %" PRIu64 "\n"
            "TX: %.10s...",
            buyer, amount, tx_hash);
        boat_claw_telegram_send(tg_token, tg_chat, msg);
    }
    free(tg_token);
    free(tg_chat);

    return BOAT_OK;
}

static void register_x402_services(boat_keypair_t *kp, GlobalState *state)
{
    /* Register hashrate rental service */
    boat_x402_offer_service_t rental = {
        .path = "/hashrate",
        .description = "Bitaxe 601 hashrate rental (1 TH/s, 1 hour)",
        .network = "base-sepolia",
        .asset = "0x036CbD53842c5426634e7929541eC2318f3dCF7e", /* USDC on Base Sepolia */
        .amount = 100000, /* 0.1 USDC */
        .validity_seconds = 3600,
        .on_payment_verified = hashrate_rental_callback,
        .user_data = state,
    };

    boat_err_t err = boat_x402_offer_register(kp, &rental);
    if (err == BOAT_OK) {
        ESP_LOGI(TAG, "x402 offer registered: /hashrate (0.1 USDC)");
    } else {
        ESP_LOGW(TAG, "Failed to register x402 offer: %d", err);
    }
}

/* ================================================================
 * x402 HTTP handler (called from http_server)
 * ================================================================ */

int claw_x402_handle_request(const char *path,
                              const char *x_payment,
                              char **out_body,
                              size_t *out_len)
{
    if (!g_claw_kp || !path || !out_body || !out_len) {
        return 500;
    }

    boat_x402_offer_result_t result;
    boat_err_t err = boat_x402_offer_handle(g_claw_kp, path, x_payment, &result);

    if (err == BOAT_OK || err == BOAT_ERR_PAYMENT_REJECTED) {
        *out_body = result.response_body;  /* caller frees */
        *out_len = result.response_len;
        return result.status_code;
    }

    /* Service not found or error */
    *out_body = NULL;
    *out_len = 0;
    return (err == BOAT_ERR_KEY_NOT_FOUND) ? 404 : 500;
}

/* ================================================================
 * Main Claw Task
 * ================================================================ */

void claw_task(void *pvParameters)
{
    GlobalState *state = (GlobalState *)pvParameters;
    g_claw_state = state;

    ESP_LOGI(TAG, "Claw task started, waiting for WiFi...");

    /* Wait for WiFi */
    while (!state->SYSTEM_MODULE.is_connected) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* Wait a bit for hashanchor_task to initialize crypto first */
    vTaskDelay(pdMS_TO_TICKS(10000));

    ESP_LOGI(TAG, "Initializing claw agent...");

    /* Initialize own keypair (shares NVS keys with hashanchor_task) */
    boat_keypair_t kp;
    if (boat_crypto_init(&kp) != BOAT_OK) {
        ESP_LOGE(TAG, "Failed to init crypto, task exiting");
        vTaskDelete(NULL);
        return;
    }
    g_claw_kp = &kp;

    ESP_LOGI(TAG, "Claw agent ready. ETH: %s", kp.eth_addr_hex);

    /* Polygon USDC contract */
    static const uint8_t USDC_CONTRACT[20] = {
        0x3c, 0x49, 0x9c, 0x54, 0x2c, 0xEF, 0x5E, 0x38, 0x11, 0xe1,
        0x19, 0x2c, 0xe7, 0x0d, 0x8c, 0xC0, 0x3d, 0x5c, 0x33, 0x59
    };
    boat_pay_set_domain(137, USDC_CONTRACT);

    /* Cached payment requirements */
    boat_pay_requirements_t pay_req;
    memset(&pay_req, 0, sizeof(pay_req));

    /* Register x402 offer services (Case B: hashrate rental) */
    if (nvs_config_get_bool(NVS_CONFIG_CLAW_ENABLE_RENTAL)) {
        register_x402_services(&kp, state);
    }

    /* Main heartbeat loop */
    while (1) {
        if (!nvs_config_get_bool(NVS_CONFIG_CLAW_ENABLED)) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        /* Build config from NVS */
        char *url = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_URL);
        char *api_key = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_API_KEY);
        char *device_id = nvs_config_get_string(NVS_CONFIG_HASHANCHOR_DEVICE_ID);

        boat_config_t cfg = {0};
        if (url && strlen(url) > 0) {
            cfg.relay_url = url;
            cfg.api_key = api_key ? api_key : "";
            cfg.device_id = device_id ? device_id : "";
        }

        uint16_t interval = nvs_config_get_u16(NVS_CONFIG_CLAW_INTERVAL);
        if (interval < 60) interval = 1800; /* default 30 min */

        /* Run heartbeat */
        claw_heartbeat_cycle(state, &kp,
                             (url && strlen(url) > 0) ? &cfg : NULL,
                             &pay_req, interval);

        free(url);
        free(api_key);
        free(device_id);

        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
    }
}
