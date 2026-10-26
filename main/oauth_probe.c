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
#include "probe_protocol.h"
#include "refresh_control.h"

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

typedef struct { char code[513]; } submission_t;

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta;
static QueueHandle_t s_submissions;
static SemaphoreHandle_t s_cap_lock;
static httpd_handle_t s_server;
static char s_cap[48];
static char s_auth_url[512];
static int64_t s_cap_deadline;
static bool s_cap_valid;
static const char *s_page_state = "Ready";
/* Only the OAuth worker owns credentials and this flag. */
static bool s_token_dirty;
static atomic_bool s_time_synced;
static portMUX_TYPE s_refresh_mux = portMUX_INITIALIZER_UNLOCKED;
static refresh_control_t s_refresh_control;
static TaskHandle_t s_worker_task;

static int64_t now_s(void) { return (int64_t)time(NULL); }
static bool wifi_up(void) { return (xEventGroupGetBits(s_wifi_events) & OAUTH_WIFI_CONNECTED) != 0; }

static void on_time_sync(struct timeval *tv) { (void)tv; atomic_store(&s_time_synced, true); }
static bool clock_valid(void) { return atomic_load(&s_time_synced) && now_s() > 1735689600; }

static void publish_refresh(bool ready, int64_t retry_at)
{
    taskENTER_CRITICAL(&s_refresh_mux);
    refresh_control_publish(&s_refresh_control, ready, retry_at);
    taskEXIT_CRITICAL(&s_refresh_mux);
}

static bool begin_refresh(bool automatic_due, bool *manual)
{
    taskENTER_CRITICAL(&s_refresh_mux);
    bool began = (automatic_due || s_refresh_control.pending) &&
                 refresh_control_begin(&s_refresh_control, manual);
    taskEXIT_CRITICAL(&s_refresh_mux);
    if (began) {
        if (*manual) ESP_LOGI(TAG, "Manual usage refresh started");
        display_refresh_cycle_begin();
        display_updating();
    }
    return began;
}

static void end_refresh(bool ready, int64_t retry_at)
{
    display_refresh_cycle_end();
    taskENTER_CRITICAL(&s_refresh_mux);
    refresh_control_finish(&s_refresh_control, ready, retry_at);
    taskEXIT_CRITICAL(&s_refresh_mux);
}

static bool request_manual_refresh(void *context)
{
    (void)context;
    if (!s_worker_task || !s_wifi_events || !wifi_up() || !clock_valid()) return false;
    int64_t now = now_s();
    taskENTER_CRITICAL(&s_refresh_mux);
    bool accepted = refresh_control_request(&s_refresh_control, now);
    taskEXIT_CRITICAL(&s_refresh_mux);
    if (accepted) {
        ESP_LOGI(TAG, "Manual usage refresh accepted");
        xTaskNotifyGive(s_worker_task);
    }
    return accepted;
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
    return err == ESP_OK && size == sizeof(*t) && token_valid(t);
}

