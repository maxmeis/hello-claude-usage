#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"

/* Serializes provider HTTPS on the shared Wi-Fi/PSRAM path. Initialize once
 * before starting either provider worker. Never hold this across LVGL/NVS. */
bool provider_http_init(void);
bool provider_http_take(TickType_t timeout);
void provider_http_give(void);
