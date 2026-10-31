#include "ui_shell.h"

#include <limits.h>
#include <string.h>

#define US_PER_SEC INT64_C(1000000)
#define ATTENTION_US (INT64_C(8) * US_PER_SEC)
#define USER_REFRESH_MARK_US (INT64_C(2) * US_PER_SEC)

static int64_t add_us(int64_t a, int64_t b)
{
    return b > INT64_MAX - a ? INT64_MAX : a + b;
}

static int64_t now(ui_shell_t *shell, int64_t t)
{
    if (t < shell->last_now_us) return shell->last_now_us;
    shell->last_now_us = t;
    return t;
}

static ui_overlay_t overlay(const ui_shell_t *shell)
{
    if (shell->wifi) return UI_OVERLAY_WIFI;
    if (shell->settings) return UI_OVERLAY_SETTINGS;
    if (shell->boot) return UI_OVERLAY_BOOT;
    if (shell->face) return UI_OVERLAY_FACE;
    return UI_OVERLAY_NONE;
}

static void sync_pause(ui_shell_t *shell, ui_overlay_t before, int64_t t)
{
    ui_overlay_t after = overlay(shell);
    if (before == UI_OVERLAY_NONE && after != UI_OVERLAY_NONE) {
        shell->paused = true;
        shell->pause_started_us = t;
    } else if (before != UI_OVERLAY_NONE && after == UI_OVERLAY_NONE && shell->paused) {
        int64_t elapsed = t - shell->pause_started_us;
        if (shell->manual_deadline_us) shell->manual_deadline_us = add_us(shell->manual_deadline_us, elapsed);
        if (shell->attention_deadline_us) shell->attention_deadline_us = add_us(shell->attention_deadline_us, elapsed);
        shell->paused = false;
    }
}

static int app_index(const ui_shell_t *shell, app_id_t id)
{
    if (!shell || !shell->apps || id == APP_ID_NONE) return -1;
    for (size_t i = 0; i < app_registry_count(shell->apps); ++i) {
        const app_descriptor_t *app = app_registry_at(shell->apps, i);
        if (app && app->id == id) return (int)i;
    }
    return -1;
}

static void cancel_attention(ui_shell_t *shell)
{
    shell->attention_active = false;
    shell->attention_pending = false;
    shell->attention_deadline_us = 0;
    shell->pending_app_id = APP_ID_NONE;
}

static void idle_route(ui_shell_t *shell)
{
    shell->route = shell->default_view == 0 ? UI_ROUTE_SAVER : UI_ROUTE_HOME;
    shell->app_id = APP_ID_NONE;
    shell->manual_deadline_us = 0;
}

static void start_attention(ui_shell_t *shell, app_id_t id, int64_t t)
{
    size_t enabled = 0;
    for (size_t i = 0; i < app_registry_count(shell->apps); ++i) {
        const app_descriptor_t *app = app_registry_at(shell->apps, i);
        if (app_registry_is_enabled(shell->apps, app->id)) ++enabled;
    }
    shell->route = enabled >= 2 ? UI_ROUTE_HOME : UI_ROUTE_APP;
    shell->app_id = enabled >= 2 ? APP_ID_NONE : id;
    shell->attention_active = true;
    shell->attention_pending = false;
    shell->pending_app_id = APP_ID_NONE;
    shell->attention_deadline_us = add_us(t, ATTENTION_US);
    shell->manual_deadline_us = 0;
}

bool ui_shell_init(ui_shell_t *shell, const app_registry_t *apps,
                   const screensaver_registry_t *savers, uint8_t default_view,
                   uint16_t manual_idle_sec, saver_id_t saver_id, int64_t now_us)
{
    if (!shell || !apps || !savers || default_view > 1 || manual_idle_sec < 10 ||
        manual_idle_sec > 120 || now_us < 0 ||
        !screensaver_registry_find(savers, saver_id)) return false;
    memset(shell, 0, sizeof(*shell));
    shell->apps = apps;
    shell->savers = savers;
    shell->route = UI_ROUTE_HOME;
    shell->saver_id = saver_id;
    shell->default_view = default_view;
    shell->idle_sec = manual_idle_sec;
    shell->last_now_us = now_us;
    shell->manual_deadline_us = add_us(now_us, (int64_t)manual_idle_sec * US_PER_SEC);
    return true;
}

bool ui_shell_navigate_home(ui_shell_t *shell, int64_t now_us)
{
    if (!shell || shell->wifi || shell->settings || shell->boot) return false;
    int64_t t = now(shell, now_us);
    ui_overlay_t before = overlay(shell);
    shell->face = false;
    sync_pause(shell, before, t);
    cancel_attention(shell);
    shell->route = UI_ROUTE_HOME;
    shell->app_id = APP_ID_NONE;
    shell->manual_deadline_us = add_us(t, (int64_t)shell->idle_sec * US_PER_SEC);
    return true;
}

