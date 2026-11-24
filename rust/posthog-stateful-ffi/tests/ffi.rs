use std::ffi::{c_char, c_void, CStr, CString};
use std::io::Read;
use std::ptr;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use flate2::read::GzDecoder;
use httpmock::prelude::*;
use posthog_stateful_ffi::*;
use serde_json::{json, Value};

const CAPTURE_PATH: &str = "/i/v1/analytics/events";
const REPLAY_PATH: &str = "/s/";
const FLAGS_PATH: &str = "/flags/";

fn c(value: &str) -> CString {
    CString::new(value).unwrap()
}

fn last_error() -> Option<String> {
    let error = posthog_last_error();
    (!error.is_null()).then(|| {
        unsafe { CStr::from_ptr(error) }
            .to_string_lossy()
            .into_owned()
    })
}

unsafe fn take_string(value: *mut c_char) -> String {
    assert!(!value.is_null(), "expected non-NULL string");
    let text = CStr::from_ptr(value).to_str().unwrap().to_owned();
    posthog_string_free(value);
    text
}

fn decode_body(request: &HttpMockRequest) -> Value {
    let raw = request.body_ref();
    let bytes = if raw.starts_with(&[0x1f, 0x8b]) {
        let mut decoder = GzDecoder::new(raw);
        let mut out = Vec::new();
        decoder.read_to_end(&mut out).unwrap();
        out
    } else {
        raw.to_vec()
    };
    serde_json::from_slice(&bytes).unwrap()
}

unsafe fn make_config(
    server: &MockServer,
    storage_dir: Option<&str>,
) -> *mut PostHogStatefulConfig {
    let config = posthog_stateful_config_new(c("phc_test").as_ptr());
    assert!(!config.is_null());
    assert_eq!(
        posthog_stateful_config_set_host(config, c(&server.base_url()).as_ptr()),
        PostHogStatus::POSTHOG_STATUS_OK
    );
    match storage_dir {
        Some(dir) => assert_eq!(
            posthog_stateful_config_set_storage_dir(config, c(dir).as_ptr()),
            PostHogStatus::POSTHOG_STATUS_OK
        ),
        None => assert_eq!(
            posthog_stateful_config_set_storage_dir(config, ptr::null()),
            PostHogStatus::POSTHOG_STATUS_OK
        ),
    }
    assert_eq!(
        posthog_stateful_config_set_sdk_identity(
            config,
            c("posthog-unreal").as_ptr(),
            c("0.0.1").as_ptr()
        ),
        PostHogStatus::POSTHOG_STATUS_OK
    );
    assert_eq!(
        posthog_stateful_config_set_preload_feature_flags(config, false),
        PostHogStatus::POSTHOG_STATUS_OK
    );
    assert_eq!(
        posthog_stateful_config_set_flush_at(config, 100),
        PostHogStatus::POSTHOG_STATUS_OK
    );
    assert_eq!(
        posthog_stateful_config_set_flush_interval_ms(config, 60_000),
        PostHogStatus::POSTHOG_STATUS_OK
    );
    config
}

