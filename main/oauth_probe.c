#include "oauth_probe.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <netdb.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "display.h"
#include "app_registry.h"
#include "box_config.h"
#include "device_discovery.h"
#include "device_identity.h"
#include "factory_reset.h"
#include "codex_adapter.h"
#include "display_format.h"
#include "provider_http.h"
#include "probe_protocol.h"
#include "refresh_control.h"
#include "wifi_manager.h"
#include "wifi_store.h"

#define CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define REDIRECT_URI "https://platform.claude.com/oauth/code/callback"
#define AUTH_URL "https://claude.com/cai/oauth/authorize"
#define CODE_URL "https://platform.claude.com/v1/oauth/token"
#define REFRESH_URL "https://platform.claude.com/v1/oauth/token"
#define USAGE_URL "https://api.anthropic.com/api/oauth/usage"
#define OAUTH_BETA "oauth-2025-04-20"
#define TOKEN_VERSION 1
#define RESPONSE_LIMIT 8192
#define CAP_LIFETIME_SEC 600
#define RESET_CONFIRM_LIFETIME_US (60LL * 1000000LL)
#define SETUP_PAGE_CAPACITY 8192

static const char *TAG = "oauth_probe";

typedef struct {
    uint32_t version;
    int64_t expires_at;
    char access[2048];
    char refresh[2048];
} token_store_t;

typedef struct {
    char body[RESPONSE_LIMIT + 1];
    size_t len;
    int retry_after;
    bool overflow;
} response_t;

typedef struct { char code[513]; uint32_t session; } submission_t;
typedef struct { uint32_t request_id; bool open; } settings_command_t;
typedef struct { uint32_t session, request_id; char cap[48]; int64_t deadline_us; } reset_command_t;
typedef struct {
    uint32_t id;
    char method[24], arg1[64], arg2[513];
} management_command_t;

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta;
static QueueHandle_t s_submissions;
static QueueHandle_t s_settings_commands;
static QueueHandle_t s_forget_commands;
static QueueHandle_t s_reset_commands;
static QueueHandle_t s_management_commands;
static SemaphoreHandle_t s_cap_lock;
static httpd_handle_t s_server;
static char s_cap[48];
static char s_setup_url[128];
static char s_auth_url[512];
static int64_t s_cap_deadline;
static bool s_cap_valid;
static uint32_t s_session;
static bool s_manual_session;
static uint32_t s_manual_request_id;
static bool s_auto_qr_visible;
static portMUX_TYPE s_settings_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_settings_wanted_id;
static bool s_settings_wanted_open;
static bool s_settings_committing;
static atomic_bool s_forget_response_ready;
static atomic_bool s_reset_response_ready;
static atomic_bool s_reset_inflight;
static atomic_bool s_reset_committed;
/* Protected by s_cap_lock; a confirmation belongs to one capability/session. */
static char s_reset_nonce[48];
static uint32_t s_reset_nonce_session;
static int64_t s_reset_nonce_deadline_us;
static const char *s_page_state = "Ready";
/* Only the OAuth worker owns credentials and this flag. */
static bool s_token_dirty;
static atomic_bool s_time_synced;
static portMUX_TYPE s_refresh_mux = portMUX_INITIALIZER_UNLOCKED;
static refresh_control_t s_refresh_control;
static TaskHandle_t s_worker_task;
static portMUX_TYPE s_management_mux = portMUX_INITIALIZER_UNLOCKED;
static oauth_management_status_t s_management;
static provider_usage_t s_claude_usage;
static uint32_t s_management_codex_id; /* OAuth worker only. */
static atomic_bool s_usb_session_active;
static atomic_bool s_token_configured;
static atomic_bool s_usage_validated;
static uint32_t s_management_wifi_id; /* OAuth worker only. */
static uint32_t s_claude_cycle_generation; /* OAuth worker owns its cycle. */
static struct {
    char state[48], verifier[80];
    int64_t deadline_us;
} s_usb_oauth;

static int64_t now_s(void) { return (int64_t)time(NULL); }
static bool wifi_up(void) { return (xEventGroupGetBits(s_wifi_events) & OAUTH_WIFI_CONNECTED) != 0; }

static void on_time_sync(struct timeval *tv) { (void)tv; atomic_store(&s_time_synced, true); }
static bool clock_valid(void) { return atomic_load(&s_time_synced) && now_s() > 1735689600; }

static void management_operation(uint32_t id, const char *method,
                                 const char *state, const char *message)
{
    taskENTER_CRITICAL(&s_management_mux);
    s_management.operation.id = id;
    snprintf(s_management.operation.method, sizeof(s_management.operation.method), "%s", method);
    snprintf(s_management.operation.state, sizeof(s_management.operation.state), "%s", state);
    snprintf(s_management.operation.message, sizeof(s_management.operation.message), "%s", message);
    taskEXIT_CRITICAL(&s_management_mux);
}

static void management_account(bool configured, const char *state,
                               const char *session, const char *url)
{
    taskENTER_CRITICAL(&s_management_mux);
    s_management.configured = configured;
    snprintf(s_management.state, sizeof(s_management.state), "%s", state);
    snprintf(s_management.session, sizeof(s_management.session), "%s", session ? session : "");
    snprintf(s_management.authorization_url, sizeof(s_management.authorization_url),
             "%s", url ? url : "");
    taskEXIT_CRITICAL(&s_management_mux);
}

static void management_network_invalidate(const char *message)
{
    taskENTER_CRITICAL(&s_management_mux);
    s_management.network.dns = false;
    s_management.network.https = false;
    snprintf(s_management.network.state, sizeof(s_management.network.state), "untested");
    snprintf(s_management.network.message, sizeof(s_management.network.message),
             "%s", message);
    taskEXIT_CRITICAL(&s_management_mux);
}

void oauth_probe_get_management_status(oauth_management_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_management_mux);
    *out = s_management;
    out->usage = s_claude_usage;
    taskEXIT_CRITICAL(&s_management_mux);
    out->configured = atomic_load(&s_token_configured);
    out->usage.configured = out->configured;
    if (!atomic_load(&s_usb_session_active))
        snprintf(out->state, sizeof(out->state), "%s",
                 !out->configured ? "not_configured" :
                 atomic_load(&s_usage_validated) ? "validated" : "checking");
    bool connected = s_wifi_events && wifi_up() && !wifi_manager_is_provisioning();
    bool just_reconnected = s_wifi_events &&
                            (xEventGroupGetBits(s_wifi_events) & OAUTH_WIFI_RECONNECTED);
    esp_netif_ip_info_t ip = {0};
    bool has_ip = connected && s_sta &&
                  esp_netif_get_ip_info(s_sta, &ip) == ESP_OK && ip.ip.addr != 0;
    out->network.wifi = connected;
    out->network.ip = has_ip;
    out->network.clock = clock_valid();
    if (!connected || !has_ip || !out->network.clock || just_reconnected) {
        out->network.dns = false;
        out->network.https = false;
    }
    if (!connected) {
        snprintf(out->network.state, sizeof(out->network.state), "offline");
        snprintf(out->network.message, sizeof(out->network.message), "Wi-Fi is offline");
    } else if (!has_ip) {
        snprintf(out->network.state, sizeof(out->network.state), "waiting_ip");
        snprintf(out->network.message, sizeof(out->network.message), "Waiting for an IP address");
    } else if (!out->network.clock) {
        snprintf(out->network.state, sizeof(out->network.state), "waiting_time");
        snprintf(out->network.message, sizeof(out->network.message), "Waiting for a valid clock");
    } else if (just_reconnected) {
        snprintf(out->network.state, sizeof(out->network.state), "untested");
        snprintf(out->network.message, sizeof(out->network.message),
                 "Network changed; run test again");
    } else if (!out->network.state[0]) {
        snprintf(out->network.state, sizeof(out->network.state), "untested");
        snprintf(out->network.message, sizeof(out->network.message), "Run network test");
    }
}

static bool claude_selected(void)
{
    return usage_provider_enabled(box_config_current()->providers, USAGE_PROVIDER_CLAUDE);
}

static bool codex_selected(void)
{
    return usage_provider_enabled(box_config_current()->providers, USAGE_PROVIDER_CODEX);
}

static void publish_claude_usage(const provider_usage_t *usage)
{
    taskENTER_CRITICAL(&s_management_mux);
    s_claude_usage = *usage;
    taskEXIT_CRITICAL(&s_management_mux);
    display_provider_update(USAGE_PROVIDER_CLAUDE, usage);
}

static void reset_claude_usage(bool configured, const char *status)
{
    provider_usage_t snapshot = {.configured = configured};
    if (status) snprintf(snapshot.status, sizeof(snapshot.status), "%s", status);
    publish_claude_usage(&snapshot);
}

static void claude_status(const char *title, const char *detail)
{
    taskENTER_CRITICAL(&s_management_mux);
    s_claude_usage.configured = atomic_load(&s_token_configured);
    snprintf(s_claude_usage.status, sizeof(s_claude_usage.status), "%s", detail);
    s_claude_usage.updating = false;
    provider_usage_t snapshot = s_claude_usage;
    taskEXIT_CRITICAL(&s_management_mux);
    display_provider_update(USAGE_PROVIDER_CLAUDE, &snapshot);
    if (claude_selected() && !codex_selected()) display_status(title, detail);
}

static void claude_updating(void)
{
    taskENTER_CRITICAL(&s_management_mux);
    s_claude_usage.configured = true;
    s_claude_usage.updating = true;
    snprintf(s_claude_usage.status, sizeof(s_claude_usage.status), "Checking usage");
    provider_usage_t snapshot = s_claude_usage;
    taskEXIT_CRITICAL(&s_management_mux);
    display_provider_update(USAGE_PROVIDER_CLAUDE, &snapshot);
    if (!codex_selected()) display_updating();
}

