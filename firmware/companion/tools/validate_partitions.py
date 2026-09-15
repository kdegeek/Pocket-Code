#!/usr/bin/env python3
"""Validate the T3 Companion's fixed logical 16 MiB partition layout.

The validator deliberately treats the layout as a contract.  It does not infer
offsets, sort records, or resize a partition on the caller's behalf.  This keeps
an accidental 32 MiB assumption or a reserved-region collision visible before
ESP-IDF generates an image.
"""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


LOGICAL_FLASH_SIZE = 0x1000000
BOOTLOADER_RESERVED = (0x000000, 0x008000)
PARTITION_TABLE_RESERVED = (0x008000, 0x009000)
DATA_ALIGNMENT = 0x1000
APP_ALIGNMENT = 0x10000


@dataclass(frozen=True)
class Partition:
    """One explicit ESP-IDF partition CSV row."""

    name: str
    type: str
    subtype: str
    offset: int
    size: int
    flags: str = ""

    @property
    def end(self) -> int:
        return self.offset + self.size


EXPECTED_PARTITIONS: tuple[Partition, ...] = (
    Partition("nvs", "data", "nvs", 0x9000, 0x6000),
    Partition("otadata", "data", "ota", 0xF000, 0x2000),
    Partition("phy_init", "data", "phy", 0x11000, 0x1000),
    Partition("ota_0", "app", "ota_0", 0x20000, 0x700000),
    Partition("ota_1", "app", "ota_1", 0x720000, 0x700000),
    Partition("storage", "data", "spiffs", 0xE20000, 0x1D0000),
    Partition("coredump", "data", "coredump", 0xFF0000, 0x10000),
)


def _parse_number(value: str, *, line_number: int, field: str) -> int:
    value = value.strip()
    if not value:
        raise ValueError(f"line {line_number}: {field} must be explicit")
    try:
        if value.lower().startswith(("0x", "+0x", "-0x")):
            return int(value, 16)
        return int(value, 10)
    except ValueError as exc:
        raise ValueError(f"line {line_number}: invalid {field} {value!r}") from exc


def parse_partition_csv(path: str | Path) -> list[Partition]:
    """Parse explicit partition rows from *path*.

    The standard ESP-IDF comment header and blank lines are ignored.  Offsets
    and sizes are required; implicit placement is intentionally unsupported.
    """

    rows: list[Partition] = []
    with Path(path).open("r", encoding="utf-8", newline="") as handle:
        for line_number, fields in enumerate(csv.reader(handle), start=1):
            if not fields or not any(field.strip() for field in fields):
                continue
            if fields[0].lstrip().startswith("#"):
                continue
            if len(fields) < 5:
                raise ValueError(f"line {line_number}: expected at least five CSV fields")
            if len(fields) > 6 and any(field.strip() for field in fields[6:]):
                raise ValueError(f"line {line_number}: unexpected CSV fields after Flags")
            name, partition_type, subtype = (field.strip() for field in fields[:3])
            if not name or not partition_type or not subtype:
                raise ValueError(f"line {line_number}: Name, Type, and SubType are required")
            rows.append(
                Partition(
                    name=name,
                    type=partition_type,
                    subtype=subtype,
                    offset=_parse_number(fields[3], line_number=line_number, field="Offset"),
                    size=_parse_number(fields[4], line_number=line_number, field="Size"),
                    flags=fields[5].strip() if len(fields) >= 6 else "",
                )
            )
    return rows


def _overlaps(partition: Partition, region: tuple[int, int]) -> bool:
    start, end = region
    return partition.offset < end and partition.end > start


def _describe(partition: Partition) -> str:
    return f"{partition.name} [{partition.offset:#x}, {partition.end:#x})"


