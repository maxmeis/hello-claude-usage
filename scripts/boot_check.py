"""Reset the flashed ESP and show only known firmware diagnostics, never QR URLs."""
import argparse
import re
import time
import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbserial-110")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--no-reset", action="store_true", help="Watch filtered diagnostics without restarting the monitor")
    args = parser.parse_args()
    allowed = re.compile(r"(?:probe_app|oauth_probe|wifi_manager|display_ui|display_check|indev_tp|app_init|cpu_start|bsp boards|board sensecap|bsp_lcd|esp_psram):|Guru Meditation|assert failed|Brownout")
    ansi = re.compile(r"\x1b\[[0-9;]*m")
    with serial.Serial(args.port, 115200, timeout=0.5, dsrdtr=False, rtscts=False) as port:
        if not args.no_reset:
            port.dtr = False
            port.rts = True
            time.sleep(0.15)
            port.rts = False
        end = time.monotonic() + args.seconds
        pending = b""
        while time.monotonic() < end:
            pending += port.read(1024)
            lines = pending.split(b"\n")
            pending = lines.pop()[-4096:]
            for raw in lines:
                line = ansi.sub("", raw.decode("utf-8", "replace")).strip()
                if allowed.search(line) and not any(word in line.lower() for word in
                       ("bearer", "access_token", "refresh_token", "http://", "/s/")):
                    print(line, flush=True)


if __name__ == "__main__":
    main()
