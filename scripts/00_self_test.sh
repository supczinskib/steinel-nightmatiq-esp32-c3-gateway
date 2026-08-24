#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="$ROOT_DIR/esphome/nightmatiq-c3.yaml"
FAIL=0

ok() { printf 'OK: %s\n' "$1"; }
fail() { printf 'ERROR: %s\n' "$1" >&2; FAIL=$((FAIL + 1)); }

printf '%s\n' '=== Steinel NightmatIQ ESP32-C3 Gateway self-test ==='

for script in "$ROOT_DIR"/scripts/*.sh; do
  if bash -n "$script"; then
    ok "shell syntax: $(basename "$script")"
  else
    fail "invalid shell syntax: $(basename "$script")"
  fi
done

for path in \
  "$ROOT_DIR/README.md" \
  "$ROOT_DIR/README_PL.md" \
  "$ROOT_DIR/CHANGELOG.md" \
  "$CONFIG" \
  "$ROOT_DIR/esphome/secrets.example.yaml" \
  "$ROOT_DIR/esphome/components/nightmatiq_mesh/__init__.py" \
  "$ROOT_DIR/esphome/components/nightmatiq_mesh/nightmatiq_mesh.h" \
  "$ROOT_DIR/esphome/components/nightmatiq_mesh/nightmatiq_mesh.cpp" \
  "$ROOT_DIR/esphome/components/nightmatiq_mesh/nightmatiq_web.cpp" \
  "$ROOT_DIR/esphome/components/nightmatiq_mesh/nightmatiq_page.html" \
  "$ROOT_DIR/esphome/components/nightmatiq_mesh/nightmatiq_page.h" \
  "$ROOT_DIR/home-assistant/steinel-nightmatiq-package.yaml" \
  "$ROOT_DIR/home-assistant/steinel-nightmatiq-popup.js" \
  "$ROOT_DIR/docs/images/esp32-c3-super-mini.jpg" \
  "$ROOT_DIR/docs/images/nightmatiq-web-interface.png" \
  "$ROOT_DIR/docs/images/home-assistant-device.png" \
  "$ROOT_DIR/docs/images/home-assistant-control.png"; do
  if [[ -f "$path" ]]; then
    ok "required file: ${path#"$ROOT_DIR/"}"
  else
    fail "missing file: ${path#"$ROOT_DIR/"}"
  fi
done

for marker in \
  'project_version: "1.0.0"' \
  '## 1.0.0'; do
  if grep -Fq "$marker" "$CONFIG" "$ROOT_DIR/CHANGELOG.md"; then
    ok "release marker: $marker"
  else
    fail "missing release marker: $marker"
  fi
done

for private_path in \
  "$ROOT_DIR/esphome/secrets.yaml" \
  "$ROOT_DIR/START_HERE.md" \
  "$ROOT_DIR/PROJECT_HANDOFF.md"; do
  [[ ! -e "$private_path" ]] || fail "private file present in repository: ${private_path#"$ROOT_DIR/"}"
done

if find "$ROOT_DIR" -path "$ROOT_DIR/.git" -prune -o \
  \( -name '.DS_Store' -o -name '._*' -o -name 'Thumbs.db' -o \
     -name 'Desktop.ini' -o -name '*professional-ai*' \) -print -quit | grep -q .; then
  fail 'operating-system or generated work file present in repository'
else
  ok 'no operating-system or generated work files'
fi

if grep -REiq --exclude-dir=.git --exclude=00_self_test.sh \
  --exclude='*.png' --exclude='*.jpg' --exclude='*.jpeg' \
  'ChatGPT|OpenAI|Codex|[0-9]+\.[0-9]+\.[0-9]+-dev|10\.1\.0\.[0-9]+' "$ROOT_DIR"; then
  fail 'private or development marker present in public repository text'
else
  ok 'no private or development markers in public repository text'
fi

for marker in \
  'variant: esp32c3' \
  'CONFIG_BLE_MESH_PROVISIONER: y' \
  'CONFIG_BT_BLE_50_FEATURES_SUPPORTED: n' \
  'CONFIG_ESP_WIFI_SOFTAP_SUPPORT: y' \
  'ssid: "NightmatIQ Fallback"' \
  'password: !secret fallback_ap_password' \
  'ap_timeout: 90s' \
  'captive_portal:' \
  'mdns:' \
  'disabled: false' \
  'password: !secret ota_password' \
  'username: !secret web_server_username' \
  'password: !secret web_server_password'; do
  grep -Fq "$marker" "$CONFIG" || fail "missing configuration marker: $marker"
done

for forbidden in api_encryption_key manual_ip; do
  if grep -Rq --exclude-dir=.esphome --exclude=secrets.yaml "$forbidden" "$ROOT_DIR/esphome"; then
    fail "forbidden standalone configuration marker: $forbidden"
  fi
done

if grep -REq --exclude-dir=.esphome --exclude=secrets.yaml '10\.1\.0\.[0-9]+' "$ROOT_DIR/esphome" "$ROOT_DIR/scripts"; then
  fail 'a deployment IP address is hardcoded in firmware or scripts'
else
  ok 'no deployment IP address is hardcoded in firmware or scripts'
fi

if python3 - "$ROOT_DIR" <<'PY'
from __future__ import annotations

import ast
import gzip
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
errors: list[str] = []

for path in (root / "scripts").glob("*.py"):
    try:
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except SyntaxError as error:
        errors.append(f"invalid Python syntax in {path.name}: {error}")

html = (root / "esphome/components/nightmatiq_mesh/nightmatiq_page.html").read_bytes()
header = (root / "esphome/components/nightmatiq_mesh/nightmatiq_page.h").read_text(encoding="utf-8")
size_match = re.search(r"NIGHTMATIQ_PAGE_RAW_SIZE = (\d+);", header)
array_match = re.search(r"NIGHTMATIQ_PAGE_GZ\[\d+\] = \{(.*?)\};", header, re.DOTALL)
if size_match is None or array_match is None:
    errors.append("generated NightmatIQ page header has an invalid structure")
else:
    compressed = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", array_match.group(1)))
    try:
        embedded = gzip.decompress(compressed)
    except gzip.BadGzipFile as error:
        errors.append(f"generated NightmatIQ page is not valid gzip: {error}")
    else:
        if embedded != html:
            errors.append("generated NightmatIQ page header is stale")
        if int(size_match.group(1)) != len(html):
            errors.append("generated NightmatIQ raw page size is stale")

mesh = (root / "esphome/components/nightmatiq_mesh/nightmatiq_mesh.cpp").read_text(encoding="utf-8")
for marker in (
    "set_scan_own_address_type(BLE_ADDR_TYPE_RANDOM)",
    "set_scan_own_address_type(BLE_ADDR_TYPE_PUBLIC)",
    "ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET",
    "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET",
):
    if marker not in mesh:
        errors.append(f"missing NightmatIQ source marker: {marker}")

header_source = (root / "esphome/components/nightmatiq_mesh/nightmatiq_mesh.h").read_text(encoding="utf-8")
web_source = (root / "esphome/components/nightmatiq_mesh/nightmatiq_web.cpp").read_text(encoding="utf-8")
component_source = (root / "esphome/components/nightmatiq_mesh/__init__.py").read_text(encoding="utf-8")
config_source = (root / "esphome/nightmatiq-c3.yaml").read_text(encoding="utf-8")
page_source = html.decode("utf-8")
for source, marker in (
    (header_source, "ADDRESS_POOL_TARGET_SIZE = 2048"),
    (header_source, "AUTO_ADDRESS_ROTATION_LIMIT = 16"),
    (header_source, "struct StoredAddressConfirmation"),
    (mesh, "advance_address_recovery_(now)"),
    (mesh, "current_address_confirmed_()"),
    (web_source, "esp_read_mac(mac, ESP_MAC_WIFI_STA)"),
    (web_source, "address_policy.installation_nonce = esp_random()"),
    (web_source, "mesh_rx_messages_.load() == 0"),
    (web_source, "Confirmed local Mesh address"),
    (mesh, "context.recv_rssi"),
    (header_source, "mesh_rssi_received_"),
    (header_source, "mesh_rssi_publish_pending_"),
    (header_source, "set_rssi_sensor"),
    (mesh, "mesh_rssi_publish_pending_.store(true)"),
    (mesh, "rssi_sensor_->publish_state"),
    (component_source, 'CONF_RSSI_SENSOR_ID = "rssi_sensor_id"'),
    (component_source, "var.set_rssi_sensor(rssi)"),
    (config_source, 'friendly_name: "Steinel NightmatIQ Plus"'),
    (config_source, "rssi_sensor_id: nightmatiq_rssi"),
    (config_source, 'name: "NightmatIQ Signal Strength"'),
    (config_source, "device_class: signal_strength"),
    (config_source, 'name: "NightmatIQ Refresh"'),
    (config_source, "id(nightmatiq_gateway).request_refresh();"),
    (config_source, "optimistic: true"),
    (config_source, "restore_value: true"),
    (config_source, 'initial_option: "Auto"'),
    (web_source, "mesh_last_rssi_dbm"),
    (web_source, "mesh_last_rssi_age_seconds"),
    (page_source, "Last Mesh RSSI"),
    (page_source, "meshRssi"),
    (web_source, "retryable_transport_error"),
    (web_source, "http_status <= 0"),
    (web_source, "CLOUD_ERROR_REBOOT_DELAY_MS"),
    (web_source, "CLOUD_DISCOVER_SESSION_TIMEOUT_MS"),
    (web_source, "schedule_cloud_session_reboot_"),
    (mesh, "cloud_session_reboot_pending_"),
    (header_source, "web_refresh_pending_"),
    (mesh, "this->request_refresh()"),
    (web_source, 'url == "/steinel/mode"'),
    (web_source, 'url == "/steinel/threshold"'),
    (web_source, 'url == "/steinel/refresh"'),
    (web_source, "handle_mode_"),
    (web_source, "handle_threshold_"),
    (web_source, "handle_refresh_"),
    (web_source, r'\"mode_known\"'),
    (web_source, 'send_json_(request, 200, "{\\\"message\\\":\\\"Changing NightmatIQ mode\\\"}")'),
    (web_source, 'send_json_(request, 200, "{\\\"message\\\":\\\"Changing twilight threshold\\\"}")'),
    (web_source, 'send_json_(request, 200, "{\\\"message\\\":\\\"Refreshing NightmatIQ state\\\"}")'),
    (page_source, "NightmatIQ control"),
    (page_source, "modeControl"),
    (page_source, "thresholdControl"),
    (page_source, "refreshControl"),
):
    if marker not in source:
        errors.append(f"missing required source marker: {marker}")

if "\n  devices:\n" in config_source:
    errors.append("standalone C3 must expose NightmatIQ as its primary Home Assistant device")

for forbidden_gui_marker in (
    "Automatic address recovery",
    "Address pool:",
    "Initial Mesh address:",
    "Automatic / manual address changes:",
    "TRY ANOTHER MESH ADDRESS",
):
    if forbidden_gui_marker in page_source:
        errors.append(f"internal Mesh address detail leaked into GUI: {forbidden_gui_marker}")

for forbidden_source_marker in (
    'url == "/steinel/rotate-address"',
    r'body.append(",\"address_policy_valid\":")',
    "local_address_confirmed",
    "address_pool_low",
    "automatic_address_rotations",
    "manual_address_rotations",
):
    if forbidden_source_marker in web_source:
        errors.append(f"obsolete Mesh address interface remains public: {forbidden_source_marker}")

control_handlers = web_source.split("void NightmatiqMesh::handle_mode_", 1)[1].split(
    "bool NightmatiqMesh::cloud_get_", 1
)[0]
if "send_json_(request, 202" in control_handlers:
    errors.append("NightmatIQ web controls must use an HTTP status supported by web_server_idf")

if errors:
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    raise SystemExit(1)
print("OK: Python syntax and embedded NightmatIQ page")
PY
then
  ok 'NightmatIQ source checks'
else
  fail 'NightmatIQ source checks failed'
fi

if (( FAIL > 0 )); then
  echo "Self-test failed: $FAIL error(s)." >&2
  exit 1
fi

echo 'All standalone project self-tests passed.'
