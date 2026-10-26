"""Exercise the firmware's display formatting helpers through native C."""
import calendar
import ctypes
from datetime import datetime
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DisplayFormatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="sensecap-display-format-")
        library = Path(cls.directory.name) / "display_format.dylib"
        subprocess.run(["cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "main/display_format.c"), "-o", str(library)],
                       check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.display_parse_reset.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int64)]
        cls.lib.display_parse_reset.restype = ctypes.c_bool
        cls.lib.display_format_reset.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                                 ctypes.c_char_p, ctypes.c_int64,
                                                 ctypes.c_bool]
        cls.lib.display_format_reset.restype = None
        cls.lib.display_format_age.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                               ctypes.c_int64]
        cls.lib.display_format_age.restype = None
        cls.lib.display_quota_color.argtypes = [ctypes.c_float]
        cls.lib.display_quota_color.restype = ctypes.c_uint32

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def parse(self, value):
        epoch = ctypes.c_int64(0x12345678)
        accepted = self.lib.display_parse_reset(value, ctypes.byref(epoch))
        return accepted, epoch.value

    def test_iso_parser_handles_utc_fraction_and_offsets(self):
        cases = [
            (b"2026-10-27T04:00:00+00:00", "2026-10-27T04:00:00"),
            (b"2026-10-27T04:00:00Z", "2026-10-27T04:00:00"),
            (b"2024-02-29T12:30:45.123456+00:00", "2024-02-29T12:30:45"),
            (b"2026-10-27T04:00:00+02:30", "2026-10-27T01:30:00"),
            (b"2026-10-27T04:00:00-05:45", "2026-10-27T09:45:00"),
        ]
        for value, utc in cases:
            with self.subTest(value=value):
                parsed, epoch = self.parse(value)
                self.assertTrue(parsed)
                self.assertEqual(epoch, calendar.timegm(datetime.fromisoformat(utc).timetuple()))

    def test_iso_parser_validates_calendar_and_complete_timezone(self):
        for value in [None, b"", b"unknown", b"2026-02-29T12:00:00Z",
                      b"2100-02-29T12:00:00Z", b"2024-02-30T12:00:00Z",
                      b"2026-10-27T24:00:00Z", b"2026-10-27T04:60:00Z",
                      b"2026-10-27T04:00:60Z", b"2026-10-27T04:00:00",
                      b"2026-10-27T04:00:00+24:00", b"2026-10-27T04:00:00+01:60",
                      b"2026-10-27T04:00:00.+00:00", b"2026-10-27T04:00:00.1x+00:00",
                      b"2026-10-27T04:00:00+00:00junk"]:
            with self.subTest(value=value):
                self.assertEqual(self.parse(value), (False, 0x12345678))

    def format_reset(self, iso, now, clock_ready=True, capacity=80):
        output = ctypes.create_string_buffer(capacity)
        self.lib.display_format_reset(output, capacity, iso, now, clock_ready)
        return output.value.decode()

    def format_age(self, age_seconds, capacity=80):
        output = ctypes.create_string_buffer(capacity)
        self.lib.display_format_age(output, capacity, age_seconds)
        return output.value.decode()

    def test_age_format_uses_floor_whole_units_and_singular_plural(self):
        cases = [
            (-1, "Just now"), (0, "Just now"), (59, "Just now"),
            (60, "1 minute ago"), (119, "1 minute ago"),
            (120, "2 minutes ago"), (3599, "59 minutes ago"),
            (3600, "1 hour ago"), (7199, "1 hour ago"),
            (7200, "2 hours ago"), (86399, "23 hours ago"),
            (86400, "1 day ago"), (172800, "2 days ago"),
        ]
        for age_seconds, expected in cases:
            with self.subTest(age_seconds=age_seconds):
                self.assertEqual(self.format_age(age_seconds), expected)

    def test_age_format_handles_int64_extremes_and_bounded_output(self):
        self.assertEqual(self.format_age(-(1 << 63)), "Just now")
        self.assertEqual(self.format_age((1 << 63) - 1),
                         f"{((1 << 63) - 1) // 86400} days ago")
        self.assertEqual(self.format_age(120, capacity=5), "2 mi")
        untouched = ctypes.create_string_buffer(b"x", 1)
        self.lib.display_format_age(untouched, 0, 60)
        self.assertEqual(untouched.raw, b"x")
        self.lib.display_format_age(None, 10, 60)

    def test_reset_countdown_uses_utc_and_expected_units(self):
        reset = calendar.timegm(datetime(2026, 10, 27, 4, 0).timetuple())
        self.assertEqual(self.format_reset(b"2026-10-27T04:00:00Z", reset - 8100),
                         "Resets in 2h 15m")
        self.assertEqual(self.format_reset(b"2026-10-27T04:00:00+00:00", reset - 259200),
                         "Resets in 3d 0h")
        self.assertEqual(self.format_reset(b"2026-10-27T04:00:00.123456Z", reset - 59),
                         "Resets in <1m")

    def test_expired_unknown_and_unready_clock_states(self):
        reset = calendar.timegm(datetime(2026, 10, 27, 4, 0).timetuple())
        self.assertEqual(self.format_reset(b"2026-10-27T04:00:00Z", reset), "Awaiting reset")
        self.assertEqual(self.format_reset(b"2026-10-27T04:00:00Z", reset + 1), "Awaiting reset")
        self.assertEqual(self.format_reset(None, reset), "Reset unknown")
        self.assertEqual(self.format_reset(b"", reset), "Reset unknown")
        self.assertEqual(self.format_reset(b"2026-10-27T04:00:00Z", reset, False),
                         "Reset unknown")

    def test_output_is_bounded_terminated_and_zero_capacity_is_safe(self):
        output = ctypes.create_string_buffer(b"xxxx", 5)
        self.lib.display_format_reset(output, 5, None, 0, True)
        self.assertEqual(output.raw, b"Rese\0")
        untouched = ctypes.create_string_buffer(b"x", 1)
        self.lib.display_format_reset(untouched, 0, None, 0, True)
        self.assertEqual(untouched.raw, b"x")

    def test_quota_colors_at_endpoints_and_intermediate_points(self):
        expected = {100.0: 0xC6CF82, 80.0: 0xC6CF82, 50.0: 0xD97757, 0.0: 0xDA624E}
        for remaining, color in expected.items():
            with self.subTest(remaining=remaining):
                self.assertEqual(self.lib.display_quota_color(remaining), color)
        self.assertEqual(self.lib.display_quota_color(65.0), 0xD0A36D)
        self.assertEqual(self.lib.display_quota_color(25.0), 0xDA6D53)

    def test_invalid_quota_values_use_neutral_color(self):
        for remaining in [-1.0, 100.01, float("nan"), float("inf"), -float("inf")]:
            with self.subTest(remaining=remaining):
                self.assertEqual(self.lib.display_quota_color(remaining), 0x5D625E)


if __name__ == "__main__":
    unittest.main()
