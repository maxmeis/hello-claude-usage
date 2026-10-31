#include "codex_adapter.h"
#include "app_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "codex_protocol.h"
#include "box_config.h"
#include "display.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "oauth_probe.h"
#include "probe_protocol.h"
#include "provider_http.h"
#include "wifi_manager.h"

#define AUTH_BASE "https://auth.openai.com"
#define USAGE_URL "https://chatgpt.com/backend-api/wham/usage"
#define PARTITION "quota_accounts"
#define NAMESPACE "codex_probe"
#define RECORD_MAGIC 0x43445831u
#define RESPONSE_LIMIT 16384u
#define AUTH_LIFETIME_US (900LL * 1000000LL)

static const char *TAG = "codex_probe";

typedef struct {
    uint32_t magic;
    codex_tokens_t token;
} stored_account_t;

typedef struct {
    char body[RESPONSE_LIMIT + 1];
    size_t length;
    uint32_t retry_after;
    bool overflow;
} http_response_t;

typedef enum { CMD_BEGIN, CMD_CANCEL, CMD_REFRESH } command_kind_t;
typedef struct { command_kind_t kind; uint32_t id, generation; } command_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static codex_management_status_t s_status;
static EventGroupHandle_t s_wifi_events;
static QueueHandle_t s_commands;
static TaskHandle_t s_task;
static codex_tokens_t *s_active; /* Worker-owned; never read by USB/LVGL. */
static bool s_storage_ready;
static bool s_token_dirty; /* Rotated credentials kept in RAM if NVS retry is needed. */
static bool s_account_validated, s_reauth_required;
static int64_t s_next_storage_retry_us;
static bool s_auth_pending;
static bool s_committing;
static bool s_refresh_pending;
static uint32_t s_refresh_id;
static bool s_usage_busy;
static uint32_t s_generation;
static uint32_t s_usage_cycle_generation; /* Codex worker owns this sequence. */
static int64_t s_next_usage_us, s_retry_until_us;
static uint32_t s_backoff_sec = 30;

/* Device-code session belongs only to the worker. */
static codex_device_code_t s_device;
static int64_t s_auth_deadline_us, s_next_auth_us;
static uint32_t s_auth_generation;
static uint32_t s_auth_request_id;
static const char *account_state(void);

static void copy_text(char *dest, size_t size, const char *source)
{
    if (!source) source = "";
    snprintf(dest, size, "%s", source);
}

static void operation(uint32_t id, const char *method, const char *state,
                      const char *message)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.operation.id = id;
    copy_text(s_status.operation.method, sizeof(s_status.operation.method), method);
    copy_text(s_status.operation.state, sizeof(s_status.operation.state), state);
    copy_text(s_status.operation.message, sizeof(s_status.operation.message), message);
    taskEXIT_CRITICAL(&s_mux);
}

static void set_state(const char *state, const char *message)
{
    taskENTER_CRITICAL(&s_mux);
    copy_text(s_status.state, sizeof(s_status.state), state);
    copy_text(s_status.usage.status, sizeof(s_status.usage.status), message);
    taskEXIT_CRITICAL(&s_mux);
}

static void publish(void)
{
    provider_usage_t usage;
    taskENTER_CRITICAL(&s_mux);
    usage = s_status.usage;
    taskEXIT_CRITICAL(&s_mux);
    display_provider_update(USAGE_PROVIDER_CODEX, &usage);
}

static bool network_ready(void)
{
    if (!s_wifi_events || !(xEventGroupGetBits(s_wifi_events) & OAUTH_WIFI_CONNECTED) ||
        wifi_manager_is_provisioning() || time(NULL) < 1735689600) return false;
    oauth_management_status_t claude;
    oauth_probe_get_management_status(&claude);
    return claude.network.clock;
}

static bool enabled(void)
{
    const box_settings_t *settings = box_config_current();
    return settings && usage_provider_enabled(settings->providers, USAGE_PROVIDER_CODEX);
}

static uint32_t interval_sec(void)
{
    const box_settings_t *settings = box_config_current();
    return settings && settings->refresh_interval_sec >= 60 ? settings->refresh_interval_sec : 300;
}

