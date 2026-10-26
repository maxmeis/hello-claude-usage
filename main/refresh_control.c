#include "refresh_control.h"

void refresh_control_publish(refresh_control_t *control, bool ready, int64_t retry_at)
{
    control->ready = ready;
    control->retry_at = retry_at;
    if (!ready) control->pending = false;
}

bool refresh_control_request(refresh_control_t *control, int64_t now)
{
    if (!control->ready || control->busy || control->pending || now < control->retry_at)
        return false;
    control->pending = true;
    return true;
}

bool refresh_control_begin(refresh_control_t *control, bool *manual)
{
    if (control->busy || !control->ready) return false;
    *manual = control->pending;
    control->pending = false;
    control->busy = true;
    return true;
}

void refresh_control_finish(refresh_control_t *control, bool ready, int64_t retry_at)
{
    control->busy = false;
    refresh_control_publish(control, ready, retry_at);
}

bool refresh_control_usage_due(int64_t now, int64_t next_usage,
                               int64_t retry_until, int64_t expires_at)
{
    return now >= next_usage && now >= retry_until && now < expires_at;
}

int refresh_control_retry_delay(int failures, int retry_after)
{
    if (retry_after > 0) return retry_after;
    int delay = 5;
    for (int i = 1; i < failures && delay < 300; ++i) delay *= 2;
    return delay > 300 ? 300 : delay;
}

int refresh_control_usage_retry_delay(int failures, int final_status, int retry_after)
{
    return refresh_control_retry_delay(failures, final_status == 429 ? retry_after : 0);
}
