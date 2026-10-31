"""Produce a fresh credential-free installer build and bind it to public inputs."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-web-release"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest():
    paths = [ROOT / name for name in (
        "CMakeLists.txt", "main/CMakeLists.txt", "partitions.csv",
        "sdkconfig", "sdkconfig.defaults", "scripts/idf.sh",
        "scripts/build_public_firmware.py",
    )]
    paths += list((ROOT / "main").glob("*.c"))
    paths += list((ROOT / "main").glob("*.h"))
    entries = sorted({str(path.relative_to(ROOT)): digest(path) for path in paths}.items())
    return hashlib.sha256("".join(f"{name}\0{value}\n" for name, value in entries).encode()).hexdigest()


def require_public_config(path):
    content = path.read_text()
    for option in ("HELLO_BOX_USB_TEST_HELPER", "CLAUDE_DISPLAY_DIAGNOSTIC"):
        if f"CONFIG_{option}=y" in content:
            raise SystemExit(f"Public firmware cannot enable {option}")
    if 'CONFIG_IDF_TARGET="esp32s3"' not in content:
        raise SystemExit("Public firmware must target ESP32-S3")


def main():
    if (ROOT / "main/provision.h").exists():
        raise SystemExit("Remove private main/provision.h before building a public release")
    require_public_config(ROOT / "sdkconfig")
    before = source_digest()
    if BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir()
    config = BUILD / "sdkconfig"
    shutil.copyfile(ROOT / "sdkconfig", config)
    subprocess.run([
        "bash", str(ROOT / "scripts/idf.sh"), "-B", str(BUILD),
        "-D", f"SDKCONFIG={config}", "build",
    ], cwd=ROOT, check=True)
    if (ROOT / "main/provision.h").exists() or source_digest() != before:
        raise SystemExit("Firmware inputs changed during the release build; rebuild after edits finish")
    require_public_config(config)
    project = json.loads((BUILD / "project_description.json").read_text())
    if project.get("target") != "esp32s3":
        raise SystemExit("Unexpected firmware target")
    app = BUILD / "sensecap_oauth_probe.bin"
    if b"HELLO_BOX_SETUP_INFO" in app.read_bytes():
        raise SystemExit("Debug helper found in public firmware")
    receipt = {
        "clean_build": True, "target": "esp32s3",
        "version": project["project_version"], "public_source_sha256": before,
        "effective_sdkconfig_sha256": digest(config),
        "images": {name: digest(BUILD / name) for name in (
            "sensecap_oauth_probe.bin", "bootloader/bootloader.bin",
            "partition_table/partition-table.bin",
        )},
    }
    (BUILD / "public-release.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Fresh public firmware build verified: {receipt['version']}")


if __name__ == "__main__":
    main()
