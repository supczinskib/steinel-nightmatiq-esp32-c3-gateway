#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 1 ]] || { echo "Usage: $0 IP_OR_HOSTNAME"; exit 1; }

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib.sh
source "$ROOT_DIR/scripts/lib.sh"
TARGET="$1"
ESPHOME_BIN="$(find_esphome)"
patch_esphome "$ESPHOME_BIN"
require_secrets "$ROOT_DIR"

cd "$ROOT_DIR/esphome"
"$ESPHOME_BIN" clean nightmatiq-c3.yaml
"$ESPHOME_BIN" compile nightmatiq-c3.yaml
"$ESPHOME_BIN" upload nightmatiq-c3.yaml --device "$TARGET"
