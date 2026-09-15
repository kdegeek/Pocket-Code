#!/usr/bin/env python3
"""Install the CodexBar companion adapter as a per-user macOS LaunchAgent."""
import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

LABEL = "com.kdegeek.pocket-code-companion"


def _job_loaded(domain):
    return subprocess.run(["launchctl", "print", f"{domain}/{LABEL}"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def _bootstrap(domain, agent, loaded_was_true):
    if loaded_was_true:
        # bootout returns before an existing process has finished shutting down.
        for _ in range(30):
            if not _job_loaded(domain):
                break
            time.sleep(1)
        else:
            raise RuntimeError("Previous companion service did not finish shutting down")
    command = ["launchctl", "bootstrap", domain, str(agent)]
    result = subprocess.run(command)
    if result.returncode == 0:
        return
    if result.returncode != 5 or not loaded_was_true or _job_loaded(domain):
        result.check_returncode()

    for _ in range(5):
        time.sleep(1)
        result = subprocess.run(command)
        if result.returncode == 0:
            return
        if result.returncode != 5 or _job_loaded(domain):
            result.check_returncode()
    result.check_returncode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path,
                        help="Private JSON configuration with the enrolled device credential hash")
    args = parser.parse_args()
    config = args.config.expanduser().resolve()
    config.chmod(0o600)
    root = Path.home() / "Library/Application Support/Pocket-Code"
    runtime = root / "runtime"
    runtime.mkdir(parents=True, exist_ok=True, mode=0o700)
    logs = Path.home() / "Library/Logs/Pocket-Code"
    logs.mkdir(parents=True, exist_ok=True, mode=0o700)
    source = Path(__file__).resolve().parent
    python = runtime / ".venv/bin/python"
    if not python.exists():
        subprocess.run([sys.executable, "-m", "venv", str(runtime / ".venv")], check=True)
    subprocess.run([str(python), "-m", "pip", "install", "-r",
                    str(source / "requirements-companion.txt")], check=True)
    # Validate with exactly the same parser used by the running service, after
    # the runtime has all of its dependencies and before unloading the agent.
    subprocess.run([
        str(python), "-c",
        "import json, sys; from pathlib import Path; "
        "from codexbar_companion.server import Companion; "
        "Companion(json.loads(Path(sys.argv[1]).read_text()))",
        str(config),
    ], check=True, cwd=str(source))
    domain = f"gui/{os.getuid()}"
    loaded_was_true = _job_loaded(domain)
    if loaded_was_true:
        subprocess.run(["launchctl", "bootout", f"{domain}/{LABEL}"], check=True)
    shutil.copytree(source / "codexbar_companion", runtime / "codexbar_companion",
                    dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    agent = Path.home() / "Library/LaunchAgents" / f"{LABEL}.plist"
    agent.parent.mkdir(parents=True, exist_ok=True)
    data = {"Label": LABEL, "ProgramArguments": [str(python), "-m", "codexbar_companion.server",
            "--config", str(config)], "WorkingDirectory": str(runtime),
            "RunAtLoad": True, "KeepAlive": True, "ThrottleInterval": 10,
            "StandardOutPath": str(logs / "adapter.log"),
            "StandardErrorPath": str(logs / "adapter-error.log"),
            "EnvironmentVariables": {"PYTHONUNBUFFERED": "1"}}
    agent.write_bytes(plistlib.dumps(data))
    agent.chmod(0o600)
    _bootstrap(domain, agent, loaded_was_true)
    print(f"Installed {LABEL}; configuration: {config}")


if __name__ == "__main__":
    main()
