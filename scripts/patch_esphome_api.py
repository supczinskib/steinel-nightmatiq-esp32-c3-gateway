#!/usr/bin/env python3
"""Patch ESPHome 2026.7.3 API overflow allocation for ESP-IDF.

ESPHome's API overflow queue uses C++ ``new`` in an ESP-IDF build where a
failed allocation aborts instead of being recoverable. Allocate this optional
TCP backlog with ``malloc`` so allocation failure can close only the congested
API client while the gateway and Bluetooth Mesh continue running.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path


CPP_ORIGINAL_INCLUDE = '#include <cstring>\n'
CPP_NOTHROW_INCLUDE = '#include <cstring>\n#include <new>\n'
CPP_PATCHED_INCLUDE = '#include <cstdlib>\n#include <cstring>\n'
CPP_ORIGINAL_ALLOCATION = (
    '  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)\n'
    '  auto *entry = new Entry{new uint8_t[buffer_size], buffer_size, 0};\n'
    '  this->queue_[this->tail_] = entry;\n'
)
CPP_NOTHROW_ALLOCATION = (
    '  // A failed throwing allocation aborts ESP-IDF because C++ exceptions\n'
    '  // are disabled. Let the existing caller close this congested API\n'
    '  // connection cleanly instead of restarting the entire gateway.\n'
    '  auto *data = new (std::nothrow) uint8_t[buffer_size];\n'
    '  if (data == nullptr)\n'
    '    return false;\n'
    '  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)\n'
    '  auto *entry = new (std::nothrow) Entry{data, buffer_size, 0};\n'
    '  if (entry == nullptr) {\n'
    '    delete[] data;\n'
    '    return false;\n'
    '  }\n'
    '  this->queue_[this->tail_] = entry;\n'
)
CPP_PATCHED_ALLOCATION = (
    '  // C allocation returns nullptr instead of invoking the disabled C++\n'
    '  // exception path that aborts ESP-IDF on a fragmented internal heap.\n'
    '  auto *data = static_cast<uint8_t *>(std::malloc(buffer_size));\n'
    '  if (data == nullptr)\n'
    '    return false;\n'
    '  auto *entry = static_cast<Entry *>(std::malloc(sizeof(Entry)));\n'
    '  if (entry == nullptr) {\n'
    '    std::free(data);\n'
    '    return false;\n'
    '  }\n'
    '  entry->data = data;\n'
    '  entry->size = buffer_size;\n'
    '  entry->offset = 0;\n'
    '  this->queue_[this->tail_] = entry;\n'
)

HEADER_ORIGINAL_INCLUDE = '#include <cstdint>\n'
HEADER_PATCHED_INCLUDE = '#include <cstdint>\n#include <cstdlib>\n'
HEADER_ORIGINAL_DESTROY = (
    '      delete[] entry->data;\n'
    '      delete entry;  // NOLINT(cppcoreguidelines-owning-memory)\n'
)
HEADER_PATCHED_DESTROY = (
    '      std::free(entry->data);\n'
    '      std::free(entry);\n'
)


def locate_sources() -> tuple[Path, Path]:
    spec = importlib.util.find_spec("esphome")
    if spec is None or spec.origin is None:
        raise SystemExit("ERROR: ESPHome Python package was not found")
    api_dir = Path(spec.origin).resolve().parent / "components/api"
    return api_dir / "api_overflow_buffer.cpp", api_dir / "api_overflow_buffer.h"


def read_source(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as err:
        raise SystemExit(f"ERROR: cannot read ESPHome API source {path}: {err}") from err


def write_source(path: Path, source: str) -> None:
    try:
        path.write_text(source, encoding="utf-8")
    except OSError as err:
        raise SystemExit(f"ERROR: cannot patch ESPHome API source {path}: {err}") from err


def patch_cpp(path: Path) -> bool:
    source = read_source(path)
    if CPP_PATCHED_ALLOCATION in source and CPP_PATCHED_INCLUDE in source:
        return False

    if CPP_NOTHROW_ALLOCATION in source and CPP_NOTHROW_INCLUDE in source:
        source = source.replace(CPP_NOTHROW_INCLUDE, CPP_PATCHED_INCLUDE, 1).replace(
            CPP_NOTHROW_ALLOCATION, CPP_PATCHED_ALLOCATION, 1
        )
    elif CPP_ORIGINAL_ALLOCATION in source and CPP_ORIGINAL_INCLUDE in source:
        source = source.replace(CPP_ORIGINAL_INCLUDE, CPP_PATCHED_INCLUDE, 1).replace(
            CPP_ORIGINAL_ALLOCATION, CPP_PATCHED_ALLOCATION, 1
        )
    else:
        raise SystemExit(
            "ERROR: unsupported ESPHome API implementation; expected ESPHome "
            f"2026.7.3 layout in {path}"
        )
    write_source(path, source)
    return True


def patch_header(path: Path) -> bool:
    source = read_source(path)
    if HEADER_PATCHED_DESTROY in source and HEADER_PATCHED_INCLUDE in source:
        return False
    if HEADER_ORIGINAL_DESTROY not in source or HEADER_ORIGINAL_INCLUDE not in source:
        raise SystemExit(
            "ERROR: unsupported ESPHome API header; expected ESPHome 2026.7.3 "
            f"layout in {path}"
        )
    source = source.replace(HEADER_ORIGINAL_INCLUDE, HEADER_PATCHED_INCLUDE, 1).replace(
        HEADER_ORIGINAL_DESTROY, HEADER_PATCHED_DESTROY, 1
    )
    write_source(path, source)
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        help="explicit api_overflow_buffer.cpp path (used by the self-test)",
    )
    args = parser.parse_args()
    if args.source:
        cpp_path = args.source.resolve()
        header_path = cpp_path.with_suffix(".h")
    else:
        cpp_path, header_path = locate_sources()

    changed = patch_cpp(cpp_path)
    changed = patch_header(header_path) or changed
    action = "applied" if changed else "already applied"
    print(f"OK: ESPHome API low-memory safety fix {action}: {cpp_path.parent}")


if __name__ == "__main__":
    main()
