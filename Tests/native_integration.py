#!/usr/bin/env python3
"""ABI v3 public-entry tests and optional Unreal consumer validation against the SDK mock server."""
import argparse
import base64
import concurrent.futures
import ctypes as c
import io
import json
import os
import socket
import subprocess
import urllib.request
from pathlib import Path
import sys
import tempfile
import threading
import time
import tomllib
import unittest
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
    server_environment = {}

    @classmethod
    def start_server(cls):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        cls.host = f"http://127.0.0.1:{port}"
        cls.log = tempfile.TemporaryFile()
        cls.server = subprocess.Popen([sys.executable, cls.mock_server_path], env={**os.environ, **cls.server_environment, "PORT": str(port)}, stdout=cls.log, stderr=cls.log)
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


OK, ACCEPTED, INVALID, CLOSED = 0, 1, 2, 3
DISABLED, FULL, OPTED_OUT, DROPPED, REENTRANT, DEFERRED, INACTIVE, WRONG, STALE = 7, 8, 10, 11, 12, 13, 14, 15, 16
HOOK = c.CFUNCTYPE(c.c_int32, c.c_void_p, Bytes, c.POINTER(Buffer))


class Hooks(c.Structure):
    _fields_ = [("before_send", HOOK), ("user_data", c.c_void_p)]


