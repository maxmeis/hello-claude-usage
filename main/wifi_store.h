#ifndef WIFI_STORE_H
#define WIFI_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_STORE_ABSENT = 0,
    WIFI_STORE_CONFIGURED = 1,
    WIFI_STORE_FORGOTTEN = 2,
} wifi_store_state_t;

typedef struct {
    wifi_store_state_t state;
    char ssid[33];
    char password[65];
} wifi_store_record_t;

typedef enum {
    WIFI_SOURCE_NONE = 0,
    WIFI_SOURCE_APP,
    WIFI_SOURCE_DRIVER,
    WIFI_SOURCE_FALLBACK,
} wifi_source_t;

/* Pure helpers used by the manager and host tests. */
bool wifi_store_valid_credentials(const char *ssid, const char *password);
wifi_source_t wifi_store_select(const wifi_store_record_t *record,
                                const char *driver_ssid, const char *driver_password,
                                const char *fallback_ssid, const char *fallback_password,
                                wifi_store_record_t *selected);

/* Only this app namespace is touched; claude_probe and driver storage are separate. */
esp_err_t wifi_store_load(wifi_store_record_t *record);
esp_err_t wifi_store_save(const wifi_store_record_t *record);

#endif