bool oauth_probe_management_request(uint32_t id, const char *method,
                                    const char *arg1, const char *arg2)
{
    if (!id || !method || !s_management_commands || !s_worker_task ||
        atomic_load(&s_reset_committed)) return false;
    management_command_t cmd = {.id = id};
    size_t method_len = strnlen(method, sizeof(cmd.method));
    size_t arg1_len = arg1 ? strnlen(arg1, sizeof(cmd.arg1)) : 0;
    size_t arg2_len = arg2 ? strnlen(arg2, sizeof(cmd.arg2)) : 0;
    if (method_len == sizeof(cmd.method) || arg1_len == sizeof(cmd.arg1) ||
        arg2_len == sizeof(cmd.arg2)) return false;
    if (strcmp(method, "wifi.connect") && strcmp(method, "network.test") &&
        strcmp(method, "codex.begin") && strcmp(method, "codex.cancel") &&
        strcmp(method, "claude.begin") && strcmp(method, "claude.submit_code") &&
        strcmp(method, "claude.cancel")) return false;
    if (!strcmp(method, "wifi.connect") &&
        (!arg1 || !arg2 || !wifi_store_valid_credentials(arg1, arg2))) return false;
    if (!strcmp(method, "claude.submit_code") && (!arg1_len || !arg2_len)) return false;
    memcpy(cmd.method, method, method_len + 1);
    if (arg1) memcpy(cmd.arg1, arg1, arg1_len + 1);
    if (arg2) memcpy(cmd.arg2, arg2, arg2_len + 1);
    taskENTER_CRITICAL(&s_management_mux);
    bool duplicate = s_management.operation.id == id &&
                     strcmp(s_management.operation.method, method) == 0;
    bool reused_id = s_management.operation.id == id && !duplicate;
    bool busy = !strcmp(s_management.operation.state, "queued") ||
                !strcmp(s_management.operation.state, "running");
    if (!duplicate && !reused_id && !busy) {
        s_management.operation.id = id;
        snprintf(s_management.operation.method, sizeof(s_management.operation.method), "%s", method);
        snprintf(s_management.operation.state, sizeof(s_management.operation.state), "queued");
        snprintf(s_management.operation.message, sizeof(s_management.operation.message),
                 "Waiting for device worker");
    }
    taskEXIT_CRITICAL(&s_management_mux);
    if (duplicate || reused_id || busy) {
        memset(&cmd, 0, sizeof(cmd));
        return duplicate;
    }
    if (xQueueSend(s_management_commands, &cmd, 0) != pdTRUE) {
        management_operation(id, method, "failed", "Device queue is busy");
        memset(&cmd, 0, sizeof(cmd));
        return false;
    }
    memset(&cmd, 0, sizeof(cmd));
    xTaskNotifyGive(s_worker_task);
    return true;
}

static void publish_refresh(bool ready, int64_t retry_at)
{
    taskENTER_CRITICAL(&s_refresh_mux);
    refresh_control_publish(&s_refresh_control, ready, retry_at);
    taskEXIT_CRITICAL(&s_refresh_mux);
}

static bool begin_refresh(bool automatic_due, bool *manual, uint32_t *generation)
{
    taskENTER_CRITICAL(&s_refresh_mux);
    bool began = (automatic_due || s_refresh_control.pending) &&
                 refresh_control_begin(&s_refresh_control, manual);
    taskEXIT_CRITICAL(&s_refresh_mux);
    if (began) {
        if (*manual) ESP_LOGI(TAG, "Manual usage refresh started");
        if (++s_claude_cycle_generation == 0) ++s_claude_cycle_generation;
        *generation = s_claude_cycle_generation;
        display_app_refresh_begin(APP_ID_CLAUDE, *generation);
        claude_updating();
    }
    return began;
}

static void end_refresh(bool ready, int64_t retry_at, uint32_t generation)
{
    display_app_refresh_end(APP_ID_CLAUDE, generation);
    taskENTER_CRITICAL(&s_refresh_mux);
    refresh_control_finish(&s_refresh_control, ready, retry_at);
    taskEXIT_CRITICAL(&s_refresh_mux);
}

static uint32_t request_app_refresh(void *context, app_id_t app_id)
{
    (void)context;
    if (app_id != APP_ID_ALL && app_id != APP_ID_CLAUDE && app_id != APP_ID_CODEX)
        return 0;
    if (!s_worker_task || !s_wifi_events || atomic_load(&s_reset_committed) ||
        !wifi_up() || !clock_valid()) return 0;
    int64_t now = now_s();
    bool claude = (app_id == APP_ID_ALL || app_id == APP_ID_CLAUDE) && claude_selected();
    bool codex = (app_id == APP_ID_ALL || app_id == APP_ID_CODEX) && codex_selected();
    codex_management_status_t codex_status = {0};
    if (codex) codex_adapter_get_status(&codex_status);
    taskENTER_CRITICAL(&s_refresh_mux);
    bool claude_accepted = claude && refresh_control_request(&s_refresh_control, now);
    taskEXIT_CRITICAL(&s_refresh_mux);
    bool codex_accepted = codex && codex_status.configured && codex_adapter_refresh();
    uint32_t accepted = (claude_accepted ? 1u << APP_ID_CLAUDE : 0u) |
                        (codex_accepted ? 1u << APP_ID_CODEX : 0u);
    if (accepted != 0) {
        ESP_LOGI(TAG, "Manual usage refresh accepted");
        xTaskNotifyGive(s_worker_task);
    }
    return accepted;
}

static bool request_manual_refresh(void *context)
{
    return request_app_refresh(context, APP_ID_ALL) != 0;
}

static bool settings_wanted(uint32_t id)
{
    taskENTER_CRITICAL(&s_settings_mux);
    bool wanted = s_settings_wanted_open && s_settings_wanted_id == id;
    taskEXIT_CRITICAL(&s_settings_mux);
    return wanted;
}

/* Reserve the account switch against a concurrent Back callback. No flash or
 * display operation runs under this critical section. */
static bool reserve_settings_commit(uint32_t id)
{
    taskENTER_CRITICAL(&s_settings_mux);
    bool reserved = !s_settings_committing && s_settings_wanted_open &&
                    s_settings_wanted_id == id;
    if (reserved) s_settings_committing = true;
    taskEXIT_CRITICAL(&s_settings_mux);
    return reserved;
}

static void finish_settings_commit(void)
{
    taskENTER_CRITICAL(&s_settings_mux);
    s_settings_committing = false;
    taskEXIT_CRITICAL(&s_settings_mux);
}

static bool request_settings(void *context, uint32_t request_id, bool open)
{
    (void)context;
    if (!s_settings_commands || !s_worker_task || !request_id) return false;
    if (open && atomic_load(&s_usb_session_active)) return false;
    settings_command_t command = {.request_id = request_id, .open = open};
    taskENTER_CRITICAL(&s_settings_mux);
    if (s_settings_committing) {
        taskEXIT_CRITICAL(&s_settings_mux);
        return false;
    }
    s_settings_wanted_id = request_id;
    s_settings_wanted_open = open;
    taskEXIT_CRITICAL(&s_settings_mux);
    if (xQueueOverwrite(s_settings_commands, &command) != pdTRUE) return false;
    xTaskNotifyGive(s_worker_task);
    return true;
}

static bool encode_random(char *dst, size_t capacity, size_t random_bytes)
{
    uint8_t bytes[48];
    unsigned char encoded[80];
    size_t written = 0;
    if (random_bytes > sizeof(bytes)) return false;
    esp_fill_random(bytes, random_bytes);
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written, bytes, random_bytes) != 0 ||
        written + 1 > capacity) return false;
    for (size_t i = 0; i < written; ++i) {
        if (encoded[i] == '+') encoded[i] = '-';
        if (encoded[i] == '/') encoded[i] = '_';
    }
    while (written && encoded[written - 1] == '=') --written;
    if (written + 1 > capacity) return false;
    memcpy(dst, encoded, written);
    dst[written] = 0;
    memset(bytes, 0, sizeof(bytes));
    return true;
}

static bool pkce_challenge(const char *verifier, char *challenge, size_t capacity)
{
    uint8_t digest[32];
    unsigned char encoded[48];
    size_t written;
    if (mbedtls_sha256((const unsigned char *)verifier, strlen(verifier), digest, 0) != 0 ||
        mbedtls_base64_encode(encoded, sizeof(encoded), &written, digest, sizeof(digest)) != 0)
        return false;
    while (written && encoded[written - 1] == '=') --written;
    if (written + 1 > capacity) return false;
    for (size_t i = 0; i < written; ++i) {
        challenge[i] = encoded[i] == '+' ? '-' : encoded[i] == '/' ? '_' : encoded[i];
    }
    challenge[written] = 0;
    return true;
}

static bool authorization_url(char *url, size_t capacity,
                              const char *challenge, const char *state)
{
    int n = snprintf(url, capacity,
        AUTH_URL "?code=true&client_id=" CLIENT_ID "&response_type=code&redirect_uri="
        "https%%3A%%2F%%2Fplatform.claude.com%%2Foauth%%2Fcode%%2Fcallback"
        "&scope=user%%3Aprofile%%20user%%3Ainference&code_challenge=%s&code_challenge_method=S256&state=%s",
        challenge, state);
    return n > 0 && (size_t)n < capacity;
}

static bool token_valid(const token_store_t *t)
{
    return t->version == TOKEN_VERSION && t->expires_at > 0 &&
           strnlen(t->access, sizeof(t->access)) < sizeof(t->access) && t->access[0] &&
           strnlen(t->refresh, sizeof(t->refresh)) < sizeof(t->refresh) && t->refresh[0];
}