bool ui_shell_navigate_app(ui_shell_t *shell, app_id_t id, int64_t now_us)
{
    if (!shell || shell->wifi || shell->settings || shell->boot ||
        !app_registry_is_enabled(shell->apps, id) ||
        !app_registry_is_configured(shell->apps, id)) return false;
    if (!ui_shell_navigate_home(shell, now_us)) return false;
    shell->route = UI_ROUTE_APP;
    shell->app_id = id;
    return true;
}

bool ui_shell_navigate_saver(ui_shell_t *shell, saver_id_t id, int64_t now_us)
{
    if (!shell || shell->wifi || shell->settings || shell->boot ||
        !screensaver_registry_find(shell->savers, id)) return false;
    int64_t t = now(shell, now_us);
    ui_overlay_t before = overlay(shell);
    shell->face = false;
    sync_pause(shell, before, t);
    cancel_attention(shell);
    for (size_t i = 0; i < app_registry_count(shell->apps); ++i) {
        if (shell->app_state[i].refreshing) shell->app_state[i].suppress_generation = true;
    }
    shell->route = UI_ROUTE_SAVER;
    shell->app_id = APP_ID_NONE;
    shell->saver_id = id;
    shell->manual_deadline_us = 0;
    shell->manual_face_until_us = 0;
    return true;
}

void ui_shell_show_status(ui_shell_t *shell, int64_t now_us)
{
    if (!shell) return;
    int64_t t = now(shell, now_us);
    ui_overlay_t before = overlay(shell);
    shell->face = false;
    sync_pause(shell, before, t);
    cancel_attention(shell);
    shell->route = UI_ROUTE_STATUS;
    shell->app_id = APP_ID_NONE;
    shell->manual_deadline_us = 0;
}

void ui_shell_user_activity(ui_shell_t *shell, int64_t now_us)
{
    if (!shell || overlay(shell) != UI_OVERLAY_NONE) return;
    int64_t t = now(shell, now_us);
    if (shell->route == UI_ROUTE_HOME || shell->route == UI_ROUTE_APP) {
        cancel_attention(shell);
        shell->manual_deadline_us = add_us(t, (int64_t)shell->idle_sec * US_PER_SEC);
    }
}

void ui_shell_mark_user_refresh(ui_shell_t *shell, int64_t now_us)
{
    if (!shell || shell->wifi || shell->settings || shell->boot) return;
    int64_t t = now(shell, now_us);
    if (shell->attention_active) {
        cancel_attention(shell);
    }
    if (shell->route == UI_ROUTE_HOME || shell->route == UI_ROUTE_APP)
        shell->manual_deadline_us = add_us(t, (int64_t)shell->idle_sec * US_PER_SEC);
    shell->manual_face_until_us = add_us(t, USER_REFRESH_MARK_US);
}

void ui_shell_refresh_begin(ui_shell_t *shell, app_id_t id, uint32_t generation,
                            int64_t now_us)
{
    if (!shell || !generation) return;
    now(shell, now_us);
    if (id == APP_ID_ALL) {
        for (size_t i = 0; i < app_registry_count(shell->apps); ++i) {
            const app_descriptor_t *app = app_registry_at(shell->apps, i);
            ui_shell_refresh_begin(shell, app->id, generation, now_us);
        }
        return;
    }
    int i = app_index(shell, id);
    if (i < 0) return;
    ui_shell_app_state_t *state = &shell->app_state[i];
    if (state->seen_generation && generation <= state->generation) return;
    state->generation = generation;
    state->seen_generation = true;
    state->refreshing = true;
    state->suppress_generation = false;
}

void ui_shell_refresh_end(ui_shell_t *shell, app_id_t id, uint32_t generation,
                          int64_t now_us)
{
    if (!shell || !generation) return;
    now(shell, now_us);
    if (id == APP_ID_ALL) {
        for (size_t i = 0; i < app_registry_count(shell->apps); ++i) {
            const app_descriptor_t *app = app_registry_at(shell->apps, i);
            ui_shell_refresh_end(shell, app->id, generation, now_us);
        }
        return;
    }
    int i = app_index(shell, id);
    if (i >= 0 && shell->app_state[i].seen_generation &&
        shell->app_state[i].generation == generation)
        shell->app_state[i].refreshing = false;
}

void ui_shell_fresh_sample(ui_shell_t *shell, app_id_t id, int64_t revision,
                           int64_t now_us)
{
    if (!shell || revision < 0) return;
    int64_t t = now(shell, now_us);
    int i = app_index(shell, id);
    if (i < 0) return;
    ui_shell_app_state_t *state = &shell->app_state[i];
    if (state->has_revision && revision <= state->last_revision) return;
    state->has_revision = true;
    state->last_revision = revision;
    const app_descriptor_t *app = app_registry_at(shell->apps, (size_t)i);
    if (!app || !(app->capabilities & APP_CAP_INTERRUPT_SAVER) ||
        !app_registry_is_enabled(shell->apps, id) ||
        !app_registry_is_configured(shell->apps, id) ||
        state->suppress_generation ||
        shell->wifi || shell->settings || shell->boot || shell->attention_active ||
        shell->route != UI_ROUTE_SAVER) return;
    if (shell->face) {
        shell->attention_pending = true;
        shell->pending_app_id = id;
    } else {
        start_attention(shell, id, t);
    }
}

