//! C bindings for the stateful PostHog Rust SDK.
//!
//! Follows the same C ABI conventions as `posthog-rs-ffi` (opaque handles,
//! `PostHogStatus` + `posthog_last_error`, `posthog_string_free`, `cbindgen`
//! compatibility) so this crate can be migrated directly into `posthog-rs`.

#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::{Arc, RwLock};
use std::time::Duration;

use chrono::{DateTime, FixedOffset, NaiveDateTime, Utc};
use posthog::stateful::{
    self, CaptureOutcome, DeliveryOutcome, DrainReport, FeatureFlagContext, FeatureFlagLoadError,
    FeatureFlagResult, FeatureFlagsUpdate, FrameLayout, FrameUnavailable, PersistenceOutcome,
    PixelFormat, ReplayFrame, SubmitOutcome,
};
use posthog::{Event, PostHogError};
use serde::ser::SerializeMap;
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use serde_json::{json, Value};
use uuid::Uuid;

// ---------------------------------------------------------------------------
// Public C enums & structs
// ---------------------------------------------------------------------------

/// Result of an FFI call (matches `posthog-rs-ffi`).
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogStatus {
    /// The call succeeded.
    POSTHOG_STATUS_OK = 0,
    /// A required pointer argument was `NULL`.
    POSTHOG_STATUS_NULL_ARGUMENT = 1,
    /// A string argument was not valid UTF-8.
    POSTHOG_STATUS_INVALID_UTF8 = 2,
    /// A JSON argument could not be parsed or had the wrong shape.
    POSTHOG_STATUS_INVALID_JSON = 3,
    /// An argument was rejected.
    POSTHOG_STATUS_INVALID_ARGUMENT = 4,
    /// A network request failed or PostHog returned an error.
    POSTHOG_STATUS_REQUEST = 5,
    /// The SDK panicked and the panic was caught.
    POSTHOG_STATUS_PANIC = 6,
}

/// Default person profile creation policy.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogPersonProfiles {
    POSTHOG_PERSON_PROFILES_NEVER = 0,
    POSTHOG_PERSON_PROFILES_IDENTIFIED_ONLY = 1,
    POSTHOG_PERSON_PROFILES_ALWAYS = 2,
}

/// Session lifecycle ownership mode.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogSessionMode {
    POSTHOG_SESSION_MODE_AUTOMATIC = 0,
    POSTHOG_SESSION_MODE_MANUAL = 1,
}

/// Whether a single feature-flag lookup emits `$feature_flag_called`.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogFlagReporting {
    POSTHOG_FLAG_REPORTING_DEFAULT = 0,
    POSTHOG_FLAG_REPORTING_SEND = 1,
    POSTHOG_FLAG_REPORTING_SILENT = 2,
}

/// Local admission outcome of `posthog_stateful_capture` / `posthog_stateful_screen`.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogCaptureOutcome {
    POSTHOG_CAPTURE_ACCEPTED = 0,
    POSTHOG_CAPTURE_DISABLED = 1,
    POSTHOG_CAPTURE_OPTED_OUT = 2,
    POSTHOG_CAPTURE_CLOSED = 3,
    POSTHOG_CAPTURE_INVALID = 4,
    POSTHOG_CAPTURE_DROPPED = 5,
    POSTHOG_CAPTURE_REENTRANT = 6,
    POSTHOG_CAPTURE_UNKNOWN = 7,
}

/// State persistence progress for a flush or shutdown operation.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogPersistenceOutcome {
    POSTHOG_PERSISTENCE_PERSISTED = 0,
    POSTHOG_PERSISTENCE_MEMORY_ONLY = 1,
    POSTHOG_PERSISTENCE_UNAVAILABLE = 2,
    POSTHOG_PERSISTENCE_WRITE_FAILED = 3,
    POSTHOG_PERSISTENCE_TIMED_OUT = 4,
    POSTHOG_PERSISTENCE_DEFERRED = 5,
    POSTHOG_PERSISTENCE_DISABLED = 6,
    POSTHOG_PERSISTENCE_NOT_REQUESTED = 7,
    POSTHOG_PERSISTENCE_UNKNOWN = 8,
}

/// Delivery worker progress for a flush or shutdown operation.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogDeliveryOutcome {
    POSTHOG_DELIVERY_ATTEMPTED = 0,
    POSTHOG_DELIVERY_STOPPED = 1,
    POSTHOG_DELIVERY_TIMED_OUT = 2,
    POSTHOG_DELIVERY_UNAVAILABLE = 3,
    POSTHOG_DELIVERY_CLOSED = 4,
    POSTHOG_DELIVERY_DEFERRED = 5,
    POSTHOG_DELIVERY_DISABLED = 6,
    POSTHOG_DELIVERY_UNKNOWN = 7,
}

/// Combined persistence and delivery report from flush or shutdown.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PostHogDrainReport {
    pub state: PostHogPersistenceOutcome,
    pub delivery: PostHogDeliveryOutcome,
}

/// Reason a replay frame could not be acquired.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogFrameUnavailable {
    POSTHOG_FRAME_OK = 0,
    POSTHOG_FRAME_NOT_RECORDING = 1,
    POSTHOG_FRAME_NOT_PERMITTED = 2,
    POSTHOG_FRAME_OPTED_OUT = 3,
    POSTHOG_FRAME_NO_SESSION = 4,
    POSTHOG_FRAME_THROTTLED = 5,
    POSTHOG_FRAME_AT_CAPACITY = 6,
    POSTHOG_FRAME_INVALID_LAYOUT = 7,
    POSTHOG_FRAME_OUT_OF_MEMORY = 8,
    POSTHOG_FRAME_REENTRANT = 9,
    POSTHOG_FRAME_CLOSED = 10,
    POSTHOG_FRAME_UNKNOWN = 11,
}

/// Local outcome of submitting a prepared replay frame.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostHogSubmitOutcome {
    POSTHOG_SUBMIT_ACCEPTED = 0,
    POSTHOG_SUBMIT_WRONG_RECORDER = 1,
    POSTHOG_SUBMIT_REENTRANT = 2,
    POSTHOG_SUBMIT_STALE = 3,
    POSTHOG_SUBMIT_CLOSED = 4,
    POSTHOG_SUBMIT_UNKNOWN = 5,
}

// ---------------------------------------------------------------------------
// Callback types
// ---------------------------------------------------------------------------

/// Synchronous event hook called before admission (on the capturing thread for
/// analytics, and on the encoder worker for replay screenshots).
///
/// Return `false` to drop the event. Return `true` and leave `*out_event_json`
/// `NULL` to keep the event unchanged, or set `*out_event_json` using
/// `posthog_string_copy` to replace the event. Rust always frees a non-`NULL`
/// `*out_event_json`.
pub type PostHogBeforeSendFn = unsafe extern "C" fn(
    user_data: *mut c_void,
    event_json: *const c_char,
    out_event_json: *mut *mut c_char,
) -> bool;

/// Called when feature flags are restored, bootstrapped, or loaded remotely.
/// May run on a background worker thread.
pub type PostHogFeatureFlagsFn =
    unsafe extern "C" fn(user_data: *mut c_void, update_json: *const c_char);

/// Called when the SDK reports an initialization, storage, flag, remote-config,
/// replay, or delivery error. May run concurrently on any SDK worker thread.
pub type PostHogErrorFn = unsafe extern "C" fn(user_data: *mut c_void, message: *const c_char);

