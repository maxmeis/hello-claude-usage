#include "wifi_store.h"

#include <string.h>

#include "nvs.h"

#define WIFI_STORE_VERSION 1U
#define WIFI_STORE_MAGIC 0x57494649U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    char ssid[33];
    char password[65];
} saved_wifi_t;

static bool bounded_length(const char *value, size_t capacity, size_t *length)
{
    if (!value) return false;
    for (size_t i = 0; i < capacity; ++i) {
        if (value[i] == 0) {
            *length = i;
            return true;
        }
    }
    return false;
}

bool wifi_store_valid_credentials(const char *ssid, const char *password)
{
    size_t ssid_len, password_len;
    if (!bounded_length(ssid, 33, &ssid_len) || ssid_len == 0 ||
        !bounded_length(password, 65, &password_len)) return false;
    for (size_t i = 0; i < ssid_len; ++i) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    if (password_len == 0) return true;
    if (password_len >= 8 && password_len <= 63) {
        for (size_t i = 0; i < password_len; ++i) {
            unsigned char c = (unsigned char)password[i];
            if (c < 0x20 || c > 0x7e) return false;
        }
        return true;
    }
    if (password_len == 64) {
        for (size_t i = 0; i < password_len; ++i) {
            char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }
    return false;
}

wifi_source_t wifi_store_select(const wifi_store_record_t *record,
                                const char *driver_ssid, const char *driver_password,
                                const char *fallback_ssid, const char *fallback_password,
                                wifi_store_record_t *selected)
{
    if (!record || !selected) return WIFI_SOURCE_NONE;
    memset(selected, 0, sizeof(*selected));
    if (record->state == WIFI_STORE_FORGOTTEN) return WIFI_SOURCE_NONE;
    if (record->state == WIFI_STORE_CONFIGURED) {
        if (!wifi_store_valid_credentials(record->ssid, record->password)) return WIFI_SOURCE_NONE;
        *selected = *record;
        return WIFI_SOURCE_APP;
    }
    if (record->state != WIFI_STORE_ABSENT) return WIFI_SOURCE_NONE;
    const char *ssid = NULL, *password = NULL;
    wifi_source_t source = WIFI_SOURCE_NONE;
    if (wifi_store_valid_credentials(driver_ssid, driver_password)) {
        ssid = driver_ssid; password = driver_password; source = WIFI_SOURCE_DRIVER;
    } else if (wifi_store_valid_credentials(fallback_ssid, fallback_password)) {
        ssid = fallback_ssid; password = fallback_password; source = WIFI_SOURCE_FALLBACK;
    }
    if (source != WIFI_SOURCE_NONE) {
        selected->state = WIFI_STORE_CONFIGURED;
        memcpy(selected->ssid, ssid, strlen(ssid) + 1);
        memcpy(selected->password, password, strlen(password) + 1);
    }
    return source;
}

esp_err_t wifi_store_load(wifi_store_record_t *record)
{
    if (!record) return ESP_ERR_INVALID_ARG;
    memset(record, 0, sizeof(*record));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_app", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    saved_wifi_t saved = {0};
    size_t length = sizeof(saved);
    err = nvs_get_blob(handle, "record", &saved, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (length != sizeof(saved) || saved.magic != WIFI_STORE_MAGIC ||
        saved.version != WIFI_STORE_VERSION ||
        (saved.state != WIFI_STORE_CONFIGURED && saved.state != WIFI_STORE_FORGOTTEN) ||
        !memchr(saved.ssid, 0, sizeof(saved.ssid)) ||
        !memchr(saved.password, 0, sizeof(saved.password))) return ESP_ERR_INVALID_STATE;
    record->state = (wifi_store_state_t)saved.state;
    if (record->state == WIFI_STORE_CONFIGURED) {
        if (!wifi_store_valid_credentials(saved.ssid, saved.password)) return ESP_ERR_INVALID_STATE;
        memcpy(record->ssid, saved.ssid, sizeof(saved.ssid));
        memcpy(record->password, saved.password, sizeof(saved.password));
    }
    return ESP_OK;
}

esp_err_t wifi_store_save(const wifi_store_record_t *record)
{
    if (!record || (record->state != WIFI_STORE_CONFIGURED &&
                    record->state != WIFI_STORE_FORGOTTEN)) return ESP_ERR_INVALID_ARG;
    if (record->state == WIFI_STORE_CONFIGURED &&
        !wifi_store_valid_credentials(record->ssid, record->password)) return ESP_ERR_INVALID_ARG;
    saved_wifi_t saved = {.magic = WIFI_STORE_MAGIC, .version = WIFI_STORE_VERSION,
                          .state = (uint32_t)record->state};
    if (record->state == WIFI_STORE_CONFIGURED) {
        memcpy(saved.ssid, record->ssid, strlen(record->ssid) + 1);
        memcpy(saved.password, record->password, strlen(record->password) + 1);
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_app", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "record", &saved, sizeof(saved));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
