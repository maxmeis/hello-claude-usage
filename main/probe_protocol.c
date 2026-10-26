#include "probe_protocol.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int probe_retry_after_seconds(const char *value, int64_t now)
{
    if (!value || !*value) return 0;
    char *end;
    errno = 0;
    long long seconds = strtoll(value, &end, 10);
    if (end != value && *end == '\0' && errno == 0 && seconds >= 0)
        return seconds > INT_MAX ? INT_MAX : (int)seconds;

    /* IMF-fixdate is always UTC; avoid changing the process-wide Berlin TZ. */
    struct tm date = {0};
    end = strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &date);
    if (!end || *end || date.tm_year < 70 || date.tm_year > 8099 ||
        date.tm_mon < 0 || date.tm_mon > 11 || date.tm_mday < 1 || date.tm_mday > 31 ||
        date.tm_hour < 0 || date.tm_hour > 23 || date.tm_min < 0 || date.tm_min > 59 ||
        date.tm_sec < 0 || date.tm_sec > 59) return 0;
    int year = date.tm_year + 1900;
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (date.tm_mday > month_days[date.tm_mon] + (date.tm_mon == 1 && leap)) return 0;
    int prior_year = year - 1;
    int64_t days = 365LL * (year - 1970) +
        prior_year / 4 - 1969 / 4 - prior_year / 100 + 1969 / 100 +
        prior_year / 400 - 1969 / 400;
    for (int month = 0; month < date.tm_mon; ++month)
        days += month_days[month] + (month == 1 && leap);
    days += date.tm_mday - 1;
    seconds = days * 86400 + date.tm_hour * 3600 + date.tm_min * 60 + date.tm_sec - now;
    return seconds <= 0 ? 0 : seconds > INT_MAX ? INT_MAX : (int)seconds;
}

bool probe_authorization_code(char *input, const char *expected_state, char **code)
{
    if (!input || !expected_state || !*expected_state || !code) return false;
    while (isspace((unsigned char)*input)) ++input;
    size_t length = strlen(input);
    while (length && isspace((unsigned char)input[length - 1])) input[--length] = 0;
    char *state = strchr(input, '#');
    if (!state || state == input || strcmp(state + 1, expected_state) != 0) return false;
    *state = 0;
    *code = input;
    return true;
}
