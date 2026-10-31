"""Regression for long Codex Bearer headers with the pinned ESP-IDF 5.1.1 client."""

from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IDF = Path("/Users/max/.platformio/packages/framework-espidf")


def production_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    quote = None
    escaped = False
    line_comment = block_comment = False
    index = opening
    while index < len(source):
        char = source[index]
        nxt = source[index + 1] if index + 1 < len(source) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and nxt == "/":
                block_comment = False
                index += 1
        elif quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char == "/" and nxt == "/":
            line_comment = True
            index += 1
        elif char == "/" and nxt == "*":
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
    raise AssertionError(f"Unterminated production function: {signature}")


ESP_ERR_H = r"""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_FOUND 0x105
"""
ESP_LOG_H = r"""#pragma once
#define ESP_LOGE(tag, format, ...) do { (void)(tag); (void)(format); } while (0)
#define ESP_LOGW(tag, format, ...) do { (void)(tag); (void)(format); } while (0)
"""
ESP_CHECK_H = r"""#pragma once
#define ESP_RETURN_ON_FALSE(condition, error, tag, format, ...) \
    do { if (!(condition)) return (error); } while (0)
#define ESP_GOTO_ON_FALSE(condition, error, label, tag, format, ...) \
    do { if (!(condition)) { ret = (error); goto label; } } while (0)
"""

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_err.h"
#include "esp_log.h"
#include "http_header.h"

#define CODEX_ACCESS_SIZE 4096
#define DEFAULT_HTTP_BUF_SIZE DEFAULT_TX_SIZE
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define HTTP_EVENT_ON_DATA 1
#define HTTP_EVENT_ON_HEADER 2
#define RESPONSE_LIMIT 64
#define pdMS_TO_TICKS(ms) (ms)
static const char *TAG = "codex-http-test";
typedef struct { char access[CODEX_ACCESS_SIZE]; char account_id[64]; } codex_tokens_t;
typedef struct { char body[RESPONSE_LIMIT + 1]; size_t length; uint32_t retry_after; bool overflow; } http_response_t;
typedef struct {
    int event_id, data_len;
    const char *data, *header_key, *header_value;
    void *user_data;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handler_t)(esp_http_client_event_t *event);
typedef struct {
    const char *url;
    int method, timeout_ms;
    void *crt_bundle_attach;
    int buffer_size_tx;
    http_event_handler_t event_handler;
    void *user_data;
    bool disable_auto_redirect;
} esp_http_client_config_t;
typedef struct fake_client {
    http_header_handle_t headers;
    esp_http_client_config_t config;
} fake_client_t;
typedef fake_client_t *esp_http_client_handle_t;

static esp_http_client_config_t captured_config;
static char actual_wire[8192], legacy_wire[8192];
static size_t actual_wire_len, legacy_wire_len;
static const int default_tx_size = DEFAULT_HTTP_BUF_SIZE;
static void *esp_crt_bundle_attach = (void *)1;

static size_t serialize_headers(http_header_handle_t headers, int tx_size,
                                char *wire, size_t wire_size)
{
    char frame[8192] = {0};
    const char *request_line = "GET /backend-api/wham/usage HTTP/1.1\r\n";
    int first_line_len = (int)strlen(request_line);
    assert(tx_size > first_line_len && (size_t)tx_size <= sizeof(frame));
    memcpy(frame, request_line, (size_t)first_line_len);
    int offset = first_line_len;
    int header_index = 0;
    int wlen = tx_size - first_line_len;
    size_t used = 0;
    /* Same pagination and termination logic as esp_http_client_request_send. */
    while ((header_index = http_header_generate_string(headers, header_index,
                                                        frame + offset, &wlen))) {
        if (wlen <= 0) break;
        if (offset) {
            wlen += offset;
            offset = 0;
        }
        assert(used + (size_t)wlen <= wire_size);
        memcpy(wire + used, frame, (size_t)wlen);
        used += (size_t)wlen;
        wlen = tx_size;
    }
    wire[used] = 0;
    return used;
}

static esp_err_t http_event(esp_http_client_event_t *event) { (void)event; return ESP_OK; }
static bool network_ready(void) { return true; }
static bool provider_http_take(unsigned ticks) { (void)ticks; return true; }
static void provider_http_give(void) { }
static bool wifi_manager_is_provisioning(void) { return false; }

static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    fake_client_t *client = calloc(1, sizeof(*client));
    assert(client);
    client->config = *config;
    captured_config = *config;
    client->headers = http_header_init();
    assert(client->headers);
    return client;
}
static esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                             const char *key, const char *value)
{
    assert(http_header_set(client->headers, key, value) == ESP_OK);
    return ESP_OK;
}
static esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
                                                const char *data, int length)
{ (void)client; (void)data; (void)length; return ESP_OK; }
static esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    int effective_tx = client->config.buffer_size_tx
        ? client->config.buffer_size_tx : default_tx_size;
    actual_wire_len = serialize_headers(client->headers, effective_tx,
                                        actual_wire, sizeof(actual_wire));
    legacy_wire_len = serialize_headers(client->headers, default_tx_size,
                                        legacy_wire, sizeof(legacy_wire));
    return ESP_OK;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t client)
{ (void)client; return 200; }
static esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    if (!client) return ESP_OK;
    http_header_destroy(client->headers);
    memset(client, 0, sizeof(*client));
    free(client);
    return ESP_OK;
}

