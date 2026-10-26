"""Enter WLAN credentials locally; never pass a password as a CLI argument."""
import getpass
import json
import os
from pathlib import Path


def main():
    destination = Path(__file__).resolve().parents[1] / "main/provision.h"
    ssid = input("Wi-Fi SSID: ")
    password = getpass.getpass("Wi-Fi password (hidden): ")
    if not 1 <= len(ssid.encode()) <= 31 or len(password.encode()) > 63:
        raise SystemExit("SSID/password exceeds this probe's supported field size")
    if "\0" in ssid or "\0" in password:
        raise SystemExit("NUL characters are unsupported")
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as stream:
        stream.write("// Local provisioning. Ignored by Git. Do not share firmware binaries.\n")
        stream.write("#define PROVISION_WIFI_SSID " + json.dumps(ssid, ensure_ascii=False) + "\n")
        stream.write("#define PROVISION_WIFI_PASSWORD " + json.dumps(password, ensure_ascii=False) + "\n")
    print("Local provisioning saved. Rebuild and flash; credentials were not printed.")


if __name__ == "__main__":
    main()
