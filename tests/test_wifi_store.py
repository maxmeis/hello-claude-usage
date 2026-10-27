"""Host checks for the actual Wi-Fi record parser, selection, and commit policy."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

ESP_ERR = r'''
#ifndef ESP_ERR_H
#define ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
'''

NVS = r'''
#ifndef NVS_H
#define NVS_H
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 1
#define NVS_READWRITE 2
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
#endif
'''

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "wifi_store.h"
#include "nvs.h"

static unsigned char durable[256], staged[256];
static size_t durable_len, staged_len;
static bool commit_fails;
static int commit_calls;

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(strcmp(name, "wifi_app") == 0);
    if (mode == NVS_READONLY && durable_len == 0) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length) {
    assert(handle == 1 && strcmp(key, "record") == 0);
    if (!durable_len) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < durable_len) return ESP_FAIL;
    memcpy(value, durable, durable_len);
    *length = durable_len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
    assert(handle == 1 && strcmp(key, "record") == 0 && length <= sizeof(staged));
    memcpy(staged, value, length);
    staged_len = length;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1);
    ++commit_calls;
    if (commit_fails) return ESP_FAIL;
    memcpy(durable, staged, staged_len);
    durable_len = staged_len;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

int main(void) {
    wifi_store_record_t record = {0}, selected = {0};
    assert(wifi_store_load(&record) == ESP_OK && record.state == WIFI_STORE_ABSENT);
    assert(wifi_store_select(&record, "driver", "driverpass", "fallback", "fallbackpass", &selected) == WIFI_SOURCE_DRIVER);
    assert(strcmp(selected.ssid, "driver") == 0);
    assert(wifi_store_select(&record, "", "", "fallback", "fallbackpass", &selected) == WIFI_SOURCE_FALLBACK);
    assert(strcmp(selected.ssid, "fallback") == 0);

    record.state = WIFI_STORE_CONFIGURED;
    strcpy(record.ssid, "new-network");
    strcpy(record.password, "new-password");
    assert(wifi_store_save(&record) == ESP_OK && commit_calls == 1);
    memset(&record, 0, sizeof(record));
    assert(wifi_store_load(&record) == ESP_OK);
    assert(wifi_store_select(&record, "driver", "driverpass", "fallback", "fallbackpass", &selected) == WIFI_SOURCE_APP);
    assert(strcmp(selected.ssid, "new-network") == 0);

    wifi_store_record_t forgotten = {.state = WIFI_STORE_FORGOTTEN};
    commit_fails = true;
    assert(wifi_store_save(&forgotten) == ESP_FAIL && commit_calls == 2);
    assert(wifi_store_load(&record) == ESP_OK && record.state == WIFI_STORE_CONFIGURED);
    commit_fails = false;
    assert(wifi_store_save(&forgotten) == ESP_OK && commit_calls == 3);
    assert(wifi_store_load(&record) == ESP_OK && record.state == WIFI_STORE_FORGOTTEN);
    assert(wifi_store_select(&record, "driver", "driverpass", "fallback", "fallbackpass", &selected) == WIFI_SOURCE_NONE);
    assert(selected.ssid[0] == 0 && selected.password[0] == 0);

    assert(wifi_store_valid_credentials("open", ""));
    assert(wifi_store_valid_credentials("secured", "12345678"));
    assert(wifi_store_valid_credentials("secured", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    assert(!wifi_store_valid_credentials("", "12345678"));
    assert(!wifi_store_valid_credentials("secured", "short"));
    assert(!wifi_store_valid_credentials("bad\nssid", "12345678"));
    assert(!wifi_store_valid_credentials("secured", "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg"));
    char long_ssid[34]; memset(long_ssid, 'x', 33); long_ssid[33] = 0;
    assert(!wifi_store_valid_credentials(long_ssid, "12345678"));
    return 0;
}
'''


class WifiStoreTests(unittest.TestCase):
    def test_native_record_selection_validation_and_commit(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("No C compiler")
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "esp_err.h").write_text(ESP_ERR)
            (directory / "nvs.h").write_text(NVS)
            (directory / "harness.c").write_text(HARNESS)
            executable = directory / "wifi_store_test"
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(directory),
                 "-I", str(ROOT / "main"), str(ROOT / "main" / "wifi_store.c"),
                 str(directory / "harness.c"), "-o", str(executable)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, check=False)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
