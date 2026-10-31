"""Host execution checks for production mDNS lifecycle and setup URLs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def c_function(source: str, signature: str) -> str:
    """Extract one production C function, including nested strings and blocks."""
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
        elif char == "/" and next_char == "*":
            block_comment = True
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

ESP_ERR = """\
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_FOUND 0x105
"""

MDNS = """\
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define MDNS_NAME_BUF_LEN 65
esp_err_t mdns_init(void);
void mdns_free(void);
esp_err_t mdns_hostname_set(const char *hostname);
esp_err_t mdns_hostname_get(char *hostname);
esp_err_t mdns_instance_name_set(const char *instance_name);
esp_err_t mdns_service_add(const char *instance_name, const char *service_type,
                           const char *proto, uint16_t port, void *txt, size_t num_items);
esp_err_t mdns_service_remove(const char *service_type, const char *proto);
"""

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device_discovery.c"

static const char *identity = "hello-box-4821";
static char hostname[MDNS_NAME_BUF_LEN], instance[MDNS_NAME_BUF_LEN];
static int init_calls, free_calls, hostname_sets, instance_sets, hostname_gets;
static int add_calls, remove_calls, held;
static uint16_t advertised_port;
static bool fail_init, fail_hostname_set, fail_instance_set, fail_hostname_get, fail_add;
static bool fail_remove;

const char *device_identity_name(void) { return identity; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
int xSemaphoreTake(SemaphoreHandle_t lock, int timeout) {
    assert(lock == (SemaphoreHandle_t)1 && timeout == portMAX_DELAY && !held);
    held = 1; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t lock) {
    assert(lock == (SemaphoreHandle_t)1 && held);
    held = 0; return 1;
}
esp_err_t mdns_init(void) { ++init_calls; return fail_init ? ESP_FAIL : ESP_OK; }
void mdns_free(void) { ++free_calls; hostname[0] = instance[0] = 0; }
esp_err_t mdns_hostname_set(const char *name) {
    ++hostname_sets;
    if (fail_hostname_set) return ESP_FAIL;
    snprintf(hostname, sizeof(hostname), "%s", name); return ESP_OK;
}
esp_err_t mdns_instance_name_set(const char *name) {
    ++instance_sets;
    if (fail_instance_set) return ESP_FAIL;
    snprintf(instance, sizeof(instance), "%s", name); return ESP_OK;
}
esp_err_t mdns_hostname_get(char *out) {
    ++hostname_gets;
    if (fail_hostname_get) return ESP_FAIL;
    snprintf(out, MDNS_NAME_BUF_LEN, "%s", hostname); return ESP_OK;
}
esp_err_t mdns_service_add(const char *name, const char *type, const char *proto,
                           uint16_t port, void *txt, size_t count) {
    assert(name == NULL && !strcmp(type, "_http") && !strcmp(proto, "_tcp"));
    assert(txt == NULL && count == 0 && "setup secrets must not appear in TXT");
    ++add_calls; advertised_port = port;
    return fail_add ? ESP_FAIL : ESP_OK;
}
esp_err_t mdns_service_remove(const char *type, const char *proto) {
    assert(!strcmp(type, "_http") && !strcmp(proto, "_tcp"));
    ++remove_calls; return fail_remove ? ESP_FAIL : ESP_OK;
}

static void check_url(const char *expected, const char *route, const char *cap,
                      const char *ip) {
    char out[128];
    assert(device_discovery_setup_url(out, sizeof(out), route, cap, ip));
    assert(!strcmp(out, expected));
}

int main(void) {
    char out[128];
    assert(!device_discovery_ready());
    assert(device_discovery_http_start(8080) == ESP_ERR_INVALID_STATE);
    device_discovery_http_stop();
    check_url("http://192.168.4.1:8080/w/abc_123", "w", "abc_123", "192.168.4.1");

    identity = NULL;
    assert(device_discovery_start() == ESP_ERR_INVALID_STATE && init_calls == 0);
    identity = "hello-box-4821";
    fail_init = true;
    assert(device_discovery_start() == ESP_FAIL && !device_discovery_ready());
    assert(add_calls == 0 && device_discovery_http_start(8080) == ESP_ERR_INVALID_STATE);
    check_url("http://192.168.4.1:8080/w/abc_123", "w", "abc_123", "192.168.4.1");
    fail_init = false; fail_hostname_set = true;
    assert(device_discovery_start() == ESP_FAIL && !device_discovery_ready());
    fail_hostname_set = false; fail_instance_set = true;
    assert(device_discovery_start() == ESP_FAIL && !device_discovery_ready());
    fail_instance_set = false;
    assert(device_discovery_start() == ESP_OK && device_discovery_ready());
    assert(!strcmp(hostname, identity) && !strcmp(instance, identity));
    assert(init_calls == 4 && free_calls == 2);
    assert(device_discovery_start() == ESP_OK && init_calls == 4);
    check_url("http://hello-box-4821.local:8080/s/abc_123", "s", "abc_123", "10.0.0.4");

    assert(device_discovery_http_start(0) == ESP_ERR_INVALID_STATE);
    fail_add = true;
    assert(device_discovery_http_start(8080) == ESP_FAIL && add_calls == 1);
    device_discovery_http_stop();
    assert(remove_calls == 0);
    fail_add = false;
    assert(device_discovery_http_start(8080) == ESP_OK);
    assert(add_calls == 2 && advertised_port == 8080);
    assert(device_discovery_http_start(8080) == ESP_OK && add_calls == 2);
    device_discovery_http_stop();
    assert(remove_calls == 1);
    device_discovery_http_stop();
    assert(remove_calls == 1);
    assert(device_discovery_http_start(8080) == ESP_OK && add_calls == 3);
    device_discovery_http_stop();
    assert(remove_calls == 2);

    assert(device_discovery_http_start(8080) == ESP_OK && add_calls == 4);
    fail_remove = true;
    device_discovery_http_stop();
    assert(remove_calls == 3 && free_calls == 3 && !device_discovery_ready());
    fail_remove = false;
    assert(device_discovery_start() == ESP_OK && device_discovery_ready());
    assert(init_calls == 5);

    strcpy(hostname, "hello-box-9999");
    assert(!device_discovery_ready());
    assert(device_discovery_http_start(8080) == ESP_ERR_INVALID_STATE && add_calls == 4);
    check_url("http://10.0.0.4:8080/s/abc_123", "s", "abc_123", "10.0.0.4");
    fail_hostname_get = true;
    check_url("http://10.0.0.4:8080/s/abc_123", "s", "abc_123", "10.0.0.4");
    fail_hostname_get = false;
    strcpy(hostname, identity);

    const char *bad[] = {"", "/s", "a/b", "a?b", "a#b", "a b", "a.b", "a:b"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        strcpy(out, "stale");
        assert(!device_discovery_setup_url(out, sizeof(out), bad[i], "cap", "10.0.0.4"));
        assert(out[0] == 0);
        strcpy(out, "stale");
        assert(!device_discovery_setup_url(out, sizeof(out), "s", bad[i], "10.0.0.4"));
        assert(out[0] == 0);
    }
    assert(!device_discovery_setup_url(out, sizeof(out), NULL, "cap", "10.0.0.4"));
    assert(!device_discovery_setup_url(out, sizeof(out), "s", NULL, "10.0.0.4"));
    assert(!device_discovery_setup_url(out, sizeof(out), "s", "cap", NULL));
    assert(!device_discovery_setup_url(out, sizeof(out), "s", "cap", ""));
    assert(!device_discovery_setup_url(NULL, sizeof(out), "s", "cap", "10.0.0.4"));
    assert(!device_discovery_setup_url(out, 0, "s", "cap", "10.0.0.4"));
    char exact[sizeof("http://hello-box-4821.local:8080/s/cap")];
    assert(device_discovery_setup_url(exact, sizeof(exact), "s", "cap", "10.0.0.4"));
    assert(!strcmp(exact, "http://hello-box-4821.local:8080/s/cap"));
    char short_out[sizeof(exact) - 1];
    memset(short_out, 'X', sizeof(short_out));
    assert(!device_discovery_setup_url(short_out, sizeof(short_out), "s", "cap", "10.0.0.4"));
    assert(short_out[0] == 0);
    char long_cap[200]; memset(long_cap, 'a', sizeof(long_cap) - 1);
    long_cap[sizeof(long_cap) - 1] = 0;
    assert(!device_discovery_setup_url(out, sizeof(out), "s", long_cap, "10.0.0.4"));
    assert(out[0] == 0);
    assert(held == 0 && hostname_gets > 0 && hostname_sets == 4 && instance_sets == 3);
    return 0;
}
'''

REFRESH_COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_OAUTH_PROBE_HTTP_PORT 8080
static bool mdns_ready = true;
static const char *device_identity_name(void) { return "hello-box-4821"; }
static bool device_discovery_ready(void) { return mdns_ready; }
'''

WIFI_REFRESH_HARNESS = r'''
typedef struct {
    void *server;
    bool ap_started, setup_committed, phone_joined;
    int64_t cap_deadline;
    char cap[33], page_url[128];
    const char *setup_message;
} manager_t;
static manager_t s_manager;
static bool provisioning = true;
static int display_calls;
static char displayed_url[128], displayed_message[96];
static bool wifi_manager_is_provisioning(void) { return provisioning; }
static void show_setup(const char *message) {
    ++display_calls;
    snprintf(displayed_url, sizeof(displayed_url), "%s", s_manager.page_url);
    snprintf(displayed_message, sizeof(displayed_message), "%s", message);
}
__PRODUCTION__
int main(void) {
    strcpy(s_manager.cap, "0123456789abcdef0123456789abcdef");
    s_manager.cap_deadline = 600000000;
    s_manager.ap_started = s_manager.phone_joined = true;
    s_manager.server = (void *)1;
    s_manager.setup_message = "Enter network details";
    assert(device_discovery_setup_url(s_manager.page_url, sizeof(s_manager.page_url),
                                      "w", s_manager.cap, "192.168.4.1"));
    mdns_ready = false; /* mDNS renamed this host after the QR was generated. */
    refresh_setup_url(1000000);
    assert(!strcmp(s_manager.page_url,
                   "http://192.168.4.1:8080/w/0123456789abcdef0123456789abcdef"));
    assert(display_calls == 1 && !strcmp(displayed_url, s_manager.page_url));
    assert(!strcmp(displayed_message, "Enter network details"));
    assert(s_manager.cap_deadline == 600000000);
    assert(!strcmp(s_manager.cap, "0123456789abcdef0123456789abcdef"));
    refresh_setup_url(2000000);
    assert(display_calls == 1);
    mdns_ready = true;
    refresh_setup_url(3000000);
    assert(!strcmp(s_manager.page_url,
                   "http://hello-box-4821.local:8080/w/0123456789abcdef0123456789abcdef"));
    assert(display_calls == 2);
    s_manager.phone_joined = false;
    mdns_ready = false;
    refresh_setup_url(4000000);
    assert(display_calls == 2 && strstr(s_manager.page_url, "192.168.4.1"));
    s_manager.phone_joined = true;
    s_manager.server = NULL;
    mdns_ready = true;
    refresh_setup_url(5000000);
    assert(display_calls == 2 && strstr(s_manager.page_url, "192.168.4.1"));
    s_manager.server = (void *)1;
    refresh_setup_url(s_manager.cap_deadline);
    assert(display_calls == 2 && strstr(s_manager.page_url, "192.168.4.1"));
    s_manager.cap_deadline += 600000000;
    s_manager.setup_committed = true;
    refresh_setup_url(6000000);
    assert(display_calls == 2);
    s_manager.setup_committed = false;
    provisioning = false;
    refresh_setup_url(6000000);
    assert(display_calls == 2);
    return 0;
}
'''

ACCOUNT_REFRESH_HARNESS = r'''
#define ESP_OK 0
#define ESP_FAIL -1
#define portMAX_DELAY -1
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (ip)->bytes[0], (ip)->bytes[1], (ip)->bytes[2], (ip)->bytes[3]
typedef int esp_err_t;
typedef struct { uint32_t addr; uint8_t bytes[4]; } ip4_t;
typedef struct { ip4_t ip; } esp_netif_ip_info_t;
static void *s_server = (void *)1, *s_sta = (void *)2;
static char s_setup_url[128], s_cap[48], s_auth_url[512], s_reset_nonce[48];
static bool s_cap_valid = true, s_manual_session, s_auto_qr_visible = true;
static bool provisioning, connected = true, wanted = true, ip_valid = true;
static uint8_t last_ip_octet = 4;
static uint32_t s_manual_request_id, s_reset_nonce_session;
static int64_t s_cap_deadline = 600, s_reset_nonce_deadline_us, clock_s = 1;
static int s_cap_lock, s_submissions, auto_calls, manual_calls, close_calls, http_stops;
static uint32_t displayed_request_id;
static char displayed_url[128];
static int64_t now_s(void) { return clock_s; }
static bool wifi_manager_is_provisioning(void) { return provisioning; }
static bool wifi_up(void) { return connected; }
static bool settings_wanted(uint32_t id) { return wanted && id == s_manual_request_id; }
static void xSemaphoreTake(int lock, int timeout) {
    assert(lock == 0 && timeout == portMAX_DELAY);
}
static void xSemaphoreGive(int lock) { assert(lock == 0); }
static esp_err_t esp_netif_get_ip_info(void *netif, esp_netif_ip_info_t *out) {
    assert(netif == (void *)2);
    if (!ip_valid) return ESP_FAIL;
    out->ip = (ip4_t){.addr = 1, .bytes = {10, 0, 0, last_ip_octet}};
    return ESP_OK;
}
static void display_setup(const char *url) {
    ++auto_calls; snprintf(displayed_url, sizeof(displayed_url), "%s", url);
}
static void display_settings_qr(uint32_t id, const char *url) {
    ++manual_calls; displayed_request_id = id;
    snprintf(displayed_url, sizeof(displayed_url), "%s", url);
}
static void display_settings_closed(uint32_t id) {
    ++close_calls; displayed_request_id = id;
}
static void device_discovery_http_stop(void) { assert(s_server == (void *)1); }
static void httpd_stop(void *server) { assert(server == (void *)1); ++http_stops; }
static void xQueueReset(int queue) { assert(queue == 0); }
__PRODUCTION__
int main(void) {
    strcpy(s_cap, "0123456789abcdef0123456789abcdef");
    strcpy(s_auth_url, "authorization-still-valid");
    assert(device_discovery_setup_url(s_setup_url, sizeof(s_setup_url),
                                      "s", s_cap, "10.0.0.4"));
    mdns_ready = false;
    refresh_setup_url(false);
    assert(auto_calls == 1 && manual_calls == 0);
    assert(!strcmp(s_setup_url,
                   "http://10.0.0.4:8080/s/0123456789abcdef0123456789abcdef"));
    assert(!strcmp(displayed_url, s_setup_url));
    assert(s_cap_deadline == 600 && !strcmp(s_auth_url, "authorization-still-valid"));
    assert(!strcmp(s_cap, "0123456789abcdef0123456789abcdef"));
    refresh_setup_url(false);
    assert(auto_calls == 1);
    last_ip_octet = 7;
    refresh_setup_url(false);
    assert(auto_calls == 2 && strstr(s_setup_url, "10.0.0.7"));
    assert(s_cap_deadline == 600 && !strcmp(s_cap, "0123456789abcdef0123456789abcdef"));
    mdns_ready = true;
    refresh_setup_url(false);
    assert(auto_calls == 3 && strstr(s_setup_url, "hello-box-4821.local"));
    refresh_setup_url(true); /* The account completed; do not reopen its QR. */
    assert(auto_calls == 3);
    s_auto_qr_visible = false;
    mdns_ready = false;
    refresh_setup_url(false);
    assert(auto_calls == 3 && strstr(s_setup_url, "hello-box-4821.local"));

    s_manual_session = true;
    s_manual_request_id = 42;
    wanted = true;
    refresh_setup_url(true);
    assert(manual_calls == 1 && displayed_request_id == 42);
    assert(strstr(s_setup_url, "10.0.0.7") && !strcmp(displayed_url, s_setup_url));
    wanted = false; /* Back was requested before the worker closes the page. */
    mdns_ready = true;
    refresh_setup_url(true);
    assert(manual_calls == 1 && strstr(s_setup_url, "10.0.0.7"));
    wanted = true;
    clock_s = s_cap_deadline;
    refresh_setup_url(true);
    assert(manual_calls == 1 && strstr(s_setup_url, "10.0.0.7"));
    clock_s = 1;
    stop_setup();
    assert(!s_cap_valid && !s_cap[0] && !s_setup_url[0]);
    assert(!s_auto_qr_visible && !s_server && http_stops == 1);
    assert(close_calls == 1 && displayed_request_id == 42);
    refresh_setup_url(true);
    assert(manual_calls == 1 && auto_calls == 3);
    return 0;
}
'''


class DeviceDiscoveryTests(unittest.TestCase):
    def run_refresh_harness(self, program: str):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang is required for ASan/UBSan host tests")
        with tempfile.TemporaryDirectory(prefix="discovery-refresh-") as temp:
            source = Path(temp) / "refresh.c"
            executable = Path(temp) / "refresh"
            source.write_text(program)
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 str(source), "-o", str(executable)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 check=False)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_production_c_lifecycle_fallback_validation_and_bounds(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang is required for ASan/UBSan host tests")
        with tempfile.TemporaryDirectory(prefix="device-discovery-") as temp:
            directory = Path(temp)
            for name, content in {
                "esp_err.h": ESP_ERR,
                "mdns.h": MDNS,
                "sdkconfig.h": "#define CONFIG_OAUTH_PROBE_HTTP_PORT 8080\n",
                "freertos/FreeRTOS.h": "#pragma once\n#define portMAX_DELAY -1\n",
                "freertos/semphr.h": (
                    "#pragma once\n"
                    "typedef void *SemaphoreHandle_t;\n"
                    "SemaphoreHandle_t xSemaphoreCreateMutex(void);\n"
                    "int xSemaphoreTake(SemaphoreHandle_t, int);\n"
                    "int xSemaphoreGive(SemaphoreHandle_t);\n"
                ),
                "device_discovery.c": (ROOT / "main/device_discovery.c").read_text(),
                "device_discovery.h": (ROOT / "main/device_discovery.h").read_text(),
                "device_identity.h": (ROOT / "main/device_identity.h").read_text(),
                "harness.c": HARNESS,
            }.items():
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            executable = directory / "discovery"
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(directory), str(directory / "harness.c"),
                 "-o", str(executable)], capture_output=True, text=True, check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 check=False)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(run.stdout + run.stderr, "", "discovery must not log setup secrets")

    def test_active_wifi_setup_refreshes_same_cap_after_hostname_collision(self):
        discovery = (ROOT / "main/device_discovery.c").read_text()
        wifi = (ROOT / "main/wifi_manager.c").read_text()
        production = "\n\n".join(c_function(discovery, signature) for signature in (
            "static bool valid_segment(",
            "bool device_discovery_setup_url(",
            "bool device_discovery_update_url(",
        )) + "\n\n" + c_function(wifi, "static void refresh_setup_url(")
        worker = c_function(wifi, "static void worker(")
        self.assertIn("refresh_setup_url(now);", worker)
        self.assertIn("pdMS_TO_TICKS(s_manager.tx_retry_pending ? 100 : 1000)", worker)
        self.run_refresh_harness(REFRESH_COMMON +
                                 WIFI_REFRESH_HARNESS.replace("__PRODUCTION__", production))

    def test_active_account_auto_and_manual_refresh_without_late_reopen(self):
        discovery = (ROOT / "main/device_discovery.c").read_text()
        account = (ROOT / "main/oauth_probe.c").read_text()
        production = "\n\n".join(c_function(discovery, signature) for signature in (
            "static bool valid_segment(",
            "bool device_discovery_setup_url(",
            "bool device_discovery_update_url(",
        )) + "\n\n" + "\n\n".join(c_function(account, signature) for signature in (
            "static void stop_setup(",
            "static void refresh_setup_url(",
        ))
        worker = c_function(account, "static void worker(")
        self.assertLess(worker.index("if (s_server && now_s() >= s_cap_deadline)"),
                        worker.index("refresh_setup_url(have_token);"))
        self.run_refresh_harness(REFRESH_COMMON +
                                 ACCOUNT_REFRESH_HARNESS.replace("__PRODUCTION__", production))


if __name__ == "__main__":
    unittest.main()