// ---------------------------------------------------------------------------
// Opaque handles
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct CallbackBinding<F: Copy> {
    callback: F,
    user_data: usize,
}

type CallbackSlot<F> = Arc<RwLock<Option<CallbackBinding<F>>>>;

/// Configuration for a stateful client, created with `posthog_stateful_config_new`.
pub struct PostHogStatefulConfig {
    config: Option<stateful::Config>,
    before_send: Option<CallbackBinding<PostHogBeforeSendFn>>,
    on_feature_flags: Option<CallbackBinding<PostHogFeatureFlagsFn>>,
    on_error: Option<CallbackBinding<PostHogErrorFn>>,
}

/// A stateful PostHog client, created with `posthog_stateful_client_new`.
pub struct PostHogStatefulClient {
    client: stateful::Client,
    before_send: CallbackSlot<PostHogBeforeSendFn>,
    on_feature_flags: CallbackSlot<PostHogFeatureFlagsFn>,
    on_error: CallbackSlot<PostHogErrorFn>,
}

/// An acquired replay frame reservation, created with `posthog_stateful_replay_begin_frame`.
pub struct PostHogReplayFrame {
    frame: ReplayFrame,
}

// ---------------------------------------------------------------------------
// Error handling & argument helpers (matches posthog-rs-ffi)
// ---------------------------------------------------------------------------

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

struct FfiError {
    status: PostHogStatus,
    message: String,
}

impl FfiError {
    fn new(status: PostHogStatus, message: impl Into<String>) -> Self {
        Self {
            status,
            message: message.into(),
        }
    }

    fn null(name: &str) -> Self {
        Self::new(
            PostHogStatus::POSTHOG_STATUS_NULL_ARGUMENT,
            format!("`{name}` is NULL"),
        )
    }
}

fn set_last_error(message: Option<String>) {
    let message = message
        .map(|message| CString::new(message.replace('\0', " ")).expect("NUL bytes were replaced"));
    let _ = LAST_ERROR.try_with(|slot| *slot.borrow_mut() = message);
}

fn ffi_call(body: impl FnOnce() -> Result<(), FfiError>) -> PostHogStatus {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(())) => {
            set_last_error(None);
            PostHogStatus::POSTHOG_STATUS_OK
        }
        Ok(Err(error)) => {
            set_last_error(Some(error.message));
            error.status
        }
        Err(_) => {
            set_last_error(Some("PostHog SDK panicked".to_string()));
            PostHogStatus::POSTHOG_STATUS_PANIC
        }
    }
}

fn ffi_new<T>(body: impl FnOnce() -> Result<T, FfiError>) -> *mut T {
    let mut created = None;
    ffi_call(|| {
        created = Some(body()?);
        Ok(())
    });
    created.map_or(ptr::null_mut(), |value| Box::into_raw(Box::new(value)))
}

unsafe fn str_arg<'a>(value: *const c_char, name: &str) -> Result<&'a str, FfiError> {
    if value.is_null() {
        return Err(FfiError::null(name));
    }
    CStr::from_ptr(value).to_str().map_err(|_| {
        FfiError::new(
            PostHogStatus::POSTHOG_STATUS_INVALID_UTF8,
            format!("`{name}` is not valid UTF-8"),
        )
    })
}

unsafe fn opt_str_arg<'a>(value: *const c_char, name: &str) -> Result<Option<&'a str>, FfiError> {
    if value.is_null() {
        Ok(None)
    } else {
        str_arg(value, name).map(Some)
    }
}

unsafe fn ref_arg<'a, T>(value: *const T, name: &str) -> Result<&'a T, FfiError> {
    value.as_ref().ok_or_else(|| FfiError::null(name))
}

unsafe fn mut_arg<'a, T>(value: *mut T, name: &str) -> Result<&'a mut T, FfiError> {
    value.as_mut().ok_or_else(|| FfiError::null(name))
}

unsafe fn opt_json_object_arg(
    value: *const c_char,
    name: &str,
) -> Result<HashMap<String, Value>, FfiError> {
    let Some(raw) = opt_str_arg(value, name)? else {
        return Ok(HashMap::new());
    };
    let parsed: Value = serde_json::from_str(raw).map_err(|error| {
        FfiError::new(
            PostHogStatus::POSTHOG_STATUS_INVALID_JSON,
            format!("`{name}` is not valid JSON: {error}"),
        )
    })?;
    let Value::Object(map) = parsed else {
        return Err(FfiError::new(
            PostHogStatus::POSTHOG_STATUS_INVALID_JSON,
            format!("`{name}` must be a JSON object"),
        ));
    };
    Ok(map.into_iter().collect())
}

unsafe fn write_out<T>(out: *mut T, value: T) {
    if !out.is_null() {
        out.write(value);
    }
}

fn into_c_string(value: String) -> Result<*mut c_char, FfiError> {
    CString::new(value).map(CString::into_raw).map_err(|_| {
        FfiError::new(
            PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT,
            "returned string contains a NUL byte",
        )
    })
}

fn to_cstring_sanitized(value: &str) -> CString {
    CString::new(value.replace('\0', " ")).expect("NUL bytes were replaced")
}

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

impl From<CaptureOutcome> for PostHogCaptureOutcome {
    fn from(outcome: CaptureOutcome) -> Self {
        match outcome {
            CaptureOutcome::Accepted { .. } => Self::POSTHOG_CAPTURE_ACCEPTED,
            CaptureOutcome::Disabled => Self::POSTHOG_CAPTURE_DISABLED,
            CaptureOutcome::OptedOut => Self::POSTHOG_CAPTURE_OPTED_OUT,
            CaptureOutcome::Closed => Self::POSTHOG_CAPTURE_CLOSED,
            CaptureOutcome::Invalid => Self::POSTHOG_CAPTURE_INVALID,
            CaptureOutcome::Dropped => Self::POSTHOG_CAPTURE_DROPPED,
            CaptureOutcome::Reentrant => Self::POSTHOG_CAPTURE_REENTRANT,
            _ => Self::POSTHOG_CAPTURE_UNKNOWN,
        }
    }
}

impl From<PersistenceOutcome> for PostHogPersistenceOutcome {
    fn from(outcome: PersistenceOutcome) -> Self {
        match outcome {
            PersistenceOutcome::Persisted => Self::POSTHOG_PERSISTENCE_PERSISTED,
            PersistenceOutcome::MemoryOnly => Self::POSTHOG_PERSISTENCE_MEMORY_ONLY,
            PersistenceOutcome::Unavailable => Self::POSTHOG_PERSISTENCE_UNAVAILABLE,
            PersistenceOutcome::WriteFailed => Self::POSTHOG_PERSISTENCE_WRITE_FAILED,
            PersistenceOutcome::TimedOut => Self::POSTHOG_PERSISTENCE_TIMED_OUT,
            PersistenceOutcome::Deferred => Self::POSTHOG_PERSISTENCE_DEFERRED,
            PersistenceOutcome::Disabled => Self::POSTHOG_PERSISTENCE_DISABLED,
            PersistenceOutcome::NotRequested => Self::POSTHOG_PERSISTENCE_NOT_REQUESTED,
            _ => Self::POSTHOG_PERSISTENCE_UNKNOWN,
        }
    }
}

