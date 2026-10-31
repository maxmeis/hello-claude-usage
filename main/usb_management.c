#include "usb_management.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "box_config.h"
#include "codex_adapter.h"
#include "cJSON.h"
#include "device_identity.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oauth_probe.h"
#include "sdkconfig.h"
#include "wifi_manager.h"
#include "wifi_store.h"

#define RPC_PREFIX "HELLO_BOX_RPC "
#define RPC_REQUEST_MAX 768u /* including prefix and LF */
#define RPC_RESPONSE_MAX 4096u /* including prefix and LF */

static const char *const s_timezones[] = {
    "Europe/Berlin", "UTC", "Europe/London", "America/New_York",
    "America/Los_Angeles", "Asia/Tokyo"
};

static bool only_keys(const cJSON *object, const char *const *keys, size_t count)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *child = object->child; child; child = child->next) {
        if (!child->string) return false;
        bool known = false;
        for (size_t i = 0; i < count; ++i) if (!strcmp(child->string, keys[i])) known = true;
        if (!known) return false;
        for (const cJSON *other = child->next; other; other = other->next)
            if (!strcmp(child->string, other->string)) return false;
    }
    return true;
}

static void wipe_request(cJSON *item)
{
    for (cJSON *child = item; child; child = child->next) {
        if (child->valuestring) memset(child->valuestring, 0, strlen(child->valuestring));
        if (child->child) wipe_request(child->child);
    }
}

static bool bounded_text(const cJSON *item, size_t max)
{
    return cJSON_IsString(item) && item->valuestring &&
           strlen(item->valuestring) <= max;
}

static bool add_usage(cJSON *parent, const provider_usage_t *usage)
{
    cJSON *object = cJSON_AddObjectToObject(parent, "usage");
    cJSON *windows = object ? cJSON_AddArrayToObject(object, "windows") : NULL;
    if (!object || !windows) return false;
    cJSON_AddBoolToObject(object, "hasSample", usage->has_sample);
    cJSON_AddNumberToObject(object, "sampledAt", usage->sampled_at);
    cJSON_AddBoolToObject(object, "updating", usage->updating);
    cJSON_AddBoolToObject(object, "resetCreditsKnown", usage->reset_credits_known);
    cJSON_AddNumberToObject(object, "resetCredits", usage->reset_credits);
    cJSON_AddStringToObject(object, "status", usage->status);
    for (unsigned i = 0; i < 2; ++i) {
        const quota_window_t *window = &usage->windows[i];
        cJSON *entry = cJSON_CreateObject();
        if (!entry) return false;
        cJSON_AddItemToArray(windows, entry);
        cJSON_AddBoolToObject(entry, "valid", window->valid);
        cJSON_AddNumberToObject(entry, "usedPercent", window->used_percent);
        cJSON_AddNumberToObject(entry, "resetsAt", window->resets_at);
        cJSON_AddNumberToObject(entry, "windowSeconds", window->window_seconds);
    }
    return true;
}

/* Registrations are immutable after display_init; do not call LVGL-owned
 * enabled/configured callbacks from the USB reader task. */
static bool add_catalog(cJSON *result)
{
    const app_registry_t *apps = display_app_registry();
    const screensaver_registry_t *savers = display_screensaver_registry();
    cJSON *app_list = cJSON_AddArrayToObject(result, "apps");
    cJSON *saver_list = cJSON_AddArrayToObject(result, "screensavers");
    if (!app_list || !saver_list) return false;
    for (size_t i = 0; apps && i < apps->count; ++i) {
        const app_descriptor_t *app = &apps->items[i];
        cJSON *entry = cJSON_CreateObject();
        if (!entry) return false;
        cJSON_AddItemToArray(app_list, entry);
        cJSON_AddNumberToObject(entry, "id", app->id);
        cJSON_AddStringToObject(entry, "name", app->name);
        cJSON_AddBoolToObject(entry, "canRefresh", app->capabilities & APP_CAP_REFRESH);
        cJSON_AddBoolToObject(entry, "canInterruptScreensaver", app->capabilities & APP_CAP_INTERRUPT_SAVER);
    }
    for (size_t i = 0; savers && i < savers->count; ++i) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) return false;
        cJSON_AddItemToArray(saver_list, entry);
        cJSON_AddNumberToObject(entry, "id", savers->items[i].id);
        cJSON_AddStringToObject(entry, "name", savers->items[i].name);
    }
    return true;
}

