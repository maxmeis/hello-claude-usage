"""Check the German clock grid and all phrases through the standalone C model."""

import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ROWS = 10
COLS = 11
CELLS = ROWS * COLS
REFERENCE_GRID = (
    "ESKISTAFÜNFZEHNZWANZIGDREIVIERTELVORFUNKNACHHALBAELFÜNFEINSXÄM"
    "ZWEIDREIAUJVIERSECHSNLACHTSIEBENZWÖLFZEHNEUNKUHR"
)
ENGLISH_ROWS = (
    "ITLISASTIME", "ACQUARTERDC", "TWENTYFIVEX", "HALFBTENFTO",
    "PASTERUNINE", "ONESIXTHREE", "FOURFIVETWO", "EIGHTELEVEN",
    "SEVENTWELVE", "TENSEOCLOCK",
)
ENGLISH_WORDS = {
    "IT": (0, 0, 2), "IS": (0, 3, 2), "A": (1, 0, 1),
    "QUARTER": (1, 2, 7), "TWENTY": (2, 0, 6),
    "FIVE_MIN": (2, 6, 4), "HALF": (3, 0, 4),
    "TEN_MIN": (3, 5, 3), "TO": (3, 9, 2), "PAST": (4, 0, 4),
    "ONE": (5, 0, 3), "SIX": (5, 3, 3), "THREE": (5, 6, 5),
    "FOUR": (6, 0, 4), "FIVE": (6, 4, 4), "TWO": (6, 8, 3),
    "EIGHT": (7, 0, 5), "ELEVEN": (7, 5, 6),
    "SEVEN": (8, 0, 5), "TWELVE": (8, 5, 6),
    "NINE": (4, 7, 4), "TEN": (9, 0, 3), "OCLOCK": (9, 5, 6),
}
ENGLISH_HOURS = ("TWELVE", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX",
                 "SEVEN", "EIGHT", "NINE", "TEN", "ELEVEN")
ENGLISH_MINUTES = (
    ("OCLOCK",), ("FIVE_MIN", "PAST"), ("TEN_MIN", "PAST"),
    ("A", "QUARTER", "PAST"), ("TWENTY", "PAST"),
    ("TWENTY", "FIVE_MIN", "PAST"), ("HALF", "PAST"),
    ("TWENTY", "FIVE_MIN", "TO"), ("TWENTY", "TO"),
    ("A", "QUARTER", "TO"), ("TEN_MIN", "TO"), ("FIVE_MIN", "TO"),
)


class WordClockState(ctypes.Structure):
    _fields_ = [
        ("valid", ctypes.c_bool),
        ("active", ctypes.c_bool * CELLS),
        ("minute_dots", ctypes.c_uint8),
    ]


# Literal text and absolute character position in the reference grid. Checking
# each slice below prevents an incorrect span from passing via a copied mask.
WORDS = {
    "ES": ("ES", 0), "IST": ("IST", 3),
    "FÜNF_MIN": ("FÜNF", 7), "ZEHN_MIN": ("ZEHN", 11),
    "ZWANZIG": ("ZWANZIG", 15), "VIERTEL": ("VIERTEL", 26),
    "VOR": ("VOR", 33), "NACH": ("NACH", 40),
    "HALB": ("HALB", 44), "UHR": ("UHR", 107),
    "EIN": ("EIN", 55), "EINS": ("EINS", 55),
    "ZWEI": ("ZWEI", 62), "DREI": ("DREI", 66),
    "VIER": ("VIER", 73), "FÜNF": ("FÜNF", 51),
    "SECHS": ("SECHS", 77), "SIEBEN": ("SIEBEN", 88),
    "ACHT": ("ACHT", 84), "NEUN": ("NEUN", 102),
    "ZEHN": ("ZEHN", 99), "ELF": ("ELF", 49),
    "ZWÖLF": ("ZWÖLF", 94),
}
HOURS = (
    "ZWÖLF", "EINS", "ZWEI", "DREI", "VIER", "FÜNF", "SECHS",
    "SIEBEN", "ACHT", "NEUN", "ZEHN", "ELF",
)
MINUTE_PHRASES = (
    ("UHR",),
    ("FÜNF_MIN", "NACH"),
    ("ZEHN_MIN", "NACH"),
    ("VIERTEL", "NACH"),
    ("ZWANZIG", "NACH"),
    ("FÜNF_MIN", "VOR", "HALB"),
    ("HALB",),
    ("FÜNF_MIN", "NACH", "HALB"),
    ("ZWANZIG", "VOR"),
    ("VIERTEL", "VOR"),
    ("ZEHN_MIN", "VOR"),
    ("FÜNF_MIN", "VOR"),
)


