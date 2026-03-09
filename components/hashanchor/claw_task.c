#include "claw_task.h"
#include "global_state.h"
#include "nvs_config.h"
#include "esp_timer.h"

#include "boat_crypto.h"
#include "boat_attest.h"
#include "boat_pay.h"
#include "boat_x402.h"
#include "boat_solana.h"
#include "boat_lightning.h"
#include "boat_claw.h"
#include "boat_pal.h"

#include "esp_log.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

static const char *TAG = "claw";

/* ---- Shared state for x402 offer handler ---- */
static boat_keypair_t *g_claw_kp = NULL;

/* ================================================================
 * Telegram via Bridge (avoids direct api.telegram.org from ESP32)
 * POST http://<bridge>/telegram/send with JSON body
 * ================================================================ */

static void claw_send_telegram(const char *message)
{
    char *tg_token = nvs_config_get_string(NVS_CONFIG_CLAW_TELEGRAM_TOKEN);
    char *tg_chat  = nvs_config_get_string(NVS_CONFIG_CLAW_TELEGRAM_CHAT);
    char *bridge   = nvs_config_get_string(NVS_CONFIG_CLAW_BRIDGE_URL);

    if (!tg_token || strlen(tg_token) == 0 || !tg_chat || strlen(tg_chat) == 0) {
        goto cleanup;
    }

    if (bridge && strlen(bridge) > 0) {
        /* Route via Python Bridge (LAN HTTP, no TLS needed) */
        char url[256];
        snprintf(url, sizeof(url), "%s/telegram/send", bridge);

        /* Simple JSON escape for message */
        char *safe = (char *)malloc(strlen(message) * 2 + 1);
        if (!safe) goto cleanup;
        size_t j = 0;
        for (size_t i = 0; message[i]; i++) {
            if (message[i] == '"') { safe[j++] = '\\'; safe[j++] = '"'; }
            else if (message[i] == '\n') { safe[j++] = '\\'; safe[j++] = 'n'; }
            else { safe[j++] = message[i]; }
        }
        safe[j] = '\0';

        char *body = (char *)malloc(strlen(safe) + 256);
        if (!body) { free(safe); goto cleanup; }
        snprintf(body, strlen(safe) + 256,
            "{\"bot_token\":\"%s\",\"chat_id\":\"%s\",\"text\":\"%s\"}",
            tg_token, tg_chat, safe);
        free(safe);

        int status = 0;
        char resp[128] = {0};
        boat_err_t err = boat_pal_http_post(url, NULL, "application/json",
                                             body, &status, resp, sizeof(resp));
        free(body);

        if (err == BOAT_OK && status == 200) {
            ESP_LOGI(TAG, "Telegram sent via bridge");
        } else {
            ESP_LOGW(TAG, "Telegram via bridge failed: HTTP %d", status);
        }
    } else {
        /* Direct (may fail in regions blocking api.telegram.org) */
        boat_claw_telegram_send(tg_token, tg_chat, message);
    }

cleanup:
    free(tg_token);
    free(tg_chat);
    free(bridge);
}

/* ================================================================
 * Direct GlobalState -> boat_claw_axeos_stats_t
 * ================================================================ */

static void collect_stats_from_global(GlobalState *state,
                                       boat_claw_axeos_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));

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
 * x402 Offer: Hashrate Rental (Case B)
 * ================================================================ */

static boat_err_t hashrate_rental_callback(const char *buyer,
                                            const char *tx_hash,
                                            uint64_t amount,
                                            void *user_data)
{
    (void)user_data;

    ESP_LOGI(TAG, "Hashrate rental paid by %s (tx: %s, amount: %" PRIu64 ")",
             buyer, tx_hash, amount);

    char msg[256];
    snprintf(msg, sizeof(msg),
        "*Hashrate Rental*\n"
        "Buyer: %.10s...\n"
        "Amount: %" PRIu64 "\n"
        "TX: %.10s...",
        buyer, amount, tx_hash);
    claw_send_telegram(msg);

    return BOAT_OK;
}

/* ================================================================
 * Public: Initialize claw (called from hashanchor_task)
 * ================================================================ */

