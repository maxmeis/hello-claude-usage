"""Production USB JSON parser and dispatch with host stand-ins for device workers."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CJSON = Path("/Users/max/.platformio/packages/framework-espidf/components/json/cJSON")

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "box_config.h"
#include "usage_provider.h"
#define RPC_PREFIX "HELLO_BOX_RPC "
#define RPC_REQUEST_MAX 768u
#define RPC_RESPONSE_MAX 4096u
typedef struct {char version[32];} esp_app_desc_t;
static const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t v = {"0.16.0-usb-setup"}; return &v;
}
static const char *device_identity_name(void) { return "hello-box-1234"; }
typedef struct {uint32_t operation_id;bool configured,connected,provisioning;char state[16],message[96];} wifi_manager_status_t;
typedef struct {
    bool configured; char state[16],session[48],authorization_url[512];
    struct {char state[16];bool wifi,ip,clock,dns,https;char message[96];} network;
    struct {uint32_t id;char method[24],state[16],message[96];} operation;
    provider_usage_t usage;
} oauth_management_status_t;
typedef struct {
    bool configured; char state[24], verification_url[128], user_code[32];
    int64_t expires_at; provider_usage_t usage;
    struct {uint32_t id; char method[24], state[16], message[96];} operation;
} codex_management_status_t;
static codex_management_status_t codex_status;
static void codex_adapter_get_status(codex_management_status_t *out) { *out=codex_status; }
static int requests;
static char last_method[24],last_arg1[80],last_arg2[80];
static void wifi_manager_get_status(wifi_manager_status_t *s) {
    *s=(wifi_manager_status_t){.operation_id=7,.configured=true,.connected=false,.provisioning=true};
    strcpy(s->state,"failed"); strcpy(s->message,"Connection test failed");
}
static void oauth_probe_get_management_status(oauth_management_status_t *s) {
    memset(s,0,sizeof(*s)); s->configured=true;
    strcpy(s->state,"ready"); strcpy(s->network.state,"idle");
    strcpy(s->network.message,"Not tested");
    s->operation.id=7; strcpy(s->operation.method,"wifi.connect");
    strcpy(s->operation.state,"failed"); strcpy(s->operation.message,"Trial failed");
}
static bool wifi_store_valid_credentials(const char *ssid,const char *password) {
    size_t n=strlen(ssid),p=strlen(password);
    return n>0 && n<=32 && (p==0 || (p>=8 && p<=63));
}
static bool oauth_probe_management_request(uint32_t id,const char *method,const char *a,const char *b) {
    assert(id>0); ++requests; snprintf(last_method,sizeof(last_method),"%s",method);
    snprintf(last_arg1,sizeof(last_arg1),"%s",a?a:"");
    snprintf(last_arg2,sizeof(last_arg2),"%s",b?b:"");
    return strcmp(method,"claude.cancel")!=0;
}
'''

STUBS += r'''
#include "app_registry.h"
#include "screensaver_registry.h"
const app_registry_t *display_app_registry(void) {
    static const app_registry_t apps = {.count=2, .items={
        {.id=APP_ID_CLAUDE,.name="Claude",.capabilities=APP_CAP_REFRESH|APP_CAP_INTERRUPT_SAVER},
        {.id=APP_ID_CODEX,.name="Codex",.capabilities=APP_CAP_REFRESH|APP_CAP_INTERRUPT_SAVER}}};
    return &apps;
}
const screensaver_registry_t *display_screensaver_registry(void) {
    static const screensaver_registry_t savers = {.count=1,.items={
        {.id=SAVER_ID_WORD_CLOCK,.name="Word clock"}}};
    return &savers;
}
'''

HARNESS = r'''
static void check(const char *line, bool accepted, uint32_t id, bool ok) {
    char response[4097]; size_t n=strlen(line);
    bool got=usb_management_process_line(line,n,response,sizeof(response));
    assert(got==accepted);
    if (!got) return;
    assert(!strncmp(response,"HELLO_BOX_RPC ",14));
    assert(response[strlen(response)-1]=='\n');
    cJSON *json=cJSON_Parse(response+14); assert(json);
    assert((uint32_t)cJSON_GetObjectItem(json,"id")->valuedouble==id);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(json,"ok"))==ok);
    cJSON_Delete(json);
}
int main(void) {
    check("HELLO_BOX_RPC {\"id\":1,\"method\":\"status.get\",\"params\":{}}",true,1,true);
    char response[4097];
    const char *status="HELLO_BOX_RPC {\"id\":2,\"method\":\"status.get\",\"params\":{}}";
    assert(usb_management_process_line(status,strlen(status),response,sizeof(response)));
    assert(strstr(response,"\"firmwareVersion\":\"0.16.0-usb-setup\""));
    assert(strstr(response,"\"clockLanguage\":\"de\""));
    assert(strstr(response,"\"dashboardTimeout\":30"));
    assert(strstr(response,"\"providers\":\"claude\""));
    assert(strstr(response,"\"configured\":true"));
    assert(strstr(response,"\"codex\":{\"configured\":false"));
    assert(strstr(response,"\"resetCreditsKnown\":false"));
    assert(strstr(response,"\"operationId\":7"));
    assert(!strstr(response,"password"));
    check("HELLO_BOX_RPC {\"id\":3,\"method\":\"wifi.connect\",\"params\":{\"ssid\":\"mywifi\",\"password\":\"secret123\"}}",true,3,true);
    assert(requests==1 && !strcmp(last_method,"wifi.connect") && !strcmp(last_arg1,"mywifi") && !strcmp(last_arg2,"secret123"));
    check("HELLO_BOX_RPC {\"id\":4,\"method\":\"claude.submit_code\",\"params\":{\"session\":\"s1\",\"code\":\"authorization-code\"}}",true,4,true);
    assert(requests==2 && !strcmp(last_arg1,"s1") && !strcmp(last_arg2,"authorization-code"));
    check("HELLO_BOX_RPC {\"id\":5,\"method\":\"claude.cancel\",\"params\":{}}",true,5,false);
    check("HELLO_BOX_RPC {\"id\":6,\"method\":\"wifi.connect\",\"params\":{\"ssid\":\"wifi\",\"password\":\"short\"}}",true,6,false);
    check("HELLO_BOX_RPC {\"id\":7,\"method\":\"unknown\",\"params\":{}}",true,7,false);
    check("HELLO_BOX_RPC {\"id\":0,\"method\":\"status.get\",\"params\":{}}",false,0,false);
    check("HELLO_BOX_RPC {\"id\":1,\"id\":2,\"method\":\"status.get\",\"params\":{}}",false,0,false);
    check("HELLO_BOX_RPC {\"id\":1,\"method\":\"status.get\",\"params\":{}}\r",false,0,false);
    check("HELLO_BOX_RPC {\"id\":1,\"method\":\"status.get\",\"params\":{}} trailing",false,0,false);
    char huge[900]; memset(huge,'x',sizeof(huge)); huge[sizeof(huge)-1]=0;
    check(huge,false,0,false);
    return 0;
}
'''


class UsbManagementTests(unittest.TestCase):
    def test_actual_parser_dispatch_and_secret_free_responses(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler or not CJSON.exists():
            self.skipTest("Host C compiler or pinned cJSON unavailable")
        source = (ROOT / "main" / "usb_management.c").read_text()
        parser = "static const char *const s_timezones" + source.split("static const char *const s_timezones", 1)[1].split("static void reader", 1)[0]
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            path = directory / "harness.c"
            path.write_text(STUBS + parser + HARNESS)
            binary = directory / "usb-test"
            result = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     "-I", str(ROOT / "main"), "-I", str(CJSON),
                                     str(ROOT / "main" / "box_config.c"), str(CJSON / "cJSON.c"),
                                     str(path), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
