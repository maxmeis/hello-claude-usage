"""Run the production Codex usage worker branch against deterministic HTTP stubs."""

from pathlib import Path
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


class CodexWorkerTests(unittest.TestCase):
    def test_cancel_restores_unvalidated_saved_account_state(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required for account-state test")
        source = (ROOT / "main/codex_adapter.c").read_text()
        state = production_function(source, "static const char *account_state(void)\n{")
        scaffold = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
static int token;
static int *s_active=&token;
static bool s_account_validated,s_reauth_required,s_token_dirty;
STATE
int main(void) {
    assert(strcmp(account_state(),"checking")==0);
    s_account_validated=true;
    assert(strcmp(account_state(),"connected")==0);
    s_token_dirty=true;
    assert(strcmp(account_state(),"storage_pending")==0);
    s_token_dirty=false;
    s_reauth_required=true;
    assert(strcmp(account_state(),"reauth_required")==0);
    s_reauth_required=false;s_active=0;
    assert(strcmp(account_state(),"signed_out")==0);
    return 0;
}
'''.replace("STATE", state)
        with tempfile.TemporaryDirectory(prefix="codex-account-state-") as directory:
            code = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            code.write_text(scaffold)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", str(code), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
        worker = production_function(source, "static void worker(void *unused)")
        self.assertIn("clear_auth();\n                set_state(account_state(),", worker)

    def test_rotated_refresh_token_survives_one_storage_failure(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required for refresh persistence test")
        source = (ROOT / "main/codex_adapter.c").read_text()
        functions = "\n".join(production_function(source, signature) for signature in (
            "static int refresh_token(uint32_t *retry_after)",
            "static void retry_dirty_token(void)",
            "static const char *account_state(void)\n{"))
        scaffold = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct { char access[32],refresh[32]; int64_t expires_at; } codex_tokens_t;
typedef struct { char body[128]; uint32_t retry_after; } http_response_t;
static codex_tokens_t *s_active;
static bool s_token_dirty, s_auth_pending, s_account_validated,
            s_reauth_required, fail_save=true;
static int64_t s_next_storage_retry_us, s_retry_until_us, current_us;
static int s_mux;
static struct {char state[24],status[72];} s_status;
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static int64_t esp_timer_get_time(void) { return current_us; }
static void copy_text(char *out,size_t size,const char *value) { snprintf(out,size,"%s",value); }
static void set_state(const char *state,const char *message) {
    copy_text(s_status.state,sizeof(s_status.state),state);
    copy_text(s_status.status,sizeof(s_status.status),message);
}
static const char *account_state(void);
static int http_calls, save_calls;
#define AUTH_BASE "https://auth.openai.com"
static char *refresh_body(const char *refresh) {
    assert(strcmp(refresh,"oldrefresh")==0);
    char *body=malloc(8); assert(body); strcpy(body,"request"); return body;
}
static void wipe_free(char *body) { memset(body,0,strlen(body)); free(body); }
static int http_request(const char *url,const char *body,const char *content_type,
                        const codex_tokens_t *token,http_response_t *response) {
    assert(strstr(url,"oauth/token") && strcmp(body,"request")==0);
    assert(strcmp(content_type,"application/json")==0 && !token);
    strcpy(response->body,"response"); ++http_calls; return 200;
}
static int64_t time_utc(void) { return 1760000000; }
#define time(x) time_utc()
static bool codex_parse_tokens(const char *json,const codex_tokens_t *current,
                               int64_t now,codex_tokens_t *out) {
    assert(strcmp(json,"response")==0 && strcmp(current->access,"old")==0);
    assert(now==1760000000);
    strcpy(out->access,"new"); strcpy(out->refresh,"newrefresh");
    out->expires_at=now+3600; return true;
}
static bool storage_save(const codex_tokens_t *token) {
    assert(strcmp(token->access,"new")==0);
    assert(strcmp(token->refresh,"newrefresh")==0);
    ++save_calls; return !fail_save;
}
FUNCTIONS
int main(void) {
    s_active=calloc(1,sizeof(*s_active)); assert(s_active);
    strcpy(s_active->access,"old"); strcpy(s_active->refresh,"oldrefresh");
    uint32_t retry=0;
    assert(refresh_token(&retry)==200);
    assert(http_calls==1 && save_calls==1 && s_token_dirty);
    assert(strcmp(s_active->access,"new")==0);
    assert(strcmp(s_active->refresh,"newrefresh")==0);
    assert(strcmp(s_status.state,"storage_pending")==0);
    s_retry_until_us=86400000000LL; /* A prior usage 429 must not delay local save. */
    assert(s_next_storage_retry_us==5000000);
    fail_save=false;retry_dirty_token();
    assert(save_calls==1);
    current_us=5000000;retry_dirty_token();
    assert(save_calls==2 && !s_token_dirty && http_calls==1);
    assert(current_us<s_retry_until_us && strcmp(s_status.state,"checking")==0);
    free(s_active); return 0;
}
'''.replace("FUNCTIONS", functions)
        with tempfile.TemporaryDirectory(prefix="codex-rotation-") as directory:
            code = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            code.write_text(scaffold)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", str(code), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
        worker_source = production_function(source, "static void worker(void *unused)")
        self.assertLess(worker_source.index("retry_dirty_token();"),
                        worker_source.index("poll_usage();"))

    def test_begin_operation_survives_footer_refresh_attempt(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required for adapter admission test")
        request = production_function((ROOT / "main/codex_adapter.c").read_text(),
                                      "bool codex_adapter_request(uint32_t id, const char *method)")
        scaffold = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "usage_provider.h"
typedef enum { CMD_BEGIN, CMD_CANCEL, CMD_REFRESH } command_kind_t;
typedef struct { command_kind_t kind; uint32_t id, generation; } command_t;
static struct {
    bool configured; provider_usage_t usage;
    struct { uint32_t id; char method[24],state[16],message[96]; } operation;
} s_status;
static bool s_storage_ready=true,s_committing,s_auth_pending,s_refresh_pending,s_usage_busy;
static uint32_t s_generation,s_refresh_id;
static int64_t s_retry_until_us;
static int s_mux,s_commands=1,s_task=1,notify_count,queue_count;
static command_t queued;
#define pdTRUE 1
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static bool network_ready(void) { return true; }
static int64_t esp_timer_get_time(void) { return 1000000; }
static void copy_text(char *dest,size_t size,const char *src) { snprintf(dest,size,"%s",src); }
static int xQueueSend(int queue,const command_t *command,int timeout) {
    assert(queue==1 && timeout==0); queued=*command; ++queue_count; return pdTRUE;
}
static void xTaskNotifyGive(int task) { assert(task==1); ++notify_count; }
REQUEST
int main(void) {
    s_status.configured=true;
    assert(codex_adapter_request(123,"codex.begin"));
    assert(s_auth_pending && queue_count==1 && queued.id==123);
    assert(strcmp(s_status.operation.method,"codex.begin")==0);
    assert(!codex_adapter_request(0,"codex.refresh"));
    assert(queue_count==1 && s_status.operation.id==123);
    assert(strcmp(s_status.operation.method,"codex.begin")==0);
    assert(!s_refresh_pending && notify_count==1);
    s_auth_pending=false;
    s_refresh_pending=true;
    assert(!codex_adapter_request(124,"codex.begin"));
    assert(s_status.operation.id==123);
    s_refresh_pending=false;
    assert(codex_adapter_request(0,"codex.refresh"));
    assert(s_status.operation.id==123 && s_refresh_pending);
    s_refresh_pending=false;
    s_committing=true;
    uint32_t before_generation=s_generation;
    int before_queue_count=queue_count;
    assert(!codex_adapter_request(125,"codex.cancel"));
    assert(s_generation==before_generation && queue_count==before_queue_count);
    return 0;
}
'''.replace("REQUEST", request)
        with tempfile.TemporaryDirectory(prefix="codex-admission-") as directory:
            code = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            code.write_text(scaffold)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                                    str(code), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_real_worker_retry_and_operation_transitions(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required for worker sanitizer test")
        worker = production_function((ROOT / "main/codex_adapter.c").read_text(),
                                     "static int poll_usage(void)")
        scaffold = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "usage_provider.h"
typedef struct { int64_t expires_at; } codex_tokens_t;
typedef uint16_t app_id_t;
enum { APP_ID_CLAUDE = 1, APP_ID_CODEX = 2 };
typedef struct { uint8_t providers; } box_settings_t;
static codex_tokens_t token = {.expires_at=0}, *s_active=&token;
static struct { bool configured; char state[24]; provider_usage_t usage; } s_status;
static int s_mux;
static bool s_refresh_pending, s_usage_busy, s_auth_pending, s_token_dirty,
            s_account_validated, s_reauth_required, net=true, selected=true, refresh_ok=true;
static uint32_t s_refresh_id, s_backoff_sec=30, next_retry_after, next_refresh_retry_after;
static uint32_t s_usage_cycle_generation;
static int64_t s_next_usage_us, s_retry_until_us, current_us=1000000000;
static int next_status[3], status_count, status_index;
static int fetch_calls, refresh_calls, face_start, face_end, publish_count;
static uint32_t started_generation, ended_generation;
static int operation_id; static char operation_state[16], operation_message[64];
static box_settings_t settings={.providers=1};
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static bool network_ready(void) { return net; }
static bool enabled(void) { return selected; }
static int64_t esp_timer_get_time(void) { return current_us; }
static int64_t time_utc(void) { return 1760000000; }
#define time(x) time_utc()
static uint32_t interval_sec(void) { return 300; }
static void display_app_refresh_begin(app_id_t id, uint32_t generation) {
    assert(id == APP_ID_CODEX && generation > started_generation);
    started_generation = generation; ++face_start;
}
static void display_app_refresh_end(app_id_t id, uint32_t generation) {
    assert(id == APP_ID_CODEX && generation == started_generation);
    ended_generation = generation; ++face_end;
}
static void publish(void) { ++publish_count; }
static int refresh_status=200;
static int refresh_token(uint32_t *retry_after) {
    ++refresh_calls; *retry_after=next_refresh_retry_after;
    return refresh_ok ? refresh_status : -1;
}
static void copy_text(char *dest, size_t size, const char *source) {
    snprintf(dest,size,"%s",source);
}
static void set_state(const char *state,const char *message) {
    copy_text(s_status.state,sizeof(s_status.state),state);
    copy_text(s_status.usage.status,sizeof(s_status.usage.status),message);
}
static const char *account_state(void) {
    if (s_reauth_required) return "reauth_required";
    if (!s_active) return "signed_out";
    if (s_token_dirty) return "storage_pending";
    return s_account_validated ? "connected" : "checking";
}
static int fetch_usage(const codex_tokens_t *unused, provider_usage_t *usage,
                       uint32_t *retry_after) {
    (void)unused; assert(status_index<status_count);
    ++fetch_calls; *retry_after=next_retry_after;
    usage->configured=true; usage->has_sample=true;
    usage->sampled_at=1760000000;
    usage->reset_credits_known=true; usage->reset_credits=2;
    return next_status[status_index++];
}
static void operation(uint32_t id,const char *method,const char *state,const char *message) {
    assert(strcmp(method,"codex.refresh")==0);
    operation_id=(int)id;
    copy_text(operation_state,sizeof(operation_state),state);
    copy_text(operation_message,sizeof(operation_message),message);
}
WORKER
static void reset_case(void) {
    memset(&s_status,0,sizeof(s_status));
    s_next_usage_us=s_retry_until_us=0; s_refresh_pending=s_usage_busy=false;
    s_backoff_sec=30; current_us=1000000000;
    net=selected=refresh_ok=true; settings.providers=1;
    s_token_dirty=s_account_validated=s_reauth_required=s_auth_pending=false;
    token.expires_at=0; refresh_status=200; next_refresh_retry_after=0;
    fetch_calls=refresh_calls=face_start=face_end=publish_count=0;
    status_count=status_index=0; next_retry_after=0;
    operation_state[0]=operation_message[0]=0;
}
int main(void) {
    reset_case();
    next_status[0]=200;status_count=1;
    assert(poll_usage()==200 && fetch_calls==1);
    assert(s_status.usage.has_sample && s_status.usage.reset_credits==2);
    assert(!s_status.usage.updating && face_start==1 && face_end==1);
    assert(s_next_usage_us==current_us+300000000LL);
    assert(poll_usage()==-2 && fetch_calls==1);

    reset_case();
    copy_text(s_status.state,sizeof(s_status.state),"error");
    next_status[0]=200;status_count=1;
    assert(poll_usage()==200 && strcmp(s_status.state,"connected")==0);

    reset_case();
    s_refresh_pending=true;s_refresh_id=7;s_status.usage.updating=true;
    next_status[0]=429;status_count=1;next_retry_after=120;
    assert(poll_usage()==429 && fetch_calls==1);
    assert(s_retry_until_us==current_us+120000000LL);
    assert(!s_refresh_pending && !s_status.usage.updating);
    assert(operation_id==7 && strcmp(operation_state,"error")==0);
    assert(strstr(operation_message,"Rate limited"));
    assert(poll_usage()==-2 && fetch_calls==1);

    reset_case();
    s_refresh_pending=true;s_refresh_id=8;
    next_status[0]=401;next_status[1]=429;status_count=2;next_retry_after=73;
    assert(poll_usage()==429 && fetch_calls==2 && refresh_calls==1);
    assert(s_retry_until_us==current_us+73000000LL);
    assert(strcmp(operation_state,"error")==0);

    reset_case();
    token.expires_at=1760000200;refresh_status=429;next_refresh_retry_after=60;
    s_refresh_pending=true;s_refresh_id=11;
    assert(poll_usage()==429 && refresh_calls==1 && fetch_calls==0);
    assert(s_retry_until_us==current_us+60000000LL);
    assert(poll_usage()==-2 && refresh_calls==1);

    reset_case();
    next_status[0]=401;status_count=1;
    refresh_status=429;next_refresh_retry_after=50;
    assert(poll_usage()==429 && fetch_calls==1 && refresh_calls==1);
    assert(s_retry_until_us==current_us+50000000LL);

    reset_case();
    token.expires_at=1760000200;refresh_status=401;
    assert(poll_usage()==401 && fetch_calls==0 && refresh_calls==1);
    assert(strcmp(s_status.state,"reauth_required")==0);

    reset_case();
    s_refresh_pending=true;s_refresh_id=10;refresh_status=401;
    s_status.usage.configured=true;
    next_status[0]=401;status_count=1;
    assert(poll_usage()==401 && strcmp(s_status.state,"reauth_required")==0);
    assert(s_status.usage.configured);

    reset_case();
    copy_text(s_status.state,sizeof(s_status.state),"connected");
    next_status[0]=429;status_count=1;next_retry_after=10;
    assert(poll_usage()==429 && strcmp(s_status.state,"connected")==0);

    /* The device-code token is now saved before this independent quota read.
       A temporary usage 429 must leave the account configured and checking. */
    reset_case();
    s_status.configured=true;s_status.usage.configured=true;
    copy_text(s_status.state,sizeof(s_status.state),"checking");
    next_status[0]=429;status_count=1;next_retry_after=120;
    assert(poll_usage()==429 && s_active==&token);
    assert(s_status.configured && s_status.usage.configured);
    assert(strcmp(s_status.state,"checking")==0);
    assert(strcmp(s_status.usage.status,"Retry after cooldown")==0);
    assert(s_retry_until_us==current_us+120000000LL);

    reset_case();
    s_status.configured=true;s_status.usage.configured=true;
    copy_text(s_status.state,sizeof(s_status.state),"checking");
    next_status[0]=401;status_count=1;
    refresh_status=401;
    assert(poll_usage()==401 && s_active==&token);
    assert(s_status.configured && s_status.usage.configured);
    assert(strcmp(s_status.state,"reauth_required")==0);

    reset_case();
    s_refresh_pending=true;s_refresh_id=9;net=false;
    assert(poll_usage()==-2 && fetch_calls==0 && s_refresh_pending);
    assert(operation_state[0]==0);
    net=true;next_status[0]=200;status_count=1;
    assert(poll_usage()==200 && strcmp(operation_state,"done")==0);

    reset_case();settings.providers=2;next_status[0]=200;status_count=1;
    assert(poll_usage()==200 && face_start==1 && face_end==1);
    assert(ended_generation==started_generation);
    return 0;
}
'''.replace("WORKER", worker)
        with tempfile.TemporaryDirectory(prefix="codex-worker-") as directory:
            code = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            code.write_text(scaffold)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                                    str(code), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
