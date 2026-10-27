"""Native tests of the production reset journal and boot storage path."""

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
#include <stdint.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 1
#define NVS_READWRITE 2
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
#endif
'''

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "factory_reset.h"
#include "wifi_store.h"
#include "nvs.h"

enum { RESET, CLAUDE, IDENTITY, WIFI, OTHER, COUNT };
static const char *names[] = {"factory_reset", "claude_probe", "hello_box", "wifi_app", "sdk_other"};
typedef struct { bool present; bool key; uint8_t data[160]; size_t size; } ns_t;
static ns_t durable[COUNT], staged[COUNT];
static int mutation, fail_at, erase_calls;
static bool journal_read_fails;

static bool fail_now(void) { return fail_at && ++mutation == fail_at; }
static int index_of(const char *name) {
    for (int i = 0; i < COUNT; ++i) if (!strcmp(name, names[i])) return i;
    assert(false); return 0;
}
static bool key_matches(int i, const char *key) {
    return !strcmp(key, i == RESET ? "pending" : i == WIFI ? "record" : "data");
}
static void reboot(void) {
    memcpy(staged, durable, sizeof(staged)); fail_at = mutation = 0;
    journal_read_fails = false;
}
static void seed(int i, uint8_t value) {
    durable[i].present = durable[i].key = true;
    durable[i].data[0] = value; durable[i].size = 1;
    reboot();
}
static void seed_settings(void) {
    memset(durable, 0, sizeof(durable)); reboot();
    seed(CLAUDE, 42); seed(IDENTITY, 43); seed(OTHER, 44);
    wifi_store_record_t wifi = {.state = WIFI_STORE_CONFIGURED};
    strcpy(wifi.ssid, "old-network"); strcpy(wifi.password, "old-password");
    assert(wifi_store_save(&wifi) == ESP_OK);
    reboot(); erase_calls = 0;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    int i = index_of(name);
    if (mode == NVS_READONLY && !durable[i].present) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) staged[i].present = true;
    *handle = i + 1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value) {
    int i = handle - 1; assert(i == RESET && key_matches(i, key));
    if (journal_read_fails) return ESP_FAIL;
    if (!durable[i].key) return ESP_ERR_NVS_NOT_FOUND;
    if (durable[i].size != 1) return ESP_ERR_INVALID_STATE;
    *value = durable[i].data[0]; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    int i = handle - 1; assert(i == RESET && key_matches(i, key));
    if (fail_now()) return ESP_FAIL;
    staged[i].key = true; staged[i].data[0] = value; staged[i].size = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length) {
    int i = handle - 1; assert(i == WIFI && key_matches(i, key));
    if (!durable[i].key) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < durable[i].size) return ESP_FAIL;
    memcpy(value, durable[i].data, durable[i].size);
    *length = durable[i].size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
    int i = handle - 1; assert(i == WIFI && key_matches(i, key) && length <= sizeof(staged[i].data));
    if (fail_now()) return ESP_FAIL;
    staged[i].key = true; memcpy(staged[i].data, value, length); staged[i].size = length;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    int i = handle - 1; assert(i == RESET && key_matches(i, key));
    if (!staged[i].key) return ESP_ERR_NVS_NOT_FOUND;
    if (fail_now()) return ESP_FAIL;
    staged[i].key = false; staged[i].size = 0; return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t handle) {
    int i = handle - 1; assert(i == CLAUDE || i == IDENTITY);
    ++erase_calls;
    if (fail_now()) return ESP_FAIL;
    staged[i].key = false; staged[i].size = 0; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    int i = handle - 1;
    if (fail_now()) return ESP_FAIL;
    durable[i] = staged[i]; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle >= 1 && handle <= COUNT); }

static void assert_pending(bool expected) {
    bool pending = !expected;
    assert(factory_reset_is_pending(&pending) == ESP_OK && pending == expected);
}
static void assert_reset_state(void) {
    assert(!durable[CLAUDE].key && !durable[IDENTITY].key);
    assert(durable[OTHER].key && durable[OTHER].data[0] == 44);
    wifi_store_record_t wifi;
    assert(wifi_store_load(&wifi) == ESP_OK && wifi.state == WIFI_STORE_FORGOTTEN);
    wifi_store_record_t selected;
    assert(wifi_store_select(&wifi, "old-driver", "password", "compiled", "password",
                             &selected) == WIFI_SOURCE_NONE);
}
int main(void) {
    seed_settings();
    assert_pending(false);
    assert(factory_reset_prepare_boot() == ESP_OK && erase_calls == 0);
    assert(factory_reset_complete() == ESP_OK && erase_calls == 0);
    assert(factory_reset_is_pending(NULL) == ESP_ERR_INVALID_ARG);

    /* Failed journal writes leave every setting intact and authorize no erasure. */
    for (int fail = 1; fail <= 2; ++fail) {
        seed_settings(); fail_at = fail; mutation = 0;
        assert(factory_reset_request() == ESP_FAIL);
        reboot(); assert_pending(false);
        assert(factory_reset_prepare_boot() == ESP_OK && erase_calls == 0);
        assert(durable[CLAUDE].key && durable[IDENTITY].key && durable[WIFI].key);
    }

    /* Invalid journal values fail closed. */
    seed_settings(); seed(RESET, 2);
    assert(factory_reset_prepare_boot() == ESP_ERR_INVALID_STATE && erase_calls == 0);
    assert(factory_reset_request() == ESP_ERR_INVALID_STATE);
    durable[RESET].size = 2; reboot();
    assert(factory_reset_prepare_boot() == ESP_ERR_INVALID_STATE && erase_calls == 0);
    reboot(); journal_read_fails = true;
    assert(factory_reset_prepare_boot() == ESP_FAIL && erase_calls == 0);
    assert(factory_reset_request() == ESP_FAIL && erase_calls == 0);

    /* Every interrupted erase or write can resume from durable journal state. */
    for (int fail = 1; fail <= 6; ++fail) {
        seed_settings(); assert(factory_reset_request() == ESP_OK); reboot();
        assert_pending(true);
        assert(factory_reset_request() == ESP_OK && mutation == 0);
        fail_at = fail; mutation = 0;
        assert(factory_reset_prepare_boot() == ESP_FAIL);
        assert_pending(true);
        reboot(); assert(factory_reset_prepare_boot() == ESP_OK);
        assert_reset_state(); assert_pending(true);
        assert(factory_reset_prepare_boot() == ESP_OK); /* Repeated boot is safe. */
        assert_reset_state();
        assert(factory_reset_complete() == ESP_OK); assert_pending(false);
    }

    /* Namespaces already absent still allow the reset to finish. */
    seed_settings(); durable[CLAUDE] = (ns_t){0}; durable[IDENTITY] = (ns_t){0}; reboot();
    assert(factory_reset_request() == ESP_OK);
    assert(factory_reset_prepare_boot() == ESP_OK); assert_reset_state();
    assert(factory_reset_complete() == ESP_OK); assert_pending(false);

    /* Failed journal erase or commit leaves the reset pending for another boot. */
    for (int fail = 1; fail <= 2; ++fail) {
        seed_settings(); assert(factory_reset_request() == ESP_OK);
        assert(factory_reset_prepare_boot() == ESP_OK);
        fail_at = fail; mutation = 0;
        assert(factory_reset_complete() == ESP_FAIL);
        reboot(); assert_pending(true); assert(factory_reset_prepare_boot() == ESP_OK);
        assert(factory_reset_complete() == ESP_OK); assert_pending(false);
    }
    return 0;
}
'''


