#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "usage_provider.h"

#define OAUTH_WIFI_CONNECTED BIT0
#define OAUTH_WIFI_RECONNECTED BIT1

void oauth_probe_start(EventGroupHandle_t wifi_events, esp_netif_t *sta);

/* Public USB-management snapshot. Never contains a token, PKCE verifier,
 * submitted code, Wi-Fi password, or capability URL. */
typedef struct {
    bool configured;
    char state[16];
    char session[48];
    char authorization_url[512];
    struct {
        char state[16];
        bool wifi, ip, clock, dns, https;
        char message[96];
    } network;
    struct {
        uint32_t id;
        char method[24], state[16], message[96];
    } operation;
    provider_usage_t usage;
} oauth_management_status_t;

void oauth_probe_get_management_status(oauth_management_status_t *out);
bool oauth_probe_management_request(uint32_t id, const char *method,
                                    const char *arg1, const char *arg2);
