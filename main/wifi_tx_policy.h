#ifndef WIFI_TX_POLICY_H
#define WIFI_TX_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* ESP-IDF expresses Wi-Fi TX power in quarter dBm. */
#define WIFI_TX_FALLBACK_QUARTER_DBM 28

bool wifi_tx_policy_eligible(uint16_t reason, bool rssi_valid, int8_t rssi,
                             bool attempted, bool original_known, int8_t original_limit);

#endif
