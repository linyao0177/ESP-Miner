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
} hashanchor_diag_t;

const hashanchor_diag_t *hashanchor_get_diag(void);

#endif /* HASHANCHOR_H_ */
