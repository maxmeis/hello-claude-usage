"""Exercise the production Codex wire parser with bounded host inputs."""

import base64
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CJSON = ROOT / ".deps/esp-idf-v5.1.6/components/json/cJSON"


class CodexProtocolTests(unittest.TestCase):
    def test_device_auth_tokens_and_sparse_usage(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required for native parser test")
        payload = json.dumps({"https://api.openai.com/auth": {"chatgpt_account_id": "acct-123"}},
                             separators=(",", ":")).encode()
        jwt = "a." + base64.urlsafe_b64encode(payload).decode().rstrip("=") + ".b"
        exp_payload = json.dumps({"exp": 1760005400}, separators=(",", ":")).encode()
        access_jwt = "a." + base64.urlsafe_b64encode(exp_payload).decode().rstrip("=") + ".b"
        initial = json.dumps({"access_token": "access", "refresh_token": "refresh",
                              "id_token": jwt, "expires_in": 3600})
        refresh = json.dumps({"access_token": "next", "expires_in": 7200})
        no_lifetime = json.dumps({"access_token": access_jwt,
                                  "refresh_token": "refresh", "id_token": jwt})
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "codex_protocol.h"
int main(void) {
    codex_device_code_t code;
    assert(codex_parse_device_code("{\"device_auth_id\":\"private\",\"user_code\":\"ABCD-EFGH\",\"interval\":\"5\"}", &code));
    assert(code.interval_sec == 5 && strcmp(code.user_code, "ABCD-EFGH") == 0);
    assert(codex_parse_device_code("{\"device_auth_id\":\"private\",\"usercode\":\"WXYZ-1234\",\"interval\":\"5\"}", &code));
    assert(strcmp(code.user_code,"WXYZ-1234")==0);
    assert(codex_parse_device_code("{\"device_auth_id\":\"private\",\"user_code\":\"WXYZ-1234\"}", &code));
    assert(code.interval_sec==5);
    assert(!codex_parse_device_code("{\"device_auth_id\":\"private\",\"user_code\":\"A\",\"usercode\":\"B\",\"interval\":\"5\"}", &code));
    assert(!codex_parse_device_code("{\"device_auth_id\":\"x\",\"user_code\":\"y\",\"interval\":\"0\"}", &code));
    assert(!code.device_auth_id[0]);
    codex_authorization_t authorization;
    assert(codex_parse_authorization("{\"authorization_code\":\"once\",\"code_verifier\":\"proof\"}", &authorization));
    assert(!codex_parse_authorization("{\"authorization_code\":\"once\"}", &authorization));
    codex_tokens_t *first = calloc(1,sizeof(*first)), *next = calloc(1,sizeof(*next));
    assert(first && next);
    assert(codex_parse_tokens(INITIAL, NULL, 1760000000, first));
    assert(strcmp(first->account_id,"acct-123")==0 && first->expires_at == 1760003600);
    assert(codex_parse_tokens(REFRESH, first, 1760000000, next));
    assert(strcmp(next->access,"next")==0 && strcmp(next->refresh,"refresh")==0);
    assert(strcmp(next->account_id,"acct-123")==0 && next->expires_at==1760007200);
    assert(codex_parse_tokens(NO_LIFETIME,NULL,1760000000,next));
    assert(next->expires_at==1760005400);
    assert(!codex_parse_tokens("{\"access_token\":\"bad\",\"id_token\":\"a.bad.b\",\"expires_in\":3600}",first,1760000000,next));
    assert(!next->access[0]);
    provider_usage_t usage;
    assert(codex_parse_usage("{\"rate_limit\":{\"primary_window\":{\"used_percent\":42.5,\"limit_window_seconds\":18000,\"reset_at\":1760000300}},\"rate_limit_reset_credits\":{\"available_count\":3}}",1760000000,&usage));
    assert(usage.windows[0].valid && usage.windows[0].used_percent==42.5f);
    assert(!usage.windows[1].valid && usage.reset_credits_known && usage.reset_credits==3);
    assert(codex_parse_usage("{\"rate_limit\":{\"primary_window\":null,\"secondary_window\":{\"used_percent\":101,\"limit_window_seconds\":604800,\"reset_at\":1760000300}}}",1760000000,&usage));
    assert(!usage.windows[0].valid && !usage.windows[1].valid && !usage.reset_credits_known);
    assert(codex_parse_usage("{\"rate_limit\":null,\"rate_limit_reset_credits\":{\"available_count\":2}}",1760000000,&usage));
    assert(!usage.windows[0].valid && !usage.windows[1].valid && usage.reset_credits==2);
    uint32_t retry;
    assert(codex_parse_retry_after("60",&retry) && retry==60);
    assert(!codex_parse_retry_after("999999999",&retry));
    free(first); free(next);
    return 0;
}
'''.replace("INITIAL", json.dumps(initial)).replace("REFRESH", json.dumps(refresh)).replace("NO_LIFETIME", json.dumps(no_lifetime))
        with tempfile.TemporaryDirectory(prefix="codex-protocol-") as directory:
            code = Path(directory) / "test.c"
            executable = Path(directory) / "test"
            code.write_text(source)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-deprecated-declarations",
                            "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                            "-I", str(CJSON), str(ROOT / "main/codex_protocol.c"),
                            str(CJSON / "cJSON.c"), str(code), "-lm", "-o", str(executable)],
                           capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
