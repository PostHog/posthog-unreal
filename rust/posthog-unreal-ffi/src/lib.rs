//! C ABI for the PostHog Unreal native integration.
//! The upstream workers own delivery. This bridge does not add a second queue.

use std::collections::HashMap;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Condvar, Mutex, OnceLock};

use chrono::{DateTime, Utc};
use posthog_rs::embedding::{EmbeddingOptions, EnqueueResult, FileEventStore, ReplayOptions};
use posthog_rs::{Client, ClientOptionsBuilder, EvaluateFlagsOptions, Event};
use serde::Deserialize;
#[cfg(test)]
use serde_json::json;
use serde_json::{Map, Value};
use uuid::Uuid;

const ABI_VERSION: u32 = 2;
const MAX_INPUT: usize = 1024 * 1024;
const MAX_REPLAY_INPUT: usize = 8 * 1024 * 1024;
const MAX_OUTPUT: usize = 4 * 1024 * 1024;

#[repr(i32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Status {
    Ok = 0,
    /// Accepted into the bounded memory queue. Checkpoint reports disk errors.
    Accepted = 1,
    InvalidInput = 2,
    Closed = 3,
    RequestFailed = 4,
    InternalError = 5,
    UnsupportedAbi = 6,
    Disabled = 7,
    QueueFull = 8,
    StorageError = 9,
}

type Result<T> = std::result::Result<T, Status>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Bytes {
    pub data: *const u8,
    pub len: usize,
}

#[repr(C)]
pub struct Buffer {
    pub data: *mut u8,
    pub len: usize,
}

impl Buffer {
    fn empty() -> Self {
        Self {
            data: std::ptr::null_mut(),
            len: 0,
        }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Options {
    project_token: String,
    host: String,
    storage_path: String,
    #[serde(default)]
    opted_out: bool,
    #[serde(default = "default_request_timeout")]
    request_timeout_seconds: u64,
    #[serde(default = "default_shutdown_timeout")]
    shutdown_timeout_ms: u64,
    #[serde(default = "default_flush_at")]
    flush_at: usize,
    #[serde(default = "default_flush_interval")]
    flush_interval_ms: u64,
    #[serde(default = "default_queue_size")]
    max_queue_size: usize,
    #[serde(default = "default_batch_size")]
    max_batch_size: usize,
    #[serde(default)]
    replay: Option<ReplayOptions>,
}

fn default_request_timeout() -> u64 {
    10
}
fn default_shutdown_timeout() -> u64 {
    3000
}
fn default_flush_at() -> usize {
    20
}
fn default_flush_interval() -> u64 {
    30_000
}
fn default_queue_size() -> usize {
    1000
}
fn default_batch_size() -> usize {
    50
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Capture {
    event: String,
    distinct_id: String,
    uuid: Uuid,
    timestamp: DateTime<Utc>,
    #[serde(default)]
    properties: Map<String, Value>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct FlagContext {
    distinct_id: String,
    groups: Option<HashMap<String, String>>,
    person_properties: Option<HashMap<String, Value>>,
    group_properties: Option<HashMap<String, HashMap<String, Value>>>,
}

#[derive(Default)]
struct Calls {
    active: usize,
    closed: bool,
    opted_out: bool,
    generation: u64,
}

struct Entry {
    client: Client,
    calls: Mutex<Calls>,
    idle: Condvar,
}

#[derive(Default)]
struct Registry {
    next: u64,
    clients: HashMap<u64, Arc<Entry>>,
}

fn registry() -> &'static Mutex<Registry> {
    static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(Registry::default()))
}

struct Call(Arc<Entry>);

impl Drop for Call {
    fn drop(&mut self) {
        // Drop must not panic while an operation is already unwinding.
        let mut calls = self.0.calls.lock().unwrap_or_else(|e| e.into_inner());
        calls.active -= 1;
        self.0.idle.notify_all();
    }
}

fn acquire(handle: u64) -> Result<Call> {
    let entry = registry()
        .lock()
        .map_err(|_| Status::InternalError)?
        .clients
        .get(&handle)
        .cloned()
        .ok_or(Status::Closed)?;
    let mut calls = entry.calls.lock().map_err(|_| Status::InternalError)?;
    if calls.closed {
        return Err(Status::Closed);
    }
    calls.active += 1;
    drop(calls);
    Ok(Call(entry))
}

fn contain(f: impl FnOnce() -> Result<Status>) -> Status {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(status)) => status,
        Ok(Err(status)) => status,
        Err(_) => Status::InternalError,
    }
}

