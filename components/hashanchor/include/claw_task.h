#ifndef CLAW_TASK_H
#define CLAW_TASK_H

#include "global_state.h"
#include "boat_types.h"
#include "boat_attest.h"

/**
 * Initialize claw agent: register x402 offer services.
 * Called once from hashanchor_task after crypto init.
 */
void claw_init(boat_keypair_t *kp, GlobalState *state);

/**
 * Run one claw heartbeat cycle (Cases A/C/D + Telegram).
 * Called from hashanchor_task main loop after attestation submit,
 * sharing the same TLS session to avoid mbedtls memory exhaustion.
 */
void claw_heartbeat(GlobalState *state,
                     boat_keypair_t *kp,
                     boat_config_t *cfg,
                     boat_pay_requirements_t *pay_req);

/**
 * Handle an x402 offer request from the HTTP server.
 * Called by the HTTP handler when a request hits /api/x402/.
 *
 * @param path        The request path (e.g. "/hashrate")
 * @param x_payment   The X-PAYMENT header value, or NULL for initial 402
 * @param out_body    Output: response body (caller must free)
 * @param out_len     Output: response body length
 * @return            HTTP status code: 200, 402, or 500
 */
int claw_x402_handle_request(const char *path,
                              const char *x_payment,
                              char **out_body,
                              size_t *out_len);

#endif /* CLAW_TASK_H */
