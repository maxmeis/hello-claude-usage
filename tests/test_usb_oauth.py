"""Exercise the production OAuth USB command admission with host stubs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.test_factory_reset_flow import c_function


SOURCE = Path(__file__).resolve().parents[1] / "main/oauth_probe.c"


class UsbOauthTests(unittest.TestCase):
    def test_command_admission_dedup_and_queue_race(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required for production C harness")
        source = SOURCE.read_text()
        extracted = "\n".join(c_function(source, signature) for signature in (
            "static void management_operation(",
            "static void management_network_invalidate(",
            "void oauth_probe_get_management_status(",
            "static void management_poll_wifi(",
            "bool oauth_probe_management_request(",
        ))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint32_t id; char method[24], arg1[64], arg2[513]; } management_command_t;
typedef struct { bool valid; float used_percent; int64_t resets_at; uint32_t window_seconds; } quota_window_t;
typedef struct {
    bool configured, has_sample, updating;
    int64_t sampled_at, sampled_us;
    quota_window_t windows[2];
    bool reset_credits_known; uint32_t reset_credits;
    char status[72];
} provider_usage_t;
typedef struct {
    bool configured; char state[16], session[48], authorization_url[512];
    struct { char state[16]; bool wifi, ip, clock, dns, https; char message[96]; } network;
    struct { uint32_t id; char method[24], state[16], message[96]; } operation;
    provider_usage_t usage;
} oauth_management_status_t;
static oauth_management_status_t s_management;
static provider_usage_t s_claude_usage;
static int s_management_mux, s_management_commands = 1, s_worker_task = 1;
static atomic_bool s_reset_committed;
static atomic_bool s_token_configured, s_usage_validated, s_usb_session_active;
static int s_wifi_events = 1, s_sta = 1;
static bool wifi_connected = true, provisioning, time_ready = true;
static bool reconnected;
#define OAUTH_WIFI_RECONNECTED 2
typedef struct { struct { uint32_t addr; } ip; } esp_netif_ip_info_t;
#define ESP_OK 0
static management_command_t queued;
static int queue_calls, notified, queue_result = 1;
static bool complete_inside_queue;
typedef struct { uint32_t operation_id; bool provisioning; char state[16], message[96]; } wifi_manager_status_t;
static wifi_manager_status_t wifi_status;
static uint32_t s_management_wifi_id;
#define pdTRUE 1
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static bool wifi_store_valid_credentials(const char *ssid, const char *password) {
    return ssid && *ssid && password && strlen(password) <= 64;
}
static bool wifi_up(void) { return wifi_connected; }
static int xEventGroupGetBits(int events) { assert(events == 1); return reconnected ? 2 : 0; }
static bool wifi_manager_is_provisioning(void) { return provisioning; }
static bool clock_valid(void) { return time_ready; }
static int esp_netif_get_ip_info(int sta, esp_netif_ip_info_t *ip) {
    assert(sta == 1); ip->ip.addr = 1; return ESP_OK;
}
static int xQueueSend(int queue, const management_command_t *cmd, int wait) {
    assert(queue == 1 && wait == 0);
    ++queue_calls;
    queued = *cmd;
    if (complete_inside_queue) strcpy(s_management.operation.state, "done");
    return queue_result;
}
static void xTaskNotifyGive(int task) { assert(task == 1); ++notified; }
static void wifi_manager_get_status(wifi_manager_status_t *out) { *out = wifi_status; }
'''
        cases = r'''
int main(void) {
    oauth_management_status_t validation_snapshot;
    atomic_store(&s_token_configured, true);
    atomic_store(&s_usage_validated, false);
    oauth_probe_get_management_status(&validation_snapshot);
    assert(validation_snapshot.configured && !strcmp(validation_snapshot.state, "checking"));
    atomic_store(&s_usage_validated, true);
    oauth_probe_get_management_status(&validation_snapshot);
    assert(validation_snapshot.configured && !strcmp(validation_snapshot.state, "validated"));
    atomic_store(&s_token_configured, false);
    oauth_probe_get_management_status(&validation_snapshot);
    assert(!validation_snapshot.configured && !strcmp(validation_snapshot.state, "not_configured"));

    assert(oauth_probe_management_request(1, "wifi.connect", "open-net", ""));
    assert(queue_calls == 1 && !strcmp(queued.arg1, "open-net"));
    assert(!strcmp(s_management.operation.state, "queued"));
    assert(oauth_probe_management_request(1, "wifi.connect", "open-net", ""));
    assert(queue_calls == 1); /* lost ACK never replays credentials */
    assert(!oauth_probe_management_request(1, "network.test", NULL, NULL));
    assert(!oauth_probe_management_request(2, "claude.begin", NULL, NULL));
    assert(queue_calls == 1); /* busy */
    strcpy(s_management.operation.state, "done");
    complete_inside_queue = true;
    assert(oauth_probe_management_request(2, "claude.begin", NULL, NULL));
    assert(!strcmp(s_management.operation.state, "done")); /* no queued overwrite */
    assert(!oauth_probe_management_request(3, "wifi.connect", "", ""));
    assert(!oauth_probe_management_request(3, "unrecognized", NULL, NULL));
    queue_result = 0;
    assert(!oauth_probe_management_request(3, "network.test", NULL, NULL));
    assert(!strcmp(s_management.operation.state, "failed"));
    assert(notified == 2);
    queue_result = 1;
    complete_inside_queue = false;
    strcpy(s_management.operation.state, "done");
    assert(oauth_probe_management_request(6, "codex.begin", NULL, NULL));
    assert(queue_calls == 4 && !strcmp(queued.method, "codex.begin"));
    s_management_wifi_id = 4;
    management_operation(4, "wifi.connect", "running", "Testing");
    wifi_status.operation_id = 3;
    strcpy(wifi_status.state, "connected");
    management_poll_wifi();
    assert(s_management_wifi_id == 4); /* old network is not candidate success */
    wifi_status.operation_id = 4;
    strcpy(wifi_status.state, "queued");
    management_poll_wifi();
    assert(s_management_wifi_id == 4 && !strcmp(s_management.operation.state, "running"));
    strcpy(wifi_status.state, "testing");
    management_poll_wifi();
    assert(s_management_wifi_id == 4);
    strcpy(wifi_status.state, "failed");
    strcpy(wifi_status.message, "Router rejected connection");
    management_poll_wifi();
    assert(!s_management_wifi_id && !strcmp(s_management.operation.state, "failed"));
    s_management_wifi_id = 5;
    wifi_status.operation_id = 5;
    strcpy(wifi_status.state, "saving");
    management_poll_wifi();
    assert(s_management_wifi_id == 5);
    strcpy(wifi_status.state, "connected");
    strcpy(wifi_status.message, "Connected and saved");
    wifi_status.provisioning = true;
    management_poll_wifi();
    assert(s_management_wifi_id == 5);
    wifi_status.provisioning = false;
    management_poll_wifi();
    assert(!s_management_wifi_id && !strcmp(s_management.operation.state, "done"));
    s_management.network.dns = true;
    s_management.network.https = true;
    strcpy(s_management.network.state, "online");
    oauth_management_status_t snapshot;
    oauth_probe_get_management_status(&snapshot);
    assert(snapshot.network.dns && snapshot.network.https);
    management_network_invalidate("Wi-Fi connection is changing");
    oauth_probe_get_management_status(&snapshot);
    assert(!snapshot.network.dns && !snapshot.network.https);
    assert(!strcmp(snapshot.network.state, "untested"));
    s_management.network.dns = true;
    s_management.network.https = true;
    strcpy(s_management.network.state, "online");
    reconnected = true;
    oauth_probe_get_management_status(&snapshot);
    assert(!snapshot.network.dns && !snapshot.network.https);
    assert(!strcmp(snapshot.network.state, "untested"));
    reconnected = false;
    wifi_connected = false;
    oauth_probe_get_management_status(&snapshot);
    assert(!snapshot.network.dns && !snapshot.network.https);
    assert(!strcmp(snapshot.network.state, "offline"));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="usb-oauth-") as directory:
            path = Path(directory) / "usb.c"
            binary = Path(directory) / "usb"
            path.write_text(harness + extracted + cases)
            build = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", str(path), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
