#include "word_clock.h"

#include <string.h>

const char *const word_clock_rows[WORD_CLOCK_ROWS] = {
    "ESKISTAFÜNF",
    "ZEHNZWANZIG",
    "DREIVIERTEL",
    "VORFUNKNACH",
    "HALBAELFÜNF",
    "EINSXÄMZWEI",
    "DREIAUJVIER",
    "SECHSNLACHT",
    "SIEBENZWÖLF",
    "ZEHNEUNKUHR",
};

typedef struct {
    uint8_t row;
    uint8_t column;
    uint8_t length;
} word_span_t;

/* Coordinates count Unicode letters, never UTF-8 bytes. */
static const word_span_t WORD_ES = {0, 0, 2};
static const word_span_t WORD_IST = {0, 3, 3};
static const word_span_t WORD_FUENF_MINUTES = {0, 7, 4};
static const word_span_t WORD_ZEHN_MINUTES = {1, 0, 4};
static const word_span_t WORD_ZWANZIG = {1, 4, 7};
static const word_span_t WORD_VIERTEL = {2, 4, 7};
static const word_span_t WORD_VOR = {3, 0, 3};
static const word_span_t WORD_NACH = {3, 7, 4};
static const word_span_t WORD_HALB = {4, 0, 4};
static const word_span_t WORD_UHR = {9, 8, 3};
static const word_span_t WORD_EIN = {5, 0, 3};

/* Index 0 is unused; hour 0 and 12 both select ZWÖLF. */
static const word_span_t HOUR_WORDS[13] = {
    {0, 0, 0},
    {5, 0, 4},   /* EINS */
    {5, 7, 4},   /* ZWEI */
    {6, 0, 4},   /* DREI */
    {6, 7, 4},   /* VIER */
    {4, 7, 4},   /* FÜNF */
    {7, 0, 5},   /* SECHS */
    {8, 0, 6},   /* SIEBEN */
    {7, 7, 4},   /* ACHT */
    {9, 3, 4},   /* NEUN */
    {9, 0, 4},   /* ZEHN */
    {4, 5, 3},   /* ELF */
    {8, 6, 5},   /* ZWÖLF */
};

static void activate(word_clock_state_t *out, word_span_t word)
{
    for (uint8_t column = word.column; column < word.column + word.length; ++column)
        out->active[word.row * WORD_CLOCK_COLS + column] = true;
}

void word_clock_compute(int hour, int minute, bool ready, word_clock_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ready || hour < 0 || hour > 23 || minute < 0 || minute > 59) return;

    out->valid = true;
    out->minute_dots = (uint8_t)(minute % 5);
    activate(out, WORD_ES);
    activate(out, WORD_IST);

    int five_minute_slot = minute / 5;
    switch (five_minute_slot) {
    case 0:
        activate(out, WORD_UHR);
        break;
    case 1:
    case 11:
        activate(out, WORD_FUENF_MINUTES);
        break;
    case 2:
    case 10:
        activate(out, WORD_ZEHN_MINUTES);
        break;
    case 3:
    case 9:
        activate(out, WORD_VIERTEL);
        break;
    case 4:
    case 8:
        activate(out, WORD_ZWANZIG);
        break;
    case 5:
    case 7:
        activate(out, WORD_FUENF_MINUTES);
        activate(out, WORD_HALB);
        break;
    case 6:
        activate(out, WORD_HALB);
        break;
    }

    if (five_minute_slot >= 1 && five_minute_slot <= 4)
        activate(out, WORD_NACH);
    else if (five_minute_slot == 5 || five_minute_slot >= 8)
        activate(out, WORD_VOR);
    else if (five_minute_slot == 7)
        activate(out, WORD_NACH);

    int shown_hour = (hour + (five_minute_slot >= 5)) % 12;
    if (shown_hour == 0) shown_hour = 12;
    activate(out, five_minute_slot == 0 && shown_hour == 1
                      ? WORD_EIN : HOUR_WORDS[shown_hour]);
}
