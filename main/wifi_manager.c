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
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "display.h"
#include "device_discovery.h"
#include "device_identity.h"
#include "factory_reset.h"
#include "oauth_probe.h"
#include "wifi_store.h"

#define CAP_LIFETIME_US (600LL * 1000000LL)
#define TRIAL_TIMEOUT_US (20LL * 1000000LL)
#define BODY_LIMIT 512
#define RETRY_MAX_SECONDS 60

static const char *TAG = "wifi_manager";

typedef enum {
    CMD_FORGET,
    CMD_GOT_IP,
    CMD_DISCONNECTED,
    CMD_PHONE_JOINED,
    CMD_PHONE_LEFT,
    CMD_SUBMIT,
} command_kind_t;

typedef struct {
    command_kind_t kind;
    wifi_store_record_t credentials;
} command_t;

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
    bool trial_active;
    bool pending_save;
    bool setup_committed;
    bool link_has_ip;
    int64_t trial_deadline;
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
static portMUX_TYPE s_cap_mux = portMUX_INITIALIZER_UNLOCKED;

bool wifi_manager_is_provisioning(void)
{
    return atomic_load_explicit(&s_provisioning, memory_order_acquire);
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
    (void)data;
    command_t command = {0};
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) command.kind = CMD_GOT_IP;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) command.kind = CMD_DISCONNECTED;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) command.kind = CMD_PHONE_JOINED;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) command.kind = CMD_PHONE_LEFT;
    else return;
    (void)xQueueSend(s_manager.queue, &command, 0);
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

static void show_setup(const char *message)
{
    s_manager.setup_message = message;
    display_wifi_setup(s_manager.join_qr, s_manager.page_url,
                       s_manager.phone_joined, message);
}

static bool cap_matches(const char *uri)
{
    if (!uri || strncmp(uri, "/w/", 3) != 0 || strlen(uri) != 35) return false;
    portENTER_CRITICAL(&s_cap_mux);
    bool valid = esp_timer_get_time() < s_manager.cap_deadline &&
                 memcmp(uri + 3, s_manager.cap, 32) == 0;
    portEXIT_CRITICAL(&s_cap_mux);
    return valid;
}

static esp_err_t portal_get(httpd_req_t *request)
{
    if (!cap_matches(request->uri)) return httpd_resp_send_404(request);
    static const char page_template[] =
        "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"referrer\" content=\"no-referrer\">"
        "<title>Claude monitor Wi-Fi setup</title>"
        "<style>body{font:18px system-ui;max-width:28rem;margin:3rem auto;padding:0 1rem}"
        "label{display:block;margin:1rem 0}input{box-sizing:border-box;width:100%%;padding:.6rem;font:inherit}"
        "button{padding:.7rem 1rem;font:inherit}</style>"
        "<h1>Connect %s to Wi-Fi</h1>"
        "<p>Enter the network name and password. Keep this page open until the monitor confirms the connection.</p>"
        "<form method=\"post\"><label>Network name<input name=\"ssid\" maxlength=\"32\" required></label>"
        "<label>Wi-Fi password<input name=\"password\" type=\"password\" maxlength=\"64\"></label>"
        "<button type=\"submit\">Connect</button></form></html>";
    char page[sizeof(page_template) + 16];
    int length = snprintf(page, sizeof(page), page_template, device_identity_name());
    if (length < 0 || (size_t)length >= sizeof(page)) return ESP_FAIL;
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
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
    if (!cap_matches(request->uri)) return httpd_resp_send_404(request);
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
    bool queued = xQueueSend(s_manager.queue, &command, 0) == pdTRUE;
    memset(&command, 0, sizeof(command));
    if (!queued) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Please retry in a moment");
    }
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "Connection test started. Check the monitor display.");
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

static void start_trial(const wifi_store_record_t *credentials)
{
    if (s_manager.trial_active) return;
    s_manager.pending_save = false;
    s_manager.link_has_ip = false;
    s_manager.trial = *credentials;
    s_manager.trial_active = true;
    s_manager.trial_deadline = esp_timer_get_time() + TRIAL_TIMEOUT_US;
    wifi_config_t station = {0};
    memcpy(station.sta.ssid, credentials->ssid, strlen(credentials->ssid));
    memcpy(station.sta.password, credentials->password, strlen(credentials->password));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_manager.trial_active = false;
        show_setup("Connection could not start. Try again.");
    } else show_setup("Testing the Wi-Fi connection...");
}

