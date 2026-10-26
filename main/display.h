#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once from app_main before any other display function. */
void display_init(void);
void display_status(const char *title, const char *detail);
void display_setup(const char *url);
void display_usage(float five_used, const char *five_reset,
                   float week_used, const char *week_reset);
void display_updating(void);
void display_clock_ready(void);


/* Called under the LVGL lock. Queue the request without blocking; return true
 * only when it was accepted. */
typedef bool (*display_refresh_callback_t)(void *context);
void display_set_refresh_callback(display_refresh_callback_t callback, void *context);

/* Bracket one logical poll, including token refresh and any retry. */
void display_refresh_cycle_begin(void);
void display_refresh_cycle_end(void);

#ifdef __cplusplus
}
#endif

#endif
