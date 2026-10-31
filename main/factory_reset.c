#include "factory_reset.h"

#include <stdint.h>

#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_store.h"
#include "box_config_store.h"

#define RESET_NAMESPACE "factory_reset"
#define RESET_KEY "pending"
#define RESET_PENDING 1U

esp_err_t factory_reset_is_pending(bool *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(RESET_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t pending = 0;
    err = nvs_get_u8(handle, RESET_KEY, &pending);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (pending != RESET_PENDING) return ESP_ERR_INVALID_STATE;
    *out = true;
    return ESP_OK;
}

esp_err_t factory_reset_request(void)
{
    bool pending;
    esp_err_t err = factory_reset_is_pending(&pending);
    if (err != ESP_OK || pending) return err;

    nvs_handle_t handle;
    err = nvs_open(RESET_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, RESET_KEY, RESET_PENDING);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t erase_app_namespace(const char *name)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(name, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t factory_reset_prepare_boot(void)
{
    bool pending;
    esp_err_t err = factory_reset_is_pending(&pending);
    if (err != ESP_OK || !pending) return err;

    err = erase_app_namespace("claude_probe");
    if (err != ESP_OK) return err;
    /* Codex credentials live in the dedicated provider-accounts partition.
     * Erase it only while the durable reset journal is pending. */
    err = nvs_flash_erase_partition("quota_accounts");
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    err = erase_app_namespace("hello_box");
    if (err != ESP_OK) return err;
    err = box_config_factory_erase();
    if (err != ESP_OK) return err;

    wifi_store_record_t forgotten = {.state = WIFI_STORE_FORGOTTEN};
    return wifi_store_save(&forgotten);
}

esp_err_t factory_reset_complete(void)
{
    bool pending;
    esp_err_t err = factory_reset_is_pending(&pending);
    if (err != ESP_OK || !pending) return err;

    nvs_handle_t handle;
    err = nvs_open(RESET_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, RESET_KEY);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