void ui_shell_set_face(ui_shell_t *shell, bool visible, int64_t now_us)
{
    if (!shell) return;
    int64_t t = now(shell, now_us);
    if (visible && (shell->wifi || shell->settings || shell->boot ||
        (shell->route != UI_ROUTE_SAVER &&
         (shell->manual_face_until_us == 0 || t > shell->manual_face_until_us)))) return;
    if (visible == shell->face) return;
    ui_overlay_t before = overlay(shell);
    shell->face = visible;
    sync_pause(shell, before, t);
    if (!visible && shell->attention_pending && shell->route == UI_ROUTE_SAVER &&
        !shell->wifi && !shell->settings && !shell->boot) {
        start_attention(shell, shell->pending_app_id, t);
    }
    if (!visible) shell->manual_face_until_us = 0;
}

static void discard_for_blocking_overlay(ui_shell_t *shell)
{
    if (shell->attention_active) idle_route(shell);
    cancel_attention(shell);
    shell->face = false;
    shell->manual_face_until_us = 0;
}

void ui_shell_set_settings(ui_shell_t *shell, bool visible, int64_t now_us)
{
    if (!shell || (visible && shell->wifi)) return;
    int64_t t = now(shell, now_us);
    ui_overlay_t before = overlay(shell);
    if (visible) discard_for_blocking_overlay(shell);
    shell->settings = visible;
    sync_pause(shell, before, t);
    if (!visible && before == UI_OVERLAY_SETTINGS && shell->route != UI_ROUTE_SAVER &&
        shell->route != UI_ROUTE_STATUS)
        shell->manual_deadline_us = add_us(t, (int64_t)shell->idle_sec * US_PER_SEC);
}

void ui_shell_set_wifi(ui_shell_t *shell, bool visible, int64_t now_us)
{
    if (!shell) return;
    int64_t t = now(shell, now_us);
    ui_overlay_t before = overlay(shell);
    if (visible) {
        discard_for_blocking_overlay(shell);
        shell->settings = false;
    }
    shell->wifi = visible;
    sync_pause(shell, before, t);
    if (!visible && before == UI_OVERLAY_WIFI) {
        shell->route = UI_ROUTE_HOME;
        shell->app_id = APP_ID_NONE;
        shell->manual_deadline_us = add_us(t, (int64_t)shell->idle_sec * US_PER_SEC);
    }
}

void ui_shell_set_boot(ui_shell_t *shell, bool visible, int64_t now_us)
{
    if (!shell) return;
    int64_t t = now(shell, now_us);
    ui_overlay_t before = overlay(shell);
    if (visible) discard_for_blocking_overlay(shell);
    shell->boot = visible;
    sync_pause(shell, before, t);
    if (!visible && before == UI_OVERLAY_BOOT && !shell->settings && !shell->wifi &&
        shell->route != UI_ROUTE_STATUS) {
        shell->route = UI_ROUTE_HOME;
        shell->app_id = APP_ID_NONE;
        shell->manual_deadline_us = add_us(t, (int64_t)shell->idle_sec * US_PER_SEC);
    }
}

void ui_shell_tick(ui_shell_t *shell, int64_t now_us)
{
    if (!shell) return;
    int64_t t = now(shell, now_us);
    if (overlay(shell) != UI_OVERLAY_NONE) return;
    if (shell->attention_active && t >= shell->attention_deadline_us) {
        cancel_attention(shell);
        idle_route(shell);
    } else if (!shell->attention_active && shell->manual_deadline_us &&
               t >= shell->manual_deadline_us) {
        idle_route(shell);
    }
}

ui_shell_view_t ui_shell_view(const ui_shell_t *shell)
{
    ui_shell_view_t view = { UI_ROUTE_STATUS, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE };
    if (shell) {
        view.route = shell->route;
        view.app_id = shell->app_id;
        view.saver_id = shell->saver_id;
        view.overlay = overlay(shell);
    }
    return view;
}

bool ui_shell_is_refreshing(const ui_shell_t *shell, app_id_t id)
{
    if (!shell) return false;
    if (id == APP_ID_ALL) {
        for (size_t i = 0; i < app_registry_count(shell->apps); ++i) {
            if (shell->app_state[i].refreshing) return true;
        }
        return false;
    }
    int i = app_index(shell, id);
    return i >= 0 && shell->app_state[i].refreshing;
}