def validate_partitions(
    partitions: Sequence[Partition], *, flash_size: int = LOGICAL_FLASH_SIZE
) -> tuple[str, ...]:
    """Return all validation errors for *partitions*.

    The return value is empty only when the records exactly match the approved
    names, types, subtypes, offsets, sizes, flags, alignments, and 16 MiB bounds.
    """

    errors: list[str] = []
    if flash_size <= 0:
        errors.append(f"flash size must be positive, got {flash_size:#x}")
    if flash_size > LOGICAL_FLASH_SIZE:
        errors.append(
            f"flash size {flash_size:#x} exceeds the logical 16 MiB limit {LOGICAL_FLASH_SIZE:#x}"
        )

    names: dict[str, list[Partition]] = {}
    for partition in partitions:
        names.setdefault(partition.name, []).append(partition)
    for name, matches in names.items():
        if len(matches) > 1:
            errors.append(f"duplicate partition name {name!r} ({len(matches)} records)")

    for partition in partitions:
        label = _describe(partition)
        if partition.offset < 0 or partition.size <= 0:
            errors.append(f"{label} has invalid non-positive offset or size")
            continue
        if partition.type == "data":
            if partition.offset % DATA_ALIGNMENT or partition.size % DATA_ALIGNMENT:
                errors.append(
                    f"{partition.name} data alignment must be {DATA_ALIGNMENT:#x}"
                )
        elif partition.type == "app":
            if partition.offset % APP_ALIGNMENT or partition.size % APP_ALIGNMENT:
                errors.append(
                    f"{partition.name} app alignment must be {APP_ALIGNMENT:#x}"
                )
        else:
            errors.append(f"{partition.name} has unsupported partition type {partition.type!r}")

        if partition.end > LOGICAL_FLASH_SIZE:
            errors.append(
                f"{partition.name} ends at {partition.end:#x}, beyond the logical 16 MiB bound"
            )
        if flash_size > 0 and partition.end > flash_size:
            errors.append(
                f"{partition.name} ends at {partition.end:#x}, beyond flash size {flash_size:#x}"
            )
        if _overlaps(partition, BOOTLOADER_RESERVED):
            errors.append(f"{partition.name} overlaps the bootloader reserved region")
        if _overlaps(partition, PARTITION_TABLE_RESERVED):
            errors.append(f"{partition.name} overlaps the partition-table reserved region")

    for index, left in enumerate(partitions):
        for right in partitions[index + 1 :]:
            if _overlaps(left, (right.offset, right.end)):
                errors.append(f"{left.name} overlaps {right.name}")

    expected_by_name = {partition.name: partition for partition in EXPECTED_PARTITIONS}
    for partition in partitions:
        expected = expected_by_name.get(partition.name)
        if expected is None:
            errors.append(f"unrecognized partition name {partition.name!r}")
            continue
        differences: list[str] = []
        for field in ("type", "subtype", "offset", "size", "flags"):
            actual = getattr(partition, field)
            wanted = getattr(expected, field)
            if actual != wanted:
                if field in {"offset", "size"}:
                    differences.append(f"{field} {actual:#x} (expected {wanted:#x})")
                else:
                    differences.append(f"{field} {actual!r} (expected {wanted!r})")
        if differences:
            errors.append(f"{partition.name} has wrong fixed record: {', '.join(differences)}")

    present_names = set(names)
    for expected in EXPECTED_PARTITIONS:
        if expected.name not in present_names:
            errors.append(f"missing required partition {expected.name!r}")

    ota_slots = [
        partition
        for partition in partitions
        if partition.type == "app" and partition.subtype in {"ota_0", "ota_1"}
    ]
    if len(ota_slots) != 2 or {partition.subtype for partition in ota_slots} != {"ota_0", "ota_1"}:
        errors.append(
            f"expected exactly two OTA app slots (ota_0 and ota_1), found "
            f"{[partition.subtype for partition in ota_slots]!r}"
        )

    if len(partitions) != len(EXPECTED_PARTITIONS):
        errors.append(
            f"expected exactly {len(EXPECTED_PARTITIONS)} partition records, found {len(partitions)}"
        )

    # Keep output deterministic when several checks identify the same malformed row.
    return tuple(dict.fromkeys(errors))


def validate_partition_file(path: str | Path, *, flash_size: int = LOGICAL_FLASH_SIZE) -> tuple[str, ...]:
    """Parse and validate a CSV, returning parse errors in the same shape."""

    try:
        partitions = parse_partition_csv(path)
    except (OSError, ValueError) as exc:
        return (str(exc),)
    return validate_partitions(partitions, flash_size=flash_size)


def _parse_cli_number(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer {value!r}") from exc


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, help="ESP-IDF partition CSV to validate")
    parser.add_argument(
        "--flash-size",
        type=_parse_cli_number,
        default=LOGICAL_FLASH_SIZE,
        help="logical flash size in bytes (default: 0x1000000)",
    )
    args = parser.parse_args(argv)
    errors = validate_partition_file(args.csv, flash_size=args.flash_size)
    if errors:
        print(f"INVALID: {args.csv}", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"VALID: {args.csv} (logical flash size {LOGICAL_FLASH_SIZE:#x}; two OTA slots)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
