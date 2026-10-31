"""Run the production USB RPC parser with host Wi-Fi/OAuth service boundaries."""

import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
IDF = ROOT / ".deps" / "esp-idf-v5.1.7"
CJSON = IDF / "components" / "json" / "cJSON"
PREFIX = b"HELLO_BOX_RPC "


FAKE_SERVICES = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "box_config.h"
#include "codex_adapter.h"
#include "oauth_probe.h"
#include "wifi_manager.h"

static wifi_manager_status_t fake_wifi;
static oauth_management_status_t fake_oauth;
static codex_management_status_t fake_codex;
static esp_app_desc_t fake_app;
static bool queue_accept = true;
static unsigned request_count, direct_wifi_calls;
static uint32_t request_id;
static char request_method[24], request_arg1[64], request_arg2[513];

static void fill_text(char *out, size_t size, char byte)
{
    assert(size > 0);
    memset(out, byte, size - 1);
    out[size - 1] = '\0';
}

void test_reset(void)
{
    memset(&fake_wifi, 0, sizeof(fake_wifi));
    fake_wifi.configured = true;
    fake_wifi.connected = false;
    fake_wifi.provisioning = true;
    strcpy(fake_wifi.state, "failed");
    strcpy(fake_wifi.message, "Wi-Fi test failed; saved network retained");
    memset(&fake_oauth, 0, sizeof(fake_oauth));
    fake_oauth.configured = true;
    strcpy(fake_oauth.state, "checking");
    strcpy(fake_oauth.network.state, "offline");
    strcpy(fake_oauth.network.message, "Connect to Wi-Fi to test the network");
    strcpy(fake_oauth.operation.state, "done");
    memset(&fake_codex, 0, sizeof(fake_codex));
    strcpy(fake_codex.state, "not_configured");
    box_settings_t settings;
    box_config_defaults(&settings);
    box_config_set_current(&settings);
    strcpy(fake_app.version, "test-version");
    queue_accept = true;
    request_count = direct_wifi_calls = request_id = 0;
    memset(request_method, 0, sizeof(request_method));
    memset(request_arg1, 0, sizeof(request_arg1));
    memset(request_arg2, 0, sizeof(request_arg2));
}
void test_set_providers(unsigned providers)
{
    box_settings_t settings = *box_config_current();
    settings.providers = (uint8_t)providers;
    box_config_set_current(&settings);
}
void test_set_codex_usage(bool known, uint32_t count)
{
    fake_codex.configured = true;
    strcpy(fake_codex.state, "validated");
    fake_codex.usage.configured = true;
    fake_codex.usage.has_sample = true;
    fake_codex.usage.sampled_at = 123456789;
    fake_codex.usage.reset_credits_known = known;
    fake_codex.usage.reset_credits = count;
    fake_codex.usage.windows[0] = (quota_window_t){
        .valid = true, .used_percent = 42.5f, .resets_at = 123457000,
        .window_seconds = 18000,
    };
}
void test_fill_max_status(void)
{
    test_set_providers(2);
    fake_wifi.configured = fake_wifi.connected = fake_wifi.provisioning = true;
    fill_text(fake_wifi.state, sizeof(fake_wifi.state), 'w');
    fill_text(fake_wifi.message, sizeof(fake_wifi.message), 'W');
    fill_text(fake_oauth.state, sizeof(fake_oauth.state), 'c');
    fill_text(fake_oauth.session, sizeof(fake_oauth.session), 'S');
    fill_text(fake_oauth.authorization_url, sizeof(fake_oauth.authorization_url), 'U');
    fill_text(fake_oauth.network.state, sizeof(fake_oauth.network.state), 'n');
    fill_text(fake_oauth.network.message, sizeof(fake_oauth.network.message), 'N');
    fill_text(fake_oauth.operation.method, sizeof(fake_oauth.operation.method), 'm');
    fill_text(fake_oauth.operation.state, sizeof(fake_oauth.operation.state), 'o');
    fill_text(fake_oauth.operation.message, sizeof(fake_oauth.operation.message), 'O');
    fill_text(fake_oauth.usage.status, sizeof(fake_oauth.usage.status), 'u');
    fake_oauth.usage.has_sample = fake_oauth.usage.configured = true;
    fake_oauth.usage.windows[0] = (quota_window_t){true, 99.5f, 2000000000, 18000};
    fake_oauth.usage.windows[1] = (quota_window_t){true, 88.5f, 2000000001, 604800};
    fake_codex.configured = true;
    fill_text(fake_codex.state, sizeof(fake_codex.state), 'x');
    fill_text(fake_codex.verification_url, sizeof(fake_codex.verification_url), 'V');
    fill_text(fake_codex.user_code, sizeof(fake_codex.user_code), 'C');
    fill_text(fake_codex.usage.status, sizeof(fake_codex.usage.status), 'q');
    fill_text(fake_codex.operation.method, sizeof(fake_codex.operation.method), 'M');
    fill_text(fake_codex.operation.state, sizeof(fake_codex.operation.state), 'R');
    fill_text(fake_codex.operation.message, sizeof(fake_codex.operation.message), 'T');
    fake_codex.usage.has_sample = fake_codex.usage.configured = true;
    fake_codex.usage.reset_credits_known = true;
    fake_codex.usage.reset_credits = 7;
    fake_codex.usage.windows[0] = (quota_window_t){true, 42.5f, 2000000002, 18000};
    fake_codex.usage.windows[1] = (quota_window_t){true, 32.5f, 2000000003, 604800};
}
void test_set_queue_accept(bool accept) { queue_accept = accept; }
unsigned test_request_count(void) { return request_count; }
unsigned test_direct_wifi_calls(void) { return direct_wifi_calls; }
uint32_t test_request_id(void) { return request_id; }
const char *test_request_method(void) { return request_method; }
const char *test_request_arg1(void) { return request_arg1; }
const char *test_request_arg2(void) { return request_arg2; }
void test_set_wifi_link(bool connected)
{
    fake_wifi.connected = connected;
    if (connected) {
        strcpy(fake_wifi.state, "connected");
        strcpy(fake_wifi.message, "Wi-Fi connected");
    } else {
        strcpy(fake_wifi.state, "failed");
        strcpy(fake_wifi.message, "Wi-Fi test failed; saved network retained");
    }
}

