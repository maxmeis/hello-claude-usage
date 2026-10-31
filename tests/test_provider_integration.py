"""Exercise the shared provider HTTP admission with production C code."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.test_factory_reset_flow import c_function


ROOT = Path(__file__).resolve().parents[1]


class ProviderHttpTests(unittest.TestCase):
    def test_automatic_claude_qr_does_not_cover_codex_home(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required")
        source = (ROOT / "main/oauth_probe.c").read_text()
        extracted = c_function(source, "static void show_auto_setup(")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
static bool selected_claude, selected_codex, s_auto_qr_visible;
static int setup_calls;
static char shown_url[32];
static bool claude_selected(void) { return selected_claude; }
static bool codex_selected(void) { return selected_codex; }
static void display_setup(const char *url) {
    ++setup_calls; strcpy(shown_url, url);
}
PRODUCTION
int main(void) {
    selected_claude=true;
    show_auto_setup("http://setup");
    assert(setup_calls==1 && s_auto_qr_visible && !strcmp(shown_url,"http://setup"));
    selected_codex=true;
    show_auto_setup("http://different");
    assert(setup_calls==1 && !s_auto_qr_visible);
    selected_claude=false;
    show_auto_setup("http://another");
    assert(setup_calls==1 && !s_auto_qr_visible);
    return 0;
}
'''.replace("PRODUCTION", extracted)
        with tempfile.TemporaryDirectory(prefix="auto-qr-") as directory:
            file = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            file.write_text(harness)
            build = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", str(file), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_claude_logical_cycle_has_positive_matching_generation(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required")
        source = (ROOT / "main/oauth_probe.c").read_text()
        extracted = "\n".join(c_function(source, signature) for signature in (
            "static bool begin_refresh(", "static void end_refresh(",
        ))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t app_id_t;
enum { APP_ID_CLAUDE=1 };
typedef struct { bool pending, inflight; } refresh_control_t;
static refresh_control_t s_refresh_control;
static uint32_t s_claude_cycle_generation;
static uint32_t begun, ended;
static int starts, ends, updates, s_refresh_mux;
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static bool refresh_control_begin(refresh_control_t *control, bool *manual) {
    if (control->inflight) return false;
    *manual=control->pending; control->pending=false; control->inflight=true;
    return true;
}
static void refresh_control_finish(refresh_control_t *control, bool ready, int64_t retry_at) {
    (void)ready; (void)retry_at; assert(control->inflight); control->inflight=false;
}
static void display_app_refresh_begin(app_id_t id, uint32_t generation) {
    assert(id==APP_ID_CLAUDE && generation && generation!=begun);
    begun=generation; ++starts;
}
static void display_app_refresh_end(app_id_t id, uint32_t generation) {
    assert(id==APP_ID_CLAUDE && generation==begun);
    ended=generation; ++ends;
}
static void claude_updating(void) { ++updates; }
PRODUCTION
int main(void) {
    bool manual=false; uint32_t generation=0;
    assert(!begin_refresh(false,&manual,&generation) && !starts);
    s_refresh_control.pending=true;
    assert(begin_refresh(false,&manual,&generation));
    assert(manual && generation==1 && starts==1 && updates==1);
    assert(!begin_refresh(true,&manual,&generation) && starts==1);
    end_refresh(false,200,generation);
    assert(ended==generation && ends==1);
    assert(begin_refresh(true,&manual,&generation));
    assert(!manual && generation==2 && starts==2);
    end_refresh(true,0,generation);
    s_claude_cycle_generation=UINT32_MAX;
    assert(begin_refresh(true,&manual,&generation));
    assert(generation==1 && starts==3);
    end_refresh(true,0,generation);
    assert(starts==ends);
    return 0;
}
'''.replace("PRODUCTION", extracted)
        with tempfile.TemporaryDirectory(prefix="claude-cycle-") as directory:
            file = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            file.write_text(harness)
            build = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", str(file), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_scoped_refresh_routes_only_selected_configured_apps(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required")
        source = (ROOT / "main/oauth_probe.c").read_text()
        extracted = "\n".join(c_function(source, signature) for signature in (
            "static uint32_t request_app_refresh(",
            "static bool request_manual_refresh(",
        ))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
typedef uint16_t app_id_t;
enum { APP_ID_ALL=0, APP_ID_CLAUDE=1, APP_ID_CODEX=2 };
typedef struct { bool configured; } codex_management_status_t;
typedef struct { bool ready, pending; int64_t retry_at; } refresh_control_t;
static refresh_control_t s_refresh_control={.ready=true};
static atomic_bool s_reset_committed;
static int s_refresh_mux, s_worker_task=1, s_wifi_events=1;
static bool wifi=true, clock_ok=true, select_claude=true, select_codex=true;
static bool codex_configured=true, codex_ready=true;
static int claude_calls, codex_calls, wake_calls;
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static bool wifi_up(void) { return wifi; }
static bool clock_valid(void) { return clock_ok; }
static int64_t now_s(void) { return 100; }
static bool claude_selected(void) { return select_claude; }
static bool codex_selected(void) { return select_codex; }
static void codex_adapter_get_status(codex_management_status_t *out) {
    out->configured=codex_configured;
}
static bool refresh_control_request(refresh_control_t *control, int64_t now) {
    ++claude_calls;
    if (!control->ready || control->pending || now < control->retry_at) return false;
    control->pending=true; return true;
}
static bool codex_adapter_refresh(void) { ++codex_calls; return codex_ready; }
static void xTaskNotifyGive(int task) { assert(task == 1); ++wake_calls; }
PRODUCTION
int main(void) {
    assert(request_app_refresh(0, APP_ID_CLAUDE)==(1u<<APP_ID_CLAUDE));
    assert(claude_calls==1 && codex_calls==0 && wake_calls==1);
    s_refresh_control.pending=false;
    assert(request_app_refresh(0, APP_ID_CODEX)==(1u<<APP_ID_CODEX));
    assert(claude_calls==1 && codex_calls==1 && wake_calls==2);
    assert(request_app_refresh(0, APP_ID_ALL)==
           ((1u<<APP_ID_CLAUDE)|(1u<<APP_ID_CODEX)));
    assert(request_manual_refresh(0));
    assert(claude_calls==3 && codex_calls==3 && wake_calls==4);
    s_refresh_control.pending=false;
    select_claude=false; codex_configured=false;
    assert(!request_app_refresh(0, APP_ID_ALL));
    assert(claude_calls==3 && codex_calls==3 && wake_calls==4);
    select_claude=true; s_refresh_control.retry_at=200;
    assert(!request_app_refresh(0, APP_ID_CLAUDE));
    assert(claude_calls==4 && wake_calls==4);
    select_codex=true; codex_configured=true;
    assert(request_app_refresh(0, APP_ID_ALL)==(1u<<APP_ID_CODEX));
    assert(claude_calls==5 && codex_calls==4 && wake_calls==5);
    s_refresh_control.retry_at=0; codex_ready=false;
    assert(request_app_refresh(0, APP_ID_ALL)==(1u<<APP_ID_CLAUDE));
    assert(claude_calls==6 && codex_calls==5 && wake_calls==6);
    assert(!request_app_refresh(0, 99));
    assert(claude_calls==6 && codex_calls==5);
    s_refresh_control.retry_at=0;
    wifi=false; assert(!request_app_refresh(0, APP_ID_ALL));
    wifi=true; clock_ok=false; assert(!request_app_refresh(0, APP_ID_ALL));
    assert(claude_calls==6 && codex_calls==5);
    return 0;
}
'''.replace("PRODUCTION", extracted)
        with tempfile.TemporaryDirectory(prefix="provider-refresh-") as directory:
            file = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            file.write_text(harness)
            build = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", str(file), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_codex_operation_result_uses_matching_request_id(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required")
        source = (ROOT / "main/oauth_probe.c").read_text()
        extracted = "\n".join(c_function(source, signature) for signature in (
            "static void management_operation(",
            "static void management_poll_codex(",
        ))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    struct { uint32_t id; char method[24], state[16], message[96]; } operation;
} codex_management_status_t;
typedef struct {
    struct { uint32_t id; char method[24], state[16], message[96]; } operation;
} oauth_management_status_t;
static codex_management_status_t codex;
static oauth_management_status_t s_management;
static uint32_t s_management_codex_id;
static int s_management_mux;
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static void codex_adapter_get_status(codex_management_status_t *out) { *out = codex; }
'''
        cases = r'''
int main(void) {
    s_management_codex_id = 42;
    codex.operation.id = 41;
    strcpy(codex.operation.method, "codex.begin");
    strcpy(codex.operation.state, "done");
    management_poll_codex();
    assert(s_management_codex_id == 42 && s_management.operation.id == 0);
    codex.operation.id = 42;
    strcpy(codex.operation.state, "accepted");
    management_poll_codex();
    assert(s_management_codex_id == 42 && s_management.operation.id == 0);
    strcpy(codex.operation.state, "running");
    management_poll_codex();
    assert(s_management_codex_id == 42 && s_management.operation.id == 0);
    strcpy(codex.operation.state, "done");
    strcpy(codex.operation.message, "Open verification page");
    management_poll_codex();
    assert(s_management_codex_id == 0 && s_management.operation.id == 42);
    assert(!strcmp(s_management.operation.method, "codex.begin"));
    assert(!strcmp(s_management.operation.state, "done"));
    assert(!strcmp(s_management.operation.message, "Open verification page"));

    /* A later OAuth failure updates the completed code-issue operation when
       it remains the latest operation for this device. */
    strcpy(codex.operation.state, "error");
    strcpy(codex.operation.message, "Token exchange failed");
    management_poll_codex();
    assert(s_management.operation.id == 42);
    assert(!strcmp(s_management.operation.state, "failed"));
    assert(!strcmp(s_management.operation.message, "Token exchange failed"));

    /* A late result from the old authorization must not overwrite newer work. */
    management_operation(99, "codex.refresh", "done", "Usage refreshed");
    management_poll_codex();
    assert(s_management.operation.id == 99);
    assert(!strcmp(s_management.operation.method, "codex.refresh"));
    assert(!strcmp(s_management.operation.state, "done"));
    assert(!strcmp(s_management.operation.message, "Usage refreshed"));

    s_management_codex_id = 43;
    codex.operation.id = 43;
    strcpy(codex.operation.state, "error");
    management_poll_codex();
    assert(!strcmp(s_management.operation.state, "failed"));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="codex-operation-") as directory:
            file = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            file.write_text(harness + extracted + cases)
            build = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", str(file), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_mutex_serializes_requests_and_requires_initialization(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required")
        with tempfile.TemporaryDirectory(prefix="provider-http-") as directory:
            root = Path(directory)
            (root / "freertos").mkdir()
            (root / "freertos" / "FreeRTOS.h").write_text(
                "#pragma once\n#include <stdint.h>\n"
                "typedef uint32_t TickType_t;\n#define pdTRUE 1\n"
            )
            (root / "freertos" / "semphr.h").write_text(
                "#pragma once\n#include <pthread.h>\n"
                "typedef pthread_mutex_t *SemaphoreHandle_t;\n"
                "SemaphoreHandle_t xSemaphoreCreateMutex(void);\n"
                "int xSemaphoreTake(SemaphoreHandle_t, unsigned);\n"
                "int xSemaphoreGive(SemaphoreHandle_t);\n"
            )
            (root / "test.c").write_text(r'''
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include "provider_http.h"
#include "freertos/semphr.h"

static int active, maximum;
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t lock = malloc(sizeof(*lock));
    assert(lock && pthread_mutex_init(lock, NULL) == 0);
    return lock;
}
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout) {
    (void)timeout;
    return pthread_mutex_lock(lock) == 0;
}
int xSemaphoreGive(SemaphoreHandle_t lock) {
    return pthread_mutex_unlock(lock) == 0;
}
static void *request(void *unused) {
    (void)unused;
    for (int i = 0; i < 20; ++i) {
        assert(provider_http_take(100));
        if (++active > maximum) maximum = active;
        usleep(100);
        --active;
        provider_http_give();
    }
    return NULL;
}
int main(void) {
    assert(!provider_http_take(0));
    assert(provider_http_init());
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, request, NULL));
    assert(!pthread_create(&b, NULL, request, NULL));
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(maximum == 1 && active == 0);
    return 0;
}
''')
            binary = root / "provider-http-test"
            build = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-pthread", "-I", str(root),
                 "-I", str(ROOT / "main"), str(ROOT / "main/provider_http.c"),
                 str(root / "test.c"), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
