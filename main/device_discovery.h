#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Call after the default event loop and Wi-Fi station netif exist. */
esp_err_t device_discovery_start(void);
bool device_discovery_ready(void);

/* The caller owns the HTTP server lifecycle and calls these after start/stop. */
esp_err_t device_discovery_http_start(uint16_t port);
void device_discovery_http_stop(void);

/* Writes a complete setup URL, falling back to the supplied IPv4 address. */
bool device_discovery_setup_url(char *out, size_t out_size, const char *route,
                                const char *cap, const char *ipv4);

/* Pure bounded comparison/copy for updating an already displayed setup URL. */
bool device_discovery_update_url(char *current, size_t current_size, const char *next);
