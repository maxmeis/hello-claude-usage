#ifndef HELLO_BOX_UI_SHELL_H
#define HELLO_BOX_UI_SHELL_H

#include "app_registry.h"
#include "screensaver_registry.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_ROUTE_STATUS,
    UI_ROUTE_HOME,
    UI_ROUTE_APP,
    UI_ROUTE_SAVER,
} ui_route_t;

typedef enum {
    UI_OVERLAY_NONE,
    UI_OVERLAY_BOOT,
    UI_OVERLAY_FACE,
    UI_OVERLAY_SETTINGS,
    UI_OVERLAY_WIFI,
} ui_overlay_t;

typedef struct {
    ui_route_t route;
    app_id_t app_id;
    saver_id_t saver_id;
    ui_overlay_t overlay;
} ui_shell_view_t;

typedef struct {
    uint32_t generation;
    int64_t last_revision;
    bool has_revision;
    bool refreshing;
    bool seen_generation;
    bool suppress_generation;
} ui_shell_app_state_t;

/* All times are monotonic microseconds. Call from the UI thread/under its lock. */
typedef struct {
    const app_registry_t *apps;
    const screensaver_registry_t *savers;
    ui_shell_app_state_t app_state[APP_REGISTRY_CAPACITY];
    ui_route_t route;
    app_id_t app_id;
    saver_id_t saver_id;
    uint8_t default_view; /* 0: saver after idle, 1: Home */
    uint16_t idle_sec;
    int64_t last_now_us;
    int64_t manual_deadline_us;
    int64_t attention_deadline_us;
    int64_t pause_started_us;
    int64_t manual_face_until_us;
    bool attention_active;
    bool attention_pending;
    app_id_t pending_app_id;
    bool boot;
    bool face;
    bool settings;
    bool wifi;
    bool paused;
} ui_shell_t;

bool ui_shell_init(ui_shell_t *shell, const app_registry_t *apps,
                   const screensaver_registry_t *savers, uint8_t default_view,
                   uint16_t manual_idle_sec, saver_id_t saver_id, int64_t now_us);
bool ui_shell_navigate_home(ui_shell_t *shell, int64_t now_us);
bool ui_shell_navigate_app(ui_shell_t *shell, app_id_t id, int64_t now_us);
bool ui_shell_navigate_saver(ui_shell_t *shell, saver_id_t id, int64_t now_us);
void ui_shell_show_status(ui_shell_t *shell, int64_t now_us);
void ui_shell_user_activity(ui_shell_t *shell, int64_t now_us);
/* Call after accepting a user refresh to permit its face over manual Home/App. */
void ui_shell_mark_user_refresh(ui_shell_t *shell, int64_t now_us);
void ui_shell_refresh_begin(ui_shell_t *shell, app_id_t id, uint32_t generation,
                            int64_t now_us);
void ui_shell_refresh_end(ui_shell_t *shell, app_id_t id, uint32_t generation,
                          int64_t now_us);
void ui_shell_fresh_sample(ui_shell_t *shell, app_id_t id, int64_t revision,
                           int64_t now_us);
void ui_shell_set_face(ui_shell_t *shell, bool visible, int64_t now_us);
void ui_shell_set_settings(ui_shell_t *shell, bool visible, int64_t now_us);
void ui_shell_set_wifi(ui_shell_t *shell, bool visible, int64_t now_us);
void ui_shell_set_boot(ui_shell_t *shell, bool visible, int64_t now_us);
void ui_shell_tick(ui_shell_t *shell, int64_t now_us);
ui_shell_view_t ui_shell_view(const ui_shell_t *shell);
bool ui_shell_is_refreshing(const ui_shell_t *shell, app_id_t id);

#endif
