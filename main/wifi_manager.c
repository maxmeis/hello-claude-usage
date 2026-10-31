#include "wifi_manager.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "wifi_portal_page.h"
#include "wifi_failure.h"
#include "wifi_tx_policy.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "display.h"
#include "box_config_store.h"
#include "device_discovery.h"
#include "device_identity.h"
#include "factory_reset.h"
#include "oauth_probe.h"
#include "wifi_store.h"
#if CONFIG_HELLO_BOX_USB_TEST_HELPER
#include "wifi_usb_test.h"
#endif

#define CAP_LIFETIME_US (600LL * 1000000LL)
#define TRIAL_TIMEOUT_US (25LL * 1000000LL)
#define LOW_TX_TRIAL_TIMEOUT_US (20LL * 1000000LL)
#define LOW_TX_RETRY_DELAY_US (100LL * 1000LL)
#define BODY_LIMIT 512
#define RETRY_MAX_SECONDS 60
#define SETUP_GRACE_US (15LL * 1000000LL)
#define SCAN_LIMIT 12
#define SCAN_WATCHDOG_US (10LL * 1000000LL)
#define DEFERRED_TRIAL_TIMEOUT_US (10LL * 1000000LL)
#define RADIO_RECOVERY_TIMEOUT_US (5LL * 1000000LL)
#define MANAGEMENT_DISCONNECT_TIMEOUT_US (5LL * 1000000LL)

static const char *TAG = "wifi_manager";

typedef enum {
    CMD_FORGET,
    CMD_GOT_IP,
    CMD_DISCONNECTED,
    CMD_STA_STOP,
    CMD_STA_START,
    CMD_AP_STOP,
    CMD_PHONE_JOINED,
    CMD_PHONE_LEFT,
    CMD_PHONE_IP,
    CMD_STA_CONNECTED,
    CMD_SCAN,
    CMD_SCAN_DONE,
    CMD_SUBMIT,
    CMD_MANAGEMENT_CONNECT,
#if CONFIG_HELLO_BOX_USB_TEST_HELPER
    CMD_USB_TEST_INFO,
#endif
} command_kind_t;

typedef struct {
    command_kind_t kind;
    wifi_store_record_t credentials;
    uint8_t mac[6];
    uint32_t attempt;
    uint16_t reason;
    uint32_t radio_event;
    int8_t rssi;
    bool rssi_valid;
    char capability[33];
    int64_t cap_deadline;
} command_t;

typedef enum {
    RADIO_READY,
    RADIO_WAIT_STOP,
    RADIO_WAIT_START,
    RADIO_FAILED,
} radio_recovery_t;

typedef enum {
    RADIO_RESTART_TIMEOUT,
    RADIO_RESTART_TX_FALLBACK,
    RADIO_RESTART_CANCELLED,
    RADIO_RESTART_MANAGEMENT,
} radio_restart_purpose_t;

typedef struct {
    QueueHandle_t queue;
    EventGroupHandle_t events;
    esp_netif_t *sta;
    esp_netif_t *ap;
    httpd_handle_t server;
    wifi_store_record_t selected;
    wifi_store_record_t trial;
    bool ap_started;
    bool setup_authorized;
    bool phone_joined;
    uint8_t phone_macs[4][6];
    uint8_t phone_count;
    uint8_t ready_macs[4][6];
    uint8_t ready_count;
    bool sta_associated;
    uint32_t attempt;
    uint32_t queued_attempt;
    bool http_busy;
    bool status_active;
    int64_t status_grace_deadline;
    char status_state[16];
    char status_message[96];
    char scan_state[12];
    char networks[SCAN_LIMIT][33];
    uint8_t network_count;
    int64_t scan_cached_at;
    int64_t scan_started_at;
    int64_t committed_at;
    bool trial_active;
    radio_recovery_t radio_recovery;
    radio_restart_purpose_t radio_restart_purpose;
    int64_t radio_recovery_deadline;
    uint32_t radio_event_before;
    command_t deferred_submit;
    int64_t deferred_deadline;
    bool pending_save;
    bool setup_committed;
    bool installer_candidate_active;
    bool installer_cleanup_error;
    uint32_t management_operation_id;
    bool management_wait_disconnect;
    int64_t management_disconnect_deadline;
    bool management_trial_active;
    bool management_restore_pending;
    bool management_restoring;
    wifi_store_record_t management_credentials;
    bool link_has_ip;
    bool saved_connecting;
    int64_t saved_connect_deadline;
    bool tx_original_known;
    int8_t tx_original_limit;
    bool tx_low;
    atomic_bool tx_fault;
    bool tx_fallback_attempted;
    bool tx_retry_pending;
    int64_t tx_retry_at;
    bool tx_restore_after_start;
    int64_t trial_deadline;
    int64_t trial_started_at;
    int64_t save_retry_at;
    int64_t cap_deadline;
    int64_t next_retry;
    unsigned retry_seconds;
    char ap_ssid[33];
    char ap_password[17];
    char cap[33];
    char join_qr[112];
    char page_url[128];
    const char *setup_message;
} manager_t;

static manager_t s_manager;
static atomic_bool s_provisioning = ATOMIC_VAR_INIT(false);
static atomic_bool s_durable_configured = ATOMIC_VAR_INIT(false);
static atomic_bool s_connected = ATOMIC_VAR_INIT(false);
static atomic_bool s_management_queued = ATOMIC_VAR_INIT(false);
static atomic_bool s_scan_done_pending = ATOMIC_VAR_INIT(false);
static atomic_uint s_sta_stop_count = ATOMIC_VAR_INIT(0);
static atomic_uint s_sta_start_count = ATOMIC_VAR_INIT(0);
static atomic_bool s_sta_stop_dropped = ATOMIC_VAR_INIT(false);
static atomic_bool s_sta_start_dropped = ATOMIC_VAR_INIT(false);
static portMUX_TYPE s_cap_mux = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_HELLO_BOX_USB_TEST_HELPER
bool wifi_manager_request_usb_test_info(void)
{
    if (!s_manager.queue) return false;
    command_t command = {.kind = CMD_USB_TEST_INFO};
    return xQueueSend(s_manager.queue, &command, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void emit_usb_test_info(int64_t now)
{
    cJSON *record = cJSON_CreateObject();
    if (!record) return;
    const char *url_cap = strrchr(s_manager.page_url, '/');
    bool available = wifi_manager_is_provisioning() && s_manager.ap_started &&
                     s_manager.server && !s_manager.setup_committed &&
                     s_manager.ap_ssid[0] && s_manager.ap_password[0] &&
                     s_manager.page_url[0] && s_manager.cap[0] &&
                     url_cap && strcmp(url_cap + 1, s_manager.cap) == 0 &&
                     now < s_manager.cap_deadline;
    cJSON_AddStringToObject(record, "status", available ? "ready" : "unavailable");
    if (available) {
        cJSON_AddStringToObject(record, "ap_ssid", s_manager.ap_ssid);
        cJSON_AddStringToObject(record, "ap_password", s_manager.ap_password);
        cJSON_AddStringToObject(record, "page_url", s_manager.page_url);
    }
    char *json = cJSON_PrintUnformatted(record);
    cJSON_Delete(record);
    if (json) {
        /* One write keeps the response together in the console stream. */
        printf("HELLO_BOX_SETUP_INFO %s\n", json);
        fflush(stdout);
        cJSON_free(json);
    }
}
#endif

bool wifi_manager_is_provisioning(void)
{
    return atomic_load_explicit(&s_provisioning, memory_order_acquire);
}

void wifi_manager_get_status(wifi_manager_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->configured = atomic_load_explicit(&s_durable_configured, memory_order_acquire);
    out->connected = atomic_load_explicit(&s_connected, memory_order_acquire);
    out->provisioning = wifi_manager_is_provisioning();
    if (!s_manager.queue) {
        strcpy(out->state, "starting");
        strcpy(out->message, "Wi-Fi is starting");
        return;
    }
    portENTER_CRITICAL(&s_cap_mux);
    out->operation_id = s_manager.management_operation_id;
    snprintf(out->state, sizeof(out->state), "%s", s_manager.status_state);
    snprintf(out->message, sizeof(out->message), "%s", s_manager.status_message);
    bool queued = atomic_load_explicit(&s_management_queued, memory_order_acquire);
    portEXIT_CRITICAL(&s_cap_mux);
    if (queued &&
        strcmp(out->state, "testing") && strcmp(out->state, "saving")) {
        strcpy(out->state, "queued");
        strcpy(out->message, "Waiting to test the new Wi-Fi connection");
    }
    if (!out->state[0]) strcpy(out->state, out->connected ? "connected" : "connecting");
}

bool wifi_manager_request_connect(uint32_t operation_id, const char *ssid, const char *password)
{
    if (!operation_id || !s_manager.queue ||
        !wifi_store_valid_credentials(ssid, password)) return false;
    portENTER_CRITICAL(&s_cap_mux);
    bool busy = atomic_load_explicit(&s_management_queued, memory_order_acquire);
    uint32_t previous_id = s_manager.management_operation_id;
    if (!busy) {
        s_manager.management_operation_id = operation_id;
        atomic_store_explicit(&s_management_queued, true, memory_order_release);
    }
    portEXIT_CRITICAL(&s_cap_mux);
    if (busy) return false;
    command_t command = {.kind = CMD_MANAGEMENT_CONNECT};
    command.attempt = operation_id;
    command.credentials.state = WIFI_STORE_CONFIGURED;
    memcpy(command.credentials.ssid, ssid, strlen(ssid) + 1);
    memcpy(command.credentials.password, password, strlen(password) + 1);
    bool queued = xQueueSend(s_manager.queue, &command, 0) == pdTRUE;
    memset(&command, 0, sizeof(command));
    if (!queued) {
        portENTER_CRITICAL(&s_cap_mux);
        s_manager.management_operation_id = previous_id;
        atomic_store_explicit(&s_management_queued, false, memory_order_release);
        portEXIT_CRITICAL(&s_cap_mux);
    }
    return queued;
}

bool wifi_manager_request_forget(void)
{
    if (!s_manager.queue) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&s_provisioning, &expected, true,
                                                  memory_order_acq_rel, memory_order_acquire))
        return false;
    command_t command = {.kind = CMD_FORGET};
    if (xQueueSend(s_manager.queue, &command, 0) != pdTRUE) {
        atomic_store_explicit(&s_provisioning, false, memory_order_release);
        return false;
    }
    return true;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    command_t command = {0};
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) command.kind = CMD_GOT_IP;
    else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        command.kind = CMD_PHONE_IP;
        memcpy(command.mac, ((ip_event_ap_staipassigned_t *)data)->mac, 6);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) command.kind = CMD_STA_CONNECTED;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) command.kind = CMD_STA_STOP;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) command.kind = CMD_STA_START;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) command.kind = CMD_AP_STOP;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) command.kind = CMD_SCAN_DONE;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        command.kind = CMD_DISCONNECTED;
        if (data) {
            const wifi_event_sta_disconnected_t *event = data;
            command.reason = event->reason;
            command.rssi = event->rssi;
            command.rssi_valid = event->rssi < 0 && event->rssi > -128;
        }
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) { command.kind = CMD_PHONE_JOINED; memcpy(command.mac, ((wifi_event_ap_staconnected_t *)data)->mac, 6); }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) { command.kind = CMD_PHONE_LEFT; memcpy(command.mac, ((wifi_event_ap_stadisconnected_t *)data)->mac, 6); }
    else return;
    if (command.kind == CMD_STA_STOP)
        command.radio_event = atomic_fetch_add_explicit(&s_sta_stop_count, 1,
                                                        memory_order_acq_rel) + 1;
    else if (command.kind == CMD_STA_START)
        command.radio_event = atomic_fetch_add_explicit(&s_sta_start_count, 1,
                                                        memory_order_acq_rel) + 1;
    if (xQueueSend(s_manager.queue, &command, 0) != pdTRUE) {
        if (command.kind == CMD_SCAN_DONE)
            atomic_store_explicit(&s_scan_done_pending, true, memory_order_release);
        else if (command.kind == CMD_STA_STOP)
            atomic_store_explicit(&s_sta_stop_dropped, true, memory_order_release);
        else if (command.kind == CMD_STA_START)
            atomic_store_explicit(&s_sta_start_dropped, true, memory_order_release);
    }
}

