"""Public C ABI integration tests against the maintained SDK mock server.

Run with Python 3.11+:
  python3 Tests/native_integration.py --library <bridge dylib/so/dll> --mock-server <server.py>
"""
import argparse
import concurrent.futures
import ctypes as c
import datetime
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
import uuid


class Bytes(c.Structure):
    _fields_ = [("data", c.c_void_p), ("len", c.c_size_t)]


class Buffer(c.Structure):
    _fields_ = [("data", c.c_void_p), ("len", c.c_size_t)]


def encoded(value):
    data = json.dumps(value, ensure_ascii=False).encode()
    storage = c.create_string_buffer(data)
    return storage, Bytes(c.cast(storage, c.c_void_p), len(data))


class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = c.CDLL(ARGS.library)
        for name, args in {
            "phu_create": [c.c_uint32, Bytes, c.POINTER(c.c_uint64)],
            "phu_capture": [c.c_uint64, Bytes],
            "phu_flags": [c.c_uint64, Bytes, c.POINTER(Buffer)],
            "phu_checkpoint": [c.c_uint64, c.POINTER(Buffer)],
            "phu_flush": [c.c_uint64], "phu_close": [c.c_uint64],
            "phu_begin_shutdown": [c.c_uint64],
            "phu_opt_in": [c.c_uint64], "phu_opt_out": [c.c_uint64],
        }.items():
            function = getattr(cls.lib, name)
            function.argtypes, function.restype = args, c.c_int32
        cls.lib.phu_buffer_free.argtypes = [Buffer]
        cls.lib.phu_buffer_free.restype = None

    @classmethod
    def start_server(cls):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        cls.host = f"http://127.0.0.1:{port}"
        cls.log = tempfile.TemporaryFile()
        cls.server = subprocess.Popen([sys.executable, ARGS.mock_server], env={**os.environ, "PORT": str(port)}, stdout=cls.log, stderr=cls.log)
        try:
            for _ in range(100):
                try:
                    cls.http("/__captured")
                    break
                except OSError:
                    if cls.server.poll() is not None:
                        cls.log.seek(0)
                        diagnostic = cls.log.read().decode(errors="replace")
                        raise RuntimeError(f"Mock server exited during startup:\n{diagnostic}")
                    time.sleep(0.05)
            else:
                raise RuntimeError("Mock server did not become ready")
        except BaseException:
            cls.server.terminate()
            cls.server.wait(timeout=10)
            cls.log.close()
            raise

    @classmethod
    def stop_server(cls):
        cls.server.terminate()
        cls.server.wait(timeout=10)
        cls.log.close()

    @classmethod
    def http(cls, path, value=None):
        data = None if value is None else json.dumps(value).encode()
        request = urllib.request.Request(cls.host + path, data=data, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.load(response)

    def setUp(self):
        self.start_server()
        self.http("/__control/reset", {})
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.handles = []

    def tearDown(self):
        for handle in self.handles:
            self.lib.phu_close(handle)
        self.temporary.cleanup()
        self.stop_server()

    def create(self, **options):
        config = {"project_token": "phc_MOCK", "host": self.host,
                  "storage_path": str(self.directory / "spool"), "flush_interval_ms": 60000,
                  "shutdown_timeout_ms": 200, **options}
        storage, data = encoded(config)
        handle = c.c_uint64()
        self.assertEqual(self.lib.phu_create(1, data, c.byref(handle)), 0)
        self.handles.append(handle.value)
        return handle.value

    def capture(self, handle, **overrides):
        event = {"event": "native integration", "distinct_id": "玩家 🚀",
                 "uuid": str(uuid.uuid4()), "timestamp": "2026-01-01T12:00:00Z",
                 "properties": {"$lib": "posthog-unreal", "$lib_version": "0.0.1", "number": 123, "string": "00123", "nested": [False, None, {"x": "ü"}]}, **overrides}
        storage, data = encoded(event)
        return self.lib.phu_capture(handle, data), event

    def checkpoint(self, handle):
        output = Buffer()
        self.assertEqual(self.lib.phu_checkpoint(handle, c.byref(output)), 0)
        try:
            return json.loads(c.string_at(output.data, output.len))
        finally:
            self.lib.phu_buffer_free(output)

    def flags(self, handle):
        storage, data = encoded({"distinct_id": "user"})
        output = Buffer()
        status = self.lib.phu_flags(handle, data, c.byref(output))
        try:
            return status, json.loads(c.string_at(output.data, output.len)) if output.data else None
        finally:
            self.lib.phu_buffer_free(output)

    def test_capture_snapshot_and_side_effect_free_flags(self):
        handle = self.create()
        status, event = self.capture(handle)
        self.assertEqual(status, 1)
        self.assertEqual(self.checkpoint(handle)["persistence_errors"], 0)
        self.assertEqual(self.lib.phu_flush(handle), 0)
        wire = self.http("/__captured/events")
        self.assertEqual(len(wire), 1)
        for key in ("uuid", "timestamp", "distinct_id", "properties"):
            self.assertEqual(wire[0][key], event[key])
        status, flags = self.flags(handle)
        self.assertEqual(status, 0)
        self.assertIs(flags["flags"]["disabled-flag"]["value"], False)
        self.assertEqual(flags["flags"]["string-value"]["value"], "test")
        self.assertNotIn("missing", flags["flags"])
        self.assertEqual(self.lib.phu_flush(handle), 0)
        self.assertEqual(len(self.http("/__captured/events")), 1)
        self.assertEqual(self.checkpoint(handle)["delivered"], 1)
        self.assertEqual(list((self.directory / "spool").glob("*.json")), [])

    def test_offline_close_restart_replays_original_event(self):
        self.http("/__control/state", {"force500": {"batch": True}})
        handle = self.create()
        status, event = self.capture(handle)
        self.assertEqual(status, 1)
        self.assertEqual(self.checkpoint(handle)["pending"], 1)
        self.assertEqual(self.lib.phu_close(handle), 0)
        self.assertEqual(len(list((self.directory / "spool").glob("*.json"))), 1)
        self.http("/__control/reset", {})
        restarted = self.create()
        self.assertEqual(self.lib.phu_flush(restarted), 0)
        wire = self.http("/__captured/events")
        self.assertEqual(len(wire), 1)
        for key in ("uuid", "timestamp", "distinct_id", "properties"):
            self.assertEqual(wire[0][key], event[key])

    def test_opt_out_purges_and_blocks_events_and_flags(self):
        handle = self.create()
        self.assertEqual(self.capture(handle)[0], 1)
        self.checkpoint(handle)
        self.assertEqual(self.lib.phu_opt_out(handle), 0)
        self.assertEqual(self.capture(handle)[0], 7)
        self.assertEqual(self.flags(handle), (7, None))
        self.assertEqual(self.checkpoint(handle)["pending"], 0)
        self.assertEqual(list((self.directory / "spool").glob("*.json")), [])
        self.assertEqual(self.lib.phu_opt_in(handle), 0)
        self.assertEqual(self.capture(handle, event="after opt in")[0], 1)
        self.lib.phu_flush(handle)
        self.assertEqual([e["event"] for e in self.http("/__captured/events")], ["after opt in"])

    def test_restored_opt_out_purges_before_replay(self):
        self.http("/__control/state", {"force500": {"batch": True}})
        handle = self.create()
        self.capture(handle)
        self.checkpoint(handle)
        self.lib.phu_close(handle)
        self.http("/__control/reset", {})
        handle = self.create(opted_out=True)
        self.assertEqual(self.checkpoint(handle)["pending"], 0)
        self.assertEqual(list((self.directory / "spool").glob("*.json")), [])
        self.lib.phu_flush(handle)
        self.assertEqual(self.http("/__captured/events"), [])

    def test_queue_bound_and_concurrent_close(self):
        handle = self.create(max_queue_size=2, max_batch_size=2, flush_at=2)
        self.http("/__control/state", {"delays": {"batch": 1}})
        self.assertEqual(self.capture(handle)[0], 1)
        self.assertEqual(self.capture(handle)[0], 1)
        self.assertEqual(self.capture(handle)[0], 8)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.lib.phu_close(handle), range(4)))
        self.assertEqual(results.count(0), 1)
        self.assertEqual(results.count(3), 3)
        self.assertEqual(self.capture(handle)[0], 3)

    def test_close_stalled_request_and_retry_backlog(self):
        self.http("/__control/state", {"delays": {"batch": 2}})
        handle = self.create(flush_at=20, max_batch_size=1)
        for _ in range(10):
            self.assertEqual(self.capture(handle)[0], 1)
        self.assertEqual(self.checkpoint(handle)["persistence_errors"], 0)
        with concurrent.futures.ThreadPoolExecutor() as pool:
            flush = pool.submit(self.lib.phu_flush, handle)
            time.sleep(0.03)
            started = time.monotonic()
            self.assertEqual(self.lib.phu_close(handle), 0)
            elapsed = time.monotonic() - started
            self.assertEqual(flush.result(timeout=1), 0)
        self.assertLess(elapsed, 0.5, f"close took {elapsed}s with a 200ms budget")
        self.assertEqual(len(list((self.directory / "spool").glob("*.json"))), 10)

    def test_unreal_editor_native_integration(self):
        if not ARGS.editor:
            self.skipTest("Pass --editor to validate the Unreal runtime module")
        root = Path(__file__).resolve().parents[1]
        evidence = root / "artifacts" / "editor-integration"
        evidence.mkdir(parents=True, exist_ok=True)
        report = evidence / str(uuid.uuid4())
        # Engine helper processes can inherit stdout. A file lets us wait for
        # the editor itself rather than waiting for those helpers to close a pipe.
        controls = report / "controls"
        controls.mkdir(parents=True)
        with (evidence / "stdout.log").open("w") as output:
            process = subprocess.Popen([
                ARGS.editor, str(root / "Examples/PostHogExample/PostHogExample.uproject"),
                "-unattended", "-nullrhi", "-nosound", "-nosplash", "-nop4", "-NoAssetRegistryCache",
                "-ExecCmds=Automation RunTests PostHog.;SoftQuit",
                f"-ReportExportPath={report}",
                f"-abslog={evidence / 'editor.log'}", "-stdout", "-FullStdOutLogOutput",
            ], env={**os.environ, "POSTHOG_TEST_HOST": self.host,
                    "POSTHOG_TEST_CONTROL_DIRECTORY": str(controls)}, stdout=output, stderr=subprocess.STDOUT, text=True)
            try:
                deadline = time.monotonic() + 120
                handled = set()
                while process.poll() is None:
                    if time.monotonic() >= deadline:
                        raise TimeoutError("Unreal automation exceeded 120 seconds")
                    for request_path in controls.glob("request-*.json"):
                        if request_path.name in handled:
                            continue
                        request = json.loads(request_path.read_text())
                        self.assertIn(request["path"], ("/__control/state", "/__captured/all"))
                        response = {"data": self.http(request["path"], request.get("body"))}
                        target = controls / request_path.name.replace("request-", "response-")
                        temporary = target.with_suffix(".tmp")
                        temporary.write_text(json.dumps(response))
                        temporary.replace(target)
                        handled.add(request_path.name)
                    time.sleep(0.01)
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=15)
        self.assertEqual(process.returncode, 0, str(evidence / "stdout.log"))
        result = json.loads((report / "index.json").read_text(encoding="utf-8-sig"))
        self.assertEqual(result["failed"], 0, result)
        self.assertEqual(result["succeeded"], 8, result)
        wire = self.http("/__captured/events")
        events = {event["event"]: event for event in wire}
        self.assertEqual(events["unreal_native_integration"]["properties"]["$lib"], "posthog-unreal")
        for index in range(3):
            name = f"stateful_nonfinite_{index}"
            self.assertEqual(sum(event["event"] == name for event in wire), 1)
            numeric = events[name]["properties"]
            self.assertNotIn("bad", numeric)
            self.assertEqual(numeric["nested"], {"kept": 7})
            self.assertEqual(numeric["items"], [None, 8, False, "NaN", None, {"kept": 7}])
            self.assertEqual(numeric["finite"], 42.5)
            self.assertEqual(numeric["literal"], "Infinity")
            json.dumps(numeric, allow_nan=False)
        self.assertEqual(sum(event["event"] == "stateful_numeric_neighbor" for event in wire), 1)
        self.assertEqual(events["stateful_numeric_neighbor"]["properties"]["finite"], 123)
        anonymous = events["stateful_anonymous"]
        identified = events["stateful_identified"]
        reset = events["stateful_reset"]
        self.assertEqual(anonymous["properties"]["precedence"], "caller")
        self.assertEqual(anonymous["properties"]["string"], "00123")
        self.assertEqual(anonymous["properties"]["array"], [None, {"kept": 1}])
        self.assertNotIn("null", anonymous["properties"])
        self.assertIs(anonymous["properties"]["$is_identified"], False)
        self.assertIs(anonymous["properties"]["$process_person_profile"], False)
        self.assertEqual(identified["distinct_id"], "user-one")
        self.assertEqual(identified["properties"]["$groups"], {"company": "acme"})
        self.assertEqual(identified["properties"]["precedence"], "context")
        self.assertEqual(anonymous["properties"]["$session_id"], identified["properties"]["$session_id"])
        self.assertNotEqual(reset["distinct_id"], anonymous["distinct_id"])
        self.assertNotEqual(reset["properties"]["$session_id"], identified["properties"]["$session_id"])
        self.assertNotIn("registered", reset["properties"])
        self.assertNotIn("$groups", reset["properties"])
        identity_events = [event for event in wire if event["event"] == "$identify" and event["distinct_id"] == "user-one"]
        self.assertEqual(len(identity_events), 1)
        self.assertEqual(identity_events[0]["properties"]["$anon_distinct_id"], anonymous["distinct_id"])
        self.assertEqual(sum(event["event"] == "$set" for event in wire), 1)
        self.assertNotIn("secret", events["stateful_redacted"]["properties"])
        self.assertNotIn("$lib", events["stateful_redacted"]["properties"])
        for name in ("ignored", "closed", "hook_withdraw", "hook_withdraw_resume", "must_not_replay", "$reset"):
            self.assertNotIn(name, events)
        self.assertNotIn("$session_id", events["explicit_before"]["properties"])
        self.assertIn("$session_id", events["explicit_during"]["properties"])
        self.assertNotIn("$session_id", events["explicit_after"]["properties"])
        self.assertNotIn("payload", events["stateful_bounds"]["properties"])
        self.assertFalse(events["stateful_never"]["properties"]["$process_person_profile"])
        parallel_a = [event for event in wire if event["event"] == "parallel_a"]
        parallel_b = [event for event in wire if event["event"] == "parallel_b"]
        self.assertEqual(len(parallel_a), 25)
        self.assertEqual(len(parallel_b), 25)
        self.assertEqual(len({event["distinct_id"] for event in parallel_a}), 1)
        self.assertEqual(len({event["distinct_id"] for event in parallel_b}), 1)
        self.assertNotEqual(parallel_a[0]["distinct_id"], parallel_b[0]["distinct_id"])
        analytics = events["analytics_before"]
        props = analytics["properties"]
        self.assertEqual(props["precedence"], "caller")
        self.assertEqual(props["registered_wins"], "registered")
        self.assertEqual(props["context_wins"], "context")
        self.assertEqual(props["default_only"], 123)
        self.assertEqual(props["string"], "00123")
        self.assertEqual(props["number"], 42)
        self.assertIs(props["boolean"], False)
        self.assertEqual(props["object"], {"nested": True})
        self.assertEqual(props["array"], [None, 1, False])
        for key in ("invalid", "secret", "$session_id"):
            self.assertNotIn(key, props)
        native_props = events["analytics_native"]["properties"]
        self.assertNotIn("default_only", native_props)
        self.assertEqual(native_props["registered_wins"], "registered")
        self.assertEqual(native_props["context_wins"], "context")
        self.assertEqual(events["analytics_native"]["distinct_id"], analytics["distinct_id"])
        self.assertIn("$session_id", events["analytics_during"]["properties"])
        self.assertNotIn("$session_id", events["analytics_after"]["properties"])
        self.assertEqual(events["analytics_custom_session"]["properties"]["$session_id"],
                         "01912345-6789-7abc-8def-0123456789ab")
        replaced = events["analytics_replaced_defaults"]["properties"]
        self.assertIs(replaced["replacement"], True)
        self.assertIs(replaced["registered_only"], True)
        self.assertEqual(replaced["registered_wins"], "registered")
        self.assertNotIn("default_only", replaced)
        auto_before = events["analytics_auto_before"]
        auto_after = events["analytics_auto_after"]
        self.assertNotEqual(auto_before["properties"]["$session_id"], auto_after["properties"]["$session_id"])
        self.assertNotEqual(auto_before["distinct_id"], analytics["distinct_id"])
        starts = [event for event in wire if event["event"] == "SessionStart" and event["properties"].get("test_suite") == "analytics"]
        ends = [event for event in wire if event["event"] == "SessionEnd" and event["properties"].get("test_suite") == "analytics"]
        self.assertEqual(len(starts), 2)
        self.assertEqual(len(ends), 3)
        explicit_start = next(event for event in starts if event["distinct_id"] == analytics["distinct_id"])
        self.assertEqual(explicit_start["properties"]["chapter"], "intro")
        self.assertEqual(explicit_start["properties"]["$session_id"], events["analytics_during"]["properties"]["$session_id"])
        self.assertTrue(any(event["properties"]["$session_id"] == explicit_start["properties"]["$session_id"] for event in ends))
        self.assertEqual(events["analytics_stock_blueprint"]["properties"]["string"], "00123")
        for name in ("analytics_stock_blueprint", "analytics_multicast", "analytics_immediate",
                     "analytics_retained_first", "analytics_retained_multicast", "analytics_restart"):
            self.assertEqual(events[name]["distinct_id"], "analytics-user")
        self.assertEqual(events["analytics_bound_second"]["distinct_id"], auto_before["distinct_id"])
        self.assertEqual(events["Game Error"]["properties"]["Error"], "ordinary-error")
        analytics_identify = next(event for event in wire if event["event"] == "$identify" and event["distinct_id"] == "analytics-user")
        self.assertEqual(analytics_identify["properties"]["$anon_distinct_id"], analytics["distinct_id"])
        for name in ("analytics_opted_out", "analytics_closed"):
            self.assertNotIn(name, events)
        blueprint = events["blueprint_anonymous"]
        self.assertEqual(blueprint["properties"]["string"], "00123 玩家 🚀")
        self.assertEqual(blueprint["properties"]["number"], 42)
        self.assertIs(blueprint["properties"]["boolean"], False)
        self.assertEqual(blueprint["properties"]["array"], [None, {"string": "00123 玩家 🚀", "number": 42, "boolean": False}])
        self.assertEqual(events["blueprint_identified"]["distinct_id"], "blueprint-user")
        blueprint_identify = next(event for event in wire if event["event"] == "$identify" and event["distinct_id"] == "blueprint-user")
        self.assertEqual(blueprint_identify["properties"]["$anon_distinct_id"], blueprint["distinct_id"])
        self.assertEqual(blueprint_identify["properties"]["$set"], {"name": "Blueprint player"})
        self.assertEqual(blueprint_identify["properties"]["$set_once"], {"origin": "blueprint"})
        self.assertNotIn("blueprint_not_ready", events)
        self.assertNotIn("blueprint_opted_out", events)
        first = events["subsystem_one"]
        second = events["subsystem_two"]
        self.assertNotEqual(first["distinct_id"], second["distinct_id"])
        self.assertNotEqual(first["properties"]["$session_id"], second["properties"]["$session_id"])
        for event in (first, second, events["subsystem_auto"], events["subsystem_replacement"]):
            self.assertTrue(event["properties"]["$engine_version"])
            self.assertTrue(event["properties"]["$os"])
        lifecycle = [event for event in wire if event["event"].startswith("Application ")]
        main_lifecycle = [event for event in lifecycle if event["properties"].get("instance") in ("one", "two", "one-updated")]
        self.assertEqual(sum(event["event"] == "Application Installed" for event in main_lifecycle), 2)
        self.assertEqual(sum(event["event"] == "Application Backgrounded" for event in main_lifecycle), 2)
        self.assertEqual(sum(event["event"] == "Application Opened" for event in main_lifecycle), 5)
        updates = [event for event in main_lifecycle if event["event"] == "Application Updated"]
        self.assertEqual(len(updates), 1)
        self.assertEqual(updates[0]["properties"]["previous_version"], "1.0")
        self.assertEqual(updates[0]["properties"]["previous_build"], "100")
        self.assertEqual(updates[0]["properties"]["$app_version"], "1.1")
        self.assertEqual(updates[0]["properties"]["$app_build"], "110")
        self.assertFalse(any(event["distinct_id"] == events["subsystem_auto"]["distinct_id"] for event in lifecycle))
        self.assertNotIn("subsystem_after_shutdown", events)
        for event in lifecycle:
            self.assertNotIn("version", event["properties"])
            self.assertNotIn("build", event["properties"])
        exposures = [event for event in wire if event["event"] == "$feature_flag_called"]
        blueprint_exposures = [event for event in exposures if event["properties"].get("test_suite") == "blueprint"]
        self.assertEqual([event["properties"]["$feature_flag"] for event in blueprint_exposures], ["disabled-flag"])
        initial = [event for event in exposures if event["properties"].get("stage") == "initial"]
        self.assertEqual(len(initial), 3)
        self.assertEqual({event["properties"]["$feature_flag"] for event in initial},
                         {"bool-value", "disabled-flag", "string-value"})
        self.assertEqual(len([event for event in exposures if event["properties"].get("stage") == "restored"]), 4)
        minimal = next(event["properties"] for event in exposures if event["properties"]["$feature_flag"] == "minimal")
        for key in ("test_suite", "stage", "hook_private", "$referrer", "$lib_version"):
            self.assertNotIn(key, minimal)
        self.assertEqual(minimal["utm_source"], "campaign")
        self.assertEqual(minimal["$referring_domain"], "example.com")
        self.assertEqual(minimal["$feature_flag_request_id"], "rich-request")
        self.assertIs(minimal["$feature_flag_has_experiment"], False)
        self.assertEqual(minimal["$feature_flag_id"], 9)
        for flag in ("experiment", "unknown", "ungated"):
            full = next(event["properties"] for event in exposures if event["properties"]["$feature_flag"] == flag)
            self.assertEqual(full["hook_private"], "custom")
            self.assertEqual(full["stage"], "restored")
            self.assertIn("$referrer", full)
            self.assertNotIn("$lib_version", full)
        enriched = events["flags_enriched"]["properties"]
        self.assertIs(enriched["$feature/bool-value"], True)
        self.assertIs(enriched["$feature/disabled-flag"], False)
        self.assertEqual(enriched["$feature/string-value"], "test")
        self.assertEqual(set(enriched["$active_feature_flags"]), {"bool-value", "string-value"})
        for event in wire:
            if event["event"].startswith("stateful_"):
                self.assertEqual(uuid.UUID(event["uuid"]).version, 7)

    def test_begin_shutdown_closes_admission_but_retains_storage_lock(self):
        handle = self.create()
        self.assertEqual(self.lib.phu_begin_shutdown(handle), 0)
        self.assertEqual(self.capture(handle)[0], 3)
        self.assertEqual(self.flags(handle), (3, None))
        self.assertEqual(self.lib.phu_opt_in(handle), 3)
        config = {"project_token": "phc_MOCK", "host": self.host,
                  "storage_path": str(self.directory / "spool")}
        storage, data = encoded(config)
        other = c.c_uint64()
        self.assertEqual(self.lib.phu_create(1, data, c.byref(other)), 9)
        self.assertEqual(other.value, 0)
        self.assertEqual(self.lib.phu_close(handle), 0)
        self.assertNotEqual(self.create(), handle)

    def test_stale_flag_completion_after_opt_out(self):
        handle = self.create()
        self.http("/__control/state", {"delays": {"flags": 0.1}})
        with concurrent.futures.ThreadPoolExecutor() as pool:
            future = pool.submit(self.flags, handle)
            time.sleep(0.02)
            self.assertEqual(self.lib.phu_opt_out(handle), 0)
            self.assertEqual(future.result(timeout=5), (7, None))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True)
    parser.add_argument("--mock-server", required=True)
    parser.add_argument("--editor", help="Path to UnrealEditor executable")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
