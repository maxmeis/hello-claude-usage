"""Run production Settings queue/state functions with host service stubs."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "main/oauth_probe.c"


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    following = re.search(r"\n(?:static )?(?:bool|void|esp_err_t) [A-Za-z_]+\(",
                          source[start + len(signature):])
    if following is None:
        raise AssertionError(f"No following function after {signature}")
    return source[start:start + len(signature) + following.start()]


class SettingsFlowTests(unittest.TestCase):
    def test_real_queue_handlers_cancel_and_stop_http_before_wifi_forget(self):
        source = SOURCE.read_text()
        functions = "\n".join(function(source, signature) for signature in (
            "static bool settings_wanted(", "static bool reserve_settings_commit(",
            "static void finish_settings_commit(", "static bool request_settings(",
            "static void stop_setup(", "static void process_settings(",
            "static void process_forget("))
        commit_start = source.index("bool saved = save_token(candidate);")
        commit_end = source.index("finish_settings_commit();", commit_start) + len(
            "finish_settings_commit();")
        production_commit = source[commit_start:commit_end]
        scaffold = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint32_t request_id; bool open; } settings_command_t;
#define pdTRUE 1
#define portMAX_DELAY -1
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGW(...) ((void)0)
static int s_settings_mux, s_cap_lock, s_worker_task = 1;
static int s_settings_commands = 1, s_forget_commands = 2, s_submissions = 3;
static void *s_server;
static int notified, stop_calls, wifi_forget_calls, display_close_calls;
static bool s_manual_session, s_cap_valid, s_settings_wanted_open, s_settings_committing;
static uint32_t s_manual_request_id, s_settings_wanted_id, s_session;
static char s_cap[48], s_auth_url[512];
static char s_setup_url[128];
static bool s_auto_qr_visible;
static char s_reset_nonce[48];
static uint32_t s_reset_nonce_session;
static int64_t s_reset_nonce_deadline_us;
static atomic_bool s_forget_response_ready;
/* USB OAuth status is a separate session and blocks the phone Settings QR. */
static atomic_bool s_usb_session_active, s_usage_validated;
static settings_command_t queued_settings;
static bool settings_queued, forget_queued, wifi_connected = true, clock_ready = true;
static uint32_t queued_forget, displayed_closed_id;
static bool provisioning;
static char event_log[32];
static const char *s_page_state;
typedef struct { int identity; } token_store_t;
static token_store_t current_token = {.identity = 1}, incoming_token = {.identity = 2};
static token_store_t *token = &current_token, *candidate = &incoming_token;
static bool s_token_dirty, first_usage, allow_save = true;
static int64_t next_usage = 99, usage_retry_until = 429, next_refresh = 88;
static int failures = 3;
static int reset_usage_calls;
static bool reset_usage_configured;
static char reset_usage_status[72];
static void reset_claude_usage(bool configured, const char *status) {
    ++reset_usage_calls; reset_usage_configured = configured;
    snprintf(reset_usage_status, sizeof(reset_usage_status), "%s", status ? status : "");
}
static bool save_token(token_store_t *value) {
    assert(value == &incoming_token); return allow_save;
}
static int xQueueOverwrite(int queue, const settings_command_t *command) {
    assert(queue == 1); queued_settings = *command; settings_queued = true; return pdTRUE;
}
static int xQueueReceive(int queue, void *out, int timeout) {
    assert(timeout == 0);
    if (queue == 1 && settings_queued) {
        *(settings_command_t *)out = queued_settings; settings_queued = false; return pdTRUE;
    }
    if (queue == 2 && forget_queued) {
        *(uint32_t *)out = queued_forget; forget_queued = false; return pdTRUE;
    }
    return 0;
}
static void xQueueReset(int queue) { assert(queue == 3); }
static void xTaskNotifyGive(int task) { assert(task == 1); ++notified; }
static void xSemaphoreTake(int lock, int timeout) { assert(lock == 0 && timeout == portMAX_DELAY); }
static void xSemaphoreGive(int lock) { assert(lock == 0); }
static void httpd_stop(void *server) {
    assert(server == (void *)1); ++stop_calls; strcat(event_log, "S");
}
static void device_discovery_http_stop(void) {
    assert(s_server == (void *)1);
}
static void display_settings_closed(uint32_t id) {
    ++display_close_calls; displayed_closed_id = id;
}
static bool wifi_manager_request_forget(void) {
    ++wifi_forget_calls; strcat(event_log, "F"); return true;
}
static bool wifi_manager_is_provisioning(void) { return provisioning; }
static bool wifi_up(void) { return wifi_connected; }
static bool clock_valid(void) { return clock_ready; }
static bool start_setup(char *state, char *verifier, bool manual, uint32_t id) {
    (void)state; (void)verifier; assert(manual);
    s_manual_session = true; s_manual_request_id = id;
    s_server = (void *)1; s_cap_valid = true; s_session = 7;
    return true;
}
'''
        commit_wrapper = "\nstatic void commit_candidate(void) {\n" + production_commit + "\n}\n"
        cases = r'''
int main(void) {
    char state[48] = {0}, verifier[80] = {0};
    assert(request_settings(NULL, 1, true));
    assert(request_settings(NULL, 1, false));
    process_settings(true, state, verifier);
    assert(!s_manual_session && !s_server && !display_close_calls);
    assert(!settings_wanted(1));

    assert(request_settings(NULL, 10, true));
    assert(request_settings(NULL, 10, false));
    assert(!reserve_settings_commit(10) && current_token.identity == 1);
    assert(request_settings(NULL, 11, true));
    assert(reserve_settings_commit(11));
    assert(!request_settings(NULL, 11, false));
    assert(!request_settings(NULL, 12, true));
    assert(settings_wanted(11) && !settings_wanted(12));
    commit_candidate();
    assert(!s_settings_committing && current_token.identity == 2);
    assert(usage_retry_until == 0 && next_usage == 0 && next_refresh == 0);
    assert(first_usage && failures == 0 && !s_token_dirty);
    assert(reset_usage_calls == 1 && reset_usage_configured &&
           !strcmp(reset_usage_status, "Checking new account"));
    assert(request_settings(NULL, 11, false));
    process_settings(true, state, verifier);

    current_token.identity = 1;
    incoming_token.identity = 3;
    usage_retry_until = 429; next_usage = 99; next_refresh = 88;
    first_usage = false; failures = 3; allow_save = false;
    assert(request_settings(NULL, 13, true));
    assert(reserve_settings_commit(13));
    commit_candidate();
    assert(current_token.identity == 1 && usage_retry_until == 429);
    assert(next_usage == 99 && next_refresh == 88 && !first_usage && failures == 3);
    assert(reset_usage_calls == 1); /* Failed save leaves old-account usage intact. */
    assert(request_settings(NULL, 13, false));
    process_settings(true, state, verifier);
    allow_save = true;

    assert(request_settings(NULL, 2, true));
    process_settings(true, state, verifier);
    assert(s_manual_session && s_manual_request_id == 2 && s_server == (void *)1);
    assert(request_settings(NULL, 2, false));
    process_settings(true, state, verifier);
    assert(!s_manual_session && !s_server && stop_calls == 1);
    assert(display_close_calls == 1 && displayed_closed_id == 2);

    provisioning = true;
    assert(request_settings(NULL, 3, true));
    process_settings(true, state, verifier);
    assert(!s_server && displayed_closed_id == 3);
    provisioning = false;

    assert(request_settings(NULL, 4, true));
    process_settings(true, state, verifier);
    assert(s_server == (void *)1 && s_session == 7);
    strcpy(s_reset_nonce, "pending-confirmation");
    s_reset_nonce_session = s_session;
    s_reset_nonce_deadline_us = 100;
    queued_forget = 6; forget_queued = true;
    atomic_store(&s_forget_response_ready, true);
    process_forget(state, verifier);
    assert(s_server == (void *)1 && wifi_forget_calls == 0);

    queued_forget = 7; forget_queued = true;
    atomic_store(&s_forget_response_ready, true);
    strcpy(state, "demo-state"); strcpy(verifier, "demo-verifier");
    process_forget(state, verifier);
    assert(!s_server && !s_cap_valid && !s_manual_session);
    assert(!s_reset_nonce[0] && !s_reset_nonce_session && !s_reset_nonce_deadline_us);
    assert(state[0] == 0 && verifier[0] == 0);
    assert(wifi_forget_calls == 1 && stop_calls == 2);
    assert(!strcmp(event_log, "SSF") && "HTTP server must stop before Wi-Fi reset");
    assert(displayed_closed_id == 4);
    puts("Settings queue cancellation and server-before-forget passed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="settings-flow-") as directory:
            c_file = Path(directory) / "settings.c"
            executable = Path(directory) / "settings"
            c_file.write_text(scaffold + functions + commit_wrapper + cases)
            subprocess.run(["clang", "-std=gnu11", "-fsanitize=address,undefined",
                            str(c_file), "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True)
            self.assertIn("server-before-forget passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
