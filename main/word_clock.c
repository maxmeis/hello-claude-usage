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

const char *const word_clock_english_rows[WORD_CLOCK_ROWS] = {
    "ITLISASTIME", "ACQUARTERDC", "TWENTYFIVEX", "HALFBTENFTO",
    "PASTERUNINE", "ONESIXTHREE", "FOURFIVETWO", "EIGHTELEVEN",
    "SEVENTWELVE", "TENSEOCLOCK",
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

const char *const *word_clock_rows_for_language(uint8_t language)
{
    return language == WORD_CLOCK_ENGLISH ? word_clock_english_rows : word_clock_rows;
}

void word_clock_compute_language(int hour, int minute, bool ready,
                                 uint8_t language, word_clock_state_t *out)
{
    if (language != WORD_CLOCK_ENGLISH) {
        word_clock_compute(hour, minute, ready, out);
        return;
    }
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ready || hour < 0 || hour > 23 || minute < 0 || minute > 59) return;

    static const word_span_t words[] = {
        {2, 6, 4}, /* FIVE */ {3, 5, 3}, /* TEN */
        {1, 2, 7}, /* QUARTER */ {2, 0, 6}, /* TWENTY */
        {3, 0, 4}, /* HALF */
    };
    static const word_span_t hours[13] = {
        {0, 0, 0}, {5, 0, 3}, {6, 8, 3}, {5, 6, 5},
        {6, 0, 4}, {6, 4, 4}, {5, 3, 3}, {8, 0, 5},
        {7, 0, 5}, {4, 7, 4}, {9, 0, 3}, {7, 5, 6},
        {8, 5, 6},
    };
    out->valid = true;
    out->minute_dots = (uint8_t)(minute % 5);
    activate(out, (word_span_t){0, 0, 2}); /* IT */
    activate(out, (word_span_t){0, 3, 2}); /* IS */
    int slot = minute / 5;
    if (slot == 0) {
        activate(out, (word_span_t){9, 5, 6}); /* OCLOCK */
    } else {
        if (slot == 1 || slot == 5 || slot == 7 || slot == 11) activate(out, words[0]);
        if (slot == 2 || slot == 10) activate(out, words[1]);
        if (slot == 3 || slot == 9) {
            activate(out, (word_span_t){1, 0, 1}); /* A */
            activate(out, words[2]);
        }
        if (slot == 4 || slot == 5 || slot == 7 || slot == 8) activate(out, words[3]);
        if (slot == 6) activate(out, words[4]);
        if (slot >= 1 && slot <= 6) activate(out, (word_span_t){4, 0, 4}); /* PAST */
        else activate(out, (word_span_t){3, 9, 2}); /* TO */
    }
    int shown_hour = (hour + (slot >= 7)) % 12;
    if (shown_hour == 0) shown_hour = 12;
    activate(out, hours[shown_hour]);
}
