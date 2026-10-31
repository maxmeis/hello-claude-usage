#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef struct {
    uint32_t operation_id;
    bool configured;
    bool connected;
    bool provisioning;
    char state[16];
    char message[96];
} wifi_manager_status_t;

/* Read-only snapshot; no radio calls or credentials leave the Wi-Fi worker. */
void wifi_manager_get_status(wifi_manager_status_t *out);
/* Queues an asynchronous credential trial; saved Wi-Fi remains intact until GOT_IP. */
bool wifi_manager_request_connect(uint32_t operation_id, const char *ssid, const char *password);

#endif
