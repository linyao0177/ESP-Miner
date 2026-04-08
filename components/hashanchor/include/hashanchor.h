#ifndef HASHANCHOR_H_
#define HASHANCHOR_H_

#include "cJSON.h"
#include <stdint.h>

/**
 * Main FreeRTOS task for HashAnchor integration.
 * Uses boat-mwr SDK for crypto, identity registration, and attestation submission.
 * Periodically collects telemetry, hashes, signs, and submits to HashAnchor API.
 */
void hashanchor_task(void *pvParameters);

/* Diagnostic: last attestation result */
typedef struct {
    int last_err;            /* boat_err_t result */
    int last_http_status;    /* HTTP status code from last attempt */
    uint32_t last_attempt;   /* timestamp of last attempt */
    uint32_t attempt_count;  /* total attempts */
    uint32_t success_count;  /* successful submissions */
    char last_response[256]; /* last HTTP response body (truncated) */
    uint32_t restart_check_count; /* how many times restart check ran */
    int restart_found;           /* 1 if "restart":true was found */
    char restart_response[128];  /* response when restart was checked */
} hashanchor_diag_t;

const hashanchor_diag_t *hashanchor_get_diag(void);

/* Pause/resume hashanchor HTTPS operations (for BLE+WiFi coexistence) */
void hashanchor_set_paused(int paused);
int hashanchor_is_paused(void);

#endif /* HASHANCHOR_H_ */
