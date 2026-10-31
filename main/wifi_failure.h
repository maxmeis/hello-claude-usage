#ifndef WIFI_FAILURE_H
#define WIFI_FAILURE_H

#include <stdint.h>

/* ESP-IDF station disconnect reason; these messages contain no network data. */
const char *wifi_failure_message(uint16_t reason);
const char *wifi_failure_device_message(uint16_t reason);

#endif
