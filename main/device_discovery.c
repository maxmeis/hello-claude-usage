#include "device_discovery.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "device_identity.h"

static SemaphoreHandle_t s_lock;
static bool s_started;
static bool s_http_advertised;

static bool hostname_matches(void)
{
    const char *expected = device_identity_name();
    char advertised[MDNS_NAME_BUF_LEN] = {0};
    return s_started && expected && mdns_hostname_get(advertised) == ESP_OK &&
           strcmp(advertised, expected) == 0;
}

esp_err_t device_discovery_start(void)
{
    const char *name = device_identity_name();
    if (!name) return ESP_ERR_INVALID_STATE;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_started) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    esp_err_t err = mdns_init();
    bool initialized = err == ESP_OK;
    if (err == ESP_OK) err = mdns_hostname_set(name);
    if (err == ESP_OK) err = mdns_instance_name_set(name);
    if (err == ESP_OK) {
        s_started = true;
    } else if (initialized) {
        mdns_free();
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool device_discovery_ready(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = hostname_matches();
    xSemaphoreGive(s_lock);
    return ready;
}

esp_err_t device_discovery_http_start(uint16_t port)
{
    if (!port || !s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!hostname_matches()) err = ESP_ERR_INVALID_STATE;
    else if (!s_http_advertised) {
        err = mdns_service_add(NULL, "_http", "_tcp", port, NULL, 0);
        if (err == ESP_OK) s_http_advertised = true;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void device_discovery_http_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_started && s_http_advertised) {
        esp_err_t err = mdns_service_remove("_http", "_tcp");
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            /* A failed removal must not leave a stale HTTP advertisement. */
            mdns_free();
            s_started = false;
        }
        s_http_advertised = false;
    }
    xSemaphoreGive(s_lock);
}

static bool valid_segment(const char *value)
{
    if (!value || !*value) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    }
    return true;
}

bool device_discovery_setup_url(char *out, size_t out_size, const char *route,
                                const char *cap, const char *ipv4)
{
    if (!out || !out_size) return false;
    out[0] = 0;
    if (!route || !cap || !ipv4 || !valid_segment(route) ||
        !valid_segment(cap) || !*ipv4) return false;

    bool ready = device_discovery_ready();
    const char *host = ready ? device_identity_name() : ipv4;
    int written = snprintf(out, out_size, ready ? "http://%s.local:%d/%s/%s" :
                           "http://%s:%d/%s/%s", host,
                           CONFIG_OAUTH_PROBE_HTTP_PORT, route, cap);
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool device_discovery_update_url(char *current, size_t current_size, const char *next)
{
    if (!current || !current_size || !next || !*next) return false;
    size_t length = strnlen(next, current_size);
    if (length == current_size || strnlen(current, current_size) == current_size ||
        strcmp(current, next) == 0) return false;
    memcpy(current, next, length + 1);
    return true;
}
