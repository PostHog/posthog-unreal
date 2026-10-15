"""Fixture-only HTTP tests of the maintained mock, not native SDK or live ingestion proof.

python3 Tests/mock_server_v1_test.py --mock-server ~/.agents/skills/sdk-mock-server/server.py
"""
import argparse
import base64
import gzip
import json
import sys
import unittest
import urllib.error
import urllib.request
import uuid

from native_integration import MockServerTestCase


class AnalyticsV1MockTests(MockServerTestCase):
    def setUp(self):
        self.start_server()
        self.http("/__control/reset", {})

    def tearDown(self):
        self.stop_server()

    def event(self, **overrides):
        return {"uuid": str(uuid.uuid4()), "event": "fixture", "distinct_id": "玩家 🚀",
                "timestamp": "2026-01-01T12:00:00.000Z", "properties": {}, "options": {}, **overrides}

    def post_batch(self, events, *, compressed=False):
        body = {"created_at": "2026-01-01T12:00:00.000Z", "batch": events}
        raw = json.dumps(body, ensure_ascii=False).encode()
        headers = {"Content-Type": "application/json", "PostHog-Sdk-Info": "posthog-unreal/0.0.1",
                   "PostHog-Request-Id": str(uuid.uuid4()), "PostHog-Attempt": "1",
                   "Authorization": "Bearer phc_MOCK"}
        if compressed:
            raw = gzip.compress(raw)
            headers["Content-Encoding"] = "gzip"
        request = urllib.request.Request(self.host + "/i/v1/analytics/events", data=raw, headers=headers)
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.load(response), body, raw

    def test_wire_evidence_is_separate_from_materialized_properties(self):
        session, window = str(uuid.uuid4()), str(uuid.uuid4())
        event = self.event(session_id=session, window_id=window,
                           properties={"custom": [False, None, {"x": "ü"}]},
                           options={"process_person_profile": False, "cookieless_mode": True,
                                    "disable_skew_correction": True, "product_tour_id": "tour"})
        response, body, raw = self.post_batch([event], compressed=True)
        self.assertEqual(response, {"results": {event["uuid"]: {"result": "ok"}}})
        requests = self.http("/__captured/analytics_requests")
        self.assertEqual(len(requests), 1)
        self.assertEqual(base64.b64decode(requests[0]["raw_body_base64"]), raw)
        self.assertEqual(requests[0]["body"], body)
        self.assertEqual(requests[0]["headers"]["posthog-sdk-info"], "posthog-unreal/0.0.1")
        self.assertNotIn("authorization", requests[0]["headers"])
        accepted = self.http("/__captured/events")
        self.assertEqual(accepted[0]["properties"], {
            "custom": [False, None, {"x": "ü"}], "$session_id": session, "$window_id": window,
            "$process_person_profile": False, "$cookieless_mode": True, "$ignore_sent_at": True,
            "$product_tour_id": "tour", "$lib": "posthog-unreal", "$lib_version": "0.0.1",
        })
        for key in ("uuid", "timestamp", "distinct_id"):
            self.assertEqual(accepted[0][key], event[key])
        self.assertEqual(requests[0]["body"]["batch"][0]["properties"], event["properties"])

    def test_minimized_properties_remain_minimal_except_transport_attribution(self):
        event = self.event()
        self.post_batch([event])
        self.assertEqual(self.http("/__captured/analytics_requests")[0]["body"]["batch"][0], event)
        self.assertEqual(self.http("/__captured/events")[0]["properties"],
                         {"$lib": "posthog-unreal", "$lib_version": "0.0.1"})

    def test_materialized_envelope_and_header_values_win(self):
        event = self.event(session_id="session", properties={"$lib": "caller", "$lib_version": "override",
                           "$session_id": "old", "$process_person_profile": True},
                           options={"process_person_profile": "false"})
        self.post_batch([event])
        self.assertEqual(self.http("/__captured/events")[0]["properties"], {
            "$lib": "posthog-unreal", "$lib_version": "0.0.1",
            "$session_id": "session", "$process_person_profile": False,
        })
        self.assertEqual(self.http("/__captured/analytics_requests")[0]["body"]["batch"][0], event)

    def test_partial_verdicts_and_per_event_retry_sequences(self):
        events = {status: self.event(event=status) for status in ("ok", "retry", "drop", "warning")}
        self.http("/__control/state", {"analyticsV1Results": {
            events["retry"]["uuid"]: [{"result": "retry", "details": "not_persisted"}, "ok"],
            events["drop"]["uuid"]: "drop", events["warning"]["uuid"]: "warning",
        }})
        response, _, _ = self.post_batch(list(events.values()))
        self.assertEqual({key: result["result"] for key, result in response["results"].items()},
                         {event["uuid"]: status for status, event in events.items()})
        self.assertEqual({e["event"] for e in self.http("/__captured/events")}, {"ok", "warning"})
        # The fixture drives this second request; SDK retry behavior is tested in NativeTests.
        response, _, _ = self.post_batch([events["retry"], events["drop"]])
        self.assertEqual(response["results"][events["retry"]["uuid"]]["result"], "ok")
        self.assertEqual(response["results"][events["drop"]["uuid"]]["result"], "drop")
        requests = self.http("/__captured/analytics_requests")
        self.assertEqual([len(r["body"]["batch"]) for r in requests], [4, 2])
        self.assertEqual({e["event"] for e in self.http("/__captured/events")}, {"ok", "warning", "retry"})

    def test_invalid_options_drop_only_invalid_events(self):
        invalid = [self.event(options=value) for value in
                   ([], {"process_person_profile": "maybe"}, {"product_tour_id": False})]
        valid = self.event(options={"process_person_profile": 0, "product_tour_id": 42,
                                   "cookieless_mode": None, "unknown": {"ignored": True}})
        response, _, _ = self.post_batch([*invalid, valid])
        for event in invalid:
            self.assertEqual(response["results"][event["uuid"]], {"result": "drop", "details": "invalid_options"})
        self.assertEqual(response["results"][valid["uuid"]], {"result": "ok"})
        accepted = self.http("/__captured/events")
        self.assertEqual(len(accepted), 1)
        self.assertIs(accepted[0]["properties"]["$process_person_profile"], False)
        self.assertEqual(accepted[0]["properties"]["$product_tour_id"], "42")
        self.assertNotIn("$cookieless_mode", accepted[0]["properties"])
        self.assertNotIn("unknown", accepted[0]["properties"])

    def test_forced_failure_records_wire_not_acceptance_and_reset_clears_it(self):
        self.http("/__control/state", {"force500": {"analytics": True}})
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.post_batch([self.event()])
        self.assertEqual(error.exception.code, 500)
        self.assertEqual(self.http("/__captured/events"), [])
        self.assertEqual(self.http("/__captured/analytics_requests")[0]["status"], 500)
        self.http("/__control/reset", {})
        self.assertEqual(self.http("/__captured/analytics_requests"), [])
        self.post_batch([self.event()])
        self.assertEqual(len(self.http("/__captured/events")), 1)

    def test_invalid_verdict_controls_are_rejected(self):
        for rule in ([], "success", {"result": "retry", "details": []}, [["ok"]]):
            with self.assertRaises(urllib.error.HTTPError) as error:
                self.http("/__control/state", {"analyticsV1Results": {str(uuid.uuid4()): rule}})
            self.assertEqual(error.exception.code, 400)
        self.assertEqual(self.http("/__control/state")["analyticsV1Results"], {})

    def test_legacy_routes_keep_their_response_and_capture_shapes(self):
        event = self.event(properties={"$lib": "legacy", "$process_person_profile": False})
        self.assertEqual(self.http("/batch/", {"api_key": "phc_MOCK", "batch": [event]}), {"status": "ok"})
        captured = self.http("/__captured/events")
        self.assertEqual(captured[0]["properties"], event["properties"])
        self.assertEqual(self.http("/__captured/analytics_requests"), [])
        self.assertIs(self.http("/flags/?v=2", {"distinct_id": "fixture"})["featureFlags"]["disabled-flag"], False)
        self.assertEqual(self.http("/i/v1/logs", {}), {"status": "ok"})
        self.assertEqual(self.http("/s/", []), {"status": "ok"})
        self.http("/__control/state", {"force500": {"batch": True}})
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.http("/batch", {"batch": [event]})
        self.assertEqual(error.exception.code, 500)
        # Legacy records attempted events even when responding 500.
        self.assertEqual(len(self.http("/__captured/events")), 2)
        self.post_batch([self.event()])
        self.assertEqual(len(self.http("/__captured/events")), 3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--mock-server", required=True)
    args, remaining = parser.parse_known_args()
    AnalyticsV1MockTests.mock_server_path = args.mock_server
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