class FactoryResetTests(unittest.TestCase):
    def test_production_storage_path_with_power_failures(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("No C compiler")
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "esp_err.h").write_text(ESP_ERR)
            (directory / "nvs.h").write_text(NVS)
            (directory / "harness.c").write_text(HARNESS)
            binary = directory / "factory_reset_test"
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(directory),
                 "-I", str(ROOT / "main"), str(ROOT / "main" / "factory_reset.c"),
                 str(ROOT / "main" / "wifi_store.c"), str(directory / "harness.c"),
                 "-o", str(binary)], capture_output=True, text=True, check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, check=False)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_boot_and_driver_restore_order(self):
        app = (ROOT / "main" / "app_main.c").read_text()
        wifi = (ROOT / "main" / "wifi_manager.c").read_text()
        self.assertLess(app.index("nvs_flash_init()"), app.index("factory_reset_prepare_boot()"))
        self.assertLess(app.index("factory_reset_prepare_boot()"), app.index("device_identity_init()"))
        self.assertLess(wifi.index("esp_wifi_init(&init)"), wifi.index("factory_reset_is_pending(&reset_pending)"))
        self.assertLess(wifi.index("esp_wifi_restore()"), wifi.index("factory_reset_complete()"))
        self.assertLess(wifi.index("factory_reset_complete()"), wifi.index("esp_wifi_get_config("))
        self.assertLess(wifi.index("factory_reset_complete()"), wifi.index("esp_wifi_start()"))


if __name__ == "__main__":
    unittest.main()
