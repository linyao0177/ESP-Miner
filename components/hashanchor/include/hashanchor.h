#ifndef HASHANCHOR_H_
#define HASHANCHOR_H_

#include "cJSON.h"

/**
 * Main FreeRTOS task for HashAnchor integration.
 * Uses boat-mwr SDK for crypto, identity registration, and attestation submission.
 * Periodically collects telemetry, hashes, signs, and submits to HashAnchor API.
 */
void hashanchor_task(void *pvParameters);

#endif /* HASHANCHOR_H_ */
