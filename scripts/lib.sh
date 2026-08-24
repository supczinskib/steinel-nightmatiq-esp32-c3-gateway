#!/usr/bin/env bash

project_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

find_esphome() {
  local candidate="${ESPHOME:-/opt/esphome-nightmatiq/bin/esphome}"
  if [[ -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  candidate="$(command -v esphome 2>/dev/null || true)"
  if [[ -n "$candidate" && -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  echo "ERROR: ESPHome was not found. Run scripts/01_install_esphome.sh" >&2
  return 1
}

require_secrets() {
  local root="$1"
  local secrets="$root/esphome/secrets.yaml"
  if [[ ! -f "$secrets" ]]; then
    echo "ERROR: esphome/secrets.yaml is missing. Run scripts/02_configure_secrets.sh" >&2
    return 1
  fi

  local key
  for key in wifi_ssid wifi_password ota_password fallback_ap_password web_server_username web_server_password; do
    if ! grep -Eq "^[[:space:]]*${key}:" "$secrets"; then
      echo "ERROR: required secret is missing: $key" >&2
      return 1
    fi
  done
}

patch_esphome() {
  local esphome_bin="$1"
  local venv_python
  venv_python="$(dirname "$esphome_bin")/python"
  if [[ ! -x "$venv_python" ]]; then
    echo "ERROR: cannot locate the Python interpreter belonging to $esphome_bin" >&2
    return 1
  fi
  "$venv_python" "$(project_root)/scripts/patch_esphome_api.py"
  "$venv_python" "$(project_root)/scripts/patch_esphome_ble_tracker.py"
}
