use super::*;
use std::ffi::c_void;
use std::sync::atomic::{AtomicI32, AtomicU64, Ordering};

fn bytes(value: &str) -> Bytes {
    Bytes::from_slice(value.as_bytes())
}
fn create(extra: Value, hooks: Hooks) -> u64 {
    let mut options = json!({"project_token":"phc_test","host":"http://127.0.0.1:9","preload_feature_flags":false,
        "shutdown_timeout_ms":100,"request_timeout_seconds":1});
    options
        .as_object_mut()
        .unwrap()
        .extend(extra.as_object().unwrap().clone());
    let mut handle = 0;
    assert_eq!(
        unsafe { phu_create(3, bytes(&options.to_string()), hooks, &mut handle) },
        Status::Ok
    );
    handle
}
fn no_hooks() -> Hooks {
    Hooks {
        before_send: None,
        user_data: std::ptr::null_mut(),
    }
}
fn read(handle: u64, query: Value) -> Value {
    let mut out = Buffer::empty();
    assert_eq!(
        unsafe { phu_read(handle, bytes(&query.to_string()), &mut out) },
        Status::Ok
    );
    let value =
        unsafe { serde_json::from_slice(std::slice::from_raw_parts(out.data, out.len)).unwrap() };
    unsafe {
        phu_buffer_free(out);
    }
    value
}
fn command(handle: u64, value: Value) -> Status {
    unsafe { phu_command(handle, bytes(&value.to_string())) }
}
fn capture(handle: u64) -> Status {
    let mut out = Buffer::empty();
    let result = unsafe {
        phu_capture(
            handle,
            bytes(r#"{"event":"test","options":{"process_person_profile":true}}"#),
            &mut out,
        )
    };
    unsafe {
        phu_buffer_free(out);
    }
    result
}
fn close(handle: u64) -> (Status, Option<Value>) {
    let mut out = Buffer::empty();
    let status = unsafe { phu_close(handle, &mut out) };
    let value = if out.data.is_null() {
        None
    } else {
        Some(unsafe {
            serde_json::from_slice(std::slice::from_raw_parts(out.data, out.len)).unwrap()
        })
    };
    unsafe {
        phu_buffer_free(out);
    }
    (status, value)
}
#[test]
fn coordination_uses_sdk_host_equivalence_and_trimmed_tokens_without_initialization() {
    let key = |host: &str, token: &str, instance: &str| {
        let mut out = Buffer::empty();
        let input = json!({"host":host,"project_token":token,"instance_name":instance});
        assert_eq!(
            unsafe { phu_coordination_key(bytes(&input.to_string()), &mut out) },
            Status::Ok
        );
        let value: String = unsafe {
            serde_json::from_slice(std::slice::from_raw_parts(out.data, out.len)).unwrap()
        };
        unsafe {
            phu_buffer_free(out);
        }
        value
    };
    let canonical = key(posthog_rs::DEFAULT_HOST, "phc_test", "owner");
    assert_eq!(canonical.len(), 40);
    for host in [
        "",
        " \t",
        "https://app.posthog.com",
        "https://us.posthog.com/",
        " \thttps://us.i.posthog.com///\n",
    ] {
        assert_eq!(key(host, "\u{2003}phc_test \n", "owner"), canonical);
    }
    assert_eq!(
        key(" https://eu.posthog.com/// ", "phc_test", "owner"),
        key("https://eu.i.posthog.com", "phc_test", "owner")
    );
    assert_eq!(
        key(" http://127.0.0.1:9/// ", "phc_test", "owner"),
        key("http://127.0.0.1:9", "phc_test", "owner")
    );
    assert_ne!(
        key(posthog_rs::DEFAULT_HOST, "different", "owner"),
        canonical
    );
    assert_ne!(
        key(posthog_rs::DEFAULT_HOST, "phc_test", "different"),
        canonical
    );
    assert_ne!(
        key("https://eu.posthog.com", "phc_test", "owner"),
        canonical
    );
    let mut out = Buffer::empty();
    assert_eq!(
        unsafe { phu_coordination_key(bytes("[]"), &mut out) },
        Status::InvalidInput
    );
    assert!(out.data.is_null());
    assert_eq!(
        unsafe { phu_coordination_key(bytes("{}"), std::ptr::null_mut()) },
        Status::InvalidInput
    );
}

#[test]
fn malformed_inputs_and_abi_mismatch_zero_outputs() {
    let mut handle = 42;
    assert_eq!(
        unsafe { phu_create(2, bytes("{}"), no_hooks(), &mut handle) },
        Status::UnsupportedAbi
    );
    assert_eq!(handle, 0);
    assert_eq!(
        unsafe { phu_create(3, bytes("[]"), no_hooks(), &mut handle) },
        Status::InvalidInput
    );
    assert_eq!(
        unsafe { phu_create(3, bytes("{}"), no_hooks(), std::ptr::null_mut()) },
        Status::InvalidInput
    );
    assert_eq!(command(0, json!({"op":"reset"})), Status::Closed);
    let handle = create(json!({}), no_hooks());
    assert_eq!(
        command(handle, json!({"op":"set_session_id","session_id":"x"})),
        Status::InvalidInput
    );
    assert_eq!(
        command(handle, json!({"op":"reset","unexpected":true})),
        Status::InvalidInput
    );
    let mut out = Buffer::empty();
    assert_eq!(
        unsafe {
            phu_capture(
                handle,
                bytes(r#"{"event":"test","distinct_id":"host"}"#),
                &mut out,
            )
        },
        Status::InvalidInput
    );
    assert!(out.data.is_null());
    assert_eq!(close(handle).0, Status::Ok);
}
#[test]
fn buffers_and_json_are_bounded_and_panics_are_contained() {
    let oversized = vec![b' '; abi::MAX_INPUT + 1];
    assert!(matches!(
        unsafe { abi::parse::<Value>(Bytes::from_slice(&oversized)) },
        Err(Status::InvalidInput)
    ));
    assert!(matches!(
        abi::encode(&json!("too big"), 2),
        Err(Status::OutputTooLarge)
    ));
    assert_eq!(
        abi::contain(|| panic!("contained ABI panic")),
        Status::InternalError
    );
    let mut out = Buffer::empty();
    assert_eq!(
        unsafe { phu_buffer_copy(bytes("owned bytes"), &mut out) },
        Status::Ok
    );
    assert_eq!(
        unsafe { std::slice::from_raw_parts(out.data, out.len) },
        b"owned bytes"
    );
    unsafe {
        phu_buffer_free(out);
    }
    let mut out = Buffer::empty();
    assert_eq!(
        unsafe {
            phu_buffer_copy(
                Bytes {
                    data: std::ptr::null(),
                    len: 1,
                },
                &mut out,
            )
        },
        Status::InvalidInput
    );
    assert!(out.data.is_null());
    unsafe {
        phu_buffer_free(out);
    }
}
#[test]
fn local_operations_and_real_outcomes() {
    let handle = create(json!({"session_mode":"manual","opt_out":true}), no_hooks());
    let original = read(handle, json!({"op":"state"}));
    assert_eq!(original["opt_out"], true);
    assert_eq!(capture(handle), Status::OptedOut);
    assert_eq!(command(handle, json!({"op":"opt_in"})), Status::Ok);
    assert_eq!(capture(handle), Status::Accepted);
    assert_eq!(read(handle, json!({"op":"session_id"})), Value::Null);
    assert_eq!(command(handle, json!({"op":"start_session"})), Status::Ok);
    assert!(read(handle, json!({"op":"session_id"})).is_string());
    assert_eq!(
        command(
            handle,
            json!({"op":"identify","distinct_id":"user","set":{"plan":"paid"}})
        ),
        Status::Ok
    );
    assert_eq!(read(handle, json!({"op":"state"}))["distinct_id"], "user");
    assert_eq!(command(handle, json!({"op":"reset"})), Status::Ok);
    let reset = read(handle, json!({"op":"state"}));
    assert_ne!(reset["anonymous_id"], original["anonymous_id"]);
    assert_eq!(reset["opt_out"], true);
    let (status, report) = close(handle);
    assert_eq!(status, Status::Ok);
    assert_eq!(
        report.unwrap(),
        json!({"state":"memory_only","delivery":"stopped"})
    );
    assert_eq!(capture(handle), Status::Closed);
    let disabled = create(json!({"disabled":true}), no_hooks());
    assert_eq!(capture(disabled), Status::Disabled);
    assert_eq!(
        close(disabled).1.unwrap(),
        json!({"state":"disabled","delivery":"disabled"})
    );
}

struct HookState {
    handle: AtomicU64,
    mode: AtomicI32,
    nested: AtomicI32,
}
unsafe extern "C" fn hook(data: *mut c_void, input: Bytes, out: *mut Buffer) -> i32 {
    let state = unsafe { &*(data as *const HookState) };
    let handle = state.handle.load(Ordering::SeqCst);
    match state.mode.load(Ordering::SeqCst) {
        1 => return Status::Dropped as i32,
        2 => return unsafe { phu_buffer_copy(bytes("{}"), out) } as i32,
        3 => {
            state.nested.store(capture(handle) as i32, Ordering::SeqCst);
        }
        4 => {
            state.nested.store(close(handle).0 as i32, Ordering::SeqCst);
        }
        5 => {
            let mut report = Buffer::empty();
            assert_eq!(unsafe { phu_flush(handle, &mut report) }, Status::Ok);
            let value: Value = unsafe {
                serde_json::from_slice(std::slice::from_raw_parts(report.data, report.len)).unwrap()
            };
            assert_eq!(value, json!({"state":"deferred","delivery":"deferred"}));
            unsafe {
                phu_buffer_free(report);
            }
        }
        _ => {}
    }
    let mut value: Value =
        unsafe { serde_json::from_slice(input.slice(abi::MAX_HOOK).unwrap()).unwrap() };
    assert_eq!(value["options"]["process_person_profile"], true);
    value["properties"]["redacted"] = json!(true);
    (unsafe { phu_buffer_copy(bytes(&value.to_string()), out) }) as i32
}
#[test]
fn hooks_drop_fail_closed_reenter_and_close_without_self_wait() {
    let state = Box::new(HookState {
        handle: AtomicU64::new(0),
        mode: AtomicI32::new(0),
        nested: AtomicI32::new(0),
    });
    let handle = create(
        json!({}),
        Hooks {
            before_send: Some(hook),
            user_data: (&*state as *const HookState).cast_mut().cast(),
        },
    );
    state.handle.store(handle, Ordering::SeqCst);
    assert_eq!(capture(handle), Status::Accepted);
    state.mode.store(1, Ordering::SeqCst);
    assert_eq!(capture(handle), Status::Dropped);
    state.mode.store(2, Ordering::SeqCst);
    assert_eq!(capture(handle), Status::Dropped);
    state.mode.store(3, Ordering::SeqCst);
    assert_eq!(capture(handle), Status::Accepted);
    assert_eq!(
        state.nested.load(Ordering::SeqCst),
        Status::Reentrant as i32
    );
    state.mode.store(5, Ordering::SeqCst);
    assert_eq!(capture(handle), Status::Accepted);
    state.mode.store(4, Ordering::SeqCst);
    assert_eq!(capture(handle), Status::Closed);
    assert_eq!(state.nested.load(Ordering::SeqCst), Status::Deferred as i32);
    assert_eq!(close(handle).0, Status::Ok);
}
#[test]
fn concurrent_close_joins_and_begin_is_idempotent() {
    let handle = create(json!({}), no_hooks());
    assert_eq!(phu_begin_shutdown(handle), Status::Ok);
    assert_eq!(phu_begin_shutdown(handle), Status::Ok);
    assert_eq!(capture(handle), Status::Closed);
    let threads: Vec<_> = (0..8)
        .map(|_| std::thread::spawn(move || close(handle).0))
        .collect();
    let results: Vec<_> = threads.into_iter().map(|t| t.join().unwrap()).collect();
    assert!(results.contains(&Status::Ok));
    assert!(results
        .iter()
        .all(|s| matches!(s, Status::Ok | Status::Closed)));
}
#[test]
fn flags_mailbox_owns_notifications_and_unsubscribes() {
    let mut config = stateful::Config::new("phc_test");
    config.host = "http://127.0.0.1:9".into();
    config.persistence = stateful::Persistence::Memory;
    config.preload_feature_flags = false;
    config.shutdown_timeout = Duration::ZERO;
    config.bootstrap_feature_flags = Some(HashMap::from([
        ("on".into(), posthog_rs::FlagValue::Boolean(true)),
        (
            "without_payload".into(),
            posthog_rs::FlagValue::Boolean(true),
        ),
        (
            "string_payload".into(),
            posthog_rs::FlagValue::Boolean(true),
        ),
    ]));
    config.bootstrap_feature_flag_payloads = Some(HashMap::from([
        ("on".into(), Value::Null),
        ("string_payload".into(), json!("00123")),
    ]));
    let client = stateful::Client::init_blocking(config);
    let mailbox = Arc::new(flags::Mailbox::default());
    let entry = handles::Entry::new(
        client,
        mailbox.clone(),
        Arc::new(hooks::CallbackScope),
        Duration::ZERO,
    );
    let bytes = mailbox.take().unwrap().unwrap();
    let update: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(update["flags"]["on"]["enabled"], true);
    assert_eq!(update["flags"]["on"]["has_payload"], true);
    assert_eq!(update["flags"]["on"]["payload"], Value::Null);
    assert_eq!(update["flags"]["without_payload"]["has_payload"], false);
    assert_eq!(update["flags"]["string_payload"]["payload"], "00123");
    assert!(mailbox.take().is_none());
    entry.begin().unwrap();
    entry.finish().unwrap();
    assert!(entry.subscription.lock().unwrap().is_none());
    assert!(mailbox.take().is_none());
    assert_eq!(update["flags"]["on"]["enabled"], true);
}
