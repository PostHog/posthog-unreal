"""Build the example first, then exercise its public launch script and the SDK mock backend."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.request


def request(host, path, value=None):
    data = None if value is None else json.dumps(value).encode()
    req = urllib.request.Request(host + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as response:
        return json.load(response)


def run_scenario(args, name, hard, delay, enabled):
    root = Path(__file__).resolve().parents[1]
    output = (root / "artifacts/runner" / name).resolve()
    output.mkdir(parents=True, exist_ok=True)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    host = f"http://127.0.0.1:{port}"
    with (output / "mock.log").open("w") as log:
        server = subprocess.Popen([sys.executable, args.mock_server], env={**os.environ, "PORT": str(port)}, stdout=log, stderr=log)
        try:
            for _ in range(100):
                try:
                    request(host, "/__captured")
                    break
                except OSError:
                    if server.poll() is not None:
                        raise RuntimeError(f"Mock exited; see {output / 'mock.log'}")
                    time.sleep(0.05)
            else:
                raise RuntimeError("Mock startup timed out")
            request(host, "/__control/state", {"flags": {"runner-hard-mode": hard}, "delays": {"flags": delay}})
            env = {k: v for k, v in os.environ.items() if k not in ("POSTHOG_TOKEN", "POSTHOG_HOST")}
            env.update(UE_ROOT=args.ue_root, RUNNER_TEST_OUTPUT=str(output),
                       RUNNER_EXPECT_HARD=str(int(hard and delay == 0)),
                       RUNNER_EXPECT_SECOND_HARD=str(int(hard and delay < 3)),
                       RUNNER_SHORT_COUNTDOWN=str(int(name == "delayed")))
            if enabled:
                env.update(POSTHOG_TOKEN="phc_MOCK", POSTHOG_HOST=host)
            result_path = output / "smoke-result.json"
            result_path.unlink(missing_ok=True)
            with (output / "stdout.log").open("w") as editor_log:
                subprocess.run([str(root / "scripts/run-example.sh"), "-unattended", "-nullrhi", "-nosplash", "-nop4",
                                "-EnablePlugins=PythonScriptPlugin", f"-ExecutePythonScript={root / 'Tests/runner_smoke.py'}",
                                "-ExecCmds=t.MaxFPS 60", f"-abslog={output / 'editor.log'}"],
                               env=env, stdout=editor_log, stderr=subprocess.STDOUT, timeout=150, check=True)
            result = json.loads(result_path.read_text())
            assert result["success"], result
            events = request(host, "/__captured/events")
            (output / "events.json").write_text(json.dumps(events, indent=2))
            if enabled:
                starts = [e for e in events if e["event"] == "run_started"]
                ends = [e for e in events if e["event"] == "run_ended"]
                coins = [e for e in events if e["event"] == "coin_collected"]
                assert len(starts) == len(ends) == 2, [e["event"] for e in events]
                assert coins, "No coin events delivered"
                ids = {e["properties"]["run_id"] for e in starts}
                assert len(ids) == 2 and ids == {e["properties"]["run_id"] for e in ends}
                assert all(e["properties"]["run_id"] in ids for e in coins)
                assert len({e["distinct_id"] for e in starts + ends + coins}) == 1, "Identity changed on restart"
                assert len({e["properties"]["$session_id"] for e in starts + ends}) == 1, "Session changed on restart"
                assert starts[0]["properties"]["difficulty"] == ("hard" if hard and delay == 0 else "normal")
                assert starts[1]["properties"]["difficulty"] == ("hard" if hard and delay < 3 else "normal")
                assert all(e["properties"]["duration_seconds"] > 0 and e["properties"]["reason"] == "collision" for e in ends)
                assert any(e["properties"]["distance_meters"] > 700 for e in ends)
            else:
                assert not events, "Unconfigured demo sent events"
                assert request(host, "/__captured")["flags_calls"] == 0
            print(f"PASS {name}: {len(result['checks'])} gameplay checks, {len(events)} ingested events", flush=True)
        finally:
            server.terminate()
            server.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ue-root", default=os.environ.get("UE_ROOT"), required=not os.environ.get("UE_ROOT"))
    parser.add_argument("--mock-server", required=True)
    parser.add_argument("--scenario", choices=["offline", "normal", "hard", "delayed", "timeout"])
    args = parser.parse_args()
    for name, hard, delay, enabled in [("offline", False, 0, False), ("normal", False, 0, True),
                                        ("hard", True, 0, True), ("delayed", True, 2, True), ("timeout", True, 5, True)]:
        if args.scenario is None or args.scenario == name:
            run_scenario(args, name, hard, delay, enabled)


if __name__ == "__main__":
    main()
