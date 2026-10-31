"""Browser installer frame contract, tested against the production C decoder."""

import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


class Settings(ctypes.Structure):
    _fields_ = [
        ("clock_language", ctypes.c_uint8), ("default_view", ctypes.c_uint8),
        ("brightness", ctypes.c_uint8), ("timezone", ctypes.c_uint8),
        ("dashboard_timeout_sec", ctypes.c_uint16),
        ("refresh_interval_sec", ctypes.c_uint16),
        ("mascot_motion", ctypes.c_bool), ("refresh_animation", ctypes.c_bool),
        ("providers", ctypes.c_uint8),
    ]


class Image(ctypes.Structure):
    _fields_ = [("settings", Settings), ("wifi_present", ctypes.c_bool),
                ("ssid", ctypes.c_char * 33), ("password", ctypes.c_char * 65)]


def frame(settings=None, ssid=b"", password=b"", present=False):
    settings = settings or [0, 0, 100, 30, 300, 0, 3]
    body = bytearray(108)
    body[0:3] = bytes(settings[0:3])
    body[3:5] = settings[3].to_bytes(2, "little")
    body[5:7] = settings[4].to_bytes(2, "little")
    body[7:9] = bytes(settings[5:7])
    body[9] = int(present)
    if present or ssid or password:
        body[10:10 + len(ssid)] = ssid
        body[43:43 + len(password)] = password
    header = b"HBX1" + len(body).to_bytes(4, "little") + zlib.crc32(body).to_bytes(4, "little")
    return bytes(header + body + b"\xff" * (4096 - 12 - len(body)))


class BoxConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory()
        binary = Path(cls.temp.name) / "box_config.so"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
             str(ROOT / "main" / "box_config.c"), "-o", str(binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.lib = ctypes.CDLL(str(binary))
        cls.lib.box_config_decode.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                                ctypes.POINTER(Image)]
        cls.lib.box_config_decode.restype = ctypes.c_bool
        cls.lib.box_config_current.restype = ctypes.POINTER(Settings)
        cls.lib.box_config_timezone.restype = ctypes.c_char_p

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def decode(self, data):
        image = Image()
        raw = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        return self.lib.box_config_decode(raw, len(data), ctypes.byref(image)), image

    def test_defaults_and_valid_import(self):
        defaults = self.lib.box_config_current().contents
        self.assertEqual((defaults.clock_language, defaults.default_view, defaults.brightness,
                          defaults.dashboard_timeout_sec, defaults.refresh_interval_sec,
                          defaults.mascot_motion, defaults.refresh_animation, defaults.providers),
                         (0, 0, 100, 30, 300, True, True, 0))
        self.assertEqual(self.lib.box_config_timezone(), b"CET-1CEST,M3.5.0,M10.5.0/3")
        accepted, image = self.decode(frame([1, 1, 20, 10, 600, 5, 0], b"Caf\xc3\xa9", b"password1", True))
        self.assertTrue(accepted)
        self.assertEqual((image.settings.clock_language, image.settings.timezone,
                          image.settings.refresh_interval_sec, image.ssid, image.password),
                         (1, 5, 600, b"Caf\xc3\xa9", b"password1"))

    def test_provider_flags_are_backward_compatible_and_bounded(self):
        for selection in range(3):
            accepted, image = self.decode(frame([0, 0, 100, 30, 300, 0, 3 | selection << 2]))
            self.assertTrue(accepted)
            self.assertEqual(image.settings.providers, selection)
        for flags in (0x0f, 0x10, 0x83):
            self.assertFalse(self.decode(frame([0, 0, 100, 30, 300, 0, flags]))[0])

    def test_corruption_and_noncanonical_values_rejected(self):
        good = frame()
        cases = [good[:-1], b"BAD!" + good[4:], good[:4] + (107).to_bytes(4, "little") + good[8:],
                 good[:70] + bytes([good[70] ^ 1]) + good[71:], good[:-1] + b"\x00",
                 frame([2, 0, 100, 30, 300, 0, 3]), frame([0, 2, 100, 30, 300, 0, 3]),
                 frame([0, 0, 19, 30, 300, 0, 3]), frame([0, 0, 100, 9, 300, 0, 3]),
                 frame([0, 0, 100, 30, 601, 0, 3]), frame([0, 0, 100, 30, 300, 6, 3]),
                 frame([0, 0, 100, 30, 300, 0, 15]), frame(ssid=b"bad\xff", password=b"password1", present=True),
                 frame(ssid=b"mywifi", password=b"short", present=True),
                 frame(ssid=b"mywifi", password=b"g" * 64, present=True),
                 frame(ssid=b"ignored", present=False)]
        for bad in cases:
            with self.subTest(case=cases.index(bad)):
                self.assertFalse(self.decode(bad)[0])

    def test_wifi_limits(self):
        self.assertTrue(self.decode(frame(ssid=b"s" * 32, password=b"p" * 63, present=True))[0])
        self.assertTrue(self.decode(frame(ssid=b"s", password=b"a" * 64, present=True))[0])
        self.assertTrue(self.decode(frame(ssid=b"s", password=b"", present=True))[0])
        self.assertFalse(self.decode(frame(ssid=b"s" * 33, password=b"passpass", present=True))[0])

    def test_sdk_free_provider_helpers(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        harness = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include "usage_provider.h"
int main(void) {
    for (int selection = 0; selection < 3; ++selection)
        for (int id = 0; id < USAGE_PROVIDER_COUNT; ++id)
            assert(usage_provider_enabled(selection, id) ==
                   (selection == 2 || selection == id));
    assert(!usage_provider_enabled(3, USAGE_PROVIDER_CLAUDE));
    assert(!usage_provider_enabled(0, USAGE_PROVIDER_COUNT));
    assert(!strcmp(usage_provider_name(USAGE_PROVIDER_CODEX), "Codex"));
    provider_usage_t usage;
    usage_provider_init_unknown(&usage);
    assert(!usage.has_sample && !usage.windows[0].valid);
    usage.configured = true;
    usage.has_sample = true;
    usage.sampled_at = 1000;
    usage.sampled_us = 1000000;
    usage.windows[0] = (quota_window_t){true, 55.5f, 2000, 18000};
    usage.windows[1] = (quota_window_t){true, NAN, -1, 604800};
    assert(!usage_provider_validate(&usage));
    assert(usage.windows[0].valid && !usage.windows[1].valid);
    assert(usage_provider_validate(&usage));
    usage.sampled_at = -1;
    assert(!usage_provider_validate(&usage));
    assert(!usage.has_sample && !usage.windows[0].valid);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            source = path / "helpers.c"
            binary = path / "helpers"
            source.write_text(harness)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(ROOT / "main"), str(ROOT / "main" / "usage_provider.c"),
                                    str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_store_commit_and_inbox_erase_order(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "box_config_store.h"
#include "box_config.h"
#include "esp_partition.h"
#include "nvs.h"
static unsigned char sector[4096], saved[11];
static esp_partition_t part = {.size = 4096};
static int exists, committed, commits, erases, fail_commit, fail_erase, staged_erase, factory_erasing;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label) {
    assert(type == 1 && subtype == 0x41 && !strcmp(label, "web_config"));
    return exists ? &part : NULL;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *dst, size_t n) {
    assert(p == &part && offset == 0 && n == 4096); memcpy(dst, sector, n); return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t n) {
    assert(p == &part && offset == 0 && n == 4096 && (committed || factory_erasing));
    ++erases; if (fail_erase) return ESP_FAIL;
    memset(sector, 0xff, sizeof(sector)); return ESP_OK;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    assert(!strcmp(name, "box_config"));
    if (mode == NVS_READONLY && !committed) return ESP_ERR_NVS_NOT_FOUND;
    *h = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *dst, size_t *n) {
    assert(h == 1 && !strcmp(key, "settings") && *n == 11);
    memcpy(dst, saved, 11); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *src, size_t n) {
    assert(h == 1 && !strcmp(key, "settings") && n == 11);
    memcpy(saved, src, n); staged_erase = factory_erasing = 0; return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h) { assert(h == 1); staged_erase = factory_erasing = 1; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h == 1); ++commits;
    if (fail_commit) return ESP_FAIL;
    committed = !staged_erase; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 1); }
