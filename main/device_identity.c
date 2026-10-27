#include "device_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "nvs.h"

#define DEVICE_NAME_LENGTH 14

static char s_name[DEVICE_NAME_LENGTH + 1];
static bool s_initialized;

static bool valid_name(const char *name)
{
    if (memcmp(name, "hello-box-", 10) != 0 || name[DEVICE_NAME_LENGTH] != '\0')
        return false;
    for (size_t i = 10; i < DEVICE_NAME_LENGTH; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return name[10] != '0';
}

esp_err_t device_identity_init(void)
{
    if (s_initialized) return ESP_OK;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("hello_box", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char candidate[sizeof(s_name)] = {0};
    size_t length = sizeof(candidate);
    err = nvs_get_str(handle, "device_name", candidate, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        unsigned number = 1000U + esp_random() % 9000U;
        (void)snprintf(candidate, sizeof(candidate), "hello-box-%04u", number);
        err = nvs_set_str(handle, "device_name", candidate);
        if (err == ESP_OK) err = nvs_commit(handle);
    } else if (err == ESP_OK && (length != sizeof(candidate) || !valid_name(candidate))) {
        err = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        memcpy(s_name, candidate, sizeof(s_name));
        s_initialized = true;
    }
    memset(candidate, 0, sizeof(candidate));
    return err;
}

const char *device_identity_name(void)
{
    return s_initialized ? s_name : NULL;
}
