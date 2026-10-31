#include "wifi_failure.h"

typedef enum {
    FAILURE_OTHER,
    FAILURE_NOT_FOUND,
    FAILURE_AUTH,
    FAILURE_HANDSHAKE,
    FAILURE_REJECTED,
    FAILURE_TIMEOUT,
} failure_kind_t;

/* Values from ESP-IDF's wifi_err_reason_t. Keep this module SDK-free so its
 * user-facing wording can be tested without a firmware build. */
static failure_kind_t failure_kind(uint16_t reason)
{
    switch (reason) {
    case 201: /* WIFI_REASON_NO_AP_FOUND */
        return FAILURE_NOT_FOUND;
    case 15:  /* WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT */
    case 16:  /* WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT */
    case 204: /* WIFI_REASON_HANDSHAKE_TIMEOUT */
        return FAILURE_HANDSHAKE;
    case 17:  /* WIFI_REASON_IE_IN_4WAY_DIFFERS */
    case 18:  /* WIFI_REASON_GROUP_CIPHER_INVALID */
    case 19:  /* WIFI_REASON_PAIRWISE_CIPHER_INVALID */
    case 20:  /* WIFI_REASON_AKMP_INVALID */
    case 21:  /* WIFI_REASON_UNSUPP_RSN_IE_VERSION */
    case 22:  /* WIFI_REASON_INVALID_RSN_IE_CAP */
    case 23:  /* WIFI_REASON_802_1X_AUTH_FAILED */
    case 24:  /* WIFI_REASON_CIPHER_SUITE_REJECTED */
    case 29:  /* WIFI_REASON_BAD_CIPHER_OR_AKM */
    case 202: /* WIFI_REASON_AUTH_FAIL */
        return FAILURE_AUTH;
    case 5:   /* WIFI_REASON_ASSOC_TOOMANY */
    case 10:  /* WIFI_REASON_DISASSOC_PWRCAP_BAD */
    case 11:  /* WIFI_REASON_DISASSOC_SUPCHAN_BAD */
    case 30:  /* WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION */
    case 203: /* WIFI_REASON_ASSOC_FAIL */
    case 208: /* WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG */
        return FAILURE_REJECTED;
    case 2:   /* WIFI_REASON_AUTH_EXPIRE */
    case 4:   /* WIFI_REASON_ASSOC_EXPIRE */
    case 39:  /* WIFI_REASON_TIMEOUT */
    case 200: /* WIFI_REASON_BEACON_TIMEOUT */
    case 209: /* WIFI_REASON_SA_QUERY_TIMEOUT */
        return FAILURE_TIMEOUT;
    default:
        return FAILURE_OTHER;
    }
}

const char *wifi_failure_message(uint16_t reason)
{
    switch (failure_kind(reason)) {
    case FAILURE_NOT_FOUND:
        return "Network not found. Refresh the list and retry.";
    case FAILURE_AUTH:
        return "Router authentication failed. Check the password or router security settings.";
    case FAILURE_HANDSHAKE:
        return "Wi-Fi handshake timed out. Check signal, password or router security settings.";
    case FAILURE_REJECTED:
        return "Router association failed or was rejected. Check router settings and retry.";
    case FAILURE_TIMEOUT:
        return "Wi-Fi response timed out. Move closer to the router and retry.";
    default:
        return "Wi-Fi connection failed. Retry or check router settings.";
    }
}

const char *wifi_failure_device_message(uint16_t reason)
{
    switch (failure_kind(reason)) {
    case FAILURE_NOT_FOUND:
        return "Network not found; refresh list";
    case FAILURE_AUTH:
        return "Check password or router security";
    case FAILURE_HANDSHAKE:
        return "Handshake timed out; check signal";
    case FAILURE_REJECTED:
        return "Router association failed";
    case FAILURE_TIMEOUT:
        return "Wi-Fi timed out; move closer";
    default:
        return "Wi-Fi failed; check router";
    }
}
