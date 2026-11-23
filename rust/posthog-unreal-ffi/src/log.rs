//! Process-wide diagnostics: SDK errors and posthog-rs warnings reach the host log.
use std::cell::Cell;
use std::ffi::c_void;
use std::fmt::{self, Write as _};
use std::sync::{Once, RwLock};

use posthog_rs::PostHogError;
use tracing::field::{Field, Visit};
use tracing::level_filters::LevelFilter;
use tracing::{span, Event, Level, Metadata, Subscriber};

use crate::abi::{contain, Bytes, Status};

pub const WARNING: i32 = 1;
pub const ERROR: i32 = 2;

pub type Log = unsafe extern "C" fn(*mut c_void, i32, Bytes);

struct Sink {
    callback: Log,
    // The host promises this opaque address is thread-safe until it clears the sink.
    user_data: usize,
}

static SINK: RwLock<Option<Sink>> = RwLock::new(None);
static INSTALL: Once = Once::new();
thread_local! { static LOGGING: Cell<bool> = const { Cell::new(false) }; }

struct Logging;
impl Drop for Logging {
    fn drop(&mut self) {
        LOGGING.with(|logging| logging.set(false));
    }
}

fn emit(level: i32, message: &str) {
    // A host callback that logs through Rust again must not recurse.
    if LOGGING.with(|logging| logging.replace(true)) {
        return;
    }
    let _logging = Logging;
    let sink = SINK.read().unwrap_or_else(|p| p.into_inner());
    if let Some(sink) = sink.as_ref() {
        unsafe {
            (sink.callback)(
                sink.user_data as *mut c_void,
                level,
                Bytes::from_slice(message.as_bytes()),
            );
        }
    }
}

/// Forwards posthog-rs and bridge warnings and errors to the sink. No other crate's events.
struct Forward;
impl Subscriber for Forward {
    fn enabled(&self, metadata: &Metadata<'_>) -> bool {
        *metadata.level() <= Level::WARN && metadata.target().starts_with("posthog")
    }
    fn max_level_hint(&self) -> Option<LevelFilter> {
        Some(LevelFilter::WARN)
    }
    fn new_span(&self, _: &span::Attributes<'_>) -> span::Id {
        span::Id::from_u64(1)
    }
    fn record(&self, _: &span::Id, _: &span::Record<'_>) {}
    fn record_follows_from(&self, _: &span::Id, _: &span::Id) {}
    fn event(&self, event: &Event<'_>) {
        let mut message = Message::default();
        event.record(&mut message);
        message.text.push_str(&message.fields);
        let level = if *event.metadata().level() == Level::ERROR {
            ERROR
        } else {
            WARNING
        };
        emit(level, &message.text);
    }
    fn enter(&self, _: &span::Id) {}
    fn exit(&self, _: &span::Id) {}
}

#[derive(Default)]
struct Message {
    text: String,
    fields: String,
}
impl Visit for Message {
    fn record_debug(&mut self, field: &Field, value: &dyn fmt::Debug) {
        if field.name() == "message" {
            let _ = write!(self.text, "{value:?}");
        } else {
            let _ = write!(self.fields, " {}={value:?}", field.name());
        }
    }
}

/// A sanitized one-line description: no tokens, distinct IDs, response bodies or pixels.
pub fn describe(error: &PostHogError<'_>) -> String {
    let status = |status: Option<u16>| status.map(|s| format!(" (HTTP {s})")).unwrap_or_default();
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

/// Install as the client's `on_error` hook so SDK failures reach the same sink.
pub fn on_error(error: &PostHogError<'_>) {
    emit(WARNING, &describe(error));
}

/// # Safety
/// The callback and user_data must be thread-safe and stay valid until a later call replaces
/// or clears them returns.
#[no_mangle]
pub unsafe extern "C" fn phu_set_log(callback: Option<Log>, user_data: *mut c_void) -> Status {
    contain(|| {
        if LOGGING.with(Cell::get) {
            return Err(Status::Reentrant);
        }
        // Another global subscriber in the process keeps its place; SDK errors still arrive.
        INSTALL.call_once(|| {
            let _ = tracing::subscriber::set_global_default(Forward);
        });
        // The write lock waits for running callbacks, so clearing is unload-safe.
        *SINK.write().unwrap_or_else(|p| p.into_inner()) = callback.map(|callback| Sink {
            callback,
            user_data: user_data as usize,
        });
        Ok(Status::Ok)
    })
}
