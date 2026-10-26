#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk_revision=77edb8d2b9a92fc67965c1b2d4a838f0d09a1800
if [ ! -d .deps/sensecap-sdk/.git ]; then
  git clone https://github.com/Seeed-Solution/SenseCAP_Indicator_ESP32.git .deps/sensecap-sdk
fi
git -C .deps/sensecap-sdk checkout --detach "$sdk_revision"