static bool load_token(token_store_t *t)
{
    nvs_handle_t h;
    size_t size = sizeof(*t);
    if (nvs_open("claude_probe", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_blob(h, "token", t, &size);
    nvs_close(h);
    bool valid = err == ESP_OK && size == sizeof(*t) && token_valid(t);
    atomic_store(&s_token_configured, valid);
    return valid;
}

static bool save_token(const token_store_t *t)
{
    nvs_handle_t h;
    if (nvs_open("claude_probe", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "token", t, sizeof(*t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) atomic_store(&s_token_configured, true);
    return err == ESP_OK;
}

static bool clear_token(void)
{
    nvs_handle_t h;
    if (nvs_open("claude_probe", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(h, "token");
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        atomic_store(&s_token_configured, false);
        atomic_store(&s_usage_validated, false);
    }
    return err == ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    response_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (r->len + (size_t)evt->data_len > RESPONSE_LIMIT) {
            r->overflow = true;
        } else {
            memcpy(r->body + r->len, evt->data, evt->data_len);
            r->len += evt->data_len;
            r->body[r->len] = 0;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
               strcasecmp(evt->header_key, "Retry-After") == 0) {
        r->retry_after = probe_retry_after_seconds(evt->header_value, now_s());
    }
    return ESP_OK;
}

static int request_json(const char *url, const char *post_body, const char *bearer, response_t *r)
{
    memset(r, 0, sizeof(*r));
    if (wifi_manager_is_provisioning()) return -1;
    if (!provider_http_take(pdMS_TO_TICKS(20000))) return -1;
    esp_http_client_config_t config = {
        .url = url, .method = post_body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event, .user_data = r, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&config);
    if (!c) { provider_http_give(); return -1; }
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "anthropic-beta", OAUTH_BETA);
    esp_http_client_set_header(c, "anthropic-version", "2023-06-01");
    if (post_body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, post_body, strlen(post_body));
    }
    char authorization[2060];
    if (bearer) {
        if (snprintf(authorization, sizeof(authorization), "Bearer %s", bearer) >= sizeof(authorization)) {
            esp_http_client_cleanup(c);
            provider_http_give();
            return -1;
        }
        esp_http_client_set_header(c, "Authorization", authorization);
    }
    int64_t began = esp_timer_get_time();
    esp_err_t err = wifi_manager_is_provisioning() ? ESP_FAIL : esp_http_client_perform(c);
    int status = err == ESP_OK && !r->overflow ? esp_http_client_get_status_code(c) : -1;
    ESP_LOGI(TAG, "HTTPS status=%d elapsed_ms=%lld heap=%lu", status,
             (long long)((esp_timer_get_time() - began) / 1000), (unsigned long)esp_get_free_heap_size());
    memset(authorization, 0, sizeof(authorization));
    esp_http_client_cleanup(c);
    provider_http_give();
    return status;
}

static bool json_text(cJSON *parent, const char *key, const char **value, size_t max)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t n = strnlen(item->valuestring, max);
    if (n == 0 || n >= max) return false;
    *value = item->valuestring;
    return true;
}

static bool parse_token(const response_t *r, token_store_t *current, bool is_refresh,
                        bool persist)
{
    cJSON *root = cJSON_Parse(r->body);
    if (!root) return false;
    const char *access = NULL, *refresh = NULL;
    cJSON *expiry = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    bool good = json_text(root, "access_token", &access, sizeof(current->access)) &&
                cJSON_IsNumber(expiry) && isfinite(expiry->valuedouble) &&
                expiry->valuedouble >= 1 && expiry->valuedouble <= 2592000 &&
                floor(expiry->valuedouble) == expiry->valuedouble;
    bool new_refresh = json_text(root, "refresh_token", &refresh, sizeof(current->refresh));
    if (!is_refresh && !new_refresh) good = false;
    if (is_refresh && !new_refresh && !current->refresh[0]) good = false;
    if (good) {
        token_store_t next = *current;
        next.version = TOKEN_VERSION;
        strcpy(next.access, access);
        if (new_refresh) strcpy(next.refresh, refresh);
        next.expires_at = now_s() + (int64_t)expiry->valuedouble;
        /* Refresh rotations stay in RAM if flash fails. The worker retries
         * persistence before another network request. Authorization exchanges
         * are staged separately so a failed manual sign-in cannot replace a
         * working account. */
        *current = next;
        if (persist) {
            s_token_dirty = !save_token(&next);
            if (s_token_dirty) ESP_LOGE(TAG, "New OAuth credential pending NVS persistence");
        }
        memset(&next, 0, sizeof(next));
    }
    cJSON_Delete(root);
    return good;
}

static bool invalid_grant(const response_t *r)
{
    cJSON *root = cJSON_Parse(r->body);
    if (!root) return false;
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    bool invalid = cJSON_IsString(error) && strcmp(error->valuestring, "invalid_grant") == 0;
    cJSON_Delete(root);
    return invalid;
}

typedef enum { REFRESH_OK, REFRESH_RECONNECT, REFRESH_RETRY } refresh_result_t;

static refresh_result_t refresh(token_store_t *t, int *retry_after)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) return REFRESH_RETRY;
    cJSON_AddStringToObject(body, "grant_type", "refresh_token");
    cJSON_AddStringToObject(body, "refresh_token", t->refresh);
    cJSON_AddStringToObject(body, "client_id", CLIENT_ID);
    char *post = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post) return REFRESH_RETRY;
    response_t *r = calloc(1, sizeof(*r));
    if (!r) { memset(post, 0, strlen(post)); free(post); return REFRESH_RETRY; }
    int status = request_json(REFRESH_URL, post, NULL, r);
    memset(post, 0, strlen(post)); free(post);
    *retry_after = r->retry_after;
    refresh_result_t result = REFRESH_RETRY;
    if (status == 200 && parse_token(r, t, true, true)) {
        ESP_LOGI(TAG, "OAuth refresh succeeded at %lld; new expiry %lld", (long long)now_s(),
                 (long long)t->expires_at);
        result = REFRESH_OK;
    } else if (status == 400 && invalid_grant(r)) {
        if (clear_token()) {
            ESP_LOGW(TAG, "OAuth refresh invalid_grant; account setup required");
            reset_claude_usage(false, "Sign in again");
            claude_status("OAuth error", "Sign in again");
            result = REFRESH_RECONNECT;
        } else {
            ESP_LOGE(TAG, "Could not clear invalid OAuth credential");
        }
    }
    memset(r, 0, sizeof(*r)); free(r);
    return result;
}

static bool exchange_code(const char *code, const char *state, const char *verifier, token_store_t *t)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) return false;
    cJSON_AddStringToObject(body, "grant_type", "authorization_code");
    cJSON_AddStringToObject(body, "client_id", CLIENT_ID);
    cJSON_AddStringToObject(body, "code", code);
    cJSON_AddStringToObject(body, "redirect_uri", REDIRECT_URI);
    cJSON_AddStringToObject(body, "code_verifier", verifier);
    cJSON_AddStringToObject(body, "state", state);
    char *post = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post) return false;
    response_t *r = calloc(1, sizeof(*r));
    if (!r) { memset(post, 0, strlen(post)); free(post); return false; }
    int status = request_json(CODE_URL, post, NULL, r);
    memset(post, 0, strlen(post)); free(post);
    bool success = status == 200 && parse_token(r, t, false, false);
    if (!success) ESP_LOGW(TAG, "Authorization exchange failed: HTTP %d", status);
    memset(r, 0, sizeof(*r)); free(r);
    return success;
}

static float utilization(cJSON *root, const char *key, char *reset, size_t reset_size)
{
    reset[0] = 0;
    cJSON *period = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(period)) return -1;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(period, "utilization");
    cJSON *date = cJSON_GetObjectItemCaseSensitive(period, "resets_at");
    if (cJSON_IsString(date) && date->valuestring &&
        strnlen(date->valuestring, reset_size) < reset_size) strcpy(reset, date->valuestring);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > 100) return -1;
    return (float)value->valuedouble;
}

static int poll_usage(token_store_t *t, int *retry_after)
{
    response_t *r = calloc(1, sizeof(*r));
    if (!r) return -1;
    int status = request_json(USAGE_URL, NULL, t->access, r);
    *retry_after = r->retry_after;
    if (status == 200 && !wifi_up()) status = -1;
    if (status == 200) {
        cJSON *root = cJSON_Parse(r->body);
        if (root && cJSON_IsObject(root)) {
            char five_reset[64], week_reset[64];
            float five = utilization(root, "five_hour", five_reset, sizeof(five_reset));
            float week = utilization(root, "seven_day", week_reset, sizeof(week_reset));
            if (wifi_up() && !wifi_manager_is_provisioning()) {
                provider_usage_t usage = {0};
                usage.configured = true;
                usage.has_sample = true;
                usage.sampled_at = now_s();
                usage.sampled_us = esp_timer_get_time();
                float used[2] = {five, week};
                const char *resets[2] = {five_reset, week_reset};
                const uint32_t durations[2] = {18000, 604800};
                for (unsigned i = 0; i < 2; ++i) {
                    quota_window_t *window = &usage.windows[i];
                    window->valid = isfinite(used[i]) && used[i] >= 0 && used[i] <= 100;
                    window->used_percent = used[i];
                    window->window_seconds = durations[i];
                    if (!display_parse_reset(resets[i], &window->resets_at))
                        window->resets_at = 0;
                }
                publish_claude_usage(&usage);
                ESP_LOGI(TAG, "Usage response accepted: five=%s week=%s", five < 0 ? "unknown" : "known",
                         week < 0 ? "unknown" : "known");
            } else status = -1;
        } else status = -1;
        cJSON_Delete(root);
    }
    memset(r, 0, sizeof(*r)); free(r);
    return status;
}

