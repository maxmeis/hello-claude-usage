#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WORD_CLOCK_ROWS 10
#define WORD_CLOCK_COLS 11
#define WORD_CLOCK_CELLS (WORD_CLOCK_ROWS * WORD_CLOCK_COLS)

/* Row-major German clock face. Each UTF-8 row has 11 Unicode letters;
 * active[] indices count letters, not bytes. */
extern const char *const word_clock_rows[WORD_CLOCK_ROWS];
extern const char *const word_clock_english_rows[WORD_CLOCK_ROWS];

enum { WORD_CLOCK_GERMAN = 0, WORD_CLOCK_ENGLISH = 1 };

typedef struct {
    bool valid;
    bool active[WORD_CLOCK_CELLS];
    uint8_t minute_dots;
} word_clock_state_t;

/* Floors to five-minute phrases; minute_dots holds the remaining 0..4 minutes. */
void word_clock_compute(int hour, int minute, bool ready, word_clock_state_t *out);
const char *const *word_clock_rows_for_language(uint8_t language);
void word_clock_compute_language(int hour, int minute, bool ready,
                                 uint8_t language, word_clock_state_t *out);
