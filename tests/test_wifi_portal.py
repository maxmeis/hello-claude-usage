"""Exercise the production Wi-Fi portal handlers with host HTTP and queue stubs."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str) -> str:
    """Copy a complete top-level C function, including its production body."""
    start = source.index(signature)
    following = re.search(r"\n(?:static )?(?:bool|wifi_source_t|esp_err_t) [A-Za-z_]+\(",
                          source[start + len(signature):])
    if following is None:
        raise AssertionError(f"No following function after {signature}")
    end = start + len(signature) + following.start()
    return source[start:end]


class WifiPortalTests(unittest.TestCase):
    def test_real_portal_handlers_validate_and_queue_without_echoing_credentials(self):
        manager = (ROOT / "main/wifi_manager.c").read_text()
        store = (ROOT / "main/wifi_store.c").read_text()
        functions = "\n".join([
            function(store, "static bool bounded_length("),
            function(store, "bool wifi_store_valid_credentials("),
            *(function(manager, signature) for signature in (
                "static bool cap_matches(", "static esp_err_t portal_get(",
                "static bool decode_field(", "static bool parse_form(",
                "static esp_err_t portal_post(")),
        ])
        scaffold = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_RESP_USE_STRLEN -1
#define WIFI_STORE_CONFIGURED 1
#define CMD_SUBMIT 5
#define BODY_LIMIT __PRODUCTION_BODY_LIMIT__
#define TRIAL_TIMEOUT_US __PRODUCTION_TRIAL_TIMEOUT__
#define pdTRUE 1
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef struct { int state; char ssid[33]; char password[65]; } wifi_store_record_t;
typedef struct { int kind; wifi_store_record_t credentials; } command_t;
typedef struct {
    const char *uri; int content_len; const char *input; int read_at;
    char status[48], body[4096], cache[32], content_type[48];
} httpd_req_t;
static struct { int64_t cap_deadline; char cap[33]; int queue; } s_manager;
static int s_cap_mux, queued, queue_accept = 1;
static command_t last_command;
static int64_t mock_us;
static const char *device_identity_name(void) { return "hello-box-4821"; }
static int64_t esp_timer_get_time(void) { return mock_us; }
static esp_err_t httpd_resp_send_404(httpd_req_t *r) {
    strcpy(r->status, "404 Not Found"); return ESP_OK;
}
static void httpd_resp_set_type(httpd_req_t *r, const char *value) {
    snprintf(r->content_type, sizeof(r->content_type), "%s", value);
}
static void httpd_resp_set_hdr(httpd_req_t *r, const char *key, const char *value) {
    if (!strcmp(key, "Cache-Control")) snprintf(r->cache, sizeof(r->cache), "%s", value);
}
static void httpd_resp_set_status(httpd_req_t *r, const char *value) {
    snprintf(r->status, sizeof(r->status), "%s", value);
}
static esp_err_t httpd_resp_send(httpd_req_t *r, const char *body, int length) {
    if (length == HTTPD_RESP_USE_STRLEN) length = (int)strlen(body);
    assert(length >= 0 && length < (int)sizeof(r->body));
    memcpy(r->body, body, (size_t)length); r->body[length] = 0; return ESP_OK;
}
static esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *body) {
    return httpd_resp_send(r, body, HTTPD_RESP_USE_STRLEN);
}
static int httpd_req_recv(httpd_req_t *r, char *out, size_t count) {
    int left = r->content_len - r->read_at;
    if (left <= 0) return 0;
    if (count > 7) count = 7; /* Exercise a split HTTP body. */
    if ((int)count > left) count = (size_t)left;
    memcpy(out, r->input + r->read_at, count);
    r->read_at += (int)count;
    return (int)count;
}
static int xQueueSend(int queue, const command_t *command, int timeout) {
    assert(queue == 1 && timeout == 0);
    if (!queue_accept) return 0;
    last_command = *command; ++queued; return pdTRUE;
}
'''
        cases = r'''
static const char *valid_uri = "/w/0123456789abcdef0123456789abcdef";
static httpd_req_t request(const char *uri, const char *body) {
    httpd_req_t r = {.uri = uri, .input = body};
    r.content_len = body ? (int)strlen(body) : 0;
    return r;
}
int main(void) {
    assert(TRIAL_TIMEOUT_US == 20LL * 1000000LL && "Wi-Fi trial must be 20 seconds");
    strcpy(s_manager.cap, "0123456789abcdef0123456789abcdef");
    s_manager.cap_deadline = 600000000;
    s_manager.queue = 1;
    httpd_req_t get = request(valid_uri, NULL);
    assert(portal_get(&get) == ESP_OK);
    assert(strstr(get.body, "<form method=\"post\">") != NULL);
    assert(strstr(get.body, "Connect hello-box-4821 to Wi-Fi") != NULL);
    assert(strstr(get.body, "width:100%;") != NULL);
    assert(strstr(get.body, "name=\"ssid\"") != NULL);
    assert(strstr(get.body, "type=\"password\"") != NULL);
    assert(!strcmp(get.cache, "no-store"));
    assert(!strcmp(get.content_type, "text/html; charset=utf-8"));

    httpd_req_t ok = request(valid_uri, "ssid=Demo+Wi-Fi&password=demo-pass123");
    assert(portal_post(&ok) == ESP_OK && !strcmp(ok.status, "202 Accepted"));
    assert(queued == 1 && last_command.kind == CMD_SUBMIT);
    assert(!strcmp(last_command.credentials.ssid, "Demo Wi-Fi"));
    assert(!strcmp(last_command.credentials.password, "demo-pass123"));
    assert(!strstr(ok.body, "demo-pass123") && !strstr(ok.body, "Demo Wi-Fi"));

    char worst[320] = "ssid=";
    for (int i = 0; i < 32; ++i) strcat(worst, "%2B");
    strcat(worst, "&password=");
    for (int i = 0; i < 63; ++i) strcat(worst, "%21");
    assert(strlen(worst) > 256 && strlen(worst) < BODY_LIMIT);
    httpd_req_t encoded = request(valid_uri, worst);
    assert(portal_post(&encoded) == ESP_OK && !strcmp(encoded.status, "202 Accepted"));
    assert(queued == 2);
    assert(strlen(last_command.credentials.ssid) == 32);
    assert(strlen(last_command.credentials.password) == 63);
    for (int i = 0; i < 32; ++i) assert(last_command.credentials.ssid[i] == '+');
    for (int i = 0; i < 63; ++i) assert(last_command.credentials.password[i] == '!');
    assert(!strstr(encoded.body, "password") && !strstr(encoded.body, "ssid"));

    const char *bad[] = {
        "ssid=&password=demo-pass123", "ssid=Demo&password=short",
        "ssid=Demo&password=%00badpass", "ssid=Demo&password=%Q1badpass",
        "ssid=Demo&password=demo-pass123&ssid=Again",
        "password=demo-pass123", "ssid=Demo", "ssid=Demo&password=demo-pass123&x=1",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        httpd_req_t r = request(valid_uri, bad[i]);
        assert(portal_post(&r) == ESP_OK && !strcmp(r.status, "400 Bad Request"));
        assert(queued == 2);
        assert(!strstr(r.body, "demo-pass123"));
    }
    httpd_req_t empty = request(valid_uri, "");
    assert(portal_post(&empty) == ESP_OK && !strcmp(empty.status, "413 Payload Too Large"));
    char huge[BODY_LIMIT + 2]; memset(huge, 'A', BODY_LIMIT + 1);
    huge[BODY_LIMIT + 1] = 0;
    httpd_req_t large = request(valid_uri, huge);
    assert(portal_post(&large) == ESP_OK && !strcmp(large.status, "413 Payload Too Large"));
    assert(queued == 2);

    httpd_req_t wrong = request("/w/0123456789abcdef0123456789abcdee", "ssid=Demo&password=demo-pass123");
    assert(portal_post(&wrong) == ESP_OK && !strcmp(wrong.status, "404 Not Found"));
    httpd_req_t suffix = request("/w/0123456789abcdef0123456789abcdef/", NULL);
    assert(portal_get(&suffix) == ESP_OK && !strcmp(suffix.status, "404 Not Found"));
    mock_us = s_manager.cap_deadline;
    httpd_req_t expired = request(valid_uri, "ssid=Demo&password=demo-pass123");
    assert(portal_post(&expired) == ESP_OK && !strcmp(expired.status, "404 Not Found"));
    assert(queued == 2);

    mock_us = 0; queue_accept = 0;
    httpd_req_t busy = request(valid_uri, "ssid=Demo&password=demo-pass123");
    assert(portal_post(&busy) == ESP_OK && !strcmp(busy.status, "503 Service Unavailable"));
    assert(queued == 2);
    puts("Wi-Fi portal: GET, form decode, queue, bounds, capability, expiry passed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="wifi-portal-") as directory:
            source = Path(directory) / "portal.c"
            executable = Path(directory) / "portal"
            limit_match = re.search(r"^#define BODY_LIMIT (\d+)$", manager, re.MULTILINE)
            timeout_match = re.search(r"^#define TRIAL_TIMEOUT_US ([^\n]+)$", manager,
                                      re.MULTILINE)
            self.assertIsNotNone(limit_match)
            self.assertIsNotNone(timeout_match)
            self.assertGreaterEqual(int(limit_match.group(1)), 301)
            source.write_text(scaffold.replace("__PRODUCTION_BODY_LIMIT__", limit_match.group(1))
                              .replace("__PRODUCTION_TRIAL_TIMEOUT__", timeout_match.group(1)) +
                              functions + cases)
            subprocess.run(["clang", "-std=gnu11", "-fsanitize=address,undefined", str(source),
                            "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
            self.assertIn("Wi-Fi portal:", result.stdout)


if __name__ == "__main__":
    unittest.main()
