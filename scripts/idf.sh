#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export IDF_PATH="${IDF_PATH:-/Users/max/.platformio/packages/framework-espidf}"
# Reuse the installed tools; do not modify global installations.
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-/Users/max/.espressif/python_env/idf5.1_py3.9_env}"
export PYTHONPATH="$PWD/.deps/python${PYTHONPATH:+:$PYTHONPATH}"
export IDF_COMPONENT_MANAGER=0
export PATH="$IDF_PYTHON_ENV_PATH/bin:/Users/max/.platformio/packages/tool-cmake/bin:/Users/max/.platformio/packages/tool-ninja:/Users/max/.espressif/tools/xtensa-esp32s3-elf/esp-12.2.0_20230208/xtensa-esp32s3-elf/bin:$PATH"
exec "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"
