# Browser flashing

The installer runs in desktop Chrome or Edge over HTTPS (localhost also works). Its **Install** action asks for a Web Serial port, downloads the three published ESP32-S3 firmware images, checks their lengths and SHA-256 hashes, detects the attached chip and flash capacity, and writes four regions. **Update** writes only the application image. The site uses [Espressif esptool-js 0.7.0](https://github.com/espressif/esptool-js/tree/v0.7.0); no flashing service or account is involved.

| Region | Address | Install | Update |
| --- | ---: | :---: | :---: |
| Bootloader | `0x000000` | Yes | No |
| Partition table | `0x008000` | Yes | No |
| Application | `0x010000` | Yes | Yes |
| One-shot settings | `0x750000` | Yes | No |

The existing `nvs`, `quota_accounts`, and `calendar_cache` partitions are preserved. The firmware consumes the settings sector on first boot. Installing on a box with existing saved Wi-Fi or Claude sign-in does **not** wipe those records. A factory reset is a separate, explicit action on the device.

The settings image is 4,096 bytes: `HBX1`, little-endian payload length 108, CRC32/IEEE of the 108-byte body, then the body. Unused bytes are `0xFF`. The body has language, view, brightness, dashboard timeout, refresh interval, time zone, flags, Wi-Fi-present byte, a 33-byte zero-terminated SSID field, and a 65-byte zero-terminated password field. The SSID is UTF-8 and limited to 32 bytes. A password is kept byte-for-byte, never trimmed. Empty passwords are accepted for open networks; secured networks use 8–63 printable ASCII bytes or 64 hex digits.

Wi-Fi credentials remain local to the browser and USB connection. After a successful install the password input is cleared and hidden; a failed operation retains the input for an explicit retry. The temporary encoded settings buffer is wiped when the operation ends. Credentials are not placed in a URL, local storage, analytics, or a server request. Only static firmware assets are fetched. Esptool verifies each written image with an MD5 readback from the chip. The result reports that flash verification passed; it does **not** claim that the box joined Wi-Fi or completed Claude sign-in.

The published manifest is `/firmware/manifest.json`. It declares the ESP32-S3 family, 8 MB flash, fixed settings layout, and SHA-256 for each image. The release preparation script generates that manifest and images from the firmware build. If an image is missing, corrupt, too large for its partition, or for a different chip, the installer stops before writing. If connection fails, close serial monitors and retry, holding **BOOT** while tapping **RESET** when necessary. Keep the USB cable attached until the write and verification finish.

Live hardware flashing still needs a test on a SenseCAP Indicator D1. The automated tests cover the config bytes and firmware manifest checks; they cannot establish USB reset timing, panel behavior, or Wi-Fi association.
