"""Focused real-allocator proof, ESP-IDF v5.3 sources only (no SDK installation).

Prepare the small pinned source bundle outside a budget lease:
  /usr/bin/python3 firmware/test/host/proto/run_idf_capacity.py --idf-root <scratch> --prepare
Run the actual allocator and production NVS adapter under one heavy lease:
  dev-budget run --kind heavy -- /usr/bin/python3 firmware/test/host/proto/run_idf_capacity.py --idf-root <scratch> --build-dir <scratch-build>
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--prepare", action="store_true")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    manifest = json.loads((tests / "idf_nvs_files.json").read_text())
    for name, hashes in manifest["files"].items():
        path = args.idf_root / name
        if args.prepare and not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            url = "https://raw.githubusercontent.com/espressif/esp-idf/" + manifest["commit"] + "/" + name
            with urllib.request.urlopen(url, timeout=30) as response:
                path.write_bytes(response.read())
        if hashlib.sha256(path.read_bytes()).hexdigest() != hashes["sha256"]:
            raise RuntimeError("pinned source hash mismatch: " + str(path))
    if args.prepare:
        print("Prepared exact ESP-IDF v5.3 NVS source subset:", manifest["commit"])
        return
    if args.build_dir is None:
        parser.error("--build-dir is required unless --prepare is used")
    root = args.source_root.resolve()
    config = (root / "firmware/sdkconfig.defaults").read_text()
    assert "CONFIG_PARTITION_TABLE_TWO_OTA=y" in config
    table = (args.idf_root / "components/partition_table/partitions_two_ota.csv").read_text()
    nvs = [line.split(",") for line in table.splitlines() if line.strip().startswith("nvs,")]
    assert len(nvs) == 1 and int(nvs[0][4].strip(), 0) == 4 * 4096
    assert "return ESP_OK;" in (args.idf_root / "components/nvs_flash/src/nvs_handle_simple.cpp").read_text().split("NVSHandleSimple::commit()", 1)[1].split("}", 1)[0]
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    stubs = build / "platform-boundary"
    stubs.mkdir(exist_ok=True)
    (stubs / "esp_log.h").write_text("#pragma once\n" + "\n".join(
        "#define ESP_LOG" + level + "(tag, ...) ((void)(tag))" for level in "EWIDV") + "\n")
    (stubs / "sdkconfig.h").write_text("#define CONFIG_IDF_TARGET_LINUX 1\n#define CONFIG_NVS_ASSERT_ERROR_CHECK 1\n")
    include_dirs = [stubs, root / "firmware/main", root / "firmware/components/safety_gate/include",
                    root / "firmware/components/feeder_util/include"]
    include_dirs += [args.idf_root / rel for rel in (
        "components/nvs_flash/src", "components/nvs_flash/private_include", "components/nvs_flash/include",
        "components/esp_common/include", "components/esp_rom/include", "components/esp_rom/include/linux",
        "components/spi_flash/include")]
    common = ["-g", "-DLINUX_TARGET", "-DLINUX_HOST_LEGACY_TEST", "-DNO_DEBUG_STORAGE"]
    common += ["-I" + str(path) for path in include_dirs]
    cpp_sources = [args.idf_root / "components/nvs_flash/src" / (name + ".cpp") for name in
                   ("nvs_storage", "nvs_page", "nvs_pagemanager", "nvs_item_hash_list", "nvs_types")]
    cpp_sources.append(tests / "test_idf_nvs_capacity.cpp")
    c_sources = [args.idf_root / "components/esp_rom/linux/esp_rom_crc.c",
                 root / "firmware/main/nv_store.c", root / "firmware/components/safety_gate/safety_gate.c"]
    objects = []
    for index, source in enumerate(c_sources + cpp_sources):
        target = build / (str(index) + ".o")
        command = ["/usr/bin/clang++", "-std=c++11"] if source.suffix == ".cpp" else ["/usr/bin/cc", "-std=c99"]
        subprocess.run(command + common + ["-c", str(source), "-o", str(target)], check=True)
        objects.append(str(target))
    binary = build / "idf_nvs_capacity"
    subprocess.run(["/usr/bin/clang++", *objects, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