static void random_hex(char *destination, size_t bytes)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t random[16];
    if (bytes > sizeof(random)) bytes = sizeof(random);
    esp_fill_random(random, bytes);
    for (size_t i = 0; i < bytes; ++i) {
        destination[i * 2] = digits[random[i] >> 4];
        destination[i * 2 + 1] = digits[random[i] & 15];
    }
    destination[bytes * 2] = 0;
    memset(random, 0, sizeof(random));
}

static void set_status(const char *state, const char *message)
{
    portENTER_CRITICAL(&s_cap_mux);
    snprintf(s_manager.status_state, sizeof(s_manager.status_state), "%s", state);
    s_manager.status_active = !strcmp(state, "queued") || !strcmp(state, "testing") ||
                              !strcmp(state, "saving");
    s_manager.http_busy = s_manager.status_active || !strcmp(state, "connected");
    if (!strcmp(state, "connected") || !strcmp(state, "failed"))
        s_manager.status_grace_deadline = esp_timer_get_time() + SETUP_GRACE_US;
    else if (!s_manager.status_active) s_manager.status_grace_deadline = 0;
    snprintf(s_manager.status_message, sizeof(s_manager.status_message), "%s", message);
    if (!strcmp(state, "connected"))
        atomic_store_explicit(&s_durable_configured, true, memory_order_release);
    if (!strcmp(state, "connected") || !strcmp(state, "failed"))
        atomic_store_explicit(&s_management_queued, false, memory_order_release);
    portEXIT_CRITICAL(&s_cap_mux);
    if (s_manager.management_trial_active &&
        (!strcmp(state, "connected") || !strcmp(state, "failed"))) {
        if (!strcmp(state, "failed") &&
            s_manager.selected.state == WIFI_STORE_CONFIGURED)
            s_manager.management_restore_pending = true;
        s_manager.management_trial_active = false;
    }
    if (s_manager.installer_candidate_active &&
        (!strcmp(state, "connected") || !strcmp(state, "failed"))) {
        esp_err_t err = box_config_finish_pending();
        if (err == ESP_OK) s_manager.installer_candidate_active = false;
        else {
            s_manager.installer_cleanup_error = true;
            ESP_LOGE(TAG, "Installer inbox could not be cleared: %s", esp_err_to_name(err));
        }
    }
}

static void show_setup(const char *message)
{
    if (s_manager.installer_cleanup_error)
        message = "Storage error clearing installer settings. Restart to retry.";
    s_manager.setup_message = message;
    display_wifi_setup(s_manager.join_qr, s_manager.page_url,
                       s_manager.phone_joined, message);
}

static bool cap_matches(const char *uri, const char *suffix)
{
    if (!uri || strncmp(uri, "/w/", 3)) return false;
    const char *query = strchr(uri, '?');
    size_t path_length = query ? (size_t)(query - uri) : strlen(uri);
    if (path_length != 35 + strlen(suffix) ||
        strncmp(uri + 35, suffix, strlen(suffix))) return false;
    if (query && (strcmp(suffix, "/scan") || strcmp(query, "?refresh=1"))) return false;
    portENTER_CRITICAL(&s_cap_mux);
    int64_t now = esp_timer_get_time();
    bool status_continues = !strcmp(suffix, "/status") && s_manager.attempt &&
                            (s_manager.status_active || now < s_manager.status_grace_deadline);
    bool valid = (now < s_manager.cap_deadline || status_continues) &&
                 memcmp(uri + 3, s_manager.cap, 32) == 0;
    portEXIT_CRITICAL(&s_cap_mux);
    return valid;
}

static esp_err_t send_json(httpd_req_t *request, cJSON *object)
{
    if (!object) return ESP_ERR_NO_MEM;
    char *json = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return err;
}

static esp_err_t portal_get(httpd_req_t *request)
{
    if (cap_matches(request->uri, "")) {
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
        httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'");
        return httpd_resp_sendstr(request, wifi_portal_page);
    }
    if (cap_matches(request->uri, "/status")) {
        cJSON *object = cJSON_CreateObject();
        if (!object) return ESP_ERR_NO_MEM;
        portENTER_CRITICAL(&s_cap_mux);
        uint32_t attempt = s_manager.attempt;
        char state[sizeof(s_manager.status_state)], message[sizeof(s_manager.status_message)];
        memcpy(state, s_manager.status_state, sizeof(state));
        memcpy(message, s_manager.status_message, sizeof(message));
        portEXIT_CRITICAL(&s_cap_mux);
        cJSON_AddStringToObject(object, "device", device_identity_name());
        cJSON_AddNumberToObject(object, "attempt", attempt);
        cJSON_AddStringToObject(object, "state", state[0] ? state : "idle");
        cJSON_AddStringToObject(object, "message", message);
        return send_json(request, object);
    }
    if (cap_matches(request->uri, "/scan")) {
        char query[24] = {0};
        bool has_query = httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK;
        if (has_query && strcmp(query, "refresh=1")) return httpd_resp_send_404(request);
        bool refresh = strchr(request->uri, '?') != NULL || has_query;
        portENTER_CRITICAL(&s_cap_mux);
        bool start = (refresh || !s_manager.scan_state[0]) &&
                     strcmp(s_manager.scan_state, "scanning") != 0;
        if (start) strcpy(s_manager.scan_state, "scanning");
        portEXIT_CRITICAL(&s_cap_mux);
        if (start) { command_t cmd = {.kind = CMD_SCAN}; portENTER_CRITICAL(&s_cap_mux); memcpy(cmd.capability, s_manager.cap, sizeof(cmd.capability)); cmd.cap_deadline = s_manager.cap_deadline; portEXIT_CRITICAL(&s_cap_mux); if (xQueueSend(s_manager.queue, &cmd, 0) != pdTRUE) { portENTER_CRITICAL(&s_cap_mux); strcpy(s_manager.scan_state, "error"); portEXIT_CRITICAL(&s_cap_mux); } }
        cJSON *object = cJSON_CreateObject();
        if (!object) return ESP_ERR_NO_MEM;
        cJSON *array = cJSON_AddArrayToObject(object, "networks");
        if (!array) { cJSON_Delete(object); return ESP_ERR_NO_MEM; }
        portENTER_CRITICAL(&s_cap_mux);
        char state[12]; memcpy(state, s_manager.scan_state, sizeof(state));
        char names[SCAN_LIMIT][33]; uint8_t count = s_manager.network_count;
        memcpy(names, s_manager.networks, sizeof(names));
        portEXIT_CRITICAL(&s_cap_mux);
        cJSON_AddStringToObject(object, "state", state[0] ? state : "scanning");
        if (!strcmp(state, "ready")) for (uint8_t i = 0; i < count; ++i) cJSON_AddItemToArray(array, cJSON_CreateString(names[i]));
        return send_json(request, object);
    }
    return httpd_resp_send_404(request);
}

