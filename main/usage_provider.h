#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USAGE_PROVIDER_CLAUDE = 0,
    USAGE_PROVIDER_CODEX = 1,
    USAGE_PROVIDER_COUNT = 2,
} usage_provider_id_t;

typedef struct {
    bool valid;
    float used_percent;
    int64_t resets_at;           /* Unix seconds; zero means unknown. */
    uint32_t window_seconds;
} quota_window_t;

typedef struct {
    bool configured;
    bool has_sample;
    bool updating;
    int64_t sampled_at;          /* Unix seconds. */
    int64_t sampled_us;          /* Monotonic microseconds. */
    quota_window_t windows[2];
    bool reset_credits_known;
    uint32_t reset_credits;      /* Available credits, never consumed here. */
    char status[72];
} provider_usage_t;

bool usage_provider_enabled(uint8_t selection, usage_provider_id_t id);
const char *usage_provider_name(usage_provider_id_t id);
void usage_provider_init_unknown(provider_usage_t *usage);
/* Invalid windows are cleared individually. Returns false if any field was repaired. */
bool usage_provider_validate(provider_usage_t *usage);
/* Select an actually reported seven-day quota; never relabel another window. */
const quota_window_t *usage_provider_weekly_window(const provider_usage_t *usage);
