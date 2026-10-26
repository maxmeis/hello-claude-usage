"""Native checks for footer refresh admission and cycle coalescing."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Control(ctypes.Structure):
    _fields_ = [("ready", ctypes.c_bool), ("pending", ctypes.c_bool),
                ("busy", ctypes.c_bool), ("retry_at", ctypes.c_int64)]


class RefreshControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="sensecap-refresh-")
        library = Path(cls.directory.name) / "refresh_control.dylib"
        subprocess.run(["cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra",
                        "-Werror", str(ROOT / "main/refresh_control.c"), "-o", str(library)],
                       check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.refresh_control_publish.argtypes = [ctypes.POINTER(Control), ctypes.c_bool,
                                                    ctypes.c_int64]
        cls.lib.refresh_control_request.argtypes = [ctypes.POINTER(Control), ctypes.c_int64]
        cls.lib.refresh_control_request.restype = ctypes.c_bool
        cls.lib.refresh_control_begin.argtypes = [ctypes.POINTER(Control),
                                                  ctypes.POINTER(ctypes.c_bool)]
        cls.lib.refresh_control_begin.restype = ctypes.c_bool
        cls.lib.refresh_control_finish.argtypes = [ctypes.POINTER(Control), ctypes.c_bool,
                                                   ctypes.c_int64]
        cls.lib.refresh_control_usage_due.argtypes = [ctypes.c_int64, ctypes.c_int64,
                                                      ctypes.c_int64, ctypes.c_int64]
        cls.lib.refresh_control_usage_due.restype = ctypes.c_bool
        cls.lib.refresh_control_usage_retry_delay.argtypes = [ctypes.c_int, ctypes.c_int,
                                                              ctypes.c_int]
        cls.lib.refresh_control_usage_retry_delay.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def begin(self, control):
        manual = ctypes.c_bool()
        started = self.lib.refresh_control_begin(ctypes.byref(control), ctypes.byref(manual))
        return started, manual.value

    def test_one_pending_tap_is_consumed_once_and_busy_taps_are_dropped(self):
        control = Control()
        self.lib.refresh_control_publish(ctypes.byref(control), True, 0)
        self.assertTrue(self.lib.refresh_control_request(ctypes.byref(control), 100))
        self.assertFalse(self.lib.refresh_control_request(ctypes.byref(control), 100))
        self.assertEqual(self.begin(control), (True, True))
        self.assertFalse(self.lib.refresh_control_request(ctypes.byref(control), 101))
        self.lib.refresh_control_finish(ctypes.byref(control), True, 0)
        self.assertEqual(self.begin(control), (True, False))
        self.lib.refresh_control_finish(ctypes.byref(control), True, 0)

    def test_backoff_rejects_taps_until_deadline(self):
        control = Control()
        self.lib.refresh_control_publish(ctypes.byref(control), True, 900)
        self.assertFalse(self.lib.refresh_control_request(ctypes.byref(control), 899))
        self.assertTrue(self.lib.refresh_control_request(ctypes.byref(control), 900))
        self.lib.refresh_control_publish(ctypes.byref(control), False, 0)
        self.assertFalse(control.pending)
        self.assertFalse(self.lib.refresh_control_request(ctypes.byref(control), 901))

    def test_reconnect_or_unsaved_token_cancels_accepted_tap(self):
        control = Control()
        self.lib.refresh_control_publish(ctypes.byref(control), True, 0)
        self.assertTrue(self.lib.refresh_control_request(ctypes.byref(control), 100))
        self.lib.refresh_control_publish(ctypes.byref(control), False, 0)
        self.assertEqual(self.begin(control), (False, False))
        self.lib.refresh_control_publish(ctypes.byref(control), True, 0)
        self.assertTrue(self.lib.refresh_control_request(ctypes.byref(control), 101))
        self.assertEqual(self.begin(control), (True, True))
        self.lib.refresh_control_finish(ctypes.byref(control), False, 0)
        self.assertFalse(self.lib.refresh_control_request(ctypes.byref(control), 102))

    def test_ready_publication_during_wakeup_keeps_one_accepted_tap(self):
        control = Control()
        self.lib.refresh_control_publish(ctypes.byref(control), True, 0)
        self.assertTrue(self.lib.refresh_control_request(ctypes.byref(control), 100))
        self.lib.refresh_control_publish(ctypes.byref(control), True, 0)
        self.assertEqual(self.begin(control), (True, True))
        self.assertFalse(control.pending)

    def test_429_then_reconnect_cannot_start_early_usage_get(self):
        control = Control()
        retry_until = 900  # A prior usage GET returned 429 with Retry-After.
        next_usage = 900
        self.lib.refresh_control_finish(ctypes.byref(control), True, retry_until)
        next_usage = 0  # GOT_IP requests an immediate fetch.
        self.assertFalse(self.lib.refresh_control_request(ctypes.byref(control), 899))
        self.assertFalse(self.lib.refresh_control_usage_due(899, next_usage, retry_until, 2000))
        self.assertTrue(self.lib.refresh_control_usage_due(900, next_usage, retry_until, 2000))
        self.assertTrue(self.lib.refresh_control_request(ctypes.byref(control), 900))

    def test_429_then_proactive_token_renewal_does_not_unlock_usage(self):
        retry_until = 900
        next_usage = 900
        expires_at = 820
        self.assertFalse(self.lib.refresh_control_usage_due(800, next_usage, retry_until,
                                                            expires_at))
        # The token POST succeeds and makes a fresh usage fetch due immediately.
        next_usage = 0
        expires_at = 2000
        self.assertFalse(self.lib.refresh_control_usage_due(850, next_usage, retry_until,
                                                            expires_at))
        self.assertTrue(self.lib.refresh_control_usage_due(900, next_usage, retry_until,
                                                           expires_at))
        self.assertFalse(self.lib.refresh_control_usage_due(2000, next_usage, retry_until,
                                                            expires_at))

    def test_final_retry_get_429_controls_backoff_after_initial_401(self):
        # Model the final response in a 401 -> token POST -> retry GET chain.
        status, retry_after = 429, 120
        delay = self.lib.refresh_control_usage_retry_delay(2, status, retry_after)
        self.assertEqual(delay, 120)
        self.assertFalse(self.lib.refresh_control_usage_due(1119, 0, 1000 + delay, 2000))
        self.assertTrue(self.lib.refresh_control_usage_due(1120, 0, 1000 + delay, 2000))
        # Connectivity loss changes the display status, not the HTTP retry policy.
        self.assertEqual(self.lib.refresh_control_usage_retry_delay(2, status, retry_after), 120)
        self.assertEqual(self.lib.refresh_control_usage_retry_delay(2, 500, 120), 10)


if __name__ == "__main__":
    unittest.main()