static bool save_token(const token_store_t *t)
{
    nvs_handle_t h;
    if (nvs_open("claude_probe", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "token", t, sizeof(*t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool clear_token(void)
{
    nvs_handle_t h;
    if (nvs_open("claude_probe", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(h, "token");
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
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
    esp_http_client_config_t config = {
        .url = url, .method = post_body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event, .user_data = r, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&config);
    if (!c) return -1;
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
            return -1;
        }
        esp_http_client_set_header(c, "Authorization", authorization);
    }
    int64_t began = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    int status = err == ESP_OK && !r->overflow ? esp_http_client_get_status_code(c) : -1;
    ESP_LOGI(TAG, "HTTPS status=%d elapsed_ms=%lld heap=%lu", status,
             (long long)((esp_timer_get_time() - began) / 1000), (unsigned long)esp_get_free_heap_size());
    memset(authorization, 0, sizeof(authorization));
    esp_http_client_cleanup(c);
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

static bool parse_token(const response_t *r, token_store_t *current, bool is_refresh)
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
        /* Retain a rotated refresh token in RAM even if flash storage fails.
         * The worker retries persistence before issuing further requests. */
        *current = next;
        s_token_dirty = !save_token(&next);
        if (s_token_dirty) ESP_LOGE(TAG, "New OAuth credential pending NVS persistence");
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
    if (status == 200 && parse_token(r, t, true)) {
        ESP_LOGI(TAG, "OAuth refresh succeeded at %lld; new expiry %lld", (long long)now_s(),
                 (long long)t->expires_at);
        result = REFRESH_OK;
    } else if (status == 400 && invalid_grant(r)) {
        if (clear_token()) {
            ESP_LOGW(TAG, "OAuth refresh invalid_grant; account setup required");
            display_status("OAuth error", "Sign in again");
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
    bool success = status == 200 && parse_token(r, t, false);
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
            if (wifi_up()) {
                display_usage(five, five_reset, week, week_reset);
                ESP_LOGI(TAG, "Usage response accepted: five=%s week=%s", five < 0 ? "unknown" : "known",
                         week < 0 ? "unknown" : "known");
            } else status = -1;
        } else status = -1;
        cJSON_Delete(root);
    }
    memset(r, 0, sizeof(*r)); free(r);
    return status;
}

static bool cap_matches(const char *uri)
{
    bool valid = false;
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    if (s_cap_valid && now_s() < s_cap_deadline) {
        char expected[64];
        snprintf(expected, sizeof(expected), "/s/%s", s_cap);
        valid = strcmp(uri, expected) == 0;
    }
    xSemaphoreGive(s_cap_lock);
    return valid;
}

static esp_err_t setup_get(httpd_req_t *req)
{
    if (!cap_matches(req->uri)) return httpd_resp_send_404(req);
    char auth[sizeof(s_auth_url)];
    const char *state;
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    strcpy(auth, s_auth_url);
    state = s_page_state;
    xSemaphoreGive(s_cap_lock);
    char page[3072];
    int n = snprintf(page, sizeof(page),
        "<!doctype html><html lang='en'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='referrer' content='no-referrer'><title>Connect Claude</title>"
        "<style>body{font:18px system-ui;max-width:32rem;margin:3rem auto;padding:0 1rem;background:#15171b;color:#fff}"
        "a,button{background:#e5ae75;color:#171717;padding:.8rem;border:0;border-radius:.5rem;display:inline-block}"
        "textarea{box-sizing:border-box;width:100%%;height:7rem;margin:1rem 0}small{color:#bbb}</style>"
        "<h1>Connect Claude</h1><p>1. Sign in to Claude and copy the displayed code.</p>"
        "<p><a href='%s' rel='noreferrer noopener'>Open Claude sign-in</a></p>"
        "<p>2. Paste the entire code, including #state, here.</p><textarea id='code' autocomplete='off' spellcheck='false'></textarea>"
        "<button id='send'>Submit code</button><p id='result'>%s</p>"
        "<small>This page is available on your local Wi-Fi for ten minutes.</small>"
        "<script>document.getElementById('send').onclick=async()=>{let b=document.getElementById('code');"
        "let r=document.getElementById('result');r.textContent='Sending...';try{let x=await fetch(location.pathname,{method:'POST',"
        "headers:{'Content-Type':'text/plain'},body:b.value});b.value='';r.textContent=await x.text()}"
        "catch(e){r.textContent='Connection failed'}};</script></html>", auth, state);
    if (n < 0 || n >= sizeof(page)) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'");
    return httpd_resp_send(req, page, n);
}

static esp_err_t setup_post(httpd_req_t *req)
{
    if (!cap_matches(req->uri)) return httpd_resp_send_404(req);
    if (req->content_len < 1 || req->content_len > 512) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "Code is missing or too long");
    }
    submission_t item = {0};
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
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    s_cap_valid = false;
    s_cap[0] = 0;
    s_auth_url[0] = 0;
    xSemaphoreGive(s_cap_lock);
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    xQueueReset(s_submissions);
}

static bool start_setup(char *state, char *verifier)
{
    char challenge[48];
    char cap[48];
    if (!encode_random(verifier, 80, 32) || !encode_random(state, 48, 24) ||
        !encode_random(cap, sizeof(cap), 24) || !pkce_challenge(verifier, challenge, sizeof(challenge)))
        return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    char url[128];
    snprintf(url, sizeof(url), "http://" IPSTR ":%d/s/%s", IP2STR(&ip.ip), CONFIG_OAUTH_PROBE_HTTP_PORT, cap);
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
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
    }
    display_setup(url);
    ESP_LOGI(TAG, "Account setup available for 600 seconds");
    return true;
}

static void set_page_state(const char *state)
{
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    s_page_state = state;
    xSemaphoreGive(s_cap_lock);
}

static void worker(void *arg)
{
    (void)arg;
    token_store_t *token = calloc(1, sizeof(*token));
    if (!token) { display_status("OAuth error", "Out of memory"); vTaskDelete(NULL); return; }
    bool have_token = load_token(token);
    bool clock_announced = false;
    bool first_usage = true;
    int64_t next_usage = 0, next_refresh = 0, usage_retry_until = 0;
    int failures = 0;
    char state[48] = {0}, verifier[80] = {0};
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();
    for (;;) {
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
        }
        if (!clock_valid()) {
            publish_refresh(false, 0);
            display_status("Time", "Waiting for clock");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!clock_announced) {
            setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
            tzset();
            display_clock_ready();
            ESP_LOGI(TAG, "SNTP time ready");
            clock_announced = true;
        }
        if (have_token && s_token_dirty) {
            publish_refresh(false, 0);
            if (!save_token(token)) {
                display_status("Storage error", "Saving sign-in; retrying");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            s_token_dirty = false;
            ESP_LOGI(TAG, "Pending OAuth credential persisted");
        }
        if (!have_token) {
            publish_refresh(false, 0);
            if (!s_server || now_s() >= s_cap_deadline) {
                if (s_server) stop_setup();
                if (!start_setup(state, verifier)) {
                    display_status("Setup error", "Local setup page unavailable");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
            }
            submission_t item = {0};
            if (xQueueReceive(s_submissions, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {
                char *code = NULL;
                if (!probe_authorization_code(item.code, state, &code)) {
                    set_page_state("Paste the full code with the matching #state");
                    display_status("OAuth error", "Code or state does not match");
                    memset(&item, 0, sizeof(item));
                    continue;
                }
                if (!wifi_up() || now_s() >= s_cap_deadline) {
                    set_page_state("Invalid code or setup page expired");
                } else {
                    display_status("OAuth", "Checking code");
                    have_token = exchange_code(code, state, verifier, token);
                    if (have_token) {
                        stop_setup();
                        display_status("OAuth", s_token_dirty ? "Token received; saving" : "Token saved; checking usage");
                        memset(verifier, 0, sizeof(verifier));
                        memset(state, 0, sizeof(state));
                        first_usage = true;
                        next_usage = 0;
                        next_refresh = 0;
                        failures = 0;
                    } else {
                        set_page_state("Check failed; use a new sign-in code");
                        display_status("OAuth error", "Code check failed");
                    }
                }
                memset(&item, 0, sizeof(item));
            }
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
        bool cycle = begin_refresh(refresh_due || usage_due, &manual);
        if (manual) next_usage = 0;
        if (cycle && now >= token->expires_at - 60 && now >= next_refresh) {
            int retry_after = 0;
            display_status("OAuth", "Refreshing token");
            ESP_LOGI(TAG, "REFRESH_TEST automatic attempt at %lld; expiry %lld",
                     (long long)now, (long long)token->expires_at);
            refresh_result_t result = refresh(token, &retry_after);
            if (result == REFRESH_RECONNECT) {
                have_token = false;
                memset(token, 0, sizeof(*token));
                next_refresh = 0;
                end_refresh(false, 0);
                continue;
            }
            if (result == REFRESH_OK) {
                next_refresh = 0;
                next_usage = 0;
                failures = 0;
            } else {
                next_refresh = now_s() + refresh_control_retry_delay(++failures, retry_after);
                if (next_usage < next_refresh) next_usage = next_refresh;
                display_status("OAuth", "Token refresh failed; retrying");
            }
        }
        if (s_token_dirty) {
            if (cycle) end_refresh(false, 0);
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
                    display_status("Usage", "Retrying after cooldown");
                    end_refresh(true, next_refresh);
                    continue;
                }
                ESP_LOGI(TAG, "REFRESH_TEST usage 401 attempt at %lld", (long long)now_s());
                refresh_result_t result = refresh(token, &retry_after);
                if (result == REFRESH_RECONNECT) {
                    have_token = false;
                    memset(token, 0, sizeof(*token));
                    next_refresh = 0;
                    end_refresh(false, 0);
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
                if (first_usage) {
                    stop_setup();
                    first_usage = false;
                    ESP_LOGI(TAG, "First authenticated usage succeeded; setup disabled");
                }
                failures = 0;
                usage_retry_until = 0;
                next_usage = now_s() + 300;
            } else {
                if (wifi_up()) display_status("Usage", "Usage check failed; retrying");
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
            end_refresh(have_token && !s_token_dirty && wifi_up() && clock_valid(), retry_gate);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

void oauth_probe_start(EventGroupHandle_t wifi_events, esp_netif_t *sta)
{
    s_wifi_events = wifi_events;
    s_sta = sta;
    s_submissions = xQueueCreate(1, sizeof(submission_t));
    s_cap_lock = xSemaphoreCreateMutex();
    if (!s_submissions || !s_cap_lock || xTaskCreate(worker, "oauth_probe", 12288, NULL, 5, &s_worker_task) != pdPASS)
        display_status("OAuth error", "Service could not start");
    else
        display_set_refresh_callback(request_manual_refresh, NULL);
}