static bool cap_matches(const char *uri, uint32_t *session)
{
    bool valid = false;
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    if (s_reset_nonce_deadline_us && esp_timer_get_time() >= s_reset_nonce_deadline_us) {
        memset(s_reset_nonce, 0, sizeof(s_reset_nonce));
        s_reset_nonce_session = 0;
        s_reset_nonce_deadline_us = 0;
    }
    if (s_cap_valid && now_s() < s_cap_deadline) {
        char expected[64];
        snprintf(expected, sizeof(expected), "/s/%s", s_cap);
        valid = strcmp(uri, expected) == 0;
        if (valid && session) *session = s_session;
    }
    xSemaphoreGive(s_cap_lock);
    return valid;
}

static void expire_reset_confirmation(void)
{
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    if (s_reset_nonce_deadline_us && esp_timer_get_time() >= s_reset_nonce_deadline_us) {
        memset(s_reset_nonce, 0, sizeof(s_reset_nonce));
        s_reset_nonce_session = 0;
        s_reset_nonce_deadline_us = 0;
    }
    xSemaphoreGive(s_cap_lock);
}

static void set_page_state(const char *state);

static esp_err_t setup_get(httpd_req_t *req)
{
    if (!cap_matches(req->uri, NULL)) return httpd_resp_send_404(req);
    char auth[sizeof(s_auth_url)];
    const char *state;
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    strcpy(auth, s_auth_url);
    state = s_page_state;
    xSemaphoreGive(s_cap_lock);
    /* The identity is shown as HTML text only after checking its fixed shape. */
    const char *device_name = device_identity_name();
    if (!device_name || strlen(device_name) != 14 ||
        strncmp(device_name, "hello-box-", 10) != 0)
        device_name = "Device";
    else for (int i = 10; i < 14; ++i) {
        if (device_name[i] < '0' || device_name[i] > '9') {
            device_name = "Device";
            break;
        }
    }
    char *page = malloc(SETUP_PAGE_CAPACITY);
    if (!page) return httpd_resp_send_500(req);
    int n = snprintf(page, SETUP_PAGE_CAPACITY,
        "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='referrer' content='no-referrer'><title>Settings</title>"
        "<style>:root{color-scheme:dark}*{box-sizing:border-box}"
        "body{margin:0;background:#18191b;color:#f2eee8;font:16px/1.5 system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
        "main{width:min(100%%,35rem);margin:0 auto;padding:2rem 1.25rem 3rem}"
        "header{padding-bottom:1.75rem}h1{font-size:2.25rem;line-height:1.08;letter-spacing:-.035em;margin:1.25rem 0 0;font-weight:700}"
        ".device-name{display:inline-block;margin:0;color:#d4cdc5;border:1px solid #45413d;border-radius:.4rem;padding:.32rem .65rem;font-size:.8rem;font-weight:600;letter-spacing:.01em}"
        "section{border-top:1px solid #3b3b3c;padding:1.7rem 0 2rem}h2{font-size:1.35rem;line-height:1.2;letter-spacing:-.02em;margin:0 0 .65rem}"
        "p{margin:.35rem 0 1rem;color:#bdb7b0}ol{list-style:none;counter-reset:step;margin:1.25rem 0 0;padding:0}"
        "li{counter-increment:step;position:relative;padding-left:2rem;margin:0 0 1.5rem}"
        "li:before{content:counter(step);position:absolute;left:0;top:.05rem;color:#d97757;font-weight:700}"
        ".step-title{display:block;color:#f2eee8;font-weight:650;margin-bottom:.3rem}"
        "a,button{font:inherit;font-weight:650;min-height:44px;border-radius:.5rem;padding:.68rem 1rem;display:inline-flex;align-items:center;justify-content:center;text-align:center;text-decoration:none;cursor:pointer}"
        "a,.primary{background:#d97757;color:#201915;border:1px solid #d97757}"
        "button{background:#292a2b;color:#f2eee8;border:1px solid #85807a}"
        "a:hover,.primary:hover{background:#e48a6b;border-color:#e48a6b}button:hover{background:#343435}"
        ":focus-visible{outline:3px solid #efa284;outline-offset:3px}"
        "label{display:block;color:#f2eee8;font-weight:650;margin:.85rem 0 .45rem}"
        "textarea{display:block;width:100%%;min-height:6rem;resize:vertical;background:#242527;color:#f2eee8;border:1px solid #706b66;border-radius:.5rem;padding:.75rem;font:inherit;margin:0 0 .8rem}"
        "#result{min-height:1.5rem;margin:1rem 0 0;color:#d4cdc5;font-size:.9rem;overflow-wrap:anywhere}"
        ".danger-zone{margin-top:.25rem;padding-top:1.5rem}.danger-zone h2{color:#d4cdc5}"
        ".danger-zone button{color:#edaa96;border-color:#8d6253;background:transparent}"
        "footer{border-top:1px solid #3b3b3c;padding-top:1.2rem;color:#a8a29b;font-size:.8rem}"
        "dialog{width:min(28rem,calc(100%% - 2rem));max-height:calc(100%% - 2rem);overflow:auto;background:#242527;color:#f2eee8;border:1px solid #706b66;border-radius:.7rem;padding:1.5rem}"
        "dialog h2{margin-bottom:.8rem}dialog::backdrop{background:#000b}.actions{display:flex;gap:.7rem;flex-wrap:wrap;margin-top:1.5rem}"
        ".actions button{flex:1 1 8rem}.actions .danger{background:#d97757;color:#201915;border-color:#d97757}"
        "@media(max-width:360px){main{padding:1.5rem 1rem 2rem}h1{font-size:2rem}a,button{width:100%%}.actions button{width:auto}}"
        "</style></head><body><main><header><p class='device-name'>%s</p><h1>Settings</h1></header>"
        "<section aria-labelledby='account-title'><h2 id='account-title'>Claude account</h2>"
        "<p>Sign in with a code to connect or change the Claude account on this device.</p>"
        "<ol><li><span class='step-title'>Get a sign-in code</span>Open Claude sign-in in a new tab, sign in, then return here with the code."
        "<p><a href='%s' target='_blank' rel='noreferrer noopener'>Open Claude sign-in</a></p></li>"
        "<li><span class='step-title'>Enter the code here</span><span id='code-help'>Paste the full code, including #state.</span>"
        "<label for='code'>Sign-in code</label><textarea id='code' autocomplete='off' spellcheck='false' aria-describedby='code-help'></textarea>"
        "<button id='send' class='primary' type='button'>Save Claude sign-in</button></li></ol>"
        "<p id='result' role='status' aria-live='polite'>%s</p></section>"
        "<section aria-labelledby='wifi-title'><h2 id='wifi-title'>Wi-Fi</h2>"
        "<p>Connect to a different network by forgetting the saved Wi-Fi. Your Claude sign-in stays saved. "
        "The device will show a setup QR code.</p>"
        "<button id='forget' type='button'>Forget Wi-Fi and reconnect</button></section>"
        "<section class='danger-zone' aria-labelledby='reset-section-title'><h2 id='reset-section-title'>Factory reset</h2>"
        "<p>Clear all saved settings and restart into fresh setup.</p>"
        "<button id='prepare-reset' type='button'>Reset all settings</button></section>"
        "<dialog id='reset-dialog' aria-labelledby='reset-title'><h2 id='reset-title'>Reset all settings?</h2>"
        "<p>This cannot be undone. Your Claude sign-in, saved Wi-Fi, and device settings will be erased. "
        "The saved device name will be cleared and a new name generated during setup.</p>"
        "<div class='actions'><button id='cancel-reset' type='button' autofocus>Cancel</button>"
        "<button id='confirm-reset' class='danger' type='button'>Reset everything</button></div></dialog>"
        "<footer>This page is available on your local Wi-Fi for ten minutes.</footer></main>"
        "<script>function resultAfter(id){let r=document.getElementById('result');"
        "document.getElementById(id).insertAdjacentElement('afterend',r);return r}"
        "document.getElementById('send').onclick=async()=>{let b=document.getElementById('code');"
        "let r=resultAfter('send');r.textContent='Sending...';try{let x=await fetch(location.pathname,{method:'POST',"
        "headers:{'Content-Type':'text/plain'},body:b.value});b.value='';r.textContent=await x.text()}"
        "catch(e){r.textContent='Connection failed'}};"
        "document.getElementById('forget').onclick=async()=>{let r=resultAfter('forget');"
        "r.textContent='Starting Wi-Fi setup...';try{let x=await fetch(location.pathname,{method:'POST',"
        "headers:{'X-Setup-Action':'forget-wifi'}});r.textContent=await x.text()}"
        "catch(e){r.textContent='The device is switching to Wi-Fi setup. Check its screen.'}};"
        "let resetNonce='';const resetDialog=document.getElementById('reset-dialog');"
        "const resetResult=document.getElementById('result');"
        "async function cancelReset(){resetNonce='';try{await fetch(location.pathname,{method:'POST',"
        "headers:{'X-Setup-Action':'cancel-reset'}})}catch(e){}resetDialog.close()}"
        "document.getElementById('prepare-reset').onclick=async()=>{resultAfter('prepare-reset');resetResult.textContent='Preparing reset...';"
        "try{let x=await fetch(location.pathname,{method:'POST',headers:{'X-Setup-Action':'prepare-reset'}});"
        "if(!x.ok){resetResult.textContent=await x.text();return}resetNonce=await x.text();resetDialog.showModal();"
        "document.getElementById('cancel-reset').focus();"
        "resetResult.textContent='Reset confirmation required'}catch(e){resetResult.textContent='Could not prepare reset'}};"
        "document.getElementById('cancel-reset').onclick=cancelReset;"
        "resetDialog.addEventListener('cancel',e=>{e.preventDefault();cancelReset()});"
        "document.getElementById('confirm-reset').onclick=async()=>{if(!resetNonce)return;"
        "let nonce=resetNonce;resetNonce='';resetDialog.close();resultAfter('prepare-reset');resetResult.textContent='Requesting reset...';"
        "try{let x=await fetch(location.pathname,{method:'POST',headers:{'X-Setup-Action':'factory-reset',"
        "'X-Reset-Confirmation':nonce}});resetResult.textContent=await x.text();"
        "if(x.ok){let poll=setInterval(async()=>{try{let y=await fetch(location.pathname,{method:'POST',"
        "headers:{'X-Setup-Action':'reset-status'}});if(y.ok){let message=await y.text();"
        "if(message.startsWith('Reset failed')){resetResult.textContent=message;clearInterval(poll)}}}"
        "catch(e){clearInterval(poll)}},1000)}}catch(e){resetResult.textContent='Connection lost; check the device'}};"
        "</script></body></html>", device_name, auth, state);
    if (n < 0 || n >= SETUP_PAGE_CAPACITY) { free(page); return httpd_resp_send_500(req); }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'");
    esp_err_t sent = httpd_resp_send(req, page, n);
    free(page);
    return sent;
}