void claw_init(boat_keypair_t *kp, GlobalState *state)
{
    g_claw_kp = kp;

    char *pay_chain = nvs_config_get_string(NVS_CONFIG_CLAW_PAY_CHAIN);
    const char *chain = (pay_chain && strlen(pay_chain) > 0) ? pay_chain : "arc";
    ESP_LOGI(TAG, "Payment chain: %s", chain);

    if (nvs_config_get_bool(NVS_CONFIG_CLAW_ENABLE_RENTAL)) {
        char *x402_network = nvs_config_get_string(NVS_CONFIG_CLAW_X402_NETWORK);
        char *x402_asset   = nvs_config_get_string(NVS_CONFIG_CLAW_X402_ASSET);
        uint64_t x402_amount = nvs_config_get_u64(NVS_CONFIG_CLAW_X402_AMOUNT);
        if (x402_amount == 0) x402_amount = 100000;

        /* Chain-specific defaults for x402 offer */
        const char *default_network = "eip155:84532";
        const char *default_asset = "0x036CbD53842c5426634e7929541eC2318f3dCF7e";
        if (strcmp(chain, "solana") == 0) {
            default_network = "solana:devnet";
            default_asset = "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU";
        } else if (strcmp(chain, "lightning") == 0) {
            default_network = "lightning";
            default_asset = "BTC";
        }

        boat_x402_offer_service_t rental = {
            .path = "/hashrate",
            .description = "Bitaxe 601 hashrate rental (1 TH/s, 1 hour)",
            .network = (x402_network && strlen(x402_network) > 0) ? x402_network : default_network,
            .asset = (x402_asset && strlen(x402_asset) > 0) ? x402_asset : default_asset,
            .amount = x402_amount,
            .validity_seconds = 3600,
            .on_payment_verified = hashrate_rental_callback,
            .user_data = state,
        };

        if (boat_x402_offer_register(kp, &rental) == BOAT_OK) {
            ESP_LOGI(TAG, "x402 offer registered: /hashrate (%" PRIu64 " on %s)",
                     x402_amount, rental.network);
        }
        /* Note: x402_network/x402_asset strings are kept alive —
           they are referenced by the service registry for the device lifetime */
    }

    /* Log Solana address if chain is solana */
    if (strcmp(chain, "solana") == 0) {
        char sol_addr[64];
        boat_base58_encode(kp->ed25519_pk, 32, sol_addr, sizeof(sol_addr));
        ESP_LOGI(TAG, "Solana wallet: %s", sol_addr);
    }

    free(pay_chain);
    ESP_LOGI(TAG, "Claw agent initialized (chain: %s)", chain);
}

/* ================================================================
 * Public: Run one heartbeat cycle (called from hashanchor_task)
 * Executes sequentially after attestation submit, sharing TLS session.
 * ================================================================ */