// SAFETY: the caller supplies readable bytes for the duration of the call.
unsafe fn parse<T: serde::de::DeserializeOwned>(bytes: Bytes) -> Result<T> {
    unsafe { parse_limited(bytes, MAX_INPUT) }
}

// SAFETY: as for `parse`.
unsafe fn parse_limited<T: serde::de::DeserializeOwned>(bytes: Bytes, limit: usize) -> Result<T> {
    if bytes.data.is_null() || bytes.len == 0 || bytes.len > limit {
        return Err(Status::InvalidInput);
    }
    let input = unsafe { std::slice::from_raw_parts(bytes.data, bytes.len) };
    serde_json::from_slice(input).map_err(|_| Status::InvalidInput)
}

fn buffer(value: Value) -> Result<Buffer> {
    let bytes = serde_json::to_vec(&value).map_err(|_| Status::InternalError)?;
    if bytes.len() > MAX_OUTPUT {
        return Err(Status::RequestFailed);
    }
    let boxed = bytes.into_boxed_slice();
    let len = boxed.len();
    let data = Box::into_raw(boxed) as *mut u8;
    Ok(Buffer { data, len })
}

#[no_mangle]
pub extern "C" fn phu_abi_version() -> u32 {
    ABI_VERSION
}

/// # Safety
/// `options` must reference readable memory; `out_handle` must be writable.
#[no_mangle]
pub unsafe extern "C" fn phu_create(version: u32, options: Bytes, out_handle: *mut u64) -> Status {
    contain(|| {
        if out_handle.is_null() {
            return Err(Status::InvalidInput);
        }
        unsafe {
            *out_handle = 0;
        }
        if version != ABI_VERSION {
            return Err(Status::UnsupportedAbi);
        }
        let options: Options = unsafe { parse(options)? };
        if options.project_token.trim().is_empty()
            || options.host.trim().is_empty()
            || options.request_timeout_seconds == 0
            || options.request_timeout_seconds > 60
            || options.shutdown_timeout_ms == 0
            || options.shutdown_timeout_ms > 60_000
            || options.flush_at == 0
            || options.storage_path.trim().is_empty()
            || options.max_batch_size == 0
            || options.max_queue_size == 0
            || options.max_queue_size > 100_000
            || options.max_batch_size > options.max_queue_size
            || options.flush_at > options.max_queue_size
            || options.flush_interval_ms == 0
            || options.flush_interval_ms > 86_400_000
        {
            return Err(Status::InvalidInput);
        }
        if options
            .replay
            .is_some_and(|replay| replay.validate().is_err())
        {
            return Err(Status::InvalidInput);
        }
        let opted_out = options.opted_out;
        let store = Arc::new(
            FileEventStore::open(&options.storage_path).map_err(|_| Status::StorageError)?,
        );
        let options = ClientOptionsBuilder::default()
            .embedding(EmbeddingOptions {
                store,
                sdk_name: "posthog-unreal".into(),
                sdk_version: env!("CARGO_PKG_VERSION").into(),
                enabled: !opted_out,
                replay: options.replay,
            })
            .api_key(options.project_token)
            .host(options.host)
            .is_server(false)
            .request_timeout_seconds(options.request_timeout_seconds)
            .feature_flags_request_timeout_seconds(options.request_timeout_seconds)
            .feature_flags_request_max_retries(0u32)
            .shutdown_timeout_ms(options.shutdown_timeout_ms)
            .flush_at(options.flush_at)
            .flush_interval_ms(options.flush_interval_ms)
            .max_queue_size(options.max_queue_size)
            .max_batch_size(options.max_batch_size)
            .build()
            .map_err(|_| Status::InvalidInput)?;
        let entry = Arc::new(Entry {
            client: posthog_rs::client(options),
            calls: Mutex::new(Calls {
                opted_out,
                ..Calls::default()
            }),
            idle: Condvar::new(),
        });
        let mut registry = registry().lock().map_err(|_| Status::InternalError)?;
        let handle = registry.next.checked_add(1).ok_or(Status::InternalError)?;
        registry.next = handle;
        registry.clients.insert(handle, entry);
        unsafe {
            *out_handle = handle;
        }
        Ok(Status::Ok)
    })
}

