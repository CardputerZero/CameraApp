#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRESET="${PRESET:-cp0-cross}"
CONFIGURATION="${CONFIGURATION:-Release}"

cmake --preset "${PRESET}" "$@"
cmake --build --preset "${PRESET}-rel"
cmake --build "${ROOT_DIR}/build/${PRESET}" --config "${CONFIGURATION}" --target package
