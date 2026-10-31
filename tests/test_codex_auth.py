"""Exercise the production post-OAuth credential commit under sanitizers."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def production_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start:offset + 1]
    raise AssertionError(f"Unclosed function {signature}")


class CodexAuthCommitTests(unittest.TestCase):
    def test_authorized_tokens_are_committed_before_usage_and_failures_preserve_old_account(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required for Codex auth commit sanitizer test")

        source = (ROOT / "main/codex_adapter.c").read_text()
        functions = "\n".join(production_function(source, signature) for signature in (
            "static const char *account_state(void)\n{",
            "static bool current_generation(uint32_t generation)",
            "static bool commit_authorized_account(const codex_tokens_t *candidate)",
        ))
        # The authorization commit seam must not call the quota service. The
        # worker retries usage through poll_usage after the token is saved.
        commit = production_function(source, "static bool commit_authorized_account(")
        poll_auth = production_function(source, "static void poll_auth(void)")
        self.assertNotIn("fetch_usage(", commit)
        self.assertLess(poll_auth.index("codex_parse_tokens("),
                        poll_auth.index("commit_authorized_account(candidate)"))
        self.assertNotIn("fetch_usage(candidate", poll_auth)
        self.assertIn('"Codex token exchange: http_status=%d parsed=%d"', poll_auth)
        fetch_usage = production_function(source, "static int fetch_usage(")
        self.assertIn('"Codex usage check: http_status=%d parsed=%d"', fetch_usage)
        for diagnostic in (poll_auth, fetch_usage):
            logs = "\n".join(re.findall(r"ESP_LOG[IW]\([^;]+;", diagnostic))
            self.assertNotIn("response->body", logs)
            self.assertNotIn("candidate->access", logs)
            self.assertNotIn("s_device.device_auth_id", logs)
            self.assertNotIn("s_device.user_code", logs)

        scaffold = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codex_protocol.h"
#include "usage_provider.h"

typedef struct {
    bool configured;
    char state[24];
    char verification_url[128];
    char user_code[32];
    int64_t expires_at;
    provider_usage_t usage;
    struct { uint32_t id; char method[24], state[16], message[96]; } operation;
} codex_management_status_t;

#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))

static codex_management_status_t s_status;
static codex_tokens_t *s_active;
static bool s_committing, s_account_validated, s_reauth_required, s_token_dirty;
static bool s_auth_pending, fail_save;
static uint32_t s_generation=7, s_auth_generation=7, s_auth_request_id=51;
static int64_t s_retry_until_us=88000000000LL, s_next_usage_us=99999999999LL;
static int64_t s_next_storage_retry_us;
static uint32_t s_backoff_sec=240;
static int s_mux, save_calls, publish_calls, operation_calls;
static bool clear_auth_saw_reserved;
static codex_tokens_t saved_token;
static void copy_text(char *out, size_t size, const char *in) {
    snprintf(out, size, "%s", in ? in : "");
}
static void set_state(const char *state, const char *message) {
    copy_text(s_status.state, sizeof(s_status.state), state);
    copy_text(s_status.usage.status, sizeof(s_status.usage.status), message);
}
static void clear_auth(void) {
    clear_auth_saw_reserved = s_committing;
    s_auth_pending = false;
    s_status.user_code[0] = 0;
    s_status.verification_url[0] = 0;
    s_status.expires_at = 0;
}
static void publish(void) { ++publish_calls; }
static void operation(uint32_t id, const char *method, const char *state, const char *message) {
    assert(id == 51 && strcmp(method, "codex.begin") == 0);
    assert(strcmp(state, "done") == 0);
    assert(strstr(message, "saved") != NULL);
    ++operation_calls;
}
static bool storage_save(const codex_tokens_t *candidate) {
    assert(candidate && strcmp(candidate->access, "new-access") == 0);
    ++save_calls;
    if (fail_save) return false;
    saved_token = *candidate;
    return true;
}
FUNCTIONS

static codex_tokens_t *new_tokens(const char *access, const char *refresh, const char *account) {
    codex_tokens_t *token = calloc(1, sizeof(*token)); assert(token);
    copy_text(token->access, sizeof(token->access), access);
    copy_text(token->refresh, sizeof(token->refresh), refresh);
    copy_text(token->account_id, sizeof(token->account_id), account);
    token->expires_at = 1760003600;
    return token;
}
static void init_old_account(void) {
    memset(&s_status, 0, sizeof(s_status));
    s_active = new_tokens("old-access", "old-refresh", "old-account");
    s_status.configured = true;
    s_status.usage.configured = true;
    s_status.usage.has_sample = true;
    s_status.usage.sampled_at = 1759999900;
    s_status.usage.windows[0].valid = true;
    s_status.usage.windows[0].used_percent = 42;
    s_auth_pending = true;
    s_status.expires_at = 1760000800;
    s_auth_generation = s_generation;
    s_committing = s_account_validated = s_reauth_required = s_token_dirty = false;
    s_retry_until_us = 88000000000LL;
    s_next_usage_us = 99999999999LL;
    s_backoff_sec = 240;
    fail_save = false;
    save_calls = publish_calls = operation_calls = 0;
}
int main(void) {
    init_old_account();
    codex_tokens_t *candidate = new_tokens("new-access", "new-refresh", "new-account");
    codex_tokens_t *old_pointer = s_active;
    assert(commit_authorized_account(candidate));
    assert(save_calls == 1 && strcmp(saved_token.access, "new-access") == 0);
    assert(s_active == old_pointer && strcmp(s_active->access, "new-access") == 0);
    assert(strcmp(s_active->account_id, "new-account") == 0);
    assert(s_status.configured && s_status.usage.configured);
    assert(!s_status.usage.has_sample && !s_status.usage.windows[0].valid);
    assert(s_status.usage.sampled_at == 0);
    assert(strcmp(s_status.state, "checking") == 0);
    assert(strcmp(s_status.usage.status, "Checking account usage") == 0);
    assert(!s_account_validated && !s_reauth_required && !s_token_dirty);
    assert(!s_auth_pending && s_status.expires_at == 0);
    assert(clear_auth_saw_reserved && s_retry_until_us == 0 &&
           s_next_usage_us == 0 && s_backoff_sec == 30);
    assert(publish_calls == 1 && operation_calls == 1 && !s_committing);

    init_old_account();
    old_pointer = s_active;
    fail_save = true;
    assert(!commit_authorized_account(candidate));
    assert(save_calls == 1 && s_active == old_pointer);
    assert(strcmp(s_active->access, "old-access") == 0);
    assert(s_status.usage.has_sample && s_status.usage.windows[0].used_percent == 42);
    assert(s_retry_until_us == 88000000000LL && s_next_usage_us == 99999999999LL);
    assert(s_auth_pending && s_status.expires_at == 1760000800);
    assert(publish_calls == 0 && operation_calls == 0 && !s_committing);

    init_old_account();
    old_pointer = s_active;
    assert(current_generation(s_generation));
    s_generation++;
    assert(!current_generation(s_auth_generation));
    assert(!commit_authorized_account(candidate));
    assert(save_calls == 0 && s_active == old_pointer);
    assert(strcmp(s_active->access, "old-access") == 0);
    assert(s_status.usage.has_sample && s_retry_until_us == 88000000000LL);
    assert(s_auth_pending && publish_calls == 0 && !s_committing);

    init_old_account();
    s_committing = true;
    assert(!commit_authorized_account(candidate));
    assert(save_calls == 0 && strcmp(s_active->access, "old-access") == 0);
    assert(s_status.usage.has_sample && s_retry_until_us == 88000000000LL);

    assert(!commit_authorized_account(NULL));
    free(candidate);
    free(s_active);
    return 0;
}
'''.replace("FUNCTIONS", functions)

        with tempfile.TemporaryDirectory(prefix="codex-auth-commit-") as directory:
            code = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            code.write_text(scaffold)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-Wno-unused-parameter", "-fsanitize=address,undefined",
                                    "-I", str(ROOT / "main"), str(code), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