static void worker(void *arg)
{
    (void)arg;
    if (wifi_manager_is_provisioning()) {
        if (start_setup() != ESP_OK) ESP_LOGW(TAG, "Setup hotspot start failed");
    } else {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "Initial Wi-Fi connect failed: %s", esp_err_to_name(err));
        s_manager.next_retry = esp_timer_get_time() + 15000000LL;
    }
    for (;;) {
        command_t command = {0};
        bool received = xQueueReceive(s_manager.queue, &command, pdMS_TO_TICKS(1000)) == pdTRUE;
        int64_t now = esp_timer_get_time();
        if (received) {
            switch (command.kind) {
            case CMD_FORGET: {
                wifi_store_record_t forgotten = {.state = WIFI_STORE_FORGOTTEN};
                esp_err_t err = wifi_store_save(&forgotten);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not commit forgotten state: %s", esp_err_to_name(err));
                    atomic_store_explicit(&s_provisioning, false, memory_order_release);
                    display_status("Storage error", "Could not forget Wi-Fi");
                    break;
                }
                memset(&s_manager.selected, 0, sizeof(s_manager.selected));
                s_manager.pending_save = false;
                s_manager.setup_committed = false;
                s_manager.setup_authorized = true;
                xEventGroupClearBits(s_manager.events, OAUTH_WIFI_CONNECTED | OAUTH_WIFI_RECONNECTED);
                (void)esp_wifi_disconnect();
                wifi_config_t empty = {0};
                (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
                if (start_setup() != ESP_OK) ESP_LOGW(TAG, "Setup hotspot start failed");
                break;
            }
            case CMD_GOT_IP:
                s_manager.link_has_ip = true;
                if (s_manager.trial_active) {
                    s_manager.trial_active = false;
                    esp_err_t err = wifi_store_save(&s_manager.trial);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Could not save connected Wi-Fi: %s", esp_err_to_name(err));
                        s_manager.pending_save = true;
                        s_manager.save_retry_at = now + 5000000LL;
                        show_setup("Connected, but saving failed. Retrying...");
                    } else {
                        s_manager.selected = s_manager.trial;
                        memset(&s_manager.trial, 0, sizeof(s_manager.trial));
                        s_manager.setup_committed = true;
                        finish_setup();
                    }
                } else if (!wifi_manager_is_provisioning()) {
                    s_manager.retry_seconds = 2;
                    xEventGroupSetBits(s_manager.events, OAUTH_WIFI_CONNECTED | OAUTH_WIFI_RECONNECTED);
                }
                break;
            case CMD_DISCONNECTED:
                s_manager.link_has_ip = false;
                xEventGroupClearBits(s_manager.events, OAUTH_WIFI_CONNECTED);
                s_manager.pending_save = false;
                if (s_manager.trial_active) {
                    s_manager.trial_active = false;
                    show_setup("Connection failed. Check the details and retry.");
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
                if (s_manager.ap_started) {
                    s_manager.phone_joined = true;
                    show_setup("Enter your network details on the phone");
                }
                break;
            case CMD_PHONE_LEFT:
                if (s_manager.ap_started) {
                    s_manager.phone_joined = false;
                    show_setup("Join the temporary Wi-Fi network");
                }
                break;
            case CMD_SUBMIT:
                if (s_manager.ap_started && !s_manager.pending_save &&
                    !s_manager.setup_committed)
                    start_trial(&command.credentials);
                break;
            }
        }
        memset(&command, 0, sizeof(command));
        now = esp_timer_get_time();
        if (wifi_manager_is_provisioning()) {
            if (s_manager.setup_committed) {
                finish_setup();
            } else if (s_manager.setup_authorized && !s_manager.ap_started) {
                if (start_setup() != ESP_OK) vTaskDelay(pdMS_TO_TICKS(2000));
            } else if (s_manager.setup_authorized && !s_manager.server) {
                if (start_server() != ESP_OK) vTaskDelay(pdMS_TO_TICKS(2000));
            }
            if (s_manager.trial_active && now >= s_manager.trial_deadline) {
                s_manager.trial_active = false;
                (void)esp_wifi_disconnect();
                show_setup("Connection timed out. Try again.");
            }
            if (s_manager.pending_save && now >= s_manager.save_retry_at) {
                esp_err_t err = wifi_store_save(&s_manager.trial);
                if (err == ESP_OK) {
                    s_manager.selected = s_manager.trial;
                    memset(&s_manager.trial, 0, sizeof(s_manager.trial));
                    s_manager.setup_committed = true;
                    finish_setup();
                } else {
                    s_manager.save_retry_at = now + 5000000LL;
                    show_setup("Connected, but saving failed. Retrying...");
                }
            }
            if (s_manager.ap_started && !s_manager.setup_committed &&
                now >= s_manager.cap_deadline) refresh_capability();
            refresh_setup_url(now);
        } else if (!(xEventGroupGetBits(s_manager.events) & OAUTH_WIFI_CONNECTED) &&
                   now >= s_manager.next_retry) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(err));
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
    /* All subsequent radio changes are RAM-only. App credentials have their own record. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    if (source == WIFI_SOURCE_APP || source == WIFI_SOURCE_FALLBACK) {
        wifi_config_t station = {0};
        memcpy(station.sta.ssid, s_manager.selected.ssid, strlen(s_manager.selected.ssid));
        memcpy(station.sta.password, s_manager.selected.password,
               strlen(s_manager.selected.password));
        err = esp_wifi_set_config(WIFI_IF_STA, &station);
        if (err != ESP_OK) return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    if (source == WIFI_SOURCE_NONE) {
        s_manager.setup_authorized = true;
        atomic_store_explicit(&s_provisioning, true, memory_order_release);
    }
    if (xTaskCreate(worker, "wifi_manager", 8192, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    *events_out = s_manager.events;
    *sta_out = s_manager.sta;
    return ESP_OK;
}
