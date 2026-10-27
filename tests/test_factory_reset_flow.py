"""Host execution tests for the production Settings factory-reset flow.

The C harness compiles functions extracted from oauth_probe.c. Hardware, HTTP,
FreeRTOS, and storage calls are replaced with deterministic stubs.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "oauth_probe.c"


def c_function(source: str, signature: str) -> str:
    """Extract one complete C function, including nested blocks and strings."""
    search = 0
    while True:
        start = source.index(signature, search)
        opening = source.index("{", start + len(signature))
        if ";" not in source[start:opening]:
            break
        search = start + len(signature)
    depth = 0
    quote = None
    escaped = False
    line_comment = False
    block_comment = False
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and next_char == "/":
                block_comment = False
                index += 1
        elif quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char == "/" and next_char == "/":
            line_comment = True
            index += 1
        elif char == "/" and next_char == "*":
            block_comment = True
            index += 1
        elif char in ('"', "'"):
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
        index += 1
    raise AssertionError(f"Unterminated function: {signature}")


def run_c_harness(test: unittest.TestCase, program: str) -> None:
    compiler = shutil.which("clang")
    if not compiler:
        test.skipTest("clang is required for ASan/UBSan host tests")
    with tempfile.TemporaryDirectory(prefix="factory-reset-flow-") as temp:
        source = Path(temp) / "flow.c"
        binary = Path(temp) / "flow"
        source.write_text(program, encoding="utf-8")
        build = subprocess.run(
            [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             str(source), "-o", str(binary)],
            capture_output=True, text=True, check=False,
        )
        test.assertEqual(build.returncode, 0, build.stderr)
        run = subprocess.run([str(binary)], capture_output=True, text=True, check=False)
        test.assertEqual(run.returncode, 0, run.stdout + run.stderr)


SCAFFOLD = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int QueueHandle_t;
typedef int SemaphoreHandle_t;
typedef int portMUX_TYPE;
typedef void *httpd_handle_t;
typedef struct { const char *uri, *action, *confirmation; int content_len;
                 const char *status; char reply[128]; } httpd_req_t;
typedef struct { uint32_t request_id; bool open; } settings_command_t;
typedef struct { uint32_t session, request_id; char cap[48]; int64_t deadline_us; } reset_command_t;
typedef struct { char code[513]; uint32_t session; } submission_t;
typedef struct { int identity; char secret[32]; } token_store_t;
static char s_setup_url[128];
static bool s_auto_qr_visible;
#define ESP_OK 0
#define ESP_FAIL -1
#define pdTRUE 1
#define portMAX_DELAY -1
#define RESET_CONFIRM_LIFETIME_US (60LL * 1000000LL)
#define SETUP_PAGE_CAPACITY 8192
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
static QueueHandle_t s_reset_commands = 1, s_forget_commands = 2,
                     s_submissions = 3, s_settings_commands = 4;
static SemaphoreHandle_t s_cap_lock;
static portMUX_TYPE s_settings_mux;
static int s_worker_task = 1;
static httpd_handle_t s_server = (void *)1;
static char s_cap[48] = "page-cap", s_auth_url[512] = "secret-url";
static char s_reset_nonce[48];
static uint32_t s_session = 7, s_manual_request_id = 42, s_reset_nonce_session;
static uint32_t s_settings_wanted_id = 42;
static int64_t s_cap_deadline = 2000, s_reset_nonce_deadline_us;
static bool s_cap_valid = true, s_manual_session = true,
            s_settings_wanted_open = true, s_settings_committing, s_token_dirty = true;
static atomic_bool s_reset_response_ready, s_reset_committed, s_forget_response_ready,
                   s_reset_inflight;
static const char *s_page_state = "Ready";
static int64_t epoch = 1000, ticks = 1000000;
static int queue_sends, queue_resets, journal_calls, stop_calls, restart_calls,
           display_close_calls, display_status_calls, refresh_calls, notify_calls;
static bool reset_queued, queue_fail, send_fail, journal_fail, close_during_commit;
static reset_command_t queued_reset;
static char events[64];
static jmp_buf restarted;
static int64_t now_s(void) { return epoch; }
static int64_t esp_timer_get_time(void) { return ticks; }
static bool encode_random(char *out, size_t capacity, size_t bytes) {
    assert(bytes == 24 && capacity >= 33);
    static int generation;
    memset(out, 'A' + (generation++ % 2), 32);
    out[32] = 0;
    return true;
}
static int httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name) {
    const char *value = !strcmp(name, "X-Setup-Action") ? req->action : req->confirmation;
    return value ? (int)strlen(value) : 0;
}
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name,
                                              char *out, size_t size) {
    const char *value = !strcmp(name, "X-Setup-Action") ? req->action : req->confirmation;
    if (!value || strlen(value) >= size) return ESP_FAIL;
    strcpy(out, value);
    return ESP_OK;
}
static void httpd_resp_set_status(httpd_req_t *req, const char *status) { req->status = status; }
static void httpd_resp_set_type(httpd_req_t *req, const char *type) { (void)req; (void)type; }
static void httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value) {
    (void)req; (void)name; (void)value;
}
static esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *reply) {
    assert(!atomic_load(&s_reset_committed));
    snprintf(req->reply, sizeof(req->reply), "%s", reply);
    if (send_fail) return ESP_FAIL;
    strcat(events, "H");
    return ESP_OK;
}
static const char *device_identity_name(void) { return "hello-box-4827"; }
static esp_err_t httpd_resp_send(httpd_req_t *req, const char *body, int length) {
    (void)req;
    assert(!atomic_load(&s_reset_committed));
    assert(length > 0 && strstr(body, "id='prepare-reset'") &&
           strstr(body, "id='confirm-reset'") && strstr(body, "id='cancel-reset'"));
    return ESP_OK;
}
static esp_err_t httpd_resp_send_404(httpd_req_t *req) { req->status = "404"; return ESP_FAIL; }
static esp_err_t httpd_resp_send_500(httpd_req_t *req) { req->status = "500"; return ESP_FAIL; }
static int httpd_req_recv(httpd_req_t *req, char *out, int size) {
    (void)req; (void)out; (void)size; return -1;
}
static int xQueueSend(QueueHandle_t queue, const void *item, int wait) {
    assert(wait == 0);
    ++queue_sends;
    if (queue_fail) return 0;
    if (queue == s_reset_commands) {
        assert(!reset_queued);
        queued_reset = *(const reset_command_t *)item;
        reset_queued = true;
    }
    return pdTRUE;
}
static int xQueueReceive(QueueHandle_t queue, void *item, int wait) {
    assert(queue == s_reset_commands && wait == 0);
    if (!reset_queued) return 0;
    *(reset_command_t *)item = queued_reset;
    reset_queued = false;
    return pdTRUE;
}
static void xQueueReset(QueueHandle_t queue) {
    ++queue_resets;
    if (queue == s_reset_commands) reset_queued = false;
    else assert(queue == s_submissions);
}
static int xQueueOverwrite(QueueHandle_t queue, const settings_command_t *item) {
    assert(queue == s_settings_commands && item->request_id); return pdTRUE;
}
static void xTaskNotifyGive(int task) { assert(task == 1); ++notify_calls; }
static void xSemaphoreTake(int lock, int delay) { assert(lock == 0 && delay == portMAX_DELAY); }
static void xSemaphoreGive(int lock) { assert(lock == 0); }
static void httpd_stop(httpd_handle_t server) {
    assert(server == (void *)1); ++stop_calls; strcat(events, "S");
}
static void device_discovery_http_stop(void) {
    assert(s_server == (void *)1);
}
static void display_settings_closed(uint32_t id) { assert(id == 42); ++display_close_calls; }
static void display_status(const char *title, const char *message) {
    (void)title; (void)message; ++display_status_calls;
}
static void publish_refresh(bool ready, int64_t retry_at) {
    assert(!ready && retry_at == 0); ++refresh_calls;
}
static esp_err_t factory_reset_request(void);
static void set_page_state(const char *state);
static void stop_setup(void);
static void esp_restart(void) {
    assert(atomic_load(&s_reset_committed));
    ++restart_calls; strcat(events, "R"); longjmp(restarted, 1);
}
static void vTaskDelay(int delay) { (void)delay; assert(false); }
'''


CASES = r'''
static esp_err_t factory_reset_request(void) {
    ++journal_calls;
    strcat(events, "J");
    if (close_during_commit) assert(!request_settings(NULL, 42, false));
    return journal_fail ? ESP_FAIL : ESP_OK;
}
static httpd_req_t request(const char *action, const char *confirmation) {
    return (httpd_req_t){.uri = "/s/page-cap", .action = action,
                         .confirmation = confirmation};
}
static void assert_no_reset(void) { assert(journal_calls == 0 && stop_calls == 0 && restart_calls == 0); }
static void fresh(void) {
    s_cap_valid = true; strcpy(s_cap, "page-cap"); s_session = 7;
    s_manual_session = true; s_manual_request_id = 42;
    s_settings_wanted_id = 42; s_settings_wanted_open = true;
    s_settings_committing = false; s_server = (void *)1;
    s_cap_deadline = 2000; epoch = 1000; ticks = 1000000;
    s_reset_nonce[0] = 0; s_reset_nonce_session = 0; s_reset_nonce_deadline_us = 0;
    atomic_store(&s_reset_response_ready, false); atomic_store(&s_reset_committed, false);
    atomic_store(&s_reset_inflight, false);
    reset_queued = queue_fail = send_fail = journal_fail = close_during_commit = false;
    queue_sends = queue_resets = journal_calls = stop_calls = restart_calls = 0;
    display_close_calls = display_status_calls = refresh_calls = notify_calls = 0;
    s_page_state = "Ready"; events[0] = 0;
}
static void process(token_store_t *token) {
    char state[48] = "oauth-state", verifier[80] = "oauth-verifier";
    process_factory_reset(token, state, verifier);
    assert(state[0] && verifier[0]);
}
static void prepare(char nonce[48]) {
    httpd_req_t req = request("prepare-reset", NULL);
    assert(setup_post(&req) == ESP_OK);
    assert(strlen(req.reply) == 32);
    strcpy(nonce, req.reply);
    assert_no_reset();
}
int main(void) {
    token_store_t token = {.identity = 17, .secret = "saved-token"};
    char nonce[48];
    fresh();
    httpd_req_t get = request(NULL, NULL);
    assert(cap_matches(get.uri, NULL));
    assert(setup_get(&get) == ESP_OK);
    assert_no_reset();
    httpd_req_t bad_path = request("prepare-reset", NULL);
    bad_path.uri = "/s/wrong-cap";
    assert(setup_post(&bad_path) == ESP_FAIL && !strcmp(bad_path.status, "404"));
    assert_no_reset();
    s_manual_session = false;
    httpd_req_t not_settings = request("prepare-reset", NULL);
    assert(setup_post(&not_settings) == ESP_OK && !strcmp(not_settings.status, "409 Conflict"));
    assert_no_reset();
    s_manual_session = true;
    prepare(nonce);
    process(&token);
    assert_no_reset();
    char first_nonce[48]; strcpy(first_nonce, nonce);
    prepare(nonce);
    assert(strcmp(nonce, first_nonce) != 0);
    httpd_req_t replaced = request("factory-reset", first_nonce);
    assert(setup_post(&replaced) == ESP_OK && !strcmp(replaced.status, "409 Conflict"));
    httpd_req_t missing = request("factory-reset", NULL);
    assert(setup_post(&missing) == ESP_OK && !strcmp(missing.status, "400 Bad Request"));
    httpd_req_t wrong = request("factory-reset", "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    assert(setup_post(&wrong) == ESP_OK && !strcmp(wrong.status, "409 Conflict"));
    assert_no_reset();
    httpd_req_t cancel = request("cancel-reset", NULL);
    assert(setup_post(&cancel) == ESP_OK && !s_reset_nonce[0]);
    httpd_req_t after_cancel = request("factory-reset", nonce);
    assert(setup_post(&after_cancel) == ESP_OK && !strcmp(after_cancel.status, "409 Conflict"));
    assert_no_reset();

    fresh(); prepare(nonce);
    ticks += RESET_CONFIRM_LIFETIME_US;
    httpd_req_t expired = request("factory-reset", nonce);
    assert(setup_post(&expired) == ESP_OK && !strcmp(expired.status, "409 Conflict"));
    assert_no_reset();

    fresh(); prepare(nonce);
    s_session = 8;
    httpd_req_t stale = request("factory-reset", nonce);
    assert(setup_post(&stale) == ESP_OK && !strcmp(stale.status, "409 Conflict"));
    assert_no_reset();

    fresh(); prepare(nonce);
    httpd_req_t confirmed = request("factory-reset", nonce);
    queue_fail = true;
    assert(setup_post(&confirmed) == ESP_OK && !atomic_load(&s_reset_response_ready));
    assert(!reset_queued && !atomic_load(&s_reset_inflight));
    assert_no_reset();

    fresh(); prepare(nonce);
    confirmed = request("factory-reset", nonce);
    send_fail = true;
    assert(setup_post(&confirmed) == ESP_FAIL && !atomic_load(&s_reset_response_ready));
    assert(!reset_queued && !atomic_load(&s_reset_inflight));
    process(&token); assert_no_reset();

    fresh(); prepare(nonce);
    confirmed = request("factory-reset", nonce);
    assert(setup_post(&confirmed) == ESP_OK && atomic_load(&s_reset_response_ready));
    assert(reset_queued && journal_calls == 0);
    httpd_req_t replay = request("factory-reset", nonce);
    assert(setup_post(&replay) == ESP_OK && !strcmp(replay.status, "409 Conflict"));
    assert(request_settings(NULL, 42, false));
    process(&token); assert_no_reset();
    assert(token.identity == 17 && s_token_dirty);

    fresh(); prepare(nonce);
    confirmed = request("factory-reset", nonce);
    assert(setup_post(&confirmed) == ESP_OK);
    ++s_session;
    process(&token); assert_no_reset();

    fresh(); prepare(nonce);
    confirmed = request("factory-reset", nonce);
    assert(setup_post(&confirmed) == ESP_OK);
    strcpy(s_cap, "replacement-cap");
    process(&token); assert_no_reset();

    fresh(); prepare(nonce);
    confirmed = request("factory-reset", nonce);
    assert(setup_post(&confirmed) == ESP_OK);
    ticks += RESET_CONFIRM_LIFETIME_US;
    process(&token); assert_no_reset();

    fresh(); prepare(nonce);
    confirmed = request("factory-reset", nonce);
    assert(setup_post(&confirmed) == ESP_OK);
    epoch = s_cap_deadline; /* The page can expire while its command is queued. */
    process(&token); assert_no_reset();

    fresh(); prepare(nonce);
    confirmed = request("factory-reset", nonce);
    assert(setup_post(&confirmed) == ESP_OK);
    journal_fail = true;
    process(&token);
    assert(journal_calls == 1 && stop_calls == 0 && restart_calls == 0);
    assert(token.identity == 17 && !strcmp(token.secret, "saved-token") && s_token_dirty);
    assert(s_cap_valid && s_server == (void *)1 && s_settings_wanted_open);
    assert(!s_settings_committing && !strcmp(s_page_state, "Reset failed; settings were not changed. You can try again."));

    fresh(); prepare(nonce);
    confirmed = request("factory-reset", nonce);
    assert(setup_post(&confirmed) == ESP_OK);
    close_during_commit = true;
    char state[48] = "oauth-state", verifier[80] = "oauth-verifier";
    if (!setjmp(restarted)) process_factory_reset(&token, state, verifier);
    assert(journal_calls == 1 && stop_calls == 1 && restart_calls == 1);
    assert(!strcmp(events, "HHJSR") && "HTTP response must precede journal, server stop, and restart");
    assert(token.identity == 0 && token.secret[0] == 0 && !s_token_dirty);
    assert(state[0] == 0 && verifier[0] == 0);
    assert(!s_server && !s_cap_valid && refresh_calls == 1);
    puts("production reset endpoint and worker flow passed");
    return 0;
}
'''


class FactoryResetFlowTests(unittest.TestCase):
    def test_production_endpoint_and_worker_with_sanitizers(self):
        source = SOURCE.read_text(encoding="utf-8")
        signatures = (
            "static bool settings_wanted(",
            "static bool reserve_settings_commit(",
            "static void finish_settings_commit(",
            "static bool request_settings(",
            "static bool cap_matches(",
            "static esp_err_t setup_get(",
            "static esp_err_t setup_post(",
            "static void stop_setup(",
            "static void set_page_state(",
            "static void process_factory_reset(",
        )
        production = "\n\n".join(c_function(source, name) for name in signatures)
        run_c_harness(self, SCAFFOLD + production + CASES)


if __name__ == "__main__":
    unittest.main()
