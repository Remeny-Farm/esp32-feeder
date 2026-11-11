"""Focused host proof against actual firmware sources; cJSON source is supplied externally.

/usr/bin/python3 firmware/test/host/proto/run_regressions.py --cjson-dir <cJSON-1.7.18> --build-dir <scratch> --case publish
No ESP SDK, broker, certificates, motor backend or device connection is used.
"""
import argparse
from pathlib import Path
import re
import subprocess


def function(source, name):
    """Keep original C function bytes, excluding the SDK-dependent entrypoint imports."""
    match = re.search(r"(?m)^static [^;\n]+\b" + name + r"\([^;]*?\)\s*\{", source)
    if match is None:
        raise ValueError("function not found: " + name)
    # Mask literals and comments at equal length so brace offsets stay exact.
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                    lambda m: " " * len(m.group()), source, flags=re.S)
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--case", choices=("replay", "protocol", "publish"), required=True)
    args = parser.parse_args()
    root = args.source_root.resolve()
    tests = Path(__file__).resolve().parent
    args.build_dir.mkdir(parents=True, exist_ok=True)
    main_dir = root / "firmware/main"
    components = root / "firmware/components"
    common = ["/usr/bin/cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
              "-Wno-unused-function", "-I" + str(tests.parent), "-I" + str(main_dir),
              "-I" + str(components / "safety_gate/include"),
              "-I" + str(components / "feeder_util/include"), "-I" + str(args.cjson_dir)]
    gate = str(components / "safety_gate/safety_gate.c")
    proto = [str(main_dir / "proto.c"), str(components / "feeder_util/rfc3339.c"),
             str(args.cjson_dir / "cJSON.c")]
    failures = []
    def build_run(name, sources, arguments=()):
        binary = args.build_dir / name
        subprocess.run(common + list(map(str, sources)) + ["-o", str(binary)], check=True)
        result = subprocess.run([str(binary), *arguments], cwd=root)
        if result.returncode:
            failures.append(name)
    # Minimal ESP/NVS declarations; compile the actual adapter, not a port.
    stubs = args.build_dir / "sdk-stubs"
    stubs.mkdir(exist_ok=True)
    (stubs / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NVS_NOT_FOUND 1\n#define ESP_ERR_NVS_INVALID_LENGTH 2\n")
    (stubs / "esp_log.h").write_text("#pragma once\n#define ESP_LOGW(tag, ...) ((void)(tag))\n")
    (stubs / "nvs.h").write_text("#pragma once\n#include <stddef.h>\n#include \"esp_err.h\"\n"
                               "typedef int nvs_handle_t;\n#define NVS_READONLY 0\n#define NVS_READWRITE 1\n"
                               "esp_err_t nvs_open(const char *, int, nvs_handle_t *);\n"
                               "void nvs_close(nvs_handle_t);\nesp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);\n"
                               "esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);\n"
                               "esp_err_t nvs_commit(nvs_handle_t);\n")
    common.append("-I" + str(stubs))
    if args.case == "replay":
        build_run("replay", [tests.parent / "test_replay_capacity.c", gate])
        build_run("gate", [tests.parent / "test_gate.c", gate])
        build_run("nv_replay", [tests / "test_nv_replay.c", main_dir / "nv_store.c", gate])
    if args.case == "protocol":
        build_run("fields", [tests / "test_proto_fields.c", gate, *proto])
    source = (main_dir / "feeder.c").read_text()
    names = ("persist_gate", "send_json", "send_ack", "state_differs", "publish_state",
             "handle_cmd", "dispatch", "feeder_task")
    harness = (tests / "feeder_publish_harness.c.in").read_text().replace(
        "/* @PRODUCTION_FUNCTIONS@ */", "\n\n".join(function(source, name) for name in names))
    generated = args.build_dir / "feeder_host.c"
    generated.write_text(harness)
    build_run("feeder_host", [generated, gate, *proto], [args.case])
    if failures:
        raise SystemExit("failed: " + ", ".join(failures))


if __name__ == "__main__":
    main()