void claw_heartbeat(GlobalState *state,
                     boat_keypair_t *kp,
                     boat_config_t *cfg,
                     boat_pay_requirements_t *pay_req)
{
    if (!nvs_config_get_bool(NVS_CONFIG_CLAW_ENABLED)) {
        return;
    }

    uint16_t interval = nvs_config_get_u16(NVS_CONFIG_CLAW_INTERVAL);
    if (interval < 60) interval = 1800;

    boat_claw_axeos_stats_t stats;
    collect_stats_from_global(state, &stats);

    ESP_LOGI(TAG, "Heartbeat: %.2f TH/s | %.1f°C | %.1fW | %" PRIu32 " MHz",
             stats.hashrate_ths, stats.temperature,
             stats.power_w, stats.frequency_mhz);

    /* ---- Anomaly: temperature >75C ---- */
    if (stats.temperature > 75.0f) {
        ESP_LOGW(TAG, "OVERHEAT %.1f°C — reducing frequency", stats.temperature);
        uint32_t safe_freq = stats.frequency_mhz > 50 ?
                             stats.frequency_mhz - 50 : 400;
        if (safe_freq >= 400 && safe_freq <= 700) {
            boat_claw_axeos_set_freq(safe_freq);
        }

        char alert[256];
        snprintf(alert, sizeof(alert),
            "*ALERT* Temp %.1f°C > 75°C, freq reduced to %" PRIu32 " MHz",
            stats.temperature, safe_freq);
        claw_send_telegram(alert);
    }

    /* ---- kWh calculation ---- */
    float hours = (float)interval / 3600.0f;
    float kwh = (stats.power_w * hours) / 1000.0f;

    /* ---- Case C: Green energy report ---- */
    if (nvs_config_get_bool(NVS_CONFIG_CLAW_ENABLE_GREEN) && kwh > 0 && cfg) {
        char green_json[512];
        snprintf(green_json, sizeof(green_json),
            "{\"type\":\"green_energy_report\","
            "\"kwh\":%.6f,"
            "\"power_w\":%.1f,"
            "\"duration_s\":%" PRIu32 ","
            "\"hashrate_ths\":%.2f,"
            "\"device\":\"%s\","
            "\"timestamp\":%" PRIu32 "}",
            kwh, stats.power_w, (uint32_t)interval,
            stats.hashrate_ths, kp->eth_addr_hex,
            (uint32_t)time(NULL));

        boat_attestation_t att;
        if (boat_attest_create(kp, green_json, &att) == BOAT_OK) {
            boat_attest_submit_paid(cfg, kp, &att, green_json, pay_req);
            ESP_LOGI(TAG, "Green report: %.4f kWh submitted", kwh);
        }
    }

    /* ---- Case D: Credit archive heartbeat ---- */
    if (nvs_config_get_bool(NVS_CONFIG_CLAW_ENABLE_CREDIT) && cfg) {
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
        if (boat_attest_create(kp, credit_json, &att) == BOAT_OK) {
            boat_attest_submit_paid(cfg, kp, &att, credit_json, pay_req);
            ESP_LOGI(TAG, "Credit heartbeat submitted");
        }
    }

    /* ---- Telegram periodic report ---- */
    {
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
        claw_send_telegram(msg);
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
        *out_body = result.response_body;
        *out_len = result.response_len;
        return result.status_code;
    }

    *out_body = NULL;
    *out_len = 0;
    return (err == BOAT_ERR_KEY_NOT_FOUND) ? 404 : 500;
}

/* ================================================================
 * Solana: device wallet info + test payment
 * GET  /api/solana/info   → returns device Solana address
 * POST /api/solana/pay    → test USDC transfer (devnet)
 * ================================================================ */

int claw_solana_info(char **out_body, size_t *out_len)
{
    if (!g_claw_kp || !out_body || !out_len) return 500;

    /* Derive Solana address (= base58 of Ed25519 public key) */
    char sol_addr[64];
    boat_base58_encode(g_claw_kp->ed25519_pk, 32, sol_addr, sizeof(sol_addr));

    /* Derive ATAs for both networks */
    char ata_mainnet[64], ata_devnet[64];
    boat_sol_pubkey_t ata;

    boat_sol_derive_ata(g_claw_kp->ed25519_pk, BOAT_SOL_USDC_MINT_MAINNET, ata);
    boat_base58_encode(ata, 32, ata_mainnet, sizeof(ata_mainnet));

    boat_sol_derive_ata(g_claw_kp->ed25519_pk, BOAT_SOL_USDC_MINT_DEVNET, ata);
    boat_base58_encode(ata, 32, ata_devnet, sizeof(ata_devnet));

    char *body = (char *)malloc(512);
    if (!body) return 500;
    int len = snprintf(body, 512,
        "{\"solana_address\":\"%s\","
        "\"eth_address\":\"%s\","
        "\"ata_mainnet\":\"%s\","
        "\"ata_devnet\":\"%s\","
        "\"did\":\"%s\"}",
        sol_addr, g_claw_kp->eth_addr_hex,
        ata_mainnet, ata_devnet,
        g_claw_kp->did);

    *out_body = body;
    *out_len = (size_t)len;
    return 200;
}

