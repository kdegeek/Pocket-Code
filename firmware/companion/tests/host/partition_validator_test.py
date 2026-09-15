from __future__ import annotations

from dataclasses import replace
from pathlib import Path

import pytest

from tools.validate_partitions import (
    LOGICAL_FLASH_SIZE,
    parse_partition_csv,
    validate_partitions,
)


ROOT = Path(__file__).resolve().parents[2]
PARTITIONS_CSV = ROOT / "partitions.csv"


def _baseline():
    return parse_partition_csv(PARTITIONS_CSV)


def _replace(rows, name: str, **changes):
    return [replace(row, **changes) if row.name == name else row for row in rows]


def test_baseline_has_exact_16_mib_ab_layout():
    rows = _baseline()

    assert [(row.name, row.type, row.subtype, row.offset, row.size, row.flags) for row in rows] == [
        ("nvs", "data", "nvs", 0x9000, 0x6000, ""),
        ("otadata", "data", "ota", 0xF000, 0x2000, ""),
        ("phy_init", "data", "phy", 0x11000, 0x1000, ""),
        ("ota_0", "app", "ota_0", 0x20000, 0x700000, ""),
        ("ota_1", "app", "ota_1", 0x720000, 0x700000, ""),
        ("storage", "data", "spiffs", 0xE20000, 0x1D0000, ""),
        ("coredump", "data", "coredump", 0xFF0000, 0x10000, ""),
    ]
    assert validate_partitions(rows, flash_size=LOGICAL_FLASH_SIZE) == ()
    assert max(row.end for row in rows) == LOGICAL_FLASH_SIZE
    assert [row.subtype for row in rows if row.type == "app"] == ["ota_0", "ota_1"]


@pytest.mark.parametrize(
    ("label", "rows", "needle"),
    [
        (
            "duplicate names",
            lambda: _baseline() + [_baseline()[0]],
            "duplicate partition name",
        ),
        (
            "reserved bootloader region",
            lambda: _replace(_baseline(), "nvs", offset=0x1000),
            "reserved",
        ),
        (
            "reserved partition-table region",
            lambda: _replace(_baseline(), "nvs", offset=0x8000),
            "reserved",
        ),
        (
            "missing OTA slot",
            lambda: [row for row in _baseline() if row.name != "ota_1"],
            "exactly two OTA",
        ),
        (
            "32 MiB placement",
            lambda: _replace(_baseline(), "coredump", offset=0x1FF0000),
            "16 MiB",
        ),
        (
            "data alignment",
            lambda: _replace(_baseline(), "nvs", offset=0x9001),
            "alignment",
        ),
        (
            "app alignment",
            lambda: _replace(_baseline(), "ota_0", offset=0x20001),
            "alignment",
        ),
        (
            "overlap",
            lambda: _replace(_baseline(), "storage", offset=0xE30000),
            "overlap",
        ),
    ],
)
def test_invalid_layouts_are_rejected(label, rows, needle):
    errors = validate_partitions(rows())

    assert errors, label
    assert any(needle.lower() in error.lower() for error in errors), errors


def test_32_mib_flash_argument_is_rejected_even_for_small_layout():
    errors = validate_partitions(_baseline(), flash_size=0x2000000)

    assert any("16 MiB" in error for error in errors), errors
