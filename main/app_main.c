#include <stddef.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "display.h"
#include "device_identity.h"
#include "factory_reset.h"
#include "oauth_probe.h"
#include "wifi_manager.h"

#if __has_include("provision.h")
#include "provision.h"
#define LOCAL_SSID PROVISION_WIFI_SSID
#define LOCAL_PASSWORD PROVISION_WIFI_PASSWORD
#else
#define LOCAL_SSID NULL
#define LOCAL_PASSWORD NULL
#endif

static const char *TAG = "probe_app";

void app_main(void)
{
    display_init();
    ESP_LOGI(TAG, "Display ready (480x480); Usage monitor RGB565");
    display_status("Starting", "Connecting to Wi-Fi...");
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        display_status("Storage error", "Check device storage");
        return;
    }

    err = factory_reset_prepare_boot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset preparation failed: %s", esp_err_to_name(err));
        display_status("Storage error", "Reset could not finish");
        return;
    }

    err = device_identity_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Device identity init failed: %s", esp_err_to_name(err));
        display_status("Storage error", "Device name unavailable");
        return;
    }
    const char *name = device_identity_name();
    display_set_device_name(name);
    ESP_LOGI(TAG, "Device name / STA hostname: %s", name);

    EventGroupHandle_t wifi_events = NULL;
    esp_netif_t *sta = NULL;
    err = wifi_manager_start(LOCAL_SSID, LOCAL_PASSWORD, &wifi_events, &sta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi manager failed: %s", esp_err_to_name(err));
        display_status("Wi-Fi error", "Network setup unavailable");
        return;
    }
    oauth_probe_start(wifi_events, sta);
}
