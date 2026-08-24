#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SECRETS="$ROOT_DIR/esphome/secrets.yaml"

read_existing() {
  local key="$1"
  python3 - "$SECRETS" "$key" <<'PY'
from pathlib import Path
import json
import sys

path = Path(sys.argv[1])
key = sys.argv[2]
if not path.exists():
    print("")
    raise SystemExit

for raw in path.read_text(encoding="utf-8").splitlines():
    line = raw.strip()
    if not line or line.startswith("#") or ":" not in line:
        continue
    k, value = line.split(":", 1)
    if k.strip() != key:
        continue
    value = value.strip()
    try:
        parsed = json.loads(value)
    except Exception:
        parsed = value.strip('"\'')
    print(parsed)
    break
else:
    print("")
PY
}

OLD_WIFI_SSID="$(read_existing wifi_ssid)"
OLD_WIFI_PASSWORD="$(read_existing wifi_password)"
OLD_OTA_PASSWORD="$(read_existing ota_password)"
OLD_WEB_USERNAME="$(read_existing web_server_username)"
OLD_WEB_PASSWORD="$(read_existing web_server_password)"

DEFAULT_SSID="${OLD_WIFI_SSID:-}"
if [[ -n "$DEFAULT_SSID" ]]; then
  read -r -p "Wi-Fi SSID [$DEFAULT_SSID]: " WIFI_SSID
  WIFI_SSID="${WIFI_SSID:-$DEFAULT_SSID}"
else
  read -r -p "Wi-Fi SSID: " WIFI_SSID
  if [[ -z "$WIFI_SSID" ]]; then
    echo 'Wi-Fi SSID cannot be empty.' >&2
    exit 1
  fi
fi

if [[ -n "$OLD_WIFI_PASSWORD" ]]; then
  read -r -s -p 'Wi-Fi password [Enter keeps the current value]: ' WIFI_PASSWORD
  echo
  WIFI_PASSWORD="${WIFI_PASSWORD:-$OLD_WIFI_PASSWORD}"
else
  read -r -s -p 'Wi-Fi password: ' WIFI_PASSWORD
  echo
  if [[ -z "$WIFI_PASSWORD" ]]; then
    echo 'Wi-Fi password cannot be empty.' >&2
    exit 1
  fi
fi

OTA_PASSWORD="${OLD_OTA_PASSWORD:-$(openssl rand -hex 16)}"

DEFAULT_WEB_USERNAME="${OLD_WEB_USERNAME:-admin}"
read -r -p "Web interface username [$DEFAULT_WEB_USERNAME]: " WEB_USERNAME
WEB_USERNAME="${WEB_USERNAME:-$DEFAULT_WEB_USERNAME}"
if [[ -z "$WEB_USERNAME" ]]; then
  echo 'Web interface username cannot be empty.' >&2
  exit 1
fi

while true; do
  REUSING_WEB_PASSWORD=false
  if [[ -n "$OLD_WEB_PASSWORD" ]]; then
    read -r -s -p 'Web interface password [Enter keeps the current value]: ' WEB_PASSWORD
    echo
    if [[ -z "$WEB_PASSWORD" ]]; then
      WEB_PASSWORD="$OLD_WEB_PASSWORD"
      REUSING_WEB_PASSWORD=true
    fi
  else
    read -r -s -p 'Web interface password (12-63 characters): ' WEB_PASSWORD
    echo
  fi

  if (( ${#WEB_PASSWORD} < 12 || ${#WEB_PASSWORD} > 63 )); then
    echo 'The web interface password must contain 12-63 characters.' >&2
    continue
  fi

  if [[ "$REUSING_WEB_PASSWORD" == true ]]; then
    break
  fi

  read -r -s -p 'Repeat the web interface password: ' WEB_PASSWORD_2
  echo
  if [[ "$WEB_PASSWORD" != "$WEB_PASSWORD_2" ]]; then
    echo 'The web interface passwords do not match.' >&2
    continue
  fi
  break
done

FALLBACK_PASSWORD_SEPARATE=false
while true; do
  read -r -s -p 'Fallback AP password [Enter = web password]: ' FALLBACK_PASSWORD
  echo
  if [[ -z "$FALLBACK_PASSWORD" ]]; then
    FALLBACK_PASSWORD="$WEB_PASSWORD"
    break
  fi

  if (( ${#FALLBACK_PASSWORD} < 8 || ${#FALLBACK_PASSWORD} > 63 )); then
    echo 'A separate fallback AP password must contain 8-63 characters.' >&2
    continue
  fi

  read -r -s -p 'Repeat the fallback AP password: ' FALLBACK_PASSWORD_2
  echo
  if [[ "$FALLBACK_PASSWORD" != "$FALLBACK_PASSWORD_2" ]]; then
    echo 'The fallback AP passwords do not match.' >&2
    continue
  fi
  FALLBACK_PASSWORD_SEPARATE=true
  break
done

python3 - "$SECRETS" \
  "$WIFI_SSID" "$WIFI_PASSWORD" "$OTA_PASSWORD" "$FALLBACK_PASSWORD" \
  "$WEB_USERNAME" "$WEB_PASSWORD" <<'PY'
from pathlib import Path
import json
import sys

p = Path(sys.argv[1])

def q(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)

p.write_text(
    f"wifi_ssid: {q(sys.argv[2])}\n"
    f"wifi_password: {q(sys.argv[3])}\n"
    f"ota_password: {q(sys.argv[4])}\n"
    f"fallback_ap_password: {q(sys.argv[5])}\n"
    f"web_server_username: {q(sys.argv[6])}\n"
    f"web_server_password: {q(sys.argv[7])}\n",
    encoding="utf-8",
)
p.chmod(0o600)
PY

unset WIFI_PASSWORD OLD_WIFI_PASSWORD WEB_PASSWORD WEB_PASSWORD_2 OLD_WEB_PASSWORD REUSING_WEB_PASSWORD
unset FALLBACK_PASSWORD FALLBACK_PASSWORD_2

echo "Created or updated: $SECRETS"
echo 'The OTA password was generated or preserved.'
if [[ "$FALLBACK_PASSWORD_SEPARATE" == true ]]; then
  echo 'The fallback AP uses a separate password.'
else
  echo 'The fallback AP uses the web interface password.'
fi
echo 'The web interface uses HTTP Digest authentication.'
