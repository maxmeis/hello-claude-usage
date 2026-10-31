"""Compile and exercise the production Wi-Fi disconnect reason mapper."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class WifiFailureMessageTests(unittest.TestCase):
    def test_reason_categories_and_safe_default_under_sanitizers(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang is required for ASan/UBSan host tests")

        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wifi_failure.h"

static void expect(uint16_t reason, const char *phone, const char *device)
{
    const char *p = wifi_failure_message(reason);
    const char *d = wifi_failure_device_message(reason);
    if (!p || !d || !strstr(p, phone) || !strstr(d, device))
        fprintf(stderr, "unexpected mapping %u: %s | %s (wanted %s / %s)\n",
                reason, p ? p : "NULL", d ? d : "NULL", phone, device);
    assert(p && d && strstr(p, phone) && strstr(d, device));
    assert(!strstr(p, "HomeNetwork") && !strstr(d, "HomeNetwork"));
    assert(!strstr(p, "s3cr3t-password") && !strstr(d, "s3cr3t-password"));
    assert(!strstr(p, "setup-capability") && !strstr(d, "setup-capability"));
    printf("%u: %s | %s\n", reason, p, d);
}

int main(void)
{
    expect(202, "authentication", "password"); /* AUTH_FAIL */
    expect(201, "Network not found", "Network not found"); /* NO_AP_FOUND */
    expect(200, "timed out", "timed out"); /* BEACON_TIMEOUT */
    expect(204, "handshake timed out", "check signal"); /* HANDSHAKE_TIMEOUT */
    expect(15, "handshake timed out", "check signal"); /* 4WAY_HANDSHAKE_TIMEOUT */
    expect(16, "handshake timed out", "check signal"); /* GROUP_KEY_UPDATE_TIMEOUT */
    assert(!strstr(wifi_failure_message(204), "authentication failed"));
    expect(203, "association", "association"); /* ASSOC_FAIL */
    expect(255, "connection failed", "Wi-Fi failed"); /* unknown */

    /* Unknown reasons stay generic and must not imply that the password is wrong. */
    const char *phone = wifi_failure_message(255);
    const char *device = wifi_failure_device_message(255);
    assert(!strstr(phone, "password") && !strstr(device, "password"));
    puts("Wi-Fi failure reason mapping passed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="wifi-failure-") as directory:
            c_file = Path(directory) / "wifi_failure_test.c"
            executable = Path(directory) / "wifi_failure_test"
            c_file.write_text(harness)
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                str(ROOT / "main/wifi_failure.c"), str(c_file), "-o", str(executable),
            ], check=True)
            result = subprocess.run([str(executable)], check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Wi-Fi failure reason mapping passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