static bool storage_load(codex_tokens_t *out)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;
    stored_account_t *record = calloc(1, sizeof(*record));
    if (!record) { nvs_close(handle); return false; }
    size_t size = sizeof(*record);
    esp_err_t err = nvs_get_blob(handle, "tokens", record, &size);
    nvs_close(handle);
    bool valid = err == ESP_OK && size == sizeof(*record) &&
        record->magic == RECORD_MAGIC &&
        memchr(record->token.access, 0, sizeof(record->token.access)) &&
        memchr(record->token.refresh, 0, sizeof(record->token.refresh)) &&
        memchr(record->token.id, 0, sizeof(record->token.id)) &&
        memchr(record->token.account_id, 0, sizeof(record->token.account_id)) &&
        record->token.access[0] && record->token.refresh[0] &&
        record->token.id[0] && record->token.account_id[0];
    if (valid) memcpy(out, &record->token, sizeof(*out));
    memset(record, 0, sizeof(*record));
    free(record);
    return valid;
}

static bool storage_save(const codex_tokens_t *token)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    stored_account_t *record = calloc(1, sizeof(*record));
    if (!record) { nvs_close(handle); return false; }
    record->magic = RECORD_MAGIC;
    memcpy(&record->token, token, sizeof(*token));
    esp_err_t err = nvs_set_blob(handle, "tokens", record, sizeof(*record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    memset(record, 0, sizeof(*record));
    free(record);
    return err == ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t length = (size_t)event->data_len;
        if (length > RESPONSE_LIMIT - response->length) response->overflow = true;
        else {
            memcpy(response->body + response->length, event->data, length);
            response->length += length;
            response->body[response->length] = 0;
        }
    } else if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
               event->header_value && strcasecmp(event->header_key, "Retry-After") == 0) {
        int delay = probe_retry_after_seconds(event->header_value, time(NULL));
        if (delay > 0) response->retry_after = (uint32_t)delay;
    }
    return ESP_OK;
}

static int http_request(const char *url, const char *post, const char *content_type,
                        const codex_tokens_t *token, http_response_t *response)
{
    memset(response, 0, sizeof(*response));
    if (!network_ready()) { ESP_LOGW(TAG, "HTTPS deferred: network or clock unavailable"); return -1; }
    if (!provider_http_take(pdMS_TO_TICKS(25000))) {
        ESP_LOGW(TAG, "HTTPS deferred: provider transport busy"); return -1;
    }
    esp_http_client_config_t config = {
        .url = url, .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 18000, .crt_bundle_attach = esp_crt_bundle_attach,
        /* IDF 5.1 cannot split a single HTTP header across TX buffers. */
        .buffer_size_tx = token ? CODEX_ACCESS_SIZE + 128 : 0,
        .event_handler = http_event, .user_data = response,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGW(TAG, "HTTPS client init failed"); provider_http_give(); return -1; }
    esp_http_client_set_header(client, "Accept", "application/json");
    if (post) {
        esp_http_client_set_header(client, "Content-Type", content_type);
        esp_http_client_set_post_field(client, post, strlen(post));
    }
    char *authorization = NULL;
    if (token) {
        size_t length = strlen(token->access) + 8;
        authorization = malloc(length);
        if (!authorization) { esp_http_client_cleanup(client); provider_http_give(); return -1; }
        snprintf(authorization, length, "Bearer %s", token->access);
        esp_http_client_set_header(client, "Authorization", authorization);
        esp_http_client_set_header(client, "ChatGPT-Account-ID", token->account_id);
    }
    esp_err_t err = wifi_manager_is_provisioning() ? ESP_FAIL : esp_http_client_perform(client);
    int status = err == ESP_OK && !response->overflow ? esp_http_client_get_status_code(client) : -1;
    if (err != ESP_OK)
        ESP_LOGW(TAG, "HTTPS transport error=%d (%s)", (int)err, esp_err_to_name(err));
    if (response->overflow) ESP_LOGW(TAG, "HTTPS response exceeded %u bytes", RESPONSE_LIMIT);
    esp_http_client_cleanup(client);
    if (authorization) { memset(authorization, 0, strlen(authorization)); free(authorization); }
    provider_http_give();
    return status;
}