/// # Safety
/// `event` must reference readable memory for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn phu_capture(handle: u64, event: Bytes) -> Status {
    contain(|| {
        let call = admit_capture(handle)?;
        let input: Capture = unsafe { parse(event)? };
        if input.event.trim().is_empty() || input.distinct_id.trim().is_empty() {
            return Err(Status::InvalidInput);
        }
        Ok(enqueue_status(
            call.0.client.capture_prepared(input.into_event()),
        ))
    })
}

/// Acquire a call that may enqueue: open and not opted out.
fn admit_capture(handle: u64) -> Result<Call> {
    let call = acquire(handle)?;
    {
        let calls = call.0.calls.lock().map_err(|_| Status::InternalError)?;
        if calls.closed {
            return Err(Status::Closed);
        }
        if calls.opted_out {
            return Err(Status::Disabled);
        }
    }
    Ok(call)
}

impl Capture {
    fn into_event(self) -> Event {
        Event::from_prepared(
            self.event,
            self.distinct_id,
            self.properties.into_iter().collect(),
            self.timestamp,
            self.uuid,
        )
    }
}

fn enqueue_status(result: EnqueueResult) -> Status {
    match result {
        EnqueueResult::Accepted => Status::Accepted,
        EnqueueResult::Disabled => Status::Disabled,
        EnqueueResult::Closed => Status::Closed,
        EnqueueResult::QueueFull => Status::QueueFull,
    }
}

fn is_snapshot(input: &Capture) -> bool {
    input.event == "$snapshot"
        && !input.distinct_id.trim().is_empty()
        && input
            .properties
            .get("$session_id")
            .and_then(Value::as_str)
            .is_some_and(|id| !id.is_empty())
        && input
            .properties
            .get("$snapshot_data")
            .and_then(Value::as_array)
            .is_some_and(|data| !data.is_empty())
}

/// Admits one prepared `$snapshot` event into the in-memory replay queue.
/// # Safety
/// `event` must reference readable memory for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn phu_capture_replay(handle: u64, event: Bytes) -> Status {
    contain(|| {
        let call = admit_capture(handle)?;
        let input: Capture = unsafe { parse_limited(event, MAX_REPLAY_INPUT)? };
        if !is_snapshot(&input) {
            return Err(Status::InvalidInput);
        }
        Ok(enqueue_status(
            call.0.client.capture_replay_snapshot(input.into_event()),
        ))
    })
}

/// Blocks until the upstream worker finishes one flush attempt. Not a receipt.
#[no_mangle]
pub extern "C" fn phu_flush(handle: u64) -> Status {
    contain(|| {
        let call = acquire(handle)?;
        call.0.client.flush();
        Ok(Status::Ok)
    })
}