impl From<DeliveryOutcome> for PostHogDeliveryOutcome {
    fn from(outcome: DeliveryOutcome) -> Self {
        match outcome {
            DeliveryOutcome::Attempted => Self::POSTHOG_DELIVERY_ATTEMPTED,
            DeliveryOutcome::Stopped => Self::POSTHOG_DELIVERY_STOPPED,
            DeliveryOutcome::TimedOut => Self::POSTHOG_DELIVERY_TIMED_OUT,
            DeliveryOutcome::Unavailable => Self::POSTHOG_DELIVERY_UNAVAILABLE,
            DeliveryOutcome::Closed => Self::POSTHOG_DELIVERY_CLOSED,
            DeliveryOutcome::Deferred => Self::POSTHOG_DELIVERY_DEFERRED,
            DeliveryOutcome::Disabled => Self::POSTHOG_DELIVERY_DISABLED,
            _ => Self::POSTHOG_DELIVERY_UNKNOWN,
        }
    }
}

impl From<DrainReport> for PostHogDrainReport {
    fn from(report: DrainReport) -> Self {
        Self {
            state: report.state.into(),
            delivery: report.delivery.into(),
        }
    }
}

impl From<FrameUnavailable> for PostHogFrameUnavailable {
    fn from(reason: FrameUnavailable) -> Self {
        match reason {
            FrameUnavailable::NotRecording => Self::POSTHOG_FRAME_NOT_RECORDING,
            FrameUnavailable::NotPermitted => Self::POSTHOG_FRAME_NOT_PERMITTED,
            FrameUnavailable::OptedOut => Self::POSTHOG_FRAME_OPTED_OUT,
            FrameUnavailable::NoSession => Self::POSTHOG_FRAME_NO_SESSION,
            FrameUnavailable::Throttled => Self::POSTHOG_FRAME_THROTTLED,
            FrameUnavailable::AtCapacity => Self::POSTHOG_FRAME_AT_CAPACITY,
            FrameUnavailable::InvalidLayout => Self::POSTHOG_FRAME_INVALID_LAYOUT,
            FrameUnavailable::OutOfMemory => Self::POSTHOG_FRAME_OUT_OF_MEMORY,
            FrameUnavailable::Reentrant => Self::POSTHOG_FRAME_REENTRANT,
            FrameUnavailable::Closed => Self::POSTHOG_FRAME_CLOSED,
            _ => Self::POSTHOG_FRAME_UNKNOWN,
        }
    }
}

impl From<SubmitOutcome> for PostHogSubmitOutcome {
    fn from(outcome: SubmitOutcome) -> Self {
        match outcome {
            SubmitOutcome::Accepted => Self::POSTHOG_SUBMIT_ACCEPTED,
            SubmitOutcome::WrongRecorder => Self::POSTHOG_SUBMIT_WRONG_RECORDER,
            SubmitOutcome::Reentrant => Self::POSTHOG_SUBMIT_REENTRANT,
            SubmitOutcome::Stale => Self::POSTHOG_SUBMIT_STALE,
            SubmitOutcome::Closed => Self::POSTHOG_SUBMIT_CLOSED,
            _ => Self::POSTHOG_SUBMIT_UNKNOWN,
        }
    }
}

fn deserialize_hook_timestamp<'de, D>(deserializer: D) -> Result<DateTime<Utc>, D::Error>
where
    D: Deserializer<'de>,
{
    let raw = String::deserialize(deserializer)?;
    if let Ok(dt) = DateTime::<FixedOffset>::parse_from_rfc3339(&raw) {
        return Ok(dt.with_timezone(&Utc));
    }
    if let Ok(naive) = NaiveDateTime::parse_from_str(&raw, "%Y-%m-%dT%H:%M:%S%.f") {
        return Ok(naive.and_utc());
    }
    if let Ok(naive) = raw.parse::<NaiveDateTime>() {
        return Ok(naive.and_utc());
    }
    Err(serde::de::Error::custom(format!(
        "invalid event timestamp: {raw}"
    )))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct HookEvent {
    event: String,
    distinct_id: String,
    #[serde(default)]
    properties: HashMap<String, Value>,
    #[serde(default)]
    options: HashMap<String, Value>,
    #[serde(default)]
    groups: HashMap<String, String>,
    #[serde(deserialize_with = "deserialize_hook_timestamp")]
    timestamp: DateTime<Utc>,
    uuid: Uuid,
}

impl HookEvent {
    fn into_event(self) -> Result<Event, &'static str> {
        if self.distinct_id.trim().is_empty() {
            return Err(
                "posthog-stateful-ffi: before_send replacement event has blank distinct_id; dropping event",
            );
        }
        let mut event = Event::new(self.event, self.distinct_id);
        for (key, value) in self.properties {
            event.insert_prop(key, value).map_err(|_| {
                "posthog-stateful-ffi: before_send replacement event has invalid property; dropping event"
            })?;
        }
        for (key, value) in self.options {
            event.insert_option(key, value).map_err(|_| {
                "posthog-stateful-ffi: before_send replacement event has invalid option; dropping event"
            })?;
        }
        for (key, value) in self.groups {
            event.add_group(&key, &value);
        }
        event.set_timestamp(self.timestamp).map_err(|_| {
            "posthog-stateful-ffi: before_send replacement event has invalid timestamp; dropping event"
        })?;
        event.set_uuid(self.uuid);
        Ok(event)
    }
}

#[derive(Serialize)]
struct FlagContextJson<'a> {
    distinct_id: &'a str,
    anonymous_id: &'a str,
    groups: &'a HashMap<String, String>,
    person_properties: &'a HashMap<String, Value>,
    group_properties: &'a HashMap<String, HashMap<String, Value>>,
    disable_geoip: bool,
}

fn flag_context_json(ctx: &FeatureFlagContext) -> FlagContextJson<'_> {
    FlagContextJson {
        distinct_id: ctx.distinct_id(),
        anonymous_id: ctx.anonymous_id(),
        groups: ctx.groups(),
        person_properties: ctx.person_properties(),
        group_properties: ctx.group_properties(),
        disable_geoip: ctx.disable_geoip(),
    }
}

#[derive(Serialize)]
struct FlagResultJson<'a> {
    key: &'a str,
    enabled: bool,
    variant: &'a Option<String>,
    has_payload: bool,
    payload: &'a Option<Value>,
    context: FlagContextJson<'a>,
    request_id: &'a Option<String>,
    evaluated_at: Option<i64>,
    partial: bool,
    id: Option<u64>,
    version: Option<u32>,
    reason: &'a Option<String>,
}

fn flag_result_json(result: &FeatureFlagResult) -> FlagResultJson<'_> {
    FlagResultJson {
        key: &result.key,
        enabled: result.enabled,
        variant: &result.variant,
        has_payload: result.payload.is_some(),
        payload: &result.payload,
        context: flag_context_json(&result.context),
        request_id: &result.request_id,
        evaluated_at: result.evaluated_at,
        partial: result.partial,
        id: result.id,
        version: result.version,
        reason: &result.reason,
    }
}

struct FlagMapJson<'a>(&'a HashMap<String, FeatureFlagResult>);

impl Serialize for FlagMapJson<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(self.0.len()))?;
        for (key, value) in self.0 {
            map.serialize_entry(key, &flag_result_json(value))?;
        }
        map.end()
    }
}

#[derive(Serialize)]
struct FlagsUpdateJson<'a> {
    flags: Option<FlagMapJson<'a>>,
    context: FlagContextJson<'a>,
    errors_loading: bool,
    error: Option<Value>,
    partial: bool,
    invalid_payloads: bool,
}

