#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once from app_main before any other display function. */
void display_init(void);
/* Set after identity initialization; copies a validated hello-box-#### name. */
void display_set_device_name(const char *name);
void display_status(const char *title, const char *detail);
void display_setup(const char *url);
void display_usage(float five_used, const char *five_reset,
                   float week_used, const char *week_reset);
void display_updating(void);
void display_clock_ready(void);

/* Called under the LVGL lock. Queue open/close without blocking. The request
 * ID identifies a Settings session; a stale QR or close is ignored. */
typedef bool (*display_settings_callback_t)(void *context, uint32_t request_id, bool open);
void display_set_settings_callback(display_settings_callback_t callback, void *context);
void display_settings_qr(uint32_t request_id, const char *url);
void display_settings_closed(uint32_t request_id);

/* Wi-Fi provisioning has priority over every other display view. */
void display_wifi_setup(const char *join_qr, const char *page_url,
                        bool phone_joined, const char *message);
void display_wifi_setup_done(void);


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