static char *json_body(const char *key1, const char *value1,
                       const char *key2, const char *value2)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, key1, value1);
    if (key2) cJSON_AddStringToObject(root, key2, value2);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static char *refresh_body(const char *refresh)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "grant_type", "refresh_token");
    cJSON_AddStringToObject(root, "client_id", CODEX_CLIENT_ID);
    cJSON_AddStringToObject(root, "refresh_token", refresh);
    cJSON_AddStringToObject(root, "refresh_reason", "scheduled");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static void wipe_free(char *value)
{
    if (value) { memset(value, 0, strlen(value)); free(value); }
}

static int refresh_token(uint32_t *retry_after)
{
    if (!s_active) return -1;
    http_response_t *response = calloc(1, sizeof(*response));
    codex_tokens_t *candidate = calloc(1, sizeof(*candidate));
    char *body = refresh_body(s_active->refresh);
    if (!response || !candidate || !body) {
        free(response); free(candidate); wipe_free(body); return -1;
    }
    int status = http_request(AUTH_BASE "/oauth/token", body, "application/json", NULL, response);
    wipe_free(body);
    if (retry_after && status == 429) *retry_after = response->retry_after;
    bool ok = status == 200 && codex_parse_tokens(response->body, s_active,
                                                   time(NULL), candidate);
    if (ok) {
        bool saved = storage_save(candidate);
        memcpy(s_active, candidate, sizeof(*s_active));
        s_token_dirty = !saved;
        if (!saved) {
            s_next_storage_retry_us = esp_timer_get_time() + 5LL * 1000000;
            if (!s_auth_pending) set_state(account_state(), "Account save pending");
        }
    }
    memset(response, 0, sizeof(*response)); free(response);
    memset(candidate, 0, sizeof(*candidate)); free(candidate);
    return ok ? 200 : status == 429 ? 429 :
           status == 400 || status == 401 ? 401 : -1;
}

static void retry_dirty_token(void)
{
    if (!s_token_dirty || !s_active || esp_timer_get_time() < s_next_storage_retry_us)
        return;
    if (storage_save(s_active)) {
        s_token_dirty = false;
        taskENTER_CRITICAL(&s_mux);
        if (strcmp(s_status.state, "storage_pending") == 0)
            copy_text(s_status.state, sizeof(s_status.state), account_state());
        taskEXIT_CRITICAL(&s_mux);
    } else {
        s_next_storage_retry_us = esp_timer_get_time() + 30LL * 1000000;
    }
}

static int fetch_usage(const codex_tokens_t *token, provider_usage_t *usage,
                       uint32_t *retry_after)
{
    http_response_t *response = calloc(1, sizeof(*response));
    if (!response) return -1;
    int status = http_request(USAGE_URL, NULL, NULL, token, response);
    int http_status = status;
    bool parsed = status == 200 && network_ready() &&
                  codex_parse_usage(response->body, time(NULL), usage);
    ESP_LOGI(TAG, "Codex usage check: http_status=%d parsed=%d",
             http_status, parsed ? 1 : 0);
    if (status == 200 && !parsed) status = -1;
    *retry_after = response->retry_after;
    memset(response, 0, sizeof(*response));
    free(response);
    return status;
}

static bool current_generation(uint32_t generation)
{
    bool result;
    taskENTER_CRITICAL(&s_mux);
    result = s_generation == generation;
    taskEXIT_CRITICAL(&s_mux);
    return result;
}

static void clear_auth(void)
{
    memset(&s_device, 0, sizeof(s_device));
    s_auth_deadline_us = s_next_auth_us = 0;
    taskENTER_CRITICAL(&s_mux);
    s_auth_pending = false;
    s_status.user_code[0] = 0;
    s_status.verification_url[0] = 0;
    s_status.expires_at = 0;
    taskEXIT_CRITICAL(&s_mux);
}

static const char *account_state(void)
{
    if (s_reauth_required) return "reauth_required";
    if (!s_active) return "signed_out";
    if (s_token_dirty) return "storage_pending";
    return s_account_validated ? "connected" : "checking";
}

static void auth_fail(const char *message)
{
    uint32_t id = s_auth_request_id;
    clear_auth();
    set_state(s_active ? account_state() : "error",
              s_active ? (s_reauth_required ? "Sign in again" :
                          s_token_dirty ? "Account save pending" :
                          s_account_validated ? "Ready" : "Checking saved sign-in") : message);
    if (id) operation(id, "codex.begin", "error", message);
    publish();
}

