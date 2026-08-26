#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 1 ]] || { echo "Usage: $0 IP_OR_HOSTNAME"; exit 1; }

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib.sh
source "$ROOT_DIR/scripts/lib.sh"
TARGET="$1"
ESPHOME_BIN="$(find_esphome)"

cd "$ROOT_DIR/esphome"
"$ESPHOME_BIN" clean nightmatiq-c3.yaml
"$ESPHOME_BIN" compile nightmatiq-c3.yaml

FIRMWARE="$(find .esphome/build -type f -name firmware.ota.bin -print -quit)"
[[ -n "$FIRMWARE" && -f "$FIRMWARE" ]] || {
  echo 'Compiled firmware.ota.bin was not found.' >&2
  exit 1
}

if [[ "$TARGET" == http://* || "$TARGET" == https://* ]]; then
  BASE_URL="${TARGET%/}"
else
  BASE_URL="http://${TARGET%/}"
fi

echo 'Enter the gateway administrator password when curl asks for it.'
curl --fail --show-error --digest --user admin \
  --form "update=@${FIRMWARE};type=application/octet-stream" \
  "$BASE_URL/update"
