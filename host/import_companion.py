#!/usr/bin/env python3
"""Import one active T3 companion's verifier locally, without reading its raw token."""
import argparse
import contextlib
import json
import os
import sqlite3
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--userdata", required=True, type=Path)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--host", required=True, help="Mac private LAN address reachable by the device")
    parser.add_argument("--port", default=39073, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    database = (args.userdata / "state.sqlite").resolve()
    with contextlib.closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as connection:
        row = connection.execute("SELECT credential_hash, scopes_json FROM companion_devices "
                                 "WHERE device_id=? AND status='active'", (args.device_id,)).fetchone()
    if row is None:
        parser.error("No active companion with read scope matches this device")
    try:
        scopes = json.loads(row[1])
    except (TypeError, json.JSONDecodeError):
        parser.error("No active companion with read scope matches this device")
    if (not isinstance(scopes, list)
            or any(not isinstance(scope, str) for scope in scopes)
            or "companion:read" not in scopes):
        parser.error("No active companion with read scope matches this device")
    prefix = "hmac-sha256:"
    if not row[0].startswith(prefix):
        parser.error("Unsupported legacy credential verifier")
    pepper = (args.userdata / "secrets/companion-auth-pepper.bin").read_bytes()
    if len(pepper) != 32:
        parser.error("Invalid legacy authentication pepper")
    config = {"device_id": args.device_id, "host": args.host, "port": args.port,
              "token_hmac_sha256": row[0][len(prefix):], "auth_pepper_hex": pepper.hex(),
              "poll_seconds": 10, "stale_seconds": 900}
    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    # Refuse to overwrite an existing live configuration.
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(config, stream, indent=2)
        stream.write("\n")
    print(f"Imported one companion verifier into {output}; source gateway unchanged")


if __name__ == "__main__":
    main()