static bool decode_field(const char *input, size_t length, char *output, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)input[i];
        if (value == '+') value = ' ';
        else if (value == '%') {
            if (i + 2 >= length) return false;
            unsigned hi = (unsigned char)input[++i];
            unsigned lo = (unsigned char)input[++i];
            hi = hi >= '0' && hi <= '9' ? hi - '0' :
                 hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 :
                 hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : 16;
            lo = lo >= '0' && lo <= '9' ? lo - '0' :
                 lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 :
                 lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : 16;
            if (hi > 15 || lo > 15) return false;
            value = (unsigned char)((hi << 4) | lo);
        }
        if (value == 0 || used + 1 >= capacity) return false;
        output[used++] = (char)value;
    }
    output[used] = 0;
    return true;
}

static bool parse_form(char *body, wifi_store_record_t *credentials)
{
    bool found_ssid = false, found_password = false;
    credentials->state = WIFI_STORE_CONFIGURED;
    char *part = body;
    while (part && *part) {
        char *next = strchr(part, '&');
        if (next) *next++ = 0;
        char *equals = strchr(part, '=');
        if (!equals) return false;
        *equals++ = 0;
        size_t length = strlen(equals);
        if (strcmp(part, "ssid") == 0 && !found_ssid) {
            if (!decode_field(equals, length, credentials->ssid, sizeof(credentials->ssid))) return false;
            found_ssid = true;
        } else if (strcmp(part, "password") == 0 && !found_password) {
            if (!decode_field(equals, length, credentials->password, sizeof(credentials->password))) return false;
            found_password = true;
        } else return false;
        part = next;
    }
    return found_ssid && found_password &&
           wifi_store_valid_credentials(credentials->ssid, credentials->password);
}

static esp_err_t portal_post(httpd_req_t *request)
{
    if (!cap_matches(request->uri, "")) return httpd_resp_send_404(request);
    if (request->content_len <= 0 || request->content_len > BODY_LIMIT) {
        httpd_resp_set_status(request, "413 Payload Too Large");
        return httpd_resp_sendstr(request, "Invalid form length");
    }
    char body[BODY_LIMIT + 1] = {0};
    command_t command = {.kind = CMD_SUBMIT};
    int received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) {
            memset(body, 0, sizeof(body));
            return ESP_FAIL;
        }
        received += count;
    }
    bool valid = parse_form(body, &command.credentials);
    memset(body, 0, sizeof(body));
    if (!valid) {
        memset(&command, 0, sizeof(command));
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "Check the network name and password");
    }
    portENTER_CRITICAL(&s_cap_mux);
    bool stalled = s_manager.radio_recovery == RADIO_FAILED || s_manager.tx_fault;
    bool busy = s_manager.queued_attempt || s_manager.http_busy ||
                s_manager.radio_recovery != RADIO_READY;
    if (!busy && !stalled) {
        command.attempt = ++s_manager.attempt;
        s_manager.queued_attempt = command.attempt;
        s_manager.http_busy = true;
        s_manager.status_active = true;
        memcpy(command.capability, s_manager.cap, sizeof(command.capability));
        command.cap_deadline = s_manager.cap_deadline;
        strcpy(s_manager.status_state, "queued");
        strcpy(s_manager.status_message, "Waiting to test the connection…");
    }
    portEXIT_CRITICAL(&s_cap_mux);
    if (stalled) { memset(&command, 0, sizeof(command)); httpd_resp_set_status(request, "503 Service Unavailable"); return httpd_resp_sendstr(request, "Wi-Fi radio unavailable. Restart the device."); }
    if (busy) { memset(&command, 0, sizeof(command)); httpd_resp_set_status(request, "409 Conflict"); return httpd_resp_sendstr(request, "A connection test is already running"); }
    uint32_t accepted = command.attempt;
    bool queued = xQueueSend(s_manager.queue, &command, 0) == pdTRUE;
    memset(&command, 0, sizeof(command));
    if (!queued) {
        portENTER_CRITICAL(&s_cap_mux); s_manager.queued_attempt = 0; s_manager.http_busy = false; s_manager.status_active = false; strcpy(s_manager.status_state, "failed"); s_manager.status_grace_deadline = esp_timer_get_time() + SETUP_GRACE_US; portEXIT_CRITICAL(&s_cap_mux);
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Please retry in a moment");
    }
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    cJSON *response = cJSON_CreateObject();
    if (!response) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(response, "attempt", accepted);
    return send_json(request, response);
}

static esp_err_t start_server(void)
{
    if (s_manager.server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_OAUTH_PROBE_HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 6144;
    esp_err_t err = httpd_start(&s_manager.server, &config);
    if (err != ESP_OK) return err;
    httpd_uri_t get = {.uri = "/w/*", .method = HTTP_GET, .handler = portal_get};
    httpd_uri_t post = {.uri = "/w/*", .method = HTTP_POST, .handler = portal_post};
    err = httpd_register_uri_handler(s_manager.server, &get);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_manager.server, &post);
    if (err != ESP_OK) {
        httpd_stop(s_manager.server);
        s_manager.server = NULL;
    } else {
        esp_err_t discovery_err = device_discovery_http_start(config.server_port);
        if (discovery_err != ESP_OK)
            ESP_LOGW(TAG, "HTTP discovery unavailable: %s", esp_err_to_name(discovery_err));
    }
    return err;
}

static void refresh_capability(void)
{
    char cap[33];
    random_hex(cap, 16);
    portENTER_CRITICAL(&s_cap_mux);
    memcpy(s_manager.cap, cap, sizeof(cap));
    s_manager.cap_deadline = esp_timer_get_time() + CAP_LIFETIME_US;
    s_manager.scan_state[0] = 0;
    s_manager.network_count = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    if (!device_discovery_setup_url(s_manager.page_url, sizeof(s_manager.page_url),
                                    "w", cap, "192.168.4.1"))
        ESP_LOGW(TAG, "Setup URL could not be rendered");
    show_setup("Enter your network details on the phone");
}

static void refresh_setup_url(int64_t now)
{
    if (!wifi_manager_is_provisioning() || !s_manager.ap_started ||
        !s_manager.server || s_manager.setup_committed ||
        !s_manager.cap[0] || now >= s_manager.cap_deadline) return;
    char url[sizeof(s_manager.page_url)];
    if (!device_discovery_setup_url(url, sizeof(url), "w", s_manager.cap,
                                    "192.168.4.1")) return;
    if (device_discovery_update_url(s_manager.page_url, sizeof(s_manager.page_url), url) &&
        s_manager.phone_joined)
        show_setup(s_manager.setup_message);
}

static esp_err_t start_setup(void)
{
    atomic_store_explicit(&s_provisioning, true, memory_order_release);
    xEventGroupClearBits(s_manager.events, OAUTH_WIFI_CONNECTED | OAUTH_WIFI_RECONNECTED);
    if (!s_manager.ap) {
        s_manager.ap = esp_netif_create_default_wifi_ap();
        if (!s_manager.ap) return ESP_ERR_NO_MEM;
    }
    esp_err_t hostname_err = esp_netif_set_hostname(s_manager.ap, device_identity_name());
    if (hostname_err != ESP_OK) return hostname_err;
    if (!s_manager.ap_ssid[0]) {
        snprintf(s_manager.ap_ssid, sizeof(s_manager.ap_ssid), "%s", device_identity_name());
        random_hex(s_manager.ap_password, 8);
        snprintf(s_manager.join_qr, sizeof(s_manager.join_qr),
                 "WIFI:T:WPA;S:%s;P:%s;;", s_manager.ap_ssid, s_manager.ap_password);
    }
    wifi_config_t ap_config = {0};
    memcpy(ap_config.ap.ssid, s_manager.ap_ssid, strlen(s_manager.ap_ssid));
    memcpy(ap_config.ap.password, s_manager.ap_password, strlen(s_manager.ap_password));
    ap_config.ap.ssid_len = strlen(s_manager.ap_ssid);
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 4;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) return err;
    s_manager.ap_started = true;
    s_manager.phone_joined = false;
    s_manager.phone_count = s_manager.ready_count = 0;
    memset(s_manager.phone_macs, 0, sizeof(s_manager.phone_macs));
    memset(s_manager.ready_macs, 0, sizeof(s_manager.ready_macs));
    portENTER_CRITICAL(&s_cap_mux); s_manager.queued_attempt = 0; portEXIT_CRITICAL(&s_cap_mux);
    set_status("idle", "Choose a network to begin.");
    refresh_capability();
    err = start_server();
    if (err == ESP_OK) show_setup("Join the temporary Wi-Fi network");
    else show_setup("Setup page unavailable; retrying");
    return err;
}

static void finish_setup(void)
{
    /* HTTP callbacks never wait on this task, so stopping the server is safe. */
    if (s_manager.server) {
        device_discovery_http_stop();
        httpd_stop(s_manager.server);
        s_manager.server = NULL;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not stop setup AP: %s", esp_err_to_name(err));
        show_setup("Could not finish setup; retrying");
        return;
    }
    s_manager.ap_started = false;
    s_manager.setup_authorized = false;
    s_manager.trial_active = false;
    s_manager.pending_save = false;
    s_manager.setup_committed = false;
    portENTER_CRITICAL(&s_cap_mux); s_manager.queued_attempt = 0; portEXIT_CRITICAL(&s_cap_mux);
    s_manager.phone_count = s_manager.ready_count = 0;
    s_manager.phone_joined = false;
    s_manager.ap_ssid[0] = 0;
    s_manager.ap_password[0] = 0;
    portENTER_CRITICAL(&s_cap_mux);
    s_manager.cap[0] = 0;
    s_manager.cap_deadline = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    s_manager.join_qr[0] = 0;
    s_manager.page_url[0] = 0;
    s_manager.setup_message = NULL;
    atomic_store_explicit(&s_provisioning, false, memory_order_release);
    display_wifi_setup_done();
    if (s_manager.link_has_ip)
        xEventGroupSetBits(s_manager.events, OAUTH_WIFI_CONNECTED | OAUTH_WIFI_RECONNECTED);
    else
        s_manager.next_retry = esp_timer_get_time();
}

