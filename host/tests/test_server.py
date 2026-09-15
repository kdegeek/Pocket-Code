import asyncio
import hashlib
import hmac
import json
import os
import subprocess
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from aiohttp.test_utils import TestClient, TestServer

from codexbar_companion.server import create_app


class CompanionServerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.source = Path(self.tmp.name) / "snapshot.json"
        self.source.write_text(json.dumps({"entries": [], "enabledProviders": []}))
        self.token = "synthetic-device-token"
        self.client = TestClient(TestServer(create_app({
            "device_id": "fixture-device", "snapshot_path": str(self.source),
            "token_sha256": hashlib.sha256(self.token.encode()).hexdigest(),
            "poll_seconds": 0.05,
        })))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()
        self.tmp.cleanup()

    @property
    def headers(self):
        return {"Authorization": f"Bearer {self.token}"}

    async def test_snapshot_requires_device_credential(self):
        response = await self.client.get("/api/companion/v1/snapshot")
        self.assertEqual(response.status, 401)

    async def test_authenticated_handshake_delivers_display_snapshot(self):
        ws = await self.client.ws_connect("/api/companion/v1/ws", headers=self.headers)
        await ws.send_json({"type": "hello", "protocolVersion": 1,
                            "deviceId": "fixture-device", "supportedVersions": {"min": 1, "max": 1},
                            "lastAcceptedAdapterSequence": 42, "pendingCommandIds": []})
        ack = await ws.receive_json()
        self.assertEqual(ack["resume"], {"mode": "snapshot"})
        self.assertEqual(ack["negotiatedVersion"], 1)
        frame = await ws.receive_json()
        self.assertEqual(frame["type"], "snapshot")
        self.assertEqual(frame["adapterSequence"], 43)
        self.assertEqual(frame["sessionId"], ack["sessionId"])
        self.assertEqual(frame["payload"]["pendingRequests"], [])
        self.assertEqual(frame["payload"]["usage"]["providers"][0]["status"], "unavailable")
        self.assertNotIn("turnId", frame["payload"]["workItems"][0])
        checker = os.environ.get("POCKET_CODE_WIRE_CHECK")
        if checker:
            result = subprocess.run([checker], input=json.dumps(ack) + "\n" + json.dumps(frame) + "\n",
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        await ws.close()

    async def test_updates_and_reconnects_use_contiguous_device_cursors(self):
        async def connect(cursor):
            ws = await self.client.ws_connect("/api/companion/v1/ws", headers=self.headers)
            await ws.send_json({"type": "hello", "protocolVersion": 1,
                "deviceId": "fixture-device", "supportedVersions": {"min": 1, "max": 1},
                "lastAcceptedAdapterSequence": cursor, "pendingCommandIds": []})
            await ws.receive_json()
            return ws, await ws.receive_json()
        ws, first = await connect(100000)
        self.assertEqual(first["adapterSequence"], 100001)
        # Unrelated HTTP polling must not create a gap in the live event stream.
        self.source.write_text(json.dumps({"entries": [], "generatedAt": "changed"}))
        await self.client.get("/healthz")
        update = await ws.receive_json(timeout=2)
        self.assertEqual(update["adapterSequence"], 100002)
        self.assertEqual(update["payload"]["adapterSequence"], 100002)
        await ws.close()
        ws, resumed = await connect(100002)
        self.assertEqual(resumed["adapterSequence"], 100003)
        await ws.close()

    async def test_superseded_connection_cannot_publish_old_cursors(self):
        async def connect(cursor):
            ws = await self.client.ws_connect("/api/companion/v1/ws", headers=self.headers)
            await ws.send_json({"type": "hello", "protocolVersion": 1,
                "deviceId": "fixture-device", "supportedVersions": {"min": 1, "max": 1},
                "lastAcceptedAdapterSequence": cursor})
            await ws.receive_json()
            await ws.receive_json()
            return ws
        old = await connect(10)
        current = await connect(11)
        self.source.write_text(json.dumps({"entries": [], "generatedAt": "changed"}))
        frame = await current.receive_json(timeout=2)
        self.assertEqual(frame["adapterSequence"], 13)
        with self.assertRaises(asyncio.TimeoutError):
            await old.receive_json(timeout=0.15)
        health = await (await self.client.get("/healthz")).json()
        self.assertEqual(health["lastSnapshotSequence"], 13)
        await old.close()
        await current.close()

    async def test_existing_t3_hmac_credential_authenticates(self):
        pepper = bytes(range(32))
        digest = hmac.new(pepper, b"device-token\0" + self.token.encode(), hashlib.sha256).hexdigest()
        client = TestClient(TestServer(create_app({"device_id": "fixture-device",
            "snapshot_path": str(self.source), "token_hmac_sha256": digest,
            "auth_pepper_hex": pepper.hex()})))
        await client.start_server()
        try:
            response = await client.get("/api/companion/v1/snapshot", headers=self.headers)
            self.assertEqual(response.status, 200)
            response = await client.get("/api/companion/v1/snapshot",
                                        headers={"Authorization": "Bearer incorrect"})
            self.assertEqual(response.status, 401)
        finally:
            await client.close()

    async def test_unchanged_source_does_not_advance_flash_write_cursor(self):
        first = await (await self.client.get("/api/companion/v1/snapshot", headers=self.headers)).json()
        second = await (await self.client.get("/api/companion/v1/snapshot", headers=self.headers)).json()
        self.assertEqual(first["adapterSequence"], second["adapterSequence"])

    async def test_fresh_quotas_without_token_counters_have_valid_center_text(self):
        now = datetime.now(timezone.utc)
        self.source.write_text(json.dumps({"generatedAt": now.isoformat(), "enabledProviders": ["codex"],
            "entries": [{"provider": "codex", "updatedAt": now.isoformat(),
                         "secondary": {"windowMinutes": 10080, "usedPercent": 31,
                                       "resetsAt": (now + timedelta(days=2)).isoformat()}}]}))
        body = await (await self.client.get("/api/companion/v1/snapshot", headers=self.headers)).json()
        self.assertEqual(body["workItems"][0]["activity"], "Quota usage from CodexBar")
        self.assertFalse(body["freshness"]["isStale"])

    async def test_account_identity_is_never_forwarded(self):
        self.source.write_text(json.dumps({"entries": [{"provider": "codex",
            "accountEmail": "private@example.invalid", "accessToken": "never-forward"}],
            "enabledProviders": ["codex"], "otherSecret": "never-forward"}))
        response = await self.client.get("/api/companion/v1/snapshot", headers=self.headers)
        body = await response.text()
        self.assertEqual(response.status, 200)
        self.assertNotIn("private@example", body)
        self.assertNotIn("never-forward", body)

    async def test_damaged_source_returns_unavailable_not_last_good_as_fresh(self):
        self.source.write_text("{")
        response = await self.client.get("/api/companion/v1/snapshot", headers=self.headers)
        body = await response.json()
        self.assertTrue(body["freshness"]["isStale"])
        self.assertTrue(all(p["status"] == "unavailable" for p in body["usage"]["providers"]))

    async def test_wrong_device_is_rejected_even_with_valid_credential(self):
        ws = await self.client.ws_connect("/api/companion/v1/ws", headers=self.headers)
        await ws.send_json({"type": "hello", "protocolVersion": 1, "deviceId": "wrong"})
        await ws.receive()
        self.assertEqual(ws.close_code, 1008)

    async def test_health_endpoint_does_not_expose_usage_or_credentials(self):
        response = await self.client.get("/healthz")
        body = await response.text()
        self.assertEqual(response.status, 200)
        self.assertNotIn(self.token, body)
        self.assertNotIn("usedPercent", body)


if __name__ == "__main__":
    unittest.main()
