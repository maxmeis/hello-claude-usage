"""Check browser-generated installer images against the production C decoder."""

import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
JS_CONFIG = ROOT / "web-installer" / "src" / "config.js"


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


class WebConfigInteropTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        node = shutil.which("node")
        if not compiler or not node:
            raise unittest.SkipTest("C compiler and Node.js are required")
        cls.node = node
        cls.temp = tempfile.TemporaryDirectory()
        library = Path(cls.temp.name) / "box_config.so"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
             str(ROOT / "main" / "box_config.c"), "-o", str(library)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.box_config_decode.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                               ctypes.POINTER(Image)]
        cls.lib.box_config_decode.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def encode_in_browser_module(self, config):
        # Import the production module directly; no copied encoder or fixture bytes.
        script = (
            "import fs from 'node:fs'; "
            f"import {{ encodeConfiguration }} from {json.dumps(JS_CONFIG.as_uri())}; "
            "const config = JSON.parse(fs.readFileSync(0, 'utf8')); "
            "process.stdout.write(Buffer.from(encodeConfiguration(config)).toString('hex'));"
        )
        result = subprocess.run([self.node, "--input-type=module", "-e", script],
                                input=json.dumps(config), capture_output=True, text=True,
                                cwd=ROOT / "web-installer")
        if result.returncode:
            raise AssertionError(result.stderr)
        return bytes.fromhex(result.stdout)

    def decode_with_production_c(self, encoded):
        image = Image()
        raw = (ctypes.c_uint8 * len(encoded)).from_buffer_copy(encoded)
        accepted = self.lib.box_config_decode(raw, len(encoded), ctypes.byref(image))
        return accepted, image

    def assert_interop(self, config, expected):
        encoded = self.encode_in_browser_module(config)
        self.assertEqual(len(encoded), 4096)
        accepted, image = self.decode_with_production_c(encoded)
        self.assertTrue(accepted, "C decoder rejected the browser's encoded image")
        settings = image.settings
        self.assertEqual((settings.clock_language, settings.default_view, settings.brightness,
                          settings.dashboard_timeout_sec, settings.refresh_interval_sec,
                          settings.timezone, settings.mascot_motion, settings.refresh_animation,
                          settings.providers),
                         expected["settings"])
        self.assertEqual(image.wifi_present, expected["wifi_present"])
        self.assertEqual(image.ssid, expected["ssid"])
        self.assertEqual(image.password, expected["password"])
        return encoded

    def test_default_image_matches_production_decoder(self):
        config = {
            "clockLanguage": "de", "defaultView": "clock", "brightness": 100,
            "dashboardTimeout": 30, "refreshInterval": 300, "timezone": "Europe/Berlin",
            "mascotMotion": True, "refreshAnimation": True, "wifi": None,
        }
        encoded = self.assert_interop(config, {
            "settings": (0, 0, 100, 30, 300, 0, True, True, 0),
            "wifi_present": False, "ssid": b"", "password": b"",
        })
        self.assertEqual(encoded[:4], b"HBX1")
        self.assertEqual(encoded[4:8], (108).to_bytes(4, "little"))

    def test_custom_unicode_wifi_image_matches_production_decoder(self):
        config = {
            "clockLanguage": "en", "defaultView": "usage", "brightness": 61,
            "dashboardTimeout": 10, "refreshInterval": 600, "timezone": "Asia/Tokyo",
            "mascotMotion": False, "refreshAnimation": False,
            "wifi": {"ssid": "Café-設備", "password": "dummy-pass-123"},
        }
        self.assert_interop(config, {
            "settings": (1, 1, 61, 10, 600, 5, False, False, 0),
            "wifi_present": True, "ssid": "Café-設備".encode("utf-8"),
            "password": b"dummy-pass-123",
        })

    def test_provider_selection_round_trips_without_changing_wifi(self):
        base = {
            "clockLanguage": "de", "defaultView": "usage", "brightness": 100,
            "dashboardTimeout": 30, "refreshInterval": 300, "timezone": "Europe/Berlin",
            "mascotMotion": True, "refreshAnimation": True,
            "wifi": {"ssid": "TestNet", "password": "password1"},
        }
        for name, selection in (("claude", 0), ("codex", 1), ("both", 2)):
            with self.subTest(providers=name):
                encoded = self.assert_interop({**base, "providers": name}, {
                    "settings": (0, 1, 100, 30, 300, 0, True, True, selection),
                    "wifi_present": True, "ssid": b"TestNet", "password": b"password1",
                })
                self.assertEqual(encoded[20], 3 | selection << 2)

    def test_decoder_rejects_crc_and_enum_corruption_of_web_image(self):
        config = {
            "clockLanguage": "en", "defaultView": "clock", "brightness": 80,
            "dashboardTimeout": 45, "refreshInterval": 180, "timezone": "UTC",
            "mascotMotion": True, "refreshAnimation": False, "wifi": None,
        }
        valid = self.encode_in_browser_module(config)
        bad_crc = bytearray(valid)
        bad_crc[40] ^= 0x20
        self.assertFalse(self.decode_with_production_c(bytes(bad_crc))[0])

        bad_enum = bytearray(valid)
        bad_enum[12] = 2  # language enum; repair CRC to isolate enum validation
        bad_enum[8:12] = zlib.crc32(bad_enum[12:120]).to_bytes(4, "little")
        self.assertFalse(self.decode_with_production_c(bytes(bad_enum))[0])

        for flags in (0x0f, 0x10):
            invalid = bytearray(valid)
            invalid[20] = flags
            invalid[8:12] = zlib.crc32(invalid[12:120]).to_bytes(4, "little")
            self.assertFalse(self.decode_with_production_c(bytes(invalid))[0])


if __name__ == "__main__":
    unittest.main()