static cJSON *status_result(void)
{
    wifi_manager_status_t wifi;
    oauth_management_status_t oauth;
    codex_management_status_t codex_status = {0};
    wifi_manager_get_status(&wifi);
    oauth_probe_get_management_status(&oauth);
    codex_adapter_get_status(&codex_status);
    const box_settings_t *settings = box_config_current();
    const char *version = esp_app_get_description()->version;
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    cJSON *display = cJSON_AddObjectToObject(result, "settings");
    cJSON *wifi_json = cJSON_AddObjectToObject(result, "wifi");
    cJSON *claude = cJSON_AddObjectToObject(result, "claude");
    cJSON *codex = cJSON_AddObjectToObject(result, "codex");
    cJSON *network = cJSON_AddObjectToObject(result, "networkTest");
    cJSON *operation = cJSON_AddObjectToObject(result, "operation");
    if (!display || !wifi_json || !claude || !codex || !network || !operation || !add_catalog(result)) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddStringToObject(result, "firmwareVersion", version ? version : "unknown");
    cJSON_AddStringToObject(result, "deviceName", device_identity_name());
    cJSON_AddStringToObject(display, "clockLanguage", settings->clock_language ? "en" : "de");
    cJSON_AddStringToObject(display, "defaultView", settings->default_view ? "usage" : "clock");
    cJSON_AddNumberToObject(display, "brightness", settings->brightness);
    cJSON_AddNumberToObject(display, "dashboardTimeout", settings->dashboard_timeout_sec);
    cJSON_AddNumberToObject(display, "refreshInterval", settings->refresh_interval_sec);
    cJSON_AddStringToObject(display, "timezone", s_timezones[settings->timezone <= 5 ? settings->timezone : 0]);
    cJSON_AddBoolToObject(display, "mascotMotion", settings->mascot_motion);
    cJSON_AddBoolToObject(display, "refreshAnimation", settings->refresh_animation);
    cJSON_AddStringToObject(display, "providers",
                            settings->providers == 1 ? "codex" :
                            settings->providers == 2 ? "both" : "claude");
    cJSON_AddBoolToObject(wifi_json, "configured", wifi.configured);
    cJSON_AddNumberToObject(wifi_json, "operationId", wifi.operation_id);
    cJSON_AddBoolToObject(wifi_json, "connected", wifi.connected);
    cJSON_AddBoolToObject(wifi_json, "provisioning", wifi.provisioning);
    cJSON_AddStringToObject(wifi_json, "state", wifi.state);
    cJSON_AddStringToObject(wifi_json, "message", wifi.message);
    cJSON_AddBoolToObject(claude, "configured", oauth.configured);
    cJSON_AddStringToObject(claude, "state", oauth.state);
    if (oauth.session[0]) cJSON_AddStringToObject(claude, "session", oauth.session);
    if (oauth.authorization_url[0])
        cJSON_AddStringToObject(claude, "authorizationUrl", oauth.authorization_url);
    if (!add_usage(claude, &oauth.usage)) { cJSON_Delete(result); return NULL; }
    cJSON_AddBoolToObject(codex, "configured", codex_status.configured);
    cJSON_AddStringToObject(codex, "state", codex_status.state);
    if (codex_status.verification_url[0])
        cJSON_AddStringToObject(codex, "verificationUrl", codex_status.verification_url);
    if (codex_status.user_code[0])
        cJSON_AddStringToObject(codex, "userCode", codex_status.user_code);
    cJSON_AddNumberToObject(codex, "expiresAt", codex_status.expires_at);
    if (!add_usage(codex, &codex_status.usage)) { cJSON_Delete(result); return NULL; }
    cJSON_AddStringToObject(network, "state", oauth.network.state);
    cJSON_AddBoolToObject(network, "wifi", oauth.network.wifi);
    cJSON_AddBoolToObject(network, "ip", oauth.network.ip);
    cJSON_AddBoolToObject(network, "clock", oauth.network.clock);
    cJSON_AddBoolToObject(network, "dns", oauth.network.dns);
    cJSON_AddBoolToObject(network, "https", oauth.network.https);
    cJSON_AddStringToObject(network, "message", oauth.network.message);
    cJSON_AddNumberToObject(operation, "id", oauth.operation.id);
    cJSON_AddStringToObject(operation, "method", oauth.operation.method);
    cJSON_AddStringToObject(operation, "state", oauth.operation.state);
    cJSON_AddStringToObject(operation, "message", oauth.operation.message);
    return result;
}