const esp_app_desc_t *esp_app_get_description(void) { return &fake_app; }
const char *device_identity_name(void) { return "hello-box-4827"; }
void wifi_manager_get_status(wifi_manager_status_t *out) { *out = fake_wifi; }
bool wifi_manager_request_connect(uint32_t operation_id, const char *ssid, const char *password)
{
    (void)operation_id; (void)ssid; (void)password; ++direct_wifi_calls; return true;
}
bool wifi_store_valid_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password || !*ssid || strlen(ssid) > 32) return false;
    size_t n = strlen(password);
    if (!n) return true;
    if (n < 8 || n > 64) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)password[i] < 0x20 || (unsigned char)password[i] > 0x7e)
            return false;
    return true;
}
void oauth_probe_get_management_status(oauth_management_status_t *out) { *out = fake_oauth; }
void codex_adapter_get_status(codex_management_status_t *out) { *out = fake_codex; }
bool oauth_probe_management_request(uint32_t id, const char *method,
                                    const char *arg1, const char *arg2)
{
    ++request_count;
    request_id = id;
    snprintf(request_method, sizeof(request_method), "%s", method ? method : "");
    snprintf(request_arg1, sizeof(request_arg1), "%s", arg1 ? arg1 : "");
    snprintf(request_arg2, sizeof(request_arg2), "%s", arg2 ? arg2 : "");
    return queue_accept;
}
int esp_vfs_dev_uart_port_set_rx_line_endings(int port, int mode)
{ (void)port; (void)mode; return 0; }
int xTaskCreate(void *task, const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle)
{ (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return 1; }
void vTaskDelay(unsigned ticks) { (void)ticks; }
void vTaskDelete(void *task) { (void)task; }
'''


FAKE_SERVICES += r'''
#include "app_registry.h"
#include "screensaver_registry.h"
const app_registry_t *display_app_registry(void) {
    static const app_registry_t apps = {.count=2, .items={
        {.id=APP_ID_CLAUDE,.name="Claude",.capabilities=APP_CAP_REFRESH|APP_CAP_INTERRUPT_SAVER},
        {.id=APP_ID_CODEX,.name="Codex",.capabilities=APP_CAP_REFRESH|APP_CAP_INTERRUPT_SAVER}}};
    return &apps;
}
const screensaver_registry_t *display_screensaver_registry(void) {
    static const screensaver_registry_t savers = {.count=1,.items={
        {.id=SAVER_ID_WORD_CLOCK,.name="Word clock"}}};
    return &savers;
}
'''


class UsbManagementIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler or not (CJSON / "cJSON.c").is_file():
            raise unittest.SkipTest("host compiler and pinned cJSON source are required")
        cls.temp = tempfile.TemporaryDirectory(prefix="usb-management-")
        root = Path(cls.temp.name)
        (root / "freertos").mkdir()
        (root / "esp_netif.h").write_text(
            "#ifndef TEST_ESP_NETIF_H\n#define TEST_ESP_NETIF_H\n"
            "typedef struct esp_netif_t { int unused; } esp_netif_t;\n#endif\n")
        (root / "sdkconfig.h").write_text("#define CONFIG_HELLO_BOX_USB_TEST_HELPER 0\n")
        (root / "esp_app_desc.h").write_text(
            "typedef struct { char version[32]; } esp_app_desc_t;\n"
            "const esp_app_desc_t *esp_app_get_description(void);\n")
        (root / "esp_vfs_dev.h").write_text(
            "#define ESP_LINE_ENDINGS_LF 1\n"
            "int esp_vfs_dev_uart_port_set_rx_line_endings(int port, int mode);\n")
        (root / "freertos" / "FreeRTOS.h").write_text(
            "#define pdPASS 1\n#define pdMS_TO_TICKS(x) (x)\n"
            "#define BIT0 (1u << 0)\n#define BIT1 (1u << 1)\n")
        (root / "freertos" / "event_groups.h").write_text(
            "typedef void *EventGroupHandle_t;\n")
        (root / "freertos" / "task.h").write_text(
            "int xTaskCreate(void *, const char *, unsigned, void *, unsigned, void *);\n"
            "void vTaskDelay(unsigned);\nvoid vTaskDelete(void *);\n")
        fake_services = root / "fake_services.c"
        fake_services.write_text(FAKE_SERVICES)
        library = root / "libusb_management.so"
        result = subprocess.run(
            [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
             "-I", str(root), "-I", str(ROOT / "scripts" / "native"),
             "-I", str(ROOT / "main"), "-I", str(CJSON),
             str(ROOT / "main" / "usb_management.c"),
             str(ROOT / "main" / "box_config.c"), str(CJSON / "cJSON.c"),
             str(fake_services), "-o", str(library)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.usb_management_process_line.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                                        ctypes.c_char_p, ctypes.c_size_t]
        cls.lib.usb_management_process_line.restype = ctypes.c_bool
        cls.lib.test_reset.argtypes = []
        cls.lib.test_set_queue_accept.argtypes = [ctypes.c_bool]
        cls.lib.test_set_providers.argtypes = [ctypes.c_uint]
        cls.lib.test_set_codex_usage.argtypes = [ctypes.c_bool, ctypes.c_uint32]
        cls.lib.test_fill_max_status.argtypes = []
        cls.lib.test_set_wifi_link.argtypes = [ctypes.c_bool]
        cls.lib.test_request_count.restype = ctypes.c_uint
        cls.lib.test_direct_wifi_calls.restype = ctypes.c_uint
        cls.lib.test_request_id.restype = ctypes.c_uint32
        for name in ("test_request_method", "test_request_arg1", "test_request_arg2"):
            getattr(cls.lib, name).restype = ctypes.c_char_p

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.lib.test_reset()

    def rpc(self, method, params=None, request_id=1, size=4097):
        request = PREFIX + json.dumps({"id": request_id, "method": method,
                                      "params": params or {}}, separators=(",", ":"),
                                     ensure_ascii=False).encode("utf-8")
        response = ctypes.create_string_buffer(size)
        accepted = self.lib.usb_management_process_line(request, len(request), response, size)
        return accepted, response.value.decode("utf-8", errors="strict")

    def test_status_separates_saved_network_from_live_connection_and_hides_secrets(self):
        accepted, response = self.rpc("status.get")
        self.assertTrue(accepted)
        self.assertTrue(response.startswith(PREFIX.decode()))
        self.assertTrue(response.endswith("\n"))
        envelope = json.loads(response[len(PREFIX):])
        self.assertTrue(envelope["ok"])
        result = envelope["result"]
        self.assertEqual(result["deviceName"], "hello-box-4827")
        self.assertEqual(result["apps"], [
            {"id": 1, "name": "Claude", "canRefresh": True, "canInterruptScreensaver": True},
            {"id": 2, "name": "Codex", "canRefresh": True, "canInterruptScreensaver": True},
        ])
        self.assertEqual(result["screensavers"], [{"id": 1, "name": "Word clock"}])
        self.assertEqual(result["wifi"], {
            "configured": True, "operationId": 0, "connected": False, "provisioning": True,
            "state": "failed", "message": "Wi-Fi test failed; saved network retained",
        })
        self.assertTrue(result["claude"]["configured"])
        self.assertEqual(result["claude"]["state"], "checking",
                         "credentials can be present before the first usage validation")
        self.assertEqual(result["settings"]["providers"], "claude")
        self.assertFalse(result["codex"]["configured"])
        self.assertFalse(result["codex"]["usage"]["resetCreditsKnown"])
        self.assertEqual(result["codex"]["usage"]["resetCredits"], 0)
        self.assertFalse(result["networkTest"]["wifi"])
        self.assertFalse(result["networkTest"]["https"])
        for forbidden in ("password", "access_token", "refresh_token", "verifier", "capability"):
            self.assertNotIn(forbidden, response)
        self.assertNotIn("dummy-secret", response)

    def test_provider_selection_and_codex_reset_credits_keep_unknown_distinct(self):
        self.lib.test_set_providers(1)
        self.lib.test_set_codex_usage(False, 0)
        accepted, response = self.rpc("status.get")
        self.assertTrue(accepted)
        result = json.loads(response[len(PREFIX):])["result"]
        self.assertEqual(result["settings"]["providers"], "codex")
        self.assertTrue(result["codex"]["configured"])
        self.assertEqual(result["codex"]["usage"]["windows"][0], {
            "valid": True, "usedPercent": 42.5, "resetsAt": 123457000,
            "windowSeconds": 18000,
        })
        self.assertFalse(result["codex"]["usage"]["resetCreditsKnown"])
        self.assertEqual(result["codex"]["usage"]["resetCredits"], 0)

        self.lib.test_set_codex_usage(True, 3)
        self.lib.test_set_providers(2)
        accepted, response = self.rpc("status.get", request_id=2)
        self.assertTrue(accepted)
        result = json.loads(response[len(PREFIX):])["result"]
        self.assertEqual(result["settings"]["providers"], "both")
        self.assertTrue(result["claude"]["configured"])
        self.assertTrue(result["codex"]["configured"])
        self.assertTrue(result["codex"]["usage"]["resetCreditsKnown"])
        self.assertEqual(result["codex"]["usage"]["resetCredits"], 3)

    def test_maximum_public_both_provider_status_fits_response_and_hides_tokens(self):
        self.lib.test_fill_max_status()
        accepted, response = self.rpc("status.get", size=4097)
        self.assertTrue(accepted)
        self.assertLessEqual(len(response.encode("utf-8")) + 1, 4096)
        result = json.loads(response[len(PREFIX):])["result"]
        self.assertEqual(result["settings"]["providers"], "both")
        self.assertEqual(len(result["claude"]["authorizationUrl"]), 511)
        self.assertEqual(len(result["codex"]["verificationUrl"]), 127)
        self.assertEqual(len(result["codex"]["userCode"]), 31)
        for forbidden in ("access_token", "refresh_token", "device_auth_id",
                          "account_id", "verifier"):
            self.assertNotIn(forbidden, response)

        # A successful connection is a separate live fact, never inferred from saved credentials.
        self.lib.test_set_wifi_link(True)
        accepted, connected_response = self.rpc("status.get", request_id=2)
        self.assertTrue(accepted)
        wifi = json.loads(connected_response[len(PREFIX):])["result"]["wifi"]
        self.assertTrue(wifi["configured"] and wifi["connected"])
        self.assertTrue(wifi["provisioning"])

    def test_wifi_and_oauth_commands_are_queued_without_echoing_credentials(self):
        password = "dummy-only-pass"
        ssid = "Café-設備"
        accepted, response = self.rpc("wifi.connect", {"ssid": ssid, "password": password}, 41)
        self.assertTrue(accepted)
        envelope = json.loads(response[len(PREFIX):])
        self.assertEqual(envelope, {"id": 41, "ok": True,
                                    "result": {"accepted": True, "operationId": 41}})
        self.assertEqual(self.lib.test_request_count(), 1)
        self.assertEqual(self.lib.test_request_method(), b"wifi.connect")
        self.assertEqual(self.lib.test_request_arg1(), ssid.encode("utf-8"))
        self.assertEqual(self.lib.test_request_arg2(), password.encode())
        self.assertEqual(self.lib.test_direct_wifi_calls(), 0,
                         "USB parser must queue through the serialized OAuth/Wi-Fi workers")
        self.assertNotIn(password, response)
        self.assertNotIn(ssid, response)

        self.lib.test_reset()
        code = "dummy-auth-code-do-not-log"
        accepted, response = self.rpc("claude.submit_code", {"session": "session-1", "code": code}, 42)
        self.assertTrue(accepted)
        self.assertEqual(self.lib.test_request_method(), b"claude.submit_code")
        self.assertEqual(self.lib.test_request_arg1(), b"session-1")
        self.assertEqual(self.lib.test_request_arg2(), code.encode())
        self.assertNotIn(code, response)
        self.assertNotIn("session-1", response)

    def test_bounded_schema_crlf_and_queue_failure(self):
        for raw in (b"HELLO_BOX_RPC {\"id\":1,\"method\":\"status.get\",\"params\":{}}\r",
                    b"HELLO_BOX_RPC {\"id\":1,\"id\":2,\"method\":\"status.get\",\"params\":{}}",
                    b"HELLO_BOX_RPC {\"id\":1.5,\"method\":\"status.get\",\"params\":{}}"):
            response = ctypes.create_string_buffer(4097)
            self.assertFalse(self.lib.usb_management_process_line(raw, len(raw), response, len(response)))
            self.assertEqual(response.value, b"")
        for method, params in (("status.get", {"x": 1}),
                               ("wifi.connect", {"ssid": "x", "password": "short"})):
            accepted, response = self.rpc(method, params)
            self.assertTrue(accepted, "well-framed semantic errors must receive a protocol response")
            envelope = json.loads(response[len(PREFIX):])
            self.assertFalse(envelope["ok"])
            self.assertEqual(envelope["error"]["code"], "BAD_PARAMS")
        too_long = b"HELLO_BOX_RPC " + b" " * 800
        response = ctypes.create_string_buffer(4097)
        self.assertFalse(self.lib.usb_management_process_line(too_long, len(too_long),
                                                              response, len(response)))

        self.lib.test_set_queue_accept(False)
        accepted, response = self.rpc("network.test", request_id=8)
        self.assertTrue(accepted)
        envelope = json.loads(response[len(PREFIX):])
        self.assertFalse(envelope["ok"])
        self.assertEqual(envelope["error"]["code"], "BUSY")
        self.assertEqual(self.lib.test_request_count(), 1)

        response = ctypes.create_string_buffer(31)
        raw = PREFIX + b'{"id":1,"method":"status.get","params":{}}'
        self.assertFalse(self.lib.usb_management_process_line(raw, len(raw), response, len(response)))


if __name__ == "__main__":
    unittest.main()
