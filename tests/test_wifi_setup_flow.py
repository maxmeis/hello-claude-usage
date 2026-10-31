"""Run the production Wi-Fi worker and event handler against host radio/storage stubs."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from tests.test_wifi_portal import function


ROOT = Path(__file__).resolve().parents[1]


HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wifi_failure.h"
#include "wifi_tx_policy.h"

typedef int esp_err_t;
typedef int BaseType_t;
typedef int QueueHandle_t;
typedef int EventGroupHandle_t;
typedef int esp_netif_t;
typedef void *httpd_handle_t;
typedef const char *esp_event_base_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define WIFI_STORE_CONFIGURED 1
#define WIFI_STORE_FORGOTTEN 2
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_MODE_STA 1
#define WIFI_MODE_AP 2
#define WIFI_MODE_APSTA 3
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY -1
#define CMD_FORGET __CMD_FORGET__
#define CMD_GOT_IP __CMD_GOT_IP__
#define CMD_DISCONNECTED __CMD_DISCONNECTED__
#define CMD_STA_STOP __CMD_STA_STOP__
#define CMD_STA_START __CMD_STA_START__
#define CMD_AP_STOP __CMD_AP_STOP__
#define CMD_PHONE_JOINED __CMD_PHONE_JOINED__
#define CMD_PHONE_LEFT __CMD_PHONE_LEFT__
#define CMD_PHONE_IP __CMD_PHONE_IP__
#define CMD_STA_CONNECTED __CMD_STA_CONNECTED__
#define CMD_SCAN __CMD_SCAN__
#define CMD_SCAN_DONE __CMD_SCAN_DONE__
#define CMD_SUBMIT __CMD_SUBMIT__
#define CMD_MANAGEMENT_CONNECT __CMD_MANAGEMENT_CONNECT__
#define CAP_LIFETIME_US 600000000LL
#define TRIAL_TIMEOUT_US 25000000LL
#define LOW_TX_TRIAL_TIMEOUT_US 20000000LL
#define LOW_TX_RETRY_DELAY_US 100000LL
#define DEFERRED_TRIAL_TIMEOUT_US 10000000LL
#define RADIO_RECOVERY_TIMEOUT_US 5000000LL
#define MANAGEMENT_DISCONNECT_TIMEOUT_US 5000000LL
#define SETUP_GRACE_US 15000000LL
#define SCAN_WATCHDOG_US 10000000LL
#define RETRY_MAX_SECONDS 60
#define SCAN_LIMIT 12
#define WIFI_EVENT "WIFI_EVENT"
#define IP_EVENT "IP_EVENT"
#define WIFI_EVENT_STA_CONNECTED 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_AP_STACONNECTED 3
#define WIFI_EVENT_AP_STADISCONNECTED 4
#define WIFI_EVENT_SCAN_DONE 5
#define WIFI_EVENT_STA_STOP 8
#define WIFI_EVENT_STA_START 9
#define WIFI_EVENT_AP_STOP 10
#define IP_EVENT_STA_GOT_IP 6
#define IP_EVENT_AP_STAIPASSIGNED 7
#define OAUTH_WIFI_CONNECTED 1
#define OAUTH_WIFI_RECONNECTED 2
#define ESP_EVENT_ANY_ID -1
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGW(tag, format, ...) capture_warning(format, ##__VA_ARGS__)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define atomic_store(p, value) (*(p) = (value))
#define atomic_load(p) (*(p))
#define atomic_store_explicit(p, value, order) (*(p) = (value))
#define atomic_load_explicit(p, order) (*(p))
#define atomic_fetch_add_explicit(p, value, order) ({ uint32_t old_value = *(p); *(p) += (value); old_value; })
#define atomic_exchange_explicit(p, value, order) ({ bool old_value = *(p); *(p) = (value); old_value; })
#define memory_order_release 0
#define memory_order_acquire 0
#define memory_order_acq_rel 0

static const char wifi_event_base[] = "WIFI_EVENT";
static const char ip_event_base[] = "IP_EVENT";
#undef WIFI_EVENT
#undef IP_EVENT
#define WIFI_EVENT wifi_event_base
#define IP_EVENT ip_event_base

typedef enum { WIFI_STORE_ABSENT = 0 } wifi_store_state_t;
typedef enum { WIFI_SOURCE_NONE = 0 } wifi_source_t;
typedef int command_kind_t;
typedef enum { RADIO_READY, RADIO_WAIT_STOP, RADIO_WAIT_START, RADIO_FAILED } radio_recovery_t;
typedef enum { RADIO_RESTART_TIMEOUT, RADIO_RESTART_TX_FALLBACK,
               RADIO_RESTART_CANCELLED, RADIO_RESTART_MANAGEMENT } radio_restart_purpose_t;
typedef struct { wifi_store_state_t state; char ssid[33]; char password[65]; } wifi_store_record_t;
typedef struct { int kind; wifi_store_record_t credentials; uint8_t mac[6]; uint32_t attempt;
                 uint16_t reason; uint32_t radio_event; int8_t rssi; bool rssi_valid;
                 char capability[33]; int64_t cap_deadline; } command_t;
typedef struct { struct { uint8_t ssid[32]; uint8_t password[64]; } sta;
                 struct { uint8_t ssid[32]; uint8_t password[64]; size_t ssid_len; int authmode, max_connection; } ap; } wifi_config_t;
typedef bool atomic_bool;
typedef struct { uint32_t operation_id; bool configured, connected, provisioning;
                 char state[16], message[96]; } wifi_manager_status_t;
typedef struct { uint8_t ssid[33]; uint8_t primary; } wifi_ap_record_t;
typedef struct { bool show_hidden; } wifi_scan_config_t;
typedef struct { uint8_t mac[6]; } wifi_event_ap_staconnected_t;
typedef struct { uint8_t mac[6]; } wifi_event_ap_stadisconnected_t;
typedef struct { uint8_t reason; int8_t rssi; } wifi_event_sta_disconnected_t;
typedef struct { uint8_t mac[6]; } ip_event_ap_staipassigned_t;
typedef struct { int dummy; } httpd_req_t;
typedef struct { int64_t cap_deadline, trial_deadline, trial_started_at, save_retry_at, committed_at, next_retry, scan_cached_at, saved_connect_deadline;
    int64_t status_grace_deadline, scan_started_at;
    QueueHandle_t queue; EventGroupHandle_t events; esp_netif_t *sta, *ap; httpd_handle_t server;
    wifi_store_record_t selected, trial; bool ap_started, setup_authorized, phone_joined, sta_associated;
    uint8_t phone_macs[4][6], phone_count, ready_macs[4][6], ready_count;
    uint32_t attempt, queued_attempt; bool http_busy, status_active;
    radio_recovery_t radio_recovery; radio_restart_purpose_t radio_restart_purpose;
    int64_t radio_recovery_deadline;
    uint32_t radio_event_before; command_t deferred_submit; int64_t deferred_deadline;
    char status_state[16], status_message[96], scan_state[12];
    char networks[SCAN_LIMIT][33]; uint8_t network_count; bool trial_active, pending_save, setup_committed, link_has_ip;
    unsigned retry_seconds; char ap_ssid[33], ap_password[17], cap[33], join_qr[112], page_url[128];
    const char *setup_message;
    bool tx_original_known, tx_low, tx_fault, tx_fallback_attempted, tx_retry_pending, tx_restore_after_start;
    bool saved_connecting; int8_t tx_original_limit; int64_t tx_retry_at;
    bool installer_candidate_active, installer_cleanup_error;
    uint32_t management_operation_id;
    bool management_wait_disconnect, management_trial_active;
    int64_t management_disconnect_deadline;
    bool management_restore_pending, management_restoring;
    wifi_store_record_t management_credentials;
} manager_t;
typedef int portMUX_TYPE;
static manager_t s_manager;
static bool s_worker_started;
static portMUX_TYPE s_cap_mux;
static bool s_provisioning = true;
static atomic_bool s_durable_configured, s_connected, s_management_queued;
static int fake_server;
static bool s_scan_done_pending;
static bool s_sta_stop_dropped, s_sta_start_dropped;
static uint32_t s_sta_stop_count, s_sta_start_count;
static int64_t now_us;
static int saved, store_fail, connect_calls, disconnect_calls, scan_calls, set_config_calls;
static int8_t tx_power = 52, tx_readback_override = -1;
static int tx_get_calls, tx_set_calls, tx_set_fail, tx_get_fail;
static char tx_call_order[128]; static size_t tx_call_order_len;
static int stop_calls, start_calls, stop_result, start_result, disconnect_result;
static int scan_stop_calls, clear_ap_list_calls;
static int reject_scan_done_queue;
static int reject_radio_event_queue;
static int http_stop_calls, discovery_stop_calls, display_done_calls;
static int scan_num_fail, scan_records_fail;
static bool pending_inbox_available;
static wifi_store_record_t pending_inbox;
static int pending_finish_calls, pending_finish_result, pending_finish_saw_saved;
static uint32_t event_bits;
static bool displayed_phone_ready;
static wifi_config_t last_config;
static char current_ap_ssid[33] = "Home";
static wifi_ap_record_t scan_records[16];
static uint16_t scan_record_count;
static command_t script[16], captured[16];
static int script_count, script_at, captured_count;
static int64_t script_times[8];
static bool script_receives[8];
static jmp_buf stop_worker;
static char warning_log[256];
static void capture_warning(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(warning_log, sizeof(warning_log), format, args);
    va_end(args);
}

static bool wifi_manager_is_provisioning(void) { return s_provisioning; }
static int64_t esp_timer_get_time(void) { return now_us; }
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
static BaseType_t xQueueSend(QueueHandle_t q, const command_t *cmd, int timeout) {
    assert(q == 1 && timeout == 0);
    if (reject_scan_done_queue && cmd->kind == CMD_SCAN_DONE) return pdFALSE;
    if (reject_radio_event_queue && (cmd->kind == CMD_STA_STOP || cmd->kind == CMD_STA_START)) return pdFALSE;
    if (captured_count < 16) captured[captured_count++] = *cmd; return pdTRUE;
}
static BaseType_t xQueueReceive(QueueHandle_t q, command_t *cmd, int timeout) {
    assert(q == 1 && (timeout == 1000 || timeout == 100));
    if (script_at >= script_count) longjmp(stop_worker, 1);
    now_us = script_times[script_at];
    bool receives = script_receives[script_at];
    if (receives) *cmd = script[script_at];
    ++script_at;
    if (!receives) return pdFALSE;
    return pdTRUE;
}
static void vTaskDelay(int ticks) { (void)ticks; }
static esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *config) {
    (void)iface; last_config = *config; ++set_config_calls; return ESP_OK;
}
static esp_err_t esp_wifi_connect(void) { ++connect_calls; return ESP_OK; }
static esp_err_t esp_wifi_get_max_tx_power(int8_t *power) {
    ++tx_get_calls; if (tx_call_order_len < sizeof(tx_call_order)) tx_call_order[tx_call_order_len++] = 'G';
    if (tx_get_fail) { --tx_get_fail; return ESP_FAIL; }
    *power = tx_readback_override >= 0 ? tx_readback_override : tx_power;
    tx_readback_override = -1; return ESP_OK;
}
static esp_err_t esp_wifi_set_max_tx_power(int8_t power) {
    ++tx_set_calls; if (tx_call_order_len < sizeof(tx_call_order)) tx_call_order[tx_call_order_len++] = 'S';
    if (tx_set_fail) return ESP_FAIL; tx_power = power; return ESP_OK;
}
static esp_err_t esp_wifi_disconnect(void) { ++disconnect_calls; return disconnect_result; }
static esp_err_t esp_wifi_stop(void) { ++stop_calls; return stop_result; }
static esp_err_t esp_wifi_start(void) { ++start_calls; return start_result; }
static esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return ESP_OK; }
static esp_err_t esp_wifi_scan_stop(void) { ++scan_stop_calls; return ESP_OK; }
static esp_err_t esp_wifi_clear_ap_list(void) { ++clear_ap_list_calls; return ESP_OK; }
static esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool blocking) {
    assert(config && !blocking); ++scan_calls; return ESP_OK;
}
static esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count) {
    if (scan_num_fail) return ESP_FAIL; *count = scan_record_count; return ESP_OK;
}
static esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records) {
    if (scan_records_fail) return ESP_FAIL;
    if (*count > scan_record_count) *count = scan_record_count;
    memcpy(records, scan_records, *count * sizeof(*records)); return ESP_OK;
}
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) {
    snprintf((char *)record->ssid, sizeof(record->ssid), "%s", current_ap_ssid); return ESP_OK;
}
static esp_err_t wifi_store_save(const wifi_store_record_t *record) {
    assert(record->state == WIFI_STORE_CONFIGURED || record->state == WIFI_STORE_FORGOTTEN);
    if (store_fail) return ESP_FAIL; ++saved; return ESP_OK;
}
static bool box_config_pending_wifi(wifi_store_record_t *out) {
    if (!pending_inbox_available) return false;
    *out = pending_inbox;
    return true;
}
static esp_err_t box_config_finish_pending(void) {
    ++pending_finish_calls;
    pending_finish_saw_saved = saved;
    return pending_finish_result;
}
static int xEventGroupGetBits(EventGroupHandle_t group) { (void)group; return event_bits; }
static int xEventGroupClearBits(EventGroupHandle_t group, int bits) { (void)group; event_bits &= ~bits; return event_bits; }
static int xEventGroupSetBits(EventGroupHandle_t group, int bits) { (void)group; event_bits |= bits; return event_bits; }
static void display_wifi_setup(const char *qr, const char *url, bool ready, const char *message) {
    (void)qr; (void)url; (void)message; displayed_phone_ready = ready;
}
static void display_status(const char *a, const char *b) { (void)a; (void)b; }
static void display_wifi_setup_done(void) { ++display_done_calls; }
static void device_discovery_http_stop(void) { ++discovery_stop_calls; }
static void httpd_stop(httpd_handle_t server) { (void)server; ++http_stop_calls; }
static void refresh_capability(void) {}
static void refresh_setup_url(int64_t now) { (void)now; }
static esp_err_t start_server(void) { return ESP_OK; }
static esp_err_t start_setup(void) { s_provisioning = true; s_manager.ap_started = true; return ESP_OK; }
static void finish_setup(void);
static bool cap_matches(const char *uri, const char *suffix) { (void)uri; (void)suffix; return true; }
static void event_handler(void *, esp_event_base_t, int32_t, void *);
'''


CASES = r'''
static void run_worker(command_t *commands, int64_t *times, bool *receives, int count) {
    memcpy(script, commands, count * sizeof(*commands));
    memcpy(script_times, times, count * sizeof(*times));
    memcpy(script_receives, receives, count * sizeof(*receives));
    script_count = count; script_at = 0;
    if (!setjmp(stop_worker)) worker(NULL);
}
static command_t cmd(int kind, uint32_t attempt) {
    command_t c = {.kind = kind, .attempt = attempt};
    memcpy(c.capability, s_manager.cap, sizeof(c.capability));
    c.cap_deadline = s_manager.cap_deadline;
    c.credentials.state = WIFI_STORE_CONFIGURED;
    strcpy(c.credentials.ssid, "Home");
    strcpy(c.credentials.password, "!@#$%^&*()_+-=[]{}|;:',.<>/?~");
    return c;
}
static void reset_flow(void) {
    memset(&s_manager, 0, sizeof(s_manager)); s_manager.queue = 1; s_manager.events = 1;
    s_worker_started = false;
    atomic_store(&s_durable_configured, false);
    atomic_store(&s_connected, false);
    atomic_store(&s_management_queued, false);
    s_manager.ap_started = true; s_manager.server = &fake_server; s_manager.retry_seconds = 2;
    s_manager.queued_attempt = 1; s_manager.attempt = 1; s_manager.cap_deadline = 900000000;
    strcpy(s_manager.cap, "0123456789abcdef0123456789abcdef");
    s_manager.http_busy = s_manager.status_active = true;
    strcpy(s_manager.status_state, "queued"); s_provisioning = true;
    now_us = 1000000; saved = store_fail = connect_calls = disconnect_calls = scan_calls = 0;
    tx_power = 52; tx_readback_override = -1; tx_get_calls = tx_set_calls = tx_set_fail = tx_get_fail = 0;
    tx_call_order_len = 0; memset(tx_call_order, 0, sizeof(tx_call_order));
    pending_inbox_available = false; memset(&pending_inbox, 0, sizeof(pending_inbox));
    pending_finish_calls = pending_finish_saw_saved = 0; pending_finish_result = ESP_OK;
    scan_stop_calls = clear_ap_list_calls = 0;
    disconnect_result = ESP_OK; stop_calls = start_calls = 0;
    stop_result = start_result = ESP_OK;
    s_sta_stop_dropped = s_sta_start_dropped = false;
    s_sta_stop_count = s_sta_start_count = 0;
    reject_scan_done_queue = reject_radio_event_queue = 0;
    s_scan_done_pending = false; displayed_phone_ready = false;
    set_config_calls = http_stop_calls = discovery_stop_calls = display_done_calls = 0;
    event_bits = 0; script_count = script_at = captured_count = 0; script_receives[0] = false;
    warning_log[0] = 0;
    scan_num_fail = scan_records_fail = 0;
    snprintf(current_ap_ssid, sizeof(current_ap_ssid), "Home");
}
static void complete_tx_fallback_barrier(int64_t stop_at, int64_t start_at) {
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    command_t stop_event = captured[captured_count - 1]; bool yes[] = {true};
    run_worker(&stop_event, &stop_at, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    command_t start_event = captured[captured_count - 1];
    run_worker(&start_event, &start_at, yes, 1);
}
int main(void) {
    /* A failed USB replacement leaves the durable profile and live-state metadata truthful. */
    bool management_yes[] = {true};
    reset_flow();
    s_worker_started = true;
    s_provisioning = false;
    s_manager.ap_started = false;
    s_manager.selected.state = WIFI_STORE_CONFIGURED;
    strcpy(s_manager.selected.ssid, "Home");
    strcpy(s_manager.selected.password, "saved-original");
    s_manager.link_has_ip = true;
    s_manager.sta_associated = true;
    s_manager.management_operation_id = 41;
    strcpy(s_manager.status_state, "connected");
    atomic_store(&s_management_queued, true);
    atomic_store(&s_durable_configured, true);
    atomic_store(&s_connected, true);
    strcpy(current_ap_ssid, "Home");
    command_t management_connect[] = {cmd(CMD_MANAGEMENT_CONNECT, 41)};
    management_connect[0].credentials.state = WIFI_STORE_CONFIGURED;
    strcpy(management_connect[0].credentials.ssid, "NewHome");
    strcpy(management_connect[0].credentials.password, "new-candidate-pass");
    int64_t management_start_at[] = {1100000};
    run_worker(management_connect, management_start_at, management_yes, 1);
    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    assert(s_manager.management_wait_disconnect && disconnect_calls == 1 && saved == 0 &&
           !strcmp(s_manager.selected.ssid, "Home") && wifi_status.operation_id == 41 &&
           wifi_status.configured && wifi_status.connected && wifi_status.provisioning &&
           !strcmp(wifi_status.state, "queued") &&
           "management connect must keep old durable credentials while requesting a trial");
    command_t management_disconnect[] = {cmd(CMD_DISCONNECTED, 0)};
    int64_t management_disconnect_at[] = {1200000};
    run_worker(management_disconnect, management_disconnect_at, management_yes, 1);
    assert(s_manager.management_trial_active && s_manager.trial_active &&
           !strcmp(s_manager.trial.ssid, "NewHome") && !strcmp(s_manager.selected.ssid, "Home") &&
           saved == 0 && atomic_load(&s_durable_configured) && !atomic_load(&s_connected));
    wifi_status = (wifi_manager_status_t){0};
    wifi_manager_get_status(&wifi_status);
    assert(wifi_status.configured && !wifi_status.connected && wifi_status.provisioning &&
           !strcmp(wifi_status.state, "testing") &&
           "status must distinguish existing saved credentials from an unverified candidate");

    command_t management_failure[] = {cmd(CMD_DISCONNECTED, 0)};
    management_failure[0].reason = 201;
    int64_t management_failure_at[] = {1300000};
    run_worker(management_failure, management_failure_at, management_yes, 1);
    assert(!s_manager.management_trial_active && s_manager.management_restoring &&
           s_manager.saved_connecting && !strcmp(s_manager.selected.ssid, "Home") && saved == 0 &&
           atomic_load(&s_durable_configured) && !atomic_load(&s_connected) &&
           !strcmp((char *)last_config.sta.ssid, "Home") &&
           "failed replacement must reconnect the old profile without saving the candidate");
    wifi_manager_get_status(&wifi_status);
    assert(wifi_status.configured && !wifi_status.connected &&
           !strcmp(wifi_status.state, "failed") &&
           "a failed trial remains failed while old-profile reconnection is pending");

    command_t restored_events[] = {cmd(CMD_STA_CONNECTED, 0), cmd(CMD_GOT_IP, 0)};
    int64_t restored_at[] = {1400000, 1500000};
    bool restored_yes[] = {true, true};
    run_worker(restored_events, restored_at, restored_yes, 2);
    assert(!s_manager.management_restoring && s_manager.link_has_ip && saved == 0 &&
           !strcmp(s_manager.selected.ssid, "Home") && atomic_load(&s_durable_configured) &&
           atomic_load(&s_connected) &&
           "old profile is active again after failed USB candidate without replacing durable storage");
    wifi_manager_get_status(&wifi_status);
    assert(wifi_status.configured && wifi_status.connected &&
           "connected status must be derived from successful IP restoration, not configured flag");

    /* A boot-time STA may accept disconnect but never emit DISCONNECTED.
     * The USB operation must pass a fresh STOP/START barrier before a trial. */
    reset_flow();
    s_worker_started = true;
    s_provisioning = false;
    s_manager.ap_started = false;
    s_manager.saved_connecting = true;
    s_manager.selected.state = WIFI_STORE_CONFIGURED;
    strcpy(s_manager.selected.ssid, "Home");
    strcpy(s_manager.selected.password, "saved-original");
    s_manager.management_operation_id = 42;
    atomic_store(&s_management_queued, true);
    atomic_store(&s_durable_configured, true);
    command_t no_event_connect[] = {cmd(CMD_MANAGEMENT_CONNECT, 42)};
    strcpy(no_event_connect[0].credentials.ssid, "NewHome");
    strcpy(no_event_connect[0].credentials.password, "new-candidate-pass");
    int64_t no_event_start[] = {1100000};
    run_worker(no_event_connect, no_event_start, management_yes, 1);
    assert(s_manager.management_wait_disconnect &&
           s_manager.management_disconnect_deadline == 6100000 &&
           disconnect_calls == 1 && connect_calls == 0 && saved == 0);
    command_t no_event_idle[] = {cmd(CMD_GOT_IP, 0)};
    bool no_event_tick[] = {false};
    int64_t no_event_at[] = {6200000};
    run_worker(no_event_idle, no_event_at, no_event_tick, 1);
    assert(!s_manager.management_wait_disconnect &&
           s_manager.radio_recovery == RADIO_WAIT_STOP && stop_calls == 1 &&
           !s_manager.trial_active && connect_calls == 0 && saved == 0 &&
           atomic_load(&s_management_queued) &&
           !strcmp(s_manager.selected.ssid, "Home"));
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    command_t management_fresh_stop[] = {captured[captured_count - 1]};
    int64_t management_fresh_stop_at[] = {6300000};
    run_worker(management_fresh_stop, management_fresh_stop_at, management_yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_START && start_calls == 1 &&
           !s_manager.trial_active && saved == 0);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    command_t management_fresh_start[] = {captured[captured_count - 1]};
    int64_t management_fresh_start_at[] = {6400000};
    run_worker(management_fresh_start, management_fresh_start_at, management_yes, 1);
    assert(s_manager.radio_recovery == RADIO_READY && s_manager.trial_active &&
           s_manager.management_trial_active && connect_calls == 1 && saved == 0 &&
           !strcmp(s_manager.trial.ssid, "NewHome") &&
           !strcmp(s_manager.selected.ssid, "Home") &&
           s_manager.management_credentials.ssid[0] == 0);

    /* If the radio never confirms STOP, end the operation and wipe the
     * queued candidate instead of waiting forever or overlapping radios. */
    reset_flow();
    s_worker_started = true; s_provisioning = false; s_manager.ap_started = false;
    s_manager.saved_connecting = true;
    s_manager.selected.state = WIFI_STORE_CONFIGURED;
    strcpy(s_manager.selected.ssid, "Home");
    strcpy(s_manager.selected.password, "saved-original");
    s_manager.management_operation_id = 43;
    atomic_store(&s_management_queued, true);
    atomic_store(&s_durable_configured, true);
    command_t no_stop_connect[] = {cmd(CMD_MANAGEMENT_CONNECT, 43)};
    strcpy(no_stop_connect[0].credentials.ssid, "NewHome");
    strcpy(no_stop_connect[0].credentials.password, "new-candidate-pass");
    run_worker(no_stop_connect, no_event_start, management_yes, 1);
    run_worker(no_event_idle, no_event_at, no_event_tick, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP &&
           s_manager.management_credentials.ssid[0]);
    int64_t no_stop_timeout_at[] = {11300000};
    run_worker(no_event_idle, no_stop_timeout_at, no_event_tick, 1);
    wifi_manager_get_status(&wifi_status);
    assert(s_manager.radio_recovery == RADIO_FAILED &&
           s_manager.management_credentials.ssid[0] == 0 &&
           s_manager.management_credentials.password[0] == 0 &&
           !atomic_load(&s_management_queued) &&
           wifi_status.operation_id == 43 && !strcmp(wifi_status.state, "failed") &&
           saved == 0 && !strcmp(s_manager.selected.ssid, "Home") &&
           connect_calls == 0);

    /* Installer inbox credentials are trialed but are not cleared before durable Wi-Fi save. */
    reset_flow();
    pending_inbox_available = true;
    pending_inbox.state = WIFI_STORE_CONFIGURED;
    strcpy(pending_inbox.ssid, "InstallerHome");
    strcpy(pending_inbox.password, "installer-dummy-pass");
    snprintf(current_ap_ssid, sizeof(current_ap_ssid), "InstallerHome");
    command_t startup_tick[] = {cmd(CMD_GOT_IP, 0)};
    int64_t startup_at[] = {1100000}; bool startup_no[] = {false};
    run_worker(startup_tick, startup_at, startup_no, 1);
    assert(s_manager.installer_candidate_active && s_manager.trial_active &&
           connect_calls == 1 && saved == 0 && pending_finish_calls == 0 &&
           "validated installer Wi-Fi starts as a candidate, not an already-persisted network");
    assert(!strcmp((char *)last_config.sta.ssid, "InstallerHome"));
    command_t installer_connected[] = {cmd(CMD_STA_CONNECTED, 0), cmd(CMD_GOT_IP, 0)};
    int64_t installer_connected_at[] = {1200000, 1300000}; bool installer_yes[] = {true, true};
    run_worker(installer_connected, installer_connected_at, installer_yes, 2);
    assert(saved == 1 && pending_finish_calls == 1 && pending_finish_saw_saved == 1 &&
           !s_manager.installer_candidate_active && s_manager.setup_committed &&
           "installer inbox clears only after the verified router credentials are saved");

    /* A failed candidate is cleared without writing it as a saved Wi-Fi profile. */
    reset_flow(); pending_inbox_available = true;
    pending_inbox.state = WIFI_STORE_CONFIGURED;
    strcpy(pending_inbox.ssid, "InstallerHome");
    strcpy(pending_inbox.password, "installer-dummy-pass");
    snprintf(current_ap_ssid, sizeof(current_ap_ssid), "InstallerHome");
    run_worker(startup_tick, startup_at, startup_no, 1);
    command_t installer_failed[] = {cmd(CMD_DISCONNECTED, 0)};
    int64_t installer_failed_at[] = {1200000};
    run_worker(installer_failed, installer_failed_at, installer_yes, 1);
    assert(saved == 0 && pending_finish_calls == 1 && pending_finish_saw_saved == 0 &&
           !s_manager.installer_candidate_active && !s_manager.trial_active &&
           "failed installer candidate is discarded without becoming the saved network");

    reset_flow();
    command_t submit[] = {cmd(CMD_SUBMIT, 999)}; int64_t submit_time[] = {1100000}; bool yes[] = {true};
    run_worker(submit, submit_time, yes, 1);
    assert(!s_manager.trial_active && connect_calls == 0 && saved == 0);
    assert(!strcmp(s_manager.status_state, "queued") && "stale POST attempt cannot start radio work");

    reset_flow(); command_t expired_submit[] = {cmd(CMD_SUBMIT, 1)};
    int64_t expired_at[] = {900000000};
    run_worker(expired_submit, expired_at, yes, 1);
    assert(!s_manager.trial_active && connect_calls == 0 && saved == 0);
    assert(!s_manager.queued_attempt && !strcmp(s_manager.status_state, "failed"));

    reset_flow(); command_t rotated_submit[] = {cmd(CMD_SUBMIT, 1)};
    strcpy(s_manager.cap, "fedcba9876543210fedcba9876543210");
    int64_t rotated_at[] = {1100000};
    run_worker(rotated_submit, rotated_at, yes, 1);
    assert(!s_manager.trial_active && connect_calls == 0 && saved == 0);
    assert(!strcmp(s_manager.status_state, "failed"));

    reset_flow(); command_t stale_scan[] = {cmd(CMD_SCAN, 0)};
    s_manager.scan_state[0] = 's'; s_manager.scan_state[1] = 0;
    s_manager.network_count = 1; strcpy(s_manager.networks[0], "Old result");
    strcpy(s_manager.cap, "fedcba9876543210fedcba9876543210");
    run_worker(stale_scan, rotated_at, yes, 1);
    assert(scan_calls == 0);

    reset_flow(); command_t start[] = {cmd(CMD_SUBMIT, 1)}; int64_t t1[] = {1100000};
    run_worker(start, t1, yes, 1);
    assert(s_manager.trial_active && connect_calls == 1 && !strcmp(s_manager.status_state, "testing"));
    assert(!strcmp((char *)last_config.sta.ssid, "Home"));
    static const char fake_key[] = "!@#$%^&*()_+-=[]{}|;:',.<>/?~";
    assert(!memcmp(last_config.sta.password, fake_key, sizeof(fake_key)) &&
           "decoded ASCII punctuation reaches the station config byte-for-byte");
    assert(scan_stop_calls == 1 && clear_ap_list_calls == 1);

    command_t scan_during_trial[] = {cmd(CMD_SCAN, 0), cmd(CMD_SCAN_DONE, 0)};
    s_manager.network_count = 1; strcpy(s_manager.networks[0], "Stale cache");
    int64_t trial_scan_times[] = {1150000, 1160000}; bool trial_scan_received[] = {true, true};
    run_worker(scan_during_trial, trial_scan_times, trial_scan_received, 2);
    assert(scan_calls == 0 && !strcmp(s_manager.scan_state, "busy") && s_manager.network_count == 0);

    command_t early_ip[] = {cmd(CMD_GOT_IP, 0)}; int64_t t2[] = {1200000};
    run_worker(early_ip, t2, yes, 1);
    assert(s_manager.trial_active && saved == 0 && strcmp(s_manager.status_state, "connected"));
    command_t wrong_ap[] = {cmd(CMD_STA_CONNECTED, 0), cmd(CMD_GOT_IP, 0)};
    int64_t t3[] = {1300000, 1400000}; bool two[] = {true, true};
    snprintf(current_ap_ssid, sizeof(current_ap_ssid), "Other");
    run_worker(wrong_ap, t3, two, 2);
    assert(s_manager.trial_active && saved == 0 && strcmp(s_manager.status_state, "connected"));

    snprintf(current_ap_ssid, sizeof(current_ap_ssid), "Home");
    command_t right_ap[] = {cmd(CMD_STA_CONNECTED, 0), cmd(CMD_GOT_IP, 0)};
    int64_t t4[] = {1500000, 1600000};
    run_worker(right_ap, t4, two, 2);
    assert(saved == 1 && s_manager.setup_committed && !s_manager.trial_active);
    assert(!strcmp(s_manager.status_state, "connected") && s_manager.ap_started && s_manager.server);
    assert(http_stop_calls == 0 && discovery_stop_calls == 0 && display_done_calls == 0);
    command_t tick[] = {cmd(CMD_GOT_IP, 0)}; int64_t t5[] = {16500000}; bool no[] = {false};
    run_worker(tick, t5, no, 1);
    assert(s_manager.ap_started && s_manager.server && http_stop_calls == 0);
    int64_t t6[] = {16600000}; run_worker(tick, t6, no, 1);
    assert(!s_manager.ap_started && !s_manager.server && http_stop_calls == 1);
    assert(discovery_stop_calls == 1 && display_done_calls == 1);

    reset_flow(); command_t begin[] = {cmd(CMD_SUBMIT, 1)}; int64_t b1[] = {1100000};
    run_worker(begin, b1, yes, 1);
    store_fail = 1;
    command_t associate_ip[] = {cmd(CMD_STA_CONNECTED, 0), cmd(CMD_GOT_IP, 0)};
    int64_t b2[] = {1200000, 1300000}; run_worker(associate_ip, b2, two, 2);
    assert(s_manager.pending_save && !s_manager.setup_committed && saved == 0);
    assert(!strcmp(s_manager.status_state, "saving") && s_manager.ap_started && s_manager.server);
    store_fail = 0;
    command_t save_retry[] = {cmd(CMD_DISCONNECTED, 0)}; int64_t b3[] = {6300000};
    run_worker(save_retry, b3, yes, 1);
    assert(!strcmp(s_manager.status_state, "failed") && !s_manager.pending_save && !s_manager.setup_committed);
    assert(s_manager.setup_message && strstr(s_manager.setup_message, "Connection lost"));
    assert(!strstr(s_manager.setup_message, "Retrying"));

    reset_flow(); run_worker(begin, b1, yes, 1); store_fail = 1;
    run_worker(associate_ip, b2, two, 2);
    assert(s_manager.pending_save && saved == 0);
    store_fail = 0;
    int64_t b4[] = {6300000};
    run_worker(save_retry, b4, no, 1);
    assert(saved == 1 && !s_manager.pending_save && s_manager.setup_committed);
    assert(!strcmp(s_manager.status_state, "connected") && s_manager.ap_started && s_manager.server);

    reset_flow(); run_worker(begin, b1, yes, 1);
    command_t lose[] = {cmd(CMD_DISCONNECTED, 0)}; int64_t l1[] = {1200000};
    run_worker(lose, l1, yes, 1);
    assert(!s_manager.trial_active && !strcmp(s_manager.status_state, "failed") && saved == 0);
    assert(!s_manager.http_busy && !s_manager.scan_state[0]);

    /* Exercise disconnect diagnostics through the production callback and worker. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    wifi_event_sta_disconnected_t handshake = {.reason = 204, .rssi = -67};
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &handshake);
    assert(captured_count == 1 && captured[0].reason == 204 &&
           captured[0].rssi_valid && captured[0].rssi == -67);
    int64_t diagnostic_time[] = {1800000};
    run_worker(captured, diagnostic_time, yes, 1);
    assert(strstr(warning_log, "reason=204") && strstr(warning_log, "rssi_valid=1") &&
           strstr(warning_log, "rssi=-67") && strstr(warning_log, "trial_elapsed_ms=700"));
    assert(!strstr(warning_log, "Home") && !strstr(warning_log, "correct-pass") &&
           !strstr(warning_log, "0123456789abcdef0123456789abcdef"));
    assert(!s_manager.trial_active && !strcmp(s_manager.status_state, "failed") && saved == 0);

    /* Zero and the SDK's -128 sentinel are invalid and omitted from log values. */
    const int8_t invalid_rssis[] = {0, -128};
    for (size_t i = 0; i < sizeof(invalid_rssis) / sizeof(invalid_rssis[0]); ++i) {
        reset_flow(); run_worker(begin, b1, yes, 1);
        wifi_event_sta_disconnected_t unusable = {.reason = 204, .rssi = invalid_rssis[i]};
        event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &unusable);
        assert(captured_count == 1 && captured[0].reason == 204 && !captured[0].rssi_valid);
        run_worker(captured, diagnostic_time, yes, 1);
        assert(strstr(warning_log, "reason=204") && strstr(warning_log, "rssi_valid=0") &&
               strstr(warning_log, "trial_elapsed_ms=700") && !strstr(warning_log, "rssi="));
        assert(!strstr(warning_log, "Home") && !strstr(warning_log, "correct-pass") &&
               !strstr(warning_log, "0123456789abcdef0123456789abcdef"));
        assert(!s_manager.trial_active && !strcmp(s_manager.status_state, "failed") && saved == 0);
    }

    reset_flow(); run_worker(begin, b1, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(captured_count == 1 && captured[0].reason == 0 && !captured[0].rssi_valid);
    run_worker(captured, diagnostic_time, yes, 1);
    assert(strstr(warning_log, "reason=0") && strstr(warning_log, "rssi_valid=0") &&
           strstr(warning_log, "trial_elapsed_ms=700") && !strstr(warning_log, "rssi="));
    assert(!strstr(warning_log, "Home") && !strstr(warning_log, "correct-pass") &&
           !strstr(warning_log, "0123456789abcdef0123456789abcdef"));
    assert(!s_manager.trial_active && !strcmp(s_manager.status_state, "failed") && saved == 0);
    assert(s_manager.setup_message && strstr(s_manager.setup_message, "Wi-Fi failed"));

    /* Capture a real Wi-Fi event reason through the callback without SDK calls,
       then verify both clients receive the mapped category and can retry. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    uint8_t retry_phone_mac[6] = {1,2,3,4,5,6};
    wifi_event_ap_staconnected_t retry_joined = {{1,2,3,4,5,6}};
    ip_event_ap_staipassigned_t retry_ready = {{1,2,3,4,5,6}};
    wifi_event_sta_disconnected_t no_ap = {.reason = 201};
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &retry_joined);
    event_handler(NULL, IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &retry_ready);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &no_ap);
    assert(captured_count == 3 && captured[2].kind == CMD_DISCONNECTED && captured[2].reason == 201);
    command_t captured_events[3]; memcpy(captured_events, captured, sizeof(captured_events));
    int64_t mapped_times[] = {1200000, 1300000, 1400000};
    bool three[] = {true, true, true};
    run_worker(captured_events, mapped_times, three, 3);
    assert(!strcmp(s_manager.status_state, "failed"));
    assert(strstr(s_manager.status_message, "Network not found"));
    assert(!strcmp(s_manager.setup_message, "Network not found; refresh list"));
    assert(s_manager.ap_started && s_manager.server && s_manager.phone_joined);
    assert(s_manager.ready_count == 1 && !memcmp(retry_phone_mac, s_manager.ready_macs[0], 6));
    s_manager.queued_attempt = 2; s_manager.attempt = 2;
    command_t retry_submit[] = {cmd(CMD_SUBMIT, 2)}; int64_t retry_time[] = {1500000};
    run_worker(retry_submit, retry_time, yes, 1);
    assert(s_manager.trial_active && connect_calls == 2 && s_manager.ap_started && s_manager.server);

    /* A missing SDK payload maps to the safe generic reason rather than dereferencing NULL. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(captured_count == 1 && captured[0].kind == CMD_DISCONNECTED && captured[0].reason == 0);
    command_t missing_payload = captured[0]; int64_t missing_time[] = {1200000};
    run_worker(&missing_payload, missing_time, yes, 1);
    assert(!strcmp(s_manager.status_message, "Wi-Fi connection failed. Retry or check router settings."));
    assert(!strcmp(s_manager.setup_message, "Wi-Fi failed; check router"));

    /* The callback only copies reasons into commands; it does not call radio APIs. */
    reset_flow();
    wifi_event_sta_disconnected_t auth_fail = {.reason = 202};
    wifi_event_sta_disconnected_t no_ap_reason = {.reason = 201};
    wifi_event_sta_disconnected_t beacon_timeout = {.reason = 200};
    wifi_event_sta_disconnected_t assoc_fail = {.reason = 203};
    wifi_event_sta_disconnected_t handshake_timeout = {.reason = 204};
    wifi_event_sta_disconnected_t unknown_reason = {.reason = 255};
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &auth_fail);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &no_ap_reason);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &beacon_timeout);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &assoc_fail);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &handshake_timeout);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &unknown_reason);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    const uint16_t callback_reasons[] = {202, 201, 200, 203, 204, 255, 0};
    assert(captured_count == 7 && connect_calls == 0 && disconnect_calls == 0 && set_config_calls == 0);
    for (int i = 0; i < 7; ++i) assert(captured[i].reason == callback_reasons[i]);

    reset_flow(); run_worker(begin, b1, yes, 1);
    command_t timeout[] = {cmd(CMD_GOT_IP, 0)}; int64_t limit[] = {26100000};
    run_worker(timeout, limit, no, 1);
    assert(!s_manager.trial_active && disconnect_calls == 0 && stop_calls == 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && !s_manager.trial.ssid[0]);
    assert(!strcmp(s_manager.status_state, "failed") && s_manager.ap_started && s_manager.server == &fake_server);

    /* Old disconnects and lifecycle callbacks cannot release recovery; retry waits for new STOP and START. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    uint8_t ready_mac[6] = {1,2,3,4,5,6};
    wifi_event_ap_staconnected_t restart_joined = {{1,2,3,4,5,6}};
    ip_event_ap_staipassigned_t restart_ready = {{1,2,3,4,5,6}};
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &restart_joined);
    event_handler(NULL, IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &restart_ready);
    command_t initial_phone_events[2]; memcpy(initial_phone_events, captured, sizeof(initial_phone_events));
    int64_t initial_phone_times[] = {1150000, 1160000};
    bool two_events[] = {true, true};
    run_worker(initial_phone_events, initial_phone_times, two_events, 2);
    assert(s_manager.phone_joined && s_manager.ready_count == 1);
    wifi_event_sta_disconnected_t stale_disconnect = {.reason = 8};
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &stale_disconnect);
    command_t old_disconnect = captured[captured_count - 1];
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    command_t stale_stop = captured[captured_count - 1];
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    command_t stale_start = captured[captured_count - 1];
    run_worker(timeout, limit, no, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && !s_manager.phone_joined &&
           s_manager.phone_count == 0 && s_manager.ready_count == 0 && s_manager.ap_started &&
           s_manager.server == &fake_server && !strcmp(s_manager.cap, "0123456789abcdef0123456789abcdef"));
    s_manager.queued_attempt = 2; s_manager.attempt = 2;
    command_t queued_retry[] = {cmd(CMD_SUBMIT, 2)}; int64_t retry_queued_at[] = {26200000};
    run_worker(queued_retry, retry_queued_at, yes, 1);
    assert(s_manager.deferred_submit.attempt == 2 && connect_calls == 1);
    int64_t stale_event_time[] = {26300000};
    run_worker(&old_disconnect, stale_event_time, yes, 1);
    run_worker(&stale_stop, stale_event_time, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && start_calls == 0 && connect_calls == 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    int64_t stop_time[] = {26400000};
    run_worker(&captured[captured_count - 1], stop_time, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_START && start_calls == 1 && connect_calls == 1);
    run_worker(&stale_start, stale_event_time, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_START && connect_calls == 1);
    wifi_event_sta_disconnected_t duplicate_old_disconnect = {.reason = 201};
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &duplicate_old_disconnect);
    int64_t duplicate_disconnect_time[] = {26450000};
    run_worker(&captured[captured_count - 1], duplicate_disconnect_time, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_START && !s_manager.trial_active && connect_calls == 1);
    /* AP_STOP clears readiness, then the AP accepts a rejoin before STA_START completes. */
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STOP, NULL);
    int64_t ap_stop_time[] = {26500000};
    run_worker(&captured[captured_count - 1], ap_stop_time, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &restart_joined);
    event_handler(NULL, IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &restart_ready);
    command_t fast_phone_events[2]; memcpy(fast_phone_events, &captured[captured_count - 2], sizeof(fast_phone_events));
    int64_t fast_phone_times[] = {26600000, 26700000};
    run_worker(fast_phone_events, fast_phone_times, two_events, 2);
    assert(s_manager.phone_joined && s_manager.ready_count == 1 &&
           !memcmp(ready_mac, s_manager.ready_macs[0], 6));
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    int64_t start_time[] = {26800000};
    run_worker(&captured[captured_count - 1], start_time, yes, 1);
    assert(s_manager.radio_recovery == RADIO_READY && s_manager.trial_active && connect_calls == 2);
    assert(s_manager.phone_joined && s_manager.ready_count == 1);
    wifi_event_sta_disconnected_t retry_failed = {.reason = 202};
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &retry_failed);
    int64_t retry_failed_at[] = {26900000};
    run_worker(&captured[captured_count - 1], retry_failed_at, yes, 1);
    assert(!s_manager.trial_active && !strcmp(s_manager.status_state, "failed"));

    /* Dropped STOP/START queue events are recovered from their monotonic callback counters. */
    reset_flow(); run_worker(begin, b1, yes, 1); run_worker(timeout, limit, no, 1);
    reject_radio_event_queue = 1;
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    reject_radio_event_queue = 0;
    command_t radio_tick[] = {cmd(CMD_GOT_IP, 0)}; int64_t recovery_tick_time[] = {26200000};
    run_worker(radio_tick, recovery_tick_time, no, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_START && start_calls == 1);
    reject_radio_event_queue = 1;
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    reject_radio_event_queue = 0;
    int64_t recovery_ready_time[] = {26300000};
    run_worker(radio_tick, recovery_ready_time, no, 1);
    assert(s_manager.radio_recovery == RADIO_READY && start_calls == 1);

    /* Missing STOP is bounded: deferred credentials are wiped and retries report restart required. */
    reset_flow(); run_worker(begin, b1, yes, 1); run_worker(timeout, limit, no, 1);
    s_manager.queued_attempt = 2; s_manager.attempt = 2;
    run_worker(queued_retry, retry_queued_at, yes, 1);
    command_t recovery_watchdog_tick[] = {cmd(CMD_GOT_IP, 0)}; int64_t recovery_expired[] = {31100000};
    run_worker(recovery_watchdog_tick, recovery_expired, no, 1);
    assert(s_manager.radio_recovery == RADIO_FAILED && !s_manager.deferred_submit.attempt &&
           !s_manager.queued_attempt && !s_manager.deferred_deadline);
    assert(!strcmp(s_manager.status_state, "failed") && strstr(s_manager.status_message, "Restart the device"));
    assert(start_calls == 1 && !s_manager.trial.password[0]);

    /* Missing START after a valid STOP also fails closed and wipes the deferred trial. */
    reset_flow(); run_worker(begin, b1, yes, 1); run_worker(timeout, limit, no, 1);
    s_manager.queued_attempt = 2; s_manager.attempt = 2;
    run_worker(queued_retry, retry_queued_at, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    int64_t phase_stop_at[] = {21250000};
    run_worker(&captured[captured_count - 1], phase_stop_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_START && s_manager.deferred_submit.attempt == 2);
    int64_t start_expired[] = {31250000};
    run_worker(recovery_watchdog_tick, start_expired, no, 1);
    assert(s_manager.radio_recovery == RADIO_FAILED && !s_manager.deferred_submit.attempt &&
           !s_manager.queued_attempt && start_calls == 2);
    assert(strstr(s_manager.status_message, "Restart the device"));

    /* stop() errors enter the same bounded failed state and clear queued credentials. */
    reset_flow(); run_worker(begin, b1, yes, 1); stop_result = ESP_FAIL;
    run_worker(timeout, limit, no, 1);
    assert(s_manager.radio_recovery == RADIO_FAILED && stop_calls == 1 && start_calls == 1);

    reset_flow(); run_worker(begin, b1, yes, 1);
    command_t success[] = {cmd(CMD_STA_CONNECTED, 0), cmd(CMD_GOT_IP, 0)};
    int64_t success_times[] = {1200000, 1300000}; run_worker(success, success_times, two, 2);
    assert(s_manager.setup_committed && !strcmp(s_manager.status_state, "connected"));
    command_t grace_link_loss[] = {cmd(CMD_DISCONNECTED, 0)}; int64_t gl[] = {1400000};
    run_worker(grace_link_loss, gl, yes, 1);
    assert(!s_manager.setup_committed && !strcmp(s_manager.status_state, "failed"));
    assert(s_manager.ap_started && s_manager.server && http_stop_calls == 0);

    reset_flow(); s_manager.cap_deadline = 1200000;
    char near_cap[33]; memcpy(near_cap, s_manager.cap, sizeof(near_cap));
    command_t near_submit[] = {cmd(CMD_SUBMIT, 1)}; int64_t near_start[] = {1100000};
    run_worker(near_submit, near_start, yes, 1);
    command_t expired_tick[] = {cmd(CMD_GOT_IP, 0)}; int64_t near_expiry[] = {1250000};
    run_worker(expired_tick, near_expiry, no, 1);
    assert(s_manager.trial_active && !strcmp(s_manager.status_state, "testing"));
    assert(!memcmp(s_manager.cap, near_cap, sizeof(near_cap)) && s_manager.cap_deadline == 1200000);
    command_t near_success[] = {cmd(CMD_STA_CONNECTED, 0), cmd(CMD_GOT_IP, 0)};
    int64_t near_success_times[] = {1290000, 1300000};
    run_worker(near_success, near_success_times, two, 2);
    assert(s_manager.setup_committed && !strcmp(s_manager.status_state, "connected"));
    assert(!memcmp(s_manager.cap, near_cap, sizeof(near_cap)) && s_manager.cap_deadline == 1200000);
    int64_t grace_end[] = {16300000}; run_worker(expired_tick, grace_end, no, 1);
    assert(!s_manager.ap_started && s_manager.cap[0] == 0);

    reset_flow();
    uint8_t phone[6] = {1,2,3,4,5,6}, other[6] = {6,5,4,3,2,1};
    wifi_event_ap_staconnected_t joined = {{1,2,3,4,5,6}};
    wifi_event_ap_stadisconnected_t left_other = {{6,5,4,3,2,1}};
    ip_event_ap_staipassigned_t ready = {{1,2,3,4,5,6}};
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &joined);
    event_handler(NULL, IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &ready);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &left_other);
    assert(captured_count == 3);
    command_t events[3]; memcpy(events, captured, sizeof(events)); int64_t event_times[] = {1100000,1200000,1300000};
    run_worker(events, event_times, two, 2);
    /* Apply the third event in its own bounded worker run. */
    run_worker(&events[2], &event_times[2], yes, 1);
    assert(!memcmp(phone, s_manager.ready_macs[0], 6) && s_manager.phone_joined);
    assert(memcmp(other, phone, 6) != 0);

    set_status("testing", "Testing router connection"); show_setup("Testing router connection...");
    command_t leave_phone = cmd(CMD_PHONE_LEFT, 0); memcpy(leave_phone.mac, phone, 6);
    int64_t phone_leave_time[] = {1400000};
    run_worker(&leave_phone, phone_leave_time, yes, 1);
    assert(!s_manager.phone_joined && s_manager.ready_count == 0 && !displayed_phone_ready);
    assert(!strcmp(s_manager.setup_message, "Testing router connection..."));

    set_status("saving", "Connected, but saving failed. Retrying");
    show_setup("Connected, but saving failed. Retrying...");
    command_t rejoin[] = {cmd(CMD_PHONE_JOINED, 0), cmd(CMD_PHONE_IP, 0)};
    memcpy(rejoin[0].mac, phone, 6); memcpy(rejoin[1].mac, phone, 6);
    int64_t rejoin_time[] = {1500000, 1600000};
    run_worker(rejoin, rejoin_time, two, 2);
    assert(s_manager.phone_joined && s_manager.ready_count == 1 && displayed_phone_ready);
    assert(!strcmp(s_manager.setup_message, "Connected, but saving failed. Retrying..."));

    set_status("connected", "Connected to Wi-Fi. Setup complete.");
    show_setup("Wi-Fi connected. Setup complete.");
    run_worker(&leave_phone, phone_leave_time, yes, 1);
    assert(!s_manager.phone_joined && !displayed_phone_ready);
    assert(!strcmp(s_manager.setup_message, "Wi-Fi connected. Setup complete."));

    set_status("failed", "Connection failed. Check the details and retry.");
    show_setup("Connection failed. Check the details and retry.");
    run_worker(rejoin, rejoin_time, two, 2);
    assert(s_manager.phone_joined && displayed_phone_ready);
    assert(!strcmp(s_manager.setup_message, "Connection failed. Check the details and retry."));

    reset_flow();
    memset(scan_records, 0, sizeof(scan_records)); scan_record_count = 4;
    strcpy(s_manager.ap_ssid, "Device");
    strcpy((char *)scan_records[0].ssid, "Device"); scan_records[0].primary = 1;
    strcpy((char *)scan_records[1].ssid, "Visible"); scan_records[1].primary = 6;
    strcpy((char *)scan_records[2].ssid, "Visible"); scan_records[2].primary = 11;
    scan_records[3].primary = 3;
    scan_done();
    assert(!strcmp(s_manager.scan_state, "ready") && s_manager.network_count == 1);
    assert(!strcmp(s_manager.networks[0], "Visible"));
    scan_record_count = 0; scan_done();
    assert(!strcmp(s_manager.scan_state, "ready") && s_manager.network_count == 0);
    s_manager.network_count = 1; strcpy(s_manager.networks[0], "Stale records");
    scan_record_count = 1; scan_records_fail = 1; strcpy((char *)scan_records[0].ssid, "Stale");
    scan_done();
    assert(!strcmp(s_manager.scan_state, "error") && s_manager.network_count == 0);
    scan_records_fail = 0; scan_num_fail = 1;
    s_manager.network_count = 1; strcpy(s_manager.networks[0], "Stale count"); scan_done();
    assert(!strcmp(s_manager.scan_state, "error") && s_manager.network_count == 0);

    reset_flow();
    s_manager.scan_state[0] = 0;
    s_manager.scan_started_at = now_us;
    scan_record_count = 1; strcpy((char *)scan_records[0].ssid, "Recovered"); scan_records[0].primary = 1;
    reject_scan_done_queue = 1;
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    reject_scan_done_queue = 0;
    assert(s_scan_done_pending && scan_calls == 0 && captured_count == 0);
    s_manager.scan_state[0] = 's'; strcpy(s_manager.scan_state + 1, "canning");
    command_t scan_tick = cmd(CMD_GOT_IP, 0); int64_t completion_time[] = {1100000};
    run_worker(&scan_tick, completion_time, no, 1);
    assert(!s_scan_done_pending && scan_calls == 0 && !strcmp(s_manager.scan_state, "ready"));
    assert(s_manager.network_count == 1 && !strcmp(s_manager.networks[0], "Recovered"));

    reset_flow();
    strcpy(s_manager.scan_state, "scanning"); s_manager.scan_started_at = now_us;
    command_t watchdog_tick = cmd(CMD_GOT_IP, 0); int64_t watchdog_time[] = {11000000};
    run_worker(&watchdog_tick, watchdog_time, no, 1);
    assert(!s_manager.scan_started_at && !strcmp(s_manager.scan_state, "error"));
    assert(scan_stop_calls == 1 && clear_ap_list_calls == 1);
    s_manager.scan_state[0] = 's'; strcpy(s_manager.scan_state + 1, "canning");
    command_t retry_scan[] = {cmd(CMD_SCAN, 0)}; int64_t retry_scan_time[] = {11100000};
    run_worker(retry_scan, retry_scan_time, yes, 1);
    assert(scan_calls == 1 && !strcmp(s_manager.scan_state, "scanning"));
    /* One qualified strong-signal failure retries the same portal attempt at 28 quarter dBm. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    assert(s_manager.tx_original_known && s_manager.tx_original_limit == 52 && tx_power == 52);
    wifi_event_sta_disconnected_t qualified = {.reason = 204, .rssi = -20};
    /* Callback events already queued before the fallback barrier must not satisfy it. */
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    command_t tx_stale_stop = captured[captured_count - 1];
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &qualified);
    command_t qualified_disconnect = captured[captured_count - 1];
    int64_t qualified_at[] = {1200000};
    run_worker(&qualified_disconnect, qualified_at, yes, 1);
    assert(s_manager.trial_active && !s_manager.tx_low && s_manager.tx_fallback_attempted &&
           !s_manager.tx_retry_pending && tx_power == 52 && connect_calls == 1 &&
           s_manager.radio_recovery == RADIO_WAIT_STOP && stop_calls == 1 && start_calls == 0 &&
           s_manager.radio_restart_purpose == RADIO_RESTART_TX_FALLBACK);
    assert(s_manager.attempt == 1 && !strcmp(s_manager.status_state, "testing") &&
           !strcmp(s_manager.cap, "0123456789abcdef0123456789abcdef"));
    assert(tx_get_calls == 1 && tx_set_calls == 0 && s_manager.tx_original_limit == 52 &&
           "original power is captured once; fallback setter waits for STA_START");
    run_worker(&tx_stale_stop, qualified_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && start_calls == 0 && tx_set_calls == 0);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    command_t tx_stale_start = captured[captured_count - 1];
    run_worker(&tx_stale_start, qualified_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && start_calls == 0 && tx_set_calls == 0);
    command_t stale_barrier_events[] = {qualified_disconnect, cmd(CMD_GOT_IP, 0)};
    int64_t stale_barrier_times[] = {1230000, 1240000}; bool stale_barrier_yes[] = {true, true};
    run_worker(stale_barrier_events, stale_barrier_times, stale_barrier_yes, 2);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && s_manager.trial_active &&
           !s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0 && connect_calls == 1 &&
           !strcmp(s_manager.status_state, "testing"));
    /* A fresh STOP starts Wi-Fi; TX stays normal until the matching fresh START. */
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    command_t fresh_stop = captured[captured_count - 1];
    int64_t fresh_stop_at[] = {1250000}; run_worker(&fresh_stop, fresh_stop_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_START && start_calls == 1 &&
           tx_power == 52 && tx_set_calls == 0 && !s_manager.tx_retry_pending);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    command_t fresh_start = captured[captured_count - 1];
    int64_t fresh_start_at[] = {1260000}; run_worker(&fresh_start, fresh_start_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_READY && s_manager.tx_low &&
           s_manager.tx_retry_pending && tx_power == 28 && connect_calls == 1);
    assert(tx_get_calls == 2 && tx_set_calls == 1 && tx_call_order_len >= 3 &&
           !strcmp(tx_call_order + tx_call_order_len - 2, "SG"));
    command_t duplicate_disconnect = qualified_disconnect;
    int64_t duplicate_at[] = {1270000};
    run_worker(&duplicate_disconnect, duplicate_at, yes, 1);
    assert(s_manager.tx_retry_pending && s_manager.trial_active && connect_calls == 1);
    command_t retry_tick[] = {cmd(CMD_GOT_IP, 0)}; int64_t retry_at[] = {1360000};
    run_worker(retry_tick, retry_at, no, 1);
    assert(!s_manager.tx_retry_pending && s_manager.trial_active && connect_calls == 2 &&
           s_manager.trial_deadline == 21360000 && tx_power == 28);
    command_t low_success[] = {cmd(CMD_STA_CONNECTED, 0), cmd(CMD_GOT_IP, 0)};
    int64_t low_success_times[] = {1400000, 1500000};
    run_worker(low_success, low_success_times, two, 2);
    assert(s_manager.setup_committed && saved == 1 && tx_power == 28 && s_manager.tx_low);
    command_t forget_low[] = {cmd(CMD_FORGET, 0)}; int64_t forget_time[] = {1600000};
    run_worker(forget_low, forget_time, yes, 1);
    assert(tx_power == 52 && !s_manager.tx_low && !s_manager.tx_fault && s_manager.tx_original_known);

    /* Other reasons, weak RSSI, and invalid RSSI do not get a reduced-power retry. */
    const uint16_t excluded_reasons[] = {202, 201, 200, 205, 204, 15};
    for (size_t i = 0; i < sizeof(excluded_reasons) / sizeof(excluded_reasons[0]); ++i) {
        reset_flow(); run_worker(begin, b1, yes, 1);
        command_t excluded = cmd(CMD_DISCONNECTED, 0);
        excluded.reason = excluded_reasons[i]; excluded.rssi_valid = true;
        excluded.rssi = (excluded.reason == 204 || excluded.reason == 15) ? -41 : -20;
        int64_t excluded_at[] = {1200000};
        run_worker(&excluded, excluded_at, yes, 1);
        assert(!s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0 &&
               !s_manager.trial_active && !strcmp(s_manager.status_state, "failed"));
    }
    const uint16_t newly_observed_reasons[] = {2, 39};
    for (size_t i = 0; i < sizeof(newly_observed_reasons) / sizeof(newly_observed_reasons[0]); ++i) {
        reset_flow(); run_worker(begin, b1, yes, 1);
        command_t observed = cmd(CMD_DISCONNECTED, 0);
        observed.reason = newly_observed_reasons[i]; observed.rssi_valid = true; observed.rssi = -26;
        int64_t observed_at[] = {1200000}; run_worker(&observed, observed_at, yes, 1);
        assert(s_manager.radio_recovery == RADIO_WAIT_STOP && s_manager.trial_active &&
               !s_manager.tx_low && !s_manager.tx_retry_pending && tx_power == 52 && tx_set_calls == 0);
        complete_tx_fallback_barrier(1250000, 1260000);
        assert(s_manager.radio_recovery == RADIO_READY && s_manager.tx_low &&
               s_manager.tx_retry_pending && tx_power == 28 && tx_set_calls == 1);
    }
    reset_flow(); run_worker(begin, b1, yes, 1);
    command_t invalid_disconnect = cmd(CMD_DISCONNECTED, 0);
    invalid_disconnect.reason = 204; invalid_disconnect.rssi_valid = false;
    int64_t invalid_at[] = {1200000}; run_worker(&invalid_disconnect, invalid_at, yes, 1);
    assert(!s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0);

    /* Missing startup readback leaves normal setup connection usable, but disables fallback. */
    reset_flow(); tx_get_fail = 1000; run_worker(begin, b1, yes, 1);
    assert(!s_manager.tx_original_known && s_manager.trial_active && connect_calls == 1);
    command_t unknown_disconnect[] = {cmd(CMD_DISCONNECTED, 0)};
    unknown_disconnect[0].reason = 204; unknown_disconnect[0].rssi_valid = true;
    unknown_disconnect[0].rssi = -20;
    run_worker(unknown_disconnect, invalid_at, yes, 1);
    assert(!s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0);

    /* Setter and readback failures fail closed; the readable original is restored when possible. */
    reset_flow(); run_worker(begin, b1, yes, 1); tx_set_fail = 1;
    command_t set_error[] = {cmd(CMD_DISCONNECTED, 0)};
    set_error[0].reason = 204; set_error[0].rssi_valid = true; set_error[0].rssi = -20;
    run_worker(set_error, invalid_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && tx_set_calls == 0);
    complete_tx_fallback_barrier(1250000, 1260000);
    assert(!s_manager.tx_retry_pending && !s_manager.trial_active && s_manager.tx_fault &&
           !strcmp(s_manager.status_state, "failed"));
    reset_flow(); run_worker(begin, b1, yes, 1); tx_get_fail = 1;
    command_t read_error[] = {cmd(CMD_DISCONNECTED, 0)};
    read_error[0].reason = 204; read_error[0].rssi_valid = true; read_error[0].rssi = -20;
    run_worker(read_error, invalid_at, yes, 1);
    complete_tx_fallback_barrier(1250000, 1260000);
    assert(!s_manager.tx_retry_pending && !s_manager.trial_active && tx_power == 52 &&
           !s_manager.tx_low && !s_manager.tx_fault);

    /* STOP and START failures/watchdogs never apply 28 or reconnect credentials. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    command_t barrier_submit[] = {cmd(CMD_DISCONNECTED, 0)};
    barrier_submit[0].reason = 204; barrier_submit[0].rssi_valid = true; barrier_submit[0].rssi = -20;
    stop_result = ESP_FAIL; run_worker(barrier_submit, qualified_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_FAILED && !s_manager.trial_active &&
           !s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0 && connect_calls == 1);
    reset_flow(); run_worker(begin, b1, yes, 1);
    run_worker(barrier_submit, qualified_at, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    command_t barrier_stop[] = {captured[captured_count - 1]}; int64_t barrier_stop_time[] = {1300000};
    start_result = ESP_FAIL; run_worker(barrier_stop, barrier_stop_time, yes, 1);
    assert(s_manager.radio_recovery == RADIO_FAILED && !s_manager.trial_active &&
           !s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0 && connect_calls == 1);
    reset_flow(); run_worker(begin, b1, yes, 1);
    run_worker(barrier_submit, qualified_at, yes, 1);
    command_t radio_watchdog[] = {cmd(CMD_GOT_IP, 0)}; bool radio_watchdog_no[] = {false};
    int64_t stop_watchdog_at[] = {6200000}; run_worker(radio_watchdog, stop_watchdog_at, radio_watchdog_no, 1);
    assert(s_manager.radio_recovery == RADIO_FAILED && !s_manager.trial_active &&
           !s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0);
    reset_flow(); run_worker(begin, b1, yes, 1);
    run_worker(barrier_submit, qualified_at, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    barrier_stop[0] = captured[captured_count - 1]; run_worker(barrier_stop, barrier_stop_time, yes, 1);
    int64_t start_watchdog_at[] = {6300000}; run_worker(radio_watchdog, start_watchdog_at, radio_watchdog_no, 1);
    assert(s_manager.radio_recovery == RADIO_FAILED && !s_manager.trial_active &&
           !s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0);

    /* Forget during the barrier cancels low TX and retry while allowing the radio restart to finish. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    run_worker(barrier_submit, qualified_at, yes, 1);
    command_t forget_during_barrier[] = {cmd(CMD_FORGET, 0)}; int64_t forget_barrier_at[] = {1220000};
    run_worker(forget_during_barrier, forget_barrier_at, yes, 1);
    assert(s_manager.radio_restart_purpose == RADIO_RESTART_CANCELLED &&
           s_manager.radio_recovery == RADIO_WAIT_STOP && !s_manager.trial_active && tx_set_calls == 0);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    barrier_stop[0] = captured[captured_count - 1]; int64_t forget_stop_at[] = {1250000};
    run_worker(barrier_stop, forget_stop_at, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    command_t forget_start[] = {captured[captured_count - 1]}; int64_t forget_start_at[] = {1260000};
    run_worker(forget_start, forget_start_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_READY && s_manager.radio_restart_purpose == RADIO_RESTART_CANCELLED &&
           !s_manager.tx_low && !s_manager.tx_retry_pending && tx_set_calls == 0 && connect_calls == 1);

    /* A restore failure after a low-power disconnect prevents a new connection attempt. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    command_t eligible_for_restore[] = {cmd(CMD_DISCONNECTED, 0)};
    eligible_for_restore[0].reason = 204; eligible_for_restore[0].rssi_valid = true;
    eligible_for_restore[0].rssi = -20;
    run_worker(eligible_for_restore, qualified_at, yes, 1);
    complete_tx_fallback_barrier(1250000, 1260000);
    int64_t restore_retry_at[] = {1360000};
    run_worker(retry_tick, restore_retry_at, no, 1);
    tx_set_fail = 1;
    command_t low_failure[] = {cmd(CMD_DISCONNECTED, 0)};
    low_failure[0].reason = 202; low_failure[0].rssi_valid = true; low_failure[0].rssi = -20;
    run_worker(low_failure, invalid_at, yes, 1);
    assert(s_manager.tx_fault && !s_manager.trial_active && connect_calls == 2);

    /* Saved credentials use the same one fallback and retain reduced TX after GOT_IP. */
    reset_flow(); s_provisioning = false; s_manager.selected.state = WIFI_STORE_CONFIGURED;
    strcpy(s_manager.selected.ssid, "Home"); strcpy(s_manager.selected.password, "saved-password");
    command_t boot_tick[] = {cmd(CMD_GOT_IP, 0)}; bool boot_no[] = {false};
    int64_t boot_time[] = {1100000}; run_worker(boot_tick, boot_time, boot_no, 1);
    assert(s_manager.saved_connecting && connect_calls == 1 && tx_power == 52);
    command_t saved_disconnect[] = {cmd(CMD_DISCONNECTED, 0)};
    saved_disconnect[0].reason = 204; saved_disconnect[0].rssi_valid = true; saved_disconnect[0].rssi = -20;
    run_worker(saved_disconnect, qualified_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && tx_power == 52 && tx_set_calls == 0);
    complete_tx_fallback_barrier(1250000, 1260000);
    assert(!s_manager.saved_connecting && s_manager.tx_retry_pending && tx_power == 28);
    run_worker(retry_tick, retry_at, boot_no, 1);
    assert(s_manager.saved_connecting && !s_manager.tx_retry_pending && connect_calls == 2 && tx_power == 28);
    command_t saved_low_failure[] = {cmd(CMD_DISCONNECTED, 0)};
    saved_low_failure[0].reason = 202;
    int64_t saved_low_failure_at[] = {1400000}; run_worker(saved_low_failure, saved_low_failure_at, yes, 1);
    assert(!s_manager.saved_connecting && !s_manager.tx_low && tx_power == 52 &&
           s_manager.tx_fallback_attempted && s_manager.next_retry == 3400000);
    command_t next_saved_cycle[] = {cmd(CMD_GOT_IP, 0)}; bool next_cycle_no[] = {false};
    int64_t next_cycle_at[] = {3500000}; run_worker(next_saved_cycle, next_cycle_at, next_cycle_no, 1);
    assert(s_manager.saved_connecting && connect_calls == 3 && !s_manager.tx_fallback_attempted && tx_power == 52);
    command_t second_cycle_eligible[] = {cmd(CMD_DISCONNECTED, 0)};
    second_cycle_eligible[0].reason = 15; second_cycle_eligible[0].rssi_valid = true;
    second_cycle_eligible[0].rssi = -22;
    int64_t second_cycle_at[] = {3600000}; run_worker(second_cycle_eligible, second_cycle_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && tx_power == 52 && tx_set_calls == 2);
    complete_tx_fallback_barrier(3650000, 3660000);
    assert(s_manager.tx_retry_pending && s_manager.tx_low && tx_power == 28 && connect_calls == 3);

    /* Saved low-phase timeout also waits for STOP/START before restoring and backoff. */
    reset_flow(); s_provisioning = false; s_manager.selected.state = WIFI_STORE_CONFIGURED;
    strcpy(s_manager.selected.ssid, "Home"); strcpy(s_manager.selected.password, "saved-password");
    run_worker(boot_tick, boot_time, boot_no, 1);
    run_worker(saved_disconnect, qualified_at, yes, 1);
    complete_tx_fallback_barrier(1250000, 1260000);
    run_worker(retry_tick, retry_at, boot_no, 1);
    command_t saved_timeout[] = {cmd(CMD_GOT_IP, 0)}; int64_t saved_timeout_at[] = {21360000};
    run_worker(saved_timeout, saved_timeout_at, boot_no, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && s_manager.tx_restore_after_start &&
           tx_power == 28 && !s_manager.saved_connecting);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    int64_t saved_stop_at[] = {21400000}; run_worker(&captured[captured_count - 1], saved_stop_at, yes, 1);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    int64_t saved_start_at[] = {21500000}; run_worker(&captured[captured_count - 1], saved_start_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_READY && tx_power == 52 && !s_manager.tx_low &&
           !s_manager.tx_restore_after_start && !s_manager.saved_connecting && connect_calls == 2 &&
           s_manager.tx_fallback_attempted);
    int64_t saved_backoff_expired[] = {23600000};
    run_worker(saved_timeout, saved_backoff_expired, boot_no, 1);
    assert(s_manager.saved_connecting && connect_calls == 3 && !s_manager.tx_fallback_attempted);
    command_t post_timeout_eligible[] = {cmd(CMD_DISCONNECTED, 0)};
    post_timeout_eligible[0].reason = 204; post_timeout_eligible[0].rssi_valid = true;
    post_timeout_eligible[0].rssi = -20;
    int64_t post_timeout_at[] = {23700000};
    run_worker(post_timeout_eligible, post_timeout_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && tx_power == 52);
    complete_tx_fallback_barrier(23750000, 23760000);
    assert(s_manager.tx_retry_pending && tx_power == 28);

    /* Failed low-power readback restores the original and terminates this attempt. */
    reset_flow(); run_worker(begin, b1, yes, 1); tx_readback_override = 27;
    command_t mismatch[] = {cmd(CMD_DISCONNECTED, 0)};
    mismatch[0].reason = 15; mismatch[0].rssi_valid = true; mismatch[0].rssi = -30;
    int64_t mismatch_at[] = {1200000}; run_worker(mismatch, mismatch_at, yes, 1);
    complete_tx_fallback_barrier(1250000, 1260000);
    assert(!s_manager.tx_retry_pending && !s_manager.trial_active && tx_power == 52 &&
           !s_manager.tx_low && !s_manager.tx_fault && !strcmp(s_manager.status_state, "failed"));

    /* Low-power phase timeout restores original TX only after fresh STOP/START events. */
    reset_flow(); run_worker(begin, b1, yes, 1);
    command_t eligible[] = {cmd(CMD_DISCONNECTED, 0)};
    eligible[0].reason = 204; eligible[0].rssi_valid = true; eligible[0].rssi = -20;
    run_worker(eligible, qualified_at, yes, 1);
    complete_tx_fallback_barrier(1250000, 1260000);
    int64_t low_retry_time[] = {1360000}; run_worker(retry_tick, low_retry_time, no, 1);
    command_t low_timeout[] = {cmd(CMD_GOT_IP, 0)}; int64_t low_timeout_at[] = {21360000};
    run_worker(low_timeout, low_timeout_at, no, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_STOP && s_manager.tx_restore_after_start &&
           tx_power == 28 && stop_calls == 2 &&
           s_manager.radio_restart_purpose == RADIO_RESTART_TIMEOUT);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    int64_t tx_stop_at[] = {21460000}; run_worker(&captured[captured_count - 1], tx_stop_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_WAIT_START && tx_power == 28);
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    int64_t tx_start_at[] = {21560000}; run_worker(&captured[captured_count - 1], tx_start_at, yes, 1);
    assert(s_manager.radio_recovery == RADIO_READY && tx_power == 52 && !s_manager.tx_low &&
           !s_manager.tx_restore_after_start && !s_manager.trial_active);
    puts("Wi-Fi setup flow: attempt correlation, association+IP+SSID, adaptive TX retry, restore, failures, MAC readiness and scan dedup passed");
    return 0;
}
'''


class WifiSetupFlowTests(unittest.TestCase):
    def test_worker_attempt_state_and_ap_client_flow_under_sanitizers(self):
        manager = (ROOT / "main/wifi_manager.c").read_text()
        enum_match = re.search(r"typedef enum \{([^}]+)\} command_kind_t;", manager, re.S)
        self.assertIsNotNone(enum_match)
        commands = re.findall(r"\b(CMD_[A-Z_]+)\b", enum_match.group(1))
        replacements = {f"__{name}__": str(commands.index(name)) for name in commands}
        production = "\n\n".join(function(manager, signature) for signature in (
            "static void event_handler(", "static void set_status(", "void wifi_manager_get_status(",
            "static void show_setup(", "static void finish_setup(",
            "static void capture_tx_limit(", "static bool restore_tx_limit(", "static bool set_low_tx_limit(",
            "static bool start_saved_connect(",
            "static void set_scan_state(", "static void start_trial(", "static bool mac_present(",
    "static void mac_remove(", "static bool valid_utf8_ssid(",
            "static void scan_done(",
            "static bool command_cap_valid(", "static void discard_deferred_trial(",
            "static void start_deferred_trial(", "static void fail_radio_recovery(",
            "static void restart_for_management(",
            "static void handle_radio_event(", "static void worker("))
        # The production worker owns one FreeRTOS task. Scripted host batches re-enter
        # worker() only to deliver each event; gate its startup block in this fixture.
        production = production.replace(
            "    capture_tx_limit();\n"
            "    if (wifi_manager_is_provisioning()) {",
            "    if (!s_worker_started) { s_worker_started = true;\n"
            "        capture_tx_limit();\n"
            "        if (wifi_manager_is_provisioning()) {")
        production = production.replace(
            "        s_manager.next_retry = esp_timer_get_time() + 15000000LL;\n"
            "    }\n"
            "    for (;;) {",
            "        s_manager.next_retry = esp_timer_get_time() + 15000000LL;\n"
            "        }\n"
            "    }\n"
            "    for (;;) {")
        source = HARNESS
        for name, value in replacements.items():
            source = source.replace(name, value)
        source += production + CASES
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang is required for ASan/UBSan host tests")
        with tempfile.TemporaryDirectory(prefix="wifi-flow-") as directory:
            c_file = Path(directory) / "wifi_flow.c"
            executable = Path(directory) / "wifi_flow"
            c_file.write_text(source)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-fsanitize=address,undefined",
                            "-I", str(ROOT / "main"), str(ROOT / "main/wifi_failure.c"),
                            str(ROOT / "main/wifi_tx_policy.c"),
                            str(c_file), "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("Wi-Fi setup flow:", result.stdout)


if __name__ == "__main__":
    unittest.main()