class StatefulNativeTests(MockServerTestCase):
    @classmethod
    def setUpClass(cls):
        cls.mock_server_path = ARGS.mock_server
        cls.lib = c.CDLL(ARGS.library)
        signatures = {
            "phu_coordination_key": [Bytes, c.POINTER(Buffer)],
            "phu_create": [c.c_uint32, Bytes, Hooks, c.POINTER(c.c_uint64)],
            "phu_capture": [c.c_uint64, Bytes, c.POINTER(Buffer)],
            "phu_screen": [c.c_uint64, Bytes, c.POINTER(Buffer)],
            "phu_command": [c.c_uint64, Bytes],
            "phu_read": [c.c_uint64, Bytes, c.POINTER(Buffer)],
            "phu_poll_flags": [c.c_uint64, c.POINTER(Buffer)],
            "phu_flush": [c.c_uint64, c.POINTER(Buffer)],
            "phu_close": [c.c_uint64, c.POINTER(Buffer)],
            "phu_begin_shutdown": [c.c_uint64],
            "phu_begin_frame": [c.c_uint64, Bytes, c.POINTER(c.c_uint64)],
            "phu_submit_frame": [c.c_uint64, c.c_uint64, Bytes],
            "phu_abandon_frame": [c.c_uint64],
            "phu_buffer_copy": [Bytes, c.POINTER(Buffer)],
        }
        for name, args in signatures.items():
            function = getattr(cls.lib, name)
            function.argtypes, function.restype = args, c.c_int32
        cls.lib.phu_buffer_free.argtypes = [Buffer]
        cls.lib.phu_buffer_free.restype = None
        cls.lib.phu_abi_version.restype = c.c_uint32
        if cls.lib.phu_abi_version() != 3:
            raise RuntimeError("Stateful integration requires ABI v3")
        lock = tomllib.loads((Path(__file__).resolve().parents[1] / "rust/Cargo.lock").read_text())
        core = next(package for package in lock["package"] if package["name"] == "posthog")
        cls.transport_sdk_info = f"posthog-rs/{core['version']}"

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.__class__.server_environment = {"SNAPSHOT_DUMP_DIR": str(self.root / "snapshots")}
        self.start_server()
        self.handles = []
        self.callbacks = []
        self.hook_errors = []
        self.http("/__control/reset", {})

    def tearDown(self):
        for handle in self.handles:
            self.close(handle)
        self.stop_server()
        self.directory.cleanup()
        self.assertEqual(self.hook_errors, [])

    def hook(self, function):
        # Never let a Python exception escape a foreign callback (ctypes otherwise discards it).
        def invoke(user_data, data, out):
            try:
                event = json.loads(c.string_at(data.data, data.len))
                result = function(event)
                if isinstance(result, int):
                    return result
                storage, value = encoded(result)
                return self.lib.phu_buffer_copy(value, out)
            except BaseException as error:
                self.hook_errors.append(repr(error))
                return DROPPED
        callback = HOOK(invoke)
        self.callbacks.append(callback)
        return Hooks(callback, None)

    def create(self, hook=None, **extra):
        options = {"project_token": "phc_MOCK", "host": self.host, "preload_feature_flags": False,
                   "flush_interval_ms": 60000, "shutdown_timeout_ms": 200, **extra}
        storage, value = encoded(options)
        handle = c.c_uint64()
        self.assertEqual(self.lib.phu_create(3, value, hook or Hooks(), c.byref(handle)), OK)
        self.assertNotEqual(handle.value, 0)
        self.handles.append(handle.value)
        return handle.value

    def result(self, name, *args):
        out = Buffer()
        status = getattr(self.lib, name)(*args, c.byref(out))
        try:
            return status, json.loads(c.string_at(out.data, out.len)) if out.data else None
        finally:
            self.lib.phu_buffer_free(out)

    def input_result(self, name, handle, value):
        storage, data = encoded(value)
        return self.result(name, handle, data)

    def command(self, handle, op, **fields):
        storage, data = encoded({"op": op, **fields})
        self.assertEqual(self.lib.phu_command(handle, data), OK)

    def read(self, handle, op, **fields):
        status, result = self.input_result("phu_read", handle, {"op": op, **fields})
        self.assertEqual(status, OK)
        return result

    def capture(self, handle, event="captured", **fields):
        return self.input_result("phu_capture", handle, {"event": event, **fields})

    def flush(self, handle):
        status, report = self.result("phu_flush", handle)
        self.assertEqual(status, OK)
        self.assertEqual(report["delivery"], "attempted")
        return report

    def close(self, handle):
        return self.result("phu_close", handle)

    def begin(self, handle, **fields):
        layout = {"width": 32, "height": 24, "row_stride_bytes": 32 * 4 + 8, **fields}
        storage, data = encoded(layout)
        frame = c.c_uint64()
        status = self.lib.phu_begin_frame(handle, data, c.byref(frame))
        return status, frame.value

    def frame(self, handle):
        status, frame = self.begin(handle)
        self.assertEqual(status, OK)
        self.assertNotEqual(frame, 0)
        return frame

    def submit(self, handle, frame, pixels=None):
        if pixels is None:
            # Blue left half, black masked right half; ignore alpha and row padding.
            pixels = (bytes([255, 0, 0, 17]) * 16 + bytes([0, 0, 0, 255]) * 16 + bytes([255]) * 8) * 24
        storage = c.create_string_buffer(pixels)
        return self.lib.phu_submit_frame(handle, frame, Bytes(c.cast(storage, c.c_void_p), len(pixels)))

    def replay(self, **options):
        handle = self.create(**options)
        self.flush(handle)  # Completes the initial remote-config attempt, not renderer startup.
        self.command(handle, "replay_start")
        self.assertTrue(self.read(handle, "replay_active"))
        return handle

    def events(self):
        return self.http("/__captured/events")

    def assert_event_attribution(self, event):
        self.assertEqual(event["properties"]["$lib"], "posthog-unreal")
        self.assertEqual(event["properties"]["$lib_version"], "0.0.1")

    def wire(self, check_attribution=True):
        requests = self.http("/__captured/analytics_requests")
        for request in requests:
            self.assertEqual(request["path"], "/i/v1/analytics/events")
            self.assertEqual(request["headers"]["posthog-sdk-info"], self.transport_sdk_info)
        events = [event for request in requests for event in request["body"]["batch"]]
        if check_attribution:
            for event in events:
                self.assert_event_attribution(event)
        return events

    def coordination_key(self, **fields):
        storage, value = encoded(fields)
        status, key = self.result("phu_coordination_key", value)
        self.assertEqual(status, OK)
        return key

    def test_coordination_key_normalizes_sdk_aliases_and_input_whitespace(self):
        expected = self.coordination_key(project_token="phc_MOCK", instance_name="owner")
        for host in ("", " \t", "https://app.posthog.com", "https://us.posthog.com///",
                     " \nhttps://us.i.posthog.com/\t"):
            self.assertEqual(self.coordination_key(host=host, project_token="\u2003phc_MOCK \n", instance_name="owner"), expected)
        self.assertEqual(self.coordination_key(host=" https://eu.posthog.com/// ", project_token="phc_MOCK"),
                         self.coordination_key(host="https://eu.i.posthog.com", project_token="phc_MOCK"))
        self.assertNotEqual(self.coordination_key(project_token="other", instance_name="owner"), expected)
        self.assertNotEqual(self.coordination_key(project_token="phc_MOCK", instance_name="other"), expected)
        self.assertEqual(len(expected), 40)
        self.assertNotIn("phc_MOCK", expected)

    def test_equivalent_inputs_restore_new_storage_and_keep_sdk_fallback(self):
        options = {"host": f" \t{self.host}///\n", "project_token": " phc_MOCK ",
                   "instance_name": "equivalent", "storage_root": str(self.root / "new-state")}
        canonical = {**options, "host": self.host, "project_token": "phc_MOCK"}
        self.assertEqual(self.coordination_key(**options), self.coordination_key(**canonical))
        owner = self.create(**options)
        self.command(owner, "identify", distinct_id="persisted-owner")
        self.command(owner, "opt_out")
        self.assertEqual(self.flush(owner)["state"], "persisted")
        fallback = self.create(**canonical)
        self.assertFalse(self.read(fallback, "state")["disabled"])
        self.assertNotEqual(self.read(fallback, "state")["distinct_id"], "persisted-owner")
        self.assertEqual(self.flush(fallback)["state"], "unavailable")
        self.assertEqual(self.close(fallback)[0], OK)
        self.assertEqual(self.close(owner)[0], OK)
        replacement = self.create(**canonical)
        state = self.read(replacement, "state")
        self.assertEqual(state["distinct_id"], "persisted-owner")
        self.assertTrue(state["opt_out"])
        self.assertEqual(self.flush(replacement)["state"], "persisted")

    def test_large_flag_output_is_retained_and_not_valid_as_command_input(self):
        variant = "x" * (700 * 1024)
        self.http("/__control/state", {"flags": {"large": variant}})
        handle = self.create(preload_feature_flags=True)
        self.flush(handle)
        flag = self.read(handle, "feature_flag", key="large", send_event=False)
        self.assertIsNotNone(flag)
        self.assertEqual(flag["variant"], variant)
        self.assertEqual(flag["payload"], variant)
        self.assertGreater(len(json.dumps(flag).encode()), 1024 * 1024)
        self.assertEqual(self.read(handle, "feature_flags")["large"], flag)
        storage, value = encoded({"op": "register", "properties": {"record": flag}})
        self.assertEqual(self.lib.phu_command(handle, value), INVALID)
        self.assertEqual(self.capture(handle, properties={"record": flag}), (INVALID, None))
        self.assertEqual(self.events(), [])

    def test_identity_properties_groups_sessions_and_receipts(self):
        handle = self.create(platform_context={"precedence": "context"})
        anonymous = self.read(handle, "state")["distinct_id"]
        self.command(handle, "register", properties={"precedence": "registered", "saved": True})
        status, receipt = self.capture(handle, "anonymous", properties={"precedence": "event"})
        self.assertEqual(status, ACCEPTED)
        self.assertEqual(uuid.UUID(receipt["uuid"]).version, 7)
        session = self.read(handle, "session_id")
        self.command(handle, "identify", distinct_id="user", set={"plan": "paid"}, set_once={"origin": "test"})
        self.command(handle, "group", group_type="company", group_key="acme", properties={"size": 12})
        self.assertEqual(self.read(handle, "session_id"), session)
        self.assertEqual(self.capture(handle, "identified")[0], ACCEPTED)
        self.command(handle, "unregister", key="saved")
        self.command(handle, "reset")
        self.assertNotEqual(self.read(handle, "state")["distinct_id"], anonymous)
        self.assertEqual(self.capture(handle, "reset")[0], ACCEPTED)
        self.assertEqual(self.flush(handle)["state"], "memory_only")
        wire = {event["event"]: event for event in self.wire()}
        self.assertEqual(wire["anonymous"]["uuid"], receipt["uuid"])
        self.assertEqual(wire["anonymous"]["properties"]["precedence"], "event")
        self.assertEqual(wire["identified"]["properties"]["precedence"], "registered")
        self.assertEqual(wire["identified"]["properties"]["$groups"], {"company": "acme"})
        self.assertEqual(wire["$identify"]["properties"]["$anon_distinct_id"], anonymous)
        self.assertNotIn("saved", wire["reset"]["properties"])
        self.assertNotEqual(wire["reset"]["session_id"], session)

    def test_consent_retains_permitted_work_and_blocks_new_collection(self):
        handle = self.create()
        self.assertEqual(self.capture(handle, "before")[0], ACCEPTED)
        self.command(handle, "opt_out")
        self.assertEqual(self.capture(handle, "blocked"), (OPTED_OUT, None))
        self.flush(handle)
        self.assertEqual([event["event"] for event in self.events()], ["before"])
        self.command(handle, "opt_in")
        self.assertEqual(self.capture(handle, "after")[0], ACCEPTED)
        self.flush(handle)
        self.assertEqual([event["event"] for event in self.events()], ["before", "after"])

    def test_lifecycle_capture_respects_sdk_session_boundaries(self):
        background = self.create(opt_out=True)
        self.command(background, "opt_in")
        self.assertIsNone(self.read(background, "peek_session_id"))
        self.command(background, "set_app_in_background", background=True)
        self.assertEqual(self.capture(background, "background_probe")[0], ACCEPTED)
        self.assertIsNone(self.read(background, "peek_session_id"))
        self.flush(background)
        self.assertIsNone(next(event for event in self.wire() if event["event"] == "background_probe").get("session_id"))

        foreground = self.create(opt_out=True)
        self.command(foreground, "opt_in")
        self.assertIsNone(self.read(foreground, "peek_session_id"))
        # Closing foreground activity is captured before reporting the background transition.
        self.assertEqual(self.capture(foreground, "Application Backgrounded")[0], ACCEPTED)
        session = self.read(foreground, "peek_session_id")
        self.assertTrue(session)
        self.command(foreground, "set_app_in_background", background=True)
        self.assertEqual(self.read(foreground, "peek_session_id"), session)
        self.command(foreground, "set_app_in_background", background=False)
        self.assertEqual(self.capture(foreground, "Application Opened", properties={"from_background": True})[0], ACCEPTED)
        self.flush(foreground)
        lifecycle = [event for event in self.wire() if event["event"].startswith("Application ")]
        self.assertEqual([event["event"] for event in lifecycle], ["Application Backgrounded", "Application Opened"])
        self.assertEqual([event.get("session_id") for event in lifecycle], [session, session])

        manual = self.create(session_mode="manual")
        self.assertEqual(self.capture(manual, "manual_foreground_probe")[0], ACCEPTED)
        self.command(manual, "set_app_in_background", background=True)
        self.assertEqual(self.capture(manual, "manual_background_probe")[0], ACCEPTED)
        self.command(manual, "set_app_in_background", background=False)
        self.assertIsNone(self.read(manual, "peek_session_id"))
        self.flush(manual)
        for event in self.wire():
            if event["event"].startswith("manual_"):
                self.assertIsNone(event.get("session_id"))

    def test_capture_fallback_precedence_hook_removal_and_nonleak(self):
        observed = []
        def inspect(event):
            if event["event"].startswith("fallback_"):
                observed.append(json.loads(json.dumps(event)))
                event["properties"].pop("remove_via_hook", None)
            return event
        handle = self.create(hook=self.hook(inspect), platform_context={"platform": "context", "choice": "context"})
        self.command(handle, "register", properties={"choice": "registered", "reveal": "registered"})
        defaults = {"choice": "fallback", "platform": "fallback", "reveal": "fallback", "only": "fallback",
                    "remove_via_hook": True, "$lib": "not-the-sdk", "$is_identified": "not-a-boolean"}
        self.assertEqual(self.capture(handle, "fallback_explicit", properties={"choice": "explicit"}, fallback_properties=defaults)[0], ACCEPTED)
        self.command(handle, "unregister", key="reveal")
        self.assertEqual(self.capture(handle, "fallback_revealed", fallback_properties=defaults)[0], ACCEPTED)
        self.assertEqual(self.capture(handle, "fallback_direct")[0], ACCEPTED)
        self.flush(handle)
        self.assertEqual(len(observed), 3)
        first, revealed, direct = [event["properties"] for event in observed]
        self.assertEqual(first["choice"], "explicit")
        self.assertEqual(first["platform"], "context")
        self.assertEqual(first["reveal"], "registered")
        self.assertEqual(first["only"], "fallback")
        self.assertEqual(first["$lib"], "posthog-unreal")
        self.assertIs(first["$is_identified"], False)
        self.assertTrue(first["remove_via_hook"])
        self.assertEqual(revealed["choice"], "registered")
        self.assertEqual(revealed["reveal"], "fallback")
        self.assertNotIn("only", direct)
        self.assertNotIn("reveal", direct)
        self.assertTrue(all("remove_via_hook" not in event["properties"] for event in self.wire()))

    def test_queue_capacity_and_screen_outcomes(self):
        handle = self.create(max_queue_size=1, max_batch_size=1)
        self.flush(handle)
        self.assertEqual(self.capture(handle, "first")[0], ACCEPTED)
        self.assertEqual(self.capture(handle, "full"), (FULL, None))
        self.flush(handle)
        status, receipt = self.input_result("phu_screen", handle,
                                            {"name": "Store", "properties": {"$screen_name": "ignored"}})
        self.assertEqual(status, ACCEPTED)
        self.assertIsNotNone(receipt["uuid"])
        self.flush(handle)
        events = self.wire()
        self.assertEqual([event["event"] for event in events], ["first", "$screen"])
        self.assertEqual(events[-1]["properties"]["$screen_name"], "Store")
        self.assertEqual(self.capture(handle, "")[0], INVALID)

    def test_fresh_persistence_restores_consent_identity_and_offline_events(self):
        root = str(self.root / "rust-owned")
        # An unrelated prototype directory must remain untouched.
        legacy = self.root / "old" / "state.json"
        legacy.parent.mkdir()
        legacy.write_text("legacy fixture")
        self.http("/__control/state", {"force500": {"analytics": True}})
        handle = self.create(storage_root=root)
        identity = self.read(handle, "state")["distinct_id"]
        status, receipt = self.capture(handle, "offline")
        self.assertEqual(status, ACCEPTED)
        self.command(handle, "opt_out")
        self.assertEqual(self.flush(handle)["state"], "persisted")
        self.assertEqual(self.close(handle)[0], OK)
        self.http("/__control/state", {"force500": {"analytics": False}})
        handle = self.create(storage_root=root)
        self.assertEqual(self.read(handle, "state")["distinct_id"], identity)
        self.assertTrue(self.read(handle, "state")["opt_out"])
        self.assertEqual(self.capture(handle)[0], OPTED_OUT)
        self.flush(handle)
        self.assertEqual(self.events()[0]["uuid"], receipt["uuid"])
        self.assertEqual(self.events()[0]["distinct_id"], identity)
        self.assertEqual(legacy.read_text(), "legacy fixture")

    def test_hook_round_trip_mutation_drop_and_options(self):
        def transform(event):
            if event["event"] == "drop":
                return DROPPED
            if event["event"] == "malformed":
                return {}
            event["event"] = "redacted"
            event["properties"].pop("secret", None)
            event["properties"]["safe"] = True
            return event
        handle = self.create(hook=self.hook(transform))
        stamp, event_id = "2026-01-01T12:00:00.000Z", str(uuid.uuid4())
        self.assertEqual(self.capture(handle, "original", properties={"secret": "private"},
                                      options={"process_person_profile": True}, groups={"company": "acme"},
                                      timestamp=stamp, uuid=event_id)[0], ACCEPTED)
        self.assertEqual(self.capture(handle, "drop"), (DROPPED, None))
        self.assertEqual(self.capture(handle, "malformed"), (DROPPED, None))
        self.flush(handle)
        event = self.wire()[0]
        self.assertEqual(event["event"], "redacted")
        self.assertEqual(event["uuid"], event_id)
        self.assertEqual(event["timestamp"], stamp)
        self.assertTrue(event["options"]["process_person_profile"])
        self.assertEqual(event["properties"]["$groups"], {"company": "acme"})
        self.assertNotIn("secret", event["properties"])
        self.assertTrue(event["properties"]["safe"])
        self.assertEqual(len(self.events()), 1)

    def test_hook_removed_attribution_is_not_refilled(self):
        def transform(event):
            event["properties"].pop("$lib", None)
            event["properties"].pop("$lib_version", None)
            return event
        handle = self.create(hook=self.hook(transform))
        self.assertEqual(self.capture(handle, "filtered attribution")[0], ACCEPTED)
        self.flush(handle)
        events = self.wire(check_attribution=False)
        self.assertEqual(len(events), 1)
        self.assertNotIn("$lib", events[0]["properties"])
        self.assertNotIn("$lib_version", events[0]["properties"])

    def test_hook_close_is_deferred_and_external_finish_joins(self):
        handle = None
        results = []
        def transform(event):
            results.append(self.capture(handle, "recursive")[0])
            results.append(self.close(handle)[0])
            return event
        handle = self.create(hook=self.hook(transform))
        self.assertEqual(self.capture(handle), (CLOSED, None))
        self.assertEqual(results, [REENTRANT, DEFERRED])
        self.assertEqual(self.close(handle)[0], OK)

    def test_concurrent_close_waits_for_active_foreign_hook(self):
        entered, release = threading.Event(), threading.Event()
        def transform(event):
            entered.set()
            if not release.wait(5):
                raise TimeoutError("hook release")
            return event
        handle = self.create(hook=self.hook(transform))
        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
            capture = pool.submit(self.capture, handle)
            self.assertTrue(entered.wait(5))
            self.assertEqual(self.lib.phu_begin_shutdown(handle), OK)
            closes = [pool.submit(self.close, handle) for _ in range(3)]
            try:
                time.sleep(0.05)
                self.assertTrue(all(not future.done() for future in closes))
                self.assertEqual(self.capture(handle)[0], CLOSED)
            finally:
                release.set()
            self.assertEqual(capture.result(timeout=5)[0], CLOSED)
            for future in closes:
                self.assertEqual(future.result(timeout=5)[0], OK)

    def test_flags_mailbox_getters_and_exposures(self):
        handle = self.create(preload_feature_flags=True)
        self.flush(handle)
        status, update = self.result("phu_poll_flags", handle)
        self.assertEqual(status, OK)
        self.assertEqual(set(update["flags"]), {"bool-value", "string-value"})
        self.assertNotIn("disabled-flag", update["flags"])
        self.assertEqual(self.result("phu_poll_flags", handle), (INACTIVE, None))
        all_flags = self.read(handle, "feature_flags")
        self.assertFalse(all_flags["disabled-flag"]["enabled"])
        self.assertIsNone(self.read(handle, "feature_flag", key="missing"))
        variant = self.read(handle, "feature_flag", key="string-value", send_event=False)
        self.assertEqual(variant["variant"], "test")
        self.assertTrue(variant["has_payload"])
        self.assertEqual(variant["payload"], "test")
        self.assertEqual(self.events(), [])
        self.assertFalse(self.read(handle, "feature_flag", key="disabled-flag")["enabled"])
        self.read(handle, "feature_flag", key="disabled-flag")
        self.flush(handle)
        self.assertEqual([event["event"] for event in self.events()], ["$feature_flag_called"])
        self.command(handle, "opt_out")
        self.assertFalse(self.read(handle, "feature_flag", key="disabled-flag")["enabled"])
        self.command(handle, "reload_feature_flags")
        self.flush(handle)
        self.assertEqual(self.result("phu_poll_flags", handle)[0], OK)

    def test_flags_load_failure_is_real_notification_not_a_future(self):
        handle = self.create(preload_feature_flags=True)
        self.flush(handle)
        self.result("phu_poll_flags", handle)
        self.http("/__control/state", {"flagsQuotaLimited": True})
        self.command(handle, "reload_feature_flags")
        self.flush(handle)
        status, update = self.result("phu_poll_flags", handle)
        self.assertEqual(status, OK)
        self.assertEqual(update["error"]["kind"], "quota_limited")
        self.assertTrue(update["errors_loading"])
        self.assertFalse(self.read(handle, "feature_flags")["disabled-flag"]["enabled"])
        self.assertEqual(self.lib.phu_begin_shutdown(handle), OK)
        self.assertEqual(self.result("phu_poll_flags", handle), (CLOSED, None))
        self.assertEqual(self.close(handle)[0], OK)

    def test_frames_capacity_ownership_abandon_and_error_release(self):
        handle = self.replay(max_frames_in_flight=1)
        other = self.replay(max_frames_in_flight=1, instance_name="other")
        self.assertEqual(self.begin(handle, width=0), (INACTIVE, 0))
        frame = self.frame(handle)
        self.assertEqual(self.begin(handle), (INACTIVE, 0))
        self.assertEqual(self.lib.phu_abandon_frame(frame), OK)
        self.assertEqual(self.lib.phu_abandon_frame(frame), CLOSED)
        frame = self.frame(handle)
        self.assertEqual(self.submit(other, frame), WRONG)
        frame = self.frame(handle)
        self.assertEqual(self.submit(handle, frame, b"short"), INVALID)
        frame = self.frame(handle)
        self.command(handle, "identify", distinct_id="changed")
        self.assertEqual(self.submit(handle, frame), STALE)
        for transition in ("reset_session", "reset", "opt_out"):
            frame = self.frame(handle)
            self.command(handle, transition)
            if transition == "opt_out":
                self.command(handle, "opt_in")
            self.assertEqual(self.submit(handle, frame), STALE)
            self.command(handle, "replay_start")
        frame = self.frame(handle)
        self.assertEqual(self.lib.phu_begin_shutdown(handle), OK)
        self.assertEqual(self.lib.phu_abandon_frame(frame), CLOSED)
        self.assertEqual(self.submit(handle, frame), CLOSED)
        self.assertEqual(self.close(handle)[0], OK)

    def test_encoded_replay_dimensions_mask_pixels_and_late_completion(self):
        from PIL import Image
        handle = self.replay(max_frames_in_flight=1)
        identity = self.read(handle, "state")["distinct_id"]
        frame = self.frame(handle)
        self.command(handle, "replay_stop")
        self.assertFalse(self.read(handle, "replay_active"))
        self.assertEqual(self.submit(handle, frame), ACCEPTED)
        self.flush(handle)
        self.assertEqual(self.begin(handle), (INACTIVE, 0))
        snapshots = self.http("/__captured/snapshots")
        self.assertEqual(len(snapshots), 1)
        envelopes = [envelope for path in (self.root / "snapshots").glob("*.json")
                     for envelope in json.loads(path.read_text())]
        self.assertEqual(len(envelopes), 1)
        envelope = envelopes[0]
        self.assertEqual(envelope["distinct_id"], identity)
        self.assert_event_attribution(envelope)
        full = next(value for value in envelope["properties"]["$snapshot_data"] if value["type"] == 2)
        wireframe = full["data"]["wireframes"][0]
        jpeg = base64.b64decode(wireframe["base64"].split(",", 1)[1])
        with Image.open(io.BytesIO(jpeg)) as image:
            self.assertEqual(image.size, (32, 24))
            self.assertEqual((wireframe["width"], wireframe["height"]), (32, 24))
            self.assertGreater(image.getpixel((4, 12))[2], 220)
            self.assertLess(max(image.getpixel((28, 12))), 25)
        self.assertEqual(self.events(), [])

    def test_replay_encoder_hook_close_and_invalid_attribution(self):
        handle, callbacks = None, []
        def transform(event):
            if event["event"] == "$snapshot":
                callbacks.append(self.close(handle)[0])
            return event
        handle = self.replay(hook=self.hook(transform))
        identity = self.read(handle, "state")["distinct_id"]
        session = self.read(handle, "peek_session_id")
        self.assertEqual(self.submit(handle, self.frame(handle)), ACCEPTED)
        deadline = time.monotonic() + 5
        while not callbacks and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(callbacks, [DEFERRED])
        self.assertEqual(self.close(handle)[0], OK)
        accepted_before_shutdown = self.http("/__captured/snapshots")
        self.assertLessEqual(len(accepted_before_shutdown), 1)
        for snapshot in accepted_before_shutdown:
            self.assertEqual(snapshot["distinct_id"], identity)
            self.assertEqual(snapshot["session_id"], session)
        def reattribute(event):
            event["distinct_id"] = "not-the-acquisition-user"
            return event
        other = self.replay(hook=self.hook(reattribute))
        self.assertEqual(self.submit(other, self.frame(other)), ACCEPTED)
        self.flush(other)
        self.assertEqual(self.http("/__captured/snapshots"), accepted_before_shutdown)

    def test_replay_remote_restrictions_and_manual_start(self):
        self.http("/__control/state", {"sampleRate": "0.5"})
        handle = self.create()
        self.flush(handle)
        self.command(handle, "replay_start")
        self.assertFalse(self.read(handle, "replay_active"))
        self.assertEqual(self.begin(handle), (INACTIVE, 0))
        self.http("/__control/state", {"sampleRate": "1"})
        other = self.create()
        self.flush(other)
        self.assertFalse(self.read(other, "replay_active"))
        self.assertEqual(self.begin(other), (INACTIVE, 0))
        self.command(other, "replay_start")
        frame = self.frame(other)
        self.assertEqual(self.lib.phu_abandon_frame(frame), OK)


    def test_unreal_editor_native_integration(self):
        if not ARGS.editor:
            self.skipTest("Pass --editor after installing matching ABI-v3 artifacts")
        root = Path(__file__).resolve().parents[1]
        evidence = root / "artifacts" / "editor-integration" / str(uuid.uuid4())
        controls = evidence / "controls"
        controls.mkdir(parents=True)
        command = [ARGS.editor, str(root / "Tests/Unreal/PostHogTestHost.uproject"),
                   "-unattended", "-nullrhi", "-nosound", "-nosplash", "-nop4", "-NoAssetRegistryCache",
                   "-ExecCmds=Automation RunTests PostHog.;SoftQuit", f"-ReportExportPath={evidence}",
                   f"-abslog={evidence / 'editor.log'}", "-stdout", "-FullStdOutLogOutput"]
        with (evidence / "stdout.log").open("w") as output:
            process = subprocess.Popen(command, env={**os.environ, "POSTHOG_TEST_HOST": self.host,
                                       "POSTHOG_TEST_CONTROL_DIRECTORY": str(controls)},
                                       stdout=output, stderr=subprocess.STDOUT, text=True)
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
        report = json.loads((evidence / "index.json").read_text(encoding="utf-8-sig"))
        self.assertEqual(report["failed"], 0, report)
        self.assertEqual(report["succeeded"], 13, report)
        events = self.wire()
        wire = {event["event"]: event for event in events}
        for name in ("adapter_client", "unreal_native_integration", "subsystem_one", "subsystem_two",
                     "subsystem_replacement", "blueprint_anonymous", "blueprint_identified"):
            self.assertIn(name, wire)
        self.assertNotIn("secret", wire["adapter_client"]["properties"])
        self.assertTrue(wire["adapter_client"]["options"]["process_person_profile"])
        self.assertEqual(wire["blueprint_anonymous"]["properties"]["string"], "00123 玩家 🚀")
        self.assertFalse(wire["blueprint_anonymous"]["properties"]["boolean"])
        self.assertNotEqual(wire["subsystem_one"]["distinct_id"], wire["subsystem_two"]["distinct_id"])
        self.assertTrue(wire["subsystem_one"]["properties"]["$engine_version"])
        for name in ("analytics_before", "analytics_during", "analytics_revealed_default", "analytics_replaced_defaults",
                     "analytics_cleared_defaults", "analytics_native", "analytics_native_after_defaults",
                     "analytics_stock_blueprint", "analytics_multicast", "analytics_restart"):
            self.assertIn(name, wire)
        expiry_ends = [event for event in events if event["event"] == "SessionEnd" and event["properties"].get("expiry_fixture")]
        self.assertEqual(len(expiry_ends), 1)
        self.assertEqual(expiry_ends[0]["session_id"], wire["analytics_expiry_seed"]["session_id"])
        before = wire["analytics_before"]["properties"]
        self.assertEqual(before["precedence"], "caller")
        self.assertEqual(before["context_wins"], "registered")
        self.assertEqual(before["registered_wins"], "registered")
        self.assertNotIn("secret", before)
        self.assertEqual(wire["analytics_revealed_default"]["properties"]["registered_wins"], "default")
        for name in ("analytics_native", "analytics_native_after_defaults", "analytics_cleared_defaults"):
            self.assertNotIn("default_only", wire[name]["properties"])
            self.assertNotIn("replacement", wire[name]["properties"])
        self.assertIn("replacement", wire["analytics_replaced_defaults"]["properties"])
        self.assertNotIn("default_only", wire["analytics_replaced_defaults"]["properties"])
        lifecycle = {"Application Opened", "Application Backgrounded"}
        def sequence(case):
            return [(event["event"], event["properties"].get("from_background")) for event in events
                    if event["event"] in lifecycle and event["properties"].get("lifecycle_test_case") == case]
        self.assertEqual(sequence("0"), [("Application Opened", False), ("Application Backgrounded", None),
                                         ("Application Opened", True)])
        self.assertEqual(sequence("1"), [("Application Opened", True)])
        self.assertEqual(sequence("3"), [("Application Backgrounded", None), ("Application Opened", True)])
        self.assertEqual(sequence("4"), [])  # Lifecycle BeforeSend drops remain authoritative.
        for case in ("0", "1", "3"):
            attributed = [event.get("session_id") for event in events
                          if event["event"] in lifecycle and event["properties"].get("lifecycle_test_case") == case]
            self.assertTrue(all(attributed))
            self.assertEqual(len(set(attributed)), 1)
        disabled = [event for event in events if event["properties"].get("$app_build") == "lifecycle-disabled"]
        self.assertTrue(any(event["event"] == "lifecycle_signal_marker" for event in disabled))
        self.assertFalse(any(event["event"] in lifecycle for event in disabled))
        snapshots = self.http("/__captured/snapshots")
        self.assertGreaterEqual(len(snapshots), 2)
        (evidence / "wire.json").write_text(json.dumps(self.http("/__captured/all"), indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True)
    parser.add_argument("--mock-server", required=True)
    parser.add_argument("--editor", help="Run the installed ABI-v3 Unreal automation fixture")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