static void begin_auth(const command_t *command)
{
    s_auth_request_id = command->id;
    if (!network_ready()) { auth_fail("Connect Wi-Fi and wait for the clock"); operation(command->id, "codex.begin", "error", "Network not ready"); return; }
    http_response_t *response = calloc(1, sizeof(*response));
    char *body = json_body("client_id", CODEX_CLIENT_ID, NULL, NULL);
    if (!response || !body) {
        free(response); wipe_free(body); auth_fail("Out of memory");
        operation(command->id, "codex.begin", "error", "Out of memory"); return;
    }
    int status = http_request(AUTH_BASE "/api/accounts/deviceauth/usercode",
                              body, "application/json", NULL, response);
    wipe_free(body);
    bool parsed = status == 200 && codex_parse_device_code(response->body, &s_device);
    bool current = current_generation(command->generation);
    bool ok = parsed && current;
    size_t response_bytes = response->length;
    memset(response, 0, sizeof(*response)); free(response);
    if (!ok) {
        int stage = status != 200 ? 0 : !parsed ? 1 : 2;
        ESP_LOGW(TAG, "Device-code start failed: http_status=%d response_bytes=%u stage=%d",
                 status, (unsigned)response_bytes, stage);
        char message[96];
        if (!current) copy_text(message, sizeof(message), "Sign-in was cancelled");
        else if (status < 0) copy_text(message, sizeof(message), "Device-code transport unavailable");
        else if (status != 200) snprintf(message, sizeof(message), "Device-code server returned HTTP %d", status);
        else copy_text(message, sizeof(message), "Device-code response invalid");
        auth_fail(message);
        operation(command->id, "codex.begin", "error", message); return;
    }
    s_auth_generation = command->generation;
    s_auth_deadline_us = esp_timer_get_time() + AUTH_LIFETIME_US;
    s_next_auth_us = esp_timer_get_time() + (int64_t)s_device.interval_sec * 1000000;
    taskENTER_CRITICAL(&s_mux);
    copy_text(s_status.state, sizeof(s_status.state), "awaiting_code");
    copy_text(s_status.verification_url, sizeof(s_status.verification_url), CODEX_VERIFY_URL);
    copy_text(s_status.user_code, sizeof(s_status.user_code), s_device.user_code);
    s_status.expires_at = time(NULL) + 900;
    taskEXIT_CRITICAL(&s_mux);
    operation(command->id, "codex.begin", "done", "Enter the code in your browser");
}

static bool form_character(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

static bool form_append(char *dest, size_t capacity, size_t *offset, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*offset + 4 >= capacity) return false;
        if (form_character(*p)) dest[(*offset)++] = (char)*p;
        else { dest[(*offset)++] = '%'; dest[(*offset)++] = hex[*p >> 4]; dest[(*offset)++] = hex[*p & 15]; }
    }
    dest[*offset] = 0;
    return true;
}

static char *authorization_form(const codex_authorization_t *auth)
{
    char *form = calloc(4096, 1);
    if (!form) return NULL;
    size_t length = (size_t)snprintf(form, 4096,
        "grant_type=authorization_code&client_id=%s&code=", CODEX_CLIENT_ID);
    bool ok = length < 4096 && form_append(form, 4096, &length, auth->authorization_code);
    if (ok) {
        int count = snprintf(form + length, 4096 - length,
            "&redirect_uri=https%%3A%%2F%%2Fauth.openai.com%%2Fdeviceauth%%2Fcallback&code_verifier=");
        ok = count > 0 && (size_t)count < 4096 - length;
        if (ok) length += (size_t)count;
    }
    if (ok) ok = form_append(form, 4096, &length, auth->code_verifier);
    if (!ok) { memset(form, 0, 4096); free(form); return NULL; }
    return form;
}

/* The authorization code is already redeemed at this point. Persist the new
 * credentials before attempting the separate, retryable usage request. */
