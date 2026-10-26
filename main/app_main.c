#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "display.h"
#include "oauth_probe.h"

#if __has_include("provision.h")
#include "provision.h"
#define HAVE_LOCAL_PROVISION 1
#else
#define HAVE_LOCAL_PROVISION 0
#endif

static const char *TAG = "probe_app";
static EventGroupHandle_t s_wifi_events;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, OAUTH_WIFI_CONNECTED | OAUTH_WIFI_RECONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, OAUTH_WIFI_CONNECTED);
    }
}

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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) {
        display_status("Wi-Fi error", "Network interface unavailable");
        return;
    }
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        display_status("Wi-Fi error", "Out of memory");
        return;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t configured = {0};
    err = esp_wifi_get_config(WIFI_IF_STA, &configured);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot inspect saved WiFi configuration: %s", esp_err_to_name(err));
        display_status("Wi-Fi error", "Saved network settings unreadable");
        return;
    }
    if (configured.sta.ssid[0] == 0) {
#if HAVE_LOCAL_PROVISION
        static const char local_ssid[] = PROVISION_WIFI_SSID;
        static const char local_password[] = PROVISION_WIFI_PASSWORD;
        if (sizeof(local_ssid) > sizeof(configured.sta.ssid) ||
            sizeof(local_password) > sizeof(configured.sta.password) || local_ssid[0] == 0) {
            display_status("Wi-Fi missing", "Network setup is invalid");
            return;
        }
        memcpy(configured.sta.ssid, local_ssid, sizeof(local_ssid));
        memcpy(configured.sta.password, local_password, sizeof(local_password));
        /* A local provision header is intentionally not copied into NVS. */
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &configured));
#else
        ESP_LOGW(TAG, "No saved WiFi credentials; local provisioning required");
        display_status("Wi-Fi missing", "No saved network settings");
        return;
#endif
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    oauth_probe_start(s_wifi_events, sta);

    unsigned backoff_seconds = 2;
    for (;;) {
        if (!(xEventGroupGetBits(s_wifi_events) & OAUTH_WIFI_CONNECTED)) {
            display_status("Wi-Fi", "Reconnecting");
            err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
            EventBits_t bits = xEventGroupWaitBits(s_wifi_events, OAUTH_WIFI_CONNECTED,
                                                    pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
            if (!(bits & OAUTH_WIFI_CONNECTED)) {
                display_status("Wi-Fi", "Connection failed; retrying");
                vTaskDelay(pdMS_TO_TICKS(backoff_seconds * 1000));
                if (backoff_seconds < 60) backoff_seconds *= 2;
                if (backoff_seconds > 60) backoff_seconds = 60;
                continue;
            }
            ESP_LOGI(TAG, "WiFi connected");
            backoff_seconds = 2;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
