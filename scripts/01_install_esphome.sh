#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "Run as root: sudo bash $0"
  exit 1
fi

apt-get update
apt-get install -y python3 python3-venv python3-pip git curl build-essential libffi-dev libssl-dev openssl

python3 - <<'PY'
import sys
if not ((3, 12) <= sys.version_info[:2] < (3, 15)):
    raise SystemExit(f"Python >=3.12 and <3.15 is required; detected {sys.version.split()[0]}")
print("Python OK:", sys.version.split()[0])
PY

VENV=/opt/esphome-nightmatiq
python3 -m venv --clear "$VENV"
"$VENV/bin/python" -m pip install --upgrade pip wheel
"$VENV/bin/python" -m pip install --upgrade "esphome==2026.7.3"
"$VENV/bin/esphome" version
