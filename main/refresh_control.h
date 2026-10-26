#ifndef REFRESH_CONTROL_H
#define REFRESH_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

/* The caller holds its short FreeRTOS critical section around every operation.
 * No credentials or HTTP state are shared with the display task. */
typedef struct {
    bool ready;
    bool pending;
    bool busy;
    int64_t retry_at;
} refresh_control_t;

void refresh_control_publish(refresh_control_t *control, bool ready, int64_t retry_at);
bool refresh_control_request(refresh_control_t *control, int64_t now);
bool refresh_control_begin(refresh_control_t *control, bool *manual);
void refresh_control_finish(refresh_control_t *control, bool ready, int64_t retry_at);

/* Keep the usage endpoint's retry deadline independent of reconnect and token
 * refresh scheduling. Automatic and manual GET admission use this same gate. */
bool refresh_control_usage_due(int64_t now, int64_t next_usage,
                               int64_t retry_until, int64_t expires_at);
int refresh_control_retry_delay(int failures, int retry_after);
int refresh_control_usage_retry_delay(int failures, int final_status, int retry_after);

#endif