fn flag_error_json(error: FeatureFlagLoadError) -> Value {
    match error {
        FeatureFlagLoadError::Network => json!({"kind": "network"}),
        FeatureFlagLoadError::Http(status) => json!({"kind": "http", "status": status}),
        FeatureFlagLoadError::InvalidResponse => json!({"kind": "invalid_response"}),
        FeatureFlagLoadError::QuotaLimited => json!({"kind": "quota_limited"}),
        _ => json!({"kind": "unknown"}),
    }
}

fn describe_error(error: &PostHogError<'_>) -> String {
    let status = |s: Option<u16>| s.map(|code| format!(" (HTTP {code})")).unwrap_or_default();
    match error {
        PostHogError::Capture(f) => format!(
            "posthog-rs: delivery of {} events failed after {} attempts{}: {}",
            f.event_count(),
            f.attempt(),
            status(f.status()),
            f.error()
                .map_or_else(|| f.verdict_summary(), ToString::to_string)
        ),
        PostHogError::FeatureFlags(f) => format!(
            "posthog-rs: feature flags request failed{}: {}",
            status(f.status()),
            f.error()
        ),
        PostHogError::Storage(f) => format!(
            "posthog-rs: storage {:?} failed: {}",
            f.operation(),
            f.error()
        ),
        PostHogError::Initialization(f) => {
            format!("posthog-rs: {}; client disabled", f.message())
        }
        PostHogError::RemoteConfig(f) => format!(
            "posthog-rs: remote config failed{}: {}",
            status(f.status()),
            f.message()
        ),
        PostHogError::Replay(f) => format!(
            "posthog-rs: replay frame dropped{}: {}",
            status(f.status()),
            f.message()
        ),
        _ => "posthog-rs: unrecognized SDK error".into(),
    }
}

// ---------------------------------------------------------------------------
// Core utilities (same contract as posthog-rs-ffi)
// ---------------------------------------------------------------------------

/// Returns a description of the last error on the calling thread, or `NULL`
/// if the last call succeeded. Free functions do not modify it.
#[no_mangle]
pub extern "C" fn posthog_last_error() -> *const c_char {
    LAST_ERROR
        .try_with(|slot| slot.borrow().as_ref().map_or(ptr::null(), |s| s.as_ptr()))
        .unwrap_or(ptr::null())
}

/// Frees a string returned by the SDK. `NULL` is ignored.
#[no_mangle]
pub unsafe extern "C" fn posthog_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(CString::from_raw(value));
    }
}

/// Copies a NUL-terminated UTF-8 string into an SDK-owned allocation.
/// Useful for returning replacement JSON from a `PostHogBeforeSendFn` callback.
/// Returns `NULL` on error.
#[no_mangle]
pub unsafe extern "C" fn posthog_string_copy(value: *const c_char) -> *mut c_char {
    let mut out = ptr::null_mut();
    ffi_call(|| {
        let s = str_arg(value, "value")?;
        out = into_c_string(s.to_owned())?;
        Ok(())
    });
    out
}

// ---------------------------------------------------------------------------
// Config builder
// ---------------------------------------------------------------------------

/// Creates stateful client configuration for a project token (`phc_...`).
/// Returns `NULL` on error. Free with `posthog_stateful_config_free`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_new(
    project_token: *const c_char,
) -> *mut PostHogStatefulConfig {
    ffi_new(|| {
        let token = str_arg(project_token, "project_token")?;
        Ok(PostHogStatefulConfig {
            config: Some(stateful::Config::new(token)),
            before_send: None,
            on_feature_flags: None,
            on_error: None,
        })
    })
}

/// Frees stateful client configuration. `NULL` is ignored.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_free(config: *mut PostHogStatefulConfig) {
    if !config.is_null() {
        drop(Box::from_raw(config));
    }
}

unsafe fn edit_config(
    config: *mut PostHogStatefulConfig,
    f: impl FnOnce(&mut stateful::Config) -> Result<(), FfiError>,
) -> PostHogStatus {
    ffi_call(|| {
        let holder = mut_arg(config, "config")?;
        let inner = holder.config.as_mut().ok_or_else(|| {
            FfiError::new(
                PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT,
                "config has already been consumed by posthog_stateful_client_new",
            )
        })?;
        f(inner)
    })
}

/// Sets the PostHog host (for example `https://eu.i.posthog.com`).
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_host(
    config: *mut PostHogStatefulConfig,
    host: *const c_char,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.host = str_arg(host, "host")?.to_owned();
        Ok(())
    })
}

/// Sets the persistence directory, or `NULL` for memory-only storage.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_storage_dir(
    config: *mut PostHogStatefulConfig,
    path: *const c_char,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.persistence = match opt_str_arg(path, "path")? {
            Some(dir) if !dir.is_empty() => stateful::Persistence::Directory(dir.into()),
            _ => stateful::Persistence::Memory,
        };
        Ok(())
    })
}

/// Sets the instance name used to namespace storage alongside the project token.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_instance_name(
    config: *mut PostHogStatefulConfig,
    name: *const c_char,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.instance_name = str_arg(name, "name")?.to_owned();
        Ok(())
    })
}

/// Explicitly disables the client when `true`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_disabled(
    config: *mut PostHogStatefulConfig,
    value: bool,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.disabled = value;
        Ok(())
    })
}

/// Sets the default opt-out state when no explicit consent choice is persisted.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_opt_out(
    config: *mut PostHogStatefulConfig,
    value: bool,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.opt_out = value;
        Ok(())
    })
}

/// Sets the person profile creation policy (a `PostHogPersonProfiles` value).
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_person_profiles(
    config: *mut PostHogStatefulConfig,
    mode: i32,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.person_profiles = match mode {
            x if x == PostHogPersonProfiles::POSTHOG_PERSON_PROFILES_NEVER as i32 => {
                stateful::PersonProfiles::Never
            }
            x if x == PostHogPersonProfiles::POSTHOG_PERSON_PROFILES_IDENTIFIED_ONLY as i32 => {
                stateful::PersonProfiles::IdentifiedOnly
            }
            x if x == PostHogPersonProfiles::POSTHOG_PERSON_PROFILES_ALWAYS as i32 => {
                stateful::PersonProfiles::Always
            }
            _ => {
                return Err(FfiError::new(
                    PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT,
                    format!("invalid `mode` for person profiles: {mode}"),
                ));
            }
        };
        Ok(())
    })
}

/// Sets the session lifecycle mode (a `PostHogSessionMode` value: `AUTOMATIC` or `MANUAL`).
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_session_mode(
    config: *mut PostHogStatefulConfig,
    mode: i32,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.session_mode = match mode {
            x if x == PostHogSessionMode::POSTHOG_SESSION_MODE_AUTOMATIC as i32 => {
                stateful::SessionMode::Automatic
            }
            x if x == PostHogSessionMode::POSTHOG_SESSION_MODE_MANUAL as i32 => {
                stateful::SessionMode::Manual
            }
            _ => {
                return Err(FfiError::new(
                    PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT,
                    format!("invalid `mode` for session mode: {mode}"),
                ));
            }
        };
        Ok(())
    })
}

/// Persists sessions across restarts when `true` and directory persistence is enabled.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_persist_sessions(
    config: *mut PostHogStatefulConfig,
    value: bool,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.session_persistence = if value {
            stateful::SessionPersistence::Persistent
        } else {
            stateful::SessionPersistence::Memory
        };
        Ok(())
    })
}