static bool commit_authorized_account(const codex_tokens_t *candidate)
{
    if (!candidate) return false;
    codex_tokens_t *active = s_active;
    bool allocated = false;
    if (!active) {
        active = calloc(1, sizeof(*active));
        if (!active) {
            ESP_LOGW(TAG, "Codex account commit failed: stage=allocate result=0");
            return false;
        }
        allocated = true;
    }
    taskENTER_CRITICAL(&s_mux);
    bool reserved = s_generation == s_auth_generation && !s_committing;
    if (reserved) s_committing = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!reserved) {
        if (allocated) free(active);
        ESP_LOGW(TAG, "Codex account commit failed: stage=reserve result=0");
        return false;
    }
    bool saved = storage_save(candidate);
    ESP_LOGI(TAG, "Codex account commit: stage=save result=%d", saved ? 1 : 0);
    if (!saved) {
        taskENTER_CRITICAL(&s_mux);
        s_committing = false;
        taskEXIT_CRITICAL(&s_mux);
        if (allocated) free(active);
        return false;
    }
    memcpy(active, candidate, sizeof(*active));
    s_active = active;
    s_account_validated = false;
    s_reauth_required = false;
    s_token_dirty = false;
    s_next_storage_retry_us = 0;
    s_backoff_sec = 30;
    s_next_usage_us = 0;
    taskENTER_CRITICAL(&s_mux);
    s_retry_until_us = 0; /* New account must not inherit an old cooldown. */
    memset(&s_status.usage, 0, sizeof(s_status.usage));
    s_status.configured = true;
    s_status.usage.configured = true;
    taskEXIT_CRITICAL(&s_mux);
    clear_auth();
    taskENTER_CRITICAL(&s_mux);
    s_committing = false;
    taskEXIT_CRITICAL(&s_mux);
    set_state(account_state(), "Checking account usage");
    if (s_auth_request_id)
        operation(s_auth_request_id, "codex.begin", "done",
                  "Codex account saved; checking usage");
    publish();
    return true;
}

static void poll_auth(void)
{
    int64_t now = esp_timer_get_time();
    if (now >= s_auth_deadline_us || !current_generation(s_auth_generation)) {
        auth_fail("Sign-in expired or cancelled"); return;
    }
    if (now < s_next_auth_us || !network_ready()) return;
    s_next_auth_us = now + (int64_t)s_device.interval_sec * 1000000;
    http_response_t *response = calloc(1, sizeof(*response));
    cJSON *root = cJSON_CreateObject();
    if (!response || !root) { free(response); cJSON_Delete(root); auth_fail("Out of memory"); return; }
    cJSON_AddStringToObject(root, "device_auth_id", s_device.device_auth_id);
    cJSON_AddStringToObject(root, "user_code", s_device.user_code);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { free(response); auth_fail("Out of memory"); return; }
    int status = http_request(AUTH_BASE "/api/accounts/deviceauth/token", body,
                              "application/json", NULL, response);
    wipe_free(body);
    if (!current_generation(s_auth_generation)) { memset(response, 0, sizeof(*response)); free(response); auth_fail("Sign-in cancelled"); return; }
    if (status == 403 || status == 404) { memset(response, 0, sizeof(*response)); free(response); return; }
    if (status < 0 || status == 429 || status >= 500) {
        uint32_t delay = status == 429 && response->retry_after > s_device.interval_sec ?
                         response->retry_after : s_device.interval_sec;
        s_next_auth_us = esp_timer_get_time() + (int64_t)delay * 1000000;
        memset(response, 0, sizeof(*response)); free(response); return;
    }
    codex_authorization_t *authorization = calloc(1, sizeof(*authorization));
    bool valid = status == 200 && authorization &&
        codex_parse_authorization(response->body, authorization);
    memset(response, 0, sizeof(*response)); free(response);
    if (!valid) {
        ESP_LOGW(TAG, "Codex authorization failed: stage=device_auth http_status=%d parsed=%d",
                 status, valid ? 1 : 0);
        free(authorization); auth_fail("Sign-in verification failed"); return;
    }
    set_state("verifying", "Saving account");
    body = authorization_form(authorization);
    memset(authorization, 0, sizeof(*authorization)); free(authorization);
    response = calloc(1, sizeof(*response));
    codex_tokens_t *candidate = calloc(1, sizeof(*candidate));
    if (!body || !response || !candidate) {
        wipe_free(body); free(response); free(candidate); auth_fail("Out of memory"); return;
    }
    status = http_request(AUTH_BASE "/oauth/token", body,
                          "application/x-www-form-urlencoded", NULL, response);
    wipe_free(body);
    valid = status == 200 && codex_parse_tokens(response->body, NULL, time(NULL), candidate);
    ESP_LOGI(TAG, "Codex token exchange: http_status=%d parsed=%d",
             status, valid ? 1 : 0);
    memset(response, 0, sizeof(*response)); free(response);
    if (!valid) auth_fail("Token exchange failed; existing sign-in kept");
    else if (!commit_authorized_account(candidate))
        auth_fail("Account save failed; existing sign-in kept");
    if (candidate) { memset(candidate, 0, sizeof(*candidate)); free(candidate); }
}