/// Fetches values, payloads, and evaluation metadata without recording exposure.
/// # Safety
/// `context` must reference readable memory; `out_buffer` must be writable.
#[no_mangle]
pub unsafe extern "C" fn phu_flags(handle: u64, context: Bytes, out_buffer: *mut Buffer) -> Status {
    contain(|| {
        if out_buffer.is_null() {
            return Err(Status::InvalidInput);
        }
        unsafe {
            *out_buffer = Buffer::empty();
        }
        let call = acquire(handle)?;
        let context: FlagContext = unsafe { parse(context)? };
        if context.distinct_id.trim().is_empty() {
            return Err(Status::InvalidInput);
        }
        let generation = {
            let calls = call.0.calls.lock().map_err(|_| Status::InternalError)?;
            if calls.opted_out {
                return Err(Status::Disabled);
            }
            calls.generation
        };
        let mut options = EvaluateFlagsOptions::default();
        options.groups = context.groups;
        options.person_properties = context.person_properties;
        options.group_properties = context.group_properties;
        let flags = call
            .0
            .client
            .evaluate_flags(context.distinct_id, options)
            .map_err(|_| Status::RequestFailed)?;
        let calls = call.0.calls.lock().map_err(|_| Status::InternalError)?;
        if calls.closed {
            return Err(Status::Closed);
        }
        if calls.opted_out || calls.generation != generation {
            return Err(Status::Disabled);
        }
        let result =
            buffer(serde_json::to_value(flags.export()).map_err(|_| Status::InternalError)?)?;
        unsafe {
            *out_buffer = result;
        }
        Ok(Status::Ok)
    })
}

/// Immediately stops event/flag admission and queues a durable purge. Persist
/// consent in the host state store before calling. Checkpoint waits for purge.
#[no_mangle]
pub extern "C" fn phu_opt_out(handle: u64) -> Status {
    contain(|| {
        let call = acquire(handle)?;
        let mut calls = call.0.calls.lock().map_err(|_| Status::InternalError)?;
        if calls.closed {
            return Err(Status::Closed);
        }
        calls.opted_out = true;
        calls.generation = calls.generation.wrapping_add(1);
        call.0.client.pause_and_purge();
        Ok(Status::Ok)
    })
}

/// Waits for purge, then resumes admission. Never call on the game thread.
#[no_mangle]
pub extern "C" fn phu_opt_in(handle: u64) -> Status {
    contain(|| {
        let call = acquire(handle)?;
        let (generation, delivery_generation) = {
            let calls = call.0.calls.lock().map_err(|_| Status::InternalError)?;
            if calls.closed {
                return Err(Status::Closed);
            }
            (calls.generation, call.0.client.delivery_generation())
        };
        let resumed = call.0.client.resume_delivery(delivery_generation);
        let mut calls = call.0.calls.lock().map_err(|_| Status::InternalError)?;
        if calls.closed {
            return Err(Status::Closed);
        }
        if calls.generation != generation {
            return Err(Status::Disabled);
        }
        if !resumed {
            return Err(Status::StorageError);
        }
        calls.opted_out = false;
        Ok(Status::Ok)
    })
}

/// Waits for earlier worker commands and returns delivery/storage counters.
/// # Safety
/// `out_buffer` must be writable. Free the returned buffer with phu_buffer_free.
#[no_mangle]
pub unsafe extern "C" fn phu_checkpoint(handle: u64, out_buffer: *mut Buffer) -> Status {
    contain(|| {
        if out_buffer.is_null() {
            return Err(Status::InvalidInput);
        }
        unsafe {
            *out_buffer = Buffer::empty();
        }
        let call = acquire(handle)?;
        let report = call.0.client.checkpoint_delivery();
        let result = buffer(serde_json::to_value(report).map_err(|_| Status::InternalError)?)?;
        unsafe {
            *out_buffer = result;
        }
        Ok(Status::Ok)
    })
}

/// Stops admission and starts the shutdown budget without waiting. The handle
/// retains its storage lock until phu_close joins calls and releases it.
#[no_mangle]
pub extern "C" fn phu_begin_shutdown(handle: u64) -> Status {
    contain(|| {
        let call = acquire(handle)?;
        let mut calls = call.0.calls.lock().map_err(|_| Status::InternalError)?;
        if calls.closed {
            return Err(Status::Closed);
        }
        calls.closed = true;
        call.0.client.begin_shutdown();
        Ok(Status::Ok)
    })
}