#[test]
fn captures_events_identifies_groups_and_persists_state() {
    let server = MockServer::start();
    let captured_batches = Arc::new(Mutex::new(Vec::<Value>::new()));
    let batches_ref = captured_batches.clone();

    let _config_mock = server.mock(|when, then| {
        when.method(GET).path("/array/phc_test/config");
        then.status(200)
            .json_body(json!({"hasFeatureFlags": false, "sessionRecording": false}));
    });
    let _capture_mock = server.mock(move |when, then| {
        when.method(POST).path(CAPTURE_PATH).is_true(move |req| {
            let body = decode_body(req);
            batches_ref.lock().unwrap().push(body);
            true
        });
        then.status(200).json_body(json!({"results": {}}));
    });

    let dir = tempfile::tempdir().unwrap();
    let storage = dir.path().to_str().unwrap();

    let first_anon = unsafe {
        let cfg = make_config(&server, Some(storage));
        posthog_stateful_config_set_platform_context_json(
            cfg,
            c(r#"{"$os":"TestOS","custom_ctx":"yes"}"#).as_ptr(),
        );
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);
        assert!(!client.is_null(), "{:?}", last_error());
        assert!(!posthog_stateful_is_disabled(client));
        assert!(!posthog_stateful_is_opt_out(client));

        let mut anon_ptr = ptr::null_mut();
        assert_eq!(
            posthog_stateful_get_anonymous_id(client, &mut anon_ptr),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let anon_id = take_string(anon_ptr);
        assert!(!anon_id.is_empty());

        assert_eq!(
            posthog_stateful_register(client, c(r#"{"super_key":"super_val"}"#).as_ptr()),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            posthog_stateful_identify(
                client,
                c("user-42").as_ptr(),
                c(r#"{"plan":"pro"}"#).as_ptr(),
                c(r#"{"signup":"2026"}"#).as_ptr(),
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            posthog_stateful_group(
                client,
                c("company").as_ptr(),
                c("acme").as_ptr(),
                c(r#"{"size":50}"#).as_ptr(),
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );

        let mut outcome = PostHogCaptureOutcome::POSTHOG_CAPTURE_INVALID;
        assert_eq!(
            posthog_stateful_capture(
                client,
                c("level_completed").as_ptr(),
                c(r#"{"score":99}"#).as_ptr(),
                ptr::null(),
                &mut outcome,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(outcome, PostHogCaptureOutcome::POSTHOG_CAPTURE_ACCEPTED);

        let mut report = PostHogDrainReport {
            state: PostHogPersistenceOutcome::POSTHOG_PERSISTENCE_UNKNOWN,
            delivery: PostHogDeliveryOutcome::POSTHOG_DELIVERY_UNKNOWN,
        };
        assert_eq!(
            posthog_stateful_shutdown(client, &mut report),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            report.state,
            PostHogPersistenceOutcome::POSTHOG_PERSISTENCE_PERSISTED
        );
        assert_eq!(
            report.delivery,
            PostHogDeliveryOutcome::POSTHOG_DELIVERY_STOPPED
        );
        posthog_stateful_client_free(client);
        anon_id
    };

    // Verify wire events
    let batches = captured_batches.lock().unwrap();
    let events: Vec<&Value> = batches
        .iter()
        .flat_map(|b| b["batch"].as_array().into_iter().flatten())
        .collect();
    let level_event = events
        .iter()
        .find(|e| e["event"] == "level_completed")
        .expect("level_completed should be captured");
    assert_eq!(level_event["distinct_id"], "user-42");
    assert_eq!(level_event["properties"]["score"], 99);
    assert_eq!(level_event["properties"]["super_key"], "super_val");
    assert_eq!(level_event["properties"]["custom_ctx"], "yes");
    assert_eq!(level_event["properties"]["$groups"]["company"], "acme");
    assert_eq!(level_event["properties"]["$lib"], "posthog-unreal");
    assert_eq!(level_event["properties"]["$lib_version"], "0.0.1");

    // Reopen from same directory and verify identity survived restart
    unsafe {
        let cfg = make_config(&server, Some(storage));
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);
        let mut distinct_ptr = ptr::null_mut();
        let mut anon_ptr = ptr::null_mut();
        posthog_stateful_get_distinct_id(client, &mut distinct_ptr);
        posthog_stateful_get_anonymous_id(client, &mut anon_ptr);
        assert_eq!(take_string(distinct_ptr), "user-42");
        assert_eq!(take_string(anon_ptr), first_anon);
        posthog_stateful_client_free(client);
    }
}

#[test]
fn before_send_mutates_and_drops_events() {
    let server = MockServer::start();
    let captured = Arc::new(Mutex::new(Vec::<Value>::new()));
    let cap_ref = captured.clone();

    let _config_mock = server.mock(|when, then| {
        when.method(GET).path("/array/phc_test/config");
        then.status(200)
            .json_body(json!({"hasFeatureFlags": false, "sessionRecording": false}));
    });
    let _capture_mock = server.mock(move |when, then| {
        when.method(POST).path(CAPTURE_PATH).is_true(move |req| {
            let body = decode_body(req);
            if let Some(batch) = body["batch"].as_array() {
                cap_ref.lock().unwrap().extend(batch.iter().cloned());
            }
            true
        });
        then.status(200).json_body(json!({"results": {}}));
    });

    unsafe extern "C" fn hook(
        _user_data: *mut c_void,
        event_json: *const c_char,
        out_event_json: *mut *mut c_char,
    ) -> bool {
        let mut event: Value =
            serde_json::from_str(CStr::from_ptr(event_json).to_str().unwrap()).unwrap();
        if event["event"] == "drop_me" {
            return false;
        }
        if event["event"] == "mutate_me" {
            event["properties"]["scrubbed"] = json!(true);
            let encoded = CString::new(event.to_string()).unwrap();
            *out_event_json = posthog_string_copy(encoded.as_ptr());
        }
        true
    }

    unsafe {
        let cfg = make_config(&server, None);
        assert_eq!(
            posthog_stateful_config_set_before_send(cfg, Some(hook), ptr::null_mut()),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);

        let mut out = PostHogCaptureOutcome::POSTHOG_CAPTURE_INVALID;
        posthog_stateful_capture(
            client,
            c("drop_me").as_ptr(),
            ptr::null(),
            ptr::null(),
            &mut out,
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_DROPPED);

        posthog_stateful_capture(
            client,
            c("mutate_me").as_ptr(),
            ptr::null(),
            ptr::null(),
            &mut out,
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_ACCEPTED);

        posthog_stateful_capture(
            client,
            c("keep_me").as_ptr(),
            ptr::null(),
            ptr::null(),
            &mut out,
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_ACCEPTED);

        posthog_stateful_shutdown(client, ptr::null_mut());
        posthog_stateful_client_free(client);
    }

    let events = captured.lock().unwrap();
    assert!(!events.iter().any(|e| e["event"] == "drop_me"));
    let mutated = events.iter().find(|e| e["event"] == "mutate_me").unwrap();
    assert_eq!(mutated["properties"]["scrubbed"], true);
    assert!(events.iter().any(|e| e["event"] == "keep_me"));
}

#[test]
fn feature_flags_load_notify_and_read() {
    let server = MockServer::start();
    let _config_mock = server.mock(|when, then| {
        when.method(GET).path("/array/phc_test/config");
        then.status(200)
            .json_body(json!({"hasFeatureFlags": true, "sessionRecording": false}));
    });
    let _flags_mock = server.mock(|when, then| {
        when.method(POST).path(FLAGS_PATH);
        then.status(200).json_body(json!({
            "flags": {
                "beta-ui": {
                    "key": "beta-ui",
                    "enabled": true,
                    "variant": "v2",
                    "metadata": {
                        "id": 10,
                        "version": 3,
                        "payload": "{\"color\":\"blue\"}"
                    }
                },
                "disabled-flag": {
                    "key": "disabled-flag",
                    "enabled": false
                }
            },
            "errorsWhileComputingFlags": false,
            "requestId": "req-123"
        }));
    });
    let _capture_mock = server.mock(|when, then| {
        when.method(POST).path(CAPTURE_PATH);
        then.status(200).json_body(json!({"results": {}}));
    });

    struct FlagWaiter {
        updates: Mutex<Vec<Value>>,
        cv: Condvar,
    }
    let waiter = Box::new(FlagWaiter {
        updates: Mutex::new(Vec::new()),
        cv: Condvar::new(),
    });

    unsafe extern "C" fn on_flags(user_data: *mut c_void, update_json: *const c_char) {
        let waiter = &*(user_data as *const FlagWaiter);
        let parsed: Value =
            serde_json::from_str(CStr::from_ptr(update_json).to_str().unwrap()).unwrap();
        waiter.updates.lock().unwrap().push(parsed);
        waiter.cv.notify_all();
    }

    unsafe {
        let cfg = make_config(&server, None);
        posthog_stateful_config_set_preload_feature_flags(cfg, true);
        posthog_stateful_config_set_on_feature_flags(
            cfg,
            Some(on_flags),
            (&*waiter as *const FlagWaiter) as *mut c_void,
        );
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);

        // Wait for remote flags update
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut updates = waiter.updates.lock().unwrap();
        while !updates
            .iter()
            .any(|u| u["flags"]["beta-ui"]["enabled"] == true)
        {
            let rem = deadline.saturating_duration_since(Instant::now());
            assert!(!rem.is_zero(), "timed out waiting for flag update");
            let (guard, _) = waiter.cv.wait_timeout(updates, rem).unwrap();
            updates = guard;
        }
        drop(updates);

        let mut flag_ptr = ptr::null_mut();
        assert_eq!(
            posthog_stateful_get_feature_flag_json(
                client,
                c("beta-ui").as_ptr(),
                PostHogFlagReporting::POSTHOG_FLAG_REPORTING_SILENT as i32,
                &mut flag_ptr,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let flag: Value = serde_json::from_str(&take_string(flag_ptr)).unwrap();
        assert_eq!(flag["enabled"], true);
        assert_eq!(flag["variant"], "v2");
        assert_eq!(flag["has_payload"], true);
        assert_eq!(flag["payload"], json!({"color": "blue"}));

        let mut all_ptr = ptr::null_mut();
        assert_eq!(
            posthog_stateful_get_feature_flags_json(client, &mut all_ptr),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let all: Value = serde_json::from_str(&take_string(all_ptr)).unwrap();
        assert_eq!(all["beta-ui"]["enabled"], true);
        assert_eq!(all["disabled-flag"]["enabled"], false);

        posthog_stateful_client_free(client);
    }
}

#[test]
fn session_replay_encodes_and_delivers_jpeg_snapshot() {
    let server = MockServer::start();
    let snapshots = Arc::new(Mutex::new(Vec::<Value>::new()));
    let snap_ref = snapshots.clone();

    let _config_mock = server.mock(|when, then| {
        when.method(GET).path("/array/phc_test/config");
        then.status(200)
            .json_body(json!({"hasFeatureFlags": false, "sessionRecording": true}));
    });
    let _capture_mock = server.mock(|when, then| {
        when.method(POST).path(CAPTURE_PATH);
        then.status(200).json_body(json!({"results": {}}));
    });
    let _replay_mock = server.mock(move |when, then| {
        when.method(POST).path(REPLAY_PATH).is_true(move |req| {
            let body = decode_body(req);
            snap_ref.lock().unwrap().push(body);
            true
        });
        then.status(200).json_body(json!({"status": 1}));
    });

    unsafe {
        let cfg = make_config(&server, None);
        posthog_stateful_config_set_replay_throttle_delay_ms(cfg, 0);
        posthog_stateful_config_set_replay_jpeg_quality(cfg, 75);
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);

        assert!(!posthog_stateful_replay_is_active(client));
        posthog_stateful_replay_start(client);

        // Wait for remote config to permit replay and frame acquisition to succeed
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut frame = ptr::null_mut();
        let mut reason = PostHogFrameUnavailable::POSTHOG_FRAME_UNKNOWN;
        while Instant::now() < deadline {
            assert_eq!(
                posthog_stateful_replay_begin_frame(
                    client,
                    16,
                    16,
                    16 * 4,
                    &mut frame,
                    &mut reason
                ),
                PostHogStatus::POSTHOG_STATUS_OK
            );
            if !frame.is_null() {
                break;
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        assert!(!frame.is_null(), "frame unavailable: {reason:?}");
        assert_eq!(reason, PostHogFrameUnavailable::POSTHOG_FRAME_OK);
        assert!(posthog_stateful_replay_is_active(client));

        // 16x16 BGRA8888 red image
        let mut pixels = vec![0u8; 16 * 16 * 4];
        for px in pixels.chunks_exact_mut(4) {
            px[0] = 0; // B
            px[1] = 0; // G
            px[2] = 255; // R
            px[3] = 255; // A
        }
        let mut submit_outcome = PostHogSubmitOutcome::POSTHOG_SUBMIT_CLOSED;
        assert_eq!(
            posthog_stateful_replay_submit_frame(
                client,
                frame,
                pixels.as_ptr(),
                pixels.len(),
                &mut submit_outcome
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            submit_outcome,
            PostHogSubmitOutcome::POSTHOG_SUBMIT_ACCEPTED
        );

        posthog_stateful_flush(client, ptr::null_mut());
        posthog_stateful_replay_stop(client);
        assert!(!posthog_stateful_replay_is_active(client));
        posthog_stateful_client_free(client);
    }

    let snaps = snapshots.lock().unwrap();
    assert!(
        !snaps.is_empty(),
        "expected at least one replay snapshot batch"
    );
    let raw_str = snaps[0].to_string();
    assert!(
        raw_str.contains("data:image/jpeg;base64,"),
        "expected JPEG data URI in snapshot payload"
    );
}

#[test]
fn reports_errors_and_validates_null_arguments() {
    unsafe {
        assert!(posthog_stateful_config_new(ptr::null()).is_null());
        assert!(last_error().unwrap().contains("project_token"));

        let mut out = PostHogCaptureOutcome::POSTHOG_CAPTURE_ACCEPTED;
        assert_eq!(
            posthog_stateful_capture(
                ptr::null(),
                c("ev").as_ptr(),
                ptr::null(),
                ptr::null(),
                &mut out
            ),
            PostHogStatus::POSTHOG_STATUS_NULL_ARGUMENT
        );

        let cfg = posthog_stateful_config_new(c("phc_test").as_ptr());
        assert_eq!(
            posthog_stateful_config_set_platform_context_json(cfg, c("[1,2,3]").as_ptr()),
            PostHogStatus::POSTHOG_STATUS_INVALID_JSON
        );

        // Consuming config twice fails cleanly
        posthog_stateful_config_set_storage_dir(cfg, ptr::null());
        posthog_stateful_config_set_disabled(cfg, true);
        let client = posthog_stateful_client_new(cfg);
        assert!(!client.is_null());
        assert!(posthog_stateful_is_disabled(client));
        assert!(posthog_stateful_client_new(cfg).is_null());
        assert!(last_error().unwrap().contains("consumed"));

        posthog_stateful_config_free(cfg);
        posthog_stateful_client_free(client);
    }
}

#[test]
fn before_send_accepts_rfc3339_timestamps_with_z_and_offset() {
    let server = MockServer::start();
    let captured = Arc::new(Mutex::new(Vec::<Value>::new()));
    let cap_ref = captured.clone();

    let _config_mock = server.mock(|when, then| {
        when.method(GET).path("/array/phc_test/config");
        then.status(200)
            .json_body(json!({"hasFeatureFlags": false, "sessionRecording": false}));
    });
    let _capture_mock = server.mock(move |when, then| {
        when.method(POST).path(CAPTURE_PATH).is_true(move |req| {
            let body = decode_body(req);
            if let Some(batch) = body["batch"].as_array() {
                cap_ref.lock().unwrap().extend(batch.iter().cloned());
            }
            true
        });
        then.status(200).json_body(json!({"results": {}}));
    });

    unsafe extern "C" fn ts_hook(
        _user_data: *mut c_void,
        event_json: *const c_char,
        out_event_json: *mut *mut c_char,
    ) -> bool {
        let mut event: Value =
            serde_json::from_str(CStr::from_ptr(event_json).to_str().unwrap()).unwrap();
        if event["event"] == "ts_z" {
            event["timestamp"] = json!("2026-03-15T10:20:30.500Z");
        } else if event["event"] == "ts_offset" {
            event["timestamp"] = json!("2026-03-15T12:20:30+02:00");
        } else {
            return true;
        }
        let encoded = CString::new(event.to_string()).unwrap();
        *out_event_json = posthog_string_copy(encoded.as_ptr());
        true
    }

    unsafe {
        let cfg = make_config(&server, None);
        assert_eq!(
            posthog_stateful_config_set_before_send(cfg, Some(ts_hook), ptr::null_mut()),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);

        let mut out = PostHogCaptureOutcome::POSTHOG_CAPTURE_INVALID;
        assert_eq!(
            posthog_stateful_capture(
                client,
                c("ts_z").as_ptr(),
                ptr::null(),
                ptr::null(),
                &mut out
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_ACCEPTED);

        assert_eq!(
            posthog_stateful_capture(
                client,
                c("ts_offset").as_ptr(),
                ptr::null(),
                ptr::null(),
                &mut out
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_ACCEPTED);

        posthog_stateful_shutdown(client, ptr::null_mut());
        posthog_stateful_client_free(client);
    }

    let events = captured.lock().unwrap();
    let ts_z = events.iter().find(|e| e["event"] == "ts_z").unwrap();
    assert!(
        ts_z["timestamp"]
            .as_str()
            .unwrap()
            .starts_with("2026-03-15T10:20:30.5"),
        "unexpected Z timestamp: {}",
        ts_z["timestamp"]
    );
    let ts_offset = events.iter().find(|e| e["event"] == "ts_offset").unwrap();
    assert!(
        ts_offset["timestamp"]
            .as_str()
            .unwrap()
            .starts_with("2026-03-15T10:20:30"),
        "unexpected offset timestamp: {}",
        ts_offset["timestamp"]
    );
}

#[test]
fn replay_ffi_error_paths_consume_frame_and_abandon_frees_capacity() {
    let server = MockServer::start();
    let _config_mock = server.mock(|when, then| {
        when.method(GET).path("/array/phc_test/config");
        then.status(200)
            .json_body(json!({"hasFeatureFlags": false, "sessionRecording": true}));
    });
    let _capture_mock = server.mock(|when, then| {
        when.method(POST).path(CAPTURE_PATH);
        then.status(200).json_body(json!({"results": {}}));
    });
    let _replay_mock = server.mock(|when, then| {
        when.method(POST).path(REPLAY_PATH);
        then.status(200).json_body(json!({"status": 1}));
    });

    unsafe fn wait_for_frame(client: *const PostHogStatefulClient) -> *mut PostHogReplayFrame {
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut frame = ptr::null_mut();
        let mut reason = PostHogFrameUnavailable::POSTHOG_FRAME_UNKNOWN;
        while Instant::now() < deadline {
            assert_eq!(
                posthog_stateful_replay_begin_frame(client, 8, 8, 8 * 4, &mut frame, &mut reason),
                PostHogStatus::POSTHOG_STATUS_OK
            );
            if !frame.is_null() {
                assert_eq!(reason, PostHogFrameUnavailable::POSTHOG_FRAME_OK);
                return frame;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        panic!("timed out acquiring replay frame: {reason:?}");
    }

    unsafe {
        let cfg = make_config(&server, None);
        posthog_stateful_config_set_replay_throttle_delay_ms(cfg, 0);
        posthog_stateful_config_set_replay_max_frames_in_flight(cfg, 1);
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);
        posthog_stateful_replay_start(client);

        let pixels = vec![0u8; 8 * 8 * 4];
        let mut outcome = PostHogSubmitOutcome::POSTHOG_SUBMIT_ACCEPTED;

        // 1. Mismatched pixels_len returns INVALID_ARGUMENT and still consumes/frees frame.
        let frame1 = wait_for_frame(client);
        assert_eq!(
            posthog_stateful_replay_submit_frame(
                client,
                frame1,
                pixels.as_ptr(),
                pixels.len() - 4,
                &mut outcome,
            ),
            PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT
        );
        assert!(last_error().unwrap().contains("pixels_len"));

        // 2. NULL pixels returns NULL_ARGUMENT and still consumes/frees frame.
        let frame2 = wait_for_frame(client);
        assert_eq!(
            posthog_stateful_replay_submit_frame(
                client,
                frame2,
                ptr::null(),
                pixels.len(),
                &mut outcome,
            ),
            PostHogStatus::POSTHOG_STATUS_NULL_ARGUMENT
        );
        assert!(last_error().unwrap().contains("pixels"));

        // 3. Abandoning a frame via posthog_stateful_replay_frame_free releases capacity.
        let frame3 = wait_for_frame(client);
        posthog_stateful_replay_frame_free(frame3);

        // 4. Subsequent acquisition & valid submit succeeds.
        let frame4 = wait_for_frame(client);
        assert_eq!(
            posthog_stateful_replay_submit_frame(
                client,
                frame4,
                pixels.as_ptr(),
                pixels.len(),
                &mut outcome,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(outcome, PostHogSubmitOutcome::POSTHOG_SUBMIT_ACCEPTED);

        posthog_stateful_client_free(client);
    }
}

#[test]
fn feature_flags_filter_failed_records_and_handle_empty_variant() {
    let server = MockServer::start();
    let _config_mock = server.mock(|when, then| {
        when.method(GET).path("/array/phc_test/config");
        then.status(200)
            .json_body(json!({"hasFeatureFlags": true, "sessionRecording": false}));
    });
    let _flags_mock = server.mock(|when, then| {
        when.method(POST).path(FLAGS_PATH);
        then.status(200).json_body(json!({
            "flags": {
                "failed-flag": {
                    "key": "failed-flag",
                    "enabled": true,
                    "failed": true
                },
                "empty-variant": {
                    "key": "empty-variant",
                    "enabled": true,
                    "variant": ""
                },
                "healthy-flag": {
                    "key": "healthy-flag",
                    "enabled": true,
                    "variant": "control"
                }
            },
            "errorsWhileComputingFlags": false,
            "requestId": "req-spec-211"
        }));
    });
    let _capture_mock = server.mock(|when, then| {
        when.method(POST).path(CAPTURE_PATH);
        then.status(200).json_body(json!({"results": {}}));
    });

    struct FlagWaiter {
        updates: Mutex<Vec<Value>>,
        cv: Condvar,
    }
    let waiter = Box::new(FlagWaiter {
        updates: Mutex::new(Vec::new()),
        cv: Condvar::new(),
    });

    unsafe extern "C" fn on_flags(user_data: *mut c_void, update_json: *const c_char) {
        let waiter = &*(user_data as *const FlagWaiter);
        let parsed: Value =
            serde_json::from_str(CStr::from_ptr(update_json).to_str().unwrap()).unwrap();
        waiter.updates.lock().unwrap().push(parsed);
        waiter.cv.notify_all();
    }

    unsafe {
        let cfg = make_config(&server, None);
        posthog_stateful_config_set_preload_feature_flags(cfg, true);
        posthog_stateful_config_set_on_feature_flags(
            cfg,
            Some(on_flags),
            (&*waiter as *const FlagWaiter) as *mut c_void,
        );
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);

        let deadline = Instant::now() + Duration::from_secs(5);
        let mut updates = waiter.updates.lock().unwrap();
        while !updates
            .iter()
            .any(|u| u["flags"]["healthy-flag"]["enabled"] == true)
        {
            let rem = deadline.saturating_duration_since(Instant::now());
            assert!(!rem.is_zero(), "timed out waiting for flag update");
            let (guard, _) = waiter.cv.wait_timeout(updates, rem).unwrap();
            updates = guard;
        }
        drop(updates);

        // Spec #211: failed: true flags must be omitted (not treated as enabled).
        let mut failed_ptr = ptr::null_mut();
        assert_eq!(
            posthog_stateful_get_feature_flag_json(
                client,
                c("failed-flag").as_ptr(),
                PostHogFlagReporting::POSTHOG_FLAG_REPORTING_SILENT as i32,
                &mut failed_ptr,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert!(
            failed_ptr.is_null(),
            "failed: true flag should be filtered out"
        );

        // Empty-string variant "" is normalized as disabled while preserving the variant string.
        let mut empty_ptr = ptr::null_mut();
        assert_eq!(
            posthog_stateful_get_feature_flag_json(
                client,
                c("empty-variant").as_ptr(),
                PostHogFlagReporting::POSTHOG_FLAG_REPORTING_SILENT as i32,
                &mut empty_ptr,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let empty_flag: Value = serde_json::from_str(&take_string(empty_ptr)).unwrap();
        assert_eq!(empty_flag["enabled"], false);
        assert_eq!(empty_flag["variant"], "");

        let mut all_ptr = ptr::null_mut();
        assert_eq!(
            posthog_stateful_get_feature_flags_json(client, &mut all_ptr),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let all: Value = serde_json::from_str(&take_string(all_ptr)).unwrap();
        assert!(all.get("failed-flag").is_none());
        assert_eq!(all["healthy-flag"]["variant"], "control");
        assert_eq!(all["empty-variant"]["enabled"], false);
        assert_eq!(all["empty-variant"]["variant"], "");

        posthog_stateful_client_free(client);
    }
}

#[test]
fn before_send_handles_optional_fields_unknown_fields_and_blank_identifiers() {
    let server = MockServer::start();
    let captured = Arc::new(Mutex::new(Vec::<Value>::new()));
    let cap_ref = captured.clone();

    let _config_mock = server.mock(|when, then| {
        when.method(GET).path("/array/phc_test/config");
        then.status(200)
            .json_body(json!({"hasFeatureFlags": false, "sessionRecording": false}));
    });
    let _capture_mock = server.mock(move |when, then| {
        when.method(POST).path(CAPTURE_PATH).is_true(move |req| {
            let body = decode_body(req);
            if let Some(batch) = body["batch"].as_array() {
                cap_ref.lock().unwrap().extend(batch.iter().cloned());
            }
            true
        });
        then.status(200).json_body(json!({"results": {}}));
    });

    let errors = Box::new(Mutex::new(Vec::<String>::new()));

    unsafe extern "C" fn on_error(user_data: *mut c_void, message: *const c_char) {
        let errors = &*(user_data as *const Mutex<Vec<String>>);
        let text = CStr::from_ptr(message).to_string_lossy().into_owned();
        errors.lock().unwrap().push(text);
    }

    unsafe extern "C" fn hook(
        _user_data: *mut c_void,
        event_json: *const c_char,
        out_event_json: *mut *mut c_char,
    ) -> bool {
        let mut event: Value =
            serde_json::from_str(CStr::from_ptr(event_json).to_str().unwrap()).unwrap();
        let name = event["event"].as_str().unwrap_or_default().to_owned();
        let obj = event.as_object_mut().unwrap();
        match name.as_str() {
            "omit_groups" => {
                obj.remove("groups");
            }
            "unknown_field" => {
                obj.insert("secret_extra_field".into(), json!("super_secret_value"));
            }
            "blank_distinct_id" => {
                obj.insert("distinct_id".into(), json!("   "));
            }
            "blank_event_name" => {
                obj.insert("event".into(), json!("   "));
            }
            _ => return true,
        }
        let encoded = CString::new(event.to_string()).unwrap();
        *out_event_json = posthog_string_copy(encoded.as_ptr());
        true
    }

    unsafe {
        let cfg = make_config(&server, None);
        assert_eq!(
            posthog_stateful_config_set_before_send(cfg, Some(hook), ptr::null_mut()),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            posthog_stateful_config_set_on_error(
                cfg,
                Some(on_error),
                (&*errors as *const Mutex<Vec<String>>) as *mut c_void,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);

        let mut out = PostHogCaptureOutcome::POSTHOG_CAPTURE_INVALID;
        // (a) Omitting "groups" is accepted.
        assert_eq!(
            posthog_stateful_capture(
                client,
                c("omit_groups").as_ptr(),
                c(r#"{"ok":true}"#).as_ptr(),
                ptr::null(),
                &mut out,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_ACCEPTED);

        // (b) Unknown top-level field drops and invokes on_error without leaking raw JSON.
        assert_eq!(
            posthog_stateful_capture(
                client,
                c("unknown_field").as_ptr(),
                ptr::null(),
                ptr::null(),
                &mut out,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_DROPPED);

        // (c) Blank distinct_id drops (never accepted as anonymous) and invokes on_error.
        assert_eq!(
            posthog_stateful_capture(
                client,
                c("blank_distinct_id").as_ptr(),
                ptr::null(),
                ptr::null(),
                &mut out,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_DROPPED);

        // (d) Blank event name is classified as POSTHOG_CAPTURE_INVALID by posthog-rs.
        assert_eq!(
            posthog_stateful_capture(
                client,
                c("blank_event_name").as_ptr(),
                ptr::null(),
                ptr::null(),
                &mut out,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(out, PostHogCaptureOutcome::POSTHOG_CAPTURE_INVALID);

        posthog_stateful_shutdown(client, ptr::null_mut());
        posthog_stateful_client_free(client);
    }

    let events = captured.lock().unwrap();
    assert_eq!(events.len(), 1);
    assert_eq!(events[0]["event"], "omit_groups");
    assert_eq!(events[0]["properties"]["ok"], true);

    let reported = errors.lock().unwrap();
    assert!(
        reported
            .iter()
            .any(|msg| msg.contains("invalid replacement event JSON")),
        "expected invalid replacement JSON error, got: {reported:?}"
    );
    assert!(
        reported.iter().any(|msg| msg.contains("blank distinct_id")),
        "expected blank distinct_id error, got: {reported:?}"
    );
    assert!(
        !reported
            .iter()
            .any(|msg| msg.contains("super_secret_value") || msg.contains("secret_extra_field")),
        "error callback must not leak raw event JSON or values: {reported:?}"
    );
}

#[test]
fn rejects_invalid_enum_discriminants() {
    unsafe {
        let cfg = posthog_stateful_config_new(c("phc_test").as_ptr());
        assert!(!cfg.is_null());

        for valid in [
            PostHogPersonProfiles::POSTHOG_PERSON_PROFILES_NEVER as i32,
            PostHogPersonProfiles::POSTHOG_PERSON_PROFILES_IDENTIFIED_ONLY as i32,
            PostHogPersonProfiles::POSTHOG_PERSON_PROFILES_ALWAYS as i32,
        ] {
            assert_eq!(
                posthog_stateful_config_set_person_profiles(cfg, valid),
                PostHogStatus::POSTHOG_STATUS_OK
            );
        }
        for invalid in [-1, 3, 99] {
            assert_eq!(
                posthog_stateful_config_set_person_profiles(cfg, invalid),
                PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT
            );
            assert!(last_error().is_some());
        }

        for valid in [
            PostHogSessionMode::POSTHOG_SESSION_MODE_AUTOMATIC as i32,
            PostHogSessionMode::POSTHOG_SESSION_MODE_MANUAL as i32,
        ] {
            assert_eq!(
                posthog_stateful_config_set_session_mode(cfg, valid),
                PostHogStatus::POSTHOG_STATUS_OK
            );
        }
        for invalid in [-1, 2, 99] {
            assert_eq!(
                posthog_stateful_config_set_session_mode(cfg, invalid),
                PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT
            );
            assert!(last_error().is_some());
        }

        posthog_stateful_config_set_storage_dir(cfg, ptr::null());
        posthog_stateful_config_set_disabled(cfg, true);
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);
        assert!(!client.is_null());

        for invalid in [-1, 3, 99] {
            let mut flag_ptr = ptr::null_mut();
            assert_eq!(
                posthog_stateful_get_feature_flag_json(
                    client,
                    c("some-flag").as_ptr(),
                    invalid,
                    &mut flag_ptr,
                ),
                PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT
            );
            assert!(flag_ptr.is_null());
            assert!(last_error().is_some());
        }

        posthog_stateful_client_free(client);
    }
}

#[test]
fn shutdown_detaches_callbacks_and_is_safe_from_callback_thread() {
    use std::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};

    struct HookState {
        client_ptr: AtomicPtr<PostHogStatefulClient>,
        calls: AtomicUsize,
    }

    unsafe extern "C" fn shutdown_inside_before_send(
        user_data: *mut c_void,
        _event_json: *const c_char,
        _out_event_json: *mut *mut c_char,
    ) -> bool {
        let state = &*(user_data as *const HookState);
        state.calls.fetch_add(1, Ordering::SeqCst);
        let client = state.client_ptr.load(Ordering::SeqCst);
        if !client.is_null() {
            let status = posthog_stateful_shutdown(client, ptr::null_mut());
            assert_eq!(status, PostHogStatus::POSTHOG_STATUS_OK);
        }
        true
    }

    let dir = tempfile::tempdir().unwrap();
    let storage = dir.path().to_string_lossy().into_owned();
    let state = Box::new(HookState {
        client_ptr: AtomicPtr::new(ptr::null_mut()),
        calls: AtomicUsize::new(0),
    });
    let state_ptr = &*state as *const HookState as *mut c_void;

    unsafe {
        let cfg = posthog_stateful_config_new(c("phc_test").as_ptr());
        assert_eq!(
            posthog_stateful_config_set_host(cfg, c("http://127.0.0.1:9").as_ptr()),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            posthog_stateful_config_set_storage_dir(cfg, c(&storage).as_ptr()),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            posthog_stateful_config_set_preload_feature_flags(cfg, false),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            posthog_stateful_config_set_shutdown_timeout_ms(cfg, 200),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(
            posthog_stateful_config_set_before_send(
                cfg,
                Some(shutdown_inside_before_send),
                state_ptr,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        let client = posthog_stateful_client_new(cfg);
        posthog_stateful_config_free(cfg);
        assert!(!client.is_null());
        state.client_ptr.store(client, Ordering::SeqCst);

        // Must not deadlock when posthog_stateful_shutdown is invoked from inside before_send.
        let mut outcome = PostHogCaptureOutcome::POSTHOG_CAPTURE_INVALID;
        assert_eq!(
            posthog_stateful_capture(
                client,
                c("trigger").as_ptr(),
                ptr::null(),
                ptr::null(),
                &mut outcome,
            ),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        assert_eq!(state.calls.load(Ordering::SeqCst), 1);

        // Explicit shutdown off the callback thread detaches callbacks cleanly.
        assert_eq!(
            posthog_stateful_shutdown(client, ptr::null_mut()),
            PostHogStatus::POSTHOG_STATUS_OK
        );
        posthog_stateful_client_free(client);
    }
    drop(state);
}