static int poll_usage(void)
{
    if (!s_active || !network_ready()) return -2;
    int64_t now = esp_timer_get_time();
    bool selected = enabled();
    bool manual;
    uint32_t manual_id = 0;
    taskENTER_CRITICAL(&s_mux);
    if (s_usage_busy || now < s_retry_until_us ||
        (!s_refresh_pending && (!selected || now < s_next_usage_us))) {
        taskEXIT_CRITICAL(&s_mux); return -2;
    }
    manual = s_refresh_pending;
    if (manual) {
        manual_id = s_refresh_id;
        s_refresh_pending = false;
    }
    s_usage_busy = true;
    s_status.usage.updating = true;
    taskEXIT_CRITICAL(&s_mux);
    if (++s_usage_cycle_generation == 0) ++s_usage_cycle_generation;
    uint32_t cycle_generation = s_usage_cycle_generation;
    display_app_refresh_begin(APP_ID_CODEX, cycle_generation);
    publish();
    provider_usage_t usage = {0};
    uint32_t retry_after = 0;
    int status = 0;
    bool attempted_refresh = s_active->expires_at && time(NULL) >= s_active->expires_at - 300;
    if (attempted_refresh)
        status = refresh_token(&retry_after);
    if (!status || status == 200)
        status = fetch_usage(s_active, &usage, &retry_after);
    if (status == 401 && !attempted_refresh) {
        int refreshed = refresh_token(&retry_after);
        status = refreshed == 200 ? fetch_usage(s_active, &usage, &retry_after) : refreshed;
    }
    now = esp_timer_get_time();
    if (status == 200) {
        usage.configured = true;
        usage.sampled_us = now;
        copy_text(usage.status, sizeof(usage.status), "Ready");
        s_account_validated = true;
        s_reauth_required = false;
        taskENTER_CRITICAL(&s_mux);
        s_status.usage = usage;
        if (!s_auth_pending)
            copy_text(s_status.state, sizeof(s_status.state), account_state());
        taskEXIT_CRITICAL(&s_mux);
        s_backoff_sec = 30;
        s_next_usage_us = now + (int64_t)interval_sec() * 1000000;
    } else {
        uint32_t delay = status == 429 ? (retry_after > 0 ? retry_after : 60) : s_backoff_sec;
        bool offline = !network_ready();
        taskENTER_CRITICAL(&s_mux);
        s_retry_until_us = now + (int64_t)delay * 1000000;
        s_next_usage_us = s_retry_until_us;
        taskEXIT_CRITICAL(&s_mux);
        if (status != 429 && s_backoff_sec < 300) s_backoff_sec *= 2;
        taskENTER_CRITICAL(&s_mux);
        s_status.usage.updating = false;
        copy_text(s_status.usage.status, sizeof(s_status.usage.status),
                  status == 429 ? "Retry after cooldown" :
                  status == 401 ? "Sign in again" :
                  offline ? "Offline" : "Usage unavailable; retrying");
        taskEXIT_CRITICAL(&s_mux);
        if (status == 401) {
            s_account_validated = false;
            s_reauth_required = true;
            set_state("reauth_required", "Sign in again");
        }
    }
    taskENTER_CRITICAL(&s_mux);
    s_usage_busy = false;
    taskEXIT_CRITICAL(&s_mux);
    publish();
    display_app_refresh_end(APP_ID_CODEX, cycle_generation);
    if (manual && manual_id) operation(manual_id, "codex.refresh", status == 200 ? "done" : "error",
                          status == 200 ? "Usage updated" :
                          status == 429 ? "Rate limited" :
                          status == 401 ? "Sign in again" : "Usage request failed");
    return status;
}

