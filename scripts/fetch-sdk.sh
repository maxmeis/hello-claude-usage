#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk_revision=77edb8d2b9a92fc67965c1b2d4a838f0d09a1800
if [ ! -d .deps/sensecap-sdk/.git ]; then
  git clone https://github.com/Seeed-Solution/SenseCAP_Indicator_ESP32.git .deps/sensecap-sdk
fi
git -C .deps/sensecap-sdk checkout --detach "$sdk_revision"
mdns_revision=5c4650d0ab6f9fde165ae1e3c1ef16714be0a619
if [ ! -d .deps/esp-protocols/.git ]; then
  git clone https://github.com/espressif/esp-protocols.git .deps/esp-protocols
fi
git -C .deps/esp-protocols checkout --detach "$mdns_revision"