static esp_err_t setup_post(httpd_req_t *req)
{
    uint32_t session = 0;
    if (!cap_matches(req->uri, &session)) return httpd_resp_send_404(req);
    size_t action_len = httpd_req_get_hdr_value_len(req, "X-Setup-Action");
    if (action_len) {
        char action[24];
        if (action_len >= sizeof(action) ||
            httpd_req_get_hdr_value_str(req, "X-Setup-Action", action, sizeof(action)) != ESP_OK ||
            req->content_len != 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "Invalid action");
        }
        if (strcmp(action, "reset-status") == 0) {
            const char *status;
            xSemaphoreTake(s_cap_lock, portMAX_DELAY);
            status = s_page_state;
            xSemaphoreGive(s_cap_lock);
            return httpd_resp_sendstr(req, status);
        }
        if (strcmp(action, "cancel-reset") == 0) {
            xSemaphoreTake(s_cap_lock, portMAX_DELAY);
            if (s_reset_nonce_session == session) {
                memset(s_reset_nonce, 0, sizeof(s_reset_nonce));
                s_reset_nonce_deadline_us = 0;
            }
            xSemaphoreGive(s_cap_lock);
            return httpd_resp_sendstr(req, "Reset cancelled");
        }
        if (strcmp(action, "prepare-reset") == 0) {
            char nonce[48];
            if (!encode_random(nonce, sizeof(nonce), 24)) return httpd_resp_send_500(req);
            xSemaphoreTake(s_cap_lock, portMAX_DELAY);
            bool allowed = s_cap_valid && s_session == session && s_manual_session &&
                           !atomic_load(&s_reset_inflight) &&
                           !atomic_load(&s_forget_response_ready) &&
                           !atomic_load(&s_reset_committed);
            uint32_t request_id = s_manual_request_id;
            if (allowed) {
                strcpy(s_reset_nonce, nonce);
                s_reset_nonce_session = session;
                s_reset_nonce_deadline_us = esp_timer_get_time() + RESET_CONFIRM_LIFETIME_US;
            }
            xSemaphoreGive(s_cap_lock);
            if (!allowed || !settings_wanted(request_id)) {
                httpd_resp_set_status(req, "409 Conflict");
                return httpd_resp_sendstr(req, "Open Settings on the device to reset it");
            }
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            esp_err_t sent = httpd_resp_sendstr(req, nonce);
            if (sent != ESP_OK) {
                xSemaphoreTake(s_cap_lock, portMAX_DELAY);
                if (s_reset_nonce_session == session && strcmp(s_reset_nonce, nonce) == 0)
                    memset(s_reset_nonce, 0, sizeof(s_reset_nonce));
                xSemaphoreGive(s_cap_lock);
            }
            memset(nonce, 0, sizeof(nonce));
            return sent;
        }
        if (strcmp(action, "factory-reset") == 0) {
            char supplied[48];
            size_t nonce_len = httpd_req_get_hdr_value_len(req, "X-Reset-Confirmation");
            if (nonce_len != 32 ||
                httpd_req_get_hdr_value_str(req, "X-Reset-Confirmation", supplied,
                                             sizeof(supplied)) != ESP_OK) {
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_sendstr(req, "Reset confirmation is missing");
            }
            reset_command_t command = {.session = session};
            xSemaphoreTake(s_cap_lock, portMAX_DELAY);
            bool confirmed = s_cap_valid && s_session == session && s_manual_session &&
                             s_reset_nonce_session == session && s_reset_nonce_deadline_us > esp_timer_get_time() &&
                             strcmp(s_reset_nonce, supplied) == 0 &&
                             !atomic_load(&s_reset_inflight) &&
                             !atomic_load(&s_forget_response_ready) &&
                             !atomic_load(&s_reset_committed);
            if (confirmed) {
                command.request_id = s_manual_request_id;
                command.deadline_us = s_reset_nonce_deadline_us;
                strcpy(command.cap, s_cap);
                memset(s_reset_nonce, 0, sizeof(s_reset_nonce));
                s_reset_nonce_deadline_us = 0;
            }
            xSemaphoreGive(s_cap_lock);
            memset(supplied, 0, sizeof(supplied));
            if (!confirmed || !settings_wanted(command.request_id)) {
                httpd_resp_set_status(req, "409 Conflict");
                return httpd_resp_sendstr(req, "Reset confirmation expired; start again");
            }
            bool expected = false;
            if (!atomic_compare_exchange_strong(&s_reset_inflight, &expected, true)) {
                httpd_resp_set_status(req, "429 Too Many Requests");
                return httpd_resp_sendstr(req, "Please wait");
            }
            esp_err_t sent = httpd_resp_sendstr(req, "Reset requested. The device will restart into fresh setup.");
            if (sent == ESP_OK && xQueueSend(s_reset_commands, &command, 0) == pdTRUE) {
                atomic_store(&s_reset_response_ready, true);
            } else {
                atomic_store(&s_reset_inflight, false);
                if (sent == ESP_OK) {
                    set_page_state("Reset failed; request could not be queued. You can try again.");
                    ESP_LOGE(TAG, "Factory reset request could not be queued");
                }
            }
            return sent;
        }
        if (strcmp(action, "forget-wifi") != 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "Invalid action");
        }
        if (atomic_load(&s_reset_inflight) ||
            xQueueSend(s_forget_commands, &session, 0) != pdTRUE) {
            httpd_resp_set_status(req, "429 Too Many Requests");
            return httpd_resp_sendstr(req, "Please wait");
        }
        esp_err_t sent = httpd_resp_sendstr(req, "Wi-Fi setup is starting. Your Claude sign-in remains saved. Check the device QR code.");
        if (sent == ESP_OK) atomic_store(&s_forget_response_ready, true);
        else xQueueReset(s_forget_commands);
        return sent;
    }
    if (atomic_load(&s_reset_inflight) || atomic_load(&s_reset_committed)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Reset is pending");
    }
    if (req->content_len < 1 || req->content_len > 512) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "Code is missing or too long");
    }
    submission_t item = {.session = session};
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, item.code + received, req->content_len - received);
        if (n <= 0) { memset(&item, 0, sizeof(item)); return ESP_FAIL; }
        received += n;
    }
    item.code[received] = 0;
    if (xQueueSend(s_submissions, &item, 0) != pdTRUE) {
        memset(&item, 0, sizeof(item));
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, "Please wait");
    }
    memset(&item, 0, sizeof(item));
    return httpd_resp_sendstr(req, "Code received. Check the display for status.");
}

static void stop_setup(void)
{
    bool manual = s_manual_session;
    uint32_t request_id = s_manual_request_id;
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    s_cap_valid = false;
    s_cap[0] = 0;
    s_auth_url[0] = 0;
    memset(s_reset_nonce, 0, sizeof(s_reset_nonce));
    s_reset_nonce_session = 0;
    s_reset_nonce_deadline_us = 0;
    s_manual_session = false;
    s_manual_request_id = 0;
    xSemaphoreGive(s_cap_lock);
    s_setup_url[0] = 0;
    s_auto_qr_visible = false;
    if (s_server) {
        device_discovery_http_stop();
        httpd_stop(s_server);
        s_server = NULL;
    }
    xQueueReset(s_submissions);
    if (manual) display_settings_closed(request_id);
}

static void show_auto_setup(const char *url)
{
    /* An unconfigured Claude account must not cover a working Codex Home. */
    s_auto_qr_visible = claude_selected() && !codex_selected();
    if (s_auto_qr_visible) display_setup(url);
}