static void set_scan_state(const char *state)
{
    portENTER_CRITICAL(&s_cap_mux);
    if (strcmp(state, "ready") != 0) {
        s_manager.network_count = 0;
        memset(s_manager.networks, 0, sizeof(s_manager.networks));
    }
    snprintf(s_manager.scan_state, sizeof(s_manager.scan_state), "%s", state);
    portEXIT_CRITICAL(&s_cap_mux);
}

static void capture_tx_limit(void)
{
    int8_t limit = 0;
    esp_err_t err = esp_wifi_get_max_tx_power(&limit);
    if (err == ESP_OK) {
        s_manager.tx_original_limit = limit;
        s_manager.tx_original_known = true;
        ESP_LOGI(TAG, "Initial Wi-Fi TX limit: %d quarter dBm", (int)limit);
    } else {
        /* The normal connection still works; an unknown limit rules out fallback. */
        ESP_LOGW(TAG, "Wi-Fi TX limit read failed; fallback disabled: %s", esp_err_to_name(err));
    }
}

static bool restore_tx_limit(void)
{
    if (!s_manager.tx_low && !s_manager.tx_fault) return true;
    if (!s_manager.tx_original_known) return false;
    esp_err_t err = esp_wifi_set_max_tx_power(s_manager.tx_original_limit);
    int8_t actual = 0;
    if (err == ESP_OK) err = esp_wifi_get_max_tx_power(&actual);
    if (err != ESP_OK || actual != s_manager.tx_original_limit) {
        s_manager.tx_fault = true;
        ESP_LOGW(TAG, "Wi-Fi TX restore failed: err=%s expected=%d actual=%d",
                 esp_err_to_name(err), (int)s_manager.tx_original_limit, (int)actual);
        return false;
    }
    s_manager.tx_low = false;
    s_manager.tx_fault = false;
    ESP_LOGI(TAG, "Wi-Fi TX limit restored: %d quarter dBm", (int)actual);
    return true;
}

static bool set_low_tx_limit(void)
{
    if (!s_manager.tx_original_known || s_manager.tx_fault) return false;
    esp_err_t err = esp_wifi_set_max_tx_power(WIFI_TX_FALLBACK_QUARTER_DBM);
    int8_t actual = 0;
    if (err == ESP_OK) err = esp_wifi_get_max_tx_power(&actual);
    if (err == ESP_OK && actual == WIFI_TX_FALLBACK_QUARTER_DBM) {
        s_manager.tx_low = true;
        ESP_LOGI(TAG, "Wi-Fi TX fallback active: %d quarter dBm", (int)actual);
        return true;
    }
    /* A failed setter or readback may still have changed the radio. */
    s_manager.tx_fault = true;
    ESP_LOGW(TAG, "Wi-Fi TX fallback rejected: err=%s actual=%d",
             esp_err_to_name(err), (int)actual);
    (void)restore_tx_limit();
    return false;
}

static bool start_saved_connect(void)
{
    if (!restore_tx_limit()) return false;
    /* A backoff cycle starts at normal power and may earn one low-power retry. */
    s_manager.tx_fallback_attempted = false;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(err));
    s_manager.saved_connecting = err == ESP_OK;
    if (s_manager.saved_connecting)
        s_manager.saved_connect_deadline = esp_timer_get_time() + TRIAL_TIMEOUT_US;
    return err == ESP_OK;
}

static void start_trial(const wifi_store_record_t *credentials)
{
    if (s_manager.trial_active) return;
    if (!restore_tx_limit()) {
        set_status("failed", "Wi-Fi radio could not reset. Try again.");
        show_setup("Wi-Fi radio could not reset. Try again.");
        return;
    }
    s_manager.tx_fallback_attempted = false;
    s_manager.tx_retry_pending = false;
    s_manager.pending_save = false;
    s_manager.link_has_ip = false;
    s_manager.trial = *credentials;
    s_manager.trial_active = true;
    s_manager.sta_associated = false;
    set_status("testing", "Testing router connection…");
    (void)esp_wifi_scan_stop();
    (void)esp_wifi_clear_ap_list();
    s_manager.scan_started_at = 0;
    set_scan_state("busy");
    s_manager.trial_started_at = esp_timer_get_time();
    s_manager.trial_deadline = s_manager.trial_started_at + TRIAL_TIMEOUT_US;
    wifi_config_t station = {0};
    memcpy(station.sta.ssid, credentials->ssid, strlen(credentials->ssid));
    memcpy(station.sta.password, credentials->password, strlen(credentials->password));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Trial set_config failed: %s", esp_err_to_name(err));
    else {
        err = esp_wifi_connect();
        if (err != ESP_OK)
            ESP_LOGW(TAG, "Trial connect start failed: %s", esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        s_manager.trial_active = false;
        set_scan_state("");
        set_status("failed", "Connection could not start. Try again.");
        show_setup("Connection could not start. Try again.");
    } else show_setup("Testing the Wi-Fi connection...");
}

static bool mac_present(uint8_t list[4][6], uint8_t count, const uint8_t mac[6])
{
    for (uint8_t i = 0; i < count; ++i) if (!memcmp(list[i], mac, 6)) return true;
    return false;
}

static void mac_remove(uint8_t list[4][6], uint8_t *count, const uint8_t mac[6])
{
    for (uint8_t i = 0; i < *count; ++i) if (!memcmp(list[i], mac, 6)) {
        --*count; memcpy(list[i], list[*count], 6); return;
    }
}

static bool valid_utf8_ssid(const uint8_t *ssid)
{
    for (size_t i = 0; i < 32 && ssid[i]; ) {
        uint8_t c = ssid[i];
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) { ++i; continue; }
        unsigned extra = c >= 0xc2 && c <= 0xdf ? 1 :
                         c >= 0xe0 && c <= 0xef ? 2 :
                         c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!extra || i + extra >= 32) return false;
        for (unsigned j = 1; j <= extra; ++j)
            if ((ssid[i+j] & 0xc0) != 0x80 || !ssid[i+j]) return false;
        if (c == 0xe0 && ssid[i+1] < 0xa0) return false;
        if (c == 0xed && ssid[i+1] >= 0xa0) return false;
        if (c == 0xf0 && ssid[i+1] < 0x90) return false;
        if (c == 0xf4 && ssid[i+1] >= 0x90) return false;
        i += extra + 1;
    }
    return true;
}

static void scan_done(void)
{
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) { (void)esp_wifi_clear_ap_list(); set_scan_state("error"); return; }
    wifi_ap_record_t records[SCAN_LIMIT];
    uint16_t kept = count > SCAN_LIMIT ? SCAN_LIMIT : count;
    if (!kept) { (void)esp_wifi_clear_ap_list(); }
    else if (esp_wifi_scan_get_ap_records(&kept, records) != ESP_OK) { (void)esp_wifi_clear_ap_list(); set_scan_state("error"); return; }
    (void)esp_wifi_clear_ap_list();
    char names[SCAN_LIMIT][33] = {{0}};
    uint8_t n = 0;
    for (uint16_t i = 0; i < kept; ++i) {
        if (!records[i].ssid[0] || records[i].primary > 14 ||
            !valid_utf8_ssid(records[i].ssid) ||
            !strcmp((const char *)records[i].ssid, s_manager.ap_ssid)) continue;
        bool duplicate = false;
        for (uint8_t j = 0; j < n; ++j) if (!strcmp(names[j], (const char *)records[i].ssid)) duplicate = true;
        if (duplicate) continue;
        memcpy(names[n], records[i].ssid, 32); names[n][32] = 0; ++n;
    }
    portENTER_CRITICAL(&s_cap_mux);
    memcpy(s_manager.networks, names, sizeof(names));
    s_manager.network_count = n;
    strcpy(s_manager.scan_state, "ready");
    s_manager.scan_cached_at = esp_timer_get_time();
    portEXIT_CRITICAL(&s_cap_mux);
}

static bool command_cap_valid(const command_t *command, int64_t now)
{
    portENTER_CRITICAL(&s_cap_mux);
    bool valid = command->cap_deadline == s_manager.cap_deadline &&
                 now < command->cap_deadline &&
                 memcmp(command->capability, s_manager.cap, sizeof(s_manager.cap)) == 0;
    portEXIT_CRITICAL(&s_cap_mux);
    return valid;
}

static void discard_deferred_trial(const char *message)
{
    if (!s_manager.deferred_submit.attempt) return;
    portENTER_CRITICAL(&s_cap_mux);
    if (s_manager.queued_attempt == s_manager.deferred_submit.attempt)
        s_manager.queued_attempt = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    memset(&s_manager.deferred_submit, 0, sizeof(s_manager.deferred_submit));
    s_manager.deferred_deadline = 0;
    set_status("failed", message);
    show_setup(message);
}

static void start_deferred_trial(int64_t now)
{
    if (!s_manager.deferred_submit.attempt ||
        s_manager.radio_recovery != RADIO_READY) return;
    if (!s_manager.ap_started || !command_cap_valid(&s_manager.deferred_submit, now) ||
        now >= s_manager.deferred_deadline) {
        discard_deferred_trial("Connection reset took too long. Try again.");
        return;
    }
    wifi_store_record_t credentials = s_manager.deferred_submit.credentials;
    portENTER_CRITICAL(&s_cap_mux);
    if (s_manager.queued_attempt == s_manager.deferred_submit.attempt)
        s_manager.queued_attempt = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    memset(&s_manager.deferred_submit, 0, sizeof(s_manager.deferred_submit));
    s_manager.deferred_deadline = 0;
    start_trial(&credentials);
    memset(&credentials, 0, sizeof(credentials));
}

