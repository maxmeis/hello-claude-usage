"""Exercise the firmware's actual protocol helpers with the native C compiler."""
import calendar
import ctypes
from datetime import datetime
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="sensecap-protocol-")
        library = Path(cls.directory.name) / "protocol.dylib"
        subprocess.run(["cc", "-shared", "-fPIC", "-std=c11", "-D_XOPEN_SOURCE=700",
                        "-Wall", "-Wextra", "-Werror", str(ROOT / "main/probe_protocol.c"),
                        "-o", str(library)], check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.probe_retry_after_seconds.argtypes = [ctypes.c_char_p, ctypes.c_int64]
        cls.lib.probe_retry_after_seconds.restype = ctypes.c_int
        cls.lib.probe_authorization_code.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                                     ctypes.POINTER(ctypes.c_char_p)]
        cls.lib.probe_authorization_code.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_retry_after_seconds_includes_long_rate_limits(self):
        for value, expected in [(b"0", 0), (b"30", 30), (b"7200", 7200), (b"86400", 86400)]:
            with self.subTest(value=value):
                self.assertEqual(self.lib.probe_retry_after_seconds(value, 0), expected)

    def test_retry_after_http_date_uses_utc(self):
        for date in [datetime(2026, 10, 26, 18, 0), datetime(2028, 2, 29, 12, 5),
                     datetime(2100, 3, 1, 0, 1)]:
            with self.subTest(date=date):
                epoch = calendar.timegm(date.timetuple())
                header = date.strftime("%a, %d %b %Y %H:%M:%S GMT").encode()
                self.assertEqual(self.lib.probe_retry_after_seconds(header, epoch - 7200), 7200)
                self.assertEqual(self.lib.probe_retry_after_seconds(header, epoch + 1), 0)

    def test_malformed_retry_after_does_not_create_a_deadline(self):
        for value in [None, b"", b"-1", b"soon", b"30junk", b"Mon, 31 Feb 2026 10:00:00 GMT"]:
            with self.subTest(value=value):
                self.assertEqual(self.lib.probe_retry_after_seconds(value, 0), 0)

    def parse(self, text, expected=b"expected_state"):
        value = ctypes.create_string_buffer(text)
        code = ctypes.c_char_p()
        accepted = self.lib.probe_authorization_code(value, expected, ctypes.byref(code))
        return accepted, code.value

    def test_authorization_code_accepts_matching_state_and_trims_copy_whitespace(self):
        self.assertEqual(self.parse(b" \nopaque_code#expected_state\r\n"), (True, b"opaque_code"))

    def test_authorization_code_rejects_absent_or_mismatched_state(self):
        for value in [b"", b"opaque_code", b"#expected_state", b"opaque_code#wrong",
                      b"opaque_code#expected_state#extra", b"opaque_code#"]:
            with self.subTest(value=value):
                self.assertEqual(self.parse(value), (False, None))


if __name__ == "__main__":
    unittest.main()
