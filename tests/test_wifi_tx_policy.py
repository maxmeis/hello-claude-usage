"""Host tests for the pure adaptive Wi-Fi TX fallback eligibility policy."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class WifiTxPolicyTests(unittest.TestCase):
    def test_reason_rssi_attempt_and_original_limit_boundaries(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("A C compiler is required for the TX policy host test")

        source = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "wifi_tx_policy.h"

int main(void) {
    /* Confirmed setup failures at strong RSSI may use the single TX fallback. */
    assert(wifi_tx_policy_eligible(2, true, -26, false, true, 52));
    assert(wifi_tx_policy_eligible(39, true, -26, false, true, 52));
    assert(wifi_tx_policy_eligible(204, true, -20, false, true, 52));
    assert(wifi_tx_policy_eligible(15, true, -40, false, true, 29));
    assert(wifi_tx_policy_eligible(15, true, -1, false, true, 52));

    /* Signal must be valid and strong enough; SDK sentinel and zero are invalid. */
    assert(!wifi_tx_policy_eligible(204, true, -41, false, true, 52));
    assert(!wifi_tx_policy_eligible(204, true, 0, false, true, 52));
    assert(!wifi_tx_policy_eligible(204, true, -128, false, true, 52));
    assert(!wifi_tx_policy_eligible(204, false, -20, false, true, 52));

    /* Other disconnects, a second fallback, and missing/low originals are excluded. */
    assert(!wifi_tx_policy_eligible(202, true, -20, false, true, 52));
    assert(!wifi_tx_policy_eligible(201, true, -20, false, true, 52));
    assert(!wifi_tx_policy_eligible(200, true, -20, false, true, 52));
    assert(!wifi_tx_policy_eligible(205, true, -20, false, true, 52));
    assert(!wifi_tx_policy_eligible(204, true, -20, true, true, 52));
    assert(!wifi_tx_policy_eligible(204, true, -20, false, false, 52));
    assert(!wifi_tx_policy_eligible(204, true, -20, false, true, 28));
    assert(!wifi_tx_policy_eligible(204, true, -20, false, true, 0));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="wifi-tx-policy-") as directory:
            c_file = Path(directory) / "policy.c"
            executable = Path(directory) / "policy"
            c_file.write_text(source)
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                            str(ROOT / "main/wifi_tx_policy.c"), str(c_file),
                            "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
