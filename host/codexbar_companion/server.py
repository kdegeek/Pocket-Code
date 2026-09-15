"""Authenticated, display-only output adapter. Never polls provider APIs."""
import argparse
import asyncio
import contextlib
import hashlib
import hmac
import json
import logging
import re
import uuid
from datetime import datetime, timezone
from pathlib import Path

from aiohttp import WSMsgType, web

from .projection import project_usage, token_summary

LOG = logging.getLogger("pocket-code")
STATE = web.AppKey("state", object)
MAX_SOURCE_BYTES = 8 * 1024 * 1024
DEFAULT_SNAPSHOT = Path.home() / "Library/Group Containers/Y5PE65HELJ.com.steipete.codexbar/widget-snapshot.json"
DEFAULT_CONFIG = Path.home() / "Library/Application Support/Pocket-Code/config.json"


def iso_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


class Companion:
    def __init__(self, config):
        self.device_id = config["device_id"]
        self.token_hash = config.get("token_sha256", config.get("token_hmac_sha256", ""))
        self.pepper = None
        if "token_hmac_sha256" in config:
            self.pepper = bytes.fromhex(config["auth_pepper_hex"])
            if len(self.pepper) != 32 or "token_sha256" in config:
                raise ValueError("legacy HMAC authentication requires one 32-byte pepper")
        if not isinstance(self.device_id, str) or not 1 <= len(self.device_id) <= 128:
            raise ValueError("device_id must identify the enrolled companion")
        if not isinstance(self.token_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", self.token_hash):
            raise ValueError("token_sha256 must be the existing device credential's SHA-256")
        self.source = Path(config.get("snapshot_path", DEFAULT_SNAPSHOT)).expanduser()
        self.poll_seconds = max(0.05, float(config.get("poll_seconds", 10)))
        self.stale_seconds = int(config.get("stale_seconds", 900))
        if self.stale_seconds < 60:
            raise ValueError("stale_seconds must be at least 60")
        self.sequence = 0
        self.revision = 0
        self.current = None
        self.fingerprint = None
        self.source_ok = False
        self.clients = set()
        self.active_session_id = None
        self.last_hello_at = None
        self.last_sent_at = None
        self.frames_sent = 0

    def authenticate(self, request):
        value = request.headers.get("Authorization", "")
        if not value.startswith("Bearer ") or len(value) > 4096:
            raise web.HTTPUnauthorized()
        digest = (hmac.new(self.pepper, b"device-token\0" + value[7:].encode(), hashlib.sha256).hexdigest()
                  if self.pepper is not None else hashlib.sha256(value[7:].encode()).hexdigest())
        if not hmac.compare_digest(digest, self.token_hash):
            raise web.HTTPUnauthorized()

    def snapshot(self):
        now = datetime.now(timezone.utc)
        stamp = iso_now()
        try:
            with self.source.open("rb") as source:
                raw = source.read(MAX_SOURCE_BYTES + 1)
            if len(raw) > MAX_SOURCE_BYTES:
                raise ValueError("snapshot exceeds size limit")
            widget = json.loads(raw)
            if not isinstance(widget, dict) or not isinstance(widget.get("entries"), list):
                raise ValueError("invalid snapshot shape")
            self.source_ok = True
        except (OSError, ValueError, UnicodeError):
            widget = {"entries": [], "enabledProviders": []}
            self.source_ok = False
        usage = project_usage(widget, now, self.stale_seconds)
        summary = token_summary(widget, now, self.stale_seconds) or "Quota usage from CodexBar"
        available = any(p["status"] == "available" for p in usage["providers"])
        if not self.source_ok:
            summary = "CodexBar data unavailable"
        elif not available:
            summary = "Waiting for fresh CodexBar data"
        # Ignore synthesized observation timestamps when data is unavailable.
        # Polling/keepalives must not create NVS writes on the ESP32 every 10s.
        def stable(value):
            if isinstance(value, dict):
                return {k: stable(v) for k, v in value.items() if k != "observedAt"}
            if isinstance(value, list):
                return [stable(v) for v in value]
            return value
        generated = widget.get("generatedAt")
        if not isinstance(generated, str) or len(generated) > 40:
            generated = None
        fingerprint = json.dumps([stable(usage), summary, self.source_ok,
                                  generated], sort_keys=True, allow_nan=False)
        if self.current is None or fingerprint != self.fingerprint:
            self.revision += 1
            self.fingerprint = fingerprint
            self.current = {
                "type": "snapshot", "protocolVersion": 1, "adapterSequence": self.sequence,
                "workItems": [{"environmentId": "pocket-code", "projectId": "codexbar",
                    "threadId": "usage-display", "projectName": "CODEXBAR",
                    "threadName": "Usage display", "state": "idle" if available else "blocked",
                    "activity": summary, "updatedAt": stamp}],
                "automaticFocusThreadId": "usage-display", "pendingRequests": [],
                "usage": usage,
                "connectivity": {"status": "connected", "lastConnectedAt": stamp, "lastSeenAt": stamp},
                "freshness": {"observedAt": stamp, "isStale": not available},
            }
        return self.current

    async def health(self, request):
        self.snapshot()
        return web.json_response({"ok": True, "sourceReadable": self.source_ok,
            "connectedDevices": len(self.clients), "lastHelloAt": self.last_hello_at,
            "lastSnapshotSentAt": self.last_sent_at, "snapshotsSent": self.frames_sent,
            "lastSnapshotSequence": self.sequence})

    async def get_snapshot(self, request):
        self.authenticate(request)
        return web.json_response(self.snapshot(), headers={"Cache-Control": "no-store"})

    def rejection(self, command_id):
        return {"commandId": command_id, "status": "rejected", "updatedAt": iso_now(),
                "error": "This companion provides usage display only"}

    async def send_snapshot(self, ws, snapshot, session_id, sequence):
        # Event cursors must advance exactly once per delivered frame, including
        # reconnects. Polling or other readers cannot consume device cursors.
        snapshot = {**snapshot, "adapterSequence": sequence}
        # The recovered live firmware decodes snapshots inside event envelopes.
        await ws.send_json({"type": "snapshot", "protocolVersion": 1,
                            "adapterSequence": snapshot["adapterSequence"], "sessionId": session_id,
                            "eventId": str(uuid.uuid4()), "payload": snapshot})
        self.sequence = max(self.sequence, sequence)
        self.current["adapterSequence"] = self.sequence
        self.last_sent_at = iso_now()
        self.frames_sent += 1

    async def publish_changes(self, ws, last_revision, session_id, sequence):
        while not ws.closed:
            await asyncio.sleep(self.poll_seconds)
            if session_id != self.active_session_id:
                return
            snapshot = self.snapshot()
            if self.revision != last_revision:
                sequence += 1
                await self.send_snapshot(ws, snapshot, session_id, sequence)
                last_revision = self.revision

    async def websocket(self, request):
        self.authenticate(request)
        ws = web.WebSocketResponse(heartbeat=30, max_msg_size=65536)
        await ws.prepare(request)
        publisher = None
        try:
            message = await asyncio.wait_for(ws.receive(), timeout=10)
            if message.type != WSMsgType.TEXT:
                raise ValueError("hello required")
            hello = json.loads(message.data)
            if (not isinstance(hello, dict) or hello.get("type") != "hello"
                    or hello.get("deviceId") != self.device_id or hello.get("protocolVersion") != 1):
                raise ValueError("incompatible hello")
            versions = hello.get("supportedVersions", {})
            if not isinstance(versions, dict) or versions.get("min") != 1 or versions.get("max") != 1:
                raise ValueError("incompatible version range")
            cursor = hello.get("lastAcceptedAdapterSequence", 0)
            if type(cursor) is not int or not 0 <= cursor < 2**53 - 1:
                raise ValueError("invalid sequence")
            pending = hello.get("pendingCommandIds", [])
            if (not isinstance(pending, list) or len(pending) > 32
                    or any(not isinstance(c, str) or not 1 <= len(c) <= 128 for c in pending)
                    or len(set(pending)) != len(pending)):
                raise ValueError("invalid command journal")
            snapshot = self.snapshot()
            session_id = str(uuid.uuid4())
            self.active_session_id = session_id
            await ws.send_json({"type": "hello_ack", "protocolVersion": 1,
                "negotiatedVersion": 1, "sessionId": session_id,
                "pendingCommands": [self.rejection(c) for c in pending],
                "resume": {"mode": "snapshot"}})
            await self.send_snapshot(ws, snapshot, session_id, cursor + 1)
            self.clients.add(ws)
            self.last_hello_at = iso_now()
            LOG.info("Companion authenticated; usage snapshot sent")
            publisher = asyncio.create_task(self.publish_changes(ws, self.revision, session_id, cursor + 1))
            async for message in ws:
                if message.type == WSMsgType.TEXT:
                    command = json.loads(message.data)
                    # Usage mode offers no active turn, prompt, or microphone target.
                    # Reject commands, never execute or claim to apply them.
                    if not isinstance(command, dict) or not isinstance(command.get("commandId"), str):
                        raise ValueError("unsupported message")
                    await ws.close(code=1008, message=b"Usage display only; reconnect to reconcile")
                elif message.type == WSMsgType.BINARY:
                    await ws.close(code=1003, message=b"Audio is not supported")
        except (ValueError, TypeError, asyncio.TimeoutError):
            await ws.close(code=1008, message=b"Invalid companion protocol")
        finally:
            self.clients.discard(ws)
            if publisher is not None:
                publisher.cancel()
                with contextlib.suppress(asyncio.CancelledError, ConnectionError):
                    await publisher
        return ws

    async def shutdown(self, app):
        for ws in list(self.clients):
            await ws.close(code=1001, message=b"Adapter restarting")


def create_app(config):
    state = Companion(config)
    app = web.Application(client_max_size=65536)
    app[STATE] = state
    app.router.add_get("/healthz", state.health)
    app.router.add_get("/api/companion/v1/snapshot", state.get_snapshot)
    app.router.add_get("/api/companion/v1/ws", state.websocket)
    app.on_shutdown.append(state.shutdown)
    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    args = parser.parse_args()
    config = json.loads(args.config.expanduser().read_text())
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    web.run_app(create_app(config), host=config.get("host", "127.0.0.1"),
                port=int(config.get("port", 39073)), access_log=None, print=None)


if __name__ == "__main__":
    main()
