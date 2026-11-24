#!/usr/bin/env python3
"""Minimal local PostHog HTTP mock server (stdlib only) for Unreal Editor automation & example smoke."""
from datetime import datetime, timezone
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import threading
import time


class MockState:
    def __init__(self):
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        with self.lock:
            self.events = []
            self.snapshots = []
            self.flags_calls = []
            self.flags = {}
            self.delays = {}
            self.session_recording = True


STATE = MockState()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length > 0 else b""
        if self.headers.get("Content-Encoding", "").lower() == "gzip" or raw[:2] == b"\x1f\x8b":
            raw = gzip.decompress(raw)
        return json.loads(raw.decode("utf-8")) if raw else {}

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/__captured":
            with STATE.lock:
                self._send_json(200, {
                    "events": len(STATE.events),
                    "snapshots": len(STATE.snapshots),
                    "flags_calls": len(STATE.flags_calls),
                })
            return
        if path == "/__captured/events":
            with STATE.lock:
                self._send_json(200, list(STATE.events))
            return
        if path == "/__captured/snapshots":
            with STATE.lock:
                self._send_json(200, list(STATE.snapshots))
            return
        if path == "/__captured/all":
            with STATE.lock:
                self._send_json(200, {
                    "events": list(STATE.events),
                    "snapshots": list(STATE.snapshots),
                    "flags_calls": list(STATE.flags_calls),
                })
            return
        if path.startswith("/array/") and path.endswith("/config"):
            with STATE.lock:
                recording = STATE.session_recording
            self._send_json(200, {
                "hasFeatureFlags": True,
                "sessionRecording": recording,
            })
            return
        self._send_json(404, {"error": "not found"})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        body = self._read_json()

        if path == "/__control/reset":
            STATE.reset()
            self._send_json(200, {"ok": True})
            return
        if path == "/__control/state":
            with STATE.lock:
                if "flags" in body:
                    STATE.flags = body["flags"]
                if "delays" in body:
                    STATE.delays = body["delays"]
                if "sessionReplayEnabled" in body:
                    STATE.session_recording = bool(body["sessionReplayEnabled"])
            self._send_json(200, {"ok": True})
            return
        if path in ("/i/v1/analytics/events", "/batch", "/batch/"):
            batch = body.get("batch", [])
            if isinstance(batch, list):
                with STATE.lock:
                    STATE.events.extend(batch)
            self._send_json(200, {"results": {}})
            return
        if path in ("/s", "/s/"):
            with STATE.lock:
                STATE.snapshots.append(body)
            self._send_json(200, {"status": 1})
            return
        if path in ("/flags", "/flags/"):
            with STATE.lock:
                delay = float(STATE.delays.get("flags", 0))
            if delay > 0:
                time.sleep(delay)
            with STATE.lock:
                STATE.flags_calls.append({
                    "ts": datetime.now(timezone.utc).isoformat(),
                    "body": body,
                })
                flags_out = {}
                for key, val in STATE.flags.items():
                    if isinstance(val, dict) and "enabled" in val:
                        flags_out[key] = {"key": key, **val}
                    elif isinstance(val, str):
                        flags_out[key] = {"key": key, "enabled": True, "variant": val}
                    else:
                        flags_out[key] = {"key": key, "enabled": bool(val)}
            self._send_json(200, {
                "flags": flags_out,
                "errorsWhileComputingFlags": False,
                "requestId": "mock-req-1",
            })
            return
        self._send_json(404, {"error": "not found"})


def main():
    port = int(os.environ.get("PORT", "8080"))
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
