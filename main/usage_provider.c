#include "usage_provider.h"

#include <math.h>
#include <string.h>

bool usage_provider_enabled(uint8_t selection, usage_provider_id_t id)
{
    if (id < 0 || id >= USAGE_PROVIDER_COUNT || selection > 2) return false;
    return selection == 2 || (selection == 0 && id == USAGE_PROVIDER_CLAUDE) ||
           (selection == 1 && id == USAGE_PROVIDER_CODEX);
}

const char *usage_provider_name(usage_provider_id_t id)
{
    switch (id) {
    case USAGE_PROVIDER_CLAUDE: return "Claude";
    case USAGE_PROVIDER_CODEX: return "Codex";
    default: return "Unknown";
    }
}

void usage_provider_init_unknown(provider_usage_t *usage)
{
    if (usage) memset(usage, 0, sizeof(*usage));
}

bool usage_provider_validate(provider_usage_t *usage)
{
    if (!usage) return false;
    bool valid = true;
    usage->status[sizeof(usage->status) - 1] = '\0';
    if (!usage->configured || !usage->has_sample || usage->sampled_at < 0 ||
        usage->sampled_us < 0) {
        if (usage->has_sample || usage->sampled_at || usage->sampled_us) valid = false;
        usage->has_sample = false;
        usage->sampled_at = 0;
        usage->sampled_us = 0;
    }
    for (unsigned i = 0; i < 2; ++i) {
        quota_window_t *window = &usage->windows[i];
        if (!usage->has_sample || !window->valid || !isfinite(window->used_percent) ||
            window->used_percent < 0 || window->used_percent > 100 ||
            window->resets_at < 0 || window->window_seconds < 60 ||
            window->window_seconds > 31u * 86400u) {
            if (window->valid) valid = false;
            memset(window, 0, sizeof(*window));
        }
    }
    if (!usage->reset_credits_known) usage->reset_credits = 0;
    return valid;
}

const quota_window_t *usage_provider_weekly_window(const provider_usage_t *usage)
{
    if (!usage || !usage->configured || !usage->has_sample ||
        usage->sampled_at < 0 || usage->sampled_us < 0) return NULL;
    for (unsigned i = 0; i < 2; ++i) {
        const quota_window_t *window = &usage->windows[i];
        if (window->valid && window->window_seconds == 7u * 86400u &&
            isfinite(window->used_percent) && window->used_percent >= 0 &&
            window->used_percent <= 100 && window->resets_at >= 0)
            return window;
    }
    return NULL;
}
