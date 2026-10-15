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


class MockServerTestCase(unittest.TestCase):
    @classmethod
    def start_server(cls):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        cls.host = f"http://127.0.0.1:{port}"
        cls.log = tempfile.TemporaryFile()
        cls.server = subprocess.Popen([sys.executable, cls.mock_server_path], env={**os.environ, "PORT": str(port)}, stdout=cls.log, stderr=cls.log)
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


class NativeTests(MockServerTestCase):
    @classmethod
    def setUpClass(cls):
        cls.mock_server_path = ARGS.mock_server
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
                 "uuid": str(uuid.uuid4()), "timestamp": "2026-01-01T12:00:00.000Z",
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

    def wire_events(self):
        requests = self.http("/__captured/analytics_requests")
        self.assertTrue(requests, "No v1 analytics request captured")
        for request in requests:
            self.assertEqual(request["path"], "/i/v1/analytics/events")
            self.assertEqual(request["headers"]["posthog-sdk-info"], "posthog-unreal/0.0.1")
            for event in request["body"]["batch"]:
                for key in ("$lib", "$lib_version", "$session_id", "$window_id", "$process_person_profile"):
                    self.assertNotIn(key, event["properties"])
                self.assertIsInstance(event["options"], dict)
        return [event for request in requests for event in request["body"]["batch"]]

    def test_capture_snapshot_and_side_effect_free_flags(self):
        handle = self.create()
        status, event = self.capture(handle)
        self.assertEqual(status, 1)
        self.assertEqual(self.checkpoint(handle)["persistence_errors"], 0)
        self.assertEqual(self.lib.phu_flush(handle), 0)
        wire = self.wire_events()
        self.assertEqual(len(wire), 1)
        for key in ("uuid", "timestamp", "distinct_id"):
            self.assertEqual(wire[0][key], event[key])
        self.assertEqual(wire[0]["properties"], {key: value for key, value in event["properties"].items()
                                                if key not in ("$lib", "$lib_version")})
        self.assertEqual(self.http("/__captured/events")[0]["properties"], event["properties"])
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
        self.http("/__control/state", {"force500": {"analytics": True}})
        handle = self.create()
        status, event = self.capture(handle)
        self.assertEqual(status, 1)
        self.assertEqual(self.checkpoint(handle)["pending"], 1)
        self.assertEqual(self.lib.phu_close(handle), 0)
        self.assertEqual(len(list((self.directory / "spool").glob("*.json"))), 1)
        self.http("/__control/reset", {})
        restarted = self.create()
        self.assertEqual(self.lib.phu_flush(restarted), 0)
        wire = self.wire_events()
        self.assertEqual(len(wire), 1)
        for key in ("uuid", "timestamp", "distinct_id"):
            self.assertEqual(wire[0][key], event[key])
        self.assertEqual(wire[0]["properties"], {key: value for key, value in event["properties"].items()
                                                if key not in ("$lib", "$lib_version")})
        self.assertEqual(self.http("/__captured/events")[0]["properties"], event["properties"])

    def test_prepared_properties_options_and_transport_attribution(self):
        handle = self.create()
        session = str(uuid.uuid4())
        self.assertEqual(self.capture(handle, properties={
            "$lib": "caller-override", "$lib_version": "caller-version",
            "$session_id": session, "$process_person_profile": False, "custom": "kept",
        })[0], 1)
        self.assertEqual(self.capture(handle, event="hook minimized", properties={})[0], 1)
        self.assertEqual(self.lib.phu_flush(handle), 0)
        wire = self.wire_events()
        self.assertEqual(wire[0]["properties"], {"custom": "kept"})
        self.assertEqual(wire[0]["options"], {"process_person_profile": False})
        self.assertEqual(wire[0]["session_id"], session)
        self.assertEqual(wire[1]["properties"], {})
        self.assertEqual(wire[1]["options"], {})
        self.assertNotIn("session_id", wire[1])
        accepted = self.http("/__captured/events")
        self.assertEqual(accepted[0]["properties"], {
            "$lib": "posthog-unreal", "$lib_version": "0.0.1", "$session_id": session,
            "$process_person_profile": False, "custom": "kept",
        })
        self.assertEqual(accepted[1]["properties"], {"$lib": "posthog-unreal", "$lib_version": "0.0.1"})

    def test_partial_verdicts_retain_retry_after_attempt_limit_until_later_flush(self):
        handle = self.create()
        events = {}
        for result in ("ok", "retry", "drop", "warning"):
            status, event = self.capture(handle, event=result)
            self.assertEqual(status, 1)
            events[result] = event
        self.http("/__control/state", {"analyticsV1Results": {
            events["retry"]["uuid"]: [{"result": "retry", "details": "not_persisted"}] * 3 + ["ok"],
            events["drop"]["uuid"]: {"result": "drop", "details": "event_restriction_drop"},
            events["warning"]["uuid"]: {"result": "warning", "details": "person_processing_disabled"},
        }})
        # Each flush makes one attempt; exhausting a sequence must retain the retry.
        for _ in range(3):
            self.assertEqual(self.lib.phu_flush(handle), 0)
        report = self.checkpoint(handle)
        self.assertEqual(report["pending"], 1)
        self.assertEqual(report["delivered"], 2)
        self.assertEqual(report["terminally_dropped"], 1)
        self.assertEqual(len(list((self.directory / "spool").glob("*.json"))), 1)
        requests = self.http("/__captured/analytics_requests")
        self.assertEqual([r["headers"]["posthog-attempt"] for r in requests], ["1", "2", "3"])
        self.assertEqual(len({r["headers"]["posthog-request-id"] for r in requests}), 1)
        self.assertEqual(len(requests[0]["body"]["batch"]), 4)
        retry_wire = next(e for e in requests[0]["body"]["batch"] if e["uuid"] == events["retry"]["uuid"])
        for request in requests[1:]:
            self.assertEqual(request["body"]["batch"], [retry_wire])
        self.assertEqual({e["event"] for e in self.http("/__captured/events")}, {"ok", "warning"})
        self.assertEqual(self.lib.phu_flush(handle), 0)
        requests = self.http("/__captured/analytics_requests")
        self.assertEqual(len(requests), 4)
        self.assertEqual(requests[-1]["headers"]["posthog-attempt"], "1")
        self.assertNotEqual(requests[-1]["headers"]["posthog-request-id"], requests[0]["headers"]["posthog-request-id"])
        self.assertEqual(requests[-1]["body"]["batch"], [retry_wire])
        self.assertEqual(self.checkpoint(handle)["pending"], 0)
        self.assertEqual(list((self.directory / "spool").glob("*.json")), [])
        self.assertEqual({e["event"] for e in self.http("/__captured/events")}, {"ok", "warning", "retry"})

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
        self.http("/__control/state", {"force500": {"analytics": True}})
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
        self.http("/__control/state", {"delays": {"analytics": 1}})
        self.assertEqual(self.capture(handle)[0], 1)
        self.assertEqual(self.capture(handle)[0], 1)
        self.assertEqual(self.capture(handle)[0], 8)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.lib.phu_close(handle), range(4)))
        self.assertEqual(results.count(0), 1)
        self.assertEqual(results.count(3), 3)
        self.assertEqual(self.capture(handle)[0], 3)

    def test_close_stalled_request_and_retry_backlog(self):
        self.http("/__control/state", {"delays": {"analytics": 2}})
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
                "-unattended", "-nullrhi", "-nosound", "-nosplash", "-nop4",
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
        self.assertEqual(result["succeeded"], 6, result)
        wire = self.wire_events()
        wire_by_name = {event["event"]: event for event in wire}
        materialized = self.http("/__captured/events")
        events = {event["event"]: event for event in materialized}
        self.assertEqual(events["unreal_native_integration"]["properties"]["$lib"], "posthog-unreal")
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
        identity_events = [event for event in materialized if event["event"] == "$identify" and event["distinct_id"] == "user-one"]
        self.assertEqual(len(identity_events), 1)
        self.assertEqual(identity_events[0]["properties"]["$anon_distinct_id"], anonymous["distinct_id"])
        self.assertEqual(sum(event["event"] == "$set" for event in materialized), 1)
        self.assertNotIn("secret", events["stateful_redacted"]["properties"])
        self.assertNotIn("secret", wire_by_name["stateful_redacted"]["properties"])
        self.assertEqual(events["stateful_redacted"]["properties"]["$lib"], "posthog-unreal")
        self.assertIs(wire_by_name["stateful_anonymous"]["options"]["process_person_profile"], False)
        self.assertIs(wire_by_name["stateful_never"]["options"]["process_person_profile"], False)
        self.assertEqual(wire_by_name["stateful_anonymous"]["session_id"],
                         anonymous["properties"]["$session_id"])
        for name in ("explicit_before", "explicit_after"):
            self.assertNotIn("session_id", wire_by_name[name])
        for name in ("ignored", "closed", "hook_withdraw", "hook_withdraw_resume", "must_not_replay", "$reset"):
            self.assertNotIn(name, events)
            self.assertNotIn(name, wire_by_name)
        self.assertNotIn("$session_id", events["explicit_before"]["properties"])
        self.assertIn("$session_id", events["explicit_during"]["properties"])
        self.assertNotIn("$session_id", events["explicit_after"]["properties"])
        self.assertNotIn("payload", events["stateful_bounds"]["properties"])
        self.assertFalse(events["stateful_never"]["properties"]["$process_person_profile"])
        parallel_a = [event for event in materialized if event["event"] == "parallel_a"]
        parallel_b = [event for event in materialized if event["event"] == "parallel_b"]
        self.assertEqual(len(parallel_a), 25)
        self.assertEqual(len(parallel_b), 25)
        self.assertEqual(len({event["distinct_id"] for event in parallel_a}), 1)
        self.assertEqual(len({event["distinct_id"] for event in parallel_b}), 1)
        self.assertNotEqual(parallel_a[0]["distinct_id"], parallel_b[0]["distinct_id"])
        first = events["subsystem_one"]
        second = events["subsystem_two"]
        self.assertNotEqual(first["distinct_id"], second["distinct_id"])
        self.assertNotEqual(first["properties"]["$session_id"], second["properties"]["$session_id"])
        for event in (first, second, events["subsystem_auto"], events["subsystem_replacement"]):
            self.assertTrue(event["properties"]["$engine_version"])
            self.assertTrue(event["properties"]["$os"])
        lifecycle = [event for event in materialized if event["event"].startswith("Application ")]
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
        exposures = [event for event in materialized if event["event"] == "$feature_flag_called"]
        initial = [event for event in exposures if event["properties"].get("stage") == "initial"]
        self.assertEqual(len(initial), 3)
        self.assertEqual({event["properties"]["$feature_flag"] for event in initial},
                         {"bool-value", "disabled-flag", "string-value"})
        self.assertEqual(len([event for event in exposures if event["properties"].get("stage") == "restored"]), 4)
        minimal = next(event["properties"] for event in exposures if event["properties"]["$feature_flag"] == "minimal")
        for key in ("test_suite", "stage", "hook_private", "$referrer"):
            self.assertNotIn(key, minimal)
        minimal_wire = next(event["properties"] for event in wire if
                            event["event"] == "$feature_flag_called" and event["properties"]["$feature_flag"] == "minimal")
        for key in ("test_suite", "stage", "hook_private", "$referrer"):
            self.assertNotIn(key, minimal_wire)
        self.assertEqual(minimal["$lib_version"], "0.0.1")
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
            self.assertEqual(full["$lib_version"], "0.0.1")
        enriched = events["flags_enriched"]["properties"]
        self.assertIs(enriched["$feature/bool-value"], True)
        self.assertIs(enriched["$feature/disabled-flag"], False)
        self.assertEqual(enriched["$feature/string-value"], "test")
        self.assertEqual(set(enriched["$active_feature_flags"]), {"bool-value", "string-value"})
        for event in materialized:
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
