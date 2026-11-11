"""Focused actual fclock/gate/build_state proof; no host clock or device is changed.

Prepare three pinned SDK interface/default-hook source files without a lease,
then compile/run under dev-budget heavy. The weak SDK hook is compiled without
editing its body; the strong production hook must actually override it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--prepare", action="store_true")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    manifest = json.loads((tests / "clock_sdk_files.json").read_text())
    for name, sha in manifest["files"].items():
        path = args.sdk_root / name
        if args.prepare and not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            url = "https://raw.githubusercontent.com/espressif/esp-idf/" + manifest["commit"] + "/" + name
            with urllib.request.urlopen(url, timeout=30) as response: path.write_bytes(response.read())
        if hashlib.sha256(path.read_bytes()).hexdigest() != sha: raise RuntimeError("SDK source hash mismatch: " + name)
    if args.prepare:
        print("Prepared clock hook interface:", manifest["commit"])
        return
    if args.build_dir is None: parser.error("--build-dir required")
    root = args.source_root.resolve()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    stubs = build / "sdk-boundary"
    (stubs / "freertos").mkdir(parents=True, exist_ok=True)
    (stubs / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n")
    (stubs / "esp_log.h").write_text("#pragma once\n" + "\n".join("#define ESP_LOG" + x + "(tag, ...) ((void)(tag))" for x in "EWIDV") + "\n")
    (stubs / "esp_timer.h").write_text("#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
    (stubs / "sdkconfig.h").write_text('#define CONFIG_FEEDER_TZ "UTC0"\n#define CONFIG_FEEDER_NTP_SERVER "test.invalid"\n#define CONFIG_LWIP_SNTP_UPDATE_DELAY 3600000\n')
    (stubs / "freertos/FreeRTOS.h").write_text("""#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
void clock_test_enter(pthread_mutex_t *);
void clock_test_exit(pthread_mutex_t *);
#define portENTER_CRITICAL(lock) clock_test_enter(lock)
#define portEXIT_CRITICAL(lock) clock_test_exit(lock)
""")
    (stubs / "esp_sntp.h").write_text("""#pragma once
#include <stdint.h>
#include <sys/time.h>
#define ESP_SNTP_OPMODE_POLL 0
#define SNTP_SYNC_MODE_IMMED 0
#define SNTP_SYNC_MODE_SMOOTH 1
typedef enum {SNTP_SYNC_STATUS_RESET, SNTP_SYNC_STATUS_COMPLETED, SNTP_SYNC_STATUS_IN_PROGRESS} sntp_sync_status_t;
void sntp_sync_time(struct timeval *);
void sntp_set_sync_interval(uint32_t);
void sntp_set_sync_status(sntp_sync_status_t);
void sntp_set_time_sync_notification_cb(void (*)(struct timeval *));
#define esp_sntp_set_sync_interval sntp_set_sync_interval
#define esp_sntp_set_sync_status sntp_set_sync_status
#define esp_sntp_set_time_sync_notification_cb sntp_set_time_sync_notification_cb
void esp_sntp_setoperatingmode(int);
void esp_sntp_setservername(int, const char *);
void esp_sntp_init(void);
""")
    sdk = (args.sdk_root / "components/lwip/apps/sntp/sntp.c").read_text()
    start = sdk.index("void __attribute__((weak)) sntp_sync_time(")
    end = sdk.index("\n}\n", start) + 3
    prefix = """#include <sys/time.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_sntp.h"
#include "esp_log.h"
static const char *TAG = "sdk-clock-boundary";
static int sntp_sync_mode = SNTP_SYNC_MODE_IMMED;
static void (*time_sync_notification_cb)(struct timeval *);
void sntp_set_time_sync_notification_cb(void (*fn)(struct timeval *)) { time_sync_notification_cb = fn; }
"""
    weak = build / "sdk_weak_hook.c"
    weak.write_text(prefix + sdk[start:end])
    extract = runpy.run_path(str(tests / "run_regressions.py"))["function"]
    fixture = (tests / "test_clock_filter.c").read_text().replace(
        "/* @BUILD_STATE@ */", extract((root / "firmware/main/feeder.c").read_text(), "build_state"))
    generated = build / "clock_host.c"
    generated.write_text(fixture)
    includes = [stubs, tests.parent, root / "firmware/main", root / "firmware/components/safety_gate/include",
                root / "firmware/components/feeder_util/include"]
    sources = [generated, weak, root / "firmware/main/fclock.c", root / "firmware/components/safety_gate/safety_gate.c",
               root / "firmware/components/feeder_util/rfc3339.c"]
    command = ["/usr/bin/cc", "-std=c99", "-D_DARWIN_C_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-pthread",
               "-Dsettimeofday=clock_test_settimeofday", "-Dgettimeofday=clock_test_gettimeofday",
               "-Dadjtime=clock_test_adjtime", "-Dtime=clock_test_time"]
    command += ["-I" + str(path) for path in includes] + list(map(str, sources)) + ["-o", str(build / "clock_filter")]
    subprocess.run(command, check=True)
    env = dict(os.environ, FEEDER_CLOCK_STATE_FIXTURE=str(build / "saved_gate.bin"))
    failures = []
    for case in ("initial", "outlier", "reboot", "concurrent"):
        result = subprocess.run([str(build / "clock_filter"), case], cwd=root, env=env)
        if result.returncode: failures.append(case)
    if failures: raise SystemExit("failed clock cases: " + ", ".join(failures))


if __name__ == "__main__":
    main()