/// Preloads feature flags at startup when `true`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_preload_feature_flags(
    config: *mut PostHogStatefulConfig,
    value: bool,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.preload_feature_flags = value;
        Ok(())
    })
}

/// Sends `$feature_flag_called` by default on single-flag lookups when `true`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_send_feature_flag_event(
    config: *mut PostHogStatefulConfig,
    value: bool,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.send_feature_flag_event = value;
        Ok(())
    })
}

/// Sets platform context properties from a JSON object (or `NULL` to clear).
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_platform_context_json(
    config: *mut PostHogStatefulConfig,
    json: *const c_char,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.platform_context = opt_json_object_arg(json, "json")?;
        Ok(())
    })
}

/// Sets the queued event threshold that triggers a flush.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_flush_at(
    config: *mut PostHogStatefulConfig,
    value: usize,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.flush_at = value;
        Ok(())
    })
}

/// Sets the automatic flush interval in milliseconds.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_flush_interval_ms(
    config: *mut PostHogStatefulConfig,
    value: u64,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.flush_interval = Duration::from_millis(value);
        Ok(())
    })
}

/// Sets the maximum number of queued events.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_max_queue_size(
    config: *mut PostHogStatefulConfig,
    value: usize,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.max_queue_size = value;
        Ok(())
    })
}

/// Sets the maximum number of events sent per batch.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_max_batch_size(
    config: *mut PostHogStatefulConfig,
    value: usize,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.max_batch_size = value;
        Ok(())
    })
}

/// Sets the per-request HTTP timeout in seconds.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_request_timeout_seconds(
    config: *mut PostHogStatefulConfig,
    value: u64,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.request_timeout = Duration::from_secs(value);
        Ok(())
    })
}

/// Sets the default shutdown budget in milliseconds.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_shutdown_timeout_ms(
    config: *mut PostHogStatefulConfig,
    value: u64,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.shutdown_timeout = Duration::from_millis(value);
        Ok(())
    })
}

/// Sets JPEG encoding quality (1..=100) for session replay screenshots.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_replay_jpeg_quality(
    config: *mut PostHogStatefulConfig,
    value: u8,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.session_replay.jpeg_quality = value;
        Ok(())
    })
}

/// Sets the maximum number of replay frames in flight.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_replay_max_frames_in_flight(
    config: *mut PostHogStatefulConfig,
    value: usize,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.session_replay.max_frames_in_flight = value;
        Ok(())
    })
}

/// Sets the minimum time between replay frames in milliseconds (0 disables throttling).
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_replay_throttle_delay_ms(
    config: *mut PostHogStatefulConfig,
    value: u64,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.session_replay.throttle_delay = Duration::from_millis(value);
        Ok(())
    })
}

/// Sets the SDK name and version reported in `$lib`, `$lib_version`, and transport headers.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_sdk_identity(
    config: *mut PostHogStatefulConfig,
    sdk_name: *const c_char,
    sdk_version: *const c_char,
) -> PostHogStatus {
    edit_config(config, |c| {
        c.sdk_name = str_arg(sdk_name, "sdk_name")?.to_owned();
        c.sdk_version = str_arg(sdk_version, "sdk_version")?.to_owned();
        Ok(())
    })
}

unsafe fn set_callback<F: Copy>(
    config: *mut PostHogStatefulConfig,
    callback: Option<F>,
    user_data: *mut c_void,
    select: impl FnOnce(&mut PostHogStatefulConfig) -> &mut Option<CallbackBinding<F>>,
) -> PostHogStatus {
    ffi_call(|| {
        let holder = mut_arg(config, "config")?;
        if holder.config.is_none() {
            return Err(FfiError::new(
                PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT,
                "config has already been consumed by posthog_stateful_client_new",
            ));
        }
        *select(holder) = callback.map(|callback| CallbackBinding {
            callback,
            user_data: user_data as usize,
        });
        Ok(())
    })
}

/// Registers a synchronous `before_send` hook. `callback` may be `NULL` to clear.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_before_send(
    config: *mut PostHogStatefulConfig,
    callback: Option<PostHogBeforeSendFn>,
    user_data: *mut c_void,
) -> PostHogStatus {
    set_callback(config, callback, user_data, |h| &mut h.before_send)
}

/// Registers a feature-flags update listener. `callback` may be `NULL` to clear.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_on_feature_flags(
    config: *mut PostHogStatefulConfig,
    callback: Option<PostHogFeatureFlagsFn>,
    user_data: *mut c_void,
) -> PostHogStatus {
    set_callback(config, callback, user_data, |h| &mut h.on_feature_flags)
}

/// Registers an SDK error callback. `callback` may be `NULL` to clear.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_config_set_on_error(
    config: *mut PostHogStatefulConfig,
    callback: Option<PostHogErrorFn>,
    user_data: *mut c_void,
) -> PostHogStatus {
    set_callback(config, callback, user_data, |h| &mut h.on_error)
}

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------

thread_local! {
    static ACTIVE_CALLBACK_CLIENTS: RefCell<Vec<usize>> = const { RefCell::new(Vec::new()) };
}

struct CallbackGuard {
    pushed: bool,
}

impl CallbackGuard {
    fn enter(client_token: usize) -> Self {
        let pushed = ACTIVE_CALLBACK_CLIENTS
            .try_with(|clients| {
                clients.borrow_mut().push(client_token);
            })
            .is_ok();
        Self { pushed }
    }
}

impl Drop for CallbackGuard {
    fn drop(&mut self) {
        if self.pushed {
            let _ = ACTIVE_CALLBACK_CLIENTS.try_with(|clients| {
                clients.borrow_mut().pop();
            });
        }
    }
}

fn in_client_callback(client_token: usize) -> bool {
    ACTIVE_CALLBACK_CLIENTS
        .try_with(|clients| clients.borrow().contains(&client_token))
        .unwrap_or(false)
}

fn detach_callbacks(client: &PostHogStatefulClient) {
    let client_token = Arc::as_ptr(&client.before_send) as usize;
    if in_client_callback(client_token) {
        return;
    }
    *client
        .before_send
        .write()
        .unwrap_or_else(|p| p.into_inner()) = None;
    *client
        .on_feature_flags
        .write()
        .unwrap_or_else(|p| p.into_inner()) = None;
    *client.on_error.write().unwrap_or_else(|p| p.into_inner()) = None;
}

fn emit_error(slot: &CallbackSlot<PostHogErrorFn>, client_token: usize, message: &str) {
    let guard = slot.read().unwrap_or_else(|p| p.into_inner());
    let Some(binding) = *guard else {
        return;
    };
    let _callback_guard = CallbackGuard::enter(client_token);
    let c_message = to_cstring_sanitized(message);
    unsafe {
        (binding.callback)(binding.user_data as *mut c_void, c_message.as_ptr());
    }
}

