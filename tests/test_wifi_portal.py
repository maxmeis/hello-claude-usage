"""Exercise the production Wi-Fi portal handlers with host HTTP and queue stubs."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str) -> str:
    """Copy one complete function without depending on its successor's signature."""
    start = source.index(signature)
    opening = source.index("{", start + len(signature))
    depth = 0
    quote = None
    escaped = False
    line_comment = block_comment = False
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and next_char == "/":
                block_comment = False
                index += 1
        elif quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char == "/" and next_char == "/":
            line_comment = True
            index += 1
        elif char == "/" and next_char == "*":
            block_comment = True
            index += 1
        elif char in ('"', "'"):
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
        index += 1
    raise AssertionError(f"Unterminated C function: {signature}")


class WifiPortalTests(unittest.TestCase):
    def test_real_portal_handlers_validate_and_queue_without_echoing_credentials(self):
        manager = (ROOT / "main/wifi_manager.c").read_text()
        store = (ROOT / "main/wifi_store.c").read_text()
        functions = "\n".join([
            function(store, "static bool bounded_length("),
            function(store, "bool wifi_store_valid_credentials("),
            *(function(manager, signature) for signature in (
                "static bool cap_matches(", "static esp_err_t send_json(", "static esp_err_t portal_get(",
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
#define ESP_ERR_NO_MEM -2
#define HTTPD_RESP_USE_STRLEN -1
#define WIFI_STORE_CONFIGURED 1
#define CMD_SUBMIT __PRODUCTION_CMD_SUBMIT__
#define CMD_SCAN __PRODUCTION_CMD_SCAN__
#define BODY_LIMIT __PRODUCTION_BODY_LIMIT__
#define TRIAL_TIMEOUT_US __PRODUCTION_TRIAL_TIMEOUT__
#define SETUP_GRACE_US (15LL * 1000000LL)
#define SCAN_LIMIT 12
#define pdTRUE 1
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define atomic_load_explicit(p, order) (*(p))
typedef struct { int state; char ssid[33]; char password[65]; } wifi_store_record_t;
typedef struct { int kind; wifi_store_record_t credentials; unsigned attempt; char capability[33]; int64_t cap_deadline; } command_t;
typedef enum { RADIO_READY, RADIO_WAIT_STOP, RADIO_WAIT_START, RADIO_FAILED } radio_recovery_t;
typedef struct {
    const char *uri, *query; int content_len; const char *input; int read_at;
    char status[48], body[16000], cache[32], content_type[48];
} httpd_req_t;
typedef struct json_value {
    int type; char *name, *string; double number;
    struct json_value *child, *next;
} cJSON;
static struct { int64_t cap_deadline, status_grace_deadline; char cap[33]; int queue;
    unsigned attempt, queued_attempt; int http_busy, status_active;
    int trial_active, pending_save, setup_committed, tx_fault; radio_recovery_t radio_recovery;
    char status_state[16], status_message[96];
    char scan_state[12], networks[SCAN_LIMIT][33]; unsigned network_count;
} s_manager;
static int s_cap_mux, queued, queue_accept = 1;
static command_t last_command;
static int64_t mock_us;
static char *json_output;
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
static esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *out, size_t capacity) {
    const char *query = r->query;
    if (!query && r->uri) { const char *mark = strchr(r->uri, '?'); if (mark) query = mark + 1; }
    if (!query || strlen(query) >= capacity) return ESP_FAIL;
    strcpy(out, query); return ESP_OK;
}
static int xQueueSend(int queue, const command_t *command, int timeout) {
    assert(queue == 1 && timeout == 0);
    if (!queue_accept) return 0;
    last_command = *command; ++queued; return pdTRUE;
}
static cJSON *json_new(int type, const char *name) {
    cJSON *v = calloc(1, sizeof(*v)); assert(v); v->type = type;
    if (name) { v->name = strdup(name); assert(v->name); } return v;
}
static cJSON *cJSON_CreateObject(void) { return json_new(1, NULL); }
static cJSON *cJSON_AddArrayToObject(cJSON *o, const char *key) {
    cJSON *v = json_new(2, key); v->next = o->child; o->child = v; return v;
}
static void json_add(cJSON *o, cJSON *v) { v->next = o->child; o->child = v; }
static void cJSON_AddNumberToObject(cJSON *o, const char *key, double n) {
    cJSON *v = json_new(3, key); v->number = n; json_add(o, v);
}
static void cJSON_AddStringToObject(cJSON *o, const char *key, const char *s) {
    cJSON *v = json_new(4, key); v->string = strdup(s); assert(v->string); json_add(o, v);
}
static void cJSON_AddItemToArray(cJSON *a, cJSON *v) { v->next = a->child; a->child = v; }
static cJSON *cJSON_CreateString(const char *s) {
    cJSON *v = json_new(4, NULL); v->string = strdup(s); assert(v->string); return v;
}
static void cJSON_Delete(cJSON *v) {
    while (v) { cJSON *next = v->next; cJSON_Delete(v->child); free(v->name); free(v->string); free(v); v = next; }
}
static void append(char *out, size_t cap, size_t *n, const char *s) {
    size_t len = strlen(s); assert(*n + len < cap); memcpy(out + *n, s, len + 1); *n += len;
}
static void json_string(char *out, size_t cap, size_t *n, const char *s) {
    append(out, cap, n, "\"");
    for (; *s; ++s) {
        char escaped[7] = {0};
        if (*s == '\\' || *s == '\"') { escaped[0] = '\\'; escaped[1] = *s; }
        else if ((unsigned char)*s < 0x20) snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char)*s);
        else { escaped[0] = *s; }
        append(out, cap, n, escaped);
    }
    append(out, cap, n, "\"");
}
static void json_value(char *out, size_t cap, size_t *n, const cJSON *v) {
    if (v->type == 1 || v->type == 2) {
        append(out, cap, n, v->type == 1 ? "{" : "[");
        for (const cJSON *c = v->child; c; c = c->next) {
            if (c != v->child) append(out, cap, n, ",");
            if (v->type == 1) { json_string(out, cap, n, c->name); append(out, cap, n, ":"); }
            json_value(out, cap, n, c);
        }
        append(out, cap, n, v->type == 1 ? "}" : "]");
    } else if (v->type == 3) {
        char number[32]; snprintf(number, sizeof(number), "%.0f", v->number); append(out, cap, n, number);
    } else json_string(out, cap, n, v->string);
}
static char *cJSON_PrintUnformatted(const cJSON *v) {
    free(json_output); json_output = calloc(1, 4096); assert(json_output); size_t n = 0;
    json_value(json_output, 4096, &n, v); return json_output;
}
static void cJSON_free(void *v) { (void)v; }
static esp_err_t httpd_resp_send_404(httpd_req_t *r);
#include "wifi_portal_page.h"
'''
        cases = r'''
static const char *valid_uri = "/w/0123456789abcdef0123456789abcdef";
static httpd_req_t request(const char *uri, const char *body) {
    httpd_req_t r = {.uri = uri, .input = body};
    r.content_len = body ? (int)strlen(body) : 0;
    return r;
}
int main(void) {
    assert(TRIAL_TIMEOUT_US == 25LL * 1000000LL && "initial Wi-Fi phase allows the SDK handshake window");
    strcpy(s_manager.cap, "0123456789abcdef0123456789abcdef");
    s_manager.cap_deadline = 600000000;
    s_manager.queue = 1;
    httpd_req_t get = request(valid_uri, NULL);
    assert(portal_get(&get) == ESP_OK);
    assert(strstr(get.body, "#manualBox{display:none}") != NULL);
    assert(strstr(get.body, "Enter network name manually") != NULL);
    assert(strstr(get.body, "createElement('option')") != NULL);
    assert(!strstr(get.body, "innerHTML") && "SSID options must be inserted as text, not markup");
    assert(!strcmp(get.cache, "no-store"));
    assert(!strcmp(get.content_type, "text/html; charset=utf-8"));

    strcpy(s_manager.status_state, "testing");
    strcpy(s_manager.status_message, "Checking router connection and IP address");
    s_manager.attempt = 17;
    httpd_req_t status = request("/w/0123456789abcdef0123456789abcdef/status", NULL);
    assert(portal_get(&status) == ESP_OK);
    assert(!strcmp(status.content_type, "application/json") && !strcmp(status.cache, "no-store"));
    assert(strstr(status.body, "\"attempt\":17") && strstr(status.body, "\"state\":\"testing\""));
    assert(strstr(status.body, "\"device\":\"hello-box-4821\""));
    assert(strstr(status.body, "Checking router connection and IP address"));

    strcpy(s_manager.scan_state, "ready");
    strcpy(s_manager.networks[0], "Guest \"<Wi-Fi>\"");
    s_manager.network_count = 1;
    httpd_req_t scan = request("/w/0123456789abcdef0123456789abcdef/scan", NULL);
    assert(portal_get(&scan) == ESP_OK);
    assert(strstr(scan.body, "\"state\":\"ready\""));
    assert(strstr(scan.body, "Guest \\\"<Wi-Fi>\\\""));
    scan = request("/w/0123456789abcdef0123456789abcdef/scan?refresh=1", NULL);
    assert(portal_get(&scan) == ESP_OK && queued == 1 && last_command.kind == CMD_SCAN);
    assert(strstr(scan.body, "\"state\":\"scanning\""));
    strcpy(s_manager.scan_state, "ready");
    scan = request("/w/0123456789abcdef0123456789abcdef/scan", NULL);
    scan.query = "refresh=1";
    assert(portal_get(&scan) == ESP_OK && queued == 2 && last_command.kind == CMD_SCAN);
    httpd_req_t bad_query = request("/w/0123456789abcdef0123456789abcdef/scan?anything=else", NULL);
    assert(portal_get(&bad_query) == ESP_OK && !strcmp(bad_query.status, "404 Not Found"));
    httpd_req_t repeated_scan = request("/w/0123456789abcdef0123456789abcdef/scan", NULL);
    assert(portal_get(&repeated_scan) == ESP_OK && queued == 2);
    queued = 0; s_manager.scan_state[0] = 0;
    s_manager.attempt = 0;

    httpd_req_t ok = request(valid_uri,
        "ssid=Demo+Wi-Fi&password=%21%40%23%24%25%5E%26%2A%28%29%5F%2B-%3D%5B%5D%7B%7D%7C%3B%3A%27%2C.%3C%3E%2F%3F%7E");
    assert(portal_post(&ok) == ESP_OK && !strcmp(ok.status, "202 Accepted"));
    assert(queued == 1 && last_command.kind == CMD_SUBMIT);
    assert(strstr(ok.body, "\"attempt\":1") && !strstr(ok.body, "!@#$%^&*()_+-=[]{}|;:',.<>/?~"));
    assert(!strcmp(last_command.credentials.ssid, "Demo Wi-Fi"));
    assert(!strcmp(last_command.credentials.password, "!@#$%^&*()_+-=[]{}|;:',.<>/?~"));
    assert(!strstr(ok.body, "!@#$%^&*()_+-=[]{}|;:',.<>/?~") && !strstr(ok.body, "Demo Wi-Fi"));

    char worst[320] = "ssid=";
    for (int i = 0; i < 32; ++i) strcat(worst, "%2B");
    strcat(worst, "&password=");
    for (int i = 0; i < 63; ++i) strcat(worst, "%21");
    assert(strlen(worst) > 256 && strlen(worst) < BODY_LIMIT);
    httpd_req_t encoded = request(valid_uri, worst);
    /* A queued attempt blocks another submission until its state transition completes. */
    s_manager.queued_attempt = 0;
    s_manager.http_busy = 0;
    s_manager.status_state[0] = 0;
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
        s_manager.queued_attempt = 0;
        s_manager.http_busy = 0;
        s_manager.status_state[0] = 0;
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
    httpd_req_t suffix_status = request("/w/0123456789abcdef0123456789abcdef/status/", NULL);
    assert(portal_get(&suffix_status) == ESP_OK && !strcmp(suffix_status.status, "404 Not Found"));
    mock_us = s_manager.cap_deadline;
    httpd_req_t expired = request(valid_uri, "ssid=Demo&password=demo-pass123");
    assert(portal_post(&expired) == ESP_OK && !strcmp(expired.status, "404 Not Found"));
    assert(queued == 2);
    httpd_req_t expired_status = request("/w/0123456789abcdef0123456789abcdef/status", NULL);
    s_manager.http_busy = 1; s_manager.attempt = 2;
    s_manager.status_active = 1;
    strcpy(s_manager.status_state, "testing");
    assert(portal_get(&expired_status) == ESP_OK && !strcmp(expired_status.content_type, "application/json"));
    assert(strstr(expired_status.body, "\"attempt\":2") && strstr(expired_status.body, "\"state\":\"testing\""));
    s_manager.status_active = 0;
    s_manager.status_grace_deadline = s_manager.cap_deadline + SETUP_GRACE_US;
    httpd_req_t grace_status = request("/w/0123456789abcdef0123456789abcdef/status", NULL);
    assert(portal_get(&grace_status) == ESP_OK && strstr(grace_status.body, "\"attempt\":2"));
    httpd_req_t expired_scan = request("/w/0123456789abcdef0123456789abcdef/scan", NULL);
    assert(portal_get(&expired_scan) == ESP_OK && !strcmp(expired_scan.status, "404 Not Found"));
    mock_us = s_manager.status_grace_deadline;
    httpd_req_t grace_ended = request("/w/0123456789abcdef0123456789abcdef/status", NULL);
    assert(portal_get(&grace_ended) == ESP_OK && !strcmp(grace_ended.status, "404 Not Found"));

    mock_us = 0; queue_accept = 0;
    strcpy(s_manager.cap, "fedcba9876543210fedcba9876543210");
    httpd_req_t rotated = request(valid_uri, NULL);
    assert(portal_get(&rotated) == ESP_OK && !strcmp(rotated.status, "404 Not Found"));
    strcpy(s_manager.cap, "0123456789abcdef0123456789abcdef");
    s_manager.scan_state[0] = 0;
    httpd_req_t scan_queue_full = request("/w/0123456789abcdef0123456789abcdef/scan?refresh=1", NULL);
    assert(portal_get(&scan_queue_full) == ESP_OK && strstr(scan_queue_full.body, "\"state\":\"error\""));
    s_manager.queued_attempt = 0;
    s_manager.http_busy = 0;
    s_manager.status_state[0] = 0;
    httpd_req_t busy = request(valid_uri, "ssid=Demo&password=demo-pass123");
    assert(portal_post(&busy) == ESP_OK && !strcmp(busy.status, "503 Service Unavailable"));
    assert(queued == 2);

    queue_accept = 1; s_manager.queued_attempt = 0; s_manager.http_busy = 1;
    httpd_req_t conflict = request(valid_uri, "ssid=Demo&password=demo-pass123");
    assert(portal_post(&conflict) == ESP_OK && !strcmp(conflict.status, "409 Conflict"));
    assert(queued == 2 && !strstr(conflict.body, "demo-pass123"));
    s_manager.http_busy = 0;
    s_manager.radio_recovery = RADIO_WAIT_START;
    httpd_req_t recovery_busy = request(valid_uri, "ssid=Demo&password=demo-pass123");
    assert(portal_post(&recovery_busy) == ESP_OK && !strcmp(recovery_busy.status, "409 Conflict"));
    s_manager.radio_recovery = RADIO_FAILED;
    httpd_req_t recovery_failed = request(valid_uri, "ssid=Demo&password=demo-pass123");
    assert(portal_post(&recovery_failed) == ESP_OK && !strcmp(recovery_failed.status, "503 Service Unavailable"));
    assert(strstr(recovery_failed.body, "Restart the device") && !strstr(recovery_failed.body, "demo-pass123"));
    s_manager.radio_recovery = RADIO_READY;
    s_manager.tx_fault = 1;
    unsigned fault_attempt = s_manager.attempt;
    httpd_req_t tx_fault_post = request(valid_uri, "ssid=Demo&password=demo-pass123");
    assert(portal_post(&tx_fault_post) == ESP_OK && !strcmp(tx_fault_post.status, "503 Service Unavailable"));
    assert(strstr(tx_fault_post.body, "Wi-Fi radio unavailable") &&
           !strstr(tx_fault_post.body, "demo-pass123") && queued == 2 &&
           !s_manager.queued_attempt && s_manager.attempt == fault_attempt);
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
            enum_match = re.search(r"typedef enum \{([^}]+)\} command_kind_t;", manager, re.S)
            self.assertIsNotNone(enum_match, "manager command enum is the test/queue contract")
            commands = re.findall(r"\b(CMD_[A-Z_]+)\b", enum_match.group(1))
            submit_value = str(commands.index("CMD_SUBMIT"))
            scan_value = str(commands.index("CMD_SCAN"))
            self.assertGreaterEqual(int(limit_match.group(1)), 301)
            source.write_text(scaffold.replace("__PRODUCTION_BODY_LIMIT__", limit_match.group(1))
                              .replace("__PRODUCTION_TRIAL_TIMEOUT__", timeout_match.group(1))
                              .replace("__PRODUCTION_CMD_SUBMIT__", submit_value)
                              .replace("__PRODUCTION_CMD_SCAN__", scan_value) + functions + cases)
            compiler = shutil.which("clang") or shutil.which("cc")
            if not compiler:
                self.skipTest("A C compiler is required for the host handler test")
            subprocess.run([compiler, "-std=gnu11", "-fsanitize=address,undefined", "-I",
                            str(ROOT / "main"), str(source),
                            "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Wi-Fi portal:", result.stdout)


if __name__ == "__main__":
    unittest.main()
