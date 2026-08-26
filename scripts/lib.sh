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
