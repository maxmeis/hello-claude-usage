#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "usage_provider.h"

/* The snapshot is safe for USB management. It never includes credentials or
 * the private device-auth identifier. */
typedef struct {
    bool configured;
    char state[24];
    char verification_url[128];
    char user_code[32];
    int64_t expires_at;
    provider_usage_t usage;
    struct {
        uint32_t id;
        char method[24];
        char state[16];
        char message[96];
    } operation;
} codex_management_status_t;

void codex_adapter_start(EventGroupHandle_t wifi_events);
/* Recognized methods: "codex.begin", "codex.cancel", "codex.refresh".
 * True means accepted for asynchronous work, not that sign-in succeeded. */
bool codex_adapter_request(uint32_t id, const char *method);
bool codex_adapter_refresh(void);
void codex_adapter_get_status(codex_management_status_t *out);
