#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "app_registry.h"
#include "screensaver_registry.h"
#include "usage_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once from app_main before any other display function. */
void display_init(void);
/* Set after identity initialization; copies a validated hello-box-#### name. */
void display_set_device_name(const char *name);
void display_status(const char *title, const char *detail);
void display_setup(const char *url);
/* Dismiss an automatic legacy account QR when the selected app set changes. */
void display_setup_closed(void);
void display_usage(float five_used, const char *five_reset,
                   float week_used, const char *week_reset);
/* Copy a provider snapshot and redraw the selected dashboard. */
void display_provider_update(usage_provider_id_t id, const provider_usage_t *usage);
void display_updating(void);
void display_clock_ready(void);

/* Read-only registration metadata; valid after display_init(). */
const app_registry_t *display_app_registry(void);
const screensaver_registry_t *display_screensaver_registry(void);

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

/* Called under the LVGL lock. Queue a refresh for one app, or APP_ID_ALL. */
/* Bit 1u << app_id for each app whose request was actually accepted. */
typedef uint32_t (*display_app_refresh_callback_t)(void *context, app_id_t id);
void display_set_app_refresh_callback(display_app_refresh_callback_t callback, void *context);

/* Bracket one provider's logical poll cycle, including its retries. */
void display_app_refresh_begin(app_id_t id, uint32_t generation);
void display_app_refresh_end(app_id_t id, uint32_t generation);

/* Bracket one logical poll, including token refresh and any retry. */
void display_refresh_cycle_begin(void);
void display_refresh_cycle_end(void);

#ifdef __cplusplus
}
#endif

#endif