/// Creates and initializes a stateful client from `config`, consuming the
/// inner configuration so `config` cannot be reused or modified afterwards.
/// The caller must still free the outer handle with `posthog_stateful_config_free(config)`.
/// Restores persisted state on the calling thread without waiting on the network.
/// Returns `NULL` on FFI error.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_client_new(
    config: *mut PostHogStatefulConfig,
) -> *mut PostHogStatefulClient {
    ffi_new(|| {
        let holder = mut_arg(config, "config")?;
        let mut inner = holder.config.take().ok_or_else(|| {
            FfiError::new(
                PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT,
                "config has already been consumed by posthog_stateful_client_new",
            )
        })?;

        let on_error: CallbackSlot<PostHogErrorFn> = Arc::new(RwLock::new(holder.on_error));

        let before_send: CallbackSlot<PostHogBeforeSendFn> =
            Arc::new(RwLock::new(holder.before_send));
        let client_token = Arc::as_ptr(&before_send) as usize;
        if holder.before_send.is_some() {
            let slot = before_send.clone();
            let error_slot = on_error.clone();
            inner.before_send(move |event| {
                let guard = slot.read().unwrap_or_else(|p| p.into_inner());
                let binding = (*guard)?;
                let _callback_guard = CallbackGuard::enter(client_token);
                let encoded = serde_json::to_string(&event).ok()?;
                let input = to_cstring_sanitized(&encoded);
                let mut out_ptr: *mut c_char = ptr::null_mut();
                let keep = unsafe {
                    (binding.callback)(
                        binding.user_data as *mut c_void,
                        input.as_ptr(),
                        &mut out_ptr,
                    )
                };
                if out_ptr.is_null() {
                    return keep.then_some(event);
                }
                let replacement = unsafe { CString::from_raw(out_ptr) };
                if !keep {
                    return None;
                }
                let json = match replacement.to_str() {
                    Ok(json) => json,
                    Err(_) => {
                        emit_error(
                            &error_slot,
                            client_token,
                            "posthog-stateful-ffi: before_send returned non-UTF-8 replacement JSON; dropping event",
                        );
                        return None;
                    }
                };
                let parsed = match serde_json::from_str::<HookEvent>(json) {
                    Ok(parsed) => parsed,
                    Err(_) => {
                        emit_error(
                            &error_slot,
                            client_token,
                            "posthog-stateful-ffi: before_send returned invalid replacement event JSON; dropping event",
                        );
                        return None;
                    }
                };
                match parsed.into_event() {
                    Ok(next) => Some(next),
                    Err(message) => {
                        emit_error(&error_slot, client_token, message);
                        None
                    }
                }
            });
        }

        let on_feature_flags: CallbackSlot<PostHogFeatureFlagsFn> =
            Arc::new(RwLock::new(holder.on_feature_flags));
        if holder.on_feature_flags.is_some() {
            let slot = on_feature_flags.clone();
            inner.on_feature_flags(move |update: &FeatureFlagsUpdate| {
                let guard = slot.read().unwrap_or_else(|p| p.into_inner());
                let Some(binding) = *guard else {
                    return;
                };
                let _callback_guard = CallbackGuard::enter(client_token);
                let payload = FlagsUpdateJson {
                    flags: update.flags.as_ref().map(FlagMapJson),
                    context: flag_context_json(&update.context),
                    errors_loading: update.errors_loading,
                    error: update.error.map(flag_error_json),
                    partial: update.partial,
                    invalid_payloads: update.invalid_payloads,
                };
                if let Ok(encoded) = serde_json::to_string(&payload) {
                    let c_str = to_cstring_sanitized(&encoded);
                    unsafe {
                        (binding.callback)(binding.user_data as *mut c_void, c_str.as_ptr());
                    }
                }
            });
        }

        if holder.on_error.is_some() {
            let slot = on_error.clone();
            inner.on_error(move |error| {
                emit_error(&slot, client_token, &describe_error(error));
            });
        }

        let client = stateful::Client::init_blocking(inner);
        Ok(PostHogStatefulClient {
            client,
            before_send,
            on_feature_flags,
            on_error,
        })
    })
}

/// Shuts the client down (if not already shut down), detaches all host
/// callbacks (waiting for any running callback invocation to finish), and
/// frees the client. `NULL` is ignored.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_client_free(client: *mut PostHogStatefulClient) {
    if !client.is_null() {
        let client = Box::from_raw(client);
        let _ = catch_unwind(AssertUnwindSafe(move || {
            let _ = client.client.shutdown_blocking(None);
            detach_callbacks(&client);
            drop(client);
        }));
    }
}

/// Persists pending state and attempts delivery of queued events.
/// `report_out` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_flush(
    client: *const PostHogStatefulClient,
    report_out: *mut PostHogDrainReport,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let report = client.client.flush_blocking();
        write_out(report_out, report.into());
        Ok(())
    })
}

/// Closes admission, waits up to the configured shutdown timeout for state
/// persistence and delivery, and detaches host callbacks (when not called from
/// within a host callback). `report_out` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_shutdown(
    client: *const PostHogStatefulClient,
    report_out: *mut PostHogDrainReport,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let report = client.client.shutdown_blocking(None);
        detach_callbacks(client);
        write_out(report_out, report.into());
        Ok(())
    })
}

// ---------------------------------------------------------------------------
// State reads
// ---------------------------------------------------------------------------

/// Returns `true` if the client is disabled (or if `client` is `NULL`).
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_is_disabled(
    client: *const PostHogStatefulClient,
) -> bool {
    catch_unwind(AssertUnwindSafe(|| {
        client.as_ref().is_none_or(|c| c.client.is_disabled())
    }))
    .unwrap_or(true)
}

/// Returns `true` if the client is opted out (or if `client` is `NULL`).
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_is_opt_out(client: *const PostHogStatefulClient) -> bool {
    catch_unwind(AssertUnwindSafe(|| {
        client.as_ref().is_none_or(|c| c.client.is_opt_out())
    }))
    .unwrap_or(true)
}

unsafe fn get_opt_string(
    client: *const PostHogStatefulClient,
    id_out: *mut *mut c_char,
    getter: impl FnOnce(&stateful::Client) -> Option<String>,
) -> PostHogStatus {
    write_out(id_out, ptr::null_mut());
    ffi_call(|| {
        if id_out.is_null() {
            return Err(FfiError::null("id_out"));
        }
        if let Some(value) = getter(&ref_arg(client, "client")?.client) {
            id_out.write(into_c_string(value)?);
        }
        Ok(())
    })
}

/// Writes the current distinct ID to `id_out`. Free with `posthog_string_free`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_get_distinct_id(
    client: *const PostHogStatefulClient,
    id_out: *mut *mut c_char,
) -> PostHogStatus {
    get_opt_string(client, id_out, |c| Some(c.get_distinct_id()))
}

/// Writes the current anonymous ID to `id_out`. Free with `posthog_string_free`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_get_anonymous_id(
    client: *const PostHogStatefulClient,
    id_out: *mut *mut c_char,
) -> PostHogStatus {
    get_opt_string(client, id_out, |c| Some(c.get_anonymous_id()))
}

/// Writes the active session ID to `id_out`, or `NULL` if there is no active session.
/// Free non-`NULL` output with `posthog_string_free`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_get_session_id(
    client: *const PostHogStatefulClient,
    id_out: *mut *mut c_char,
) -> PostHogStatus {
    get_opt_string(client, id_out, |c| {
        c.get_session_id().map(|id| id.to_string())
    })
}

/// Passively reads the session ID without refreshing expiry. Writes `NULL` if none.
/// Free non-`NULL` output with `posthog_string_free`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_peek_session_id(
    client: *const PostHogStatefulClient,
    id_out: *mut *mut c_char,
) -> PostHogStatus {
    get_opt_string(client, id_out, |c| {
        c.peek_session_id().map(|id| id.to_string())
    })
}

// ---------------------------------------------------------------------------
// Capture & screen
// ---------------------------------------------------------------------------