static bool start_setup(char *state, char *verifier, bool manual, uint32_t request_id)
{
    if (wifi_manager_is_provisioning()) return false;
    char challenge[48];
    char cap[48];
    if (!encode_random(verifier, 80, 32) || !encode_random(state, 48, 24) ||
        !encode_random(cap, sizeof(cap), 24) || !pkce_challenge(verifier, challenge, sizeof(challenge)))
        return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    char url[128];
    char ipv4[16];
    int ip_len = snprintf(ipv4, sizeof(ipv4), IPSTR, IP2STR(&ip.ip));
    if (ip_len < 0 || (size_t)ip_len >= sizeof(ipv4) ||
        !device_discovery_setup_url(url, sizeof(url), "s", cap, ipv4)) return false;
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    ++s_session;
    if (!s_session) ++s_session;
    strcpy(s_cap, cap);
    int n = snprintf(s_auth_url, sizeof(s_auth_url),
        AUTH_URL "?code=true&client_id=" CLIENT_ID "&response_type=code&redirect_uri="
        "https%%3A%%2F%%2Fplatform.claude.com%%2Foauth%%2Fcode%%2Fcallback"
        "&scope=user%%3Aprofile%%20user%%3Ainference&code_challenge=%s&code_challenge_method=S256&state=%s",
        challenge, state);
    s_cap_deadline = now_s() + CAP_LIFETIME_SEC;
    s_page_state = "Ready";
    s_cap_valid = n > 0 && n < sizeof(s_auth_url);
    xSemaphoreGive(s_cap_lock);
    if (!s_cap_valid) return false;
    if (wifi_manager_is_provisioning()) {
        stop_setup();
        return false;
    }
    if (!s_server) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = CONFIG_OAUTH_PROBE_HTTP_PORT;
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.stack_size = 6144;
        if (httpd_start(&s_server, &config) != ESP_OK) return false;
        httpd_uri_t get = {.uri = "/s/*", .method = HTTP_GET, .handler = setup_get};
        httpd_uri_t post = {.uri = "/s/*", .method = HTTP_POST, .handler = setup_post};
        if (httpd_register_uri_handler(s_server, &get) != ESP_OK ||
            httpd_register_uri_handler(s_server, &post) != ESP_OK) {
            stop_setup();
            return false;
        }
        esp_err_t discovery_err = device_discovery_http_start(config.server_port);
        if (discovery_err != ESP_OK)
            ESP_LOGW(TAG, "HTTP discovery unavailable: %s", esp_err_to_name(discovery_err));
    }
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    s_manual_session = manual;
    s_manual_request_id = manual ? request_id : 0;
    xSemaphoreGive(s_cap_lock);
    if (manual) {
        if (!settings_wanted(request_id)) {
            stop_setup();
            return false;
        }
        display_settings_qr(request_id, url);
        s_auto_qr_visible = false;
    } else {
        show_auto_setup(url);
    }
    (void)device_discovery_update_url(s_setup_url, sizeof(s_setup_url), url);
    ESP_LOGI(TAG, "Account setup available for 600 seconds");
    return true;
}

static void refresh_setup_url(bool have_token)
{
    if (!s_server || wifi_manager_is_provisioning() || !wifi_up()) return;
    char cap[sizeof(s_cap)];
    bool manual;
    uint32_t request_id;
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    bool active = s_cap_valid && now_s() < s_cap_deadline;
    if (active) memcpy(cap, s_cap, sizeof(cap));
    manual = s_manual_session;
    request_id = s_manual_request_id;
    xSemaphoreGive(s_cap_lock);
    if (!active || (manual ? !settings_wanted(request_id) :
                    (have_token || !s_auto_qr_visible))) return;

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta, &ip) != ESP_OK || ip.ip.addr == 0) return;
    char ipv4[16];
    int ip_len = snprintf(ipv4, sizeof(ipv4), IPSTR, IP2STR(&ip.ip));
    if (ip_len < 0 || (size_t)ip_len >= sizeof(ipv4)) return;
    char url[sizeof(s_setup_url)];
    if (!device_discovery_setup_url(url, sizeof(url), "s", cap, ipv4) ||
        !device_discovery_update_url(s_setup_url, sizeof(s_setup_url), url)) return;
    if (manual) {
        if (settings_wanted(request_id)) display_settings_qr(request_id, s_setup_url);
    } else if (!have_token && s_auto_qr_visible) {
        display_setup(s_setup_url);
    }
}

static void set_page_state(const char *state)
{
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    s_page_state = state;
    xSemaphoreGive(s_cap_lock);
}

static void process_settings(bool have_token, char *state, char *verifier)
{
    settings_command_t command;
    if (xQueueReceive(s_settings_commands, &command, 0) != pdTRUE) return;
    if (!command.open) {
        if (s_manual_session && command.request_id == s_manual_request_id) {
            stop_setup();
            memset(state, 0, 48);
            memset(verifier, 0, 80);
        }
        return;
    }
    if (!settings_wanted(command.request_id)) return;
    if (s_manual_session) stop_setup();
    if (!have_token || !wifi_up() || !clock_valid() ||
        wifi_manager_is_provisioning() || atomic_load(&s_usb_session_active)) {
        display_settings_closed(command.request_id);
        return;
    }
    if (!start_setup(state, verifier, true, command.request_id) && settings_wanted(command.request_id))
        display_settings_closed(command.request_id);
}

static void process_forget(char *state, char *verifier)
{
    if (!atomic_load(&s_forget_response_ready)) return;
    uint32_t session;
    if (xQueueReceive(s_forget_commands, &session, 0) != pdTRUE) return;
    atomic_store(&s_forget_response_ready, false);
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    bool current = s_cap_valid && s_session == session;
    xSemaphoreGive(s_cap_lock);
    if (!current) return;
    stop_setup();
    memset(state, 0, 48);
    memset(verifier, 0, 80);
    if (!wifi_manager_request_forget())
        ESP_LOGW(TAG, "Wi-Fi reset request could not be queued");
}

/* Runs only on the OAuth worker, after the final HTTP response was sent. The
 * journal commit is the point of no return; nothing sensitive changes before
 * that call succeeds. */
static void process_factory_reset(token_store_t *token, char *state, char *verifier)
{
    if (!atomic_load(&s_reset_response_ready)) return;
    reset_command_t command;
    if (xQueueReceive(s_reset_commands, &command, 0) != pdTRUE) return;
    atomic_store(&s_reset_response_ready, false);
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    bool current = s_cap_valid && now_s() < s_cap_deadline &&
                   s_manual_session && s_session == command.session &&
                   s_manual_request_id == command.request_id &&
                   strcmp(s_cap, command.cap) == 0 &&
                   esp_timer_get_time() < command.deadline_us;
    xSemaphoreGive(s_cap_lock);
    if (!current || !settings_wanted(command.request_id)) {
        atomic_store(&s_reset_inflight, false);
        return;
    }
    if (!reserve_settings_commit(command.request_id)) {
        atomic_store(&s_reset_inflight, false);
        return;
    }
    /* No lock is held while NVS commits. Back is rejected while committed. */
    esp_err_t err = factory_reset_request();
    if (err != ESP_OK) {
        finish_settings_commit();
        atomic_store(&s_reset_inflight, false);
        set_page_state("Reset failed; settings were not changed. You can try again.");
        display_status("Reset failed", "Settings unchanged");
        ESP_LOGE(TAG, "Factory reset journal commit failed: %s", esp_err_to_name(err));
        return;
    }
    atomic_store(&s_reset_committed, true);
    publish_refresh(false, 0);
    stop_setup();
    memset(token, 0, sizeof(*token));
    s_token_dirty = false;
    memset(state, 0, 48);
    memset(verifier, 0, 80);
    memset(&command, 0, sizeof(command));
    esp_restart();
    for (;;) vTaskDelay(portMAX_DELAY);
}

static void usb_oauth_clear(bool configured)
{
    atomic_store(&s_usb_session_active, false);
    memset(&s_usb_oauth, 0, sizeof(s_usb_oauth));
    management_account(configured, configured ? "configured" : "not_configured", "", "");
}

static void management_network_test(void)
{
    bool connected = wifi_up() && !wifi_manager_is_provisioning();
    bool clock = clock_valid();
    bool has_ip = false, dns = false, https = false;
    esp_netif_ip_info_t ip = {0};
    has_ip = connected && s_sta && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK && ip.ip.addr;
    const char *message = !connected ? "Wi-Fi is offline" :
                          !has_ip ? "Waiting for an IP address" :
                          !clock ? "Waiting for a valid clock" : "Network test failed";
    if (connected && has_ip && clock) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
        struct addrinfo *result = NULL;
        dns = getaddrinfo("api.anthropic.com", "443", &hints, &result) == 0 && result;
        if (result) freeaddrinfo(result);
        if (dns) {
            response_t *response = calloc(1, sizeof(*response));
            if (response) {
                /* A non-2xx HTTP response still proves DNS and certificate-
                 * verified TLS to the fixed endpoint, without any token. */
                int status = request_json("https://api.anthropic.com/", NULL, NULL, response);
                https = status >= 100 && status <= 599;
                memset(response, 0, sizeof(*response));
                free(response);
            }
        }
        message = !dns ? "DNS lookup failed" :
                  !https ? "Verified HTTPS failed" : "DNS and verified HTTPS reachable";
    }
    if (!wifi_up() || wifi_manager_is_provisioning()) {
        connected = false;
        has_ip = false;
        dns = false;
        https = false;
        message = "Wi-Fi disconnected during test";
    }
    taskENTER_CRITICAL(&s_management_mux);
    s_management.network.wifi = connected;
    s_management.network.ip = has_ip;
    s_management.network.clock = clock;
    s_management.network.dns = dns;
    s_management.network.https = https;
    snprintf(s_management.network.state, sizeof(s_management.network.state),
             "%s", https ? "online" : "failed");
    snprintf(s_management.network.message, sizeof(s_management.network.message), "%s", message);
    taskEXIT_CRITICAL(&s_management_mux);
}

static void management_poll_wifi(void)
{
    if (!s_management_wifi_id) return;
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.operation_id == s_management_wifi_id &&
        ((!strcmp(wifi.state, "connected") && !wifi.provisioning) ||
         !strcmp(wifi.state, "failed"))) {
        management_operation(s_management_wifi_id, "wifi.connect",
                             !strcmp(wifi.state, "connected") ? "done" : "failed",
                             wifi.message[0] ? wifi.message :
                             !strcmp(wifi.state, "connected") ? "Wi-Fi connected and saved" :
                             "Wi-Fi connection failed");
        s_management_wifi_id = 0;
    }
}

