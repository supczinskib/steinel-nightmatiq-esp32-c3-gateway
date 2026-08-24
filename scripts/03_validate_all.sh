#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib.sh
source "$ROOT_DIR/scripts/lib.sh"

"$ROOT_DIR/scripts/00_self_test.sh"
ESPHOME_BIN="$(find_esphome)"
patch_esphome "$ESPHOME_BIN"
require_secrets "$ROOT_DIR"

cd "$ROOT_DIR/esphome"
"$ESPHOME_BIN" config nightmatiq-c3.yaml
echo 'NightmatIQ ESPHome configuration passed validation.'