/// Captures an event. `properties_json`, `options_json`, and `outcome_out` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_capture(
    client: *const PostHogStatefulClient,
    event: *const c_char,
    properties_json: *const c_char,
    options_json: *const c_char,
    outcome_out: *mut PostHogCaptureOutcome,
) -> PostHogStatus {
    write_out(outcome_out, PostHogCaptureOutcome::POSTHOG_CAPTURE_INVALID);
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let mut capture = stateful::Capture::new(str_arg(event, "event")?);
        capture.properties = opt_json_object_arg(properties_json, "properties_json")?;
        capture.options = opt_json_object_arg(options_json, "options_json")?;
        write_out(outcome_out, client.client.capture(capture).into());
        Ok(())
    })
}

/// Captures a `$screen` event. `properties_json` and `outcome_out` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_screen(
    client: *const PostHogStatefulClient,
    name: *const c_char,
    properties_json: *const c_char,
    outcome_out: *mut PostHogCaptureOutcome,
) -> PostHogStatus {
    write_out(outcome_out, PostHogCaptureOutcome::POSTHOG_CAPTURE_INVALID);
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let mut screen = stateful::Screen::new(str_arg(name, "name")?);
        screen.properties = opt_json_object_arg(properties_json, "properties_json")?;
        write_out(outcome_out, client.client.screen(screen).into());
        Ok(())
    })
}

// ---------------------------------------------------------------------------
// Identity, properties, groups, consent, sessions
// ---------------------------------------------------------------------------

/// Identifies the user. `set_json` and `set_once_json` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_identify(
    client: *const PostHogStatefulClient,
    distinct_id: *const c_char,
    set_json: *const c_char,
    set_once_json: *const c_char,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let mut identify = stateful::Identify::new(str_arg(distinct_id, "distinct_id")?);
        identify.set = opt_json_object_arg(set_json, "set_json")?;
        identify.set_once = opt_json_object_arg(set_once_json, "set_once_json")?;
        client.client.identify(identify);
        Ok(())
    })
}

/// Resets identity, registered properties, groups, and sessions.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_reset(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.reset();
        Ok(())
    })
}

/// Creates an alias for the current distinct ID.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_alias(
    client: *const PostHogStatefulClient,
    alias: *const c_char,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        client.client.alias(str_arg(alias, "alias")?);
        Ok(())
    })
}

/// Registers persistent super-properties from a JSON object.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_register(
    client: *const PostHogStatefulClient,
    properties_json: *const c_char,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let props = opt_json_object_arg(properties_json, "properties_json")?;
        client.client.register(props);
        Ok(())
    })
}

/// Unregisters a super-property by key.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_unregister(
    client: *const PostHogStatefulClient,
    key: *const c_char,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        client.client.unregister(str_arg(key, "key")?);
        Ok(())
    })
}

/// Updates person properties. `set_json` and `set_once_json` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_set_person_properties(
    client: *const PostHogStatefulClient,
    set_json: *const c_char,
    set_once_json: *const c_char,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let mut props =
            stateful::PersonProperties::from(opt_json_object_arg(set_json, "set_json")?);
        props.set_once = opt_json_object_arg(set_once_json, "set_once_json")?;
        client.client.set_person_properties(props);
        Ok(())
    })
}

/// Associates future events with a group. `properties_json` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_group(
    client: *const PostHogStatefulClient,
    group_type: *const c_char,
    group_key: *const c_char,
    properties_json: *const c_char,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let mut group = stateful::Group::new(
            str_arg(group_type, "group_type")?,
            str_arg(group_key, "group_key")?,
        );
        group.properties = opt_json_object_arg(properties_json, "properties_json")?;
        client.client.group(group);
        Ok(())
    })
}

/// Sets properties on a group without changing active group memberships.
/// `properties_json` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_group_identify(
    client: *const PostHogStatefulClient,
    group_type: *const c_char,
    group_key: *const c_char,
    properties_json: *const c_char,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let mut group = stateful::Group::new(
            str_arg(group_type, "group_type")?,
            str_arg(group_key, "group_key")?,
        );
        group.properties = opt_json_object_arg(properties_json, "properties_json")?;
        client.client.group_identify(group);
        Ok(())
    })
}

/// Removes an active group membership.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_remove_group(
    client: *const PostHogStatefulClient,
    group_type: *const c_char,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        client
            .client
            .remove_group(str_arg(group_type, "group_type")?);
        Ok(())
    })
}

/// Opts in to event collection.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_opt_in(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.opt_in();
        Ok(())
    })
}

/// Opts out of new event collection.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_opt_out(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.opt_out();
        Ok(())
    })
}

/// Starts a session if none is active.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_start_session(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.start_session();
        Ok(())
    })
}

/// Ends the current session.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_end_session(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.end_session();
        Ok(())
    })
}

/// Ends the current session and starts a fresh one.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_reset_session(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.reset_session();
        Ok(())
    })
}

/// Records user activity to extend the current automatic session.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_session_activity(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.session_activity();
        Ok(())
    })
}

/// Notifies the SDK whether the application is in the background.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_set_app_in_background(
    client: *const PostHogStatefulClient,
    background: bool,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?
            .client
            .set_app_in_background(background);
        Ok(())
    })
}

// ---------------------------------------------------------------------------
// Feature flags
// ---------------------------------------------------------------------------

/// Looks up a single feature flag using a `PostHogFlagReporting` value and writes its JSON record
/// to `json_out`, or `NULL` if the flag is not found. Free non-`NULL` output with
/// `posthog_string_free`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_get_feature_flag_json(
    client: *const PostHogStatefulClient,
    key: *const c_char,
    reporting: i32,
    json_out: *mut *mut c_char,
) -> PostHogStatus {
    write_out(json_out, ptr::null_mut());
    ffi_call(|| {
        if json_out.is_null() {
            return Err(FfiError::null("json_out"));
        }
        let client = ref_arg(client, "client")?;
        let key = str_arg(key, "key")?;
        let lookup = client.client.feature_flag(key);
        let lookup = match reporting {
            x if x == PostHogFlagReporting::POSTHOG_FLAG_REPORTING_DEFAULT as i32 => lookup,
            x if x == PostHogFlagReporting::POSTHOG_FLAG_REPORTING_SEND as i32 => {
                lookup.send_event(true)
            }
            x if x == PostHogFlagReporting::POSTHOG_FLAG_REPORTING_SILENT as i32 => {
                lookup.send_event(false)
            }
            _ => {
                return Err(FfiError::new(
                    PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT,
                    format!("invalid `reporting` for feature flag lookup: {reporting}"),
                ));
            }
        };
        if let Some(flag) = lookup.get() {
            let encoded = serde_json::to_string(&flag_result_json(&flag)).map_err(|error| {
                FfiError::new(
                    PostHogStatus::POSTHOG_STATUS_INVALID_JSON,
                    error.to_string(),
                )
            })?;
            json_out.write(into_c_string(encoded)?);
        }
        Ok(())
    })
}

/// Writes all cached feature flags as a JSON object to `json_out`, or `NULL`
/// if flags have not been loaded. Free non-`NULL` output with `posthog_string_free`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_get_feature_flags_json(
    client: *const PostHogStatefulClient,
    json_out: *mut *mut c_char,
) -> PostHogStatus {
    write_out(json_out, ptr::null_mut());
    ffi_call(|| {
        if json_out.is_null() {
            return Err(FfiError::null("json_out"));
        }
        let client = ref_arg(client, "client")?;
        if let Some(flags) = client.client.get_feature_flags() {
            let encoded = serde_json::to_string(&FlagMapJson(&flags)).map_err(|error| {
                FfiError::new(
                    PostHogStatus::POSTHOG_STATUS_INVALID_JSON,
                    error.to_string(),
                )
            })?;
            json_out.write(into_c_string(encoded)?);
        }
        Ok(())
    })
}

