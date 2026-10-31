#include "wifi_tx_policy.h"

bool wifi_tx_policy_eligible(uint16_t reason, bool rssi_valid, int8_t rssi,
                             bool attempted, bool original_known, int8_t original_limit)
{
    return (reason == 204 || reason == 15 || reason == 2 || reason == 39) &&
           rssi_valid && rssi >= -40 && rssi <= -1 &&
           !attempted && original_known && original_limit > WIFI_TX_FALLBACK_QUARTER_DBM;
}