static cJSON *error_object(const char *code, const char *message)
{
    cJSON *error = cJSON_CreateObject();
    if (!error) return NULL;
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return error;
}

bool usb_management_process_line(const char *line, size_t length,
                                 char *response, size_t response_size)
{
    if (!response || response_size < 32) return false;
    response[0] = 0;
    const size_t prefix_len = sizeof(RPC_PREFIX) - 1;
    if (!line || length <= prefix_len || length + 1 > RPC_REQUEST_MAX ||
        memcmp(line, RPC_PREFIX, prefix_len) || memchr(line, '\r', length) ||
        memchr(line, '\n', length) || memchr(line, 0, length)) return false;

    /* Require exactly one JSON object and a positive integer request ID. */
    char request_text[RPC_REQUEST_MAX];
    size_t json_length = length - prefix_len;
    memcpy(request_text, line + prefix_len, json_length);
    request_text[json_length] = 0;
    if (strstr(request_text, "\\u0000")) {
        memset(request_text, 0, sizeof(request_text));
        return false;
    }
    const char *end = NULL;
    cJSON *request = cJSON_ParseWithLengthOpts(request_text,
                                               json_length + 1, &end, 1);
    if (!request) { memset(request_text, 0, sizeof(request_text)); return false; }
    while (end && end < request_text + json_length && *end == ' ') ++end;
    const char *const request_keys[] = {"id", "method", "params"};
    cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
    if (!only_keys(request, request_keys, 3) || !cJSON_IsNumber(id) ||
        id->valuedouble < 1 || id->valuedouble > UINT32_MAX ||
        (double)(uint32_t)id->valuedouble != id->valuedouble ||
        !bounded_text(method, 23) || !params || !cJSON_IsObject(params) ||
        !end || end != request_text + json_length) {
        wipe_request(request);
        cJSON_Delete(request);
        memset(request_text, 0, sizeof(request_text));
        return false;
    }
    uint32_t operation_id = (uint32_t)id->valuedouble;
    const char *name = method->valuestring;
    cJSON *result = NULL;
    cJSON *error = NULL;
    const char *const wifi_keys[] = {"ssid", "password"};
    const char *const code_keys[] = {"session", "code"};
    if (!strcmp(name, "status.get")) {
        if (params->child) error = error_object("BAD_PARAMS", "This method takes no parameters.");
        else result = status_result();
    } else if (!strcmp(name, "wifi.connect")) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(params, "password");
        if (!only_keys(params, wifi_keys, 2) || !bounded_text(ssid, 32) ||
            !bounded_text(password, 64) ||
            !wifi_store_valid_credentials(ssid->valuestring, password->valuestring))
            error = error_object("BAD_PARAMS", "Check the network name and password.");
        else if (!oauth_probe_management_request(operation_id, name,
                                                 ssid->valuestring, password->valuestring))
            error = error_object("BUSY", "The device is busy. Retry shortly.");
        else {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "accepted", true);
            cJSON_AddNumberToObject(result, "operationId", operation_id);
        }
    } else if (!strcmp(name, "network.test") || !strcmp(name, "claude.begin") ||
               !strcmp(name, "claude.cancel") || !strcmp(name, "codex.begin") ||
               !strcmp(name, "codex.cancel")) {
        if (params->child) error = error_object("BAD_PARAMS", "This method takes no parameters.");
        else if (!oauth_probe_management_request(operation_id, name, NULL, NULL))
            error = error_object("BUSY", "The device is busy. Retry shortly.");
        else {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "accepted", true);
            cJSON_AddNumberToObject(result, "operationId", operation_id);
        }
    } else if (!strcmp(name, "claude.submit_code")) {
        cJSON *session = cJSON_GetObjectItemCaseSensitive(params, "session");
        cJSON *code = cJSON_GetObjectItemCaseSensitive(params, "code");
        if (!only_keys(params, code_keys, 2) || !bounded_text(session, 47) ||
            !bounded_text(code, 511) || !session->valuestring[0] || !code->valuestring[0])
            error = error_object("BAD_PARAMS", "Check the authorization code.");
        else if (!oauth_probe_management_request(operation_id, name,
                                                 session->valuestring, code->valuestring))
            error = error_object("BUSY", "The device is busy. Retry shortly.");
        else {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "accepted", true);
            cJSON_AddNumberToObject(result, "operationId", operation_id);
        }
    } else error = error_object("UNKNOWN_METHOD", "This method is unavailable.");

    cJSON *envelope = cJSON_CreateObject();
    if (envelope) {
        cJSON_AddNumberToObject(envelope, "id", operation_id);
        cJSON_AddBoolToObject(envelope, "ok", error == NULL && result != NULL);
        if (result) cJSON_AddItemToObject(envelope, "result", result);
        else if (error) cJSON_AddItemToObject(envelope, "error", error);
        else cJSON_AddItemToObject(envelope, "error", error_object("INTERNAL", "Could not prepare a response."));
    } else { cJSON_Delete(result); cJSON_Delete(error); }
    wipe_request(request);
    memset(request_text, 0, sizeof(request_text));
    cJSON_Delete(request);
    if (!envelope) return false;
    char *json = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (!json) return false;
    size_t json_len = strlen(json);
    bool fits = prefix_len + json_len + 2 <= response_size &&
                prefix_len + json_len + 1 <= RPC_RESPONSE_MAX;
    if (fits) {
        memcpy(response, RPC_PREFIX, prefix_len);
        memcpy(response + prefix_len, json, json_len);
        response[prefix_len + json_len] = '\n';
        response[prefix_len + json_len + 1] = 0;
    }
    cJSON_free(json);
    return fits;
}

static void reader(void *arg)
{
    (void)arg;
    char *line = malloc(RPC_REQUEST_MAX);
    char *response = malloc(RPC_RESPONSE_MAX + 1);
    if (!line || !response) { free(line); free(response); vTaskDelete(NULL); return; }
    size_t length = 0;
    bool discard = false;
    for (;;) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) != 1) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (ch == '\n') {
            if (!discard && usb_management_process_line(line, length, response, RPC_RESPONSE_MAX + 1)) {
                printf("%s", response);
                fflush(stdout);
            }
            length = 0;
            memset(line, 0, RPC_REQUEST_MAX);
            discard = false;
        } else if (length + 2 <= RPC_REQUEST_MAX && !discard) line[length++] = ch;
        else discard = true;
    }
}

bool usb_management_start(void)
{
#if CONFIG_HELLO_BOX_USB_TEST_HELPER
    /* The diagnostic helper also reads stdin; never run both readers. */
    return false;
#else
    if (esp_vfs_dev_uart_port_set_rx_line_endings(0, ESP_LINE_ENDINGS_LF) != 0)
        return false;
    return xTaskCreate(reader, "usb_management", 6144, NULL, 3, NULL) == pdPASS;
#endif
}
