"""Display selection of an actually reported Codex weekly quota."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class UsageLayoutTests(unittest.TestCase):
    def test_weekly_window_selection_uses_only_valid_reported_data(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.skipTest("clang required for production C test")
        harness = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include "usage_provider.h"

static provider_usage_t sample(void) {
    provider_usage_t u = {.configured=true, .has_sample=true,
                          .sampled_at=1760000000, .sampled_us=1000000};
    u.windows[0] = (quota_window_t){.valid=true, .used_percent=20,
                                    .resets_at=1760100000, .window_seconds=18000};
    u.windows[1] = (quota_window_t){.valid=true, .used_percent=40,
                                    .resets_at=1760600000, .window_seconds=604800};
    return u;
}

int main(void) {
    assert(!usage_provider_weekly_window(NULL));
    provider_usage_t u = sample(), before = u;
    assert(usage_provider_weekly_window(&u)==&u.windows[1]);
    assert(!memcmp(&u,&before,sizeof(u)));

    u.windows[0] = u.windows[1];
    u.windows[1].window_seconds = 86400;
    before = u;
    assert(usage_provider_weekly_window(&u)==&u.windows[0]);
    assert(!memcmp(&u,&before,sizeof(u)));

    u = sample(); u.windows[0].window_seconds=604800;
    u.windows[0].used_percent=NAN;
    assert(usage_provider_weekly_window(&u)==&u.windows[1]);
    u.windows[0].used_percent=-1;
    assert(usage_provider_weekly_window(&u)==&u.windows[1]);
    u.windows[0].used_percent=101;
    assert(usage_provider_weekly_window(&u)==&u.windows[1]);
    u.windows[0].used_percent=50; u.windows[0].valid=false;
    assert(usage_provider_weekly_window(&u)==&u.windows[1]);

    u = sample(); u.windows[1].window_seconds=86400;
    assert(!usage_provider_weekly_window(&u)); /* 5h/24h are not weekly. */
    u.windows[1].window_seconds=604800;
    u.windows[1].resets_at=-1;
    assert(!usage_provider_weekly_window(&u));
    u.windows[1].resets_at=1;
    u.windows[1].used_percent=INFINITY;
    assert(!usage_provider_weekly_window(&u));
    u.windows[1].used_percent=100.01f;
    assert(!usage_provider_weekly_window(&u));
    u.windows[1].used_percent=0;
    assert(usage_provider_weekly_window(&u)==&u.windows[1]);
    u.windows[1].used_percent=100;
    assert(usage_provider_weekly_window(&u)==&u.windows[1]);
    u.configured=false;
    assert(!usage_provider_weekly_window(&u));
    u.configured=true; u.has_sample=false;
    assert(!usage_provider_weekly_window(&u));
    u.has_sample=true; u.sampled_at=-1;
    assert(!usage_provider_weekly_window(&u));
    u.sampled_at=1; u.sampled_us=-1;
    assert(!usage_provider_weekly_window(&u));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="weekly-layout-") as directory:
            source = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            source.write_text(harness)
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                 str(source), str(ROOT / "main/usage_provider.c"), "-lm",
                 "-o", str(binary)], capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
