#pragma once

#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define OAUTH_WIFI_CONNECTED BIT0
#define OAUTH_WIFI_RECONNECTED BIT1

void oauth_probe_start(EventGroupHandle_t wifi_events, esp_netif_t *sta);