static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        retry_dirty_token();
        command_t command;
        while (xQueueReceive(s_commands, &command, 0) == pdTRUE) {
            if (command.kind == CMD_BEGIN) {
                operation(command.id, "codex.begin", "running", "Requesting device code");
                set_state("starting", "Starting sign-in"); begin_auth(&command);
            } else if (command.kind == CMD_CANCEL) {
                clear_auth();
                set_state(account_state(),
                          s_active ? (s_reauth_required ? "Sign in again" :
                                      s_token_dirty ? "Account save pending" :
                                      s_account_validated ? "Ready" : "Checking saved sign-in") :
                          "Sign-in cancelled");
                operation(command.id, "codex.cancel", "done", "Sign-in cancelled"); publish();
            } else if (command.kind == CMD_REFRESH) {
                if (command.id) operation(command.id, "codex.refresh", "running", "Reading usage");
                poll_usage();
            }
        }
        if (s_auth_deadline_us) poll_auth();
        poll_usage();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void codex_adapter_start(EventGroupHandle_t wifi_events)
{
    s_wifi_events = wifi_events;
    memset(&s_status, 0, sizeof(s_status));
    if (nvs_flash_init_partition(PARTITION) != ESP_OK) {
        set_state("unavailable", "Account storage unavailable"); publish(); return;
    }
    s_active = calloc(1, sizeof(*s_active));
    if (!s_active) {
        set_state("unavailable", "Out of memory"); publish(); return;
    }
    bool configured = storage_load(s_active);
    if (!configured) { memset(s_active, 0, sizeof(*s_active)); free(s_active); s_active = NULL; }
    s_status.configured = configured;
    s_status.usage.configured = configured;
    set_state(account_state(), configured ? "Checking saved sign-in" : "Connect Codex");
    publish();
    s_commands = xQueueCreate(4, sizeof(command_t));
    if (!s_commands || xTaskCreate(worker, "codex_probe", 12288, NULL, 4, &s_task) != pdPASS) {
        if (s_commands) { vQueueDelete(s_commands); s_commands = NULL; }
        set_state("unavailable", "Account worker unavailable"); publish();
        return;
    }
    s_storage_ready = true;
}

bool codex_adapter_request(uint32_t id, const char *method)
{
    if (!s_storage_ready || !s_commands || !method) return false;
    command_t command = {.id = id};
    if (strcmp(method, "codex.begin") == 0) command.kind = CMD_BEGIN;
    else if (strcmp(method, "codex.cancel") == 0) command.kind = CMD_CANCEL;
    else if (strcmp(method, "codex.refresh") == 0) command.kind = CMD_REFRESH;
    else return false;
    if (command.kind == CMD_REFRESH && !network_ready()) return false;
    bool accepted = false;
    taskENTER_CRITICAL(&s_mux);
    if (!s_committing && (command.kind != CMD_REFRESH ||
                           (s_status.configured && !s_auth_pending &&
                            !s_refresh_pending && !s_usage_busy &&
                            esp_timer_get_time() >= s_retry_until_us)) &&
        (command.kind != CMD_BEGIN ||
            (!s_auth_pending && !s_refresh_pending && !s_usage_busy)) &&
        (command.kind != CMD_CANCEL || s_auth_pending)) {
        command.generation = s_generation +
            (command.kind == CMD_BEGIN || command.kind == CMD_CANCEL ? 1u : 0u);
        if (xQueueSend(s_commands, &command, 0) == pdTRUE) {
            accepted = true;
            s_generation = command.generation;
            if (command.kind == CMD_BEGIN) s_auth_pending = true;
            if (command.kind == CMD_REFRESH) {
                s_refresh_pending = true;
                s_refresh_id = id;
                s_status.usage.updating = true;
            }
            if (id) {
                s_status.operation.id = id;
                copy_text(s_status.operation.method, sizeof(s_status.operation.method), method);
                copy_text(s_status.operation.state, sizeof(s_status.operation.state), "accepted");
                s_status.operation.message[0] = 0;
            }
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (accepted && s_task) xTaskNotifyGive(s_task);
    return accepted;
}

bool codex_adapter_refresh(void)
{
    return codex_adapter_request(0, "codex.refresh");
}

void codex_adapter_get_status(codex_management_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}
