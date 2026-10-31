#pragma once

#include "esp_err.h"
#include "wifi_store.h"

/* Load persisted settings and consume a validated installer inbox. */
esp_err_t box_config_init(void);
bool box_config_pending_wifi(wifi_store_record_t *out);
/* Call after a terminal Wi-Fi trial, never before the successful NVS save. */
esp_err_t box_config_finish_pending(void);
/* Idempotent factory-reset cleanup, while the reset journal is still pending. */
esp_err_t box_config_factory_erase(void);