static void fail_radio_recovery(void)
{
    const char *message = s_manager.tx_fault ?
        "Wi-Fi radio could not reset. Restart the device." :
        "Wi-Fi recovery failed. Restart the device.";
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Could not restore setup hotspot: %s", esp_err_to_name(err));
    portENTER_CRITICAL(&s_cap_mux);
    s_manager.radio_recovery = RADIO_FAILED;
    portEXIT_CRITICAL(&s_cap_mux);
    s_manager.tx_retry_pending = false;
    if (s_manager.radio_restart_purpose == RADIO_RESTART_MANAGEMENT) {
        s_manager.management_wait_disconnect = false;
        s_manager.management_disconnect_deadline = 0;
        s_manager.management_trial_active = false;
        memset(&s_manager.management_credentials, 0,
               sizeof(s_manager.management_credentials));
    }
    if (s_manager.radio_restart_purpose == RADIO_RESTART_TX_FALLBACK) {
        s_manager.trial_active = false;
        memset(&s_manager.trial, 0, sizeof(s_manager.trial));
        set_scan_state("");
    }
    if (s_manager.deferred_submit.attempt) discard_deferred_trial(message);
    else {
        set_status("failed", message);
        if (wifi_manager_is_provisioning()) show_setup(message);
        else display_status("Wi-Fi", "Recovery failed");
    }
}

static void restart_for_management(int64_t now)
{
    s_manager.management_wait_disconnect = false;
    s_manager.management_disconnect_deadline = 0;
    s_manager.saved_connecting = false;
    s_manager.radio_restart_purpose = RADIO_RESTART_MANAGEMENT;
    s_manager.radio_event_before = atomic_load_explicit(
        &s_sta_stop_count, memory_order_acquire);
    portENTER_CRITICAL(&s_cap_mux);
    s_manager.radio_recovery = RADIO_WAIT_STOP;
    portEXIT_CRITICAL(&s_cap_mux);
    s_manager.radio_recovery_deadline = now + RADIO_RECOVERY_TIMEOUT_US;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Management radio stop failed: %s", esp_err_to_name(err));
        fail_radio_recovery();
    }
}

static void handle_radio_event(command_kind_t kind, uint32_t event, int64_t now)
{
    if (kind == CMD_STA_STOP && s_manager.radio_recovery == RADIO_WAIT_STOP &&
        event > s_manager.radio_event_before) {
        s_manager.radio_event_before = atomic_load_explicit(
            &s_sta_start_count, memory_order_acquire);
        esp_err_t err = esp_wifi_start();
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_cap_mux);
            s_manager.radio_recovery = RADIO_WAIT_START;
            portEXIT_CRITICAL(&s_cap_mux);
            s_manager.radio_recovery_deadline = now + RADIO_RECOVERY_TIMEOUT_US;
        } else fail_radio_recovery();
    } else if (kind == CMD_STA_START &&
               s_manager.radio_recovery == RADIO_WAIT_START &&
               event > s_manager.radio_event_before) {
        if (s_manager.radio_restart_purpose == RADIO_RESTART_CANCELLED) {
            portENTER_CRITICAL(&s_cap_mux);
            s_manager.radio_recovery = RADIO_READY;
            portEXIT_CRITICAL(&s_cap_mux);
            return;
        }
        if (s_manager.radio_restart_purpose == RADIO_RESTART_TX_FALLBACK) {
            bool low_ready = set_low_tx_limit();
            portENTER_CRITICAL(&s_cap_mux);
            s_manager.radio_recovery = RADIO_READY;
            portEXIT_CRITICAL(&s_cap_mux);
            if (low_ready) {
                s_manager.tx_retry_pending = true;
                s_manager.tx_retry_at = now + LOW_TX_RETRY_DELAY_US;
                if (s_manager.trial_active) {
                    s_manager.trial_deadline = s_manager.tx_retry_at + LOW_TX_TRIAL_TIMEOUT_US;
                    show_setup("Testing the Wi-Fi connection...");
                }
                ESP_LOGI(TAG, "Retrying Wi-Fi connection after radio restart at reduced TX power");
            } else if (s_manager.trial_active) {
                s_manager.trial_active = false;
                memset(&s_manager.trial, 0, sizeof(s_manager.trial));
                set_scan_state("");
                set_status("failed", s_manager.tx_fault ?
                           "Wi-Fi radio could not reset. Try again." :
                           "Connection could not restart. Try again.");
                show_setup(s_manager.tx_fault ?
                           "Wi-Fi radio could not reset. Try again." :
                           "Connection could not restart. Try again.");
            } else {
                s_manager.next_retry = now + (int64_t)s_manager.retry_seconds * 1000000;
            }
            return;
        }
        if (s_manager.radio_restart_purpose == RADIO_RESTART_MANAGEMENT) {
            portENTER_CRITICAL(&s_cap_mux);
            s_manager.radio_recovery = RADIO_READY;
            portEXIT_CRITICAL(&s_cap_mux);
            wifi_store_record_t credentials = s_manager.management_credentials;
            memset(&s_manager.management_credentials, 0,
                   sizeof(s_manager.management_credentials));
            s_manager.management_trial_active = true;
            start_trial(&credentials);
            memset(&credentials, 0, sizeof(credentials));
            return;
        }
        if (s_manager.tx_restore_after_start) {
            s_manager.tx_restore_after_start = false;
            if (!restore_tx_limit()) {
                fail_radio_recovery();
                return;
            }
        }
        portENTER_CRITICAL(&s_cap_mux);
        s_manager.radio_recovery = RADIO_READY;
        portEXIT_CRITICAL(&s_cap_mux);
        if (wifi_manager_is_provisioning()) {
            if (!s_manager.deferred_submit.attempt)
                set_status("failed", "Connection timed out. Rejoin setup and retry.");
            show_setup("Rejoin the temporary Wi-Fi network to retry.");
            start_deferred_trial(now);
        } else {
            s_manager.next_retry = now + (int64_t)s_manager.retry_seconds * 1000000;
        }
    }
}

