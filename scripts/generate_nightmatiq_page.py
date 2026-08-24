#!/usr/bin/env python3
from __future__ import annotations

import gzip
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / "esphome/components/nightmatiq_mesh/nightmatiq_page.html"
target = root / "esphome/components/nightmatiq_mesh/nightmatiq_page.h"
raw = source.read_bytes()
compressed = gzip.compress(raw, compresslevel=9, mtime=0)
rows = [
    "  " + ", ".join(f"0x{value:02x}" for value in compressed[offset : offset + 16]) + ","
    for offset in range(0, len(compressed), 16)
]
target.write_text(
    "#pragma once\n#include <cstddef>\n#include <cstdint>\n"
    "namespace esphome::nightmatiq_mesh {\n"
    f"static constexpr size_t NIGHTMATIQ_PAGE_RAW_SIZE = {len(raw)};\n"
    f"static const uint8_t NIGHTMATIQ_PAGE_GZ[{len(compressed)}] = {{\n"
    + "\n".join(rows)
    + "\n};\n}  // namespace esphome::nightmatiq_mesh\n",
    encoding="utf-8",
)
print(f"Generated {target.name}: {len(raw)} -> {len(compressed)} bytes")
