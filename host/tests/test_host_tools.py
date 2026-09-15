import io
import hashlib
import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from contextlib import closing, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


HOST = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HOST))

import install_companion


class HostToolTests(unittest.TestCase):
    def _run_import(self, scopes):
        with tempfile.TemporaryDirectory() as directory:
            userdata = Path(directory)
            (userdata / "secrets").mkdir()
            (userdata / "secrets/companion-auth-pepper.bin").write_bytes(bytes(32))
            database = userdata / "state.sqlite"
            with closing(sqlite3.connect(database)) as connection:
                connection.execute(
                    "CREATE TABLE companion_devices "
                    "(device_id TEXT, status TEXT, credential_hash TEXT, scopes_json TEXT)"
                )
                connection.execute(
                    "INSERT INTO companion_devices VALUES (?, ?, ?, ?)",
                    ("fixture-device", "active", "hmac-sha256:" + "a" * 64,
                     json.dumps(scopes)),
                )
                connection.commit()
            output = userdata / "companion.json"
            result = subprocess.run(
                [sys.executable, str(HOST / "import_companion.py"),
                 "--userdata", str(userdata), "--device-id", "fixture-device",
                 "--host", "192.168.1.7", "--output", str(output)],
                capture_output=True,
                text=True,
            )
            return result, output.exists()

    def test_import_rejects_non_list_scopes_even_when_text_contains_read_scope(self):
        result, output_exists = self._run_import("prefix companion:read suffix")

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output_exists)
        self.assertIn("No active companion with read scope", result.stderr)

    def test_import_accepts_exact_read_scope_in_string_list(self):
        result, output_exists = self._run_import(["companion:read", "companion:status"])

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output_exists)

    def test_install_validates_with_runtime_interpreter_before_unloading_agent(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            runtime_python = home / "Library/Application Support/Pocket-Code/runtime/.venv/bin/python"
            runtime_python.parent.mkdir(parents=True)
            runtime_python.write_text("placeholder")
            config = home / "companion.json"
            config.write_text(json.dumps({
                "device_id": "fixture-device",
                "snapshot_path": str(home / "snapshot.json"),
                "token_sha256": hashlib.sha256(b"fixture-token").hexdigest(),
                "poll_seconds": 10,
                "stale_seconds": 900,
            }))
            calls = []

            def fake_run(command, **kwargs):
                calls.append((list(command), kwargs))
                if command[:2] == ["launchctl", "print"]:
                    return SimpleNamespace(returncode=1)
                return SimpleNamespace(returncode=0)

            with patch.object(Path, "home", return_value=home), \
                    patch.object(install_companion.subprocess, "run", side_effect=fake_run):
                with patch.object(sys, "argv", ["install_companion.py", "--config", str(config)]):
                    with redirect_stdout(io.StringIO()):
                        install_companion.main()

            pip_index = next(
                index for index, (command, _) in enumerate(calls)
                if command[:4] == [str(runtime_python), "-m", "pip", "install"]
            )
            validation_index = next(
                index for index, (command, _) in enumerate(calls)
                if command[:2] == [str(runtime_python), "-c"]
            )
            launchctl_index = next(
                index for index, (command, _) in enumerate(calls)
                if command[:2] == ["launchctl", "print"]
            )

            self.assertLess(pip_index, validation_index)
            self.assertLess(validation_index, launchctl_index)
            self.assertEqual(calls[validation_index][1]["cwd"], str(HOST))

    def test_install_retries_only_after_loaded_agent_bootstrap_race(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            runtime_python = home / "Library/Application Support/Pocket-Code/runtime/.venv/bin/python"
            runtime_python.parent.mkdir(parents=True)
            runtime_python.write_text("placeholder")
            config = home / "companion.json"
            config.write_text(json.dumps({
                "device_id": "fixture-device",
                "snapshot_path": str(home / "snapshot.json"),
                "token_sha256": hashlib.sha256(b"fixture-token").hexdigest(),
                "poll_seconds": 10,
                "stale_seconds": 900,
            }))
            calls = []
            print_results = iter((0, 1))
            bootstrap_results = iter((5, 0))

            def fake_run(command, **kwargs):
                calls.append((list(command), kwargs))
                if command[:2] == ["launchctl", "print"]:
                    return SimpleNamespace(returncode=next(print_results))
                if command[:2] == ["launchctl", "bootstrap"]:
                    return SimpleNamespace(returncode=next(bootstrap_results))
                return SimpleNamespace(returncode=0)

            with patch.object(Path, "home", return_value=home), \
                    patch.object(install_companion.subprocess, "run", side_effect=fake_run), \
                    patch.object(install_companion, "time", create=True) as clock:
                with patch.object(sys, "argv", ["install_companion.py", "--config", str(config)]):
                    with redirect_stdout(io.StringIO()):
                        install_companion.main()

            bootstrap_calls = [command for command, _ in calls
                               if command[:2] == ["launchctl", "bootstrap"]]
            self.assertEqual(len(bootstrap_calls), 2)
            clock.sleep.assert_called_once_with(1)


if __name__ == "__main__":
    unittest.main()
