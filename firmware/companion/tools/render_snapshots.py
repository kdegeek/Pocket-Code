#!/usr/bin/env python3
"""Generate/check deterministic 466x466 T3 Companion framebuffer snapshots.

The host C++ renderer is the firmware-facing seam.  This small dependency-free
reference writer keeps snapshot generation usable before an ESP-IDF build exists
and deliberately mirrors the approved ring geometry, palette, and state names.
It writes PNGs with no timestamps or ancillary metadata, so --check is a byte
for byte proof rather than a perceptual comparison.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
import tempfile
import zlib
from pathlib import Path

WIDTH = HEIGHT = 466
CENTER = 233
RADII = (205, 187, 169)
STROKE = 9
TRACK = (0x1B, 0x24, 0x30)
COLORS = ((0x58, 0xA6, 0xFF), (0xE8, 0x89, 0x62), (0xF4, 0xF6, 0xF8))
NAMES = (
    "ambient",
    "stale",
    "prompt",
    "bounded-choice",
    "transcript",
    "applied-glow",
    "rejected-glow",
    "locked",
    "protocol-incompatible",
    "provisioning",
)

FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01111", "10000", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "00010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "01010", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "11011", "10001"),
    "X": ("10001", "01010", "01010", "00100", "01010", "01010", "10001"),
    "Y": ("10001", "01010", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    "/": ("00001", "00010", "00010", "00100", "01000", "01000", "10000"),
    "%": ("11001", "11001", "00010", "00100", "01000", "10011", "10011"),
    ":": ("00000", "00100", "00100", "00000", "00100", "00100", "00000"),
    ".": ("00000", "00000", "00000", "00000", "00000", "01100", "01100"),
    "<": ("00000", "00100", "01000", "10000", "01000", "00100", "00000"),
    ">": ("00000", "10000", "01000", "00100", "01000", "10000", "00000"),
    "|": ("00100", "00100", "00100", "00100", "00100", "00100", "00100"),
}


def put(frame: bytearray, x: int, y: int, color: tuple[int, int, int]) -> None:
    if 0 <= x < WIDTH and 0 <= y < HEIGHT:
        offset = (y * WIDTH + x) * 3
        frame[offset : offset + 3] = bytes(color)


def text(frame: bytearray, value: str, x: int, y: int, scale: int, color: tuple[int, int, int]) -> None:
    cursor = x
    for raw in value.upper():
        bitmap = FONT.get(raw, FONT.get(" ", ("00000",) * 7))
        for row, bits in enumerate(bitmap):
            for col, bit in enumerate(bits):
                if bit != "1":
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        put(frame, cursor + col * scale + dx, y + row * scale + dy, color)
        cursor += 6 * scale


def centered(frame: bytearray, value: str, y: int, scale: int, color: tuple[int, int, int]) -> None:
    text(frame, value, CENTER - len(value) * 6 * scale // 2, y, scale, color)


def round_rect(frame: bytearray, left: int, top: int, right: int, bottom: int, radius: int, color: tuple[int, int, int]) -> None:
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            nx = min(max(x, left + radius), right - radius)
            ny = min(max(y, top + radius), bottom - radius)
            if (x - nx) ** 2 + (y - ny) ** 2 <= radius**2:
                put(frame, x, y, color)


def ring(frame: bytearray, radius: int, percent: float | None, color: tuple[int, int, int]) -> None:
    outer = radius + STROKE // 2 + 1
    inner = radius - STROKE // 2 - 1
    fraction = max(0.0, min(percent if percent is not None else 0.0, 100.0)) / 100.0
    for y in range(CENTER - outer, CENTER + outer + 1):
        for x in range(CENTER - outer, CENTER + outer + 1):
            distance = math.hypot(x - CENTER, y - CENTER)
            if not inner <= distance <= outer:
                continue
            angle = math.atan2(x - CENTER, -(y - CENTER))
            if angle < 0:
                angle += 2.0 * math.pi
            put(frame, x, y, color if angle / (2.0 * math.pi) <= fraction else TRACK)


def background() -> bytearray:
    frame = bytearray(bytes((5, 7, 10)) * (WIDTH * HEIGHT))
    for y in range(HEIGHT):
        for x in range(WIDTH):
            distance = math.hypot(x - CENTER, y - CENTER)
            if distance <= 220:
                amount = max(0.0, 1.0 - distance / 220.0)
                put(frame, x, y, (int(5 + 17 * amount), int(7 + 22 * amount), int(10 + 30 * amount)))
    return frame


def render(name: str) -> bytes:
    frame = background()
    values: tuple[float | None, float | None, float | None]
    if name == "stale":
        values = (None, None, None)
    else:
        values = (54.0, 31.0, 18.0)
    for radius, percent, color in zip(RADII, values, COLORS):
        ring(frame, radius, percent, color)
    row = ("CLAUDE 5H --", "CODEX 5H --", "XAI 7D --") if name == "stale" else (
        "CLAUDE 5H 28%", "CODEX 5H 61%", "XAI 7D 18%"
    )
    for x, value, color in zip((72, 187, 306), row, COLORS):
        text(frame, value, x, 86, 1, color)

    if name in {"prompt", "bounded-choice"}:
        centered(frame, "INPUT REQUIRED", 132, 1, (119, 133, 152))
        centered(frame, "ALLOW MIGRATION?" if name == "prompt" else "HOW PROCEED?", 154, 2, (244, 246, 248))
        round_rect(frame, 108, 190, 358, 295, 14, (24, 33, 46))
        centered(frame, "RUN REQUEST" if name == "prompt" else "APPLY MIGRATION", 222, 2, (244, 246, 248))
        centered(frame, "← DENY · APPROVE → · CANCEL ↓" if name == "prompt" else "← BROWSE → · ANSWER ↑", 313, 1, (148, 160, 177))
    elif name == "transcript":
        centered(frame, "TRANSCRIPT", 137, 1, (119, 133, 152))
        round_rect(frame, 92, 175, 374, 288, 14, (24, 33, 46))
        centered(frame, "FOLLOW UP WITH T3", 226, 2, (244, 246, 248))
        centered(frame, "← DISCARD · SUBMIT →", 315, 1, (148, 160, 177))
    elif name == "locked":
        centered(frame, "LOCKED", 198, 3, (244, 246, 248))
        centered(frame, "DOUBLE-CLICK TO WAKE", 244, 1, (148, 160, 177))
    elif name == "protocol-incompatible":
        centered(frame, "INCOMPATIBLE", 198, 2, (235, 95, 113))
        centered(frame, "UPDATE FIRMWARE", 244, 1, (148, 160, 177))
    elif name == "provisioning":
        centered(frame, "PROVISIONING", 198, 2, (88, 166, 255))
        centered(frame, "ADD REMEMBERED NETWORK", 244, 1, (148, 160, 177))
    elif name == "applied-glow":
        centered(frame, "APPLIED", 198, 3, (162, 255, 215))
        centered(frame, "ACKNOWLEDGED BY T3", 244, 1, (148, 160, 177))
    elif name == "rejected-glow":
        centered(frame, "REJECTED", 198, 3, (235, 95, 113))
        centered(frame, "T3 DID NOT APPLY", 244, 1, (148, 160, 177))
    elif name == "stale":
        centered(frame, "RECONNECTING", 183, 2, (88, 166, 255))
        centered(frame, "CACHED STATE STALE", 226, 1, (148, 160, 177))
        centered(frame, "T3 | STALE", 248, 1, (148, 160, 177))
    else:
        centered(frame, "T3 // MILLWORK", 157, 1, (119, 133, 152))
        centered(frame, "WORKING", 183, 3, (244, 246, 248))
        centered(frame, "PROVIDER RUNTIME EVENTS", 226, 1, (148, 160, 177))
        centered(frame, "CODEX | LIVE", 248, 1, (148, 160, 177))

    if name in {"applied-glow", "rejected-glow"}:
        glow = (162, 255, 215) if name == "applied-glow" else (235, 95, 113)
        for radius in (120, 82):
            for y in range(CENTER - radius, CENTER + radius + 1):
                for x in range(CENTER - radius, CENTER + radius + 1):
                    if abs(math.hypot(x - CENTER, y - CENTER) - radius) <= 2:
                        put(frame, x, y, glow)
    return png(frame)


def png(rgb: bytes) -> bytes:
    raw = b"".join(b"\x00" + rgb[y * WIDTH * 3 : (y + 1) * WIDTH * 3] for y in range(HEIGHT))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--update", action="store_true")
    mode.add_argument("--check", action="store_true")
    parser.add_argument("--directory", type=Path, default=Path(__file__).resolve().parents[1] / "snapshots")
    args = parser.parse_args()
    directory: Path = args.directory
    directory.mkdir(parents=True, exist_ok=True)
    if args.update:
        for name in NAMES:
            (directory / f"{name}.png").write_bytes(render(name))
        print(f"updated {len(NAMES)} deterministic {WIDTH}x{HEIGHT} snapshots in {directory}")
        return 0

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="t3-companion-snapshots-") as temporary:
        generated = Path(temporary)
        for name in NAMES:
            expected = directory / f"{name}.png"
            actual = generated / f"{name}.png"
            actual.write_bytes(render(name))
            if not expected.exists():
                failures.append(f"missing {expected.name}")
            elif expected.read_bytes() != actual.read_bytes():
                failures.append(f"changed {expected.name}")
    if failures:
        for failure in failures:
            print(f"SNAPSHOT MISMATCH: {failure}", file=sys.stderr)
        return 1
    print(f"checked {len(NAMES)} deterministic {WIDTH}x{HEIGHT} snapshots")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
