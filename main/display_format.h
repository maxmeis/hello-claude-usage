#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool display_parse_reset(const char *iso, int64_t *epoch);

typedef enum {
    DISPLAY_FORECAST_UNKNOWN,
    DISPLAY_FORECAST_HEADROOM,
    DISPLAY_FORECAST_TIGHT
} display_forecast_t;

/* Classify projected end-of-window utilization from the pace at sampled_at. */
display_forecast_t display_forecast(float utilized, const char *reset_iso,
                                    int64_t window_seconds, int64_t sampled_at,
                                    int64_t now, bool fresh);

void display_format_reset(char *out, size_t capacity, const char *iso, int64_t now,
                          bool clock_ready);
void display_format_age(char *out, size_t capacity, int64_t age_seconds);
uint32_t display_quota_color(float remaining);
