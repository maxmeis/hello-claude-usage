#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool display_parse_reset(const char *iso, int64_t *epoch);
void display_format_reset(char *out, size_t capacity, const char *iso, int64_t now,
                          bool clock_ready);
void display_format_age(char *out, size_t capacity, int64_t age_seconds);
uint32_t display_quota_color(float remaining);
