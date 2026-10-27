#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* The manager owns the radio, reconnects, and its temporary setup server. */
esp_err_t wifi_manager_start(const char *fallback_ssid, const char *fallback_password,
                             EventGroupHandle_t *events_out, esp_netif_t **sta_out);

/* The caller must stop the OAuth HTTP server before requesting setup. */
bool wifi_manager_request_forget(void);
bool wifi_manager_is_provisioning(void);

#endif
