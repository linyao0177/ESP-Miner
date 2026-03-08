#ifndef CLAW_TASK_H
#define CLAW_TASK_H

#include "global_state.h"
#include "boat_x402.h"

/**
 * Claw Task — AI agent heartbeat & x402 offer handler
 *
 * Runs as a FreeRTOS task at priority 2, reading mining data
 * directly from GlobalState (no HTTP round-trip).
 *
 * Features:
 *   - Periodic heartbeat with anomaly detection (Case A)
 *   - Temperature-based auto-throttle (>75°C → reduce freq)
 *   - Green energy reporting via HashAnchor (Case C)
 *   - Credit archive logging (Case D)
 *   - x402 hashrate rental offer (Case B)
 *   - Telegram notifications
 */
void claw_task(void *pvParameters);

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
