#!/usr/bin/env python3
"""Allow the pinned ESPHome BLE tracker to select its scanner address type."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path


HEADER_SETTER = "  void set_scan_active(bool scan_active) { scan_active_ = scan_active; }\n"
HEADER_PATCHED_SETTER = (
    HEADER_SETTER
    + "  void set_scan_own_address_type(esp_ble_addr_type_t address_type) { scan_own_address_type_ = address_type; }\n"
)
HEADER_MEMBER = "  ScannerState scanner_state_{ScannerState::IDLE};\n"
HEADER_PATCHED_MEMBER = (
    HEADER_MEMBER
    + "  esp_ble_addr_type_t scan_own_address_type_{BLE_ADDR_TYPE_PUBLIC};\n"
)
CPP_ADDRESS = "  this->scan_params_.own_addr_type = BLE_ADDR_TYPE_PUBLIC;\n"
CPP_PATCHED_ADDRESS = "  this->scan_params_.own_addr_type = this->scan_own_address_type_;\n"


def locate_sources() -> tuple[Path, Path]:
    spec = importlib.util.find_spec("esphome")
    if spec is None or spec.origin is None:
        raise SystemExit("ERROR: ESPHome Python package was not found")
    directory = Path(spec.origin).resolve().parent / "components/esp32_ble_tracker"
    return directory / "esp32_ble_tracker.cpp", directory / "esp32_ble_tracker.h"


def patch_source(path: Path, original: str, patched: str) -> bool:
    source = path.read_text(encoding="utf-8")
    if patched in source:
        return False
    if original not in source:
        raise SystemExit(
            "ERROR: unsupported ESPHome BLE tracker implementation; expected "
            f"ESPHome 2026.7.3 layout in {path}"
        )
    path.write_text(source.replace(original, patched, 1), encoding="utf-8")
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, help="explicit esp32_ble_tracker.cpp test path")
    args = parser.parse_args()
    if args.source:
        cpp_path = args.source.resolve()
        header_path = cpp_path.with_suffix(".h")
    else:
        cpp_path, header_path = locate_sources()

    changed = patch_source(cpp_path, CPP_ADDRESS, CPP_PATCHED_ADDRESS)
    changed = patch_source(header_path, HEADER_SETTER, HEADER_PATCHED_SETTER) or changed
    changed = patch_source(header_path, HEADER_MEMBER, HEADER_PATCHED_MEMBER) or changed
    action = "applied" if changed else "already applied"
    print(f"OK: ESPHome BLE scanner address patch {action}: {cpp_path.parent}")


if __name__ == "__main__":
    main()
