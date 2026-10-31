"""Exercise the production, LVGL-independent shell with future app descriptors."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
SECOND = 1_000_000

HARNESS = r'''
#include "app_registry.h"
#include "screensaver_registry.h"
#include "ui_shell.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#define S 1000000LL
#define VIEW(shell, want_route, want_overlay) do { \
    ui_shell_view_t v = ui_shell_view(&(shell)); \
    assert(v.route == (want_route)); assert(v.overlay == (want_overlay)); \
} while (0)

typedef struct { bool enabled, configured; unsigned refresh_calls; } fake_app_t;
static bool enabled(void *p) { return ((fake_app_t *)p)->enabled; }
static bool configured(void *p) { return ((fake_app_t *)p)->configured; }
static bool request(void *p) { ((fake_app_t *)p)->refresh_calls++; return true; }
static void noop_mount(void *ctx, void *parent) { (void)ctx; (void)parent; }
static void noop_update(void *ctx) { (void)ctx; }
static void noop_visible(void *ctx, bool visible) { (void)ctx; (void)visible; }

static app_registry_t apps;
static screensaver_registry_t savers;
static fake_app_t claude = {true, true, 0};
static fake_app_t codex = {true, true, 0};
static fake_app_t future = {true, true, 0};

static void register_app(app_id_t id, fake_app_t *ctx)
{
    app_descriptor_t d = {id, "app", APP_CAP_REFRESH | APP_CAP_INTERRUPT_SAVER,
                          ctx, enabled, configured, request,
                          noop_mount, noop_mount, noop_update};
    assert(app_registry_register(&apps, &d));
}

static void setup(bool with_codex)
{
    app_registry_reset(&apps);
    screensaver_registry_reset(&savers);
    claude = (fake_app_t){true, true, 0};
    codex = (fake_app_t){true, true, 0};
    future = (fake_app_t){true, true, 0};
    register_app(APP_ID_CLAUDE, &claude);
    if (with_codex) register_app(APP_ID_CODEX, &codex);
    screensaver_descriptor_t d = {SAVER_ID_WORD_CLOCK, "clock", NULL,
                                   noop_mount, noop_update, noop_visible};
    assert(screensaver_registry_register(&savers, &d));
}

static ui_shell_t shell_at(bool with_codex)
{
    setup(with_codex);
    ui_shell_t shell;
    assert(ui_shell_init(&shell, &apps, &savers, 0, 30, SAVER_ID_WORD_CLOCK, 0));
    return shell;
}

static void registry_cases(void)
{
    setup(false);
    assert(app_registry_count(&apps) == 1);
    assert(app_registry_find(&apps, APP_ID_CLAUDE));
    assert(!app_registry_find(&apps, APP_ID_NONE));
    app_descriptor_t duplicate = *app_registry_at(&apps, 0);
    assert(!app_registry_register(&apps, &duplicate));
    duplicate.id = APP_ID_NONE;
    assert(!app_registry_register(&apps, &duplicate));
    for (app_id_t id = 3; id < 10; ++id) register_app(id, &future);
    assert(app_registry_count(&apps) == APP_REGISTRY_CAPACITY);
    duplicate.id = 10;
    assert(!app_registry_register(&apps, &duplicate));
    assert(app_registry_find(&apps, 9));
    assert(!app_registry_find(&apps, 10));
    assert(app_registry_request_refresh(&apps, 9));
    assert(future.refresh_calls == 1);
    future.enabled = false;
    assert(!app_registry_request_refresh(&apps, 9));
    assert(screensaver_registry_count(&savers) == 1);
    assert(!screensaver_registry_register(&savers, screensaver_registry_at(&savers, 0)));
}

static void idle_and_manual_routes(void)
{
    ui_shell_t sh = shell_at(true);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE);
    assert(!ui_shell_navigate_app(&sh, 99, S));
    codex.configured = false;
    assert(!ui_shell_navigate_app(&sh, APP_ID_CODEX, S));
    assert(ui_shell_navigate_app(&sh, APP_ID_CLAUDE, S));
    assert(ui_shell_view(&sh).app_id == APP_ID_CLAUDE);
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 100, 2*S);
    ui_shell_tick(&sh, 30*S);
    VIEW(sh, UI_ROUTE_APP, UI_OVERLAY_NONE); /* data did not extend idle */
    ui_shell_tick(&sh, 31*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE);
    assert(ui_shell_navigate_home(&sh, 32*S));
    ui_shell_user_activity(&sh, 50*S);
    ui_shell_tick(&sh, 79*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE);
    ui_shell_tick(&sh, 80*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE);
    assert(ui_shell_navigate_home(&sh, 81*S)); /* saver tap */
    assert(ui_shell_navigate_saver(&sh, SAVER_ID_WORD_CLOCK, 82*S));
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE);
}

static void fresh_lease_and_coalescing(void)
{
    ui_shell_t sh = shell_at(true);
    ui_shell_tick(&sh, 30*S);
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 1, 31*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE); /* 2 enabled apps -> widgets */
    ui_shell_refresh_begin(&sh, APP_ID_CODEX, 1, 32*S);
    ui_shell_set_face(&sh, true, 32*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE); /* second app cannot pause lease */
    ui_shell_fresh_sample(&sh, APP_ID_CODEX, 1, 35*S);
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 1, 36*S); /* duplicate */
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 2, 37*S); /* new revision */
    ui_shell_tick(&sh, 38*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE);
    ui_shell_tick(&sh, 39*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE); /* no extension */
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 3, 40*S);
    assert(ui_shell_navigate_app(&sh, APP_ID_CODEX, 41*S));
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 4, 42*S);
    ui_shell_tick(&sh, 48*S);
    VIEW(sh, UI_ROUTE_APP, UI_OVERLAY_NONE); /* manual app keeps focus */
    assert(ui_shell_view(&sh).app_id == APP_ID_CODEX);
    ui_shell_tick(&sh, 71*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE);
    codex.enabled = false;
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 5, 72*S);
    VIEW(sh, UI_ROUTE_APP, UI_OVERLAY_NONE); /* one enabled -> detail */
    assert(ui_shell_view(&sh).app_id == APP_ID_CLAUDE);
}

static void face_and_overlay_cases(void)
{
    ui_shell_t sh = shell_at(false);
    ui_shell_tick(&sh, 30*S);
    ui_shell_refresh_begin(&sh, APP_ID_CLAUDE, 1, 31*S);
    ui_shell_set_face(&sh, true, 31*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_FACE);
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 10, 32*S);
    ui_shell_tick(&sh, 40*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_FACE);
    ui_shell_set_face(&sh, false, 40*S);
    VIEW(sh, UI_ROUTE_APP, UI_OVERLAY_NONE); /* eight seconds start now */
    ui_shell_tick(&sh, 47*S);
    VIEW(sh, UI_ROUTE_APP, UI_OVERLAY_NONE);
    ui_shell_tick(&sh, 48*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE);

    ui_shell_set_face(&sh, true, 49*S);
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 11, 50*S);
    ui_shell_set_settings(&sh, true, 51*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_SETTINGS);
    ui_shell_set_settings(&sh, false, 60*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE); /* pending discarded */
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 12, 61*S);
    VIEW(sh, UI_ROUTE_APP, UI_OVERLAY_NONE);
    ui_shell_set_wifi(&sh, true, 62*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_WIFI);
    ui_shell_set_settings(&sh, true, 63*S); /* Wi-Fi keeps priority */
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_WIFI);
    ui_shell_set_wifi(&sh, false, 64*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE);
    ui_shell_set_boot(&sh, true, 65*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_BOOT);
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 13, 66*S);
    ui_shell_set_boot(&sh, false, 70*S);
    ui_shell_tick(&sh, 99*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE);
    ui_shell_tick(&sh, 100*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE);
}

static void generation_and_clock_escape(void)
{
    ui_shell_t sh = shell_at(true);
    ui_shell_tick(&sh, 30*S);
    ui_shell_refresh_begin(&sh, APP_ID_CLAUDE, 1, 31*S);
    ui_shell_refresh_begin(&sh, APP_ID_CODEX, 4, 31*S);
    assert(ui_shell_is_refreshing(&sh, APP_ID_ALL));
    assert(ui_shell_is_refreshing(&sh, APP_ID_CLAUDE));
    ui_shell_refresh_end(&sh, APP_ID_CLAUDE, 1, 32*S);
    assert(!ui_shell_is_refreshing(&sh, APP_ID_CLAUDE));
    assert(ui_shell_is_refreshing(&sh, APP_ID_CODEX));
    ui_shell_refresh_begin(&sh, APP_ID_CLAUDE, 2, 33*S);
    ui_shell_refresh_end(&sh, APP_ID_CLAUDE, 1, 34*S); /* stale end */
    assert(ui_shell_is_refreshing(&sh, APP_ID_CLAUDE));
    assert(ui_shell_navigate_saver(&sh, SAVER_ID_WORD_CLOCK, 35*S));
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 1, 36*S);
    ui_shell_fresh_sample(&sh, APP_ID_CODEX, 1, 36*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE); /* current generations suppressed */
    ui_shell_refresh_end(&sh, APP_ID_CLAUDE, 2, 37*S);
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 2, 37*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE); /* late publish also suppressed */
    ui_shell_refresh_begin(&sh, APP_ID_CLAUDE, 3, 38*S);
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 3, 39*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE); /* next cycle eligible */
    ui_shell_refresh_end(&sh, APP_ID_CODEX, 4, 40*S);
    assert(ui_shell_is_refreshing(&sh, APP_ID_CLAUDE));
}

static void manual_face_and_future_app(void)
{
    ui_shell_t sh = shell_at(false);
    register_app(17, &future); /* shell has no hard-coded provider names */
    ui_shell_set_face(&sh, true, S); /* background work cannot cover manual Home */
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE);
    ui_shell_mark_user_refresh(&sh, 2*S);
    ui_shell_set_face(&sh, true, 2*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_FACE);
    ui_shell_set_face(&sh, false, 7*S);
    ui_shell_tick(&sh, 36*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE); /* idle paused for face */
    ui_shell_tick(&sh, 37*S);
    VIEW(sh, UI_ROUTE_SAVER, UI_OVERLAY_NONE);
    ui_shell_fresh_sample(&sh, 17, 1, 36*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE); /* future app joins Home */
    assert(app_registry_count(&apps) == 2);
}

static void home_default_and_status(void)
{
    setup(false);
    ui_shell_t sh;
    assert(ui_shell_init(&sh, &apps, &savers, 1, 30, SAVER_ID_WORD_CLOCK, 0));
    ui_shell_tick(&sh, 30*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE); /* Home is the idle default */
    assert(ui_shell_navigate_saver(&sh, SAVER_ID_WORD_CLOCK, 31*S));
    ui_shell_fresh_sample(&sh, APP_ID_CLAUDE, 1, 32*S);
    VIEW(sh, UI_ROUTE_APP, UI_OVERLAY_NONE);
    ui_shell_tick(&sh, 40*S);
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE);
    ui_shell_show_status(&sh, 41*S);
    ui_shell_tick(&sh, 100*S);
    VIEW(sh, UI_ROUTE_STATUS, UI_OVERLAY_NONE); /* fatal status stays visible */
    assert(ui_shell_navigate_home(&sh, 101*S));
    VIEW(sh, UI_ROUTE_HOME, UI_OVERLAY_NONE);
    assert(!ui_shell_init(&sh, &apps, &savers, 2, 30, SAVER_ID_WORD_CLOCK, 0));
    assert(!ui_shell_init(&sh, &apps, &savers, 0, 9, SAVER_ID_WORD_CLOCK, 0));
    assert(!ui_shell_init(&sh, &apps, &savers, 0, 30, 99, 0));
}

int main(void)
{
    registry_cases();
    idle_and_manual_routes();
    fresh_lease_and_coalescing();
    face_and_overlay_cases();
    generation_and_clock_escape();
    manual_face_and_future_app();
    home_default_and_status();
    return 0;
}
'''


class UIShellTests(unittest.TestCase):
    def test_production_core_with_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="hello-box-shell-") as tmp:
            source = Path(tmp) / "shell_harness.c"
            binary = Path(tmp) / "shell_harness"
            source.write_text(HARNESS)
            command = [
                os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I", str(MAIN), str(source),
                str(MAIN / "app_registry.c"), str(MAIN / "screensaver_registry.c"),
                str(MAIN / "ui_shell.c"), "-o", str(binary),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