int claw_solana_pay(const char *recipient_b58, uint64_t amount,
                     int devnet, char **out_body, size_t *out_len)
{
    if (!g_claw_kp || !recipient_b58 || !out_body || !out_len) return 500;

    /* Decode recipient */
    boat_sol_payment_t payment;
    int dec_len = boat_base58_decode(recipient_b58, payment.recipient, 32);
    if (dec_len != 32) {
        char *body = strdup("{\"error\":\"invalid recipient address\"}");
        *out_body = body; *out_len = strlen(body);
        return 400;
    }
    payment.amount = amount;
    payment.devnet = devnet;

    /* Execute payment */
    char tx_sig[128] = {0};
    boat_err_t err = boat_sol_pay_usdc(g_claw_kp, &payment, 1,
                                        tx_sig, sizeof(tx_sig));

    char *body = (char *)malloc(256);
    if (!body) return 500;

    if (err == BOAT_OK) {
        int len = snprintf(body, 256,
            "{\"status\":\"success\",\"tx_signature\":\"%s\","
            "\"network\":\"%s\",\"amount\":%" PRIu64 "}",
            tx_sig, devnet ? "devnet" : "mainnet", amount);
        *out_body = body; *out_len = (size_t)len;
        ESP_LOGI(TAG, "Solana payment sent: %s", tx_sig);
        return 200;
    } else {
        int len = snprintf(body, 256,
            "{\"error\":\"payment failed\",\"code\":%d}", (int)err);
        *out_body = body; *out_len = (size_t)len;
        ESP_LOGE(TAG, "Solana payment failed: %d", (int)err);
        return 500;
    }
}

/* ================================================================
 * Lightning: create invoice + check payment
 * POST /api/lightning/invoice   → create BOLT11 invoice
 * GET  /api/lightning/check     → check payment status
 * ================================================================ */

int claw_lightning_invoice(uint64_t amount_sats, const char *memo,
                            char **out_body, size_t *out_len)
{
    if (!out_body || !out_len) return 500;

    /* Read LNbits config from NVS */
    char *lnbits_url = nvs_config_get_string(NVS_CONFIG_CLAW_BRIDGE_URL);
    /* Reuse bridge URL as LNbits proxy, or add dedicated NVS keys */

    if (!lnbits_url || strlen(lnbits_url) == 0) {
        free(lnbits_url);
        char *body = strdup("{\"error\":\"LNbits not configured. "
            "Set claw_bridge_url to your LNbits URL\"}");
        *out_body = body; *out_len = strlen(body);
        return 400;
    }

    /* For now, use bridge as LNbits proxy endpoint */
    /* The bridge will forward /lightning/invoice to LNbits API */
    char url[256];
    snprintf(url, sizeof(url), "%s/lightning/invoice", lnbits_url);
    free(lnbits_url);

    char req_body[256];
    snprintf(req_body, sizeof(req_body),
        "{\"amount\":%" PRIu64 ",\"memo\":\"%s\"}",
        amount_sats, memo ? memo : "Bitaxe hashrate");

    char response[1024] = {0};
    int status = 0;
    boat_err_t err = boat_pal_http_post(url, NULL, "application/json",
                                         req_body, &status, response,
                                         sizeof(response));

    if (err == BOAT_OK && (status == 200 || status == 201)) {
        char *body = strdup(response);
        *out_body = body; *out_len = strlen(body);
        ESP_LOGI(TAG, "Lightning invoice created: %" PRIu64 " sats", amount_sats);
        return 200;
    } else {
        char *body = (char *)malloc(256);
        int len = snprintf(body, 256,
            "{\"error\":\"invoice creation failed\",\"http_status\":%d}", status);
        *out_body = body; *out_len = (size_t)len;
        return 500;
    }
}

int claw_lightning_check(const char *payment_hash,
                          char **out_body, size_t *out_len)
{
    if (!payment_hash || !out_body || !out_len) return 500;

    char *lnbits_url = nvs_config_get_string(NVS_CONFIG_CLAW_BRIDGE_URL);
    if (!lnbits_url || strlen(lnbits_url) == 0) {
        free(lnbits_url);
        char *body = strdup("{\"error\":\"LNbits not configured\"}");
        *out_body = body; *out_len = strlen(body);
        return 400;
    }

    char url[320];
    snprintf(url, sizeof(url), "%s/lightning/check/%s",
             lnbits_url, payment_hash);
    free(lnbits_url);

    char response[512] = {0};
    int status = 0;
    boat_err_t err = boat_pal_http_post(url, NULL, "application/json",
                                         "", &status, response,
                                         sizeof(response));

    if (err == BOAT_OK && status == 200) {
        char *body = strdup(response);
        *out_body = body; *out_len = strlen(body);
        return 200;
    } else {
        char *body = strdup("{\"error\":\"check failed\"}");
        *out_body = body; *out_len = strlen(body);
        return 500;
    }
}