static void management_poll_codex(void)
{
    codex_management_status_t codex = {0};
    codex_adapter_get_status(&codex);
    if (!s_management_codex_id) {
        /* Issuing the code completes the begin request, but authorization can
         * fail later. Report that failure while this is still the latest USB
         * operation, without replacing a newer request's result. */
        taskENTER_CRITICAL(&s_management_mux);
        bool latest_begin = codex.operation.id &&
            codex.operation.id == s_management.operation.id &&
            !strcmp(s_management.operation.method, "codex.begin") &&
            !strcmp(s_management.operation.state, "done") &&
            !strcmp(codex.operation.method, "codex.begin") &&
            !strcmp(codex.operation.state, "error");
        taskEXIT_CRITICAL(&s_management_mux);
        if (latest_begin)
            management_operation(codex.operation.id, "codex.begin", "failed",
                                 codex.operation.message);
        return;
    }
    if (codex.operation.id != s_management_codex_id ||
        (!strcmp(codex.operation.state, "accepted") ||
         !strcmp(codex.operation.state, "queued") ||
         !strcmp(codex.operation.state, "running"))) return;
    management_operation(s_management_codex_id, codex.operation.method,
                         !strcmp(codex.operation.state, "error") ? "failed" :
                         codex.operation.state, codex.operation.message);
    s_management_codex_id = 0;
}

static void process_management(token_store_t *token, bool *have_token,
                               char *state, char *verifier,
                               int64_t *next_usage, int64_t *next_refresh,
                               int64_t *usage_retry_until, int *failures,
                               bool *first_usage)
{
    management_command_t cmd = {0};
    if (!s_management_commands || xQueueReceive(s_management_commands, &cmd, 0) != pdTRUE) return;
    management_operation(cmd.id, cmd.method, "running", "Processing on device");
    bool done = false;
    const char *message = "Request failed";
    if (!strcmp(cmd.method, "wifi.connect")) {
        codex_management_status_t codex = {0};
        codex_adapter_get_status(&codex);
        if (s_manual_session || atomic_load(&s_usb_session_active) ||
            !strcmp(codex.state, "starting") || !strcmp(codex.state, "awaiting_code") ||
            !strcmp(codex.state, "verifying")) {
            message = "Close active account setup first";
        } else {
            if (s_server) {
                stop_setup();
                memset(state, 0, 48);
                memset(verifier, 0, 80);
            }
            done = wifi_manager_request_connect(cmd.id, cmd.arg1, cmd.arg2);
            if (done) {
                management_network_invalidate("Wi-Fi connection is changing");
                s_management_wifi_id = cmd.id;
                management_operation(cmd.id, cmd.method, "running",
                                     "Wi-Fi connection test in progress");
                memset(&cmd, 0, sizeof(cmd));
                return;
            }
            message = "Wi-Fi connection test unavailable";
        }
    } else if (!strcmp(cmd.method, "codex.begin") ||
               !strcmp(cmd.method, "codex.cancel")) {
        done = codex_adapter_request(cmd.id, cmd.method);
        if (done) {
            s_management_codex_id = cmd.id;
            management_operation(cmd.id, cmd.method, "running", "Codex request in progress");
            memset(&cmd, 0, sizeof(cmd));
            return;
        }
        message = "Codex setup is busy or unavailable";
    } else if (!strcmp(cmd.method, "network.test")) {
        management_network_test();
        oauth_management_status_t snapshot;
        oauth_probe_get_management_status(&snapshot);
        done = snapshot.network.https;
        message = snapshot.network.message;
        management_operation(cmd.id, cmd.method, done ? "done" : "failed", message);
        memset(&cmd, 0, sizeof(cmd));
        return;
    } else if (!strcmp(cmd.method, "claude.begin")) {
        bool gui_wanted;
        taskENTER_CRITICAL(&s_settings_mux);
        gui_wanted = s_settings_wanted_open || s_settings_committing;
        taskEXIT_CRITICAL(&s_settings_mux);
        if (s_manual_session || gui_wanted || atomic_load(&s_usb_session_active) ||
            s_token_dirty || wifi_manager_is_provisioning() || !wifi_up() || !clock_valid()) {
            message = "Claude setup is busy or network is not ready";
        } else {
            char challenge[48], url[sizeof(s_management.authorization_url)];
            if (s_server) {
                stop_setup();
                memset(state, 0, 48);
                memset(verifier, 0, 80);
            }
            done = encode_random(s_usb_oauth.verifier, sizeof(s_usb_oauth.verifier), 32) &&
                   encode_random(s_usb_oauth.state, sizeof(s_usb_oauth.state), 24) &&
                   pkce_challenge(s_usb_oauth.verifier, challenge, sizeof(challenge)) &&
                   authorization_url(url, sizeof(url), challenge, s_usb_oauth.state);
            if (done) {
                s_usb_oauth.deadline_us = esp_timer_get_time() + CAP_LIFETIME_SEC * 1000000LL;
                atomic_store(&s_usb_session_active, true);
                management_account(atomic_load(&s_token_configured), "awaiting_code",
                                   s_usb_oauth.state, url);
                claude_status("OAuth", "Complete sign-in in browser");
                message = "Open authorization URL in a new tab";
            } else {
                usb_oauth_clear(*have_token);
                message = "Could not start Claude sign-in";
            }
        }
    } else if (!strcmp(cmd.method, "claude.submit_code")) {
        if (!atomic_load(&s_usb_session_active) ||
            esp_timer_get_time() >= s_usb_oauth.deadline_us ||
            strcmp(cmd.arg1, s_usb_oauth.state) || !wifi_up() || !clock_valid() ||
            wifi_manager_is_provisioning()) {
            message = "Sign-in session expired or network unavailable";
        } else {
            char *code = NULL;
            if (!probe_authorization_code(cmd.arg2, s_usb_oauth.state, &code)) {
                message = "Code or state does not match";
            } else {
                token_store_t *candidate = calloc(1, sizeof(*candidate));
                bool exchanged = candidate && exchange_code(code, s_usb_oauth.state,
                                                             s_usb_oauth.verifier, candidate);
                if (exchanged && wifi_up() && !wifi_manager_is_provisioning() &&
                    esp_timer_get_time() < s_usb_oauth.deadline_us && save_token(candidate)) {
                    *token = *candidate;
                    *have_token = true;
                    atomic_store(&s_usage_validated, false);
                    s_token_dirty = false;
                    *next_usage = 0;
                    *next_refresh = 0;
                    *usage_retry_until = 0;
                    *failures = 0;
                    *first_usage = true;
                    reset_claude_usage(true, "Checking new account");
                    usb_oauth_clear(true);
                    done = true;
                    message = "Claude account saved; checking usage";
                } else message = exchanged ? "Could not save sign-in" : "Code exchange failed";
                if (candidate) { memset(candidate, 0, sizeof(*candidate)); free(candidate); }
            }
        }
    } else if (!strcmp(cmd.method, "claude.cancel")) {
        usb_oauth_clear(atomic_load(&s_token_configured));
        done = true;
        message = "Claude sign-in cancelled";
    }
    management_operation(cmd.id, cmd.method, done ? "done" : "failed", message);
    memset(&cmd, 0, sizeof(cmd));
}

