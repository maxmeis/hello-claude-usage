"""Host checks against the production identity implementation and its Wi-Fi wiring."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

ESP_ERR = """\
#ifndef ESP_ERR_H
#define ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
"""

NVS = """\
#ifndef NVS_H
#define NVS_H
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READWRITE 2
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
#endif
"""

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "device_identity.c"

static char durable[64], staged[64];
static int opens, reads, writes, commits, random_calls;
static int fail_open, fail_read, fail_write, fail_commit;
static uint32_t random_value = 3821;

uint32_t esp_random(void) { ++random_calls; return random_value; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(strcmp(name, "hello_box") == 0 && mode == NVS_READWRITE);
    ++opens;
    if (fail_open) return ESP_FAIL;
    *handle = 7;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *length) {
    assert(handle == 7 && strcmp(key, "device_name") == 0);
    ++reads;
    if (fail_read) return ESP_FAIL;
    if (!durable[0]) return ESP_ERR_NVS_NOT_FOUND;
    size_t stored_length = strlen(durable) + 1;
    if (*length < stored_length) return ESP_FAIL;
    memcpy(value, durable, stored_length);
    *length = stored_length;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    assert(handle == 7 && strcmp(key, "device_name") == 0);
    ++writes;
    if (fail_write) return ESP_FAIL;
    snprintf(staged, sizeof(staged), "%s", value);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 7);
    ++commits;
    if (fail_commit) return ESP_FAIL;
    strcpy(durable, staged);
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 7); }

static void reboot(void) {
    memset(s_name, 0, sizeof(s_name));
    s_initialized = false;
}
static void reset_storage(void) {
    reboot();
    durable[0] = staged[0] = 0;
    opens = reads = writes = commits = random_calls = 0;
    fail_open = fail_read = fail_write = fail_commit = 0;
}
int main(void) {
    assert(device_identity_name() == NULL);
    assert(device_identity_init() == ESP_OK);
    assert(strcmp(device_identity_name(), "hello-box-4821") == 0);
    assert(writes == 1 && commits == 1 && random_calls == 1);
    const char *first = device_identity_name();
    assert(device_identity_init() == ESP_OK && device_identity_name() == first);
    assert(opens == 1 && writes == 1);
    reboot();
    random_value = 0;
    assert(device_identity_init() == ESP_OK);
    assert(strcmp(device_identity_name(), "hello-box-4821") == 0);
    assert(writes == 1 && random_calls == 1);

    reset_storage();
    assert(device_identity_init() == ESP_OK);
    assert(strcmp(device_identity_name(), "hello-box-1000") == 0);
    reset_storage();
    random_value = 8999;
    assert(device_identity_init() == ESP_OK);
    assert(strcmp(device_identity_name(), "hello-box-9999") == 0);

    const char *invalid[] = {
        "hello-box-0000", "hello-box-123", "hello-box-12345",
        "Hello-box-1234", "hello-box-1<34", "hello-box-12;4"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset_storage();
        strcpy(durable, invalid[i]);
        assert(device_identity_init() != ESP_OK);
        assert(device_identity_name() == NULL && writes == 0 && random_calls == 0);
    }

    reset_storage(); fail_open = 1;
    assert(device_identity_init() == ESP_FAIL && device_identity_name() == NULL);
    reset_storage(); fail_read = 1;
    assert(device_identity_init() == ESP_FAIL && device_identity_name() == NULL);
    reset_storage(); fail_write = 1;
    assert(device_identity_init() == ESP_FAIL && device_identity_name() == NULL);
    reset_storage(); fail_commit = 1;
    assert(device_identity_init() == ESP_FAIL && device_identity_name() == NULL);
    assert(durable[0] == 0);
    fail_commit = 0;
    assert(device_identity_init() == ESP_OK && commits == 2);
    assert(strcmp(durable, device_identity_name()) == 0);
    return 0;
}
'''


class DeviceIdentityTests(unittest.TestCase):
    def test_production_c_generation_persistence_validation_and_failures(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("No C compiler")
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "esp_err.h").write_text(ESP_ERR)
            (directory / "nvs.h").write_text(NVS)
            (directory / "esp_random.h").write_text(
                "#include <stdint.h>\nuint32_t esp_random(void);\n")
            (directory / "device_identity.c").write_text(
                (ROOT / "main/device_identity.c").read_text())
            (directory / "device_identity.h").write_text(
                (ROOT / "main/device_identity.h").read_text())
            (directory / "harness.c").write_text(HARNESS)
            executable = directory / "identity_test"
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(directory),
                 str(directory / "harness.c"), "-o", str(executable)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 check=False)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stdout + run.stderr, "", "identity must not log stored data")

    def test_network_and_startup_use_same_identity_before_start(self):
        manager = (ROOT / "main/wifi_manager.c").read_text()
        app = (ROOT / "main/app_main.c").read_text()
        self.assertRegex(manager, r"esp_netif_set_hostname\(s_manager\.sta, device_identity_name\(\)\)")
        self.assertRegex(manager, r"esp_netif_set_hostname\(s_manager\.ap, device_identity_name\(\)\)")
        self.assertLess(manager.index("esp_netif_set_hostname(s_manager.sta"),
                        manager.index("esp_wifi_start()"))
        self.assertLess(manager.index("esp_netif_set_hostname(s_manager.ap"),
                        manager.index("esp_wifi_set_mode(WIFI_MODE_APSTA)"))
        self.assertRegex(manager, r"snprintf\(s_manager\.ap_ssid, sizeof\(s_manager\.ap_ssid\),\s*\"%s\", device_identity_name\(\)\)")
        self.assertIn('random_hex(s_manager.ap_password, 8)', manager)
        self.assertLess(app.index("nvs_flash_init()"), app.index("device_identity_init()"))
        self.assertLess(app.index("device_identity_init()"), app.index("wifi_manager_start("))
        self.assertIn('display_set_device_name(name)', app)
        self.assertIn('"Device name / STA hostname: %s", name', app)
        self.assertNotIn("ESP_LOG", (ROOT / "main/device_identity.c").read_text())


if __name__ == "__main__":
    unittest.main()