static void worker(void *arg)
{
    (void)arg;
    capture_tx_limit();
    if (wifi_manager_is_provisioning()) {
        if (start_setup() != ESP_OK) ESP_LOGW(TAG, "Setup hotspot start failed");
        else {
            wifi_store_record_t candidate;
            if (box_config_pending_wifi(&candidate)) {
                s_manager.installer_candidate_active = true;
                start_trial(&candidate);
                memset(&candidate, 0, sizeof(candidate));
            }
        }
    } else {
        (void)start_saved_connect();
        s_manager.next_retry = esp_timer_get_time() + 15000000LL;
    }
    for (;;) {
        command_t command = {0};
        bool received = xQueueReceive(s_manager.queue, &command,
                                      pdMS_TO_TICKS(s_manager.tx_retry_pending ? 100 : 1000)) == pdTRUE;
        int64_t now = esp_timer_get_time();
        if (received) {
            switch (command.kind) {
#if CONFIG_HELLO_BOX_USB_TEST_HELPER
            case CMD_USB_TEST_INFO:
                emit_usb_test_info(now);
                break;
#endif
            case CMD_FORGET: {
                wifi_store_record_t forgotten = {.state = WIFI_STORE_FORGOTTEN};
                esp_err_t err = wifi_store_save(&forgotten);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not commit forgotten state: %s", esp_err_to_name(err));
                    atomic_store_explicit(&s_provisioning, false, memory_order_release);
                    display_status("Storage error", "Could not forget Wi-Fi");
                    break;
                }
                atomic_store_explicit(&s_durable_configured, false, memory_order_release);
                atomic_store_explicit(&s_connected, false, memory_order_release);
                memset(&s_manager.selected, 0, sizeof(s_manager.selected));
                memset(&s_manager.trial, 0, sizeof(s_manager.trial));
                s_manager.trial_active = false;
                s_manager.pending_save = false;
                s_manager.setup_committed = false;
                s_manager.setup_authorized = true;
                if (s_manager.radio_restart_purpose == RADIO_RESTART_TX_FALLBACK &&
                    s_manager.radio_recovery != RADIO_READY)
                    s_manager.radio_restart_purpose = RADIO_RESTART_CANCELLED;
                s_manager.tx_retry_pending = false;
                s_manager.tx_fallback_attempted = false;
                s_manager.saved_connecting = false;
                s_manager.link_has_ip = false;
                xEventGroupClearBits(s_manager.events, OAUTH_WIFI_CONNECTED | OAUTH_WIFI_RECONNECTED);
                (void)esp_wifi_disconnect();
                bool tx_restored = restore_tx_limit();
                wifi_config_t empty = {0};
                (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
                if (start_setup() != ESP_OK) ESP_LOGW(TAG, "Setup hotspot start failed");
                if (!tx_restored) {
                    set_status("failed", "Wi-Fi radio could not reset. Try again.");
                    show_setup("Wi-Fi radio could not reset. Try again.");
                }
                break;
            }
            case CMD_STA_CONNECTED:
                if (s_manager.trial_active && s_manager.radio_recovery == RADIO_READY)
                    s_manager.sta_associated = true;
                break;
            case CMD_AP_STOP:
                s_manager.phone_joined = false;
                s_manager.phone_count = s_manager.ready_count = 0;
                memset(s_manager.phone_macs, 0, sizeof(s_manager.phone_macs));
                memset(s_manager.ready_macs, 0, sizeof(s_manager.ready_macs));
                if (s_manager.radio_recovery != RADIO_READY)
                    show_setup("Wi-Fi is restarting. Rejoin the temporary network.");
                break;
            case CMD_STA_STOP:
                handle_radio_event(CMD_STA_STOP, command.radio_event, now);
                break;
            case CMD_STA_START:
                handle_radio_event(CMD_STA_START, command.radio_event, now);
                break;
            case CMD_SCAN:
                if (!command_cap_valid(&command, now)) {
                    portENTER_CRITICAL(&s_cap_mux);
                    if (!memcmp(command.capability, s_manager.cap, sizeof(s_manager.cap)) &&
                        !strcmp(s_manager.scan_state, "scanning")) strcpy(s_manager.scan_state, "error");
                    portEXIT_CRITICAL(&s_cap_mux);
                    break;
                }
                if (s_manager.radio_recovery == RADIO_READY &&
                    !s_manager.trial_active && !s_manager.pending_save && !s_manager.setup_committed) {
                    wifi_scan_config_t config = {.show_hidden = false};
                    esp_err_t err = esp_wifi_scan_start(&config, false);
                    if (err == ESP_OK) s_manager.scan_started_at = now;
                    else {
                        ESP_LOGW(TAG, "Setup scan start failed: %s", esp_err_to_name(err));
                        set_scan_state("error");
                    }
                } else { portENTER_CRITICAL(&s_cap_mux); strcpy(s_manager.scan_state, "busy"); portEXIT_CRITICAL(&s_cap_mux); }
                break;
            case CMD_SCAN_DONE: {
                s_manager.scan_started_at = 0;
                portENTER_CRITICAL(&s_cap_mux);
                bool scanning = !strcmp(s_manager.scan_state, "scanning");
                portEXIT_CRITICAL(&s_cap_mux);
                if (scanning && s_manager.radio_recovery == RADIO_READY &&
                    !s_manager.trial_active && !s_manager.pending_save &&
                    !s_manager.setup_committed) scan_done();
                else { (void)esp_wifi_clear_ap_list(); if (s_manager.trial_active || s_manager.pending_save) set_scan_state("busy"); }
                break;
            }
            case CMD_GOT_IP:
                if (s_manager.radio_recovery != RADIO_READY) break;
                if (s_manager.tx_retry_pending) break;
                if (s_manager.trial_active) {
                    wifi_ap_record_t current = {0};
                    if (!s_manager.sta_associated || esp_wifi_sta_get_ap_info(&current) != ESP_OK ||
                        strcmp((const char *)current.ssid, s_manager.trial.ssid)) break;
                } else if (s_manager.management_restoring) {
                    wifi_ap_record_t current = {0};
                    if (esp_wifi_sta_get_ap_info(&current) != ESP_OK ||
                        strcmp((const char *)current.ssid, s_manager.selected.ssid)) break;
                }
                if (s_manager.tx_low) {
                    int8_t actual = 0;
                    esp_err_t tx_err = esp_wifi_get_max_tx_power(&actual);
                    if (tx_err == ESP_OK)
                        ESP_LOGI(TAG, "Wi-Fi connected with TX limit: %d quarter dBm", (int)actual);
                    else
                        ESP_LOGW(TAG, "Connected Wi-Fi TX limit read failed: %s",
                                 esp_err_to_name(tx_err));
                }
                s_manager.link_has_ip = true;
                atomic_store_explicit(&s_connected, true, memory_order_release);
                s_manager.saved_connecting = false;
                s_manager.tx_retry_pending = false;
                if (s_manager.trial_active) {
                    s_manager.trial_active = false;
                    esp_err_t err = wifi_store_save(&s_manager.trial);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Could not save connected Wi-Fi: %s", esp_err_to_name(err));
                        s_manager.pending_save = true;
                        s_manager.save_retry_at = now + 5000000LL;
                        set_status("saving", "Connected, but saving failed. Retrying…");
                        show_setup("Connected, but saving failed. Retrying...");
                    } else {
                        s_manager.selected = s_manager.trial;
                        memset(&s_manager.trial, 0, sizeof(s_manager.trial));
                        s_manager.pending_save = false;
                        s_manager.setup_committed = true;
                        s_manager.committed_at = now;
                        bool usb_candidate = s_manager.management_trial_active;
                        set_status("connected", "Connected to Wi-Fi. Setup complete.");
                        if (usb_candidate) finish_setup();
                        else show_setup("Wi-Fi connected. Setup complete.");
                    }
                } else if (s_manager.management_restoring) {
                    s_manager.management_restoring = false;
                    /* Old credentials are already durable; leave setup mode only
                     * after this matching old-network IP is verified. Keep the
                     * candidate's failed status and operation ID for the USB UI. */
                    finish_setup();
                } else if (!wifi_manager_is_provisioning()) {
                    s_manager.retry_seconds = 2;
                    xEventGroupSetBits(s_manager.events, OAUTH_WIFI_CONNECTED | OAUTH_WIFI_RECONNECTED);
                }
                break;
            case CMD_DISCONNECTED:
                if (command.rssi_valid)
                    ESP_LOGW(TAG, "Station disconnected: reason=%u rssi_valid=1 rssi=%d attempt=%lu trial=%u trial_elapsed_ms=%lld saving=%u committed=%u provisioning=%u",
                             (unsigned)command.reason, (int)command.rssi, (unsigned long)s_manager.attempt,
                             (unsigned)s_manager.trial_active,
                             (long long)(s_manager.trial_active ? (now - s_manager.trial_started_at) / 1000 : 0),
                             (unsigned)s_manager.pending_save, (unsigned)s_manager.setup_committed,
                             (unsigned)wifi_manager_is_provisioning());
                else
                    ESP_LOGW(TAG, "Station disconnected: reason=%u rssi_valid=0 attempt=%lu trial=%u trial_elapsed_ms=%lld saving=%u committed=%u provisioning=%u",
                             (unsigned)command.reason, (unsigned long)s_manager.attempt,
                             (unsigned)s_manager.trial_active,
                             (long long)(s_manager.trial_active ? (now - s_manager.trial_started_at) / 1000 : 0),
                             (unsigned)s_manager.pending_save, (unsigned)s_manager.setup_committed,
                             (unsigned)wifi_manager_is_provisioning());
                if (s_manager.management_wait_disconnect) {
                    s_manager.management_wait_disconnect = false;
                    s_manager.management_disconnect_deadline = 0;
                    s_manager.link_has_ip = false;
                    s_manager.saved_connecting = false;
                    s_manager.sta_associated = false;
                    atomic_store_explicit(&s_connected, false, memory_order_release);
                    xEventGroupClearBits(s_manager.events, OAUTH_WIFI_CONNECTED);
                    wifi_store_record_t credentials = s_manager.management_credentials;
                    memset(&s_manager.management_credentials, 0, sizeof(s_manager.management_credentials));
                    s_manager.management_trial_active = true;
                    start_trial(&credentials);
                    memset(&credentials, 0, sizeof(credentials));
                    break;
                }
                if (s_manager.tx_retry_pending) break;
                bool had_ip = s_manager.link_has_ip;
                bool was_connecting = s_manager.trial_active || s_manager.saved_connecting;
                s_manager.link_has_ip = false;
                atomic_store_explicit(&s_connected, false, memory_order_release);
                if (s_manager.management_restoring) {
                    s_manager.management_restoring = false;
                    portENTER_CRITICAL(&s_cap_mux);
                    s_manager.http_busy = false;
                    portEXIT_CRITICAL(&s_cap_mux);
                }
                s_manager.sta_associated = false;
                s_manager.saved_connecting = false;
                xEventGroupClearBits(s_manager.events, OAUTH_WIFI_CONNECTED);
                if (s_manager.radio_recovery != RADIO_READY) break;
                if (had_ip) {
                    s_manager.tx_fallback_attempted = false;
                    if (!restore_tx_limit()) {
                        set_status("failed", "Wi-Fi radio could not reset. Try again.");
                        show_setup("Wi-Fi radio could not reset. Try again.");
                    }
                } else if (was_connecting &&
                           wifi_tx_policy_eligible(command.reason, command.rssi_valid,
                                                   command.rssi, s_manager.tx_fallback_attempted,
                                                   s_manager.tx_original_known,
                                                   s_manager.tx_original_limit)) {
                    s_manager.tx_fallback_attempted = true;
                    s_manager.radio_restart_purpose = RADIO_RESTART_TX_FALLBACK;
                    s_manager.radio_event_before = atomic_load_explicit(
                        &s_sta_stop_count, memory_order_acquire);
                    portENTER_CRITICAL(&s_cap_mux);
                    s_manager.radio_recovery = RADIO_WAIT_STOP;
                    portEXIT_CRITICAL(&s_cap_mux);
                    s_manager.radio_recovery_deadline = now + RADIO_RECOVERY_TIMEOUT_US;
                    esp_err_t stop_err = esp_wifi_stop();
                    if (stop_err != ESP_OK) {
                        ESP_LOGW(TAG, "Fallback station stop failed: %s", esp_err_to_name(stop_err));
                        fail_radio_recovery();
                    }
                    break;
                }
                if (!had_ip && s_manager.tx_low && !restore_tx_limit()) {
                    set_status("failed", "Wi-Fi radio could not reset. Try again.");
                    show_setup("Wi-Fi radio could not reset. Try again.");
                }
                if (s_manager.tx_fault) {
                    s_manager.trial_active = false;
                    s_manager.pending_save = false;
                    s_manager.setup_committed = false;
                    set_scan_state("");
                    set_status("failed", "Wi-Fi radio could not reset. Try again.");
                    if (wifi_manager_is_provisioning())
                        show_setup("Wi-Fi radio could not reset. Try again.");
                    break;
                }
                if (s_manager.pending_save) {
                    s_manager.pending_save = false;
                    set_scan_state("");
                    set_status("failed", "Connection lost before settings could be saved. Retry.");
                    show_setup("Connection lost before saving. Retry setup.");
                }
                if (s_manager.trial_active) {
                    s_manager.trial_active = false;
                    set_scan_state("");
                    set_status("failed", wifi_failure_message(command.reason));
                    show_setup(wifi_failure_device_message(command.reason));
                } else if (s_manager.setup_committed) {
                    s_manager.setup_committed = false;
                    set_scan_state("");
                    set_status("failed", "Wi-Fi connection was lost. Rejoin setup and retry.");
                    show_setup("Connection lost. Retry Wi-Fi setup.");
                } else if (!wifi_manager_is_provisioning()) {
                    s_manager.next_retry = now + (int64_t)s_manager.retry_seconds * 1000000;
                    if (s_manager.retry_seconds < RETRY_MAX_SECONDS) {
                        s_manager.retry_seconds *= 2;
                        if (s_manager.retry_seconds > RETRY_MAX_SECONDS)
                            s_manager.retry_seconds = RETRY_MAX_SECONDS;
                    }
                    display_status("Wi-Fi", "Reconnecting");
                }
                break;
            case CMD_PHONE_JOINED:
                if (s_manager.ap_started &&
                    (s_manager.radio_recovery == RADIO_READY ||
                     s_manager.radio_recovery == RADIO_WAIT_START) &&
                    s_manager.phone_count < 4 &&
                    !mac_present(s_manager.phone_macs, s_manager.phone_count, command.mac))
                    memcpy(s_manager.phone_macs[s_manager.phone_count++], command.mac, 6);
                break;
            case CMD_PHONE_IP:
                if (s_manager.ap_started &&
                    (s_manager.radio_recovery == RADIO_READY ||
                     s_manager.radio_recovery == RADIO_WAIT_START) &&
                    mac_present(s_manager.phone_macs, s_manager.phone_count, command.mac) &&
                    s_manager.ready_count < 4 && !mac_present(s_manager.ready_macs, s_manager.ready_count, command.mac)) {
                    memcpy(s_manager.ready_macs[s_manager.ready_count++], command.mac, 6);
                    s_manager.phone_joined = true;
                    portENTER_CRITICAL(&s_cap_mux);
                    bool preserve = strcmp(s_manager.status_state, "idle") != 0;
                    portEXIT_CRITICAL(&s_cap_mux);
                    show_setup(preserve && s_manager.setup_message ? s_manager.setup_message :
                               "Open the setup page on your phone");
                }
                break;
            case CMD_PHONE_LEFT:
                if (s_manager.ap_started) {
                    mac_remove(s_manager.phone_macs, &s_manager.phone_count, command.mac);
                    mac_remove(s_manager.ready_macs, &s_manager.ready_count, command.mac);
                    s_manager.phone_joined = s_manager.ready_count > 0;
                    portENTER_CRITICAL(&s_cap_mux);
                    bool preserve = strcmp(s_manager.status_state, "idle") != 0;
                    portEXIT_CRITICAL(&s_cap_mux);
                    show_setup(preserve && s_manager.setup_message ? s_manager.setup_message :
                               s_manager.phone_joined ? "Open the setup page on your phone" :
                                                        "Join the temporary Wi-Fi network");
                }
                break;
            case CMD_SUBMIT: {
                if (!command_cap_valid(&command, now)) {
                    portENTER_CRITICAL(&s_cap_mux);
                    bool current = command.attempt == s_manager.queued_attempt;
                    if (current) s_manager.queued_attempt = 0;
                    portEXIT_CRITICAL(&s_cap_mux);
                    if (current) set_status("failed", "Setup link expired. Reopen setup and retry.");
                    break;
                }
                portENTER_CRITICAL(&s_cap_mux);
                bool reserved = command.attempt == s_manager.queued_attempt;
                portEXIT_CRITICAL(&s_cap_mux);
                if (s_manager.ap_started && reserved &&
                    !s_manager.trial_active && !s_manager.pending_save && !s_manager.setup_committed) {
                    if (s_manager.radio_recovery == RADIO_WAIT_STOP ||
                        s_manager.radio_recovery == RADIO_WAIT_START) {
                        s_manager.deferred_submit = command;
                        s_manager.deferred_deadline = now + DEFERRED_TRIAL_TIMEOUT_US;
                    } else if (s_manager.radio_recovery == RADIO_READY) {
                        portENTER_CRITICAL(&s_cap_mux); s_manager.queued_attempt = 0; portEXIT_CRITICAL(&s_cap_mux);
                        start_trial(&command.credentials);
                    } else {
                        portENTER_CRITICAL(&s_cap_mux); s_manager.queued_attempt = 0; portEXIT_CRITICAL(&s_cap_mux);
                        set_status("failed", "Wi-Fi recovery failed. Restart the device.");
                    }
                }
                break;
            }
            case CMD_MANAGEMENT_CONNECT: {
                if (s_manager.trial_active || s_manager.pending_save ||
                    s_manager.setup_committed || s_manager.management_restoring ||
                    s_manager.management_restore_pending ||
                    s_manager.radio_recovery != RADIO_READY) {
                    set_status("failed", "Wi-Fi is busy. Retry in a moment.");
                    break;
                }
                s_manager.setup_authorized = true;
                if (!s_manager.ap_started && start_setup() != ESP_OK) {
                    set_status("failed", "Setup network unavailable. Retry in a moment.");
                    break;
                }
                if (s_manager.link_has_ip || s_manager.saved_connecting ||
                    s_manager.sta_associated) {
                    s_manager.management_credentials = command.credentials;
                    s_manager.management_wait_disconnect = true;
                    s_manager.management_disconnect_deadline =
                        now + MANAGEMENT_DISCONNECT_TIMEOUT_US;
                    esp_err_t err = esp_wifi_disconnect();
                    if (err != ESP_OK) {
                        restart_for_management(now);
                    }
                } else {
                    s_manager.management_trial_active = true;
                    start_trial(&command.credentials);
                }
                break;
            }
            }
        }
        memset(&command, 0, sizeof(command));
        if (s_manager.management_restore_pending &&
            s_manager.radio_recovery == RADIO_READY &&
            !s_manager.trial_active && !s_manager.pending_save) {
            s_manager.management_restore_pending = false;
            wifi_config_t old = {0};
            memcpy(old.sta.ssid, s_manager.selected.ssid,
                   strlen(s_manager.selected.ssid));
            memcpy(old.sta.password, s_manager.selected.password,
                   strlen(s_manager.selected.password));
            esp_err_t restore_err = esp_wifi_set_config(WIFI_IF_STA, &old);
            memset(&old, 0, sizeof(old));
            if (restore_err == ESP_OK && start_saved_connect()) {
                s_manager.management_restoring = true;
                portENTER_CRITICAL(&s_cap_mux);
                s_manager.http_busy = true;
                portEXIT_CRITICAL(&s_cap_mux);
            } else
                ESP_LOGW(TAG, "Previous Wi-Fi reconnect unavailable; setup hotspot remains active");
        }
        if (!received && atomic_exchange_explicit(&s_sta_stop_dropped, false,
                                                  memory_order_acq_rel))
            handle_radio_event(CMD_STA_STOP, atomic_load_explicit(&s_sta_stop_count,
                                                                  memory_order_acquire), now);
        if (!received && atomic_exchange_explicit(&s_sta_start_dropped, false,
                                                  memory_order_acq_rel))
            handle_radio_event(CMD_STA_START, atomic_load_explicit(&s_sta_start_count,
                                                                   memory_order_acquire), now);
        if (atomic_exchange_explicit(&s_scan_done_pending, false, memory_order_acq_rel)) {
            s_manager.scan_started_at = 0;
            portENTER_CRITICAL(&s_cap_mux);
            bool scanning = !strcmp(s_manager.scan_state, "scanning");
            portEXIT_CRITICAL(&s_cap_mux);
            if (scanning && s_manager.radio_recovery == RADIO_READY &&
                !s_manager.trial_active && !s_manager.pending_save &&
                !s_manager.setup_committed) scan_done();
            else { (void)esp_wifi_clear_ap_list(); if (s_manager.trial_active || s_manager.pending_save) set_scan_state("busy"); }
        }
        now = esp_timer_get_time();
        if (s_manager.management_wait_disconnect &&
            now >= s_manager.management_disconnect_deadline)
            restart_for_management(now);
        if (s_manager.tx_retry_pending && now >= s_manager.tx_retry_at) {
            s_manager.tx_retry_pending = false;
            esp_err_t err = esp_wifi_connect();
            if (err == ESP_OK) {
                s_manager.saved_connecting = !s_manager.trial_active;
                if (s_manager.saved_connecting)
                    s_manager.saved_connect_deadline = now + LOW_TX_TRIAL_TIMEOUT_US;
                if (s_manager.trial_active)
                    s_manager.trial_deadline = now + LOW_TX_TRIAL_TIMEOUT_US;
            } else {
                ESP_LOGW(TAG, "Reduced-TX connect failed: %s", esp_err_to_name(err));
                if (!restore_tx_limit()) {
                    set_status("failed", "Wi-Fi radio could not reset. Try again.");
                    show_setup("Wi-Fi radio could not reset. Try again.");
                }
                if (s_manager.trial_active) {
                    s_manager.trial_active = false;
                    set_scan_state("");
                    if (!s_manager.tx_fault) {
                        set_status("failed", "Connection could not start. Try again.");
                        show_setup("Connection could not start. Try again.");
                    }
                } else {
                    s_manager.next_retry = now + (int64_t)s_manager.retry_seconds * 1000000;
                }
            }
        }
        if (s_manager.management_restoring && s_manager.saved_connecting &&
            now >= s_manager.saved_connect_deadline) {
            s_manager.management_restoring = false;
            s_manager.saved_connecting = false;
            portENTER_CRITICAL(&s_cap_mux);
            s_manager.http_busy = false;
            portEXIT_CRITICAL(&s_cap_mux);
            ESP_LOGW(TAG, "Previous Wi-Fi restore timed out; setup hotspot remains active");
        }
        if ((s_manager.radio_recovery == RADIO_WAIT_STOP ||
             s_manager.radio_recovery == RADIO_WAIT_START) &&
            now >= s_manager.radio_recovery_deadline)
            fail_radio_recovery();
        if (s_manager.deferred_submit.attempt && now >= s_manager.deferred_deadline)
            discard_deferred_trial("Wi-Fi recovery failed. Restart the device.");
        if (s_manager.scan_started_at && now - s_manager.scan_started_at >= SCAN_WATCHDOG_US) {
            s_manager.scan_started_at = 0;
            (void)esp_wifi_scan_stop();
            (void)esp_wifi_clear_ap_list();
            set_scan_state("error");
        }
        if (!wifi_manager_is_provisioning() && s_manager.saved_connecting &&
            now >= s_manager.saved_connect_deadline &&
            s_manager.radio_recovery == RADIO_READY) {
            s_manager.saved_connecting = false;
            s_manager.radio_restart_purpose = RADIO_RESTART_TIMEOUT;
            s_manager.tx_restore_after_start = s_manager.tx_low || s_manager.tx_fault;
            s_manager.radio_event_before = atomic_load_explicit(
                &s_sta_stop_count, memory_order_acquire);
            portENTER_CRITICAL(&s_cap_mux);
            s_manager.radio_recovery = RADIO_WAIT_STOP;
            portEXIT_CRITICAL(&s_cap_mux);
            s_manager.radio_recovery_deadline = now + RADIO_RECOVERY_TIMEOUT_US;
            esp_err_t err = esp_wifi_stop();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Timed-out saved connection stop failed: %s", esp_err_to_name(err));
                fail_radio_recovery();
            }
        }
        if (wifi_manager_is_provisioning()) {
            if (s_manager.setup_committed && now - s_manager.committed_at >= SETUP_GRACE_US) {
                finish_setup();
            } else if (s_manager.setup_authorized && !s_manager.ap_started) {
                if (start_setup() != ESP_OK) vTaskDelay(pdMS_TO_TICKS(2000));
            } else if (s_manager.setup_authorized && !s_manager.server) {
                if (start_server() != ESP_OK) vTaskDelay(pdMS_TO_TICKS(2000));
            }
            if (s_manager.trial_active && s_manager.radio_recovery == RADIO_READY &&
                now >= s_manager.trial_deadline) {
                s_manager.trial_active = false;
                s_manager.tx_retry_pending = false;
                s_manager.radio_restart_purpose = RADIO_RESTART_TIMEOUT;
                s_manager.tx_restore_after_start = s_manager.tx_low || s_manager.tx_fault;
                memset(&s_manager.trial, 0, sizeof(s_manager.trial));
                s_manager.phone_joined = false;
                s_manager.phone_count = s_manager.ready_count = 0;
                memset(s_manager.phone_macs, 0, sizeof(s_manager.phone_macs));
                memset(s_manager.ready_macs, 0, sizeof(s_manager.ready_macs));
                s_manager.radio_event_before = atomic_load_explicit(
                    &s_sta_stop_count, memory_order_acquire);
                portENTER_CRITICAL(&s_cap_mux);
                s_manager.radio_recovery = RADIO_WAIT_STOP;
                portEXIT_CRITICAL(&s_cap_mux);
                s_manager.radio_recovery_deadline = now + RADIO_RECOVERY_TIMEOUT_US;
                esp_err_t err = esp_wifi_stop();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Timed-out station stop failed: %s", esp_err_to_name(err));
                    fail_radio_recovery();
                }
                set_scan_state("");
                if (s_manager.radio_recovery != RADIO_FAILED) {
                    set_status("failed", "Connection timed out. Wi-Fi is restarting.");
                    show_setup("Connection timed out. Rejoin the temporary Wi-Fi network.");
                }
            }
            if (s_manager.pending_save && now >= s_manager.save_retry_at) {
                esp_err_t err = wifi_store_save(&s_manager.trial);
                if (err == ESP_OK) {
                    s_manager.selected = s_manager.trial;
                    memset(&s_manager.trial, 0, sizeof(s_manager.trial));
                    s_manager.pending_save = false;
                    s_manager.setup_committed = true;
                    s_manager.committed_at = now;
                    bool usb_candidate = s_manager.management_trial_active;
                    set_status("connected", "Connected to Wi-Fi. Setup complete.");
                    if (usb_candidate) finish_setup();
                    else show_setup("Wi-Fi connected. Setup complete.");
                } else {
                    s_manager.save_retry_at = now + 5000000LL;
                    show_setup("Connected, but saving failed. Retrying...");
                }
            }
            portENTER_CRITICAL(&s_cap_mux);
            bool keep_status_cap = s_manager.status_active || now < s_manager.status_grace_deadline;
            portEXIT_CRITICAL(&s_cap_mux);
            if (s_manager.ap_started && !s_manager.setup_committed && !keep_status_cap &&
                now >= s_manager.cap_deadline) refresh_capability();
            refresh_setup_url(now);
        } else if (!(xEventGroupGetBits(s_manager.events) & OAUTH_WIFI_CONNECTED) &&
                   !s_manager.tx_retry_pending && !s_manager.saved_connecting &&
                   s_manager.radio_recovery == RADIO_READY &&
                   now >= s_manager.next_retry) {
            (void)start_saved_connect();
            s_manager.next_retry = now + (int64_t)s_manager.retry_seconds * 1000000;
        }
    }
}