int main(int argc, char **argv) {
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb"); assert(f && fread(sector, 1, 4096, f) == 4096); fclose(f);
    exists = 1;
    fail_commit = 1;
    assert(box_config_init() == ESP_FAIL && !committed && !erases);
    wifi_store_record_t pending;
    assert(!box_config_pending_wifi(&pending));
    fail_commit = 0;
    assert(box_config_init() == ESP_OK && committed && !erases);
    assert(box_config_current()->brightness == 42 && box_config_current()->providers == 2);
    assert(saved[8] == 11 && saved[9] == 0x48 && saved[10] == 0x42);
    assert(box_config_pending_wifi(&pending));
    assert(!strcmp(pending.ssid, "example") && !strcmp(pending.password, "password1"));
    fail_erase = 1;
    assert(box_config_finish_pending() == ESP_FAIL && box_config_pending_wifi(&pending));
    fail_erase = 0;
    assert(box_config_finish_pending() == ESP_OK && !box_config_pending_wifi(&pending));
    assert(erases == 2 && sector[0] == 0xff);
    f = fopen(argv[2], "rb"); assert(f && fread(sector, 1, 4096, f) == 4096); fclose(f);
    erases = 0;
    assert(box_config_init() == ESP_OK && erases == 1);
    assert(!box_config_pending_wifi(&pending));
    /* An existing 11-byte NVS blob with no provider bits selects Claude. */
    saved[8] = 3;
    exists = 0;
    assert(box_config_init() == ESP_OK && box_config_current()->providers == 0);
    saved[8] = 15;
    assert(box_config_init() == ESP_ERR_INVALID_STATE);
    saved[8] = 3;
    /* Missing partition preserves already committed settings. */
    exists = 0;
    assert(box_config_init() == ESP_OK && box_config_current()->brightness == 42);
    /* A pending factory reset removes both the saved settings and inbox. */
    exists = 1;
    f = fopen(argv[1], "rb"); assert(f && fread(sector, 1, 4096, f) == 4096); fclose(f);
    fail_erase = 1;
    assert(box_config_factory_erase() == ESP_FAIL);
    assert(sector[0] == 'H');
    fail_erase = 0;
    assert(box_config_factory_erase() == ESP_OK && sector[0] == 0xff && !committed);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define ESP_ERR_INVALID_STATE 5
""")
            (directory / "esp_partition.h").write_text("""#pragma once
#include <stddef.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_DATA 1
typedef struct {size_t size;} esp_partition_t;
const esp_partition_t *esp_partition_find_first(int,int,const char*);
esp_err_t esp_partition_read(const esp_partition_t*,size_t,void*,size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t*,size_t,size_t);
""")
            (directory / "esp_log.h").write_text("#define ESP_LOGW(...) ((void)0)\n")
            (directory / "nvs.h").write_text("""#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 1
#define NVS_READWRITE 2
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
""")
            (directory / "harness.c").write_text(harness)
            wifi_file = directory / "wifi.bin"
            no_wifi_file = directory / "no_wifi.bin"
            wifi_file.write_bytes(frame([0, 0, 42, 30, 300, 0, 11], b"example", b"password1", True))
            no_wifi_file.write_bytes(frame([0, 0, 42, 30, 300, 0, 3]))
            binary = directory / "store-test"
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(directory), "-I", str(ROOT / "main"),
                                    str(ROOT / "main" / "box_config.c"),
                                    str(ROOT / "main" / "box_config_store.c"),
                                    str(directory / "harness.c"), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary), str(wifi_file), str(no_wifi_file)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