PRODUCTION_FUNCTION

int main(void)
{
    const size_t lengths[] = {600, 2000, CODEX_ACCESS_SIZE - 1};
    for (size_t test = 0; test < sizeof(lengths) / sizeof(lengths[0]); ++test) {
        codex_tokens_t token = {0};
        memset(token.access, 'S', lengths[test]); /* synthetic token bytes only */
        token.access[lengths[test]] = 0;
        strcpy(token.account_id, "synthetic-account");
        http_response_t response = {0};
        assert(http_request("https://example.invalid/backend-api/wham/usage",
                            NULL, NULL, &token, &response) == 200);
        assert(captured_config.buffer_size_tx == CODEX_ACCESS_SIZE + 128);
        assert(captured_config.disable_auto_redirect);

        char expected[CODEX_ACCESS_SIZE + 32];
        int expected_len = snprintf(expected, sizeof(expected),
                                    "Authorization: Bearer %s", token.access);
        assert(expected_len > 0 && (size_t)expected_len < sizeof(expected));
        const char *header = strstr(actual_wire, "Authorization: Bearer ");
        assert(header && strncmp(header, expected, (size_t)expected_len) == 0);
        assert(header[expected_len] == '\r' && header[expected_len + 1] == '\n');
        assert(actual_wire_len >= 4 &&
               memcmp(actual_wire + actual_wire_len - 4, "\r\n\r\n", 4) == 0);

        /* IDF 5.1.1 emits fitting headers only; it cannot split one long line. */
        assert(legacy_wire_len > 0);
        assert(strstr(legacy_wire, "Authorization: Bearer ") == NULL);
        assert(legacy_wire_len < 4 ||
               memcmp(legacy_wire + legacy_wire_len - 4, "\r\n\r\n", 4) != 0);
    }

    http_response_t response = {0};
    assert(http_request("https://example.invalid/backend-api/wham/usage",
                        NULL, NULL, NULL, &response) == 200);
    assert(captured_config.buffer_size_tx == 0);
    assert(actual_wire_len > 4 &&
           memcmp(actual_wire + actual_wire_len - 4, "\r\n\r\n", 4) == 0);
    assert(strstr(actual_wire, "Authorization:") == NULL);
    assert(strstr(actual_wire, "Accept: application/json\r\n") != NULL);
    return 0;
}
'''


class CodexHttpHeaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang")
        if not compiler:
            raise unittest.SkipTest("clang is required for the ASan/UBSan HTTP test")
        idf = Path(os.environ.get("IDF_PATH", str(DEFAULT_IDF)))
        version_path = idf / "version.txt"
        version = version_path.read_text().strip() if version_path.exists() else ""
        if version != "5.1.1":
            raise unittest.SkipTest(f"pinned ESP-IDF 5.1.1 source unavailable (found {version!r})")

        component = idf / "components/esp_http_client"
        header_source = component / "lib/http_header.c"
        utils_source = component / "lib/http_utils.c"
        header_include = component / "lib/include/http_header.h"
        client_header = component / "include/esp_http_client.h"
        for path in (header_source, utils_source, header_include, client_header):
            if not path.is_file():
                raise AssertionError(f"missing pinned ESP-IDF source: {path}")
        default_match = re.search(
            r"^#define\s+DEFAULT_HTTP_BUF_SIZE\s+\(?\s*(\d+)\s*\)?",
            client_header.read_text(), re.MULTILINE)
        if not default_match:
            raise AssertionError("could not find SDK default TX buffer size")
        if int(default_match.group(1)) != 512:
            raise AssertionError("pinned SDK default TX buffer size changed; update the regression deliberately")

        adapter = (ROOT / "main/codex_adapter.c").read_text()
        request = production_function(adapter,
                                      "static int http_request(const char *url, const char *post,")
        harness = HARNESS.replace("DEFAULT_TX_SIZE", default_match.group(1))
        harness = harness.replace("PRODUCTION_FUNCTION", request)

        cls.temp = tempfile.TemporaryDirectory(prefix="codex-http-header-")
        temp = Path(cls.temp.name)
        (temp / "esp_err.h").write_text(ESP_ERR_H)
        (temp / "esp_log.h").write_text(ESP_LOG_H)
        (temp / "esp_check.h").write_text(ESP_CHECK_H)
        harness_c = temp / "test_codex_http.c"
        harness_c.write_text(harness)
        binary = temp / "test_codex_http"
        command = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-g",
                   "-I", str(temp), "-I", str(component / "lib/include"),
                   str(harness_c), str(header_source), str(utils_source),
                   "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode:
            cls.temp.cleanup()
            raise AssertionError("ASan/UBSan Codex HTTP harness compile failed:\n" + build.stderr)
        cls.binary = binary

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def test_production_request_preserves_long_bearer_header_and_default_anonymous_tx(self):
        run = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "")


if __name__ == "__main__":
    unittest.main()