/// Requests an asynchronous feature flag refresh.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_reload_feature_flags(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.reload_feature_flags();
        Ok(())
    })
}

/// Sets person properties used for feature flag targeting.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_set_person_properties_for_flags(
    client: *const PostHogStatefulClient,
    properties_json: *const c_char,
    reload: bool,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let props = opt_json_object_arg(properties_json, "properties_json")?;
        client
            .client
            .set_person_properties_for_flags(props, Some(reload));
        Ok(())
    })
}

/// Clears person properties used for feature flag targeting.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_reset_person_properties_for_flags(
    client: *const PostHogStatefulClient,
    reload: bool,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?
            .client
            .reset_person_properties_for_flags(Some(reload));
        Ok(())
    })
}

/// Sets group properties used for feature flag targeting.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_set_group_properties_for_flags(
    client: *const PostHogStatefulClient,
    group_type: *const c_char,
    properties_json: *const c_char,
    reload: bool,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let group_type = str_arg(group_type, "group_type")?;
        let props = opt_json_object_arg(properties_json, "properties_json")?;
        client
            .client
            .set_group_properties_for_flags(group_type, props, Some(reload));
        Ok(())
    })
}

/// Clears group properties used for feature flag targeting.
/// Pass `NULL` for `group_type` to clear all group types.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_reset_group_properties_for_flags(
    client: *const PostHogStatefulClient,
    group_type: *const c_char,
    reload: bool,
) -> PostHogStatus {
    ffi_call(|| {
        let client = ref_arg(client, "client")?;
        let group_type = opt_str_arg(group_type, "group_type")?;
        client
            .client
            .reset_group_properties_for_flags(group_type, Some(reload));
        Ok(())
    })
}

// ---------------------------------------------------------------------------
// Session replay
// ---------------------------------------------------------------------------

/// Starts manual session replay recording.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_replay_start(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.session_replay().start();
        Ok(())
    })
}

/// Stops manual session replay recording.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_replay_stop(
    client: *const PostHogStatefulClient,
) -> PostHogStatus {
    ffi_call(|| {
        ref_arg(client, "client")?.client.session_replay().stop();
        Ok(())
    })
}

/// Returns `true` if session replay recording is active (false if `client` is `NULL`).
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_replay_is_active(
    client: *const PostHogStatefulClient,
) -> bool {
    catch_unwind(AssertUnwindSafe(|| {
        client
            .as_ref()
            .is_some_and(|c| c.client.session_replay().is_active())
    }))
    .unwrap_or(false)
}

/// Acquires a replay frame reservation for a top-down BGRA8888 image.
/// On acquisition, writes a non-`NULL` handle to `frame_out` and sets
/// `*unavailable_out` to `POSTHOG_FRAME_OK`. On refusal, sets `*frame_out` to
/// `NULL` and writes the reason to `unavailable_out` (returns `POSTHOG_STATUS_OK`).
/// Submit with `posthog_stateful_replay_submit_frame` or abandon with
/// `posthog_stateful_replay_frame_free`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_replay_begin_frame(
    client: *const PostHogStatefulClient,
    width: u32,
    height: u32,
    row_stride_bytes: usize,
    frame_out: *mut *mut PostHogReplayFrame,
    unavailable_out: *mut PostHogFrameUnavailable,
) -> PostHogStatus {
    write_out(frame_out, ptr::null_mut());
    write_out(
        unavailable_out,
        PostHogFrameUnavailable::POSTHOG_FRAME_UNKNOWN,
    );
    ffi_call(|| {
        if frame_out.is_null() {
            return Err(FfiError::null("frame_out"));
        }
        let client = ref_arg(client, "client")?;
        let layout = FrameLayout::new(width, height, row_stride_bytes, PixelFormat::Bgra8888);
        match client.client.session_replay().begin_frame(layout) {
            Ok(frame) => {
                frame_out.write(Box::into_raw(Box::new(PostHogReplayFrame { frame })));
                write_out(unavailable_out, PostHogFrameUnavailable::POSTHOG_FRAME_OK);
            }
            Err(reason) => {
                write_out(unavailable_out, reason.into());
            }
        }
        Ok(())
    })
}

/// Submits BGRA8888 pixels for an acquired `frame` and ALWAYS consumes and frees
/// `frame`, even when this call returns an error status. Never call
/// `posthog_stateful_replay_frame_free` on `frame` after calling this function.
/// `pixels_len` must equal `row_stride_bytes * height`. `outcome_out` may be `NULL`.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_replay_submit_frame(
    client: *const PostHogStatefulClient,
    frame: *mut PostHogReplayFrame,
    pixels: *const u8,
    pixels_len: usize,
    outcome_out: *mut PostHogSubmitOutcome,
) -> PostHogStatus {
    write_out(outcome_out, PostHogSubmitOutcome::POSTHOG_SUBMIT_CLOSED);
    // Always reclaim `frame` so callers never leak it on error.
    let frame_box = if frame.is_null() {
        None
    } else {
        Some(Box::from_raw(frame))
    };
    ffi_call(|| {
        let mut frame = frame_box.ok_or_else(|| FfiError::null("frame"))?;
        let client = ref_arg(client, "client")?;
        if pixels.is_null() {
            return Err(FfiError::null("pixels"));
        }
        let target = frame.frame.pixels_mut();
        if pixels_len != target.len() {
            return Err(FfiError::new(
                PostHogStatus::POSTHOG_STATUS_INVALID_ARGUMENT,
                format!(
                    "`pixels_len` ({pixels_len}) does not match frame size ({})",
                    target.len()
                ),
            ));
        }
        let src = std::slice::from_raw_parts(pixels, pixels_len);
        target.copy_from_slice(src);
        let outcome = client.client.session_replay().submit_frame(frame.frame);
        write_out(outcome_out, outcome.into());
        Ok(())
    })
}

/// Abandons and frees an unsubmitted replay frame. `NULL` is ignored.
#[no_mangle]
pub unsafe extern "C" fn posthog_stateful_replay_frame_free(frame: *mut PostHogReplayFrame) {
    if !frame.is_null() {
        drop(Box::from_raw(frame));
    }
}

#[cfg(test)]
mod unit_tests {
    use super::*;

    fn assert_send_sync<T: Send + Sync>() {}

    #[test]
    fn client_is_thread_safe() {
        assert_send_sync::<PostHogStatefulClient>();
    }

    #[test]
    fn enums_are_four_bytes() {
        assert_eq!(std::mem::size_of::<PostHogStatus>(), 4);
        assert_eq!(std::mem::size_of::<PostHogPersonProfiles>(), 4);
        assert_eq!(std::mem::size_of::<PostHogSessionMode>(), 4);
        assert_eq!(std::mem::size_of::<PostHogFlagReporting>(), 4);
        assert_eq!(std::mem::size_of::<PostHogCaptureOutcome>(), 4);
        assert_eq!(std::mem::size_of::<PostHogPersistenceOutcome>(), 4);
        assert_eq!(std::mem::size_of::<PostHogDeliveryOutcome>(), 4);
        assert_eq!(std::mem::size_of::<PostHogFrameUnavailable>(), 4);
        assert_eq!(std::mem::size_of::<PostHogSubmitOutcome>(), 4);
    }
}
