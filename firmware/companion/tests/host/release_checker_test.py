from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools" / "build_release.py"


def _run_checker(
    *extra_args: str, root: Path = ROOT
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), "--check", *extra_args, "--root", str(root)],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )


def _report(result: subprocess.CompletedProcess[str]) -> dict[str, object]:
    return json.loads(result.stdout)


def _development_root() -> tempfile.TemporaryDirectory[str]:
    temporary = tempfile.TemporaryDirectory()
    root = Path(temporary.name)
    for relative in ("dependencies.lock", "partitions.csv", "docs/toolchain.md", "docs/hardware-manifest.md"):
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)
    shutil.copytree(ROOT / "fixtures", root / "fixtures")
    shutil.copytree(ROOT / "tools", root / "tools")
    for relative in (
        "build/bootloader/bootloader.bin",
        "build/partition_table/partition-table.bin",
        "build/t3_companion_firmware.bin",
    ):
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(b"development artifact")
    return temporary


class ReleaseCheckerTest(unittest.TestCase):
    def test_default_check_is_reproducible_unsigned_development(self) -> None:
        with _development_root() as temporary:
            result = _run_checker(root=Path(temporary))

            self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
            report = _report(result)
            self.assertEqual(report["mode"], "development")
            self.assertEqual(report["releaseStatus"], "unsigned-development")
            self.assertIs(report["otaPublicationEligible"], False)
            self.assertIs(report["reproducible"], True)
            self.assertEqual(report["errors"], [])
            self.assertTrue(
                any("not for OTA publication" in warning for warning in report["warnings"])
            )

    def test_production_check_keeps_signing_gate_strict_after_board_verification(self) -> None:
        with _development_root() as temporary:
            result = _run_checker("--production", root=Path(temporary))

            self.assertEqual(result.returncode, 2, result.stderr or result.stdout)
            report = _report(result)
            self.assertEqual(report["mode"], "production")
            self.assertEqual(report["releaseStatus"], "production-blocked")
            self.assertIs(report["otaPublicationEligible"], False)
            self.assertIs(report["productionReady"], False)
            self.assertFalse(
                any("verified physical board manifest" in error for error in report["errors"])
            )
            self.assertTrue(
                any("signing policy/public key" in error for error in report["errors"])
            )


if __name__ == "__main__":
    unittest.main()
