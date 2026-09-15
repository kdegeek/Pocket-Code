#!/usr/bin/env python3
"""Check reproducible, host-only firmware release inputs.

This command never signs, downloads, flashes, or contacts a service.  It
hashes artifacts already present in the checkout and reports release-time
requirements that cannot be inferred (notably the physical board manifest and
the production signing policy).  The default check is for local development;
``--production`` (or ``--require-signing``) makes those authority records
strict requirements for OTA publication.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


FIXTURE_ROOT = Path("fixtures/companion/v1")
FIXTURE_MANIFEST = FIXTURE_ROOT / "manifest.json"
ARTIFACTS = (
    Path("build/bootloader/bootloader.bin"),
    Path("build/partition_table/partition-table.bin"),
    Path("build/t3_companion_firmware.bin"),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def check_fixture_manifest(root: Path, checks: list[dict[str, str]], errors: list[str]) -> None:
    path = root / FIXTURE_MANIFEST
    if not path.is_file():
        errors.append(f"missing protocol fixture manifest: {path}")
        return
    try:
        manifest = read_json(path)
    except (OSError, ValueError) as exc:
        errors.append(f"invalid protocol fixture manifest {path}: {exc}")
        return
    if manifest.get("manifestVersion") != 1 or manifest.get("protocolVersion") != 1:
        errors.append("protocol fixture manifest must declare manifestVersion=1 and protocolVersion=1")
    fixtures = manifest.get("fixtures")
    if not isinstance(fixtures, list) or not fixtures:
        errors.append("protocol fixture manifest has no fixture records")
        return
    for record in fixtures:
        relative = record.get("path") if isinstance(record, dict) else None
        expected_hash = record.get("sha256") if isinstance(record, dict) else None
        expected_size = record.get("size") if isinstance(record, dict) else None
        if not isinstance(relative, str) or not isinstance(expected_hash, str):
            errors.append("protocol fixture manifest contains an incomplete record")
            continue
        fixture = root / FIXTURE_ROOT / relative
        if not fixture.is_file():
            errors.append(f"missing protocol fixture: {fixture}")
            continue
        actual_hash = sha256(fixture)
        actual_size = fixture.stat().st_size
        if actual_hash != expected_hash or actual_size != expected_size:
            errors.append(
                f"fixture mismatch {relative}: size/hash {actual_size}/{actual_hash} "
                f"!= {expected_size}/{expected_hash}"
            )
        else:
            checks.append({"name": f"fixture:{relative}", "status": "verified", "sha256": actual_hash})
    manifest_hash = sha256(path)
    checks.append({"name": "protocol-fixture-manifest", "status": "verified", "sha256": manifest_hash})


def check_toolchain(root: Path, checks: list[dict[str, str]], errors: list[str]) -> None:
    lock = root / "dependencies.lock"
    docs = root / "docs/toolchain.md"
    if not lock.is_file():
        errors.append(f"missing toolchain lock: {lock}")
        return
    if not docs.is_file():
        errors.append(f"missing toolchain provenance: {docs}")
        return
    lock_text = lock.read_text(encoding="utf-8")
    docs_text = docs.read_text(encoding="utf-8")
    required = ("version: 5.5.5", "version: 3.0.0")
    if not all(token in lock_text for token in required):
        errors.append("dependencies.lock does not pin ESP-IDF 5.5.5 and Waveshare BSP 3.0.0")
    if "ESP-IDF 5.5" not in docs_text or "Waveshare" not in docs_text:
        errors.append("docs/toolchain.md is missing pinned ESP-IDF/Waveshare provenance")
    checks.append({"name": "toolchain-lock", "status": "verified", "sha256": sha256(lock)})


def check_partitions(root: Path, checks: list[dict[str, str]], errors: list[str]) -> None:
    try:
        sys.path.insert(0, str(root))
        from tools.validate_partitions import validate_partition_file

        partition_path = root / "partitions.csv"
        partition_errors = validate_partition_file(partition_path)
        if partition_errors:
            errors.extend(f"partition layout: {error}" for error in partition_errors)
        else:
            partition_hash = sha256(partition_path)
            checks.append(
                {
                    "name": "partition-layout",
                    "status": "verified",
                    "sha256": partition_hash,
                }
            )
            checks.append(
                {
                    "name": "ota-geometry",
                    "status": "verified",
                    "sha256": partition_hash,
                    "details": (
                        "fixed logical 16 MiB flash with compatible A/B OTA slots "
                        "and immutable storage geometry"
                    ),
                }
            )
    except (ImportError, OSError, ValueError) as exc:
        errors.append(f"partition validator unavailable: {exc}")


def check_artifacts(root: Path, checks: list[dict[str, str]], errors: list[str]) -> None:
    for relative in ARTIFACTS:
        path = root / relative
        if not path.is_file():
            errors.append(f"missing existing build artifact: {path}; run the pinned host/IDF build first")
            continue
        checks.append(
            {
                "name": f"artifact:{relative}",
                "status": "verified",
                "sha256": sha256(path),
                "size": str(path.stat().st_size),
            }
        )


def check_release_authority(
    root: Path,
    checks: list[dict[str, str]],
    errors: list[str],
) -> None:
    # Task 2's verified physical manifest is independent of the logical
    # partition CSV; never use the CSV alone to claim a board SKU/flash.
    board_candidates = (root / "docs/hardware-manifest.md", root / "docs/board-manifest.json")
    board = next((candidate for candidate in board_candidates if candidate.is_file()), None)
    if board is None:
        errors.append(
            "RELEASE REQUIREMENT: verified physical board manifest is unavailable "
            "(Task 2 probe/approval must record SKU, revision, and flash geometry)"
        )
    elif board.suffix == ".json":
        try:
            data = read_json(board)
        except (OSError, ValueError) as exc:
            errors.append(f"invalid supported board manifest {board}: {exc}")
        else:
            if not isinstance(data, dict):
                errors.append(
                    f"invalid supported board manifest {board}: expected a JSON object"
                )
            elif data.get("verified") is not True:
                errors.append(
                    "RELEASE REQUIREMENT: supported board manifest is not marked verified"
                )
            else:
                checks.append({"name": "supported-board-manifest", "status": "verified", "sha256": sha256(board)})
    else:
        text = board.read_text(encoding="utf-8")
        if "verified: true" not in text.lower():
            errors.append(
                "RELEASE REQUIREMENT: verified physical board manifest "
                "(hardware-manifest.md) must contain an explicit verified: true record"
            )
        else:
            checks.append({"name": "supported-board-manifest", "status": "verified", "sha256": sha256(board)})

    signing_candidates = (root / "docs/release-signing-policy.md", root / "docs/signing-policy.md")
    signing = next((candidate for candidate in signing_candidates if candidate.is_file()), None)
    if signing is None:
        errors.append(
            "RELEASE REQUIREMENT: production signing policy/public key is unavailable; "
            "the deterministic fixture verifier is host-test-only"
        )
    else:
        text = signing.read_text(encoding="utf-8")
        if "public" not in text.lower() or "private" not in text.lower():
            errors.append(
                "RELEASE REQUIREMENT: signing policy must identify production public/private-key custody"
            )
        else:
            checks.append({"name": "signing-policy", "status": "verified", "sha256": sha256(signing)})


def run_check(root: Path, *, require_production: bool = False) -> int:
    checks: list[dict[str, str]] = []
    reproducibility_errors: list[str] = []
    check_toolchain(root, checks, reproducibility_errors)
    check_fixture_manifest(root, checks, reproducibility_errors)
    check_partitions(root, checks, reproducibility_errors)
    check_artifacts(root, checks, reproducibility_errors)

    authority_errors: list[str] = []
    check_release_authority(root, checks, authority_errors)
    errors = [*reproducibility_errors]
    warnings: list[str] = []
    if require_production:
        errors.extend(authority_errors)
    else:
        warnings.extend(
            "unsigned-development: "
            f"{error.removeprefix('RELEASE REQUIREMENT: ')}; not for OTA publication"
            for error in authority_errors
        )

    production_ready = require_production and not errors
    if require_production:
        release_status = "production-ready" if production_ready else "production-blocked"
    else:
        release_status = "unsigned-development" if not errors else "development-invalid"

    result = {
        "schemaVersion": 1,
        "mode": "production" if require_production else "development",
        "releaseStatus": release_status,
        "otaPublicationEligible": production_ready,
        "productionReady": production_ready,
        "reproducible": not reproducibility_errors,
        "checks": sorted(checks, key=lambda item: item["name"]),
        "errors": errors,
        "warnings": warnings,
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if not errors else 2


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify existing release inputs")
    parser.add_argument(
        "--production",
        "--require-signing",
        dest="require_production",
        action="store_true",
        help="require verified board and production signing authority for OTA publication",
    )
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    if not args.check:
        parser.error("only --check is supported; signing and publishing require a release authority")
    return run_check(args.root.resolve(), require_production=args.require_production)


if __name__ == "__main__":
    raise SystemExit(main())
