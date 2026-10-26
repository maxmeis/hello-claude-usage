#include "display_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool parse_digits(const char **cursor, size_t count, int *value)
{
    int result = 0;
    for (size_t i = 0; i < count; ++i) {
        char ch = (*cursor)[i];
        if (ch == '\0' || ch < '0' || ch > '9') return false;
        result = result * 10 + ch - '0';
    }
    *cursor += count;
    *value = result;
    return true;
}

static bool consume(const char **cursor, char expected)
{
    if (**cursor != expected) return false;
    ++*cursor;
    return true;
}

bool display_parse_reset(const char *iso, int64_t *epoch)
{
    if (!iso || !epoch) return false;

    const char *cursor = iso;
    int year, month, day, hour, minute, second;
    if (!parse_digits(&cursor, 4, &year) || !consume(&cursor, '-') ||
        !parse_digits(&cursor, 2, &month) || !consume(&cursor, '-') ||
        !parse_digits(&cursor, 2, &day) || !consume(&cursor, 'T') ||
        !parse_digits(&cursor, 2, &hour) || !consume(&cursor, ':') ||
        !parse_digits(&cursor, 2, &minute) || !consume(&cursor, ':') ||
        !parse_digits(&cursor, 2, &second)) return false;

    if (year < 1 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59)
        return false;
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    int month_days = days_in_month[month - 1] + (month == 2 && leap);
    if (day < 1 || day > month_days) return false;

    /* Fractions do not affect this whole-second epoch, but must be well formed. */
    if (*cursor == '.') {
        ++cursor;
        const char *fraction = cursor;
        while (*cursor >= '0' && *cursor <= '9') ++cursor;
        if (cursor == fraction) return false;
    }

    int offset_sign = 0, offset_hour = 0, offset_minute = 0;
    if (*cursor == 'Z') {
        ++cursor;
    } else if (*cursor == '+' || *cursor == '-') {
        offset_sign = *cursor++ == '+' ? 1 : -1;
        if (!parse_digits(&cursor, 2, &offset_hour) || !consume(&cursor, ':') ||
            !parse_digits(&cursor, 2, &offset_minute) || offset_hour > 23 ||
            offset_minute > 59) return false;
    } else {
        return false;
    }
    if (*cursor != '\0') return false;

    int prior_year = year - 1;
    int64_t days = 365LL * (year - 1970) +
        prior_year / 4 - 1969 / 4 - prior_year / 100 + 1969 / 100 +
        prior_year / 400 - 1969 / 400;
    for (int m = 1; m < month; ++m)
        days += days_in_month[m - 1] + (m == 2 && leap);
    days += day - 1;

    int64_t result = days * 86400 + hour * 3600 + minute * 60 + second;
    result -= offset_sign * (offset_hour * 3600 + offset_minute * 60);
    *epoch = result;
    return true;
}

void display_format_reset(char *out, size_t capacity, const char *iso, int64_t now,
                          bool clock_ready)
{
    if (!out || capacity == 0) return;
    out[0] = '\0';

    int64_t reset_epoch;
    if (!clock_ready || !display_parse_reset(iso, &reset_epoch)) {
        (void)snprintf(out, capacity, "%s", "Reset unknown");
        return;
    }
    if (reset_epoch <= now) {
        (void)snprintf(out, capacity, "%s", "Awaiting reset");
        return;
    }

    /* ISO years are bounded, but now may be any int64_t supplied by the caller. */
    uint64_t remaining = (uint64_t)reset_epoch - (uint64_t)now;
    if (remaining < 60) {
        (void)snprintf(out, capacity, "%s", "Resets in <1m");
    } else if (remaining >= 86400) {
        uint64_t days = remaining / 86400;
        uint64_t hours = (remaining % 86400) / 3600;
        (void)snprintf(out, capacity, "Resets in %llud %lluh",
                       (unsigned long long)days, (unsigned long long)hours);
    } else {
        uint64_t hours = remaining / 3600;
        uint64_t minutes = (remaining % 3600) / 60;
        (void)snprintf(out, capacity, "Resets in %lluh %llum",
                       (unsigned long long)hours, (unsigned long long)minutes);
    }
}

static uint32_t interpolate_color(uint32_t first, uint32_t second, float fraction)
{
    uint32_t result = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int a = (int)((first >> shift) & 0xff);
        int b = (int)((second >> shift) & 0xff);
        int channel = (int)(a + (b - a) * fraction + 0.5f);
        result |= (uint32_t)channel << shift;
    }
    return result;
}

uint32_t display_quota_color(float remaining)
{
    const uint32_t unknown = 0x5D625E;
    const uint32_t low = 0xDA624E;
    const uint32_t medium = 0xD97757;
    const uint32_t high = 0xC6CF82;
    if (!isfinite(remaining) || remaining < 0.0f || remaining > 100.0f) return unknown;
    if (remaining >= 80.0f) return high;
    if (remaining >= 50.0f)
        return interpolate_color(medium, high, (remaining - 50.0f) / 30.0f);
    return interpolate_color(low, medium, remaining / 50.0f);
}