/// Stops admission, waits for admitted calls, drains and joins upstream workers.
/// Requests and starting new worker disk operations respect the shutdown budget.
/// An already-running filesystem operation and OS scheduling are not preempted.
#[no_mangle]
pub extern "C" fn phu_close(handle: u64) -> Status {
    contain(|| {
        let entry = registry()
            .lock()
            .map_err(|_| Status::InternalError)?
            .clients
            .remove(&handle)
            .ok_or(Status::Closed)?;
        let mut calls = entry.calls.lock().map_err(|_| Status::InternalError)?;
        calls.closed = true;
        entry.client.begin_shutdown();
        while calls.active != 0 {
            calls = entry.idle.wait(calls).map_err(|_| Status::InternalError)?;
        }
        drop(calls);
        entry.client.shutdown();
        Ok(Status::Ok)
    })
}

/// # Safety
/// Pass a buffer returned by this ABI, unchanged, exactly once. A zeroed buffer
/// is also valid. Do not pass pointers allocated by Unreal or a different CRT.
#[no_mangle]
pub unsafe extern "C" fn phu_buffer_free(value: Buffer) {
    if !value.data.is_null() {
        let slice = std::ptr::slice_from_raw_parts_mut(value.data, value.len);
        unsafe {
            drop(Box::from_raw(slice));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bytes(s: &str) -> Bytes {
        Bytes {
            data: s.as_ptr(),
            len: s.len(),
        }
    }

    #[test]
    fn input_validation() {
        assert_eq!(
            unsafe {
                parse::<Value>(Bytes {
                    data: std::ptr::null(),
                    len: 2,
                })
            }
            .unwrap_err(),
            Status::InvalidInput
        );
        assert_eq!(
            unsafe {
                parse::<Value>(Bytes {
                    data: 1usize as *const u8,
                    len: MAX_INPUT + 1,
                })
            }
            .unwrap_err(),
            Status::InvalidInput
        );
        assert!(unsafe { parse::<Value>(bytes("not JSON")) }.is_err());
        assert!(unsafe { parse::<Capture>(bytes("{}")) }.is_err());
    }

    #[test]
    fn abi_mismatch_zeroes_handle() {
        let mut handle = 123;
        assert_eq!(
            unsafe { phu_create(99, bytes("{}"), &mut handle) },
            Status::UnsupportedAbi
        );
        assert_eq!(handle, 0);
    }

    #[test]
    fn closed_handles_are_harmless() {
        assert_eq!(phu_flush(0), Status::Closed);
        assert_eq!(phu_close(u64::MAX), Status::Closed);
    }

    #[test]
    fn panics_are_contained() {
        assert_eq!(contain(|| panic!("test panic")), Status::InternalError);
    }

    #[test]
    fn owned_buffer_round_trip() {
        let output = buffer(json!({"text": "玩家 🚀"})).unwrap();
        let value: Value = unsafe {
            parse(Bytes {
                data: output.data,
                len: output.len,
            })
        }
        .unwrap();
        assert_eq!(value["text"], "玩家 🚀");
        unsafe {
            phu_buffer_free(output);
        }
    }

    fn storage_path() -> std::path::PathBuf {
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let next = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        std::env::temp_dir().join(format!("phu-test-{}-{next}", std::process::id()))
    }

    fn create(replay: Option<Value>) -> (u64, std::path::PathBuf) {
        let storage = storage_path();
        let mut options = json!({
            "project_token": "phc_test",
            "host": "http://127.0.0.1:1",
            "storage_path": storage,
            "shutdown_timeout_ms": 100,
        });
        if let Some(replay) = replay {
            options["replay"] = replay;
        }
        let options = options.to_string();
        let mut handle = 0;
        assert_eq!(
            unsafe { phu_create(ABI_VERSION, bytes(&options), &mut handle) },
            Status::Ok
        );
        (handle, storage)
    }

    fn close(handle: u64, storage: std::path::PathBuf) {
        assert_eq!(phu_close(handle), Status::Ok);
        let _ = std::fs::remove_dir_all(storage);
    }

    fn snapshot(properties: Value) -> String {
        json!({
            "event": "$snapshot",
            "distinct_id": "user",
            "uuid": Uuid::nil(),
            "timestamp": "2026-10-15T12:00:00Z",
            "properties": properties,
        })
        .to_string()
    }

    fn frame(padding: usize) -> String {
        snapshot(json!({
            "$session_id": "session",
            "$window_id": "session",
            "$snapshot_source": "mobile",
            "$snapshot_data": [{"type": 4, "timestamp": 1, "data": {"href": "x".repeat(padding)}}],
        }))
    }

    #[test]
    fn replay_input_size_limit_is_separate_from_events() {
        let oversized = Bytes {
            data: std::ptr::NonNull::<u8>::dangling().as_ptr(),
            len: MAX_REPLAY_INPUT + 1,
        };
        assert_eq!(
            unsafe { parse_limited::<Value>(oversized, MAX_REPLAY_INPUT) }.unwrap_err(),
            Status::InvalidInput
        );
        let (handle, storage) = create(Some(json!({})));
        assert_eq!(
            unsafe { phu_capture_replay(handle, oversized) },
            Status::InvalidInput
        );
        let large = frame(2 * MAX_INPUT);
        assert!(large.len() > MAX_INPUT && large.len() <= MAX_REPLAY_INPUT);
        assert_eq!(
            unsafe { phu_capture_replay(handle, bytes(&large)) },
            Status::Accepted
        );
        assert_eq!(
            unsafe { phu_capture(handle, bytes(&large)) },
            Status::InvalidInput
        );
        close(handle, storage);
    }

    #[test]
    fn replay_snapshots_are_validated() {
        let (handle, storage) = create(Some(json!({"flush_interval_ms": 60000})));
        let capture = |input: &str| unsafe { phu_capture_replay(handle, bytes(input)) };
        assert_eq!(capture(&frame(0)), Status::Accepted);
        let data = json!([{"type": 4, "timestamp": 1, "data": {}}]);
        for invalid in [
            snapshot(json!({"$snapshot_data": data})),
            snapshot(json!({"$session_id": "", "$snapshot_data": data})),
            snapshot(json!({"$session_id": 1, "$snapshot_data": data})),
            snapshot(json!({"$session_id": "session", "$snapshot_data": []})),
            snapshot(json!({"$session_id": "session"})),
            frame(0).replace(r#""event":"$snapshot""#, r#""event":"$pageview""#),
            frame(0).replace(r#""distinct_id":"user""#, r#""distinct_id":" ""#),
        ] {
            assert_eq!(capture(&invalid), Status::InvalidInput, "{invalid}");
        }
        assert_eq!(phu_opt_out(handle), Status::Ok);
        assert_eq!(capture(&frame(0)), Status::Disabled);
        assert_eq!(phu_begin_shutdown(handle), Status::Ok);
        assert_eq!(capture(&frame(0)), Status::Closed);
        close(handle, storage);
    }

    #[test]
    fn replay_is_disabled_unless_configured() {
        let (handle, storage) = create(None);
        assert_eq!(
            unsafe { phu_capture_replay(handle, bytes(&frame(0))) },
            Status::Disabled
        );
        close(handle, storage);
    }

    #[test]
    fn replay_options_are_bounded() {
        for replay in [
            json!({"unknown": 1}),
            json!({"max_queue_bytes": 1024}),
            json!({"max_queue_bytes": 1048576, "max_batch_bytes": 2097152}),
            json!({"max_batch_bytes": 1024}),
            json!({"flush_interval_ms": 99}),
            json!({"flush_interval_ms": 60001}),
        ] {
            let storage = storage_path();
            let options = json!({
                "project_token": "phc_test",
                "host": "http://127.0.0.1:1",
                "storage_path": storage,
                "replay": replay,
            })
            .to_string();
            let mut handle = 0;
            assert_eq!(
                unsafe { phu_create(ABI_VERSION, bytes(&options), &mut handle) },
                Status::InvalidInput,
                "{replay}"
            );
            assert!(!storage.exists());
        }
    }
}