def cells_for(*words):
    return {
        index
        for word in words
        for literal, start in [WORDS[word]]
        for index in range(start, start + len(literal))
    }


def expected_words(hour, minute):
    slot = minute // 5
    shown_hour = (hour + (minute >= 25)) % 12
    hour_word = "EIN" if slot == 0 and shown_hour == 1 else HOURS[shown_hour]
    return ("ES", "IST", *MINUTE_PHRASES[slot], hour_word)


class WordClockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="sensecap-word-clock-")
        library = Path(cls.directory.name) / "word_clock.dylib"
        subprocess.run(
            ["cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "main/word_clock.c"), "-o", str(library)],
            check=True, capture_output=True,
        )
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.word_clock_compute.argtypes = [
            ctypes.c_int, ctypes.c_int, ctypes.c_bool,
            ctypes.POINTER(WordClockState),
        ]
        cls.lib.word_clock_compute.restype = None
        cls.lib.word_clock_compute_language.argtypes = [
            ctypes.c_int, ctypes.c_int, ctypes.c_bool, ctypes.c_uint8,
            ctypes.POINTER(WordClockState),
        ]
        cls.lib.word_clock_compute_language.restype = None
        cls.lib.word_clock_rows_for_language.argtypes = [ctypes.c_uint8]
        cls.lib.word_clock_rows_for_language.restype = ctypes.POINTER(ctypes.c_char_p)
        cls.rows = [row.decode("utf-8") for row in
                    (ctypes.c_char_p * ROWS).in_dll(cls.lib, "word_clock_rows")]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def compute(self, hour, minute, ready=True):
        state = WordClockState()
        self.lib.word_clock_compute(hour, minute, ready, ctypes.byref(state))
        return state

    @staticmethod
    def active_cells(state):
        return {index for index, active in enumerate(state.active) if active}

    def assert_phrase(self, hour, minute, words, dots=0):
        state = self.compute(hour, minute)
        self.assertTrue(state.valid)
        self.assertEqual(state.minute_dots, dots)
        self.assertEqual(self.active_cells(state), cells_for("ES", "IST", *words))

    def test_grid_matches_reference_unicode_letters(self):
        self.assertEqual(len(REFERENCE_GRID), CELLS)
        self.assertEqual("".join(self.rows), REFERENCE_GRID)
        for row in self.rows:
            self.assertEqual(len(row), COLS)
            self.assertTrue(row.isalpha() and row.isupper())
        self.assertIn("Ü", REFERENCE_GRID)
        self.assertIn("Ä", REFERENCE_GRID)
        self.assertIn("Ö", REFERENCE_GRID)

    def test_every_word_span_matches_its_literal_unicode_letters(self):
        grid = "".join(self.rows)
        for name, (literal, start) in WORDS.items():
            with self.subTest(word=name):
                self.assertEqual(grid[start:start + len(literal)], literal)
                self.assertEqual(start // COLS, (start + len(literal) - 1) // COLS)

    def test_all_twelve_five_minute_phrases(self):
        for slot, words in enumerate(MINUTE_PHRASES):
            hour_word = "EIN" if slot == 0 else ("EINS" if slot < 5 else "ZWEI")
            with self.subTest(minute=slot * 5):
                self.assert_phrase(1, slot * 5, (*words, hour_word))

    def test_ein_uhr_only_at_exact_one_oclock_slot(self):
        for hour in (1, 13):
            for minute in range(5):
                self.assert_phrase(hour, minute, ("EIN", "UHR"), dots=minute)
            self.assert_phrase(hour, 5, ("EINS", "FÜNF_MIN", "NACH"))
            self.assert_phrase(hour, 55, ("ZWEI", "FÜNF_MIN", "VOR"))
        for hour in (0, 12):
            self.assert_phrase(hour, 55, ("EINS", "FÜNF_MIN", "VOR"))

    def test_midnight_noon_and_hour_rollover(self):
        self.assert_phrase(0, 0, ("ZWÖLF", "UHR"))
        self.assert_phrase(12, 0, ("ZWÖLF", "UHR"))
        self.assert_phrase(11, 24, ("ELF", "ZWANZIG", "NACH"), dots=4)
        self.assert_phrase(11, 25, ("ZWÖLF", "FÜNF_MIN", "VOR", "HALB"))
        self.assert_phrase(11, 34, ("ZWÖLF", "HALB"), dots=4)
        self.assert_phrase(11, 35, ("ZWÖLF", "FÜNF_MIN", "NACH", "HALB"))
        self.assert_phrase(23, 59, ("ZWÖLF", "FÜNF_MIN", "VOR"), dots=4)

    def test_all_1440_minutes_match_independent_phrase_specification(self):
        for hour in range(24):
            for minute in range(60):
                state = self.compute(hour, minute)
                with self.subTest(hour=hour, minute=minute):
                    self.assertTrue(state.valid)
                    self.assertEqual(state.minute_dots, minute % 5)
                    self.assertEqual(self.active_cells(state),
                                     cells_for(*expected_words(hour, minute)))

    def test_invalid_or_unready_call_clears_prior_output(self):
        for hour, minute, ready in [(-1, 0, True), (24, 0, True),
                                    (0, -1, True), (0, 60, True),
                                    (12, 30, False)]:
            state = self.compute(12, 34)
            self.assertTrue(state.valid)
            with self.subTest(hour=hour, minute=minute, ready=ready):
                self.lib.word_clock_compute(hour, minute, ready, ctypes.byref(state))
                self.assertFalse(state.valid)
                self.assertEqual(state.minute_dots, 0)
                self.assertEqual(self.active_cells(state), set())
        self.lib.word_clock_compute(12, 30, True, None)

    def test_english_grid_and_all_1440_minutes(self):
        rows = self.lib.word_clock_rows_for_language(1)
        self.assertEqual(tuple(rows[i].decode() for i in range(ROWS)), ENGLISH_ROWS)
        grid = "".join(ENGLISH_ROWS)
        for name, (row, col, length) in ENGLISH_WORDS.items():
            self.assertEqual(grid[row * COLS + col:row * COLS + col + length],
                             name.replace("_MIN", ""))
        for hour in range(24):
            for minute in range(60):
                state = WordClockState()
                self.lib.word_clock_compute_language(hour, minute, True, 1,
                                                     ctypes.byref(state))
                slot = minute // 5
                shown_hour = (hour + (slot >= 7)) % 12
                words = ("IT", "IS", *ENGLISH_MINUTES[slot],
                         ENGLISH_HOURS[shown_hour])
                expected = {row * COLS + col + offset
                            for word in words
                            for row, col, length in (ENGLISH_WORDS[word],)
                            for offset in range(length)}
                self.assertTrue(state.valid)
                self.assertEqual(state.minute_dots, minute % 5)
                self.assertEqual(self.active_cells(state), expected,
                                 (hour, minute))

    def test_language_fallback_and_invalid_english_input(self):
        self.assertEqual([self.lib.word_clock_rows_for_language(255)[i].decode()
                          for i in range(ROWS)], self.rows)
        german = self.compute(10, 31)
        fallback = WordClockState()
        self.lib.word_clock_compute_language(10, 31, True, 255,
                                             ctypes.byref(fallback))
        self.assertEqual(self.active_cells(german), self.active_cells(fallback))
        invalid = WordClockState()
        self.lib.word_clock_compute_language(24, 0, True, 1,
                                             ctypes.byref(invalid))
        self.assertFalse(invalid.valid)
        self.assertEqual(self.active_cells(invalid), set())


if __name__ == "__main__":
    unittest.main()
