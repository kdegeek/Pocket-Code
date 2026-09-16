# CodexBar companion adapter

Use the running macOS CodexBar app as the data source for the recovered
Waveshare **ESP32-S3-Touch-AMOLED-1.75C** companion UI.

```text
CodexBar.app → widget-snapshot.json → this adapter → companion WebSocket → display
```

This is a companion service, not an in-process CodexBar plugin. It reads
CodexBar's existing widget snapshot; it never reads provider credentials or
starts a second provider polling process. Tested against CodexBar 0.60.3.
The widget JSON is an internal app interface, so a future CodexBar schema
change may require an adapter update.

## What appears

- Existing Codex, Claude and xAI/Grok usage rings and provider row.
- Today's and 30-day token totals in the existing center activity text,
  when the selected providers report fresh counters.
- Unavailable/stale data stays unavailable; missing quota windows are never
  converted into zero usage. Quota percentages cannot be converted to a count
  of remaining tokens.
- The display uses a CODEXBAR heading and USAGE / NO DATA status.
  This adapter provides usage only: no active turn, approval requests, or
  microphone target is advertised.

Snapshots are reopened every 10 seconds so atomic replacement by CodexBar
works. Provider data expires after 15 minutes by default. This reads cached
data at CodexBar's own refresh cadence; it does not make it more current.
Only changed snapshots are sent; WebSocket ping/pong keeps the connection
alive without rewriting the ESP32's persistent snapshot every few seconds.

## Setup for an already-enrolled companion

1. Use Python 3.11 or newer and install the adapter dependencies:

   ```sh
   python3 -m venv .venv
   .venv/bin/pip install -r host/requirements-companion.txt
   ```

2. Copy `host/companion-config.json.example` to a private file outside the
   repository. Set the exact device ID and the SHA-256 hash of its existing
   companion credential. The raw bearer stays on the device. A previously
   enrolled T3 gateway's HMAC verifier and server-only pepper can be migrated
   locally without displaying or exporting the bearer:

   ```sh
   .venv/bin/python host/import_companion.py \
     --userdata /path/to/old-gateway/userdata \
     --device-id t3-YOUR_DEVICE_MAC --host YOUR_MAC_LAN_IP \
     --output "$HOME/Library/Application Support/Pocket-Code/config.json"
   ```

   The import reads only the selected active companion record and its local
   authentication pepper. It does not change the old gateway or device. It
   refuses to overwrite an existing config. The resulting private config uses
   `token_hmac_sha256` and `auth_pepper_hex` instead of `token_sha256`.
   This is an independent verifier copy: later revocation in the old gateway
   does not revoke it here. Stop this adapter and remove its private config
   to revoke this display connection.
   This version does not enroll new devices.

3. Set `host` to the Mac's specific private LAN address that the device can
   reach, and choose the device's existing gateway port where possible. The
   default is loopback for local testing. Use a stable DHCP assignment for
   the Mac. The recovered firmware uses trusted-LAN HTTP/WebSocket transport;
   do not publish or port-forward this endpoint to the Internet.

4. Run in the foreground:

   ```sh
   PYTHONPATH=host .venv/bin/python -m codexbar_companion.server --config /path/to/private/config.json
   ```

   Run that command from the repository root.

5. Install the login service after validating the foreground connection:

   ```sh
   .venv/bin/python host/install_companion.py --config /path/to/private/config.json
   ```

   The installer creates its own runtime under
   `~/Library/Application Support/Pocket-Code/runtime` and a per-user
   `com.kdegeek.pocket-code-companion` LaunchAgent. It does not modify CodexBar
   or the original TokenGenie service. Configuration and credentials are not
   copied into the repository.

Optional `snapshot_path` overrides the default
`~/Library/Group Containers/Y5PE65HELJ.com.steipete.codexbar/widget-snapshot.json`.

## Endpoints and verification

- `GET /healthz`: service/source readiness and connection counters, no usage
  values or credential material.
- `GET /api/companion/v1/snapshot`: bearer-authenticated protocol v1 snapshot.
- `GET /api/companion/v1/ws`: bearer-authenticated live stream. The first
  message must identify the configured device and negotiate protocol v1.

```sh
PYTHONPATH=host .venv/bin/python -m unittest discover -s host/tests
```

Tests exercise missing/stale data, projection, authenticated handshake,
identity redaction, and damaged snapshots with synthetic data. Live acceptance
also requires the physical device to connect and apply a snapshot.

## Stop and recover

```sh
launchctl bootout "gui/$(id -u)/com.kdegeek.pocket-code-companion"
```

Remove its plist from `~/Library/LaunchAgents/` to disable login startup.
Keep the private config and recovery files until you no longer need them.
The recovered firmware source lives in `firmware/companion`; the original
Arduino TokenGenie source remains under `firmware/token-meter`. They target
different board variants and speak different protocols.
