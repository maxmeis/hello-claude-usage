#include "box_config_store.h"

#include <string.h>
#include <stdlib.h>

#include "box_config.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "nvs.h"

#define CONFIG_NS "box_config"
#define CONFIG_KEY "settings"
#define CONFIG_PART "web_config"

static wifi_store_record_t s_candidate;
static bool s_has_candidate;

static const esp_partition_t *inbox(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, CONFIG_PART);
}

static void encode_settings(const box_settings_t *s, uint8_t bytes[11])
{
    bytes[0] = s->clock_language;
    bytes[1] = s->default_view;
    bytes[2] = s->brightness;
    bytes[3] = (uint8_t)s->dashboard_timeout_sec;
    bytes[4] = (uint8_t)(s->dashboard_timeout_sec >> 8);
    bytes[5] = (uint8_t)s->refresh_interval_sec;
    bytes[6] = (uint8_t)(s->refresh_interval_sec >> 8);
    bytes[7] = s->timezone;
    bytes[8] = (s->mascot_motion ? 1 : 0) | (s->refresh_animation ? 2 : 0) |
               (uint8_t)(s->providers << 2);
    bytes[9] = 0x48;
    bytes[10] = 0x42;
}

static bool decode_settings(const uint8_t bytes[11], box_settings_t *s)
{
    if (bytes[9] != 0x48 || bytes[10] != 0x42 || (bytes[8] & ~15u)) return false;
    *s = (box_settings_t){
        .clock_language = bytes[0], .default_view = bytes[1], .brightness = bytes[2],
        .dashboard_timeout_sec = (uint16_t)bytes[3] | (uint16_t)bytes[4] << 8,
        .refresh_interval_sec = (uint16_t)bytes[5] | (uint16_t)bytes[6] << 8,
        .timezone = bytes[7], .mascot_motion = !!(bytes[8] & 1),
        .refresh_animation = !!(bytes[8] & 2),
        .providers = (bytes[8] >> 2) & 3u,
    };
    return box_config_validate_settings(s);
}

static esp_err_t save_settings(const box_settings_t *s)
{
    uint8_t bytes[11];
    encode_settings(s, bytes);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, CONFIG_KEY, bytes, sizeof(bytes));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t load_settings(box_settings_t *s)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t bytes[11];
    size_t size = sizeof(bytes);
    err = nvs_get_blob(handle, CONFIG_KEY, bytes, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size != sizeof(bytes) || !decode_settings(bytes, s)) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t box_config_init(void)
{
    memset(&s_candidate, 0, sizeof(s_candidate));
    s_has_candidate = false;
    box_settings_t settings;
    box_config_defaults(&settings);
    esp_err_t err = load_settings(&settings);
    if (err != ESP_OK) return err;
    box_config_set_current(&settings);

    const esp_partition_t *partition = inbox();
    if (!partition) return ESP_OK; /* Existing partition layouts still boot. */
    if (partition->size < BOX_CONFIG_FRAME_SIZE) return ESP_ERR_INVALID_SIZE;
    uint8_t *frame = malloc(BOX_CONFIG_FRAME_SIZE);
    if (!frame) return ESP_ERR_NO_MEM;
    err = esp_partition_read(partition, 0, frame, BOX_CONFIG_FRAME_SIZE);
    if (err != ESP_OK) { free(frame); return err; }
    bool empty = true;
    for (size_t i = 0; i < BOX_CONFIG_FRAME_SIZE; ++i) if (frame[i] != 0xff) { empty = false; break; }
    if (empty) { free(frame); return ESP_OK; }
    box_config_image_t image;
    if (!box_config_decode(frame, BOX_CONFIG_FRAME_SIZE, &image)) {
        memset(frame, 0, BOX_CONFIG_FRAME_SIZE);
        free(frame);
        ESP_LOGW("box_config", "Invalid installer inbox ignored");
        return ESP_OK;
    }
    memset(frame, 0, BOX_CONFIG_FRAME_SIZE);
    free(frame);
    err = save_settings(&image.settings);
    if (err != ESP_OK) {
        memset(&image, 0, sizeof(image));
        return err;
    }
    box_config_set_current(&image.settings);
    if (image.wifi_present) {
        s_candidate.state = WIFI_STORE_CONFIGURED;
        memcpy(s_candidate.ssid, image.ssid, sizeof(image.ssid));
        memcpy(s_candidate.password, image.password, sizeof(image.password));
        s_has_candidate = true;
        memset(&image, 0, sizeof(image));
        return ESP_OK;
    }
    memset(&image, 0, sizeof(image));
    return esp_partition_erase_range(partition, 0, BOX_CONFIG_FRAME_SIZE);
}

bool box_config_pending_wifi(wifi_store_record_t *out)
{
    if (!out || !s_has_candidate) return false;
    *out = s_candidate;
    return true;
}

esp_err_t box_config_finish_pending(void)
{
    if (!s_has_candidate) return ESP_OK;
    const esp_partition_t *partition = inbox();
    if (!partition) return ESP_ERR_NOT_FOUND;
    esp_err_t err = esp_partition_erase_range(partition, 0, BOX_CONFIG_FRAME_SIZE);
    if (err == ESP_OK) {
        memset(&s_candidate, 0, sizeof(s_candidate));
        s_has_candidate = false;
    }
    return err;
}

esp_err_t box_config_factory_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    const esp_partition_t *partition = inbox();
    if (!partition) return ESP_OK;
    return esp_partition_erase_range(partition, 0, BOX_CONFIG_FRAME_SIZE);
}
