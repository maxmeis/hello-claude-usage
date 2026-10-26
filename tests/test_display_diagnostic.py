"""Host checks for the raw RGB565 framebuffer renderer."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WIDTH = HEIGHT = 480
PIXELS = WIDTH * HEIGHT
GUARD = 32
CANARY = 0xA55A


class DisplayDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="sensecap-display-diagnostic-")
        library = Path(cls.directory.name) / "display_diagnostic.dylib"
        subprocess.run(
            ["cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-DDISPLAY_DIAGNOSTIC_HOST", str(ROOT / "main/display_diagnostic.c"),
             "-o", str(library)],
            check=True, capture_output=True,
        )
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.display_diagnostic_render.argtypes = [ctypes.POINTER(ctypes.c_uint16),
                                                       ctypes.c_int, ctypes.c_int]
        cls.lib.display_diagnostic_render.restype = None

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def render(self, width=WIDTH, height=HEIGHT):
        count = width * height
        storage = (ctypes.c_uint16 * (count + 2 * GUARD))()
        for i in range(len(storage)):
            storage[i] = CANARY
        start = ctypes.cast(ctypes.byref(storage, GUARD * 2),
                            ctypes.POINTER(ctypes.c_uint16))
        self.lib.display_diagnostic_render(start, width, height)
        self.assertTrue(all(storage[i] == CANARY for i in range(GUARD)))
        self.assertTrue(all(storage[count + GUARD + i] == CANARY
                            for i in range(GUARD)))
        return [storage[GUARD + i] for i in range(count)]

    @staticmethod
    def pixel(frame, x, y, width=WIDTH):
        return frame[y * width + x]

    def test_color_block_interiors_and_grayscale(self):
        frame = self.render()
        samples = [
            (60, 110, 0x0000), (180, 110, 0xFFFF),
            (300, 110, 0xF800), (420, 110, 0x07E0),
            (60, 260, 0x001F), (180, 260, 0xDBAA),
            (300, 260, 0x8410),
        ]
        for x, y, color in samples:
            with self.subTest(x=x, y=y):
                self.assertEqual(self.pixel(frame, x, y), color)
        for i, color in enumerate([0x0000, 0x2104, 0x4208, 0x630C,
                                   0x8410, 0xA514, 0xC618, 0xFFFF]):
            self.assertEqual(self.pixel(frame, 30 + i * 60, 380), color)

    def test_checker_and_single_pixel_alignment(self):
        frame = self.render()
        self.assertEqual(self.pixel(frame, 372, 230), 0xFFFF)
        self.assertEqual(self.pixel(frame, 373, 230), 0x0000)
        self.assertEqual(self.pixel(frame, 372, 231), 0x0000)
        self.assertEqual(self.pixel(frame, 4, 432), 0xFFFF)
        self.assertEqual(self.pixel(frame, 5, 433), 0x0000)
        self.assertEqual(self.pixel(frame, 12, 450), 0xFFFF)
        self.assertEqual(self.pixel(frame, 13, 450), 0x0000)

    def test_color_labels_have_no_missing_characters(self):
        frame = self.render()
        for text, x, y, ink in [
            ("BLACK", 12, 56, 0xFFFF), ("WHITE", 132, 56, 0x0000),
            ("RED", 252, 56, 0xFFFF), ("GREEN", 372, 56, 0x0000),
            ("BLUE", 12, 206, 0xFFFF), ("CLAY", 132, 206, 0x0000),
            ("GRAY", 252, 206, 0x0000), ("CHECK", 372, 206, 0x0000),
        ]:
            for index, character in enumerate(text):
                with self.subTest(label=text, character=character):
                    left = x + index * 12
                    self.assertTrue(any(self.pixel(frame, col, row) == ink
                                        for row in range(y, y + 14)
                                        for col in range(left, left + 10)))

    def test_deterministic_and_clipped_small_buffer(self):
        self.assertEqual(self.render(), self.render())
        small = self.render(13, 17)
        self.assertEqual(len(small), 13 * 17)
        self.assertIn(0xFFFF, small)  # The clipped title remains visible.


if __name__ == "__main__":
    unittest.main()