esp_err_t wifi_manager_start(const char *fallback_ssid, const char *fallback_password,
                             EventGroupHandle_t *events_out, esp_netif_t **sta_out)
{
    if (!events_out || !sta_out) return ESP_ERR_INVALID_ARG;
    *events_out = NULL;
    *sta_out = NULL;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;
    s_manager.sta = esp_netif_create_default_wifi_sta();
    if (!s_manager.sta) return ESP_ERR_NO_MEM;
    err = esp_netif_set_hostname(s_manager.sta, device_identity_name());
    if (err != ESP_OK) return err;
    esp_err_t discovery_err = device_discovery_start();
    if (discovery_err != ESP_OK)
        ESP_LOGW(TAG, "mDNS unavailable, using IP setup links: %s",
                 esp_err_to_name(discovery_err));
    s_manager.events = xEventGroupCreate();
    s_manager.queue = xQueueCreate(10, sizeof(command_t));
    if (!s_manager.events || !s_manager.queue) return ESP_ERR_NO_MEM;
    s_manager.retry_seconds = 2;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) return err;
    bool reset_pending = false;
    err = factory_reset_is_pending(&reset_pending);
    if (err != ESP_OK) return err;
    if (reset_pending) {
        err = esp_wifi_restore();
        if (err != ESP_OK) return err;
        err = factory_reset_complete();
        if (err != ESP_OK) return err;
    }
    wifi_config_t driver = {0};
    err = esp_wifi_get_config(WIFI_IF_STA, &driver);
    if (err != ESP_OK) return err;
    wifi_store_record_t record;
    err = wifi_store_load(&record);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi app record unreadable; opening setup");
        memset(&record, 0, sizeof(record));
        record.state = WIFI_STORE_FORGOTTEN;
    }
    char driver_ssid[33] = {0}, driver_password[65] = {0};
    memcpy(driver_ssid, driver.sta.ssid, sizeof(driver.sta.ssid));
    memcpy(driver_password, driver.sta.password, sizeof(driver.sta.password));
    wifi_source_t source = wifi_store_select(&record, driver_ssid, driver_password,
                                              fallback_ssid, fallback_password,
                                              &s_manager.selected);
    atomic_store_explicit(&s_durable_configured, source != WIFI_SOURCE_NONE,
                          memory_order_release);
    wifi_store_record_t installer_candidate;
    bool has_installer_candidate = box_config_pending_wifi(&installer_candidate);
    memset(&installer_candidate, 0, sizeof(installer_candidate));
    /* All subsequent radio changes are RAM-only. App credentials have their own record. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    if (!has_installer_candidate &&
        (source == WIFI_SOURCE_APP || source == WIFI_SOURCE_FALLBACK)) {
        wifi_config_t station = {0};
        memcpy(station.sta.ssid, s_manager.selected.ssid, strlen(s_manager.selected.ssid));
        memcpy(station.sta.password, s_manager.selected.password,
               strlen(s_manager.selected.password));
        err = esp_wifi_set_config(WIFI_IF_STA, &station);
        if (err != ESP_OK) return err;
    } else if (has_installer_candidate) {
        /* Keep the old saved record, but do not auto-connect to it before the trial. */
        wifi_config_t empty_station = {0};
        err = esp_wifi_set_config(WIFI_IF_STA, &empty_station);
        if (err != ESP_OK) return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    if (source == WIFI_SOURCE_NONE || has_installer_candidate) {
        s_manager.setup_authorized = true;
        atomic_store_explicit(&s_provisioning, true, memory_order_release);
    }
    if (xTaskCreate(worker, "wifi_manager", 8192, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
#if CONFIG_HELLO_BOX_USB_TEST_HELPER
    if (!wifi_usb_test_start())
        ESP_LOGW(TAG, "USB test command reader unavailable");
#endif
    *events_out = s_manager.events;
    *sta_out = s_manager.sta;
    return ESP_OK;
}