static void worker(void *arg)
{
    (void)arg;
    token_store_t *token = calloc(1, sizeof(*token));
    if (!token) { claude_status("OAuth error", "Out of memory"); vTaskDelete(NULL); return; }
    bool have_token = load_token(token);
    bool clock_announced = false;
    bool first_usage = true;
    int64_t next_usage = 0, next_refresh = 0, usage_retry_until = 0;
    int failures = 0;
    char state[48] = {0}, verifier[80] = {0};
    management_account(have_token, have_token ? "configured" : "not_configured", "", "");
    provider_usage_t initial = {0};
    initial.configured = have_token;
    publish_claude_usage(&initial);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();
    for (;;) {
        if (atomic_load(&s_usb_session_active) &&
            esp_timer_get_time() >= s_usb_oauth.deadline_us)
            usb_oauth_clear(atomic_load(&s_token_configured));
        process_management(token, &have_token, state, verifier, &next_usage,
                           &next_refresh, &usage_retry_until, &failures, &first_usage);
        management_poll_wifi();
        management_poll_codex();
        expire_reset_confirmation();
        process_factory_reset(token, state, verifier);
        process_forget(state, verifier);
        process_settings(have_token, state, verifier);
        if (s_server && now_s() >= s_cap_deadline) {
            stop_setup();
            memset(state, 0, sizeof(state));
            memset(verifier, 0, sizeof(verifier));
        }
        if (wifi_manager_is_provisioning()) {
            publish_refresh(false, 0);
            if (s_server) stop_setup();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!wifi_up()) {
            publish_refresh(false, 0);
            if (s_server) stop_setup();
            clock_announced = false;
            display_status("Wi-Fi", "Waiting for connection");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        /* Remember every GOT_IP event, including reconnects shorter than this
         * worker's polling interval. Fetch immediately; animate at request start. */
        if (xEventGroupClearBits(s_wifi_events, OAUTH_WIFI_RECONNECTED) & OAUTH_WIFI_RECONNECTED) {
            next_usage = 0;
            management_network_invalidate("Network changed; run test again");
        }
        if (!clock_valid()) {
            publish_refresh(false, 0);
            s_auto_qr_visible = false;
            display_status("Time", "Waiting for clock");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!clock_announced) {
            setenv("TZ", box_config_timezone(), 1);
            tzset();
            display_clock_ready();
            ESP_LOGI(TAG, "SNTP time ready");
            clock_announced = true;
        }
        refresh_setup_url(have_token);
        if (have_token && s_token_dirty) {
            publish_refresh(false, 0);
            if (!save_token(token)) {
                claude_status("Storage error", "Saving sign-in; retrying");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            s_token_dirty = false;
            ESP_LOGI(TAG, "Pending OAuth credential persisted");
        }
        if (!have_token) {
            publish_refresh(false, 0);
            if (codex_selected() && !s_manual_session &&
                !atomic_load(&s_usb_session_active)) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
                continue;
            }
            if (atomic_load(&s_usb_session_active)) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
                continue;
            }
            if (!s_server || now_s() >= s_cap_deadline) {
                if (s_server) stop_setup();
                if (!start_setup(state, verifier, false, 0)) {
                    claude_status("Setup error", "Local setup page unavailable");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
            }
            submission_t item = {0};
            if (xQueueReceive(s_submissions, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {
                if (item.session != s_session || !s_cap_valid) {
                    memset(&item, 0, sizeof(item));
                    continue;
                }
                char *code = NULL;
                if (!probe_authorization_code(item.code, state, &code)) {
                    set_page_state("Paste the full code with the matching #state");
                    s_auto_qr_visible = false;
                    claude_status("OAuth error", "Code or state does not match");
                    memset(&item, 0, sizeof(item));
                    continue;
                }
                if (!wifi_up() || now_s() >= s_cap_deadline) {
                    set_page_state("Invalid code or setup page expired");
                } else {
                    s_auto_qr_visible = false;
                    claude_status("OAuth", "Checking code");
                    token_store_t *candidate = calloc(1, sizeof(*candidate));
                    bool valid = candidate && exchange_code(code, state, verifier, candidate);
                    if (valid && item.session == s_session && s_cap_valid &&
                        !wifi_manager_is_provisioning()) {
                        *token = *candidate;
                        s_token_dirty = !save_token(token);
                        if (s_token_dirty) ESP_LOGE(TAG, "New OAuth credential pending NVS persistence");
                        have_token = true;
                        atomic_store(&s_usage_validated, false);
                        reset_claude_usage(true, "Checking new account");
                    }
                    if (candidate) { memset(candidate, 0, sizeof(*candidate)); free(candidate); }
                    if (have_token) {
                        stop_setup();
                        claude_status("OAuth", s_token_dirty ? "Token received; saving" : "Token saved; checking usage");
                        memset(verifier, 0, sizeof(verifier));
                        memset(state, 0, sizeof(state));
                        first_usage = true;
                        next_usage = 0;
                        next_refresh = 0;
                        failures = 0;
                    } else {
                        set_page_state("Check failed; use a new sign-in code");
                        claude_status("OAuth error", "Code check failed");
                    }
                }
                memset(&item, 0, sizeof(item));
            }
            continue;
        }
        if (s_manual_session) {
            submission_t item = {0};
            if (xQueueReceive(s_submissions, &item, 0) == pdTRUE) {
                char *code = NULL;
                if (item.session == s_session && s_cap_valid &&
                    settings_wanted(s_manual_request_id)) {
                    if (!probe_authorization_code(item.code, state, &code)) {
                        set_page_state("Paste the full code with the matching #state");
                    } else if (now_s() < s_cap_deadline && wifi_up() &&
                               !wifi_manager_is_provisioning()) {
                        token_store_t *candidate = calloc(1, sizeof(*candidate));
                        bool valid = candidate && exchange_code(code, state, verifier, candidate);
                        if (valid && item.session == s_session && s_cap_valid &&
                            wifi_up() && !wifi_manager_is_provisioning() &&
                            reserve_settings_commit(s_manual_request_id)) {
                            bool saved = save_token(candidate);
                            if (saved) {
                                *token = *candidate;
                                s_token_dirty = false;
                                atomic_store(&s_usage_validated, false);
                                first_usage = true;
                                next_usage = 0;
                                usage_retry_until = 0;
                                next_refresh = 0;
                                failures = 0;
                                reset_claude_usage(true, "Checking new account");
                            }
                            finish_settings_commit();
                            if (saved) {
                                stop_setup();
                                memset(state, 0, sizeof(state));
                                memset(verifier, 0, sizeof(verifier));
                            } else {
                                set_page_state("Could not save sign-in; current account is unchanged");
                            }
                        } else if (settings_wanted(s_manual_request_id)) {
                            set_page_state("Check failed; use a new sign-in code");
                        }
                        if (candidate) { memset(candidate, 0, sizeof(*candidate)); free(candidate); }
                    } else {
                        set_page_state("Setup page expired; open Settings again");
                    }
                }
                memset(&item, 0, sizeof(item));
            }
        }
        if (!claude_selected()) {
            publish_refresh(false, 0);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        int64_t now = now_s();
        int64_t retry_gate = usage_retry_until;
        if (now >= token->expires_at - 60 && next_refresh > retry_gate)
            retry_gate = next_refresh;
        publish_refresh(true, retry_gate);
        bool refresh_due = now >= token->expires_at - 60 && now >= next_refresh;
        bool usage_due = refresh_control_usage_due(now, next_usage, usage_retry_until,
                                                   token->expires_at);
        bool manual = false;
        uint32_t cycle_generation = 0;
        bool cycle = begin_refresh(refresh_due || usage_due, &manual, &cycle_generation);
        if (manual) next_usage = 0;
        if (cycle && now >= token->expires_at - 60 && now >= next_refresh) {
            int retry_after = 0;
            claude_status("OAuth", "Refreshing token");
            ESP_LOGI(TAG, "REFRESH_TEST automatic attempt at %lld; expiry %lld",
                     (long long)now, (long long)token->expires_at);
            refresh_result_t result = refresh(token, &retry_after);
            if (result == REFRESH_RECONNECT) {
                if (s_manual_session) stop_setup();
                have_token = false;
                memset(token, 0, sizeof(*token));
                next_refresh = 0;
                end_refresh(false, 0, cycle_generation);
                continue;
            }
            if (result == REFRESH_OK) {
                next_refresh = 0;
                next_usage = 0;
                failures = 0;
            } else {
                next_refresh = now_s() + refresh_control_retry_delay(++failures, retry_after);
                if (next_usage < next_refresh) next_usage = next_refresh;
                claude_status("OAuth", "Token refresh failed; retrying");
            }
        }
        if (s_token_dirty) {
            if (cycle) {
                claude_status("Storage error", "Saving sign-in; retrying");
                end_refresh(false, 0, cycle_generation);
            }
            continue;
        }
        now = now_s();
        if (cycle && refresh_control_usage_due(now, next_usage, usage_retry_until,
                                               token->expires_at) && wifi_up()) {
            int retry_after = 0;
            int status = poll_usage(token, &retry_after);
            if (status == 401 && wifi_up()) {
                if (now_s() < next_refresh) {
                    next_usage = next_refresh;
                    usage_retry_until = next_refresh;
                    claude_status("Usage", "Retrying after cooldown");
                    end_refresh(true, next_refresh, cycle_generation);
                    continue;
                }
                ESP_LOGI(TAG, "REFRESH_TEST usage 401 attempt at %lld", (long long)now_s());
                refresh_result_t result = refresh(token, &retry_after);
                if (result == REFRESH_RECONNECT) {
                    if (s_manual_session) stop_setup();
                    have_token = false;
                    memset(token, 0, sizeof(*token));
                    next_refresh = 0;
                    end_refresh(false, 0, cycle_generation);
                    continue;
                }
                if (result == REFRESH_OK && !s_token_dirty) {
                    next_refresh = 0;
                    status = poll_usage(token, &retry_after);
                } else if (result == REFRESH_RETRY) {
                    retry_after = refresh_control_retry_delay(++failures, retry_after);
                    next_refresh = now_s() + retry_after;
                    status = 429; /* Apply the refresh response's retry deadline. */
                }
            }
            if (status == 200 && wifi_up()) {
                atomic_store(&s_usage_validated, true);
                if (first_usage) {
                    stop_setup();
                    first_usage = false;
                    ESP_LOGI(TAG, "First authenticated usage succeeded; setup disabled");
                }
                failures = 0;
                usage_retry_until = 0;
                next_usage = now_s() + box_config_current()->refresh_interval_sec;
            } else {
                if (wifi_up()) claude_status("Usage", "Usage check failed; retrying");
                else display_status("Wi-Fi", "Waiting for connection");
                next_usage = now_s() +
                             refresh_control_usage_retry_delay(++failures, status, retry_after);
                usage_retry_until = next_usage;
            }
        }
        if (cycle) {
            now = now_s();
            retry_gate = usage_retry_until;
            if (now >= token->expires_at - 60 && next_refresh > retry_gate)
                retry_gate = next_refresh;
            end_refresh(have_token && !s_token_dirty && wifi_up() && clock_valid(), retry_gate,
                        cycle_generation);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

void oauth_probe_start(EventGroupHandle_t wifi_events, esp_netif_t *sta)
{
    s_wifi_events = wifi_events;
    s_sta = sta;
    s_submissions = xQueueCreate(1, sizeof(submission_t));
    s_settings_commands = xQueueCreate(1, sizeof(settings_command_t));
    s_forget_commands = xQueueCreate(1, sizeof(uint32_t));
    s_reset_commands = xQueueCreate(1, sizeof(reset_command_t));
    s_management_commands = xQueueCreate(1, sizeof(management_command_t));
    s_cap_lock = xSemaphoreCreateMutex();
    if (!s_submissions || !s_settings_commands || !s_forget_commands || !s_reset_commands ||
        !s_management_commands || !s_cap_lock ||
        xTaskCreate(worker, "oauth_probe", 12288, NULL, 5, &s_worker_task) != pdPASS)
        claude_status("OAuth error", "Service could not start");
    else {
        display_set_refresh_callback(request_manual_refresh, NULL);
        display_set_app_refresh_callback(request_app_refresh, NULL);
        display_set_settings_callback(request_settings, NULL);
    }
}
